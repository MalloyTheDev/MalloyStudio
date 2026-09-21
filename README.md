# MalloyStudio

MalloyStudio is a Windows desktop application for recording, live streaming and simple
editing, in the style of OBS Studio. It is written in C++20 with Qt 6. It captures displays,
windows, cameras and audio through native Windows APIs, composites them into scenes on a
1920x1080 canvas, and encodes the result with an external ffmpeg for local recording, RTMP
streaming, replay clips and timeline renders.

**Status:** under active development, pre-release. The build reports version 8.0.0
(`project()` in `CMakeLists.txt`); there are no tagged releases or installers. Windows only.

---

## Contents

- [Features](#features)
- [Partial and preview features](#partial-and-preview-features)
- [Known limitations](#known-limitations)
- [Requirements](#requirements)
- [Building and running](#building-and-running)
- [Configuration](#configuration)
- [Where data is stored](#where-data-is-stored)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [License](#license)

---

## Features

### Scenes and sources

- Scenes with layered items over a shared source library: a source can appear in several
  scenes, and edits to it show everywhere it is used.
- Source types: display capture, window capture, camera, image, text, color block,
  audio input, and a browser source (placeholder only, see below).
- Per-layer transform with drag and resize in the preview, snapping, visibility and lock.
- Per-layer filters: crop, opacity, color correction, chroma key, blur and scroll, each with
  its own enable switch.
- Undo and redo for scene edits.
- Studio mode: edit a staged scene while the program scene stays on air, then take it with
  a cut or a fade.

### Capture

- Display capture through DXGI Desktop Duplication (the default) or Windows.Graphics.Capture,
  chosen in Settings, Performance; a machine that cannot run Windows.Graphics.Capture keeps
  Desktop Duplication.
- Window capture of an application's client area with PrintWindow, falling back to BitBlt. A
  hidden or minimised window holds its last frame; a window that closes ends its capture.
- Camera capture through Media Foundation. The camera's native formats are ranked by frame
  rate, then size (up to 1920x1080), then conversion cost, and the negotiated format is logged.
- Capture sessions that fail (after a UAC prompt, the lock screen, a display mode change or an
  unplugged device) are retried with a backoff from 0.5 s to 10 s.
- Capture sessions stop reading frames back while nothing consumes them (no recording, stream,
  replay buffer or visible preview), and resume without restarting when something does.

### Audio

- Desktop audio (WASAPI loopback of the default playback device) is always present in the
  mixer and follows a change of the Windows default playback device.
- Microphones and other capture endpoints as audio inputs, with volume, pan and mute, and
  per-input mute hotkeys.
- Surround endpoints (5.1, 7.1) are downmixed to stereo; 8, 16, 24 and 32-bit PCM and 32-bit
  float are read, and any other sample format is reported rather than recorded as silence.
- A 48 kHz stereo program bus mixed every 20 ms, paced by elapsed time, with a limiter on the
  master bus. Audio capture threads are registered with MMCSS.

### Recording and streaming

- Recording to MP4, MKV or MOV. Video and audio reach ffmpeg over two named pipes restricted to
  the current user and to local clients; frames carry wall-clock timestamps, so a dropped frame
  leaves a gap rather than shortening the file.
- Software encoders (libx264, libx265) and hardware encoders (NVENC, Quick Sync, AMF). A
  hardware encoder is offered only after a short trial encode with it has succeeded on this
  machine; the check runs in the background at startup.
- Recordings and streams are converted and tagged as BT.709 limited range.
- RTMP and RTMPS streaming to Twitch, YouTube or a custom server, at a constant frame rate with
  a fixed keyframe interval.
- Twitch integration: device-code sign-in, fetching the stream key, and setting the stream
  title and category on Go Live (requires your own Twitch application client ID).
- Optional stream key relay: ffmpeg publishes to a local relay, and the key is substituted in
  this process on the way to the ingest, so it never appears on ffmpeg's command line.
- Replay buffer: keeps the last N seconds in memory and saves them to a file on demand.
- A per-run summary line in the log ("capture stages") counting frames produced, composed,
  accepted, written, dropped and repeated, late clock ticks, and dropped audio.

### Editor and render

- Editor timeline of clips that reference media files on disk, with trim, move, speed, gain,
  opacity, scale and position.
- Background render queue that persists across restarts. Clips that run past the end of their
  source are cut there and the job records what was cut.

### Projects and library

- Projects saved as `.malloy.json` files. Opened projects are treated as untrusted input: media
  paths must be local drive-absolute paths, files over 32 MiB and out-of-range source ids are
  refused, item ids and most filter values are corrected, and timeline values are checked
  before a render. [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md) lists every rule.
- Device consent: cameras, microphones, displays and windows named by an opened project stay
  off until allowed. Held sources are marked in the Sources panel and the Inspector, with an
  Allow action per source.
- Media and Projects libraries that scan their folders off the GUI thread, report folders that
  are unavailable, and let them be removed; a Clips library of saved replays with favorites.

### Interface

- Workspaces for Dashboard, Recording, Streaming, Editor, Media, Clips, Projects, Render and
  Settings, a command palette (Ctrl+K), a first-run setup wizard and a dark theme.
- Global hotkeys for record, stream, replay save, studio transition, scene switching (1 to 9)
  and per-input mute. None are bound by default.
- Closing the window while recording or streaming asks first, and stops them cleanly.

---

## Partial and preview features

| Feature | State |
|---|---|
| Browser source | Placeholder: draws a grey box. No web engine is integrated. |
| AI Lab workspace | A preview page describing planned features; none of them is implemented. |
| Windows.Graphics.Capture backend | Works; kept as an alternative to DXGI for comparison. |
| Replay buffer | Saves 5 fps JPEG stills rather than the encoded stream ([#89](https://github.com/MalloyTheDev/MalloyStudio/issues/89)). |
| Rotated monitors (DXGI) | Frames are turned upright; not yet verified on a rotated monitor ([#56](https://github.com/MalloyTheDev/MalloyStudio/issues/56)). |
| Camera format choice | Chosen automatically; no resolution or rate choice in the picker ([#52](https://github.com/MalloyTheDev/MalloyStudio/issues/52)). |

---

## Known limitations

Streaming

- A stream uses the recording's codec, frame rate and resolution; only bitrate and keyframe
  interval come from the stream settings, so a stream can go out as HEVC at 120 fps
  ([#87](https://github.com/MalloyTheDev/MalloyStudio/issues/87)).
- A dropped connection ends the stream; there is no automatic reconnect
  ([#88](https://github.com/MalloyTheDev/MalloyStudio/issues/88)).
- Without the key relay, the stream key is visible on ffmpeg's command line to other processes
  of the same user and to administrators ([#10](https://github.com/MalloyTheDev/MalloyStudio/issues/10)).

Capture

- The mouse pointer is not captured ([#90](https://github.com/MalloyTheDev/MalloyStudio/issues/90)).
- There is no game capture, and window capture does not substitute for it
  ([#33](https://github.com/MalloyTheDev/MalloyStudio/issues/33)).
- Window capture copies the client area only; DRM-protected content captures as black.
- A microphone layer is drawn into the picture as a labelled box, including in recordings
  and streams ([#108](https://github.com/MalloyTheDev/MalloyStudio/issues/108)). Hiding the
  layer is not a workaround: a hidden microphone layer also stops that microphone.

Recording and editing

- No pause and resume during a recording.
- Still images in the editor render as a single frame
  ([#100](https://github.com/MalloyTheDev/MalloyStudio/issues/100)).
- Timeline renders are not converted or tagged as BT.709
  ([#92](https://github.com/MalloyTheDev/MalloyStudio/issues/92)).

Application

- Some storage queries still run on the GUI thread and can stall it on an unreachable network
  drive ([#70](https://github.com/MalloyTheDev/MalloyStudio/issues/70)).
- Some settings pages keep stale copies of settings changed elsewhere
  ([#65](https://github.com/MalloyTheDev/MalloyStudio/issues/65)), and some settings are stored
  but not yet used ([#66](https://github.com/MalloyTheDev/MalloyStudio/issues/66)).

The full list is the [open issue tracker](https://github.com/MalloyTheDev/MalloyStudio/issues).

---

## Requirements

| Requirement | Notes |
|---|---|
| Windows 10 1903 or later, or Windows 11 | Windows.Graphics.Capture needs 1903; DXGI Desktop Duplication needs a WDDM 1.2+ driver. |
| ffmpeg on `PATH` | Required for recording, streaming, replays and renders. Without it Record and Go Live are disabled with a tooltip, the status bar says ffmpeg was not found, and replays and renders fail with an error. Developed against ffmpeg 8.1. |
| ffprobe on `PATH` | Optional. Reads media lengths for the Media library and source lengths before a render; without it renders are not clamped. |
| A GPU driver with NVENC, Quick Sync or AMF | Optional. Used only when its trial encode succeeds. |

To build from source you also need MSYS2 with the MinGW-w64 GCC toolchain and Qt 6.5 or later;
see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

---

## Building and running

From PowerShell in the repository root, with MSYS2 installed at `C:\msys64`:

```powershell
.\build.ps1 -NoLaunch    # configure (first run only, Debug) and build
.\build.ps1 -RunTests    # build, then run the test suite
.\build.ps1 -Deploy      # build, copy the Qt and MinGW runtime next to the exe, then launch
.\build.ps1              # build and launch
```

The application is `build\MalloyStudio.exe`. Run it after `-Deploy`, or from a shell whose
`PATH` starts with `C:\msys64\mingw64\bin`. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) covers
building with plain CMake, the test suite and troubleshooting.

---

## Configuration

Most options are set in the Settings workspace and the Output and Stream Settings dialogs.
A few are worth knowing about:

| What | Where |
|---|---|
| Recording folder | Settings, Recording (stored as `recording/lastDir`; defaults to your Videos folder). |
| Encoder, resolution, frame rate, quality, container, audio codec | Edit, Output Settings, or Settings, Recording. |
| Stream service, custom URL, key, bitrate, keyframe interval | Edit, Stream Settings, or Settings, Streaming. |
| Stream key relay | Settings, Streaming: "Hide the key from the command line". Off by default. |
| Twitch sign-in | Settings, Streaming: paste the client ID of your own application registered at dev.twitch.tv (OAuth type "Device", no secret), then sign in. |
| Global hotkeys | Edit, Hotkeys, or Settings, Hotkeys. None are bound until you set them. |
| Capture backend | Settings, Performance (stored as `capture/backend`: `dxgi`, the default, or `wgc`). |
| Frame timing profile | Set the environment variable `MALLOY_PROFILE_FRAMES=1` for one run, or the setting `profile/frames` to `true`, to log per-stage frame timings once a second. |

Settings are stored with QSettings in the registry under
`HKEY_CURRENT_USER\Software\MalloyStudio\MalloyStudio`.

---

## Where data is stored

| Data | Location |
|---|---|
| Settings, window layout, hotkeys, library folders | Registry: `HKCU\Software\MalloyStudio\MalloyStudio` |
| Stream key | Windows Credential Manager, generic credential `MalloyStudio_StreamKey` |
| Twitch tokens | Windows Credential Manager, generic credential `MalloyStudio_TwitchTokens` |
| Clip list | `%APPDATA%\MalloyStudio\MalloyStudio\clips.json` |
| Render queue | `%APPDATA%\MalloyStudio\MalloyStudio\renderqueue.json` |
| Media probe cache | `%LOCALAPPDATA%\MalloyStudio\MalloyStudio\cache\media-probe-cache.json` |
| Recordings | The recording folder (your Videos folder by default) |
| Replay clips | Your Videos folder, as `replay-<date>-<time>.mp4` |
| Render graphs (temporary) | Your temporary folder |
| Projects | Wherever you save them, as `.malloy.json` |

Nothing is sent anywhere except to the streaming service you configure and, when you sign in,
to Twitch.

---

## Repository layout

```
src/
  main.cpp, MainWindow.*   Entry point and the top-level window that wires everything together
  model/                   Canvas, Source, SceneItem, FilterEffect, Scene, SceneCollection
  capture/                 CaptureController and sessions: DXGI, WGC, window and camera capture,
                           WASAPI capture, frame handoff, worker retirement
  audio/                   AudioController and the program-bus mixer, resampler, stereo downmix
  media/                   The frame and PCM source interfaces the encoders read
  recording/               EncoderPipeline and its recorder and streaming subclasses,
                           MediaController, EncoderRegistry, output and stream settings,
                           stream key relay, render queue, render pipeline, timeline graphs
  platform/                Credential store, Twitch, Smart Config, process tree, frame profile,
                           off-thread helpers
  project/                 Project documents, media path policy, media, clips and projects registries
  input/                   Global hotkeys
  ui/                      Preview, panels, dialogs, shell, workspaces, dashboard, components
tests/                     QtTest suites (see docs/DEVELOPMENT.md)
docs/                      Architecture, project format, changelog, decision records
MalloyStudioJS/            The original HTML prototype, kept for reference
build.ps1                  Build, test, deploy and launch helper
```

---

## Documentation

| Document | Contents |
|---|---|
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Toolchain, building, tests, the validation gate, conventions, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Subsystems, threads, data flow, timing, lifetimes and design decisions |
| [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md) | The `.malloy.json` project format and the rules applied when one is opened |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | Notable changes by version |
| [docs/adr/](docs/adr/) | Architecture decision records |

---

## License

No license has been published for this repository. Until one is, all rights are reserved by the
author, and the code may not be copied, modified or redistributed without permission.
