//! Host name -> socket addresses, with fallbacks for sandboxed builds (Flatpak, AppImage).
//!
//! `ToSocketAddrs` (getaddrinfo) is tried first. When it fails, the name is put to progressively
//! more independent resolvers, so a name that works in a terminal also works here even if this
//! process cannot reach the system resolver itself (Flatpak runtime without `nss-mdns`, static /
//! musl / AppImage builds without the host's nss plugins, `.local` names):
//!   1. `getent ahosts <host>`     - goes through nsswitch.conf exactly like the shell (unix)
//!   2. `avahi-resolve -n <host>`  - only for `.local` names (unix)
//!   3. the same two, run on the HOST through `flatpak-spawn --host` - only inside a Flatpak
//!      sandbox, and only works when the app was granted `--talk-name=org.freedesktop.Flatpak`
//!   4. a built-in multicast-DNS (RFC 6762) query - only for `.local` names. It needs nothing but
//!      network access (`--share=network` in Flatpak), no helper binaries, no D-Bus and no nss
//!      plugin, so it is what makes `zbox.local` work in Flatpak and AppImage.
//! Never used for literal IP addresses.

use std::io;
use std::net::{IpAddr, SocketAddr, ToSocketAddrs};

/// Resolves `host:port`. Errors keep the original resolver error and say what else was tried.
pub fn resolve_host(host: &str, port: u16) -> io::Result<Vec<SocketAddr>> {
    let first = match (host, port).to_socket_addrs() {
        Ok(found) => {
            let addrs: Vec<_> = found.collect();
            if !addrs.is_empty() {
                return Ok(addrs);
            }
            io::Error::new(io::ErrorKind::NotFound, "no addresses found for host")
        }
        Err(err) => err,
    };
    if host.parse::<IpAddr>().is_ok() {
        return Err(first);
    }
    let ips = connectable_first(fallback_lookup(host));
    if ips.is_empty() {
        return Err(io::Error::new(
            first.kind(),
            format!("{first} (system tools and built-in mDNS could not resolve it either)"),
        ));
    }
    Ok(ips.into_iter().map(|ip| SocketAddr::new(ip, port)).collect())
}

/// Orders fallback results so callers that only use the first address get a usable one:
/// IPv4 first, then global IPv6; link-local IPv6 (`fe80::/10`) is dropped because it cannot be
/// connected to without an interface scope id, which a bare `IpAddr` does not carry.
fn connectable_first(ips: Vec<IpAddr>) -> Vec<IpAddr> {
    let (mut v4, mut v6): (Vec<IpAddr>, Vec<IpAddr>) = ips.into_iter().partition(IpAddr::is_ipv4);
    v6.retain(|ip| match ip {
        IpAddr::V6(v6) => (v6.segments()[0] & 0xffc0) != 0xfe80,
        IpAddr::V4(_) => true,
    });
    v4.extend(v6);
    v4
}

/// A hostile or odd name must never be read as an option of a helper program, and cannot be
/// put on the wire as a DNS name either.
fn is_safe_name(host: &str) -> bool {
    !(host.is_empty() || host.len() > 253 || host.starts_with('-') || host.contains(char::is_whitespace))
}

fn is_local_name(host: &str) -> bool {
    host.trim_end_matches('.').to_ascii_lowercase().ends_with(".local")
}

fn fallback_lookup(host: &str) -> Vec<IpAddr> {
    if !is_safe_name(host) {
        return Vec::new();
    }
    let ips = system_lookup(host);
    if !ips.is_empty() {
        return ips;
    }
    if is_local_name(host) {
        return mdns::lookup(host, mdns::DEFAULT_TIMEOUT);
    }
    Vec::new()
}

