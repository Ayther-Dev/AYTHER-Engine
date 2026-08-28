# ---------------------------------------------------------------------------
# Vulkan and shader contracts.
#
# These are the CPU side of the shader contracts and need no device, so they
# are deliberately NOT labelled `gpu`: `ctest -LE gpu` still runs them. The
# hardware counterparts live in tools/ and are registered by the parent
# directory under AYTHER_BUILD_GPU_TESTS.
# ---------------------------------------------------------------------------

foreach(_source IN ITEMS
        video_bt601_test.cpp
        xbr_ref_test.cpp)
    ayther_add_native_test("${_source}" renderer)
endforeach()
