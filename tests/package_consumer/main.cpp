#include <ayther/ayther_sdk.h>
#include <ayther/ayther_sdk_version.h>
#include <ayther/ayther_session.h>

#include <iostream>

int main() {
    static_assert(sizeof(AySessionConfig) > 0);
    static_assert(sizeof(ayther::FrameView) > 0);

    const auto error = ayther::sdk_version_check();
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "AYTHER SDK " << ayther::sdk_version().str() << '\n';
    return 0;
}
