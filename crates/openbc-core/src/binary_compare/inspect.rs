//! Byte-level and instruction-level views of one modified element (function or section).
//!
//! This module is pure: it receives the raw bytes of both versions (read by the engine from the
//! files on disk, never from a parsed model) plus, optionally, their disassembly, and builds the
//! rows the viewer paints.
//!
//! * **Hex diff**: 16 bytes per row, aligned by offset inside the element. A byte is marked when
//!   it differs from the other side (or exists on one side only).
//! * **Disassembly diff**: instruction texts aligned with a Myers diff, so one inserted instruction
//!   shifts the rest instead of turning every following line into a change. Operands that are
//!   relative to the function (jump targets) are shown as formatted by the disassembler.
//!
//! Nothing here modifies or patches a binary.

use std::fmt::Write as _;

use similar::{capture_diff_slices, Algorithm, DiffOp};

use super::ChangeState;

/// Bytes shown per hex row.
pub const BYTES_PER_ROW: usize = 16;
/// Most bytes read per side; larger elements are truncated and say so in the notes.
pub const MAX_INSPECT_BYTES: usize = 1024 * 1024;
/// Most instructions disassembled per side.
pub const MAX_INSTRUCTIONS: usize = 20_000;

/// Column (in characters) at which byte `index` of a hex row starts in [`HexRow::left`] /
/// [`HexRow::right`]: `"%08x  "` offset, then two characters per byte followed by a space, with an
/// extra space in the middle of the row. The viewer uses this to colour single bytes.
#[must_use]
pub const fn hex_byte_column(index: usize) -> usize {
    10 + index * 3 + if index >= 8 { 1 } else { 0 }
}

/// One disassembled instruction.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AsmLine {
    /// Offset of the instruction from the start of the element.
    pub offset: u64,
    /// Instruction text without the offset (the diff compares only this).
    pub text: String,
}

/// One row of the side-by-side hex view.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HexRow {
    /// `Unchanged`, `Modified` (some byte differs), `Removed` (left only) or `Added` (right only).
    pub state: ChangeState,
    /// Bit `i` set: byte `i` of this row differs between the two sides.
    pub mask: u16,
    /// Formatted left row (empty when the left side has no bytes here).
    pub left: String,
    /// Formatted right row (empty when the right side has no bytes here).
    pub right: String,
}

/// One row of the side-by-side disassembly view.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AsmRow {
    /// `Unchanged`, `Modified` (both sides, different text), `Removed` or `Added`.
    pub state: ChangeState,
    /// Left instruction, formatted `+offset  text` (empty when absent).
    pub left: String,
    /// Right instruction, formatted `+offset  text` (empty when absent).
    pub right: String,
}

/// Everything the viewer shows for one inspected element.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InspectOutput {
    /// Element name for the header of the pane.
    pub title: String,
    /// Caveats: truncation, missing side, bytes on disk not matching the analysed hash, no
    /// disassembler for this architecture, ...
    pub notes: Vec<String>,
    /// Side-by-side hex rows.
    pub hex: Vec<HexRow>,
    /// Number of bytes that differ (including bytes present on one side only).
    pub differing_bytes: usize,
    /// Left byte count shown.
    pub left_len: usize,
    /// Right byte count shown.
    pub right_len: usize,
    /// Side-by-side disassembly; `None` when no disassembly is available.
    pub asm: Option<Vec<AsmRow>>,
}

/// `00000010  48 89 e5 ...  |H..|` with fixed-width padding so both sides line up.
fn format_hex_row(offset: usize, bytes: &[u8]) -> String {
    let mut s = String::with_capacity(80);
    let _ = write!(s, "{offset:08x}  ");
    for i in 0..BYTES_PER_ROW {
        if i == 8 {
            s.push(' ');
        }
        match bytes.get(i) {
            Some(b) => {
                let _ = write!(s, "{b:02x} ");
            }
            None => s.push_str("   "),
        }
    }
    s.push_str(" |");
    for b in bytes {
        s.push(if (0x20..0x7f).contains(b) { char::from(*b) } else { '.' });
    }
    s.push('|');
    s
}

