use serde::{Deserialize, Serialize};

/// Facts about the encoded source that are not recoverable from pixels.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImageMeta {
    /// Human readable container format, e.g. "JPEG", "WebP".
    pub format: Option<String>,
    /// Bits per pixel of the *source* (24 for RGB JPEG, 32 for RGBA WebP).
    pub bits_per_pixel: u16,
}

/// Owned, straight (non-premultiplied) RGBA8 image, row-major, no padding.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct RgbaImage {
    width: u32,
    height: u32,
    #[serde(with = "serde_bytes")]
    data: Vec<u8>,
}

impl RgbaImage {
    pub const BYTES_PER_PIXEL: usize = 4;

    /// Byte length of a `width x height` RGBA buffer, `None` on overflow.
    pub fn byte_len(width: u32, height: u32) -> Option<usize> {
        (width as usize)
            .checked_mul(height as usize)?
            .checked_mul(Self::BYTES_PER_PIXEL)
    }

    /// Zero-filled (transparent black) image.
    pub fn new(width: u32, height: u32) -> Option<Self> {
        let len = Self::byte_len(width, height)?;
        Some(Self { width, height, data: vec![0; len] })
    }

    pub fn from_raw(width: u32, height: u32, data: Vec<u8>) -> Option<Self> {
        (Self::byte_len(width, height)? == data.len()).then_some(Self { width, height, data })
    }

    #[inline]
    pub fn width(&self) -> u32 {
        self.width
    }
    #[inline]
    pub fn height(&self) -> u32 {
        self.height
    }
    #[inline]
    pub fn dimensions(&self) -> (u32, u32) {
        (self.width, self.height)
    }
    pub fn as_bytes(&self) -> &[u8] {
        &self.data
    }
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        &mut self.data
    }
    pub fn into_bytes(self) -> Vec<u8> {
        self.data
    }

    /// Pixel at in-range coordinates. Panics when out of range; use [`Self::get`]
    /// for untrusted coordinates.
    #[inline]
    pub fn pixel(&self, x: u32, y: u32) -> [u8; 4] {
        let i = (y as usize * self.width as usize + x as usize) * 4;
        [self.data[i], self.data[i + 1], self.data[i + 2], self.data[i + 3]]
    }

    #[inline]
    pub fn get(&self, x: i64, y: i64) -> Option<[u8; 4]> {
        if x < 0 || y < 0 || x >= self.width as i64 || y >= self.height as i64 {
            None
        } else {
            Some(self.pixel(x as u32, y as u32))
        }
    }

    #[inline]
    pub fn row(&self, y: u32) -> &[u8] {
        let stride = self.width as usize * 4;
        let start = y as usize * stride;
        &self.data[start..start + stride]
    }
}
