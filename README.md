# 轻录 · ScreenRecorderLite

[English](README.en.md) · [下载](https://github.com/sqliu07/ScreenRecorderLite/releases)

一款轻量的 Windows 录屏工具。选择屏幕或区域，录制画面和声音，保存为本地 MP4 视频或 GIF 动图。

## 功能

- **全屏与区域录制**：选择显示器，或拖动框选需要录制的区域。
- **系统声音与麦克风**：录制中可分别开关，MP4 保留混音、系统声、麦克风三条音轨。
- **悬浮控制**：查看录制时间、切换声音、截图和停止录制。
- **GIF 动图**：直接录制无声动图，方便演示和分享。
- **PNG 截图**：一键保存当前画面。
- **HDR 屏幕支持**：将 HDR 画面映射为 SDR，保存为 MP4、GIF 或 PNG。
- **画质设置**：选择分辨率、帧率和画质，也可自定义输出尺寸。
- **中英文界面**：在窗口右上角的“语言”菜单中切换。

## 下载与使用

运行环境：Windows 10 22H2 或更新版本（64 位）。

1. 从 [Releases](https://github.com/sqliu07/ScreenRecorderLite/releases) 下载 `ScreenRecorderLite-win-x64.zip`，完整解压后运行 `ScreenRecorderLite.exe`。请保留解压后的全部文件。
2. 选择录制范围、输出格式、画质、声音和保存位置，点击“开始录制”。
3. 使用悬浮窗控制录制，点击停止后保存视频。

录制可保存为 MP4 视频或无声 GIF 动图；PNG 截图保存在导出目录下的 `Screenshots` 文件夹。MP4 默认播放混音轨，也可在支持音轨切换的播放器中单独选择系统声或麦克风。

## 快捷键

| 快捷键 | 操作 |
| --- | --- |
| `Ctrl + Alt + F9` | 开始 / 停止录制 |
| `Ctrl + Alt + F10` | 截图 |
| `Ctrl + Alt + F11` | 显示 / 隐藏录制悬浮窗 |

<details>
<summary>从源码构建</summary>

需要支持 C++20 的 MSVC 构建工具、Windows SDK 10.0.22621+、CMake 3.25+、Ninja 和 vcpkg。可以只安装 C++ Build Tools；脚本会自动检测工具链，无须安装完整的 Visual Studio IDE。

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
scripts\build.cmd
```

首次构建时，vcpkg 会下载并编译所需的 FFmpeg 库；源码不包含预编译库，也无需单独安装 `ffmpeg.exe`。

运行 `out\build\windows\Release\ScreenRecorderLite.exe`，并保留同目录的 FFmpeg DLL。

</details>

## 许可证

项目源码采用 [MIT License](LICENSE)。随包提供的 FFmpeg 使用 LGPL 许可证，详见 [第三方软件声明](THIRD_PARTY_NOTICES.md)。
