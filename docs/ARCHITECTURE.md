# MalloyStudio — Architecture

This document describes how the subsystems fit together, the data flow from capture to
recording, and the design decisions behind them. The layer sections describe the code as
it is now; sections headed with a version record what that version added.

---

## High-level component map

```
┌─────────────────────────────────────────────────────────────────────┐
│                           MainWindow                                │
│  (shell, workspaces, signal wiring, settings, undo stack)           │
└────┬─────────────────────────┬──────────────────────┬──────────────┘
     │                         │                      │
     ▼                         ▼                      ▼
┌──────────┐         ┌──────────────────┐     ┌──────────────────┐
│  Model   │         │   Capture layer  │     │ MediaController  │
│          │◄───────►│                  │     │  recording       │
│SceneCol- │         │CaptureController │     │  streaming       │
│lection   │         │  ├ display       │     │  replay saves    │
│ Scenes   │         │  │  (DXGI or WGC)│     │ (EncoderPipeline │
│ Sources  │         │  ├ window        │     │  + ffmpeg over   │
│ Filters  │         │  └ camera        │     │  named pipes)    │
└────┬─────┘         └──────────┬───────┘     └──────┬───────────┘
     │ signals                  │ frames             │ frames, pcm
     │                          ▼                    │
     │                 ┌────────────────┐            │
     │                 │ PreviewWidget  │────────────┤ currentFrame()
     │                 │  compositing   │            │
     │                 │  transitions   │            │
     │                 │  replay ring   │            │
     │                 └────────────────┘            │
     ▼                                               │
┌──────────────────────────────────────────┐         │
│              UI panels and workspaces    │         │
│  Dashboard · Recording · Streaming       │         │
│  Editor · Media · Settings               │         │
│  ScenesPanel · SourcesPanel · Inspector  │         │
│  AudioMixerPanel · ControlsBar           │         │
└──────────────────────────────────────────┘         │
                                                     │
┌──────────────────────────────────────────┐         │
│           AudioController                │         │
│  one WasapiCapture worker per input      │─────────┘
│  50 Hz program-bus mixer, replay ring    │  pcmReady
└──────────────────────────────────────────┘
```

---

## Model layer

### `Source`

The **atomic unit of content** — an independently named, typed object stored in the
`SceneCollection` source library. Multiple `SceneItem` layers across any number of scenes
can reference the same `Source`; changing a source's text or monitor assignment immediately
affects every item that references it.

Source types and their meaningful fields:

| Type | Key fields |
|---|---|
| `DisplayCapture` | `adapterIndex`, `outputIndex` |
| `Image` | `imagePath` |
| `Text` | `text`, `color` |
| `ColorBlock` | `color` |
| `Browser` | _(placeholder; no fields used yet)_ |
| `WindowCapture` | `windowHandle` (HWND as `quintptr`), `windowTitle` |
| `AudioInput` | `audioDeviceId` (WASAPI device GUID string) |

Sources are GC-collected: `collectUnusedSources()` in `SceneCollection` removes any
source that has a reference count of zero across all scenes.

### `SceneItem`

A **layer** within a scene. It holds:

- `sourceId` — which `Source` it renders
- `transform` — `QRectF` in canvas coordinates (0,0 to 1920,1080)
- `visible`, `locked`, `selected` flags
- A **filter chain** — `QList<FilterEffect*>` QObject-parented to the item

Items are scene-local; the same `Source` can appear as different items with different
transforms and different filter chains in different scenes.

### `FilterEffect`

Abstract base for per-layer visual filters, with three concrete subclasses:

| Subclass | `apply()` behaviour | Special handling |
|---|---|---|
| `CropFilter` | Copies inner `QRect`, scales back to original size (`Qt::SmoothTransformation`) | None |
| `OpacityFilter` | `apply()` is a **no-op** | `PreviewWidget` accumulates `opacity()` from all `OpacityFilter`s in the chain and calls `painter.setOpacity()` — avoids a full pixel loop |
| `ColorCorrectionFilter` | Builds a 256-entry LUT from brightness + contrast; walks pixels with Rec.601 luma weighting for saturation | Applied at item rect size, not full canvas |

Filters serialize to / from JSON via `toJson()` / `FilterEffect::fromJson()` (factory).
The `"filters"` key is optional in item JSON so older files load cleanly with empty chains.

