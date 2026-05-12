"""
Force PlatformIO's filesystem target to use the web SPIFFS partition.

PlatformIO's espressif32 builder picks the last data/spiffs row in
partitions.csv. BugBuster has two SPIFFS partitions:

    spiffs   0x810000  0x400000  web UI
    scripts  0xc10000  0x300000  MicroPython scripts

Without this override, `pio run -t buildfs` emits a 3 MB image for the scripts
partition. Uploading that image through /api/ota/uploadfs corrupts the 4 MB web
partition and the device boots with `/spiffs/index.html` missing.
"""

Import("env")

WEB_SPIFFS_START = 0x810000
WEB_SPIFFS_SIZE = 0x400000


def use_web_spiffs_partition(source, target, env):
    env.Replace(
        FS_START=WEB_SPIFFS_START,
        FS_SIZE=WEB_SPIFFS_SIZE,
        FS_PAGE=0x100,
        FS_BLOCK=0x1000,
    )
    return None


env.AddPreAction("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin", use_web_spiffs_partition)
