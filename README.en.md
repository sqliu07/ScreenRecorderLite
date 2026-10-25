# ScreenRecorderLite

[简体中文](README.md) · [Download](https://github.com/sqliu07/ScreenRecorderLite/releases)

A lightweight Windows screen recorder. Capture a display or a selected region,
record audio, and save MP4 videos locally.

## Features

- **Full-screen and region capture** — choose a display or drag to select an area.
- **System audio and microphone** — toggle each source while recording. MP4 files include mixed, system-audio and microphone tracks.
- **Floating controls** — view elapsed time, toggle audio, take screenshots and stop recording.
- **PNG screenshots** — save the current frame with one click.
- **Quality settings** — choose resolution, frame rate and quality, or set a custom output size.
- **Chinese and English UI** — switch using the Language menu in the upper-right corner.

## Download and use

Requires 64-bit Windows 10 22H2 or later with an SDR display mode.

1. Download `ScreenRecorderLite-win-x64.zip` from [Releases](https://github.com/sqliu07/ScreenRecorderLite/releases). Extract the complete folder and run `ScreenRecorderLite.exe`. Keep all extracted files together.
2. Choose the capture area, quality, audio sources and save location, then select **Start recording**.
3. Use the floating controls during recording. Select **Stop** to save the video.

Videos are saved as MP4 files. Screenshots go to the `Screenshots` folder inside
the export directory. The mixed audio track plays by default; compatible media
players can switch to the isolated system-audio or microphone track.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl + Alt + F9` | Start / stop recording |
| `Ctrl + Alt + F10` | Take a screenshot |
| `Ctrl + Alt + F11` | Show / hide the floating controls |

<details>
<summary>Build from source</summary>

Requires C++20-capable MSVC Build Tools, Windows SDK 10.0.22621+, CMake 3.25+, Ninja and
vcpkg. The build script detects the toolchain automatically. The standalone C++
Build Tools are sufficient; the full Visual Studio IDE is optional.

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
scripts\build.cmd
```

Run `out\build\windows\Release\ScreenRecorderLite.exe` and keep `ffmpeg.exe` beside it.

</details>

## License

The project source is licensed under the [MIT License](LICENSE). The bundled
FFmpeg tool uses the LGPL; see [Third-party notices](THIRD_PARTY_NOTICES.md).