### `SceneCollection`

The **root model object**. Owns the source library and the list of scenes.

**Undo system** — entirely snapshot-based. Every mutating method:
1. Captures `before = snapshot()` (= `toJson()`)
2. Applies the change
3. Calls `pushSnapshotCommand(label, before, snapshot())`

`SnapshotCommand` stores two `QJsonObject`s and calls `restoreSnapshot()` on undo/redo.
This trades memory for simplicity — works well because projects are small (< 100 KB JSON).

**Edit session** — for operations like text-typing where you want a single undo entry
across many small changes, call `beginEditSession()` / `commitEditSession(label)`. The
session captures `before` at start and pushes one command at end.

**Signals** — `SceneCollection` emits fine-grained signals (`itemChanged`, `sourceChanged`,
`itemVisibilityChanged`, …) that drive UI refresh and `CaptureController::reconcile()`.
`audioInputsChanged` is a dedicated signal for `AudioController::reconcileInputs()`.

---

## Capture layer

### `CaptureSession` (abstract)

Small interface with `startCapture()` / `stopCapture()`, `setDelivering(bool)`, `stats()`
and signals `frameReady(QImage)` / `captureError(QString)`. Three kinds of session exist:

- **display**: `DxgiCaptureSession` (a `DxgiCapture` QThread) or `WgcCaptureSession`
  (Windows.Graphics.Capture), chosen by the `capture/backend` setting through
  `CaptureBackend::effective()`, which falls back to DXGI where WGC is unavailable. The
  two exist to be compared; everything downstream of them is identical.
- **window**: `WindowCaptureSession` (a `WindowCapture` QThread using PrintWindow), or
  WGC under the same setting.
- **camera**: `CameraCaptureSession`, a Media Foundation source reader on its own
  thread.

Every session bounds the frames it has handed over and not yet had taken: DXGI and WGC
allow two in flight, window and camera capture go through a bounded
`CaptureFrameHandoff`. A frame past the bound is dropped and counted, and `stats()`
reports what was produced and dropped. The abstract session is what lets tests inject
fakes.

### `DxgiCapture` (QThread worker)

Uses **DXGI Desktop Duplication** (`IDXGIOutputDuplication`):
1. `AcquireNextFrame(timeout=33ms)`
2. `QueryInterface<IDXGIResource>` → `ID3D11Texture2D`
3. `CopyResource` into a staging texture, `Map(D3D11_MAP_READ)`
4. Copy into a `QImage(Format_ARGB32)` and emit `frameReady`, unless two frames are
   already in flight, in which case the newest is dropped and counted.

`DXGI_ERROR_ACCESS_LOST` (a UAC prompt, the lock screen, a display mode change) ends the
worker with `captureError`; the controller recreates it (see below).

### `WindowCapture` (QThread worker)

Uses **PrintWindow / BitBlt** to capture one HWND at about 30 fps:
1. `IsWindow(hwnd)` false → emit `windowClosed` and exit; nothing else ends the worker
   except a run of failures (step 6)
2. A hidden or minimised window holds its last frame until it is shown again
3. A window whose application is not responding (`IsHungAppWindow`) is skipped, since
   PrintWindow waits on that application and would block the worker
4. `GetClientRect`, a compatible DC and bitmap
5. `PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT)`, falling back to `BitBlt`
6. `GetDIBits` → BGRA → `QImage(Format_ARGB32)` → `frameReady`. A failed capture is
   skipped with the last frame held; about a second of consecutive failures ends the
   worker with `captureError`

Workers are stopped through `retireWorker` (`WorkerRetirement.h`): a worker that does not
stop within its timeout is detached and deletes itself when its thread ends, because Qt
aborts the process if a running `QThread` is destroyed.

### `CaptureController`

Keeps three session maps:

```
QHash<QString, ActiveSession>       m_sessions        // display: key "adapter:output"
QHash<QString, ActiveWindowSession> m_windowSessions  // window:  key "window:0x<8hex>"
QHash<QString, ActiveCameraSession> m_cameraSessions  // camera:  key = device id
```

`reconcile()` runs on every `SceneCollection` signal that could change what needs
capturing, and on program and studio mode changes. It:
1. Collects the sources wanted by the live scenes: the current scene and, in studio
   mode, the program scene, which is what is recorded and streamed
