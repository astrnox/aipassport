# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/esp/esp-idf-v5.5.3/components/bootloader/subproject"
  "/workspace/build-baseline/bootloader"
  "/workspace/build-baseline/bootloader-prefix"
  "/workspace/build-baseline/bootloader-prefix/tmp"
  "/workspace/build-baseline/bootloader-prefix/src/bootloader-stamp"
  "/workspace/build-baseline/bootloader-prefix/src"
  "/workspace/build-baseline/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/build-baseline/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/build-baseline/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
