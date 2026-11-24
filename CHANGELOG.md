# Changelog

All notable changes to EliVier Explorer are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are the app's content version.

## [Unreleased]

### Fixed

- "Fill the screen" in the video viewer's Options menu now actually fills it; confirming it
  (a checkable item) never reached the code that applied it.

## [v00.002.001] - 2026-11-24

### Changed

- The app now lists under the Media tab instead of Games.
- When a folder cannot be opened because the console was never unsandboxed (no etaHEN or
  similar daemon answered), the Files screen now says so and suggests running etaHEN or the
  Lapy JB Daemon, instead of just showing an errno.

### Fixed

- The build downloads the ps5-opengl SDK from its new home (the old account was removed).

## [v00.002.000] - 2026-11-23

### Added

- **An experimental video player.** Open a video in the Files screen and it plays, with its sound and subtitles.
  It has been tried on one console with a handful of films; expect rough edges. What it does:
  - **Files:** `.mp4 .m4v .mkv .webm .avi .mov .ts .m2ts .mts .wmv .flv .mpg .mpeg .ogv .3gp`, and any file that
    looks like one.
  - **Picture:** H.264, H.265 (HEVC), VP9, AV1, MPEG-2, MPEG-4 and more, up to 3840 x 2176 (4K).
    - H.264 up to 1080p and H.265 Main and Main 10 up to 4K are decoded by the console's own hardware decoder
      (`libSceVideodec2`).
    - Everything else is decoded in software (FFmpeg and dav1d).
    - On one console a 4K 60 fps HEVC 10-bit film showed 60 pictures a second in most seconds.
  - **HDR:** an HDR10 (PQ) film in full screen is sent to the TV as HDR10, with the control bar and subtitles at
    203 nits white. In a window it is tone-mapped to SDR. Dolby Vision and HDR10+ files show their HDR10 base.
    HDR10 output was checked with test bars; a whole HDR10 film has not been watched yet.
  - **Sound:** AAC, AC-3, E-AC-3, DTS, TrueHD (its 7.1 core; Atmos objects are not decoded), FLAC, Opus, Vorbis,
    MP3 and more.
    - Films with more than two channels play as 5.1 or 7.1 on an eight-channel output, so an AV receiver gets
      every channel. Checked on a 5.1 set; the console mixes down itself for a stereo TV.
    - Other films play in stereo.
  - **Subtitles:** the file's own text tracks (SubRip, WebVTT, ASS as plain text), and `.srt`, `.vtt`, `.ass` or
    `.ssa` files beside the film. Picture subtitles (PGS, VobSub) are not shown.
  - **Controls:**
    - Cross: play and pause.
    - D-pad: left and right seek 10 s, up and down change the volume.
    - L2/R2: one minute back or forward, faster while held.
    - L1/R1: the sound track. Square: the subtitles. Triangle: full screen.
    - Options: a menu with start from the beginning, subtitle delay, sound delay and fill the screen.
    - Circle: back.
  - **Memory:** a film resumes where it was left, and the sound and subtitle languages last picked are chosen
    again for the next film.
  - **Not yet:** chapters, the next episode, films inside archives, Atmos and picture subtitles.
- Films and songs opened in a viewer play at their own volume and no longer also follow the background music
  volume, which made them about 16 dB quieter at its default.
- The app's memory (heap) is 1 GiB when the console gives it, and 128 MiB as before when it does not.
- For developers:
  - `make video` builds FFmpeg, dav1d and zlib from pinned, checked sources (about ten minutes, once); the app
    grows by about 11 MB.
  - `make HW_DECODE=0` builds without the hardware decoder, for a firmware that refuses the library.
  - `tools/console-install.py` and `tools/console-trace.py` put a build on a console over FTP and read the app's
    trace; a probe mode measures memory, texture uploads, conversion, the decoder, the speakers and HDR output
    (`docs/CONSOLE_VALIDATION.md`).

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