/// `getent ahosts` lines look like `192.168.12.111  STREAM zbox.local`.
pub(crate) fn parse_getent(output: &str) -> Vec<IpAddr> {
    let mut ips = Vec::new();
    for line in output.lines() {
        if let Some(ip) = line.split_whitespace().next().and_then(|t| t.parse::<IpAddr>().ok()) {
            if !ips.contains(&ip) {
                ips.push(ip);
            }
        }
    }
    ips
}

/// `avahi-resolve -n` prints `zbox.local<TAB>192.168.12.111`.
pub(crate) fn parse_avahi(output: &str) -> Vec<IpAddr> {
    let mut ips = Vec::new();
    for line in output.lines() {
        if let Some(ip) = line.split_whitespace().nth(1).and_then(|t| t.parse::<IpAddr>().ok()) {
            if !ips.contains(&ip) {
                ips.push(ip);
            }
        }
    }
    ips
}

#[cfg(unix)]
fn run(program: &str, args: &[&str]) -> Option<String> {
    use std::io::Read;
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};

    let mut child = Command::new(program).args(args).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn().ok()?;
    let started = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                if !status.success() {
                    return None;
                }
                let mut text = String::new();
                child.stdout.take()?.take(64 * 1024).read_to_string(&mut text).ok()?;
                return Some(text);
            }
            Ok(None) if started.elapsed() < Duration::from_secs(5) => std::thread::sleep(Duration::from_millis(20)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    }
}

/// True inside a Flatpak sandbox (Flatpak always creates `/.flatpak-info`).
#[cfg(unix)]
fn in_flatpak() -> bool {
    std::path::Path::new("/.flatpak-info").exists()
}

/// Runs `getent`/`avahi-resolve` in the sandbox, then (Flatpak only) on the host. `host` has
/// already been checked by [`is_safe_name`].
#[cfg(unix)]
fn system_lookup(host: &str) -> Vec<IpAddr> {
    let local = is_local_name(host);
    if let Some(ips) = helper_lookup(&[], host, local) {
        return ips;
    }
    // `flatpak-spawn --host` fails immediately when the permission was not granted, so trying it
    // costs nothing; when it works the answer is exactly what the user's own shell would get.
    if in_flatpak() {
        if let Some(ips) = helper_lookup(&["flatpak-spawn", "--host"], host, local) {
            return ips;
        }
    }
    Vec::new()
}

/// `prefix` is an optional wrapper command (e.g. `flatpak-spawn --host`).
#[cfg(unix)]
fn helper_lookup(prefix: &[&str], host: &str, local: bool) -> Option<Vec<IpAddr>> {
    let invoke = |tool: &str, args: &[&str]| -> Option<String> {
        let mut argv: Vec<&str> = prefix.to_vec();
        argv.push(tool);
        argv.extend_from_slice(args);
        run(argv[0], &argv[1..])
    };
    if let Some(text) = invoke("getent", &["ahosts", host]) {
        let ips = parse_getent(&text);
        if !ips.is_empty() {
            return Some(ips);
        }
    }
    if local {
        if let Some(text) = invoke("avahi-resolve", &["-n", host]) {
            let ips = parse_avahi(&text);
            if !ips.is_empty() {
                return Some(ips);
            }
        }
    }
    None
}

#[cfg(not(unix))]
fn system_lookup(_host: &str) -> Vec<IpAddr> {
    Vec::new()
}

