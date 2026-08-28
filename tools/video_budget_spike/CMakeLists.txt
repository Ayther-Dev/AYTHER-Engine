# ---------------------------------------------------------------------------
# video_budget_spike — ¿entra un frame de video por frame de juego? (issue #263)
# Mide el memcpy de staging, que es el costo que el motor ya identificó como
# dominante (un memcpy de ~10 MB hacía starve al audio). No linkea el motor: es
# una medición de presupuesto, no un test de integración.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(video_budget_spike main.cpp)
target_compile_features(video_budget_spike PRIVATE cxx_std_17)
