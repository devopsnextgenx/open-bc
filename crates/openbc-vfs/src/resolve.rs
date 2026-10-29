//! Host name -> socket addresses, with a fallback to the system's own tools.
//!
//! `ToSocketAddrs` (getaddrinfo) is tried first. When it fails, the same question is put to the
//! tools the user's shell uses, so a name that works in a terminal also works here even if this
//! process cannot reach the resolver itself (static / sandboxed builds without the nss mDNS
//! plugin, `.local` names, a resolver that is only reachable through the system's helpers):
//!   1. `getent ahosts <host>`     - goes through nsswitch.conf exactly like the shell
//!   2. `avahi-resolve -n <host>`  - only for `.local` names (mDNS)
//! Unix only; elsewhere the fallback is a no-op. Never used for literal IP addresses.

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
    let ips = system_lookup(host);
    if ips.is_empty() {
        return Err(io::Error::new(
            first.kind(),
            format!("{first} (system tools `getent`/`avahi-resolve` could not resolve it either)"),
        ));
    }
    Ok(ips.into_iter().map(|ip| SocketAddr::new(ip, port)).collect())
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

#[cfg(unix)]
fn system_lookup(host: &str) -> Vec<IpAddr> {
    // A hostile or odd name must never be read as an option of the helper program.
    if host.is_empty() || host.starts_with('-') || host.contains(char::is_whitespace) {
        return Vec::new();
    }
    if let Some(text) = run("getent", &["ahosts", host]) {
        let ips = parse_getent(&text);
        if !ips.is_empty() {
            return ips;
        }
    }
    if host.to_ascii_lowercase().ends_with(".local") {
        if let Some(text) = run("avahi-resolve", &["-n", host]) {
            return parse_avahi(&text);
        }
    }
    Vec::new()
}

#[cfg(not(unix))]
fn system_lookup(_host: &str) -> Vec<IpAddr> {
    Vec::new()
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
        #[cfg(unix)]
        assert!(system_lookup("-evil").is_empty());
    }
}