2. Skips any source held for device consent (`SceneCollection::deviceHeld`)
3. Stops sessions no longer wanted and starts the missing ones

A session that fails is stopped and tried again after a delay, from half a second
doubling to ten seconds while it keeps failing; a delivered frame resets the delay, and
removing the source cancels the retry. The exception is a window that no longer exists:
its handle could later name a different window, so it stays off until its source is
removed or pointed at another window. A session is disconnected from the controller
before it is stopped, so a frame it had already queued cannot mark the source live again.
`setDelivering(false)` suspends every session when nothing consumes frames.

Session creation is injected through `SessionFactory` (and
`setCameraSessionFactoryForTesting`), which is how tests use `FakeCaptureSession`.

---

## Audio layer

### `WasapiCapture` (QThread worker)

Captures one WASAPI endpoint (loopback **or** input). All COM objects live on the worker
thread. Output is fixed at **48 kHz / 16-bit / stereo** (s16le): the device's format is
converted per sample and resampled to 48 kHz where the device runs at another rate
(`audio/Resampler.h`). Every failure that ends the worker is reported through
`captureError` with its HRESULT, and a quiet endpoint is polled on each timeout so a
device removed while silent is noticed.

### `AudioController`

Owns a flat list of `AudioInput` structs and a parallel list of `WasapiCapture*` workers.

**Loopback default**: always the first entry (`id = "loopback:default"`); never removed.

**`reconcileInputs(QStringList activeDeviceIds)`**: adds workers for device ids without
one and removes those no longer listed. MainWindow calls it with
`SceneCollection::gatherVisibleAudioIds()` on every structural model signal, not only
`audioInputsChanged`, because most edits that change the set of microphones do not emit
that one.

**Device loss**: an input whose worker reports an error is marked disconnected and
restarted after a delay, from half a second doubling to ten seconds; audio arriving marks
it connected again. Following a change of the default playback device is not implemented.

**Program bus**: each worker's `samplesReady` goes into a per-input byte FIFO. A 50 Hz
timer (`mixAndEmit`) emits one 20 ms tick for every 20 ms of elapsed time since the mixer
started, catching up after a late or stalled fire, because a recording's audio time is
the number of bytes written. Each tick takes exactly 20 ms from each FIFO, applies volume, mute
and balance, sums into int32, runs the optional limiter, clamps to int16 and emits one
chunk through `TimedPcmSource::pcmReady`: silence when nothing is connected. The encoder
and the replay ring both consume that signal. Volume, pan and the limiter threshold are
bounded, and a value that is not a number is replaced with a default.

---

## Recording layer

### `OutputSettings`

Header-only POD + QSettings load/save. Not per-project: stored under
`HKCU\Software\MalloyStudio\MalloyStudio\output\*`. `normalized()` bounds every field
(even dimensions, 1 to 1000 fps, and so on), and every writer is expected to go through it.

### `MediaController`

Owns a `RecorderPipeline` for recording and a `StreamingPipeline` for streaming, both
`EncoderPipeline` subclasses, and runs a one-shot `RecorderPipeline` for each replay save
over snapshots of the preview's frame ring and the controller's PCM ring
(`RingTimedFrameSource`, `RingTimedPcmSource`, which play the snapshot back in real time
by its timestamps). Its destructor stops any of them still running.

### `EncoderPipeline`

Runs ffmpeg with video and audio on two named pipes:

```
PreviewWidget::currentFrame() ──[tick]──► VideoPipeWriter ──► \\.\pipe\malloy_video_… ──► ffmpeg
AudioController::pcmReady ─────────────► AudioPipeWriter ──► \\.\pipe\malloy_audio_… ──►┘
```

**Pipes.** Each is created before ffmpeg starts with an explicit security descriptor
(the system and the current user only), `PIPE_REJECT_REMOTE_CLIENTS`, a single instance,
and a name salted from the system random generator. ffmpeg reads
`-f rawvideo -pix_fmt bgra -s 1920x1080 -framerate <fps> -use_wallclock_as_timestamps 1`
from one and `-f s16le -ar 48000 -ac 2` from the other. A `PipeAcceptThread` per pipe
waits in an overlapped, cancellable `ConnectNamedPipe` (`CancellablePipeIo`); the video
pipe is primed with one frame so ffmpeg's probe does not starve.

