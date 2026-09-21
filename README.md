# MalloyStudio

A C++ OBS-style streaming studio built on **Qt 6** and the Windows **DXGI / WASAPI** capture stack.
It composites multiple source layers into a 1920×1080 canvas, lets you add visual filters per layer,
and records to MP4/MKV/MOV via **ffmpeg**.

---

## Feature overview

| Feature | Added |
|---|---|
| Scene management (add / rename / reorder) | v1 |
| Source library with shared references | v1 |
| Drag-resize in preview, snap guides | v2 |
| Undo / redo (snapshot-based) | v2 |
| Display capture (DXGI Desktop Duplication) | v3 |
| Cut / Fade scene transitions | v3 |
| Image, Text, Color Block, Browser sources | v3 |
| WASAPI desktop-audio capture & Audio Mixer | v4 |
| MP4 recording (ffmpeg, video and audio over restricted named pipes) | v4 |
| Window Capture (PrintWindow / BitBlt per HWND) | v5 |
| Source Filters (Crop, Opacity, Color Correction) | v5 |
| Per-source microphone input (model layer) | v5 |
| Encoder / output settings dialog | v5 |
| Hardware encoders (NVENC / QSV / AMF via EncoderRegistry) | v6 |
| RTMP streaming (Twitch / YouTube / Custom) | v6 |
| Replay buffer (in-memory ring + save-on-demand) | v6 |
| **Microphone & Window Capture in the Add menu** | **v7** |
| **`+ Add Microphone` button in the Audio Mixer** | **v7** |
| **Quality presets in Output & Stream Settings** | **v7** |
| **Live bitrate + dropped-frames during streaming** | **v7** |
| **Per-filter enable toggle (Enabled checkbox)** | **v7** |
| **Per-source mute hotkeys** | **v7** |
| **ffmpeg stderr in error dialogs** | **v7** |

### Since v7

| Feature | Notes |
|---|---|
| Camera source | Media Foundation; opens the camera's highest-rate native format up to 1080p, preferring uncompressed over MJPG, and logs the format it runs |
| Windows.Graphics.Capture backend | Alternative to DXGI for display and window capture (`capture/backend` setting), for comparison |
| Studio mode | Preview and program scenes, with the program scene's captures kept running while another is staged |
| Editor timeline and render queue | Clips reference media files; renders run in the background and survive restarts |
| Twitch sign-in | Device-code sign-in fetches the stream key and pushes title and category on Go Live |
| Stream key relay | Opt-in: ffmpeg publishes to a local relay so the key never appears on its command line; rtmps is carried over TLS |
| Replay saves in real time | A saved replay plays at the speed it was captured, with its audio from the first moment |
| Device consent on project load | Cameras, microphones, displays and windows named by an opened project stay off until allowed |
| Untrusted project input | Media paths must be local drive-absolute paths; files over 32 MB, out-of-range ids and values are refused |
| Capture recovery | Display, camera and microphone capture restart by themselves after a UAC prompt, a lock screen or an unplugged device |

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Windows 10 (1903+) or Windows 11 | DXGI Desktop Duplication requires WDDM 2.x |
| Qt 6.5 or later | Widgets module; MinGW-w64 or MSVC 2022 toolchain |
| CMake 3.21+ | Included with Qt installer |
| **ffmpeg.exe** on `PATH` | `winget install Gyan.FFmpeg` or the full build from ffmpeg.org |
| ffprobe.exe on `PATH` (optional) | Ships with ffmpeg; used to read media durations and sizes for the Media library, and each source's length before a render |
| Ninja | Bundled with CMake; or install via `winget install Ninja-build.Ninja` |

> The project is Windows-only. DXGI, WASAPI, PrintWindow, and named-pipe audio
> transport are all Win32 APIs with no cross-platform equivalents planned.

---

## Building

```powershell
# Configure + build (Release by default)
cmake -S . -B build -G Ninja
cmake --build build --config Release

# Or use the provided helper:
.\build.ps1            # build + launch
.\build.ps1 -NoLaunch  # build only
.\build.ps1 -RunTests  # build + run test suite
```

The build script auto-detects Qt at `C:\msys64\mingw64` (MSYS2 default). For a
custom Qt install set `CMAKE_PREFIX_PATH` before running cmake:

```powershell
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.8.0\mingw_64"
cmake -S . -B build -G Ninja
```

### Output

