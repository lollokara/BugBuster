"""
Delete AppleDouble sidecar files (._foo.cpp) from the source tree before
every build.

This volume's filesystem creates a `._foo.cpp` sidecar for any source file
that gets an xattr set on it (e.g. any editor/tool write) -- ESP-IDF's CMake
component registration globs the whole src/ tree and picks these up as real
C/C++ source, failing with "stray '\\5' in program" (binary AppleDouble
header bytes misparsed as tokens) -- this is exactly what broke
src/hat/hat.cpp and src/net/api_core.cpp compiling after an edit.
PlatformIO's `build_src_filter` does NOT apply to espidf-framework projects
(silently ignored, "cannot be used with ESP-IDF" warning), so deleting them
before each build is the only reliable fix.
"""

import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
removed = 0
for root, dirs, files in os.walk(project_dir):
    if os.sep + ".pio" in root or os.sep + ".git" in root:
        continue
    for f in files:
        if f.startswith("._"):
            os.remove(os.path.join(root, f))
            removed += 1
if removed:
    print(f"pio_clean_appledouble: removed {removed} AppleDouble sidecar file(s)")
