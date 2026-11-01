//! Format backends: each turns one archive file into an [`Index`] plus a [`Backend`] reader.

#![allow(unused_imports, dead_code)]

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::SystemTime;

use super::index::{civil_to_system_time, dos_to_system_time, normalize_entry_name, unix_to_system_time, Index, Node};
use super::{classify_name, ArchiveOptions, Backend, Container};

type Opened = (Index, Box<dyn Backend>);
type Res<T> = Result<T, String>;

macro_rules! gated {
    ($feature:literal, $what:literal, $body:expr) => {{
        #[cfg(feature = $feature)]
        {
            $body
        }
        #[cfg(not(feature = $feature))]
        {
            Err::<Opened, String>(format!("this build has no {} support (cargo feature `{}`)", $what, $feature))
        }
    }};
}

fn sniff(head: &[u8]) -> Option<Container> {
    let starts = |m: &[u8]| head.starts_with(m);
    if starts(b"PK\x03\x04") || starts(b"PK\x05\x06") || starts(b"PK\x07\x08") {
        Some(Container::Zip)
    } else if starts(b"7z\xBC\xAF\x27\x1C") {
        Some(Container::SevenZ)
    } else if starts(b"Rar!\x1A\x07") {
        Some(Container::Rar)
    } else if starts(b"MSCF") {
        Some(Container::Cab)
    } else if starts(b"!<arch>\n") {
        Some(Container::Ar)
    } else if starts(&[0x1f, 0x8b]) {
        Some(Container::Gzip)
    } else if starts(b"BZh") {
        Some(Container::Bzip2)
    } else if starts(&[0xfd, b'7', b'z', b'X', b'Z', 0]) {
        Some(Container::Xz)
    } else if starts(&[0x28, 0xb5, 0x2f, 0xfd]) {
        Some(Container::Zstd)
    } else if starts(&[0x04, 0x22, 0x4d, 0x18]) {
        Some(Container::Lz4)
    } else if head.len() >= 262 && &head[257..262] == b"ustar" {
        Some(Container::Tar)
    } else {
        None
    }
}

#[allow(unused_variables)]
pub(super) fn open(file: &Path, name: &str, ws: &Path, opts: &ArchiveOptions, mtime: Option<SystemTime>) -> Res<Opened> {
    let mut head = vec![0u8; 512];
    let n = File::open(file).and_then(|mut f| f.read(&mut head)).map_err(|e| format!("cannot read {name}: {e}"))?;
    head.truncate(n);
    let container = sniff(&head)
        .or_else(|| classify_name(name).map(|c| c.container))
        .ok_or_else(|| format!("{name} is not a recognised archive"))?;
    match container {
        Container::Zip => gated!("archive-zip", "zip", zip_open(file, opts)),
        Container::Tar => gated!("archive-tar", "tar", tar_open(file, opts)),
        Container::Gzip | Container::Bzip2 | Container::Xz | Container::Zstd | Container::Lz4 => {
            gated!("archive-tar", "compressed", stream_open(container, file, name, ws, opts, mtime))
        }
        Container::SevenZ => gated!("archive-7z", "7z", sevenz_open(file, ws, opts)),
        Container::Rar => gated!("archive-rar", "rar", rar_open(file, ws, opts)),
        Container::Cab => gated!("archive-cab", "cab", cab_open(file, opts)),
        Container::Ar => gated!("archive-ar", "ar/deb", ar_open(file, opts)),
    }
}

fn too_many(opts: &ArchiveOptions) -> String {
    format!("archive has more than {} entries", opts.max_entries)
}

fn read_limited(r: &mut dyn Read, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
    let mut buf = Vec::new();
    r.take(limit.saturating_add(1)).read_to_end(&mut buf)?;
    Ok((buf.len() as u64 <= limit).then_some(buf))
}

// ------------------------------------------------------------------------------------- zip

#[cfg(feature = "archive-zip")]
struct ZipBackend(Mutex<zip::ZipArchive<BufReader<File>>>);

#[cfg(feature = "archive-zip")]
impl Backend for ZipBackend {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
        let mut z = self.0.lock().unwrap_or_else(|p| p.into_inner());
        let mut entry = z.by_index(id).map_err(std::io::Error::other)?;
        if entry.size() > limit {
            return Ok(None);
        }
        read_limited(&mut entry, limit)
    }
}

