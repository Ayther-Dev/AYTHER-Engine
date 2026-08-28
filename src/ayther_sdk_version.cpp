// ---------------------------------------------------------------------------
// ayther_sdk_version.cpp — la versión que reporta la LIB ().
//
// Vive en un .cpp y no en el header porque ésa es toda la gracia: acá se
// compila UNA vez, con la versión que tenía el motor el día que se construyó,
// y el header lleva la que tenía el consumidor el día que compiló. Si las dos
// salieran del mismo `#define`, el chequeo de compatibilidad no podría fallar
// nunca — y un chequeo que siempre pasa es peor que no tenerlo, porque además
// tranquiliza.
// ---------------------------------------------------------------------------
#include "ayther_sdk_version.h"

namespace ayther {

SdkVersion sdk_version() noexcept {
    return SdkVersion{ AYTHER_SDK_VERSION_MAJOR,
                       AYTHER_SDK_VERSION_MINOR,
                       AYTHER_SDK_VERSION_PATCH };
}

}  // namespace ayther
