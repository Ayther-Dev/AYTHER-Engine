#![no_main]

use libfuzzer_sys::fuzz_target;
use std::ffi::CString;

const MAX_INPUT: usize = 1024 * 1024;

fuzz_target!(|data: &[u8]| {
    if data.len() > MAX_INPUT {
        return;
    }

    let mut x = 0i16;
    let mut y = 0i16;
    let mut banks = [0u16; 8];
    let mut presets = [0u16; 8];

    // The fuzzer supplies valid pointer/length pairs; malformed pointees are
    // the boundary condition under test, while invalid pointers would make the
    // harness itself violate the documented C ABI contract.
    unsafe {
        let _ = ayther_core::ayther_sonic_read_xy(
            data.as_ptr(),
            data.len(),
            &mut x,
            &mut y,
        );
        let _ = ayther_core::ayther_sonic_read_velocity(
            data.as_ptr(),
            data.len(),
            &mut x,
            &mut y,
        );
        let _ = ayther_core::ayther_sf2_list_presets(
            data.as_ptr(),
            data.len(),
            banks.as_mut_ptr(),
            presets.as_mut_ptr(),
            banks.len() as u32,
        );
    }

    let text: Vec<u8> = data.iter().copied().filter(|byte| *byte != 0).collect();
    let Ok(text) = CString::new(text) else { return };
    unsafe {
        let profile = ayther_core::ayther_game_profile_load_str(text.as_ptr());
        if !profile.is_null() {
            let mut ids = [0u64; 8];
            let mut world_x = [0i32; 8];
            let mut world_y = [0i32; 8];
            let _ = ayther_core::ayther_game_profile_entities(
                profile,
                data.as_ptr(),
                data.len(),
                ids.as_mut_ptr(),
                world_x.as_mut_ptr(),
                world_y.as_mut_ptr(),
                ids.len(),
            );
            ayther_core::ayther_game_profile_free(profile);
        }
    }
});
