# vnef-video

Minimal FFmpeg-based video decoder module intended for Vnefall.

## Goals
- Cross‑platform (Linux/Windows/macOS)
- Decode **video + audio**
- Simple C API, easy to bind from Odin/C/C++

## What it is
- A small wrapper around FFmpeg to decode to:
  - Video: RGBA frames
  - Audio: signed 16‑bit interleaved PCM

## .video Support
The decoder can open either plain media files (`.webm`, `.mp4`, etc.) or the custom
`.video` container used by the build tool. The `.video` file format is:

- 4 bytes: magic `VID0`
- 4 bytes: version (uint32 little‑endian, currently `1`)
- 8 bytes: WebM byte size (uint64 little‑endian)
- followed by raw WebM bytes

## What it is not
- A renderer or audio player
- A full media framework

## Build
Requires FFmpeg dev libs:
- `libavformat`
- `libavcodec`
- `libavutil`
- `libswscale`
- `libswresample`

Linux (example):
```bash
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

Build:
```bash
cmake -S . -B build
cmake --build build
```

## Quick Test
```bash
./build/vnef_dump /path/to/file.webm
./build/vnef_dump /path/to/file.video
```

## Debug Logging
Build with `-DVNEF_VIDEO_DEBUG=1` to enable verbose decode logs. Default is off.

## API Overview
See `include/vnef_video.h`.

## Odin Binding
An Odin binding is provided at `bindings/odin/vnef_video.odin`.

Example usage:
```bash
odin run examples/odin_dump.odin -extra-linker-flags:\"-L./build -lvnef_video\"
```

## Licensing
This project is MIT (see `LICENSE`).

FFmpeg is **not** bundled. You are responsible for complying with FFmpeg’s license in your distribution.
Using LGPL builds with dynamic linking is the simplest compliance path.
See `THIRD_PARTY_NOTICES.md` for a summary of FFmpeg licensing obligations.
