#![no_main]

use ayther_core::archive_vfs::AyArchive;
use ayther_core::pack_validate::{SessionCtx, compat_grade, validate_path};
use libfuzzer_sys::fuzz_target;
use std::io::Write;

const MAX_INPUT: usize = 1024 * 1024;

fn exercise(path: &std::path::Path) {
    let Some(path) = path.to_str() else { return };
    let ctx = SessionCtx {
        engine_version: Some(ayther_core::RELEASE_VERSION),
        ..SessionCtx::default()
    };
    let _ = validate_path(path, &ctx);
    let _ = compat_grade(path, &ctx);
    let _ = AyArchive::open_verbose(path);
}

fuzz_target!(|data: &[u8]| {
    if data.len() > MAX_INPUT {
        return;
    }
    let Ok(dir) = tempfile::tempdir() else { return };

    // Exercise the raw container rejection path.
    let raw_path = dir.path().join("raw.ay");
    if std::fs::write(&raw_path, data).is_ok() {
        exercise(&raw_path);
    }

    // Keep ZIP framing valid so mutations reach manifest and entry decoding.
    let zip_path = dir.path().join("framed.ay");
    let Ok(file) = std::fs::File::create(&zip_path) else { return };
    let mut zip = zip::ZipWriter::new(file);
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    if zip.start_file("manifest.toml", options).is_err()
        || zip.write_all(data).is_err()
        || zip.start_file("assets/fuzz.bin", options).is_err()
        || zip.write_all(data).is_err()
        || zip.finish().is_err()
    {
        return;
    }
    exercise(&zip_path);
});
