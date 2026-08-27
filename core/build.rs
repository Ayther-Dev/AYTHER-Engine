// build.rs — compile the cxx::bridge C++ glue for ayther_core.
//
// cxx_build reads src/ffi.rs, generates a C++ header + implementation, and
// compiles them into a static library that corrosion links alongside the Rust
// static library.  The generated header is made available to C++ callers via
// the `ayther_cxx` CMake target produced by corrosion_add_cxxbridge.

fn main() {
    cxx_build::bridge("src/ffi.rs")
        .std("c++20")
        .compile("ayther_cxx");

    println!("cargo:rerun-if-changed=src/ffi.rs");
}
