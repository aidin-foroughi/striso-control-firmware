# Host builds (presets host, host-asan): unit tests and renderers of the striso_dsp library on the PC.
# striso_host_options: an interface target with the language level, warnings and sanitizers that
# every host target links.

option(STRISO_SANITIZE "host: build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(STRISO_GOLDEN "host tests: compare renders with the golden checksums (tests/golden/renders.txt)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(striso_host_options INTERFACE)
target_compile_options(striso_host_options INTERFACE -Wall -Wextra)
if(STRISO_SANITIZE)
  # the bowed harness' set: float-divide-by-zero and float-cast-overflow are not in 'undefined'
  set(_san -fsanitize=address,undefined,float-divide-by-zero,float-cast-overflow
           -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_compile_options(striso_host_options INTERFACE ${_san} -g)
  target_link_options(striso_host_options INTERFACE ${_san})
endif()
