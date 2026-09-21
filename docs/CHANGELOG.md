# Changelog

All notable changes to MalloyStudio are documented here.

---

## Unreleased

Everything since v7, grouped by area. Commit ids are given where one change is worth
finding again.

### Interface

- The native Qt interface replaces the prototype layout: an icon rail and workspaces
  (Dashboard, Recording, Streaming, Editor, Media, Clips, Projects, Render, Settings),
  a Ctrl+K command palette, a first-run onboarding wizard, a bundled icon set and a dark
  theme.
- The Dashboard, status bar and Smart Config report measured state rather than
  placeholder figures, and Smart Config recommends encoder settings from the detected
  hardware (b186382).
- Settings pages are backed by the real settings, including hotkeys, and a frame rate
  that is configured is shown as configured (06a5f5d).
- The Dashboard's record and live quick actions say Stop Recording and End Stream while
  those are running (1d7fb14).

### Sources and capture

- Camera source through Media Foundation, choosing the camera's highest-rate native
  format up to 1080p (a27c962, 3628eaa, 83c143a).
- A Windows.Graphics.Capture backend beside DXGI for display and window capture, chosen
  by the `capture/backend` setting (174613c).
- Capture stops producing when nothing consumes frames (9239817), bounds the frames in
  flight to the compositor (47dec02, b594ab9), and outlives late callbacks.
- A display, camera or microphone that fails is tried again with a backoff instead of
  staying off: after a UAC prompt, the lock screen or an unplugged device (9d6685d,
  55c39bf, f7e842c). A window capture is too, and a single failed copy no longer ends
  it; an unrelated scene edit no longer restarts a failing camera ahead of its backoff.
- A hidden or minimised window holds its last frame instead of being treated as closed.
- A capture thread that will not stop is detached rather than destroyed, which Qt
  treats as fatal (bfc8a7c); a hung captured window is skipped.

### Audio

- Every input is mixed on a 50 Hz program bus, byte-accurately and in stereo (6013ef8,
  64d9cd7), with capture resampled to 48 kHz (3d211f2).
- Volume, pan and the limiter threshold refuse values that are not numbers (eeb2cc7).
- The mixer emits audio by elapsed time rather than one tick per timer fire, which ran
  2% short and lost stalls, so recorded sound no longer drifts ahead of the picture.

### Recording and streaming

- Video reaches ffmpeg over its own named pipe and thread, as audio does, with the pipes
  restricted to this user and to local clients (1ef24c9, 8b97d77, 3efa8e1).
- Frames carry wall-clock timestamps, so a dropped frame leaves a gap instead of
  shortening the file (bbfa085, da26718); a file follows its source while a stream holds
  its own cadence (e065892, cd3a3ae).
- Per-stage frame telemetry for diagnosing throughput (0bcdf7c, 25fb138).
- Software streams are capped at the configured bitrate (cc98877).
- A recording that has to be killed, a cancelled render and a hung media probe end with
  every process they started, not only a package manager's launcher, and cancelling a
  render no longer blocks the window.
- Recordings and streams are converted with the BT.709 matrix and tagged as BT.709, so
  players no longer shift their colours.
- A recording of a still scene keeps its length and its audio: a file writes the unchanged
  picture again after 100 ms without a frame, so ffmpeg never stops reading the audio.
- Replay saves play in real time, keep their first audio, and finish before their
  sources when the app closes (61a9abc, 9112313); the replay buffer counts as a consumer
  (18dee41).
- Fades start from the composed canvas rather than a screenshot of the preview
  (ebaa167).

### Twitch

- Device-code sign-in fetches the stream key and pushes title and category (95a4fec).
- An opt-in local relay keeps the stream key out of ffmpeg's command line, carries
  rtmps over TLS, and applies backpressure (1ae226c, d4c00ba, 3cbcd06); the key is
  removed from ffmpeg output (69e66b9).
- Sign-out is final even with a refresh in flight, a network failure no longer signs
  the user out, and a cancelled sign-in stays cancelled (1b8a66b, 6227246).

### Editor and render

- The editor timeline is persisted in the project and its clips reference their source
  media (ADR-0001, 7fe0923); render jobs carry their settings and a timeline snapshot
  (ADR-0002, b506592); the timeline is rendered for real (ADR-0003, d2717d2).
- The media library remembers probe results instead of probing every file again at each
  launch, and never opens cloud-only files (3ae6cb1).

### Projects and trust

- Devices named by an opened project are held until the user allows them, per source,
  and the hold survives undo and redo (ad90551, 0a46032, 7e6b395, cd57095).
- Project media paths must be local drive-absolute paths (a60fec6); the numbers and
  addresses a project supplies are bounded before use (bd75eb0, c50879d); restored
  render jobs are validated like new ones (f0e7a7a, 7574924).
- What is on air stays stable through undo, scene removal and staging (83f6df7, 18dee41),
  and an edit session ends with the state it edits (3d9224c).

### Documentation

- README, ARCHITECTURE and PROJECT_FORMAT describe the current code, including the
  project format's trust rules and the corrected layer order (3bfb243, c258ba9,
  60dc5b3).

---

## v7

### Streaming + ffmpeg bug fixes (Tier 1)

- **`StreamingPipeline` now consults `EncoderRegistry`** for per-codec args. Pre-v7
  it hardcoded libx264-only flags (`-preset <s.preset> -tune zerolatency`) so any
  hardware encoder (NVENC / QSV / AMF) tripped EINVAL (-22) on Start Stream.
  Same class of bug we fixed in `RecorderPipeline::buildOutputArgs` in v6, but in
  the streaming path.