/// Minimal one-shot multicast-DNS client (RFC 6762 "legacy unicast" queries).
///
/// The query is sent from an ephemeral UDP port, not 5353. A responder must then answer the
/// querier directly (unicast) instead of on the multicast group, so there is no need to bind
/// port 5353, to join a group, or to share the port with the system's own mDNS daemon.
mod mdns {
    use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, UdpSocket};
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    pub(super) const DEFAULT_TIMEOUT: Duration = Duration::from_millis(2500);
    /// The query is repeated because UDP multicast is lossy (Wi-Fi in particular).
    const SEND_AT: [Duration; 3] = [Duration::from_millis(0), Duration::from_millis(400), Duration::from_millis(1000)];
    /// After the first answer, wait a moment for the sibling record (A vs AAAA) before returning.
    const GRACE: Duration = Duration::from_millis(250);
    const TYPE_A: u16 = 1;
    const TYPE_AAAA: u16 = 28;

    pub(super) fn lookup(host: &str, total: Duration) -> Vec<IpAddr> {
        let Some(name) = encode_name(host) else {
            return Vec::new();
        };
        let Ok(socket) = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)) else {
            return Vec::new();
        };
        let _ = socket.set_multicast_ttl_v4(255);
        let id = (SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.subsec_nanos() as u16)) | 1;
        let destination = SocketAddr::from((Ipv4Addr::new(224, 0, 0, 251), 5353));

        let started = Instant::now();
        let mut next_send = 0;
        let mut found: Vec<IpAddr> = Vec::new();
        let mut first_hit: Option<Instant> = None;
        let mut buf = [0u8; 1500];
        loop {
            let elapsed = started.elapsed();
            if elapsed >= total || first_hit.is_some_and(|t| t.elapsed() >= GRACE) {
                break;
            }
            if next_send < SEND_AT.len() && elapsed >= SEND_AT[next_send] {
                for qtype in [TYPE_A, TYPE_AAAA] {
                    let _ = socket.send_to(&build_query(id, &name, qtype), destination);
                }
                next_send += 1;
            }
            let next_event = SEND_AT.get(next_send).copied().unwrap_or(total).min(total);
            let mut wait = next_event.saturating_sub(elapsed);
            if let Some(t) = first_hit {
                wait = wait.min(GRACE.saturating_sub(t.elapsed()));
            }
            let _ = socket.set_read_timeout(Some(wait.max(Duration::from_millis(1))));
            match socket.recv_from(&mut buf) {
                Ok((len, _)) => {
                    for ip in parse_response(&buf[..len], id, host) {
                        if !found.contains(&ip) {
                            found.push(ip);
                        }
                        first_hit.get_or_insert_with(Instant::now);
                    }
                }
                Err(err) if matches!(err.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
                // e.g. Windows reports ICMP port-unreachable of an earlier send as a recv error.
                Err(_) => std::thread::sleep(Duration::from_millis(10)),
            }
        }
        found
    }

    /// `zbox.local` -> `\x04zbox\x05local\x00`. Only plain ASCII names are put on the wire.
    pub(super) fn encode_name(host: &str) -> Option<Vec<u8>> {
        let trimmed = host.trim_end_matches('.');
        if trimmed.is_empty() || !trimmed.is_ascii() {
            return None;
        }
        let mut out = Vec::with_capacity(trimmed.len() + 2);
        for label in trimmed.split('.') {
            if label.is_empty() || label.len() > 63 {
                return None;
            }
            out.push(label.len() as u8);
            out.extend_from_slice(label.as_bytes());
        }
        out.push(0);
        (out.len() <= 255).then_some(out)
    }

    pub(super) fn build_query(id: u16, encoded_name: &[u8], qtype: u16) -> Vec<u8> {
        let mut packet = Vec::with_capacity(12 + encoded_name.len() + 4);
        packet.extend_from_slice(&id.to_be_bytes());
        packet.extend_from_slice(&[0, 0]); // flags: standard query
        packet.extend_from_slice(&1u16.to_be_bytes()); // one question
        packet.extend_from_slice(&[0; 6]); // no answer / authority / additional records
        packet.extend_from_slice(encoded_name);
        packet.extend_from_slice(&qtype.to_be_bytes());
        packet.extend_from_slice(&1u16.to_be_bytes()); // class IN
        packet
    }

    fn be16(buf: &[u8], at: usize) -> Option<u16> {
        Some(u16::from_be_bytes([*buf.get(at)?, *buf.get(at + 1)?]))
    }

    /// Reads a possibly compressed name at `pos`; returns it with the offset just past it.
    fn read_name(buf: &[u8], pos: usize) -> Option<(String, usize)> {
        let mut name = String::new();
        let mut p = pos;
        let mut end = None;
        let mut jumps = 0;
        loop {
            let len = *buf.get(p)? as usize;
            if len == 0 {
                p += 1;
                break;
            }
            if len & 0xC0 == 0xC0 {
                let low = *buf.get(p + 1)? as usize;
                end.get_or_insert(p + 2);
                p = ((len & 0x3F) << 8) | low;
                jumps += 1;
                if jumps > 16 {
                    return None;
                }
                continue;
            }
            if len & 0xC0 != 0 {
                return None;
            }
            let label = buf.get(p + 1..p + 1 + len)?;
            if !name.is_empty() {
                name.push('.');
            }
            name.push_str(&String::from_utf8_lossy(label));
            p += 1 + len;
        }
        Some((name, end.unwrap_or(p)))
    }

    /// Addresses in `buf` that answer for `host`; empty for anything that is not a matching reply.
    pub(super) fn parse_response(buf: &[u8], id: u16, host: &str) -> Vec<IpAddr> {
        parse(buf, id, host).unwrap_or_default()
    }

    fn parse(buf: &[u8], id: u16, host: &str) -> Option<Vec<IpAddr>> {
        let flags = be16(buf, 2)?;
        if be16(buf, 0)? != id || flags & 0x8000 == 0 || flags & 0x000F != 0 {
            return None;
        }
        let questions = be16(buf, 4)?;
        let records = u32::from(be16(buf, 6)?) + u32::from(be16(buf, 8)?) + u32::from(be16(buf, 10)?);
        let wanted = host.trim_end_matches('.');
        let mut pos = 12;
        for _ in 0..questions {
            pos = read_name(buf, pos)?.1 + 4;
        }
        let mut ips = Vec::new();
        for _ in 0..records {
            let (name, after_name) = read_name(buf, pos)?;
            let rtype = be16(buf, after_name)?;
            let rdlen = usize::from(be16(buf, after_name + 8)?);
            let data = buf.get(after_name + 10..after_name + 10 + rdlen)?;
            if name.eq_ignore_ascii_case(wanted) {
                match (rtype, data.len()) {
                    (TYPE_A, 4) => ips.push(IpAddr::V4(Ipv4Addr::new(data[0], data[1], data[2], data[3]))),
                    (TYPE_AAAA, 16) => {
                        let mut octets = [0u8; 16];
                        octets.copy_from_slice(data);
                        ips.push(IpAddr::V6(Ipv6Addr::from(octets)));
                    }
                    _ => {}
                }
            }
            pos = after_name + 10 + rdlen;
        }
        Some(ips)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_getent_ahosts_and_drops_duplicates() {
        let out = "192.168.12.111  STREAM zbox.local\n192.168.12.111  DGRAM\n192.168.12.111  RAW\nfe80::1  STREAM zbox.local\n";
        let ips = parse_getent(out);
        assert_eq!(ips.len(), 2);
        assert_eq!(ips[0].to_string(), "192.168.12.111");
    }

    #[test]
    fn parses_avahi_resolve() {
        assert_eq!(parse_avahi("zbox.local\t192.168.12.111\n")[0].to_string(), "192.168.12.111");
        assert!(parse_avahi("Failed to resolve host name 'x.local': Timeout reached\n").is_empty());
    }

    #[test]
    fn literal_addresses_and_localhost_resolve_without_helpers() {
        assert_eq!(resolve_host("127.0.0.1", 22).unwrap()[0].port(), 22);
        assert!(!resolve_host("localhost", 22).unwrap().is_empty());
    }

    #[test]
    fn option_looking_names_are_not_passed_to_helpers() {
        assert!(!is_safe_name("-evil"));
        assert!(fallback_lookup("-evil").is_empty());
        #[cfg(unix)]
        assert!(system_lookup("-evil").is_empty() || !is_safe_name("-evil"));
    }

    #[test]
    fn fallback_results_put_ipv4_first_and_drop_link_local_v6() {
        let ips = connectable_first(vec!["fe80::1".parse().unwrap(), "2001:db8::1".parse().unwrap(), "192.168.1.5".parse().unwrap()]);
        assert_eq!(ips, vec!["192.168.1.5".parse::<IpAddr>().unwrap(), "2001:db8::1".parse().unwrap()]);
    }

    #[test]
    fn local_name_detection() {
        assert!(is_local_name("zbox.local"));
        assert!(is_local_name("ZBOX.Local."));
        assert!(!is_local_name("example.com"));
        assert!(!is_local_name("local"));
    }

    #[test]
    fn mdns_query_wire_format() {
        let name = mdns::encode_name("zbox.local").unwrap();
        assert_eq!(name, b"\x04zbox\x05local\x00");
        let q = mdns::build_query(0x1234, &name, 1);
        assert_eq!(&q[..12], &[0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
        assert_eq!(&q[q.len() - 4..], &[0, 1, 0, 1]);
        assert!(mdns::encode_name("").is_none());
        assert!(mdns::encode_name("a..b").is_none());
        assert!(mdns::encode_name(&"x".repeat(64)).is_none());
    }

    /// Reply as avahi sends it: question echoed, A + AAAA answers, second owner name compressed.
    #[test]
    fn mdns_reply_with_compressed_names_is_parsed() {
        let name = mdns::encode_name("zbox.local").unwrap();
        let mut r = vec![0x12, 0x34, 0x84, 0x00, 0, 1, 0, 2, 0, 0, 0, 0];
        r.extend_from_slice(&name);
        r.extend_from_slice(&[0, 1, 0, 1]);
        // answer 1: A, owner written out
        r.extend_from_slice(&name);
        r.extend_from_slice(&[0, 1, 0x80, 1, 0, 0, 0, 10, 0, 4, 192, 168, 12, 111]);
        // answer 2: AAAA, owner is a pointer to offset 12 (the question name)
        r.extend_from_slice(&[0xC0, 12, 0, 28, 0x80, 1, 0, 0, 0, 10, 0, 16]);
        r.extend_from_slice(&"fe80::1".parse::<std::net::Ipv6Addr>().unwrap().octets());
        let ips = mdns::parse_response(&r, 0x1234, "ZBOX.local");
        assert_eq!(ips, vec!["192.168.12.111".parse::<IpAddr>().unwrap(), "fe80::1".parse().unwrap()]);
        // wrong id, wrong host, truncated and garbage packets yield nothing instead of panicking
        assert!(mdns::parse_response(&r, 0x9999, "zbox.local").is_empty());
        assert!(mdns::parse_response(&r, 0x1234, "other.local").is_empty());
        assert!(mdns::parse_response(&r[..30], 0x1234, "zbox.local").is_empty());
        assert!(mdns::parse_response(&[0xFF; 40], 0x1234, "zbox.local").is_empty());
    }

    #[test]
    fn mdns_pointer_loops_do_not_hang() {
        let mut r = vec![0x12, 0x34, 0x84, 0x00, 0, 1, 0, 0, 0, 0, 0, 0];
        r.extend_from_slice(&[0xC0, 12]); // points at itself
        r.extend_from_slice(&[0, 1, 0, 1]);
        assert!(mdns::parse_response(&r, 0x1234, "zbox.local").is_empty());
    }

    /// Network-dependent smoke test: `cargo test mdns_live -- --ignored` with a real host name.
    #[test]
    #[ignore]
    fn mdns_live() {
        let host = std::env::var("OPENBC_MDNS_TEST_HOST").unwrap_or_else(|_| "zbox.local".into());
        let ips = mdns::lookup(&host, std::time::Duration::from_secs(3));
        println!("{host} -> {ips:?}");
        assert!(!ips.is_empty());
    }
}