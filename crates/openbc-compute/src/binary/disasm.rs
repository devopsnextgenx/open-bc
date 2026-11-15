//! Basic x86-64 disassembler for the Binary Compare inspector. Pure Rust, no dependency.
//!
//! This is deliberately a *basic* decoder: it covers the instruction forms that make up ordinary
//! compiled code (integer ALU, moves, `lea`, stack, branches and calls, `setcc` / `cmovcc`,
//! shifts, `imul`, `movzx` / `movsx`, `endbr64`, the common scalar / packed SSE moves and
//! arithmetic). Anything it does not recognise is emitted as `db 0xNN` for one byte and decoding
//! resumes at the next byte, so the diff still lines up for the rest of the function; it never
//! panics on arbitrary data. Text uses Intel syntax and offsets are *relative to the start of the
//! function*, so a function that only moved disassembles identically in both versions.

use openbc_core::binary_compare::inspect::MAX_INSTRUCTIONS;
use openbc_core::binary_compare::AsmLine;

/// Instruction-set width for an `architecture` metadata value (`X86_64`, `AMD64`, `x86_64`, ...),
/// or `None` when no disassembler is available for it (32-bit x86, ARM, ...).
#[must_use]
pub fn bitness_for(architecture: &str) -> Option<u32> {
    let a = architecture.to_ascii_lowercase();
    (a.contains("x86_64") || a.contains("x86-64") || a.contains("amd64") || a.contains("x64")).then_some(64)
}

/// Disassemble `bytes` (at most [`MAX_INSTRUCTIONS`] instructions). Only `bitness == 64` is
/// supported; anything else returns no lines.
#[must_use]
pub fn disassemble(bitness: u32, bytes: &[u8]) -> Vec<AsmLine> {
    let mut out = Vec::new();
    if bitness != 64 {
        return out;
    }
    let mut pos = 0usize;
    while pos < bytes.len() && out.len() < MAX_INSTRUCTIONS {
        match decode(bytes, pos) {
            Some((next, text)) if next > pos => {
                out.push(AsmLine { offset: pos as u64, text });
                pos = next;
            }
            _ => {
                out.push(AsmLine { offset: pos as u64, text: format!("db 0x{:02x}", bytes[pos]) });
                pos += 1;
            }
        }
    }
    out
}

const R64: [&str; 16] =
    ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"];
const R32: [&str; 16] = [
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d",
    "r15d",
];
const R16: [&str; 16] =
    ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"];
const R8_REX: [&str; 16] = [
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
];
const R8_LEGACY: [&str; 8] = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"];
const ALU: [&str; 8] = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"];
const SHIFT: [&str; 8] = ["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"];
const GROUP3: [&str; 8] = ["test", "test", "not", "neg", "mul", "imul", "div", "idiv"];
const CC: [&str; 16] = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"];

/// Signed value as `0x..` / `-0x..`.
fn imm(v: i64) -> String {
    if v < 0 {
        format!("-0x{:x}", v.unsigned_abs())
    } else {
        format!("0x{v:x}")
    }
}

enum Rm {
    Reg(u8),
    Mem(String),
}

struct Dec<'a> {
    b: &'a [u8],
    p: usize,
    rex: u8,
    opsz16: bool,
    rep: u8,
    seg: Option<&'static str>,
}