**Writers.** `VideoPipeWriter` holds at most three frames; a frame arriving at a full
queue is dropped and counted rather than buffered. `AudioPipeWriter` holds up to 1500
chunks and drops the oldest only past that, since dropped audio deletes time.

**Time.** Frames carry wall-clock timestamps, so a dropped frame leaves a gap instead of
shortening the file. A file uses `-fps_mode vfr` and writes a frame when the composition
sequence has advanced (`Cadence::FollowSource`), and writes the unchanged picture again
when it has gone 100 ms without one (`kStillFloorMs`): ffmpeg stops reading an input that
runs ahead of the others, so a video stream that stopped during a still scene used to take
the audio with it. A stream uses
`-fps_mode cfr -r <fps>` and writes on its own clock, repeating the latest picture
(`Cadence::ConstantRate`), because an ingest expects a steady rate.

**Output.** Every output first goes through `EncoderPipeline::videoFilterArgs`: a `scale`
filter to the output size that converts BGRA to YUV with the BT.709 matrix at limited
range, `setparams` to stamp the frames so, and the matching `-color_*` tags. ffmpeg's
defaults were a BT.601 conversion with no colour description, which players read as
BT.709, shifting saturated colours. The codec arguments then come from `EncoderRegistry`
(see below). `-shortest` is not used.

**Stop.** The writers and acceptors are asked to stop and their pending I/O cancelled,
then joined; the pipes are disconnected, which is ffmpeg's end of input; the pipeline
waits up to 5 s for ffmpeg to finish with the event loop running, and kills it after
that. A one-line summary of the run's capture stages is logged.

---

## UI layer

### `PreviewWidget`

The central canvas widget.

**Rendering** — `paintEvent` calls `drawItem()` for each visible item in the current
scene, from the last item to item 0, so item 0 is drawn last and is on top. Coordinate systems:

- *Canvas coordinates*: 0,0 → 1920,1080 (Source of truth for transforms)
- *Widget coordinates*: letterboxed into the widget, maintaining 16:9 aspect

`canvasRect()` returns the letterboxed destination rect. `widgetToCanvas()` /
`canvasToWidget()` convert between them.

**Filter rendering** — `drawItem()` uses two paths:
- *Fast path* (no filters): calls `drawSourceDirect()` directly into the scene painter
- *Slow path* (filters present): renders source to a `QImage` temp buffer, applies
  non-Opacity filters in chain order, accumulates Opacity values, calls
  `painter.setOpacity(accumulated)`, draws the image

**Frame caching** — two `QHash` caches keyed by `"adapter:output"` and `HWND` hold
the most recent frames from each capture worker. Protected by `m_frameMutex`.
`cachedComposedFrame()` returns the last fully-painted canvas (protected by `m_composedMutex`);
this is what the `Recorder` frame supplier calls at 30+ fps without triggering extra paints.

**Transitions** — `beginFadeTransition(from, durationMs)` starts a `QTimeLine` that
animates `m_transFactor` 0→1. During `paintEvent`, the old frame (`m_transFrom`) is
painted at `1-factor` opacity followed by the new scene at `factor` opacity.

**Drag-resize** — `mousePressEvent` determines `DragMode` based on proximity to item
corners vs. interior. The 12-pixel corner handles are tested against canvas coordinates.
`resizedRect()` computes the new transform during drag, clamped by `MalloyCanvas::clampRect()`
and snapped by `MalloyCanvas::snapRect()`.

### `InspectorPanel`

Shows properties for the currently selected scene item. Sections:
1. **Header** — source name + type label
2. **Toggles** — Visible / Locked
3. **Transform** — X, Y, W, H spinboxes + Fit / Fill / Center / Reset buttons
4. **Source-specific** — rendered inside a `QStackedWidget` keyed by source type:
   - Text → `QLineEdit`
   - ColorBlock → color picker button
   - DisplayCapture → monitor label + "Change Monitor…"
   - Image → path label + "Browse…"
   - WindowCapture → window title label + "Change Window…" → `WindowPickerDialog`
   - AudioInput → device `QComboBox` populated by `AudioController::enumerateInputDevices()`
5. **Filters** — `QGroupBox` with `QListWidget` + Add/Remove/Up/Down toolbar,
   and a `QStackedWidget` for per-filter property pages (Crop, Opacity, Color Correction).

