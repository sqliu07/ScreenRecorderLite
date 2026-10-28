# Third-party software

ScreenRecorderLite source code is licensed under the MIT License. Third-party
software retains its own license.

## FFmpeg

This software uses libraries from the [FFmpeg project](https://ffmpeg.org/)
under the GNU Lesser General Public License (LGPL). The portable package
includes the dynamically linked avformat, avcodec and avutil DLLs for MP4
muxing. No FFmpeg executable is required.

The pinned vcpkg configuration builds shared libraries without GPL or nonfree
features. The DLLs use the static MSVC runtime and can be replaced with
compatible builds. The MIT license places no restriction on modifying,
relinking or debugging the application with modified FFmpeg libraries.

Each [GitHub release](https://github.com/sqliu07/ScreenRecorderLite/releases) includes:

- `licenses/FFmpeg-LICENSE.txt` and `licenses/FFmpeg-build.json` inside the portable ZIP, with the runtime versions, licenses and configure flags.
- `FFmpeg-corresponding-source.tar.gz` with the exact patched source, manifest baseline and build recipe used for the DLLs.
- SHA-256 checksums for both archives.

See [FFmpeg license information](https://ffmpeg.org/legal.html) and the included
license text for details. FFmpeg remains the work of its respective authors.
