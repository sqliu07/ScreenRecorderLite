# Third-party software

ScreenRecorderLite source code is licensed under the MIT License. Third-party
software retains its own license.

## FFmpeg

The portable package includes FFmpeg as a separate executable for MP4 muxing.
The default vcpkg configuration builds an LGPL version without GPL or nonfree
features, using a static runtime so users can run the complete portable folder.

Each GitHub release includes:

- `licenses/FFmpeg-LICENSE.txt` and `licenses/FFmpeg-build.txt` inside the portable ZIP.
- `FFmpeg-corresponding-source.tar.gz` with the exact patched source and build information.
- SHA-256 checksums for both archives.

FFmpeg project and license information: <https://ffmpeg.org/legal.html>.
