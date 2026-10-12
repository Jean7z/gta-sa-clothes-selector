APP_STL := c++_static
# arm64-v8a only. SA 2.10 ships arm64 exclusively and is the only libGTASA.so
# this mod was verified against; the script loader is resolved by symbol name,
# but the psdk headers are only checked for arm64.
APP_ABI := arm64-v8a
APP_PLATFORM := android-24