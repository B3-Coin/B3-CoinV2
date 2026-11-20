//! The byte codec of FlowMesh V2 objects (src/flowmesh2/core/codec.h):
//! Bitcoin serialization, little-endian integers, canonical CompactSize
//! counts that are compared with their cap before anything is read, and
//! exact decoding (trailing bytes are an error).

use super::Error;

/// A 32-byte hash or identifier, in the order the protocol hashes it.
pub type H256 = [u8; 32];

const HEX: &[u8; 16] = b"0123456789abcdef";

/// Plain hex, first byte first: how the API writes encoded structures.
pub fn hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

pub fn unhex(text: &str) -> Option<Vec<u8>> {
    let text = text.as_bytes();
    if text.len() % 2 != 0 {
        return None;
    }
    let nibble = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    };
    text.chunks(2)
        .map(|pair| Some((nibble(pair[0])? << 4) | nibble(pair[1])?))
        .collect()
}

/// Hex of a hash or identifier as the node prints it (`uint256::GetHex`):
/// the 32 bytes in reverse order. Every id in the API's JSON, in the genesis
/// document and in the `/v2/state/<account>` path has this form.
pub fn id_hex(hash: &H256) -> String {
    let mut reversed = *hash;
    reversed.reverse();
    hex(&reversed)
}

/// The inverse of `id_hex`: exactly 64 hex digits.
pub fn id_from_hex(text: &str) -> Option<H256> {
    if text.len() != 64 {
        return None;
    }
    let mut bytes: H256 = unhex(text)?.try_into().ok()?;
    bytes.reverse();
    Some(bytes)
}

/// Reads one object; `finish` refuses trailing bytes.
pub struct Reader<'a> {
    bytes: &'a [u8],
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes }
    }

    pub fn remaining(&self) -> usize {
        self.bytes.len()
    }

    pub fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        if self.bytes.len() < n {
            return Err(Error::Decode("truncated"));
        }
        let (head, tail) = self.bytes.split_at(n);
        self.bytes = tail;
        Ok(head)
    }

    pub fn array<const N: usize>(&mut self) -> Result<[u8; N], Error> {
        Ok(self.take(N)?.try_into().expect("length checked"))
    }

    pub fn h256(&mut self) -> Result<H256, Error> {
        self.array()
    }

    pub fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }

    pub fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.array()?))
    }

    pub fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.array()?))
    }

    pub fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.array()?))
    }

    pub fn i64(&mut self) -> Result<i64, Error> {
        Ok(i64::from_le_bytes(self.array()?))
    }

    /// A CompactSize, refused when it is not in its shortest form.
    pub fn compact(&mut self) -> Result<u64, Error> {
        let first = self.u8()?;
        let (value, least) = match first {
            0..=252 => (u64::from(first), 0),
            253 => (u64::from(self.u16()?), 253),
            254 => (u64::from(self.u32()?), 0x1_0000),
            255 => (self.u64()?, 0x1_0000_0000),
        };
        if value < least {
            return Err(Error::Decode("non-canonical CompactSize"));
        }
        Ok(value)
    }

    /// A count that may not exceed `cap`; checked before anything is read.
    pub fn count(&mut self, cap: usize) -> Result<usize, Error> {
        let count = self.compact()?;
        if count > cap as u64 {
            return Err(Error::Oversize("count exceeds its cap"));
        }
        Ok(count as usize)
    }

    pub fn finish(self) -> Result<(), Error> {
        if self.bytes.is_empty() {
            Ok(())
        } else {
            Err(Error::Decode("trailing bytes"))
        }
    }
}

/// Appends a CompactSize.
pub fn put_compact(out: &mut Vec<u8>, value: u64) {
    match value {
        0..=252 => out.push(value as u8),
        253..=0xffff => {
            out.push(253);
            out.extend_from_slice(&(value as u16).to_le_bytes());
        }
        0x1_0000..=0xffff_ffff => {
            out.push(254);
            out.extend_from_slice(&(value as u32).to_le_bytes());
        }
        _ => {
            out.push(255);
            out.extend_from_slice(&value.to_le_bytes());
        }
    }
}

/// The length in bytes of a CompactSize holding `value`.
pub fn compact_len(value: u64) -> usize {
    match value {
        0..=252 => 1,
        253..=0xffff => 3,
        0x1_0000..=0xffff_ffff => 5,
        _ => 9,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trips_and_refuses_bad_text() {
        assert_eq!(hex(&[0x00, 0xab, 0xff]), "00abff");
        assert_eq!(unhex("00ABff"), Some(vec![0x00, 0xab, 0xff]));
        assert_eq!(unhex("0"), None);
        assert_eq!(unhex("zz"), None);
    }

    #[test]
    fn ids_print_reversed_like_the_node() {
        let mut id = [0u8; 32];
        id[0] = 0x01;
        id[31] = 0xff;
        let text = id_hex(&id);
        assert!(text.starts_with("ff") && text.ends_with("01"));
        assert_eq!(id_from_hex(&text), Some(id));
        assert_eq!(id_from_hex("ff"), None);
    }

    #[test]
    fn compact_sizes_are_canonical_only() {
        for value in [0u64, 252, 253, 0xffff, 0x1_0000, 0xffff_ffff, 0x1_0000_0000] {
            let mut bytes = Vec::new();
            put_compact(&mut bytes, value);
            assert_eq!(bytes.len(), compact_len(value));
            let mut reader = Reader::new(&bytes);
            assert_eq!(reader.compact(), Ok(value));
            assert!(reader.finish().is_ok());
        }
        // 252 written in the three-byte form.
        assert_eq!(
            Reader::new(&[253, 252, 0]).compact(),
            Err(Error::Decode("non-canonical CompactSize"))
        );
        assert_eq!(
            Reader::new(&[254, 0xff, 0xff, 0, 0]).compact(),
            Err(Error::Decode("non-canonical CompactSize"))
        );
    }

    #[test]
    fn a_count_above_its_cap_is_refused_before_reading() {
        assert_eq!(
            Reader::new(&[5]).count(4),
            Err(Error::Oversize("count exceeds its cap"))
        );
        assert_eq!(Reader::new(&[4]).count(4), Ok(4));
    }

    #[test]
    fn trailing_and_missing_bytes_are_errors() {
        let mut reader = Reader::new(&[1, 2, 3]);
        assert_eq!(reader.u16(), Ok(0x0201));
        assert_eq!(reader.finish(), Err(Error::Decode("trailing bytes")));
        assert_eq!(Reader::new(&[1]).u32(), Err(Error::Decode("truncated")));
    }
}
