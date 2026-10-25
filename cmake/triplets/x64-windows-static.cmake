set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# FFmpeg is an independent runtime tool, so both app configurations use its Release build.
set(VCPKG_BUILD_TYPE release)
