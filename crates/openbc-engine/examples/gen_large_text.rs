#![warn(dead_code, unused)]

use std::env;
use std::error::Error;
use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::PathBuf;

#[derive(Clone, Copy)]
enum Shape {
    Log,
    Json,
    Code,
    Minified,
    Random,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Encoding {
    Utf8,
    Utf16Le,
    Utf16Be,
}

struct Args {
    size: u64,
    shape: Shape,
    encoding: Encoding,
    diff_rate: f64,
    block_moves: usize,
    seed: u64,
    left: PathBuf,
    right: PathBuf,
}

#[derive(Clone, Copy)]
struct Block {
    start: u64,
    len: u64,
}

fn main() -> Result<(), Box<dyn Error>> {
    let args = parse_args()?;
    let line_count = (args.size / typical_line_size(args.shape)).max(1);
    let blocks = make_blocks(line_count, args.block_moves, args.seed);

    let left_file = File::create(&args.left)?;
    let right_file = File::create(&args.right)?;
    let mut left = BufWriter::with_capacity(1024 * 1024, left_file);
    let mut right = BufWriter::with_capacity(1024 * 1024, right_file);

    let bom = match args.encoding {
        Encoding::Utf8 => &[0xef, 0xbb, 0xbf][..],
        Encoding::Utf16Le => &[0xff, 0xfe][..],
        Encoding::Utf16Be => &[0xfe, 0xff][..],
    };
    left.write_all(bom)?;
    right.write_all(bom)?;

    for output_index in 0..line_count {
        write_record(
            &mut left,
            record(output_index, false, &args)?,
            output_index,
            line_count,
            args.encoding,
        )?;
        let source_index = moved_index(output_index, &blocks);
        write_record(
            &mut right,
            record(source_index, true, &args)?,
            output_index,
            line_count,
            args.encoding,
        )?;
    }

    left.flush()?;
    right.flush()?;
    extend_to_size(&args.left, &args.right, line_count, &args)?;
    fit_file(&args.left, args.size, args.encoding)?;
    fit_file(&args.right, args.size, args.encoding)?;
    let left_bytes = std::fs::metadata(&args.left)?.len();
    let right_bytes = std::fs::metadata(&args.right)?.len();
    println!(
        "generated {} and {} ({} / {} bytes; shape={}, encoding={}, diff_rate={}, \
         block_moves={}, seed={}, records={})",
        args.left.display(),
        args.right.display(),
        left_bytes,
        right_bytes,
        shape_name(args.shape),
        encoding_name(args.encoding),
        args.diff_rate,
        args.block_moves,
        args.seed,
        line_count
    );
    Ok(())
}

fn parse_args() -> Result<Args, Box<dyn Error>> {
    let mut size = parse_size("100MiB")?;
    let mut shape = Shape::Log;
    let mut encoding = Encoding::Utf8;
    let mut diff_rate = 0.001;
    let mut block_moves = 20;
    let mut seed = 1u64;
    let mut out = None;
    let mut args = env::args().skip(1);

    while let Some(option) = args.next() {
        match option.as_str() {
            "--size" => size = parse_size(&args.next().ok_or("--size needs a value")?)?,
            "--shape" => {
                shape = match args.next().as_deref() {
                    Some("log") => Shape::Log,
                    Some("json") => Shape::Json,
                    Some("code") => Shape::Code,
                    Some("minified") => Shape::Minified,
                    Some("random") => Shape::Random,
                    _ => return Err("--shape must be log, json, code, minified, or random".into()),
                }
            }
            "--encoding" => {
                encoding = match args.next().as_deref() {
                    Some("utf8") => Encoding::Utf8,
                    Some("utf16le") => Encoding::Utf16Le,
                    Some("utf16be") => Encoding::Utf16Be,
                    _ => return Err("--encoding must be utf8, utf16le, or utf16be".into()),
                }
            }
            "--diff-rate" => diff_rate = args.next().ok_or("--diff-rate needs a value")?.parse()?,
            "--block-moves" => {
                block_moves = args.next().ok_or("--block-moves needs a value")?.parse()?
            }
            "--seed" => seed = args.next().ok_or("--seed needs a value")?.parse()?,
            "--out" => {
                let left = PathBuf::from(args.next().ok_or("--out needs two paths")?);
                let right = PathBuf::from(args.next().ok_or("--out needs two paths")?);
                out = Some((left, right));
            }
            "--help" | "-h" => {
                println!(
                    "Usage: cargo run -p openbc-engine --example gen_large_text -- \\\n\
                     --size 500MiB --shape log|json|code|minified|random \\\n\
                     --diff-rate 0.001 --block-moves 20 --seed 1 \\\n\
                     --encoding utf8|utf16le|utf16be --out a.txt b.txt"
                );
                std::process::exit(0);
            }
            _ => return Err(format!("unknown option: {option}").into()),
        }
    }

    if !(0.0..=1.0).contains(&diff_rate) {
        return Err("--diff-rate must be between 0 and 1".into());
    }
    let (left, right) = out.ok_or("--out must specify two output paths")?;
    Ok(Args {
        size,
        shape,
        encoding,
        diff_rate,
        block_moves,
        seed,
        left,
        right,
    })
}

fn parse_size(value: &str) -> Result<u64, Box<dyn Error>> {
    let split = value
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(value.len());
    let amount: u64 = value[..split].parse()?;
    let unit = value[split..].to_ascii_lowercase();
    let multiplier = match unit.as_str() {
        "" | "b" => 1,
        "kib" => 1024,
        "mib" => 1024 * 1024,
        "gib" => 1024 * 1024 * 1024,
        "kb" => 1000,
        "mb" => 1000 * 1000,
        "gb" => 1000 * 1000 * 1000,
        _ => return Err(format!("unsupported size unit: {unit}").into()),
    };
    amount
        .checked_mul(multiplier)
        .ok_or_else(|| "size overflows u64".into())
}

fn typical_line_size(shape: Shape) -> u64 {
    match shape {
        Shape::Log => 103,
        Shape::Json => 124,
        Shape::Code => 88,
        Shape::Minified => 2304,
        Shape::Random => 129,
    }
}

fn shape_name(shape: Shape) -> &'static str {
    match shape {
        Shape::Log => "log",
        Shape::Json => "json",
        Shape::Code => "code",
        Shape::Minified => "minified",
        Shape::Random => "random",
    }
}

