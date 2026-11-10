# parity_bench (spec 008, TP-005.13): plane 3 of the parity bench. It compares
# the FrameView an AytherSession produces with the libretro provider (gpgx)
# against the native Tessera provider, and converts a recorded take (.arp) into
# a Tessera input trace. This is the skeleton: the subcommands parse their
# arguments and report "not implemented" until TP-005.14 onwards.
add_executable(parity_bench main.cpp trace_arp.cpp frame_dump.cpp frame_compare.cpp
    exception_accounting.cpp identities.cpp)
target_link_libraries(parity_bench PRIVATE ayther_engine ayther_core)
target_compile_features(parity_bench PRIVATE cxx_std_20)
set_target_properties(parity_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ayther.unit.parity_bench_help COMMAND parity_bench --help)
set_tests_properties(ayther.unit.parity_bench_help PROPERTIES
    PASS_REGULAR_EXPRESSION "dump.*compare.*trace-from-arp.*--provider libretro\|tessera_native"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_unknown_subcommand COMMAND parity_bench frobnicate)
set_tests_properties(ayther.unit.parity_bench_unknown_subcommand PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown subcommand: frobnicate"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_bad_provider COMMAND parity_bench dump --provider bogus)
set_tests_properties(ayther.unit.parity_bench_bad_provider PROPERTIES
    PASS_REGULAR_EXPRESSION "invalid --provider: bogus"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_dump_needs_args COMMAND parity_bench dump --provider libretro)
set_tests_properties(ayther.unit.parity_bench_dump_needs_args PROPERTIES
    PASS_REGULAR_EXPRESSION "dump needs --core"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_trace_needs_args COMMAND parity_bench trace-from-arp)
set_tests_properties(ayther.unit.parity_bench_trace_needs_args PROPERTIES
    PASS_REGULAR_EXPRESSION "trace-from-arp needs --arp"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_compare_needs_args COMMAND parity_bench compare)
set_tests_properties(ayther.unit.parity_bench_compare_needs_args PROPERTIES
    PASS_REGULAR_EXPRESSION "compare needs --golden"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_compare_identities_needs_args
    COMMAND parity_bench compare-identities)
set_tests_properties(ayther.unit.parity_bench_compare_identities_needs_args PROPERTIES
    PASS_REGULAR_EXPRESSION "compare-identities needs --golden"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_identities_need_segments
    COMMAND parity_bench dump --provider libretro --core x --rom x --frames 1 --out x --identities x)
set_tests_properties(ayther.unit.parity_bench_identities_need_segments PROPERTIES
    PASS_REGULAR_EXPRESSION "--identities and --segments go together"
    LABELS "engine;native;unit;parity" TIMEOUT 30)

add_test(NAME ayther.unit.parity_bench_check_needs_args COMMAND parity_bench check)
set_tests_properties(ayther.unit.parity_bench_check_needs_args PROPERTIES
    PASS_REGULAR_EXPRESSION "check needs --exceptions"
    LABELS "engine;native;unit;parity" TIMEOUT 30)
