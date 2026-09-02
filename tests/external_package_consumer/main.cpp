#include <ayther/engine/engine.hpp>

int main() {
    const auto version = ayther::engine::version();
    return version.major == 0U && version.minor == 1U && version.patch == 0U
               ? 0
               : 1;
}