fn encoding_name(encoding: Encoding) -> &'static str {
    match encoding {
        Encoding::Utf8 => "utf8",
        Encoding::Utf16Le => "utf16le",
        Encoding::Utf16Be => "utf16be",
    }
}

fn make_blocks(line_count: u64, requested_moves: usize, seed: u64) -> Vec<Block> {
    let block_count = (requested_moves.saturating_add(1) as u64).min(line_count);
    let base = line_count / block_count;
    let remainder = line_count % block_count;
    let mut blocks = Vec::with_capacity(block_count as usize);
    let mut start = 0;
    for index in 0..block_count {
        let len = base + u64::from(index < remainder);
        blocks.push(Block { start, len });
        start += len;
    }

    let mut state = seed;
    for index in (1..blocks.len()).rev() {
        state = next_random(state);
        blocks.swap(index, (state as usize) % (index + 1));
    }
    blocks
}

fn moved_index(output_index: u64, blocks: &[Block]) -> u64 {
    let mut emitted = 0;
    for block in blocks {
        if output_index < emitted + block.len {
            return block.start + output_index - emitted;
        }
        emitted += block.len;
    }
    output_index
}

fn record(index: u64, changed: bool, args: &Args) -> Result<Vec<u8>, Box<dyn Error>> {
    if index == 1 {
        let mut long = vec![b'x'; 70 * 1024];
        long[..16].copy_from_slice(b"very-long-line: ");
        return Ok(long);
    }

    let mut bytes = match args.shape {
        Shape::Log => format!(
            "2026-11-04T16:{:02}:{:02}.123Z INFO worker-{:04} request={} completed duration_ms={} user=sample-{:08}",
            (index / 60) % 60,
            index % 60,
            index % 10000,
            index,
            index % 600_000,
            (index * 7919) % 100_000_000
        )
        .into_bytes(),
        Shape::Json => format!(
            "{{\"time\":\"2026-11-04T16:06:{:02}Z\",\"level\":\"info\",\"seq\":{},\"worker\":\"pod-worker-{:06}\",\"message\":\"record-{}\"}}",
            index % 60,
            index,
            index % 1_000_000,
            index
        )
        .into_bytes(),
        Shape::Code => format!(
            "fn process_{:08}(item: Record) -> Result<(), Error> {{ let value = item.field_{}; Ok(()) }}",
            index,
            index % 100_000
        )
        .into_bytes(),
        Shape::Minified => {
            let mut line = Vec::with_capacity(2048);
            for part in 0..128 {
                if part != 0 {
                    line.push(b',');
                }
                line.extend_from_slice(format!("\"k{}\":\"v{}\"", part, index + part).as_bytes());
            }
            line
        }
        Shape::Random => {
            let mut state = args.seed ^ index.wrapping_mul(0x9e3779b97f4a7c15);
            let mut line = Vec::with_capacity(128);
            for _ in 0..128 {
                state = next_random(state);
                line.push(b'!' + (state % 90) as u8);
            }
            line
        }
    };

    if args.encoding == Encoding::Utf8 && index == 2 {
        bytes.extend_from_slice(&[0xff, 0xfe, 0x80]);
    }
    if changed && should_change(index, args.seed, args.diff_rate) {
        if let Some(byte) = bytes.iter_mut().find(|byte| byte.is_ascii_alphanumeric()) {
            *byte = if *byte == b'X' { b'Y' } else { b'X' };
        }
    }
    Ok(bytes)
}