#[cfg(feature = "archive-zip")]
fn zip_open(file: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    let mut z = zip::ZipArchive::new(BufReader::new(File::open(file).map_err(|e| e.to_string())?)).map_err(|e| e.to_string())?;
    let mut index = Index::default();
    for i in 0..z.len() {
        if index.len() >= opts.max_entries {
            return Err(too_many(opts));
        }
        let e = z.by_index_raw(i).map_err(|e| e.to_string())?;
        let modified = e.last_modified().and_then(|d| {
            civil_to_system_time(d.year() as i64, d.month() as i64, d.day() as i64, d.hour() as i64, d.minute() as i64, d.second() as i64)
        });
        let dir = e.is_dir();
        let link = e.is_symlink();
        let name = e.name().map(|n| n.into_owned()).unwrap_or_default();
        index.add(&name, Node { dir, link, size: if dir { 0 } else { e.size() }, modified, id: i });
    }
    Ok((index, Box::new(ZipBackend(Mutex::new(z)))))
}

// ------------------------------------------------------------------- tar (+ raw data files)

/// Entries stored as `(offset, length)` spans of one plain file: used for tar and for the
/// expanded single-file compressions.
#[cfg(feature = "archive-tar")]
struct SpanBackend {
    file: Mutex<File>,
    spans: Vec<(u64, u64)>,
}

#[cfg(feature = "archive-tar")]
impl Backend for SpanBackend {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
        let (off, len) = *self.spans.get(id).ok_or_else(|| std::io::Error::other("bad entry id"))?;
        if len > limit {
            return Ok(None);
        }
        let mut f = self.file.lock().unwrap_or_else(|p| p.into_inner());
        f.seek(SeekFrom::Start(off))?;
        let mut buf = vec![0u8; len as usize];
        f.read_exact(&mut buf)?;
        Ok(Some(buf))
    }
}

#[cfg(feature = "archive-tar")]
fn tar_open(file: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    use tar::EntryType as T;
    let mut ar = tar::Archive::new(File::open(file).map_err(|e| e.to_string())?);
    let mut index = Index::default();
    let mut spans: Vec<(u64, u64)> = Vec::new();
    let mut by_name: HashMap<String, usize> = HashMap::new();
    for entry in ar.entries_with_seek().map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if index.len() >= opts.max_entries {
            return Err(too_many(opts));
        }
        let name = String::from_utf8_lossy(&entry.path_bytes()).into_owned();
        let modified = entry.header().mtime().ok().and_then(unix_to_system_time);
        let (pos, size) = (entry.raw_file_position(), entry.size());
        match entry.header().entry_type() {
            T::Directory => {
                index.add(&name, Node { dir: true, link: false, size: 0, modified, id: 0 });
            }
            T::Regular | T::Continuous => {
                spans.push((pos, size));
                let id = spans.len() - 1;
                if index.add(&name, Node { dir: false, link: false, size, modified, id }) {
                    if let Some(k) = normalize_entry_name(&name) {
                        by_name.insert(k, id);
                    }
                }
            }
            T::Symlink => {
                index.add(&name, Node { dir: false, link: true, size: 0, modified, id: 0 });
            }
            T::Link => {
                let target = entry.link_name_bytes().and_then(|b| normalize_entry_name(&String::from_utf8_lossy(&b)));
                match target.and_then(|t| by_name.get(&t).copied()) {
                    Some(id) => {
                        let len = spans[id].1;
                        index.add(&name, Node { dir: false, link: false, size: len, modified, id });
                    }
                    None => index.skipped += 1,
                }
            }
            T::XGlobalHeader | T::XHeader | T::GNULongName | T::GNULongLink => {}
            _ => index.skipped += 1, // sparse files, devices, fifos
        }
    }
    let file = File::open(file).map_err(|e| e.to_string())?;
    Ok((index, Box::new(SpanBackend { file: Mutex::new(file), spans })))
}

/// gz / bz2 / xz / zst / lz4: expand once into the workspace, then it is either a tarball or one
/// file.
#[cfg(feature = "archive-tar")]
fn stream_open(kind: Container, file: &Path, name: &str, ws: &Path, opts: &ArchiveOptions, mtime: Option<SystemTime>) -> Res<Opened> {
    let expanded = ws.join("expanded");
    let input = BufReader::new(File::open(file).map_err(|e| e.to_string())?);
    let mut reader: Box<dyn Read> = match kind {
        Container::Gzip => Box::new(flate2::read::MultiGzDecoder::new(input)),
        Container::Bzip2 => Box::new(bzip2::read::MultiBzDecoder::new(input)),
        Container::Xz => Box::new(lzma_rust2::XzReader::new(input, true)),
        Container::Zstd => Box::new(zstd::stream::read::Decoder::new(input).map_err(|e| e.to_string())?),
        Container::Lz4 => Box::new(lz4_flex::frame::FrameDecoder::new(input)),
        _ => return Err("not a stream compression".into()),
    };
    let cap = opts.max_expanded_bytes;
    let mut out = BufWriter::new(File::create(&expanded).map_err(|e| e.to_string())?);
    let written = std::io::copy(&mut reader.by_ref().take(cap + 1), &mut out).map_err(|e| format!("cannot decompress {name}: {e}"))?;
    out.flush().map_err(|e| e.to_string())?;
    drop(out);
    if written > cap {
        return Err(format!("{name} expands to more than {} MiB; refusing to open it", cap >> 20));
    }
    let mut head = [0u8; 512];
    let n = File::open(&expanded).and_then(|mut f| f.read(&mut head)).map_err(|e| e.to_string())?;
    if sniff(&head[..n]) == Some(Container::Tar) {
        return tar_open(&expanded, opts);
    }
    // One file; name it like `gunzip` would.
    let inner = match name.rsplit_once('.') {
        Some((stem, ext)) if ext.eq_ignore_ascii_case("svgz") => format!("{stem}.svg"),
        Some((stem, _)) if !stem.is_empty() => stem.to_owned(),
        _ => "data".to_owned(),
    };
    let mut index = Index::default();
    index.add(&inner, Node { dir: false, link: false, size: written, modified: mtime, id: 0 });
    let f = File::open(&expanded).map_err(|e| e.to_string())?;
    Ok((index, Box::new(SpanBackend { file: Mutex::new(f), spans: vec![(0, written)] })))
}

