# MalloyStudio — Architecture

This document describes how the subsystems fit together, the data flow from capture to
recording, and the key design decisions made across versions v1–v5.

---

## High-level component map

```
┌─────────────────────────────────────────────────────────────────────┐
│                           MainWindow                                │
│  (menus, dock layout, signal wiring, OutputSettings, undo stack)    │
└────┬─────────────────────────┬──────────────────────┬──────────────┘
     │                         │                      │
     ▼                         ▼                      ▼
┌──────────┐         ┌──────────────────┐     ┌──────────────┐
│  Model   │         │   Capture layer  │     │ Recording    │
│          │◄───────►│                  │     │              │
│SceneCol- │         │CaptureController │     │  Recorder    │
│lection   │         │  ├ DxgiSession   │     │  (ffmpeg     │
│ Scenes   │         │  └ WindowSession │     │   subprocess)│
│ Sources  │         │WasapiCapture     │     └──────┬───────┘
│ Filters  │         └──────────┬───────┘            │
└────┬─────┘                    │ frames              │ pcm
     │ signals                  ▼                     │
     │                 ┌────────────────┐             │
     │                 │ PreviewWidget  │─────────────┘
     │                 │  compositing   │ cachedComposedFrame()
     │                 │  transitions   │
     │                 │  drag-resize   │
     │                 └────────────────┘
     │
     ▼
┌──────────────────────────────────────────┐
│              UI panels                   │
│  ScenesPanel · SourcesPanel              │
│  InspectorPanel · AudioMixerPanel        │
│  ControlsBar · VuMeter                   │
│  MonitorPickerDialog · WindowPickerDialog│
│  OutputSettingsDialog                    │
└──────────────────────────────────────────┘

┌──────────────────────────────────────────┐
│           AudioController                │
│  owns WasapiCapture workers              │
│  per-source reconciliation               │
│  program-bus PCM → Recorder              │
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

Tiny interface with `startCapture()` / `stopCapture()` and signals `frameReady(QImage)` /
`captureError(QString)`. Two concrete implementations:

- **`DxgiCaptureSession`** — wraps a `DxgiCapture` QThread
- **`WindowCaptureSession`** — wraps a `WindowCapture` QThread

The abstract session lets `CaptureController` tests inject fake sessions.

### `DxgiCapture` (QThread worker)

Uses **DXGI Desktop Duplication** (`IDXGIOutputDuplication`). Runs at ~30 fps:
1. `AcquireNextFrame(timeout=33ms)`
2. `QueryInterface<IDXGIResource>` → `ID3D11Texture2D`
3. `CreateTexture2D(STAGING)` → `CopyResource`
4. `Map(D3D11_MAP_READ)` → construct `QImage(Format_ARGB32)`
5. Emit `frameReady`

### `WindowCapture` (QThread worker)

Uses **PrintWindow / BitBlt** to capture a specific HWND at ~30 fps:
1. `IsWindow(hwnd)` — false → emit `windowClosed`, exit
2. `GetClientRect` for dimensions
3. Create a compatible DC + DIB section (`CreateCompatibleBitmap`)
4. Try `PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT)` (works with GPU-rendered apps)
   — fall back to `BitBlt` on failure
5. `GetDIBits` → BGRA bytes → `QImage(Format_ARGB32)` → emit `frameReady`

`PW_RENDERFULLCONTENT` forces the compositor to render GPU content into the DC; this
works for Chrome, VS Code, etc. DRM-protected content still produces a black frame.

### `CaptureController`

Keeps two parallel session maps:

```
QHash<QString, ActiveSession>       m_sessions        // display: key "adapter:output"
QHash<QString, ActiveWindowSession> m_windowSessions  // window:  key "window:0x<8hex>"
```

`reconcile()` is called on every `SceneCollection` signal that could change what needs
capturing. It:
1. Builds `required` (display) and `requiredWindows` (HWND) sets by scanning visible items
2. Stops sessions whose key is no longer in the required set
3. Starts sessions for keys that are required but not yet running

Session creation is injected via `SessionFactory` / `WindowSessionFactory` functors —
this is how tests swap in `FakeCaptureSession` without touching production code.

---

## Audio layer

### `WasapiCapture` (QThread worker)

Captures one WASAPI endpoint (loopback **or** input). All COM objects live on the worker
thread. Output is hard-pinned to **48 kHz / 16-bit / stereo** (s16le). The worker
converts whatever the device delivers (float32 most common, int16/int32 also handled)
via per-sample inline converters.

### `AudioController`

Owns a flat list of `AudioInput` structs and a parallel list of `WasapiCapture*` workers.

**Loopback default** — always the first entry (`id = "loopback:default"`); never removed.

**`reconcileInputs(QStringList activeDeviceIds)`** — called whenever `SceneCollection`
emits `audioInputsChanged`. Adds workers for device IDs in the list that don't yet have
one; stops and removes workers for IDs that are no longer in the list. The loopback
entry is immune to removal.

**`enumerateInputDevices()`** — one-shot COM call (`IMMDeviceEnumerator::EnumAudioEndpoints(eCapture)`)
returning `{deviceId, friendlyName}` pairs. Called to populate the Inspector's audio-device
combo box and to assign friendly names to new reconciled inputs.

**Program bus** — each worker emits `samplesReady(QByteArray pcm)` which `AudioController`
intercepts, applies per-input volume/mute, and re-emits as `mixedSamples(QByteArray)`.
`Recorder` subscribes to `mixedSamples` and writes it to the audio named pipe.

> Note: the current mixing model is additive — when multiple inputs are active, only
> the **loopback** channel's samples flow to the recorder (the architecture for mixing
> multiple streams is deferred to v6). Per-source audio currently controls whether mic
> workers run; the mic stream is connected to the same `mixedSamples` signal but the
> recorder only processes the first emission per tick.

---

## Recording layer

### `OutputSettings`

Header-only POD + QSettings load/save. Not per-project — stored in the Windows Registry
under `HKCU\Software\MalloyStudio\MalloyStudio\output\*`. Defaults match the v4
hard-coded values (1080p / 30fps / libx264 / CRF 23 / AAC 192k / mp4).

### `Recorder`

Two-channel ffmpeg pipeline:

```
PreviewWidget ──[QTimer@fps]──► stdin ──► ffmpeg ──► output file
AudioController ──[mixedSamples]──► named pipe ──► ffmpeg ──►┘
```

**Video channel**: `onTickVideo()` calls `m_frameSupplier()` (= `PreviewWidget::cachedComposedFrame()`),
writes raw BGRA scanlines to ffmpeg's stdin. ffmpeg uses a `-vf scale=W:H` filter when
the requested output resolution differs from the canvas native 1920×1080.

**Audio channel**: a Windows named pipe (`\\.\pipe\malloy_audio_<pid>_<rnd>`) is created
server-side before ffmpeg starts. `PipeAcceptThread` blocks on `ConnectNamedPipe` off
the main thread; once connected, `AudioController::mixedSamples` is wired to write PCM
into the pipe. ffmpeg reads `-f s16le -ar 48000 -ac 2 -i <pipename>`.

`-shortest` ensures ffmpeg finalises the file when the shorter stream ends.

**Cleanup**: `stop()` closes stdin (video EOF), flushes + disconnects the pipe (audio EOF),
waits up to 5 s for ffmpeg to finish, then emits `finished(path, bytes)`.

---

## UI layer

### `PreviewWidget`

The central canvas widget.

**Rendering** — `paintEvent` calls `drawItem()` for each visible item in the current
scene, back-to-front (item 0 is bottom, last item is top). Coordinate systems:

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

### Named-pipe audio transport

ffmpeg cannot read from a Qt `QIODevice` directly; it needs a path. On Windows, named
pipes are addressable via `\\.\pipe\<name>`. The server side is created before ffmpeg
starts; `PipeAcceptThread` blocks on `ConnectNamedPipe` off the main thread to avoid
stalling the UI while ffmpeg opens the pipe.

### OutputSettings as app-global QSettings

Encoder settings are not stored per-project. Rationale: codec choice and bitrate are
hardware-capability decisions (does the machine have a fast enough CPU for x265?) that
don't change per-project. Storing them globally means the user configures once.

### AudioController::reconcileInputs

Rather than AudioController watching SceneCollection directly, MainWindow wires
`SceneCollection::audioInputsChanged → AudioController::reconcileInputs(gatherVisibleAudioIds())`.
This keeps AudioController ignorant of the model layer and makes the data flow explicit.
