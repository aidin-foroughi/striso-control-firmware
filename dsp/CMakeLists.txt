# striso_dsp: the synth's DSP, platform independent (no HAL, no libm calls, no heap): the strisy
# engine and its helpers (strisy.h, fastpow.h), the voice engines (voice_<name>.h), the master
# effects (fx.h). Header only on purpose: the engines are static inline and compile into the one
# translation unit that runs them (src/synth/synth_strisy.cpp on the board at -O3, the renderers
# and tests on the host), so the compiler sees the whole DSP at once.
#   #include "striso_dsp/strisy.h"   (engine headers include their siblings directly)
add_library(striso_dsp INTERFACE)
add_library(striso::dsp ALIAS striso_dsp)
target_include_directories(striso_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