All filter mutations go through `SceneCollection` wrapper methods that push snapshot
undo commands.

---

## Project serialization

Projects are saved as pretty-printed JSON with a `.malloy.json` extension via `QSaveFile`
(atomic write — temporary file + rename). The format is documented in
[`PROJECT_FORMAT.md`](PROJECT_FORMAT.md).

**v1 → v2 migration** is handled transparently in `SceneCollection::loadFromJson`:
- v1 embedded a full source object inside each item; v2 separates the source library from scene items.
- `loadFromJson` detects `"version": 1` (or no version key) and inlines the embedded source.

---

## Key design decisions

### Snapshot-based undo

Every mutation snapshots the full model to JSON before and after and pushes a
`QUndoCommand` containing both blobs. Undo/redo simply calls `loadFromJson`.

**Why**: the model is small (< 100 KB), and this approach lets any operation become
undoable with two lines of code — no per-operation inverse functions. The cost is
O(model_size) per command, which is negligible.

### Source sharing

Sources live in a single library; items hold integer `sourceId` references. This lets
the user add the same camera feed or text to multiple scenes without duplicating data,
and means changing a source property is automatically reflected everywhere.

### CaptureSession injection

`CaptureController` accepts `SessionFactory` and `WindowSessionFactory` functors at
construction. The default factories create real DXGI / Window sessions; tests pass in
`FakeCaptureSession` factories. This avoids any mocking framework.

### OpacityFilter as a no-op

Opacity could be implemented as a pixel loop (`img[i].alpha *= factor`). Instead,
`PreviewWidget` accumulates opacity values from all `OpacityFilter` instances in the chain
and applies the product via `QPainter::setOpacity()`. This leverages hardware alpha
compositing with zero CPU cost.

### Named-pipe media transport

ffmpeg cannot read from a Qt `QIODevice` directly; it needs a path. On Windows, named
pipes are addressable via `\\.\pipe\<name>`. Video used to go to ffmpeg's stdin, which
left the write in `QProcess`'s buffer on the GUI thread; both streams now have a pipe of
their own, served by threads of their own, so a stalled encoder costs frames, counted,
rather than the UI. The pipes are created before ffmpeg starts, and a `PipeAcceptThread`
waits for each connection off the main thread.

### OutputSettings as app-global QSettings

Encoder settings are not stored per-project. Rationale: codec choice and bitrate are
hardware-capability decisions (does the machine have a fast enough CPU for x265?) that
don't change per-project. Storing them globally means the user configures once.

### AudioController::reconcileInputs

Rather than AudioController watching SceneCollection directly, MainWindow calls
`AudioController::reconcileInputs(gatherVisibleAudioIds())` on every structural model
signal. This keeps AudioController ignorant of the model layer and makes the data flow
explicit.

---

## v6 + v7 additions

### EncoderRegistry (v6) + per-encoder streaming tune (v7)

`EncoderRegistry::available()` lazily probes `ffmpeg -encoders` once per process
and returns a list of `Encoder` records: `{id, display, isHardware, buildArgs,
streamingTune}`. Software encoders (libx264, libx265) are always listed even
when ffmpeg isn't present, so the Output Settings dialog is never empty.

Each `Encoder::buildArgs` is a
`std::function<QStringList(const OutputSettings&, EncoderRegistry::Destination)>`
that emits the ffmpeg flags for that codec and for where the output goes. A file
(`Destination::File`) holds a quality target; a stream (`Destination::Stream`) is
held to the configured bitrate. All of them end with `-pix_fmt yuv420p`.

- libx264/libx265 emit `-c:v <codec> -preset <s.preset> -crf <s.crf>`, and a stream
  adds `-maxrate <bitrate>k -bufsize <2×bitrate>k`: CRF with a ceiling.
- NVENC emits `-c:v <codec> -preset <preset, p4 by default>`, then for a stream
  `-rc cbr -b:v <bitrate>k -maxrate <bitrate>k -bufsize <2×bitrate>k` and for a file
  `-rc constqp -qp <s.crf>`.
- QSV/AMF emit `-c:v <codec>`, then for a stream
  `-b:v <bitrate>k -maxrate <bitrate>k -bufsize <2×bitrate>k`; for a file QSV uses
  `-global_quality <s.crf>` and AMF `-rc cqp -qp_i <s.crf> -qp_p <s.crf>`.

