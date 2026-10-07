use std::collections::HashMap;
use std::io::{self, Cursor};
use std::sync::Arc;
use std::time::Duration;

use openbc_core::image_compare::{CompareMode, ImageCompareOptions, PixelOffset, Verdict};
use openbc_engine::image_compare::{
    spawn_image_compare, ImageByteSource, ImageCompareConfig, ImageCompareEvent, Side, SourceFuture,
    SourceRead,
};
use tokio::sync::mpsc;

struct MemSource(HashMap<String, Vec<u8>>);

impl ImageByteSource for MemSource {
    fn read<'a>(&'a self, path: &'a str, _max: u64) -> SourceFuture<'a, io::Result<SourceRead>> {
        Box::pin(async move {
            let bytes = self
                .0
                .get(path)
                .cloned()
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "missing"))?;
            Ok(SourceRead { size_bytes: bytes.len() as u64, modified_unix_ms: Some(0), bytes })
        })
    }
}

fn png(w: u32, h: u32, f: impl Fn(u32, u32) -> [u8; 4]) -> Vec<u8> {
    let img = image::RgbaImage::from_fn(w, h, |x, y| image::Rgba(f(x, y)));
    let mut out = Vec::new();
    img.write_to(&mut Cursor::new(&mut out), image::ImageFormat::Png).unwrap();
    out
}

async fn next(rx: &mut mpsc::Receiver<ImageCompareEvent>) -> ImageCompareEvent {
    tokio::time::timeout(Duration::from_secs(10), rx.recv())
        .await
        .expect("timed out waiting for event")
        .expect("channel closed")
}

async fn next_result(rx: &mut mpsc::Receiver<ImageCompareEvent>) -> (openbc_core::image_compare::ImageCompareOutput, bool) {
    loop {
        if let ImageCompareEvent::Result { output, binary_identical, .. } = next(rx).await {
            return (output, binary_identical);
        }
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn open_compare_tune_swap_and_errors() {
    let a = png(8, 8, |_, _| [100, 100, 100, 255]);
    let b = png(8, 8, |x, _| [if x < 4 { 100 } else { 110 }, 100, 100, 255]); // delta 10 on half
    let c = png(8, 8, |_, _| [200, 100, 100, 255]); // delta 100 everywhere
    let files = HashMap::from([("a".to_string(), a.clone()), ("a2".to_string(), a), ("b".to_string(), b), ("c".to_string(), c)]);

    let (tx, mut rx) = mpsc::channel(4);
    let handle = spawn_image_compare(
        &tokio::runtime::Handle::current(),
        Arc::new(MemSource(files)),
        tx,
        ImageCompareOptions { tolerance: 25, ..Default::default() },
        ImageCompareConfig::default(),
    );

    // Byte-identical files -> BinarySame, and the loaded event carries both images.
    handle.open("a", "a2").unwrap();
    assert!(matches!(next(&mut rx).await, ImageCompareEvent::Loading { .. }));
    match next(&mut rx).await {
        ImageCompareEvent::Loaded { left, binary_identical, left_image, .. } => {
            assert!(binary_identical);
            assert_eq!((left.width, left.height), (8, 8));
            assert_eq!(left.format.as_deref(), Some("PNG"));
            assert_eq!(left_image.dimensions(), (8, 8));
        }
        other => panic!("unexpected {other:?}"),
    }
    let (out, bin) = next_result(&mut rx).await;
    assert!(bin);
    assert_eq!(out.verdict, Verdict::BinarySame);

    // Probe is available after the first result.
    let probe = handle.probe(4, 4).expect("probe snapshot");
    assert_eq!(probe.left[40], Some([100, 100, 100, 255]));

    // Different pixels within tolerance -> Similar.
    handle.open("a", "b").unwrap();
    let (out, bin) = next_result(&mut rx).await;
    assert!(!bin);
    assert_eq!(out.verdict, Verdict::Similar);
    assert_eq!(out.stats.similar_pixels, 32);

    // Tightening the tolerance re-runs only the compare phase (no Loading event).
    handle.set_options(ImageCompareOptions { tolerance: 5, ..handle.options() });
    let (out, _) = next_result(&mut rx).await;
    assert_eq!(out.verdict, Verdict::Different);
    assert_eq!(out.options.tolerance, 5);

    // Blend mode + offset change are honoured and echoed back.
    handle.set_options(ImageCompareOptions {
        mode: CompareMode::Blend,
        offset: PixelOffset { x: 2, y: 0 },
        ..handle.options()
    });
    let (out, _) = next_result(&mut rx).await;
    assert_eq!(out.options.offset, PixelOffset { x: 2, y: 0 });
    assert_eq!(out.verdict, Verdict::Different); // canvas not fully covered

    // Swap negates the offset and does not re-read files.
    handle.swap().unwrap();
    let (out, _) = next_result(&mut rx).await;
    assert_eq!(out.options.offset, PixelOffset { x: -2, y: 0 });

    // A missing file is reported per side with its path.
    handle.open("a", "nope").unwrap();
    loop {
        match next(&mut rx).await {
            ImageCompareEvent::PathError { side, path, .. } => {
                assert_eq!((side, path.as_str()), (Side::Right, "nope"));
                break;
            }
            ImageCompareEvent::Loading { .. } => continue,
            other => panic!("unexpected {other:?}"),
        }
    }
    assert!(handle.probe(0, 0).is_none());
    handle.close();
}
