"""
Guarantee the riscv32 toolchain/cmake/ninja are on PATH before ESP-IDF's cmake
configure step runs.

PlatformIO's own espidf integration is supposed to inject these automatically,
but a fresh/reinstalled toolchain package (e.g. after a `pio pkg` cache
clear) can leave a bare `pio run` invocation from an ordinary shell failing
at the very first cmake `project()` call with:

    CMake Error: The CMAKE_C_COMPILER: riscv32-esp-elf-gcc
    is not a full path and was not found in the PATH.

even though the toolchain is correctly installed under
~/.platformio/packages/. Prepending these dirs here makes `pio run` work the
same regardless of what the invoking shell's PATH already contains.
"""

import os

Import("env")

home = os.path.expanduser("~")
extra_dirs = [
    os.path.join(home, ".platformio", "packages", "toolchain-riscv32-esp", "riscv32-esp-elf", "bin"),
    os.path.join(home, ".platformio", "packages", "tool-cmake", "bin"),
    os.path.join(home, ".platformio", "packages", "tool-ninja"),
]
for d in extra_dirs:
    if os.path.isdir(d):
        env.PrependENVPath("PATH", d)