- **Per-encoder `streamingTune` field** on `EncoderRegistry::Encoder` — libx264 →
  `zerolatency`, NVENC → `ull` (ultra-low-latency), QSV/AMF → empty (don't
  accept `-tune` at all).
- **`StreamSettings::keyframeSec` plumbing**: was exposed in the UI but silently
  dropped by `MediaController` and the streaming pipeline always emitted a
  hardcoded 2 s GOP. Now properly forwarded via `OutputSettings.keyframeSec`.
- **Forced GOP override** in streaming output args: software encoders' builders
  don't emit `-g`; we always append `-g <fps × keyframeSec>` after the registry
  args so Twitch/YouTube don't terminate streams for missing keyframes.
- **ffmpeg stderr capture** (`EncoderPipeline.m_stderrTail`): the most recent
  ~4 KB of ffmpeg stderr is now appended to `errorOccurred` messages. MainWindow
  splits this into `QMessageBox::setDetailedText` so users see the actual ffmpeg
  diagnostic behind a "Show Details" button instead of just an exit code.
- New tests: `streamingPipelineUsesRegistryNotHardcodedX264`,
  `streamingPipelineHonorsKeyframeSec`,
  `streamingPipelineForcesGopWhenSoftwareEncoderOmitsIt`.

### Microphone source + Window Capture in Add menu (Tier 2)

- **`Microphone` is now in the Sources Add dialog** — picks a WASAPI capture
  endpoint via new `MicrophonePickerDialog` (modelled on `MonitorPickerDialog`)
  and creates a scene-scoped `AudioInput` source. The model layer always
  supported this; the UI gateway was missing.
- **`Window Capture` is also now in the Add dialog** — was missing pre-v7 too;
  pops `WindowPickerDialog::pickWindow` and wires the picked HWND via
  `SceneCollection::setCurrentSourceWindow`.
- **`+ Add Microphone` button in `AudioMixerPanel`** — second entry point for
  the same flow, so users don't have to leave the mixer to add a mic.
- `SourcesPanel` constructor now takes `AudioController*`; `AudioMixerPanel`
  takes `SceneCollection*` for the quick-add button.
- New tests: `addAudioInputFromUiCreatesScopedSource`,
  `addingAudioInputTriggersAudioInputsChanged`,
  `togglingAudioInputVisibilityChangesGatherList`.

### UI polish (Tier 3)

- **Empty-state placeholders** in `ScenesPanel`, `SourcesPanel`, `AudioMixerPanel`
  — friendly grey hint when the panel has nothing in it instead of a blank widget.
- **Drag-to-reorder** in `SourcesPanel` via Qt's `InternalMove` mode, mirrored
  back into `SceneCollection::moveCurrentItem`.
- **F2 rename shortcut** in `SourcesPanel` (matches `ScenesPanel`'s F2).
- **Quality presets in Output Settings** — "1080p60 High Quality", "1080p30
  Balanced", "720p60 Streaming", "720p30 Fast", "4K30 Cinematic". Bundle
  resolution + fps + CRF + bitrate; manually editing any of those fields
  switches the combo back to "Custom".
- **Quality presets in Stream Settings** — Twitch / YouTube / low-latency
  combos for bitrate + keyframe interval; service-recommended bitrate hint
  warns when the user's bitrate exceeds the platform's published cap.
- **Per-filter enable toggle** on `FilterEffect` (new `enabled` field with
  JSON round-trip). Inspector adds an "Enabled" checkbox above the per-filter
  property page. `PreviewWidget::drawItem` skips disabled filters and excludes
  them from the opacity accumulator. Legacy project files (no `enabled` key)
  default to enabled.
- **Live streaming stats in `ControlsBar`** — bitrate (kbps) + dropped frames
  parsed from ffmpeg's ~1 Hz progress lines. Updates next to the `LIVE` timer
  during streaming; hidden when idle.
- New tests: `filterEnabledFlagRoundtripsJson`,
  `streamProgressLineParsesBitrateAndDrops`.

### Per-source mute hotkeys (Tier 4)

- **`audio.mute.<inputId>` action IDs** — `HotkeyManager` already supported
  arbitrary IDs; `MainWindow` now dispatches these to
  `AudioController::setMuted`. Bindings persist in `QSettings`.
- **`HotkeysDialog` lists per-source mute rows** — enumerates the current
  `AudioController::inputs()` and shows one row per input under the
  friendly device name. `loopback:default` shows as "Mute: Desktop Audio".
- New test: `audioMuteActionIdTogglesInput`.

### Behind the scenes

- `StreamingPipeline` no longer `final` so the test suite can subclass it
  through `ProbeStreamingPipeline` (mirrors v6's `ProbeEncoderPipeline`).
- ffmpeg stderr is captured via `QProcess::SeparateChannels` + a
  `readyReadStandardError` slot instead of forwarded straight to the parent
  console.
- `EncoderPipeline::tryParseProgressLine` is a public static helper so the
  ffmpeg progress regex is unit-testable without instantiating a pipeline.

### Out of scope / deferred

- **Desktop Audio as a scene source** — design collision with the auto-loopback
  worker; defer to v8 with a proper `Source::Type::DesktopAudio` enum and JSON v3
  migration. The mixer already shows desktop audio.
- **Pause / Resume during recording** — ffmpeg has no clean pause primitive.
- **Push-to-talk** — requires low-level keyboard hook.
- **SVG icon set / theme pass** — emoji glyphs (▶ ● ■ ⬛) still used in
  `ControlsBar`; defer to a dedicated visual polish release.
- **OBS reference directory** at `obs-studio-master/` is read-only and is
  used purely as design reference, never copied from.

### Test counts

- Total: **33** (was 22 before v7).
- New in v7: 10 across Tiers 1-4.

---

## v5

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
