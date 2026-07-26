"""
Stop every `pio run` from doing a full ESP-IDF CMake reconfigure (which
regenerates build.ninja and makes ninja recompile the entire project, even
when nothing changed).

PlatformIO's own espidf builder (`is_cmake_reconfigure_required()` in
.../platforms/espressif32/builder/frameworks/espidf.py) decides whether to
re-run `cmake` by comparing CMakeCache.txt's mtime against a few other
files/dirs, including `<build_dir>/config` (the generated sdkconfig.h etc.,
rewritten by kconfgen on every build). CMakeCache.txt itself is only written
once, at initial configure time, and never touched again by a plain ninja
build -- so the moment `config/`'s mtime (bumped on THIS build) ends up newer
than CMakeCache.txt's (from some EARLIER configure), every following build
sees "reconfigure required" and starts over, forever, even though nothing
actually needs it.

Touching CMakeCache.txt's mtime to "now" after each build -- once anything
that build regenerated has already been written -- keeps it the newest file
in that comparison, so the next build correctly sees no reconfigure is
needed and ninja does a real incremental build.
"""

import os

Import("env")


def _touch_cmake_cache(source, target, env):
    cache_file = os.path.join(env.subst("$BUILD_DIR"), "CMakeCache.txt")
    if os.path.isfile(cache_file):
        os.utime(cache_file, None)


env.AddPostAction("buildprog", _touch_cmake_cache)