fn should_change(index: u64, seed: u64, rate: f64) -> bool {
    let mut state = seed ^ index.wrapping_mul(0xd6e8feb86659fd93);
    state = next_random(state);
    (state as f64 / u64::MAX as f64) < rate
}

fn write_record(
    output: &mut impl Write,
    line: Vec<u8>,
    index: u64,
    line_count: u64,
    encoding: Encoding,
) -> Result<(), Box<dyn Error>> {
    let newline = if index % 11 == 10 {
        "\r"
    } else if index % 5 == 4 {
        "\r\n"
    } else {
        "\n"
    };
    match encoding {
        Encoding::Utf8 => {
            output.write_all(&line)?;
            if index + 1 < line_count {
                write_encoded(output, newline.as_bytes(), encoding)?;
            }
        }
        Encoding::Utf16Le | Encoding::Utf16Be => {
            write_encoded(output, &line, encoding)?;
            if index + 1 < line_count {
                write_encoded(output, newline.as_bytes(), encoding)?;
            }
        }
    }
    Ok(())
}

fn extend_to_size(
    left_path: &PathBuf,
    right_path: &PathBuf,
    mut index: u64,
    args: &Args,
) -> Result<(), Box<dyn Error>> {
    let mut left_size = std::fs::metadata(left_path)?.len();
    let mut right_size = std::fs::metadata(right_path)?.len();
    if left_size >= args.size || right_size >= args.size {
        return Ok(());
    }

    let mut left = OpenOptions::new().append(true).open(left_path)?;
    let mut right = OpenOptions::new().append(true).open(right_path)?;
    while left_size < args.size && right_size < args.size {
        let previous_index = index - 1;
        let separator = if previous_index % 11 == 10 {
            "\r"
        } else if previous_index % 5 == 4 {
            "\r\n"
        } else {
            "\n"
        };
        let separator_bytes = separator.as_bytes();
        left_size += write_encoded(&mut left, separator_bytes, args.encoding)?;
        right_size += write_encoded(&mut right, separator_bytes, args.encoding)?;

        let left_line = record(index, false, args)?;
        let right_line = record(index, true, args)?;
        left_size += write_encoded(&mut left, &left_line, args.encoding)?;
        right_size += write_encoded(&mut right, &right_line, args.encoding)?;
        index += 1;
    }
    left.flush()?;
    right.flush()?;
    Ok(())
}

fn write_encoded(
    output: &mut impl Write,
    bytes: &[u8],
    encoding: Encoding,
) -> Result<u64, Box<dyn Error>> {
    match encoding {
        Encoding::Utf8 => {
            output.write_all(bytes)?;
            Ok(bytes.len() as u64)
        }
        Encoding::Utf16Le | Encoding::Utf16Be => {
            let text = std::str::from_utf8(bytes)?;
            let mut written = 0;
            for unit in text.encode_utf16() {
                let encoded = if matches!(encoding, Encoding::Utf16Le) {
                    unit.to_le_bytes()
                } else {
                    unit.to_be_bytes()
                };
                output.write_all(&encoded)?;
                written += encoded.len() as u64;
            }
            Ok(written)
        }
    }
}

fn fit_file(path: &PathBuf, requested_size: u64, encoding: Encoding) -> Result<(), Box<dyn Error>> {
    let mut file = OpenOptions::new().read(true).write(true).open(path)?;
    let mut length = std::fs::metadata(path)?.len().min(requested_size);
    if matches!(encoding, Encoding::Utf16Le | Encoding::Utf16Be) {
        length &= !1;
    }

    match encoding {
        Encoding::Utf8 => {
            while length > 0 {
                file.seek(SeekFrom::Start(length - 1))?;
                let mut byte = [0];
                file.read_exact(&mut byte)?;
                if byte[0] != b'\n' && byte[0] != b'\r' {
                    break;
                }
                length -= 1;
            }
        }
        Encoding::Utf16Le | Encoding::Utf16Be => {
            while length >= 2 {
                file.seek(SeekFrom::Start(length - 2))?;
                let mut bytes = [0; 2];
                file.read_exact(&mut bytes)?;
                let code_unit = if matches!(encoding, Encoding::Utf16Le) {
                    u16::from_le_bytes(bytes)
                } else {
                    u16::from_be_bytes(bytes)
                };
                if code_unit != b'\n' as u16 && code_unit != b'\r' as u16 {
                    break;
                }
                length -= 2;
            }
        }
    }
    file.set_len(length)?;
    Ok(())
}

fn next_random(value: u64) -> u64 {
    let mut x = value.wrapping_add(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d049bb133111eb);
    x ^ (x >> 31)
}
