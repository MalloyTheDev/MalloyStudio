# Changelog

All notable changes to MalloyStudio are documented here.

---

## v5 (current)

### Window Capture

- New source type `WindowCapture` captures a specific application window via HWND.
- `WindowCapture` worker thread uses `PrintWindow(PW_RENDERFULLCONTENT)` (with `BitBlt`
  fallback) + `GetDIBits` to produce a `QImage` at ~30 fps.
- `WindowPickerDialog` lists all visible top-level windows with process names
  (via `QueryFullProcessImageNameW`) for easy selection.
- Inspector shows the current window title and a "Change Window…" button.
- `CaptureController` manages window sessions in a separate `QHash` alongside the
  existing display sessions; existing tests are unaffected.
- Windows that close or become invalid emit `windowClosed` and the item shows a
  placeholder — no crash.

### Source Filters

- New `FilterEffect` hierarchy: `CropFilter`, `OpacityFilter`, `ColorCorrectionFilter`.
- Filter chain stored on `SceneItem` (`QList<FilterEffect*>`, QObject-parented).
- Serialized as an optional `"filters": [...]` array per item — files without it load
  with empty chains (no schema version bump).
- `duplicate()` deep-copies the filter chain via `FilterEffect::clone()`.
- **Rendering**: `PreviewWidget::drawItem()` uses a fast path (direct paint) when the
  chain is empty, or a slow path (render to temp `QImage` → apply filters → blit) when
  not.
- `OpacityFilter::apply()` is a no-op; `PreviewWidget` accumulates opacity values and
  calls `painter.setOpacity()` to avoid a pixel loop.
- `ColorCorrectionFilter` uses a 256-entry LUT for brightness/contrast and a Rec.601
  luma-weighted saturation loop.
- Inspector Filters section: `QGroupBox` with list, Add/Remove/Up/Down toolbar, and
  per-type property pages for Crop (4 spinboxes), Opacity (slider), and
  Color Correction (3 sliders). All edits undo correctly.

### Encoder / Output Settings

- New `OutputSettings` struct (header-only) persisted in `QSettings` (app-wide, not
  per-project).
- Fields: width, height, fps, videoCodec, crf, preset, audioCodec, audioBitratekbps,
  container.
- Defaults match v4 hard-coded values — existing workflows unchanged.
- `OutputSettingsDialog` (Edit → Output Settings…): resolution spinboxes with presets,
  FPS, video codec, CRF slider, preset, audio bitrate, container.
- `Recorder::start()` now accepts `OutputSettings` and drives all ffmpeg arguments from
  it. A `-vf scale=W:H` filter is added when the requested output resolution differs
  from the canvas native 1920×1080.

### Per-Source Audio (AudioInput)

- New source type `AudioInput` links a WASAPI capture device to the Audio Mixer.
- `AudioController::reconcileInputs(QStringList)` dynamically starts/stops
  `WasapiCapture` workers based on the device IDs of visible AudioInput sources in the
  current scene.
- `AudioController::enumerateInputDevices()` enumerates active eCapture endpoints via
  `IMMDeviceEnumerator` and returns `{deviceId, friendlyName}` pairs.
- `SceneCollection::audioInputsChanged` is emitted on scene switch and item visibility
  changes; `MainWindow` wires it to `reconcileInputs`.
- Inspector shows a `QComboBox` populated with available capture devices.

### Other changes

- `CaptureController::windowFrameReady` / `windowFrameCleared` signals wired to
  `PreviewWidget::updateWindowFrame` / `clearWindowFrame` in `MainWindow`.
- Inspector panel wrapped in a `QScrollArea` to accommodate the new Filters section.
- `propsys`, `user32`, and `gdi32` added to both CMake link targets.
- 4 new unit tests: `windowCaptureKeyIsStable`, `filterChainRoundTrips`,
  `outputSettingsRoundtrip`, `perSourceAudioReconcileActivates`.
- Test count: **12** (was 8).

---

## v4

- WASAPI desktop-audio loopback capture (`WasapiCapture` QThread worker).
- `AudioController` owns loopback worker, exposes `mixedSamples(QByteArray pcm)` bus.
- `AudioMixerPanel` with per-input VU meter (`VuMeter`) + volume slider + mute button.
- Real recording pipeline via `Recorder`: ffmpeg subprocess, BGRA video via stdin,
  PCM audio via Windows named pipe, muxed to MP4 (libx264 + AAC).
- `FrameSupplier` abstraction in `Recorder` (enables test injection without real GPU).
- Volume and mute state persisted in `QSettings` per input ID.
- `"audio": {}` reserved key added to project JSON (v2, no version bump) for future routing.

---

## v3

- DXGI Desktop Duplication capture (`DxgiCapture` QThread, `DxgiCaptureSession`).
- `CaptureController` with `SessionFactory` injection and `reconcile()` auto-management.
- `MonitorPickerDialog` for selecting adapter/output via `MonitorInfo` enumeration.
- Image source (renders a static image from disk, cached per path in `PreviewWidget`).
- Text source (QPainter-rendered text).
- Color Block source (solid fill).
- Browser source (placeholder grey box).
- Cut / Fade scene transitions: `PreviewWidget::beginFadeTransition()` + `QTimeLine`.
- Off-screen 1920×1080 frame cache in `PreviewWidget::cachedComposedFrame()` for the
  recorder (avoids extra paint cycles).
- `ControlsBar`: transition type + duration + Record button with elapsed timer.

---

## v2

- Source library separated from scene items; items hold `sourceId` integer references.
- Multiple items can reference the same source (shared source pattern).
- Source GC — `collectUnusedSources()` removes sources with zero references.
- Snapshot-based undo/redo via `QUndoStack` + `SnapshotCommand`.
- Edit-session coalescing for text editing (`beginEditSession` / `commitEditSession`).
- Drag-resize in `PreviewWidget` with corner handles + `DragMode` enum.
- Canvas snap guides (center-snap, edge-snap via `MalloyCanvas::snapRect()`).
- Fit / Fill / Center / Reset transform buttons in `InspectorPanel`.
- Project JSON v2: `"sources"` array + `"scenes"` with `"sourceId"` references.
- Auto-migration of v1 projects (inline source objects → library).

---

## v1

- Initial prototype.
- `SceneCollection`, `Scene`, `SceneItem`, `Source` model.
- `ScenesPanel`, `SourcesPanel`, `InspectorPanel` (read-only).
- `PreviewWidget` basic paint (no drag, no capture, placeholder rendering).
- `ProjectDocument` load/save (`.malloy.json`).
- Project JSON v1: source embedded inside each item object.
