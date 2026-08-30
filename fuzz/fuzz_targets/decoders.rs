#![no_main]

use ayther_core::{sf2, sf2_bake, sf3};
use libfuzzer_sys::fuzz_target;

const MAX_INPUT: usize = 1024 * 1024;

fuzz_target!(|data: &[u8]| {
    if data.len() > MAX_INPUT {
        return;
    }

    let _ = sf3::is_sf3(data);
    let _ = sf3::to_sf2(data);
    let _ = sf2_bake::list_presets(data);
    let _ = sf2_bake::bake(data, &[(0, 0), (0, 1)], "fuzz");
    let _ = sf2::Sf2Synth::new(data, 44_100);
});