/// Side-by-side hex rows for two byte strings (`None` = the element does not exist on that side).
/// Returns the rows and the number of differing bytes.
#[must_use]
pub fn hex_diff(left: Option<&[u8]>, right: Option<&[u8]>) -> (Vec<HexRow>, usize) {
    let l = left.unwrap_or(&[]);
    let r = right.unwrap_or(&[]);
    let longest = l.len().max(r.len());
    let mut rows = Vec::with_capacity((longest + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
    let mut differing = 0usize;
    let mut offset = 0usize;
    while offset < longest {
        let lrow = &l[offset.min(l.len())..(offset + BYTES_PER_ROW).min(l.len())];
        let rrow = &r[offset.min(r.len())..(offset + BYTES_PER_ROW).min(r.len())];
        let mut mask = 0u16;
        for i in 0..BYTES_PER_ROW {
            if lrow.get(i) != rrow.get(i) {
                mask |= 1 << i;
                differing += 1;
            }
        }
        let state = if lrow.is_empty() {
            ChangeState::Added
        } else if rrow.is_empty() {
            ChangeState::Removed
        } else if mask == 0 {
            ChangeState::Unchanged
        } else {
            ChangeState::Modified
        };
        rows.push(HexRow {
            state,
            mask,
            left: if lrow.is_empty() { String::new() } else { format_hex_row(offset, lrow) },
            right: if rrow.is_empty() { String::new() } else { format_hex_row(offset, rrow) },
        });
        offset += BYTES_PER_ROW;
    }
    (rows, differing)
}

fn show(line: &AsmLine) -> String {
    format!("+{:04x}  {}", line.offset, line.text)
}

/// Aligned instruction rows. Alignment uses the instruction text only, so a relocated or shifted
/// function whose instructions are the same produces no change rows.
#[must_use]
pub fn asm_diff(left: &[AsmLine], right: &[AsmLine]) -> Vec<AsmRow> {
    let lt: Vec<&str> = left.iter().map(|l| l.text.as_str()).collect();
    let rt: Vec<&str> = right.iter().map(|l| l.text.as_str()).collect();
    let mut rows = Vec::with_capacity(left.len().max(right.len()));
    for op in capture_diff_slices(Algorithm::Myers, &lt, &rt) {
        match op {
            DiffOp::Equal { old_index, new_index, len } => {
                for k in 0..len {
                    rows.push(AsmRow {
                        state: ChangeState::Unchanged,
                        left: show(&left[old_index + k]),
                        right: show(&right[new_index + k]),
                    });
                }
            }
            DiffOp::Delete { old_index, old_len, .. } => {
                for k in 0..old_len {
                    rows.push(AsmRow { state: ChangeState::Removed, left: show(&left[old_index + k]), right: String::new() });
                }
            }
            DiffOp::Insert { new_index, new_len, .. } => {
                for k in 0..new_len {
                    rows.push(AsmRow { state: ChangeState::Added, left: String::new(), right: show(&right[new_index + k]) });
                }
            }
            DiffOp::Replace { old_index, old_len, new_index, new_len } => {
                for k in 0..old_len.max(new_len) {
                    let l = (k < old_len).then(|| show(&left[old_index + k]));
                    let r = (k < new_len).then(|| show(&right[new_index + k]));
                    let state = match (&l, &r) {
                        (Some(_), Some(_)) => ChangeState::Modified,
                        (Some(_), None) => ChangeState::Removed,
                        _ => ChangeState::Added,
                    };
                    rows.push(AsmRow { state, left: l.unwrap_or_default(), right: r.unwrap_or_default() });
                }
            }
        }
    }
    rows
}

/// Assemble the full output from the bytes of both sides and their optional disassembly.
#[must_use]
pub fn build_output(
    title: &str,
    left: Option<&[u8]>,
    right: Option<&[u8]>,
    left_asm: Option<&[AsmLine]>,
    right_asm: Option<&[AsmLine]>,
    notes: Vec<String>,
) -> InspectOutput {
    let (hex, differing_bytes) = hex_diff(left, right);
    let asm = match (left_asm, right_asm) {
        (None, None) => None,
        (l, r) => Some(asm_diff(l.unwrap_or(&[]), r.unwrap_or(&[]))),
    };
    InspectOutput {
        title: title.to_owned(),
        notes,
        hex,
        differing_bytes,
        left_len: left.map_or(0, <[u8]>::len),
        right_len: right.map_or(0, <[u8]>::len),
        asm,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(offset: u64, text: &str) -> AsmLine {
        AsmLine { offset, text: text.into() }
    }

    #[test]
    fn identical_bytes_have_no_differences() {
        let data: Vec<u8> = (0..40u8).collect();
        let (rows, diff) = hex_diff(Some(data.as_slice()), Some(data.as_slice()));
        assert_eq!(diff, 0);
        assert_eq!(rows.len(), 3);
        assert!(rows.iter().all(|r| r.state == ChangeState::Unchanged && r.mask == 0));
        assert!(rows[0].left.starts_with("00000000  00 01 02"));
    }

    #[test]
    fn a_changed_byte_sets_exactly_its_mask_bit() {
        let a = vec![0u8; 32];
        let mut b = a.clone();
        b[17] = 0xff;
        let (rows, diff) = hex_diff(Some(a.as_slice()), Some(b.as_slice()));
        assert_eq!(diff, 1);
        assert_eq!(rows[0].state, ChangeState::Unchanged);
        assert_eq!(rows[1].state, ChangeState::Modified);
        assert_eq!(rows[1].mask, 1 << 1);
    }

    #[test]
    fn length_differences_and_missing_sides() {
        let a = vec![1u8; 16];
        let b = vec![1u8; 20];
        let (rows, diff) = hex_diff(Some(a.as_slice()), Some(b.as_slice()));
        assert_eq!(diff, 4);
        assert_eq!(rows[1].state, ChangeState::Added);
        let (only_right, _) = hex_diff(None, Some(b.as_slice()));
        assert!(only_right.iter().all(|r| r.state == ChangeState::Added && r.left.is_empty()));
    }

    #[test]
    fn hex_columns_match_the_formatted_text() {
        let bytes = [0xab_u8; 16];
        let row = format_hex_row(0, &bytes);
        for i in 0..16 {
            let c = hex_byte_column(i);
            assert_eq!(&row[c..c + 2], "ab", "byte {i}");
        }
    }

    #[test]
    fn inserted_instruction_does_not_turn_the_rest_into_changes() {
        let a = [line(0, "push rbp"), line(1, "mov rbp, rsp"), line(4, "ret")];
        let b = [line(0, "push rbp"), line(1, "nop"), line(2, "mov rbp, rsp"), line(5, "ret")];
        let rows = asm_diff(&a, &b);
        let states: Vec<_> = rows.iter().map(|r| r.state).collect();
        assert_eq!(
            states,
            [ChangeState::Unchanged, ChangeState::Added, ChangeState::Unchanged, ChangeState::Unchanged]
        );
    }

    #[test]
    fn replaced_instructions_pair_up() {
        let a = [line(0, "add eax, 1"), line(3, "ret")];
        let b = [line(0, "add eax, 2"), line(3, "ret")];
        let rows = asm_diff(&a, &b);
        assert_eq!(rows[0].state, ChangeState::Modified);
        assert_eq!(rows[1].state, ChangeState::Unchanged);
    }

    #[test]
    fn build_output_without_disassembly_has_no_asm() {
        let out = build_output("f", Some(&[1u8, 2][..]), Some(&[1u8, 3][..]), None, None, vec![]);
        assert!(out.asm.is_none());
        assert_eq!(out.differing_bytes, 1);
        assert_eq!((out.left_len, out.right_len), (2, 2));
    }
}