// ------------------------------------------------------- solid formats: expand once, lazily

/// Entries live as files `<dir>/<id>`, written by `fill` the first time any content is read.
/// Listing never triggers it.
struct StoreBackend {
    dir: PathBuf,
    file_ids: Vec<usize>,
    total: u64,
    cap: u64,
    state: Mutex<Option<Result<(), String>>>,
    fill: Box<dyn Fn(&Path) -> Result<(), String> + Send + Sync>,
}

impl StoreBackend {
    fn ensure(&self) -> std::io::Result<()> {
        let mut st = self.state.lock().unwrap_or_else(|p| p.into_inner());
        if st.is_none() {
            *st = Some(if self.total > self.cap {
                Err(format!("archive expands to {} MiB, over the {} MiB limit", self.total >> 20, self.cap >> 20))
            } else {
                std::fs::create_dir_all(&self.dir).map_err(|e| e.to_string()).and_then(|()| {
                    for id in &self.file_ids {
                        File::create(self.dir.join(id.to_string())).map_err(|e| e.to_string())?;
                    }
                    (self.fill)(&self.dir)
                })
            });
        }
        st.clone().unwrap().map_err(std::io::Error::other)
    }
}

impl Backend for StoreBackend {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
        self.ensure()?;
        let mut f = File::open(self.dir.join(id.to_string()))?;
        read_limited(&mut f, limit)
    }
}

fn store(ws: &Path, index: &Index, fill: impl Fn(&Path) -> Result<(), String> + Send + Sync + 'static, ids: Vec<usize>, opts: &ArchiveOptions) -> Box<dyn Backend> {
    Box::new(StoreBackend {
        dir: ws.join("store"),
        file_ids: ids,
        total: index.total_file_bytes(),
        cap: opts.max_expanded_bytes,
        state: Mutex::new(None),
        fill: Box::new(fill),
    })
}

#[cfg(feature = "archive-7z")]
fn sevenz_open(file: &Path, ws: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    let archive = sevenz_rust2::Archive::open(file).map_err(|e| e.to_string())?;
    let mut index = Index::default();
    let mut names: HashMap<String, usize> = HashMap::new();
    for (i, f) in archive.files.iter().enumerate() {
        if index.len() >= opts.max_entries {
            return Err(too_many(opts));
        }
        if f.is_anti_item() {
            continue;
        }
        let modified = f.has_last_modified_date.then(|| SystemTime::from(f.last_modified_date));
        // Windows reparse-point/symlink attribute bit (0x400) or the Unix S_IFLNK mode in the high half.
        let attrs = f.windows_attributes;
        let link = f.has_windows_attributes && (attrs & 0x400 != 0 || (attrs & 0x8000 != 0 && (attrs >> 16) & 0o170000 == 0o120000));
        if index.add(f.name(), Node { dir: f.is_directory(), link, size: f.size, modified, id: i }) && !f.is_directory() {
            if let Some(k) = normalize_entry_name(f.name()) {
                names.insert(k, i);
            }
        }
    }
    let ids: Vec<usize> = names.values().copied().collect();
    let path = file.to_path_buf();
    let cap = opts.max_expanded_bytes;
    let fill = move |dir: &Path| -> Result<(), String> {
        let mut reader = sevenz_rust2::ArchiveReader::open(&path, sevenz_rust2::Password::empty()).map_err(|e| e.to_string())?;
        reader
            .for_each_entries(|entry, data| {
                if entry.is_directory() {
                    return Ok(true);
                }
                if let Some(id) = normalize_entry_name(entry.name()).and_then(|k| names.get(&k).copied()) {
                    let mut out = File::create(dir.join(id.to_string()))?;
                    std::io::copy(&mut data.take(cap), &mut out)?;
                }
                Ok(true)
            })
            .map_err(|e| e.to_string())
    };
    let backend = store(ws, &index, fill, ids, opts);
    Ok((index, backend))
}

