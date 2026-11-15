//! Basic x86 / x86-64 disassembly for the Binary Compare inspector (feature `disasm`).
//!
//! Instruction text is produced with iced-x86's Intel formatter and positioned *relative to the
//! start of the function*, so a function that merely moved to another address disassembles to the
//! same text in both versions. Other architectures (ARM, ...) are reported as unsupported and the
//! inspector shows the hex view only.

use openbc_core::binary_compare::inspect::MAX_INSTRUCTIONS;
use openbc_core::binary_compare::AsmLine;

/// Instruction-set width for an `architecture` metadata value (`X86_64`, `AMD64`, `I386`, `x86_64`,
/// ...), or `None` when no disassembler is available for it.
#[must_use]
pub fn bitness_for(architecture: &str) -> Option<u32> {
    if cfg!(not(feature = "disasm")) {
        return None;
    }
    let a = architecture.to_ascii_lowercase();
    if a.contains("x86_64") || a.contains("x86-64") || a.contains("amd64") || a.contains("x64") {
        Some(64)
    } else if a.contains("i386") || a.contains("i686") || a.contains("x86") || a == "i486" || a == "i586" {
        Some(32)
    } else {
        None
    }
}

/// Disassemble `bytes` (at most [`MAX_INSTRUCTIONS`] instructions).
#[cfg(feature = "disasm")]
#[must_use]
pub fn disassemble(bitness: u32, bytes: &[u8]) -> Vec<AsmLine> {
    use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter};

    let mut decoder = Decoder::with_ip(bitness, bytes, 0, DecoderOptions::NONE);
    let mut formatter = IntelFormatter::new();
    let mut instruction = Instruction::default();
    let mut out = Vec::new();
    while decoder.can_decode() && out.len() < MAX_INSTRUCTIONS {
        decoder.decode_out(&mut instruction);
        let mut text = String::new();
        formatter.format(&instruction, &mut text);
        out.push(AsmLine { offset: instruction.ip(), text });
    }
    out
}

/// Without the `disasm` feature nothing is disassembled.
#[cfg(not(feature = "disasm"))]
#[must_use]
pub fn disassemble(_bitness: u32, _bytes: &[u8]) -> Vec<AsmLine> {
    let _ = MAX_INSTRUCTIONS;
    Vec::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn architecture_names_map_to_bitness() {
        if cfg!(feature = "disasm") {
            assert_eq!(bitness_for("X86_64"), Some(64));
            assert_eq!(bitness_for("AMD64"), Some(64));
            assert_eq!(bitness_for("x86_64"), Some(64));
            assert_eq!(bitness_for("I386"), Some(32));
        }
        assert_eq!(bitness_for("ARM64"), None);
        assert_eq!(bitness_for("AARCH64"), None);
    }

    #[cfg(feature = "disasm")]
    #[test]
    fn decodes_a_tiny_function_with_relative_offsets() {
        // push rbp; mov rbp, rsp; pop rbp; ret
        let lines = disassemble(64, &[0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3]);
        let texts: Vec<&str> = lines.iter().map(|l| l.text.as_str()).collect();
        assert_eq!(texts.len(), 4);
        assert_eq!(texts[0], "push rbp");
        assert!(texts[1].starts_with("mov rbp"), "{texts:?}");
        assert_eq!(texts[3], "ret");
        assert_eq!(lines.iter().map(|l| l.offset).collect::<Vec<_>>(), [0, 1, 4, 5]);
    }
}
