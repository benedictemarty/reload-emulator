# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/bmarty/reload-emulator/platforms/rp2040/lib/pico-sdk/tools/pioasm")
  file(MAKE_DIRECTORY "/home/bmarty/reload-emulator/platforms/rp2040/lib/pico-sdk/tools/pioasm")
endif()
file(MAKE_DIRECTORY
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/pioasm"
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm"
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/tmp"
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/src/PioasmBuild-stamp"
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/src"
  "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/src/PioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/src/PioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/bmarty/reload-emulator/platforms/rp2040/build_mb/lib/PicoDVI/software/libdvi/pioasm/src/PioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
