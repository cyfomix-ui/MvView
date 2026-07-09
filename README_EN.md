# MvSuite

[日本語](./README.md) | [English](./README_EN.md)

**MvSuite** is a Windows media workflow utility suite for working with video, image, and audio files.

It is designed to help you collect, preview, arrange, inspect, and export media assets with lightweight tools before moving into full editing software.

> Status: Experimental / under active development  
> Platform: Windows 11  
> Repository: https://github.com/cyfomix-ui/MvSuite

---

## Highlights

- Media-focused utility suite for video, images, and audio
- Quick preview from Explorer and in-app media lists
- Multi-media simultaneous preview playback
- Stocker-style workspace for collecting and checking media assets
- Lightweight drag-and-drop players for audio and video
- FFmpeg-based export, conversion, and concatenation support
- Designed as a pre-editing workflow companion for tools such as DaVinci Resolve and Shotcut
- Designed with MvSticky-style media attachment and preview workflows in mind
- Event-driven behavior is preferred where practical, avoiding heavy always-on timer polling

---

## Main Apps / Features

### MvStocker / Stocker

A workspace for collecting video, image, and audio assets and checking their order or playback before export.

Typical uses:

- Temporary media collection
- Playback order checking
- Pre-export sequence review
- Bridge to FFmpeg-based output workflows

### MvHover / MvView Preview

A lightweight preview function that opens a small media preview window when a media file is selected.

Typical uses:

- Fast video/image checking in Explorer
- Quick preview from media lists inside the app
- Simultaneous preview of multiple files
- Faster media selection and review

### DropMp3 / DropMp4

Lightweight drag-and-drop players for quickly checking local audio and video files.

Typical uses:

- Quick audio playback
- Quick video playback
- Simple playlist-style playback
- Small local media preview player

### MvSticky Integration

MvSticky is a related sticky-note and media memo application. MvSuite is designed so that media preview concepts and lightweight media handling can be shared with MvSticky-style workflows.

---

## Requirements

Base environment:

- Windows 11
- Python 3.x
- PowerShell 7 or Windows PowerShell
- FFmpeg

Optional components depending on features:

- mpv / libmpv runtime
- Visual Studio / Build Tools
- .NET SDK

Features that use FFmpeg require `ffmpeg.exe` to be available through PATH or placed where the application can find it.

---

## Getting Started

Using a release build:

1. Download the latest package from GitHub Releases.
2. Extract the zip file to any folder.
3. Run the launcher script or executable.
4. Place FFmpeg / mpv runtime files if required by the feature you use.

Building from source:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1 -Publish
```

Running as Administrator is not recommended unless a specific function requires it.

---

## Project Layout

The current source entry point is based under `apps/`.

```text
apps/
  launchers/   Launch scripts
  shared/      Shared code
  tools/       Helper tools

docs/
  Design notes, work logs, structure documents

version.xml
  Application version metadata

build.ps1
  Build / package script
```

---

## Version Management

Version information is managed through `version.xml` as the single source of truth.

The version should be reflected in:

- Window titles
- Tray tooltips
- About dialogs
- Splash screens
- Startup logs
- README files
- Build output folders
- Zip / release package names

Versions start at `0.01` and are incremented by `0.01` for each source/release update.

---

## License and Notes

This project is a personal experimental tool under active development.

- No warranty is provided.
- Use at your own risk.
- Make sure you have the right to use any media files you process.
- If FFmpeg / mpv / libmpv or other third-party components are bundled, include and follow their respective license notices.

---

## Development Philosophy

MvSuite is not intended to replace full editing software. It is a lightweight companion suite for checking media, organizing assets, making quick previews, performing simple output tasks, and preparing materials for external editors.

The project focuses on:

- Fast media checking
- Lightweight startup
- Comparing multiple media files at once
- Not interrupting Explorer or existing workflows
- Keeping settings and version management clear

---

## Repository

https://github.com/cyfomix-ui/MvSuite
