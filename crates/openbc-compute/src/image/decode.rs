use std::io::Cursor;
use std::sync::Arc;

use image::{ImageFormat, ImageReader, Limits};
use openbc_core::image_compare::{ImageMeta, RgbaImage};

use super::error::ImageComputeError;

/// Hard cap on a single decoded dimension.
pub const MAX_DIMENSION: u32 = 32_768;
/// Decoder allocation budget (bytes). Bounds memory for hostile files.
pub const MAX_DECODE_ALLOC: u64 = 1 << 30;

#[derive(Clone, Debug)]
pub struct DecodedImage {
    pub image: Arc<RgbaImage>,
    pub meta: ImageMeta,
}

fn format_name(f: ImageFormat) -> String {
    match f {
        ImageFormat::Png => "PNG".into(),
        ImageFormat::Jpeg => "JPEG".into(),
        ImageFormat::Gif => "GIF".into(),
        ImageFormat::WebP => "WebP".into(),
        ImageFormat::Tiff => "TIFF".into(),
        ImageFormat::Bmp => "BMP".into(),
        other => format!("{other:?}"),
    }
}

/// CPU-bound: call from a Rayon worker, never from a Tokio task.
pub fn decode_image(bytes: &[u8]) -> Result<DecodedImage, ImageComputeError> {
    let mut reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .map_err(ImageComputeError::decode)?;
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_DIMENSION);
    limits.max_image_height = Some(MAX_DIMENSION);
    limits.max_alloc = Some(MAX_DECODE_ALLOC);
    reader.limits(limits);

    let format = reader.format().map(format_name);
    let dynamic = reader.decode().map_err(ImageComputeError::decode)?;
    let bits_per_pixel = dynamic.color().bits_per_pixel();
    let rgba = dynamic.into_rgba8();
    let (width, height) = rgba.dimensions();
    let image = RgbaImage::from_raw(width, height, rgba.into_raw())
        .ok_or_else(|| ImageComputeError::too_large(width, height))?;
    Ok(DecodedImage { image: Arc::new(image), meta: ImageMeta { format, bits_per_pixel } })
}
