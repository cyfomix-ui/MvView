# MvView v0.30

[日本語](README.md)

![MvView previewing Explorer media](docs/images/MvView_Title.png)

![Multiple-media preview](docs/images/MvView03.png)

*Multiple-media preview*

MvView is a resident C++/Win32 application that previews images, videos, and audio in a small libmpv-powered window when you hover over media items in Windows 11 Explorer.

## What's new in v0.30

- Adds a **Preview position** setting
- Lets you choose **Top-left**, **Top-right**, **Center**, **Bottom-left**, or **Bottom-right**
- Applies the selected position to both single previews and multi-preview tile grids of up to nine items
- Positions previews relative to the mouse cursor and only clamps them when they would extend beyond the current monitor's work area
- Existing settings default to **Top-right**, which is closest to the previous behavior

## Highlights

- Monitors media-item hover only while Explorer is in the foreground
- Resolves full paths safely by cross-checking UI Automation with the active Shell View
- Previews images, video, and audio through libmpv
- Confirms Ctrl/Shift multi-selection before showing up to nine tiled previews
- Configurable hover delay, leave behavior, audio, resolution, border color, preview position, and pointer movement over the preview
- Supports `--open` and generation-aware UTF-16 JSON `WM_COPYDATA` hover IPC
- Notifies you when a newer GitHub Release is available

## Supported media

| Type | Extensions |
|---|---|
| Images | `.jpg` `.jpeg` `.png` `.webp` `.bmp` `.gif` `.tif` `.tiff` |
| Video | `.mp4` `.mkv` `.mov` `.avi` `.webm` `.m4v` `.wmv` `.flv` `.ts` `.mts` `.m2ts` `.mpg` `.mpeg` |
| Audio | `.mp3` `.wav` `.flac` `.m4a` `.aac` `.ogg` `.opus` `.wma` `.aiff` |

## Usage

1. Download and extract the ZIP from Releases.
2. Start `MvView.exe`.
3. Hover over a media item in Explorer for the configured delay.
4. Move away to close the preview, or enable pointer movement onto the preview in Settings.

Clicks and selection changes alone do not start playback. MvView also refuses to play an item when it cannot resolve its path unambiguously.

## Preview controls

- Click: pause / resume
- Mouse wheel: seek
- Ctrl + wheel: temporarily change preview resolution
- Click the volume display: open the vertical volume slider
- Esc: close the preview
- Multiple previews: up to nine

## Settings

Settings are stored in:

```text
%APPDATA%\MvView\settings.json
```

The tray Settings dialog includes hover enablement and delay (0–5000 ms), immediate close on leave, multi-selection confirmation, pointer movement over the preview, startup audio, resolution, border color, and preview position.

## DirectOpen

Open a media file directly from another application or PowerShell:

```powershell
MvView.exe --open "D:\media\sample.mp4"
```

If MvView is already resident, the second process forwards the path to the existing instance.

## External hover IPC

DirectOpen uses `WM_COPYDATA` with `dwData == 1`. External hover clients use `dwData == 0x4D564831` (`MVH1`) with a null-terminated UTF-16 JSON document.

Protocol `mvview-hover` version `1` supports `hover_open`, `hover_update`, `hover_move`, and `hover_close`. Requests include `source_pid`, `request_id`, and a monotonically increasing `generation`; stale requests are ignored.

## libmpv

### Installation

MvView requires a 64-bit libmpv runtime.

1. Open the [official mpv installation page](https://mpv.io/installation/) and follow its link to the [shinchiro Windows builds](https://github.com/shinchiro/mpv-winbuild-cmake/releases/latest).
2. From the latest release assets, download `mpv-dev-x86_64-...7z`. The `x86_64-v3` build targets newer CPUs; choose the regular `x86_64` build if you are unsure.
3. Extract the archive with [7-Zip](https://www.7-zip.org/) or a compatible tool, then copy `mpv-2.dll` beside `MvView.exe`.
4. Start `MvView.exe` and verify that a media preview opens.

If the package provides `libmpv-2.dll` instead, keep that filename when copying it. Download DLLs only from a trusted distribution source, and do not use the 32-bit (`i686`) build.

Runtime DLLs are searched in this order:

```text
mpv-2.dll beside MvView.exe
libmpv-2.dll beside MvView.exe
runtime\mpv-2.dll
runtime\libmpv-2.dll
mpv-2.dll / libmpv-2.dll on PATH
```

MvView can still start in the tray without the DLL, but playback requires a libmpv runtime.

## Build

Requirements:

- Windows 11
- Visual Studio or Build Tools
- Desktop development with C++
- Windows SDK

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1 -Configuration Release -Publish
```

Output:

```text
bin\x64\Release\MvView.exe
publish\MvView_v0.30\
publish\MvView_v0.30_YYYYMMDD_HHMMSS.zip
```

## Update check

After startup, MvView checks the latest release of `cyfomix-ui/MvView` once in the background. It only prompts when that version is newer than `version.xml`. The **Download** button opens the release ZIP, or the release page when no asset is attached. Network failures never block startup and do not display an error dialog.

## Log

```text
%APPDATA%\MvView\logs\MvView.log
```

## Known limitations

- Items whose full path cannot be resolved uniquely from UI Automation and the Shell View are not played.
- Virtual items, unavailable cloud placeholders, and items without filesystem paths are unsupported.
- Changes to Explorer's internal UI may require adjustments for particular view modes.

## Versioning

- Current version: `v0.30`
- `version.xml` is the single source of truth.
- The same version is applied to the tray tooltip, About dialog, splash screen, executable resources, and package name.
