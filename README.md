# MalloyStudio

A C++ OBS-style streaming studio built on **Qt 6** and the Windows **DXGI / WASAPI** capture stack.
It composites multiple source layers into a 1920×1080 canvas, lets you add visual filters per layer,
and records to MP4/MKV via **ffmpeg**.

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
| Real MP4 recording (ffmpeg stdin + named-pipe audio) | v4 |
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

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Windows 10 (1903+) or Windows 11 | DXGI Desktop Duplication requires WDDM 2.x |
| Qt 6.5 or later | Widgets module; MinGW-w64 or MSVC 2022 toolchain |
| CMake 3.21+ | Included with Qt installer |
| **ffmpeg.exe** on `PATH` | `winget install Gyan.FFmpeg` or the full build from ffmpeg.org |
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
- `build\MalloyStudioTests.exe` — unit test binary (run via `ctest --test-dir build`)

---

## Running

Launch `MalloyStudio.exe`. On first run the window is 1280×720; docks can be
rearranged and the layout is persisted in `QSettings` (Windows Registry under
`HKCU\Software\MalloyStudio`).

If ffmpeg is not on `PATH`, the Record button is disabled with an explanatory
tooltip; all other features work normally.

---

## Project structure

```
src/
├── main.cpp                    Entry point — dark palette + MainWindow
├── MainWindow.{h,cpp}          Top-level window, menus, signal wiring
│
├── model/
│   ├── Canvas.h                Canvas constants (1920×1080) + layout helpers
│   ├── Source.{h,cpp}          Named, typed source (DisplayCapture → AudioInput)
│   ├── SceneItem.{h,cpp}       Per-layer item: transform + filter chain
│   ├── FilterEffect.{h,cpp}    Filter base + CropFilter, OpacityFilter, ColorCorrectionFilter
│   ├── Scene.{h,cpp}           Ordered list of SceneItems, selection tracking
│   └── SceneCollection.{h,cpp} Full model: sources, scenes, undo, JSON I/O
│
├── capture/
│   ├── DxgiCapture.{h,cpp}     DXGI Desktop Duplication worker thread
│   ├── WindowCapture.{h,cpp}   PrintWindow/BitBlt window-capture worker thread
│   ├── WindowCaptureSession.{h,cpp}  CaptureSession wrapper for WindowCapture
│   ├── WasapiCapture.{h,cpp}   WASAPI audio worker thread (loopback + input)
│   ├── MonitorInfo.h            DXGI adapter/output enumeration helper
│   └── CaptureController.{h,cpp}  Reconciles active capture sessions to scene state
│
├── audio/
│   ├── AudioInput.h             POD: id, name, deviceId, volume, muted, peaks
│   └── AudioController.{h,cpp}  Owns WASAPI workers; per-source reconcile; VU bus
│
├── recording/
│   ├── OutputSettings.h         QSettings-backed encoder config (fps/crf/codec…)
│   └── Recorder.{h,cpp}         ffmpeg subprocess + video stdin + audio named pipe
│
├── project/
│   └── ProjectDocument.{h,cpp}  Load/save .malloy.json via QSaveFile
│
└── ui/
    ├── PreviewWidget.{h,cpp}    Compositing canvas, drag-resize, transitions
    ├── InspectorPanel.{h,cpp}   Per-item transform, source props, filter chain UI
    ├── ScenesPanel.{h,cpp}      Scene list with Add/Remove/Rename
    ├── SourcesPanel.{h,cpp}     Source library with type-based Add menu
    ├── AudioMixerPanel.{h,cpp}  Per-input VU strip + volume + mute
    ├── ControlsBar.{h,cpp}      Transition picker + Record button + elapsed timer
    ├── VuMeter.{h,cpp}          Stereo peak bargraph widget
    ├── MonitorPickerDialog.{h,cpp}  DXGI output picker for DisplayCapture
    ├── WindowPickerDialog.{h,cpp}   EnumWindows-based window picker for WindowCapture
    └── OutputSettingsDialog.{h,cpp} Encoder settings form (fps/crf/codec/bitrate…)

tests/
└── model_tests.cpp             QtTest suite (12 tests, run via ctest)

docs/
├── ARCHITECTURE.md             Subsystem deep-dive and design decisions
├── PROJECT_FORMAT.md           .malloy.json schema reference
└── CHANGELOG.md                Per-version feature list
```

---

## Tests

```powershell
ctest --test-dir build --output-on-failure
```

The suite mocks `CaptureSession` to avoid needing real hardware and verifies:

- Source sharing, GC, and JSON round-trips
- v1 → v2 project migration
- Undo/redo + edit-session coalescing
- CaptureController display-session reconciliation
- Filter chain serialization round-trip
- OutputSettings QSettings round-trip
- AudioController per-source reconciliation
- Recorder construction without ffmpeg

---

## Known limitations (v7)

- **Window capture** captures the client area only; title bar chrome is excluded.
- **DRM-protected windows** (Netflix, Prime Video in browsers) capture as a black frame — Windows OS limitation, same as OBS.
- **No pause/resume** during recording — ffmpeg has no clean pause primitive. Use the Replay Buffer to snapshot a recent slice without stopping.
- **Browser source** is a placeholder (renders a grey box); WebEngineView integration is still deferred.
- **Desktop Audio as a scene source** is not yet available — `AudioController` auto-creates a `loopback:default` mixer strip so desktop audio always reaches recordings, but adding it as a scene-level source needs a JSON schema bump (deferred to v8).
- **Push-to-talk** for microphones requires a low-level keyboard hook (out of scope).
- Audio sample-rate conversion is not implemented — devices that deliver at rates other than 48 kHz will produce a one-time warning and may drift.

---

## See also

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the subsystems fit together
- [`docs/PROJECT_FORMAT.md`](docs/PROJECT_FORMAT.md) — `.malloy.json` schema reference
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md) — per-version history
