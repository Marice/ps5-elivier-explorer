# Changelog

All notable changes to EliVier Explorer are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are the app's content version.

## [Unreleased]

### Added

- A video viewer (`.mp4 .mkv .webm .avi .mov` and more): H.264, H.265, VP9, AV1 and older codecs decoded in
  software up to 3840 x 2176 (4K) with the large heap and 1920 x 1088 without it, with sound (stereo output; surround is mixed down) and subtitles (the file's own
  text tracks, and `.srt`, `.vtt`, `.ass` or `.ssa` files beside the film). Cross plays and pauses, the D-pad seeks
  and changes the volume, L1 and R1 choose the sound track, Square the subtitles, Triangle switches to full
  screen and back. On one console, two 1080p HEVC films (8-bit and 10-bit, 5.1 sound) played at 24 pictures a
  second with none late, and a 4K HEVC 10-bit film at 60 fps showed 60 pictures a second in most seconds; surround is mixed down at the usual weights with a soft limit on peaks.
- Films and songs opened in a viewer play at their own volume (D-pad up and down) and no longer also follow the
  background music volume, which made them about 16 dB quieter at its default.
- The app heap is 1 GiB of direct memory when the console gives it, and 128 MiB of flexible memory as before
  when it does not (a 1080p film needs about 200 MiB).
- `make video` builds FFmpeg, dav1d and zlib from pinned, checked sources (about ten minutes, once). The app
  grows by about 11 MB.
- Console tools: `tools/console-install.py` and `tools/console-trace.py` put a build on a console over FTP and read
  the app's trace; `tools/console-probe.py` and a probe mode in the app measure memory, texture uploads, EGL
  surfaces and the video decoder library before video features are built on them (`docs/CONSOLE_VALIDATION.md`).

### Changed

- The sound player accepts a source that has no sound yet (`audio::Decoder::kTryLater`): it waits instead of
  ending the song.

## [v00.001.000] - 2026-11-22

### Added

- The Files screen: two panes, sorting, marks, rename, new folder, and copy, move and delete jobs that run in
  the background, show progress and can be cancelled.
- Changes only in `/data` and on USB and extended drives; the rest of the file system is read-only. Links are
  never followed.
- Viewers: text, images (JPEG, PNG, BMP, GIF, TGA, DDS), a hex view, and an MP3 player with playlists (`.m3u`)
  and album pictures. *Open as...* opens any file with any viewer.
- File icons, names that scroll when they do not fit, and an About screen on L3 or R3.

## Before this

EliVier Explorer started from PayLoader v00.001.003; the history of PayLoader is in its own repository.