#[cfg(feature = "archive-rar")]
fn rar_open(file: &Path, ws: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    let path = file.to_path_buf();
    let mut index = Index::default();
    let mut wanted: HashMap<String, usize> = HashMap::new();
    for (i, header) in unrar::Archive::new(&path).open_for_listing().map_err(|e| e.to_string())?.enumerate() {
        let h = header.map_err(|e| e.to_string())?;
        if index.len() >= opts.max_entries {
            return Err(too_many(opts));
        }
        let name = h.filename.to_string_lossy().into_owned();
        let dir = h.is_directory();
        if index.add(&name, Node { dir, link: false, size: if dir { 0 } else { h.unpacked_size }, modified: dos_to_system_time(h.file_time), id: i }) && !dir {
            if let Some(k) = normalize_entry_name(&name) {
                wanted.insert(k, i);
            }
        }
    }
    let ids: Vec<usize> = wanted.values().copied().collect();
    let want: HashSet<usize> = ids.iter().copied().collect();
    let fill = move |dir: &Path| -> Result<(), String> {
        let mut cursor = unrar::Archive::new(&path).open_for_processing().map_err(|e| e.to_string())?;
        let mut i = 0usize;
        while let Some(header) = cursor.read_header().map_err(|e| e.to_string())? {
            cursor = if header.entry().is_file() && want.contains(&i) {
                header.extract_to(dir.join(i.to_string())).map_err(|e| e.to_string())?
            } else {
                header.skip().map_err(|e| e.to_string())?
            };
            i += 1;
        }
        Ok(())
    };
    let backend = store(ws, &index, fill, ids, opts);
    Ok((index, backend))
}

// ------------------------------------------------------------------------------- cab / ar

#[cfg(feature = "archive-cab")]
struct CabBackend {
    cab: Mutex<cab::Cabinet<File>>,
    names: Vec<String>,
    sizes: Vec<u64>,
}

#[cfg(feature = "archive-cab")]
impl Backend for CabBackend {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
        if self.sizes[id] > limit {
            return Ok(None);
        }
        let mut cab = self.cab.lock().unwrap_or_else(|p| p.into_inner());
        let mut r = cab.read_file(&self.names[id])?;
        read_limited(&mut r, limit)
    }
}

#[cfg(feature = "archive-cab")]
fn cab_open(file: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    let cabinet = cab::Cabinet::new(File::open(file).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    let mut index = Index::default();
    let (mut names, mut sizes) = (Vec::new(), Vec::new());
    for folder in cabinet.folder_entries() {
        for f in folder.file_entries() {
            if index.len() >= opts.max_entries {
                return Err(too_many(opts));
            }
            let modified = f.datetime().and_then(|d| {
                civil_to_system_time(d.year() as i64, u8::from(d.month()) as i64, d.day() as i64, d.hour() as i64, d.minute() as i64, d.second() as i64)
            });
            let size = u64::from(f.uncompressed_size());
            names.push(f.name().to_owned());
            sizes.push(size);
            index.add(f.name(), Node { dir: false, link: false, size, modified, id: names.len() - 1 });
        }
    }
    Ok((index, Box::new(CabBackend { cab: Mutex::new(cabinet), names, sizes })))
}

#[cfg(feature = "archive-ar")]
struct ArBackend(Mutex<ar::Archive<File>>);

#[cfg(feature = "archive-ar")]
impl Backend for ArBackend {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>> {
        let mut a = self.0.lock().unwrap_or_else(|p| p.into_inner());
        let mut e = a.jump_to_entry(id)?;
        if e.header().size() > limit {
            return Ok(None);
        }
        read_limited(&mut e, limit)
    }
}

#[cfg(feature = "archive-ar")]
fn ar_open(file: &Path, opts: &ArchiveOptions) -> Res<Opened> {
    let mut a = ar::Archive::new(File::open(file).map_err(|e| e.to_string())?);
    let mut index = Index::default();
    let mut i = 0usize;
    while let Some(entry) = a.next_entry() {
        let entry = entry.map_err(|e| e.to_string())?;
        if index.len() >= opts.max_entries {
            return Err(too_many(opts));
        }
        let h = entry.header();
        let name = String::from_utf8_lossy(h.identifier()).into_owned();
        index.add(&name, Node { dir: false, link: false, size: h.size(), modified: unix_to_system_time(h.mtime()), id: i });
        i += 1;
    }
    Ok((index, Box::new(ArBackend(Mutex::new(a)))))
}