`streamingTune` (v7) holds the per-encoder `-tune` value used **only** when
streaming: libx264/libx265 → `zerolatency`, NVENC → `ull`, QSV/AMF → empty
(those encoders reject `-tune` and would EINVAL).

### MediaController (v6)

Facade for recording, streaming, and replay save. Owns one
`RecorderPipeline` and one `StreamingPipeline` (both inherit `EncoderPipeline`).
Surfaces a single `errorOccurred(origin, message)` signal so MainWindow has one
place to show user-facing diagnostics. v7 adds `streamingProgress(kbps, drops)`
re-emitted from the streamer's `EncoderPipeline::progress` signal.

### StreamingPipeline (v6, fixed in v7)

Subclasses `EncoderPipeline::buildOutputArgs` to emit RTMP-flavored args:
`-fps_mode cfr -r <fps>` for the constant rate an ingest expects, the codec
arguments for `Destination::Stream`, `-f flv`, and a forced GOP for live
delivery. v6 hardcoded libx264 flags here — v7 routes through
`EncoderRegistry::find(s.videoCodec)->buildArgs(s)` then appends
`-tune <streamingTune>` (if non-empty) and `-g <fps × keyframeSec>` last
(ffmpeg's last `-g` wins).

### ffmpeg stderr capture (v7)

`EncoderPipeline` now runs ffmpeg in `QProcess::SeparateChannels` mode and
binds `readyReadStandardError` to a ring-trimmed `m_stderrTail` member capped
at ~4 KB. Each stderr line is also fed to
`EncoderPipeline::tryParseProgressLine(line, &kbps, &drops)` which extracts
bitrate and dropped-frame counts from ffmpeg's ~1 Hz progress lines.

`onFfmpegError`, `onFfmpegFinished`, and `onPipeConnectFailed` append the
tail to their `errorOccurred` messages. MainWindow splits the message at
`\n\nLast stderr:\n` and pushes the detail through `QMessageBox::setDetailedText`.

### MicrophonePickerDialog (v7)

Reuses `AudioController::enumerateInputDevices()` (already used by Inspector
combo). Mirrors `MonitorPickerDialog`'s shape. Two entry points open it:

- SourcesPanel → `+` → "Microphone": creates a scene-scoped AudioInput source
  via `SceneCollection::addAudioInputToCurrent(name, deviceId)`.
- AudioMixerPanel → "+ Add Microphone": same action, shorter path.

Both flows fire `SceneCollection::audioInputsChanged`, which MainWindow
forwards to `AudioController::reconcileInputs(gatherVisibleAudioIds())` —
the same reconcile loop v5 introduced. No extra wiring needed.

### FilterEffect::enabled flag (v7)

`FilterEffect` base class gained `bool m_enabled = true`. The base provides
`writeBaseFields()` / `readBaseFields()` helpers so each concrete subclass's
JSON I/O picks up the flag with a single line change. The flag is only
serialized when **false** (keeps existing project files byte-identical until
a user toggles a filter off).

`PreviewWidget::drawItem` skips disabled filters in the chain and excludes
them from the opacity accumulator. `InspectorPanel` adds an "Enabled" checkbox
above the per-filter property page that toggles the flag via a snapshot-based
undo command.

### Per-source mute hotkeys (v7)

`HotkeyManager` already supported arbitrary action IDs. v7 adds the
`audio.mute.<inputId>` convention where `<inputId>` is the existing
`AudioInput::id`. `MainWindow`'s hotkey dispatcher looks up the matching
input and flips its `muted` flag. `HotkeysDialog` enumerates
`AudioController::inputs()` and offers one Mute row per input.

### Streaming progress + ControlsBar stats (v7)

`EncoderPipeline::progress(kbps, drops)` is emitted on each parsed stderr
progress line. `MediaController` re-emits as `streamingProgress` (file
recording doesn't need this). `ControlsBar::setStreamStats` updates a small
secondary label next to the orange `LIVE` timer; it hides cleanly on
`forceStopStreaming`.

### OBS source as read-only reference

`obs-studio-master/` contains a snapshot of OBS Studio used **only** for UX
pattern inspiration (the Add-Source flow, mixer strip layout, settings tree
structure, etc.). No code is copied or linked. The build system never
references that directory; the directory is filesystem-read-only.