impl Dec<'_> {
    fn u8(&mut self) -> Option<u8> {
        let v = *self.b.get(self.p)?;
        self.p += 1;
        Some(v)
    }

    fn i8(&mut self) -> Option<i64> {
        self.u8().map(|v| i64::from(v as i8))
    }

    fn i16(&mut self) -> Option<i64> {
        let s = self.b.get(self.p..self.p + 2)?;
        self.p += 2;
        Some(i64::from(i16::from_le_bytes([s[0], s[1]])))
    }

    fn i32(&mut self) -> Option<i64> {
        let s = self.b.get(self.p..self.p + 4)?;
        self.p += 4;
        Some(i64::from(i32::from_le_bytes([s[0], s[1], s[2], s[3]])))
    }

    fn u64(&mut self) -> Option<u64> {
        let s = self.b.get(self.p..self.p + 8)?;
        self.p += 8;
        let mut a = [0u8; 8];
        a.copy_from_slice(s);
        Some(u64::from_le_bytes(a))
    }

    fn w(&self) -> bool {
        self.rex & 8 != 0
    }

    fn osz(&self) -> u8 {
        if self.w() {
            64
        } else if self.opsz16 {
            16
        } else {
            32
        }
    }

    /// Immediate of operand size (16 or 32 bits, sign-extended).
    fn imm_z(&mut self) -> Option<i64> {
        if self.opsz16 {
            self.i16()
        } else {
            self.i32()
        }
    }

    fn reg(&self, n: u8, size: u8) -> String {
        let i = usize::from(n & 15);
        match size {
            64 => R64[i].to_owned(),
            32 => R32[i].to_owned(),
            16 => R16[i].to_owned(),
            _ => {
                if self.rex != 0 || i >= 8 {
                    R8_REX[i].to_owned()
                } else {
                    R8_LEGACY[i].to_owned()
                }
            }
        }
    }

    fn xmm(n: u8) -> String {
        format!("xmm{}", n & 15)
    }

    fn rex_r(&self) -> u8 {
        ((self.rex >> 2) & 1) << 3
    }

    fn rex_x(&self) -> u8 {
        ((self.rex >> 1) & 1) << 3
    }

    fn rex_b(&self) -> u8 {
        (self.rex & 1) << 3
    }

    /// ModRM (+ SIB + displacement): `(reg field with REX.R, raw reg field, r/m operand)`.
    fn modrm(&mut self) -> Option<(u8, u8, Rm)> {
        let m = self.u8()?;
        let md = m >> 6;
        let ext = (m >> 3) & 7;
        let reg = ext | self.rex_r();
        let rm = m & 7;
        if md == 3 {
            return Some((reg, ext, Rm::Reg(rm | self.rex_b())));
        }
        let mut inner = String::new();
        if rm == 4 {
            let sib = self.u8()?;
            let scale = 1u8 << (sib >> 6);
            let index = ((sib >> 3) & 7) | self.rex_x();
            let base = (sib & 7) | self.rex_b();
            let no_base = (sib & 7) == 5 && md == 0;
            if !no_base {
                inner.push_str(R64[usize::from(base)]);
            }
            if index != 4 {
                if !inner.is_empty() {
                    inner.push('+');
                }
                inner.push_str(R64[usize::from(index)]);
                if scale > 1 {
                    inner.push_str(&format!("*{scale}"));
                }
            }
            let disp = match md {
                1 => self.i8()?,
                2 => self.i32()?,
                _ if no_base => self.i32()?,
                _ => 0,
            };
            if md != 0 || no_base {
                if inner.is_empty() {
                    inner.push_str(&imm(disp));
                } else if disp < 0 {
                    inner.push_str(&imm(disp));
                } else if disp > 0 || md != 0 {
                    inner.push('+');
                    inner.push_str(&imm(disp));
                }
            }
        } else if rm == 5 && md == 0 {
            let disp = self.i32()?;
            inner.push_str("rip");
            if disp < 0 {
                inner.push_str(&imm(disp));
            } else {
                inner.push('+');
                inner.push_str(&imm(disp));
            }
        } else {
            inner.push_str(R64[usize::from(rm | self.rex_b())]);
            let disp = match md {
                1 => self.i8()?,
                2 => self.i32()?,
                _ => 0,
            };
            if disp < 0 {
                inner.push_str(&imm(disp));
            } else if md != 0 {
                inner.push('+');
                inner.push_str(&imm(disp));
            }
        }
        let seg = self.seg.map(|s| format!("{s}:")).unwrap_or_default();
        Some((reg, ext, Rm::Mem(format!("{seg}[{inner}]"))))
    }

    fn operand(&self, rm: &Rm, size: u8) -> String {
        match rm {
            Rm::Reg(n) => self.reg(*n, size),
            Rm::Mem(addr) => {
                let ptr = match size {
                    8 => "byte",
                    16 => "word",
                    32 => "dword",
                    64 => "qword",
                    _ => "xmmword",
                };
                format!("{ptr} ptr {addr}")
            }
        }
    }

    fn operand_xmm(&self, rm: &Rm, mem_size: u8) -> String {
        match rm {
            Rm::Reg(n) => Self::xmm(*n),
            Rm::Mem(_) => self.operand(rm, mem_size),
        }
    }

    fn rel(&mut self, size: u8) -> Option<String> {
        let rel = if size == 8 { self.i8()? } else { self.i32()? };
        Some(imm(self.p as i64 + rel))
    }

    fn opcode(&mut self, op: u8) -> Option<String> {
        let osz = self.osz();
        match op {
            0x00..=0x3f if op & 7 < 6 => {
                let name = ALU[usize::from(op >> 3)];
                match op & 7 {
                    0 => {
                        let (r, _, rm) = self.modrm()?;
                        Some(format!("{name} {}, {}", self.operand(&rm, 8), self.reg(r, 8)))
                    }
                    1 => {
                        let (r, _, rm) = self.modrm()?;
                        Some(format!("{name} {}, {}", self.operand(&rm, osz), self.reg(r, osz)))
                    }
                    2 => {
                        let (r, _, rm) = self.modrm()?;
                        Some(format!("{name} {}, {}", self.reg(r, 8), self.operand(&rm, 8)))
                    }
                    3 => {
                        let (r, _, rm) = self.modrm()?;
                        Some(format!("{name} {}, {}", self.reg(r, osz), self.operand(&rm, osz)))
                    }
                    4 => {
                        let v = self.i8()?;
                        Some(format!("{name} al, {}", imm(v)))
                    }
                    _ => {
                        let v = self.imm_z()?;
                        Some(format!("{name} {}, {}", self.reg(0, osz), imm(v)))
                    }
                }
            }
            0x50..=0x57 => Some(format!("push {}", self.reg((op & 7) | self.rex_b(), 64))),
            0x58..=0x5f => Some(format!("pop {}", self.reg((op & 7) | self.rex_b(), 64))),
            0x63 => {
                let (r, _, rm) = self.modrm()?;
                Some(format!("movsxd {}, {}", self.reg(r, osz), self.operand(&rm, 32)))
            }
            0x68 => {
                let v = self.i32()?;
                Some(format!("push {}", imm(v)))
            }
            0x6a => {
                let v = self.i8()?;
                Some(format!("push {}", imm(v)))
            }
            0x69 | 0x6b => {
                let (r, _, rm) = self.modrm()?;
                let v = if op == 0x69 { self.imm_z()? } else { self.i8()? };
                Some(format!("imul {}, {}, {}", self.reg(r, osz), self.operand(&rm, osz), imm(v)))
            }
            0x70..=0x7f => {
                let t = self.rel(8)?;
                Some(format!("j{} {t}", CC[usize::from(op & 15)]))
            }
            0x80 | 0x81 | 0x83 => {
                let (_, ext, rm) = self.modrm()?;
                let size = if op == 0x80 { 8 } else { osz };
                let v = if op == 0x81 { self.imm_z()? } else { self.i8()? };
                Some(format!("{} {}, {}", ALU[usize::from(ext)], self.operand(&rm, size), imm(v)))
            }
            0x84 | 0x85 | 0x86 | 0x87 | 0x88 | 0x89 => {
                let size = if op & 1 == 0 { 8 } else { osz };
                let name = match op {
                    0x84 | 0x85 => "test",
                    0x86 | 0x87 => "xchg",
                    _ => "mov",
                };
                let (r, _, rm) = self.modrm()?;
                Some(format!("{name} {}, {}", self.operand(&rm, size), self.reg(r, size)))
            }
            0x8a | 0x8b => {
                let size = if op == 0x8a { 8 } else { osz };
                let (r, _, rm) = self.modrm()?;
                Some(format!("mov {}, {}", self.reg(r, size), self.operand(&rm, size)))
            }
            0x8d => {
                let (r, _, rm) = self.modrm()?;
                match rm {
                    Rm::Mem(addr) => Some(format!("lea {}, {addr}", self.reg(r, osz))),
                    Rm::Reg(_) => None,
                }
            }
            0x90 => Some(if self.rep == 0xf3 {
                "pause".to_owned()
            } else if self.rex & 1 != 0 {
                format!("xchg {}, {}", self.reg(8, osz), self.reg(0, osz))
            } else {
                "nop".to_owned()
            }),
            0x91..=0x97 => Some(format!("xchg {}, {}", self.reg((op & 7) | self.rex_b(), osz), self.reg(0, osz))),
            0x98 => Some((if self.w() { "cdqe" } else if self.opsz16 { "cbw" } else { "cwde" }).to_owned()),
            0x99 => Some((if self.w() { "cqo" } else if self.opsz16 { "cwd" } else { "cdq" }).to_owned()),
            0xa8 => {
                let v = self.i8()?;
                Some(format!("test al, {}", imm(v)))
            }
            0xa9 => {
                let v = self.imm_z()?;
                Some(format!("test {}, {}", self.reg(0, osz), imm(v)))
            }
            0xb0..=0xb7 => {
                let v = self.i8()?;
                Some(format!("mov {}, {}", self.reg((op & 7) | self.rex_b(), 8), imm(v)))
            }
            0xb8..=0xbf => {
                let r = self.reg((op & 7) | self.rex_b(), osz);
                let v = if self.w() {
                    self.u64()? as i64
                } else if self.opsz16 {
                    self.i16()? & 0xffff
                } else {
                    self.i32()? & 0xffff_ffff
                };
                Some(format!("mov {r}, 0x{v:x}"))
            }
            0xc0 | 0xc1 | 0xd0 | 0xd1 | 0xd2 | 0xd3 => {
                let size = if op & 1 == 0 { 8 } else { osz };
                let (_, ext, rm) = self.modrm()?;
                let count = match op {
                    0xc0 | 0xc1 => imm(self.i8()?),
                    0xd0 | 0xd1 => "1".to_owned(),
                    _ => "cl".to_owned(),
                };
                Some(format!("{} {}, {count}", SHIFT[usize::from(ext)], self.operand(&rm, size)))
            }
            0xc2 => {
                let v = self.i16()?;
                Some(format!("ret {}", imm(v & 0xffff)))
            }
            0xc3 => Some(if self.rep == 0xf3 { "repz ret".to_owned() } else { "ret".to_owned() }),
            0xc6 | 0xc7 => {
                let size = if op == 0xc6 { 8 } else { osz };
                let (_, ext, rm) = self.modrm()?;
                if ext != 0 {
                    return None;
                }
                let v = if op == 0xc6 { self.i8()? } else { self.imm_z()? };
                Some(format!("mov {}, {}", self.operand(&rm, size), imm(v)))
            }
            0xc9 => Some("leave".to_owned()),
            0xcc => Some("int3".to_owned()),
            0xcd => {
                let v = self.i8()?;
                Some(format!("int {}", imm(v & 0xff)))
            }
            0xe8 => {
                let t = self.rel(32)?;
                Some(format!("call {t}"))
            }
            0xe9 => {
                let t = self.rel(32)?;
                Some(format!("jmp {t}"))
            }
            0xeb => {
                let t = self.rel(8)?;
                Some(format!("jmp {t}"))
            }
            0xf4 => Some("hlt".to_owned()),
            0xf6 | 0xf7 => {
                let size = if op == 0xf6 { 8 } else { osz };
                let (_, ext, rm) = self.modrm()?;
                let name = GROUP3[usize::from(ext)];
                if ext < 2 {
                    let v = if op == 0xf6 { self.i8()? } else { self.imm_z()? };
                    Some(format!("{name} {}, {}", self.operand(&rm, size), imm(v)))
                } else {
                    Some(format!("{name} {}", self.operand(&rm, size)))
                }
            }
            0xfe | 0xff => {
                let (_, ext, rm) = self.modrm()?;
                let size = if op == 0xfe { 8 } else { osz };
                match (op, ext) {
                    (_, 0) => Some(format!("inc {}", self.operand(&rm, size))),
                    (_, 1) => Some(format!("dec {}", self.operand(&rm, size))),
                    (0xff, 2) => Some(format!("call {}", self.operand(&rm, 64))),
                    (0xff, 4) => Some(format!("jmp {}", self.operand(&rm, 64))),
                    (0xff, 6) => Some(format!("push {}", self.operand(&rm, 64))),
                    _ => None,
                }
            }
            0x0f => {
                let op2 = self.u8()?;
                self.two_byte(op2)
            }
            _ => None,
        }
    }

    fn two_byte(&mut self, op: u8) -> Option<String> {
        let osz = self.osz();
        match op {
            0x05 => Some("syscall".to_owned()),
            0x0b => Some("ud2".to_owned()),
            0xa2 => Some("cpuid".to_owned()),
            0x1e if self.rep == 0xf3 && self.b.get(self.p) == Some(&0xfa) => {
                self.p += 1;
                Some("endbr64".to_owned())
            }
            0x1f => {
                let (_, _, rm) = self.modrm()?;
                Some(format!("nop {}", self.operand(&rm, osz)))
            }
            0x40..=0x4f => {
                let (r, _, rm) = self.modrm()?;
                Some(format!("cmov{} {}, {}", CC[usize::from(op & 15)], self.reg(r, osz), self.operand(&rm, osz)))
            }
            0x80..=0x8f => {
                let t = self.rel(32)?;
                Some(format!("j{} {t}", CC[usize::from(op & 15)]))
            }
            0x90..=0x9f => {
                let (_, _, rm) = self.modrm()?;
                Some(format!("set{} {}", CC[usize::from(op & 15)], self.operand(&rm, 8)))
            }
            0xaf => {
                let (r, _, rm) = self.modrm()?;
                Some(format!("imul {}, {}", self.reg(r, osz), self.operand(&rm, osz)))
            }
            0xb6 | 0xb7 | 0xbe | 0xbf => {
                let from = if op & 1 == 0 { 8 } else { 16 };
                let name = if op < 0xb8 { "movzx" } else { "movsx" };
                let (r, _, rm) = self.modrm()?;
                Some(format!("{name} {}, {}", self.reg(r, osz), self.operand(&rm, from)))
            }
            0x10 | 0x11 | 0x28 | 0x29 | 0x57 | 0x58 | 0x59 | 0x5c | 0x5e | 0xef | 0x6f | 0x7f => self.sse(op),
            _ => None,
        }
    }

    fn sse(&mut self, op: u8) -> Option<String> {
        let (r, _, rm) = self.modrm()?;
        let (suffix, mem) = match (self.rep, self.opsz16) {
            (0xf3, _) => ("ss", 32),
            (0xf2, _) => ("sd", 64),
            (_, true) => ("pd", 128),
            _ => ("ps", 128),
        };
        let name = match op {
            0x10 | 0x11 => format!("mov{}", if suffix == "ps" { "ups" } else if suffix == "pd" { "upd" } else { suffix }),
            0x28 | 0x29 => format!("mova{suffix}"),
            0x57 => format!("xor{suffix}"),
            0x58 => format!("add{suffix}"),
            0x59 => format!("mul{suffix}"),
            0x5c => format!("sub{suffix}"),
            0x5e => format!("div{suffix}"),
            0xef => "pxor".to_owned(),
            _ => (if self.rep == 0xf3 { "movdqu" } else { "movdqa" }).to_owned(),
        };
        let size = if matches!(op, 0x28 | 0x29 | 0x57 | 0xef | 0x6f | 0x7f) { 128 } else { mem };
        let (a, b) = (Self::xmm(r), self.operand_xmm(&rm, size));
        Some(if matches!(op, 0x11 | 0x29 | 0x7f) { format!("{name} {b}, {a}") } else { format!("{name} {a}, {b}") })
    }
}

