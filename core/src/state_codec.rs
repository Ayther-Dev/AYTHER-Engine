//! Little-endian writer and bounds-checked reader for the serialized visual
//! state sections (spec 002, contracts.md C4).
//!
//! Every payload starts with an 8-byte magic and a `u16` version. A reader
//! accepts a payload only when the magic and version match, every read stays in
//! bounds and the payload is consumed exactly, so a section is either fully
//! valid or rejected before anything is applied.

/// Longest string a section may carry (asset paths).
pub(crate) const STATE_STRING_LIMIT: usize = 4096;

pub(crate) struct StateWriter {
    buf: Vec<u8>,
}

impl StateWriter {
    pub(crate) fn new(magic: &[u8; 8], version: u16) -> Self {
        let mut buf = Vec::with_capacity(256);
        buf.extend_from_slice(magic);
        buf.extend_from_slice(&version.to_le_bytes());
        Self { buf }
    }

    pub(crate) fn u8(&mut self, value: u8) {
        self.buf.push(value);
    }

    pub(crate) fn bool(&mut self, value: bool) {
        self.buf.push(u8::from(value));
    }

    pub(crate) fn u16(&mut self, value: u16) {
        self.buf.extend_from_slice(&value.to_le_bytes());
    }

    pub(crate) fn u32(&mut self, value: u32) {
        self.buf.extend_from_slice(&value.to_le_bytes());
    }

    pub(crate) fn u64(&mut self, value: u64) {
        self.buf.extend_from_slice(&value.to_le_bytes());
    }

    pub(crate) fn i64(&mut self, value: i64) {
        self.buf.extend_from_slice(&value.to_le_bytes());
    }

    /// Writes a length-prefixed count. Callers bound their collections, so a
    /// count above `u32::MAX` cannot occur.
    pub(crate) fn count(&mut self, value: usize) {
        self.u32(u32::try_from(value).unwrap_or(u32::MAX));
    }

    pub(crate) fn str(&mut self, value: &str) {
        self.count(value.len());
        self.buf.extend_from_slice(value.as_bytes());
    }

    pub(crate) fn finish(self) -> Vec<u8> {
        self.buf
    }
}

pub(crate) struct StateReader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> StateReader<'a> {
    /// Opens `data` when it starts with `magic` and `version`.
    pub(crate) fn open(data: &'a [u8], magic: &[u8; 8], version: u16) -> Option<Self> {
        let mut reader = Self { data, pos: 0 };
        if reader.bytes(8)? != magic || reader.u16()? != version {
            return None;
        }
        Some(reader)
    }

    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.pos.checked_add(n)?;
        let slice = self.data.get(self.pos..end)?;
        self.pos = end;
        Some(slice)
    }

    fn array<const N: usize>(&mut self) -> Option<[u8; N]> {
        self.bytes(N)?.try_into().ok()
    }

    pub(crate) fn u8(&mut self) -> Option<u8> {
        Some(self.array::<1>()?[0])
    }

    pub(crate) fn bool(&mut self) -> Option<bool> {
        match self.u8()? {
            0 => Some(false),
            1 => Some(true),
            _ => None,
        }
    }

    pub(crate) fn u16(&mut self) -> Option<u16> {
        Some(u16::from_le_bytes(self.array()?))
    }

    pub(crate) fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.array()?))
    }

    pub(crate) fn u64(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.array()?))
    }

    pub(crate) fn i64(&mut self) -> Option<i64> {
        Some(i64::from_le_bytes(self.array()?))
    }

    /// Reads a count and rejects it above `limit`.
    pub(crate) fn count(&mut self, limit: usize) -> Option<usize> {
        let n = usize::try_from(self.u32()?).ok()?;
        (n <= limit).then_some(n)
    }

    pub(crate) fn str(&mut self) -> Option<String> {
        let n = self.count(STATE_STRING_LIMIT)?;
        String::from_utf8(self.bytes(n)?.to_vec()).ok()
    }

    /// Succeeds only when the whole payload was consumed.
    pub(crate) fn finish(self) -> Option<()> {
        (self.pos == self.data.len()).then_some(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_codec_round_trips_and_rejects_tampering() {
        let mut w = StateWriter::new(b"AYTESTS\0", 3);
        w.u8(7);
        w.bool(true);
        w.u16(0xBEEF);
        w.u32(0xDEAD_BEEF);
        w.u64(u64::MAX - 1);
        w.i64(-42);
        w.str("graphics/a.png");
        let bytes = w.finish();

        let mut r = StateReader::open(&bytes, b"AYTESTS\0", 3).expect("header");
        assert_eq!(r.u8(), Some(7));
        assert_eq!(r.bool(), Some(true));
        assert_eq!(r.u16(), Some(0xBEEF));
        assert_eq!(r.u32(), Some(0xDEAD_BEEF));
        assert_eq!(r.u64(), Some(u64::MAX - 1));
        assert_eq!(r.i64(), Some(-42));
        assert_eq!(r.str().as_deref(), Some("graphics/a.png"));
        assert!(r.finish().is_some());

        assert!(
            StateReader::open(&bytes, b"AYTESTS\0", 4).is_none(),
            "version"
        );
        assert!(
            StateReader::open(&bytes, b"AYOTHER\0", 3).is_none(),
            "magic"
        );
        let mut short = StateReader::open(&bytes[..12], b"AYTESTS\0", 3).expect("header");
        assert_eq!(short.u8(), Some(7));
        assert_eq!(short.bool(), Some(true));
        assert_eq!(short.u16(), None, "truncated payload");
        let mut extra = bytes.clone();
        extra.push(0);
        let mut r = StateReader::open(&extra, b"AYTESTS\0", 3).expect("header");
        r.u8();
        r.bool();
        r.u16();
        r.u32();
        r.u64();
        r.i64();
        r.str();
        assert!(r.finish().is_none(), "trailing bytes");
    }
}