- `build\MalloyStudio.exe` — main application
- Five test executables, run together by `ctest --test-dir build`: `MalloyStudioTests`
  (the model, capture, audio, encoder and UI rules), `MalloyPipeIoTests`,
  `MalloyCaptureLifetimeTests`, `MalloyOutputSettingsTests` and `MalloyCaptureHandoffTests`

---

## Running

Launch `MalloyStudio.exe`. On first run the window is 1280×720; docks can be
rearranged and the layout is persisted in `QSettings` (Windows Registry under
`HKCU\Software\MalloyStudio\MalloyStudio`).

If ffmpeg is not on `PATH`, the Record button is disabled with an explanatory
tooltip and the status bar says ffmpeg was not found; all other features work
normally. Otherwise the status bar shows the version that ffmpeg reports, and
its path on hover.

---

## Project structure

```
src/
├── main.cpp, MainWindow.{h,cpp}   Entry point; top-level window and signal wiring
├── model/        Canvas, Source, SceneItem, FilterEffect, Scene, SceneCollection
│                 (sources, scenes, undo snapshots, JSON I/O, device consent hold)
├── capture/      CaptureController and sessions: DXGI and WGC display capture,
│                 PrintWindow and WGC window capture, Media Foundation camera,
│                 WASAPI audio workers, worker retirement, frame handoff
├── audio/        AudioController (inputs, 50 Hz program-bus mixer), AudioMix, Resampler
├── media/        TimedSource: the frame and PCM source interfaces the encoder reads
├── recording/    EncoderPipeline (ffmpeg over named pipes), Recorder and Streaming
│                 pipelines, MediaController, EncoderRegistry, OutputSettings,
│                 StreamSettings, RtmpKeyRelay, replay ring sources, RenderQueue,
│                 RenderPipeline, TimelineGraphBuilder
├── platform/     TwitchAuth, TwitchApi, CredentialStore, SmartConfig, MachineLoad,
│                 FrameProfile
├── project/      ProjectDocument, MediaPathPolicy, MediaRegistry, ProjectRegistry,
│                 ClipsRegistry, RecentRecordings
├── input/        HotkeyManager (global hotkeys)
└── ui/           PreviewWidget, InspectorPanel, ScenesPanel, SourcesPanel,
                  AudioMixerPanel, ControlsBar, dialogs; shell/ (AppShell, icon rail,
                  status bar); workspaces/ (Recording, Streaming, Editor, Settings,
                  Media and project libraries); dashboard/; components/

tests/
├── model_tests.cpp             QtTest suite for the model, capture, audio, encoder and UI rules
├── pipe_io_tests.cpp           Cancellable named-pipe I/O
├── capture_lifetime_tests.cpp  Capture callback gate lifetime
├── output_settings_tests.cpp   Output settings persistence
└── capture_handoff_tests.cpp   Bounded frame handoff

docs/
├── ARCHITECTURE.md             Subsystem deep-dive and design decisions
├── PROJECT_FORMAT.md           .malloy.json schema reference
├── CHANGELOG.md                Per-version feature list
└── adr/                        Architecture decision records
```

---

## Tests

```powershell
ctest --test-dir build --output-on-failure
```

The suites use fake capture sessions and audio workers, so they need no capture
hardware. Tests that run a real encode skip themselves when ffmpeg is not on `PATH`. They cover, among other things:

- Source sharing, JSON round-trips, v1 to v2 migration and the load-time checks
- Undo/redo, edit sessions and what stays on air across them
- Capture reconciliation, retry after failure, and worker thread retirement
- The audio mixer, input restarts and value bounds
- Encoder arguments, cadence and timestamps, and real short encodes
- The stream key relay, Twitch sign-in against a local stand-in, and the render queue

---

## Known limitations

- **Window capture** captures the client area only; title bar chrome is excluded.
- **DRM-protected windows** (Netflix, Prime Video in browsers) capture as a black frame — Windows OS limitation, same as OBS.
- **No pause/resume** during recording — ffmpeg has no clean pause primitive. Use the Replay Buffer to snapshot a recent slice without stopping.
- **Browser source** is a placeholder (renders a grey box); WebEngineView integration is still deferred.
- **Desktop Audio as a scene source** is not yet available — `AudioController` auto-creates a `loopback:default` mixer strip so desktop audio always reaches recordings, but adding it as a scene-level source needs a JSON schema bump (deferred to v8).
- **Push-to-talk** for microphones requires a low-level keyboard hook (out of scope).

---

## See also

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the subsystems fit together
- [`docs/PROJECT_FORMAT.md`](docs/PROJECT_FORMAT.md) — `.malloy.json` schema reference
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md) — per-version history
