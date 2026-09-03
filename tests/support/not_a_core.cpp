#if defined(_WIN32)
#define AYTHER_TEST_EXPORT __declspec(dllexport)
#else
#define AYTHER_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" AYTHER_TEST_EXPORT int ayther_not_a_core() {
    return 0;
}