/// Decodes the instruction at `start`: `(position after it, text)`.
fn decode(b: &[u8], start: usize) -> Option<(usize, String)> {
    let mut d = Dec { b, p: start, rex: 0, opsz16: false, rep: 0, seg: None };
    loop {
        match *d.b.get(d.p)? {
            0x66 => d.opsz16 = true,
            0xf2 | 0xf3 => d.rep = d.b[d.p],
            0x64 => d.seg = Some("fs"),
            0x65 => d.seg = Some("gs"),
            0x2e | 0x3e | 0x26 | 0x36 | 0xf0 => {}
            _ => break,
        }
        d.p += 1;
        if d.p - start > 14 {
            return None;
        }
    }
    if (0x40..=0x4f).contains(d.b.get(d.p)?) {
        d.rex = d.b[d.p];
        d.p += 1;
    }
    let op = d.u8()?;
    let text = d.opcode(op)?;
    Some((d.p, text))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one(bytes: &[u8]) -> String {
        let lines = disassemble(64, bytes);
        assert_eq!(lines.len(), 1, "{lines:?}");
        lines[0].text.clone()
    }

    #[test]
    fn only_x86_64_is_supported() {
        assert_eq!(bitness_for("X86_64"), Some(64));
        assert_eq!(bitness_for("AMD64"), Some(64));
        assert_eq!(bitness_for("I386"), None);
        assert_eq!(bitness_for("ARM64"), None);
        assert!(disassemble(32, &[0x90]).is_empty());
    }

    #[test]
    fn common_instructions() {
        assert_eq!(one(&[0x55]), "push rbp");
        assert_eq!(one(&[0x48, 0x89, 0xe5]), "mov rbp, rsp");
        assert_eq!(one(&[0x5d]), "pop rbp");
        assert_eq!(one(&[0xc3]), "ret");
        assert_eq!(one(&[0x31, 0xc0]), "xor eax, eax");
        assert_eq!(one(&[0x48, 0x83, 0xec, 0x20]), "sub rsp, 0x20");
        assert_eq!(one(&[0xf3, 0x0f, 0x1e, 0xfa]), "endbr64");
        assert_eq!(one(&[0x8b, 0x45, 0xfc]), "mov eax, dword ptr [rbp-0x4]");
        assert_eq!(one(&[0x48, 0x8d, 0x05, 0x10, 0x00, 0x00, 0x00]), "lea rax, [rip+0x10]");
        assert_eq!(one(&[0xe8, 0x00, 0x00, 0x00, 0x00]), "call 0x5");
        assert_eq!(one(&[0x74, 0x02]), "je 0x4");
        assert_eq!(one(&[0x0f, 0xb6, 0xc0]), "movzx eax, al");
    }

    #[test]
    fn offsets_are_relative_and_unknown_bytes_resync() {
        let lines = disassemble(64, &[0x55, 0x48, 0x89, 0xe5, 0x06, 0x5d, 0xc3]);
        let offsets: Vec<u64> = lines.iter().map(|l| l.offset).collect();
        assert_eq!(offsets, [0, 1, 4, 5, 6]);
        assert_eq!(lines[2].text, "db 0x06");
        assert_eq!(lines[4].text, "ret");
    }

    #[test]
    fn truncated_and_random_input_never_panics() {
        for len in 0..16 {
            let _ = disassemble(64, &vec![0x48u8; len]);
            let _ = disassemble(64, &vec![0x0fu8; len]);
        }
        let noise: Vec<u8> = (0..4096u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8).collect();
        let lines = disassemble(64, &noise);
        assert!(!lines.is_empty());
    }
}
