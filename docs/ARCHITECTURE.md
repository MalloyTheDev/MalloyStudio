# MalloyStudio Architecture

This is the architecture reference for MalloyStudio: what the subsystems are, where their
boundaries lie, which thread runs what, how a captured frame and a captured sample become
a file or a live stream, and the rules that keep timing, lifetime and security correct. It
describes the code as it is. Where a limitation matters to the design, the open issue that
tracks it is cited; nothing here describes planned work as if it existed.

Related documents:

- [README.md](../README.md): what the application does and how to use it.
- [docs/DEVELOPMENT.md](DEVELOPMENT.md): building, testing and the development workflow.
- [docs/PROJECT_FORMAT.md](PROJECT_FORMAT.md): the `.malloy.json` project file format.
- [docs/adr/0001-timeline-clip-source-reference.md](adr/0001-timeline-clip-source-reference.md),
  [docs/adr/0002-render-job-contract.md](adr/0002-render-job-contract.md) and
  [docs/adr/0003-render-worker-pipeline.md](adr/0003-render-worker-pipeline.md): the
  decisions behind the timeline and the render queue.
- [docs/CHANGELOG.md](CHANGELOG.md): what changed between releases.

## Contents

1. [System overview](#1-system-overview)
2. [Terminology](#2-terminology)
3. [Threads and processes](#3-threads-and-processes)
4. [Recurring design patterns](#4-recurring-design-patterns)
5. [Composition root and startup](#5-composition-root-and-startup)
6. [Model](#6-model)
7. [Capture](#7-capture)
8. [Audio](#8-audio)
9. [Composition](#9-composition)
10. [Encoding and outputs](#10-encoding-and-outputs)
11. [Rendering the editor timeline](#11-rendering-the-editor-timeline)
12. [Timing and cadence rules](#12-timing-and-cadence-rules)
13. [Lifetime and shutdown](#13-lifetime-and-shutdown)
14. [Persistence](#14-persistence)
15. [Trust boundaries and security-relevant design](#15-trust-boundaries-and-security-relevant-design)
16. [Platform services](#16-platform-services)
17. [UI shell](#17-ui-shell)
18. [Test seams and test suites](#18-test-seams-and-test-suites)
19. [Known architectural limitations](#19-known-architectural-limitations)

---

## 1. System overview

MalloyStudio is a single-process Qt Widgets application for Windows (Qt 6.5 or later),
written in C++20 and built with CMake against MSYS2 MinGW. It captures displays, windows
and cameras, captures desktop audio and microphones through WASAPI, composes scenes onto a
fixed 1920x1080 canvas, and hands the result to `ffmpeg` child processes over named pipes,
which write files or publish to an RTMP ingest. A multi-track editor timeline is rendered by a
separate background queue, also through `ffmpeg`.

### Component map

```
+-------------------------------------------------------------------------------------+
| main.cpp: QApplication, frame profiling switch, theme, EncoderRegistry hardware check |
+-------------------------------------------------------------------------------------+
| MainWindow: composition root. Creates, owns and wires every subsystem below.          |
+-------------------------------------------------------------------------------------+
   |              |               |               |               |              |
   v              v               v               v               v              v
SceneCollection CaptureController AudioController PreviewWidget  MediaController RenderQueue
(scenes,        (one session per  (one WASAPI     (x2: Program   (Recorder-,     (jobs, store,
 sources,        display, window   worker per      and Staged;    Streaming-      RenderPipeline,
 undo, device    and camera in     input, 50 Hz    composes the   Pipeline,       Timeline-
 consent holds)  use)              program bus)    canvas)        replay saves,   GraphBuilder)
                                                                  RtmpKeyRelay)
   |
   +-- ProjectDocument (.malloy.json)      Library registries: ClipsRegistry,
                                           ProjectRegistry, MediaRegistry, RecentRecordings

Platform services: CredentialStore, TwitchAuth, TwitchApi, HotkeyManager, FrameProfile,
SmartConfig, MachineLoad, OffThread, ProcessTree, FfmpegVersion, EncoderRegistry
```

### Frame and sample path at a glance

```
Video                       (from CaptureSession on, everything runs on the GUI thread
                             unless marked "own thread")

DxgiCapture ----+
WgcCapture -----+--> CaptureSession --> CaptureController --+--> PreviewWidget (Staged)
WindowCapture --+    one per display,   starts, stops and   |
CameraCapture --+    window, camera     retries sessions    +--> PreviewWidget (Program)
(worker or system                                                  | composes on demand
 threads; queued                                                   | currentFrame() on each tick
 hand-off)                                                         v
                                             EncoderPipeline: video clock, BGRA conversion
                                                          |
                                             VideoPipeWriter (own thread, <= 3 frames)
                                                          |
                                             \\.\pipe\malloy_video_<pid>_<salt> --> ffmpeg

Audio

WasapiCapture (one thread per input, MMCSS)
      | samplesReady (queued)
      v
AudioController: per-input FIFO (<= 160 ms), 50 Hz mixer on the GUI thread
      | pcmReady (queued)
      v
EncoderPipeline --> AudioPipeWriter (own thread, <= 1500 chunks)
                               |
                  \\.\pipe\malloy_audio_<pid>_<salt> --> ffmpeg --> file, or FLV over RTMP
```

### Source tree

| Path | Contents |
|---|---|
| `src/main.cpp` | Process entry: application identity, profiling switch, theme, hardware encoder check, main window. |
| `src/MainWindow.*` | The composition root (section 5). |
| `src/model/` | `SceneCollection`, `Scene`, `SceneItem`, `Source`, `FilterEffect` and subclasses, `Canvas.h`. |
| `src/capture/` | Capture sessions and backends (DXGI, WGC, PrintWindow, Media Foundation camera), `WasapiCapture`, and the handoff, gate and retirement helpers. |
| `src/audio/` | `AudioController` and the pure mixing, resampling and downmix headers. |
| `src/media/TimedSource.h` | The frame and PCM producer interfaces the encoder consumes. |
| `src/recording/` | `EncoderPipeline` and subclasses, `MediaController`, `RtmpKeyRelay`, `EncoderRegistry`, settings structs, replay ring sources, render queue and graph builder, `FfmpegVersion`. |
| `src/project/` | `ProjectDocument`, library registries, `MediaPathPolicy`. |
| `src/platform/` | Credential storage, Twitch, machine probes, `OffThread`, `ProcessTree`, `FrameProfile`. |
| `src/input/` | `HotkeyManager`. |
| `src/ui/` | Widgets: shell, workspaces, panels, dialogs, `PreviewWidget`. |
| `tests/` | QTest and plain C++ test executables (section 18). |
| `MalloyStudioJS/` | The HTML and JSX design prototype the interface was built from. Not part of the build. |

`CMakeLists.txt` splits the sources into `MALLOY_CORE_SOURCES` (everything outside
`src/ui/` and `MainWindow`) and `MALLOY_UI_SOURCES`. The application links both; the main
test executable links the core sources plus a named subset of UI sources, each listed with
the reason it is under test. Nothing in the core sources includes a UI header, and that
split is what keeps capture, audio, encoding and the model testable without the shell.

---

## 2. Terminology

These terms are used with one meaning throughout this document and the code comments.

| Term | Meaning |
|---|---|
| Canvas | The 1920x1080 coordinate space every layer is placed in (`MalloyCanvas::Width` and `Height`). Composition always happens at this size. |
| Source | A named, typed piece of content in the collection's shared library (`Source`). Several layers can reference one source. |
| Layer | A `SceneItem`: one placement of a source in one scene, with a transform, visibility, lock and filter chain. |
| Current scene | The scene being edited (`SceneCollection::currentIndex()`). |
| Program scene | The scene on air, which is what is recorded and streamed (`programIndex()`). |
| Staged scene | In studio mode, the scene being prepared (`previewIndex()`); outside studio mode it equals the program scene. |
| Studio mode | The mode in which the current scene is staged and only a transition puts it on air. |
| Session | A `CaptureSession`: the GUI-thread object that owns one capture backend for one display, window or camera. |
| Backend | The code that actually acquires frames: `DxgiCapture`, `WgcCapture`, `WindowCapture` or `CameraCapture`. |
| Capture sequence | A backend's count of frames it produced (`CapturedFrame::sourceSequence`). Feeds the SOURCE RX and CAP DROP counters. |
| Composition sequence | The sequence of the picture most recently composed (`TimedFrameSource::compositionSequence()`). It advances when what the canvas would show changes, not when a backend produces a frame. |
| Consumer | Anything that needs composed frames: a recording, a stream, the replay buffer, or a visible preview. |
| Sink | The encode side of a run: `EncoderPipeline`, which owns its own clock. |
| Cadence | How the sink decides a frame is due: `FollowSource` for files, `ConstantRate` for streams. |
| Tick | One firing of the sink's video clock. |
| Still floor | The longest a file goes without a video frame (100 ms); the unchanged picture is written again when it is reached. |
| Repeat | A frame that rewrites the picture already written: the still floor, or a stream holding its rate. |
| Transport | The pipe writer threads and their bounded queues between the sink and ffmpeg. |
| Run | One start-to-stop of an `EncoderPipeline`. |
| Run summary | The one-line `capture stages:` log written when a run stops (section 10.2.11). |
| Held source | A device-backed source loaded from a project file that the user has not yet allowed. |
| Program bus | The mixed 48 kHz stereo s16le audio `AudioController` emits every 20 ms. |

---

## 3. Threads and processes

The GUI thread owns the model and every `QObject` in the pipeline. Worker threads exist for
exactly two reasons: an operating system API that delivers or blocks on its own schedule
(capture, audio, pipe I/O), or a call that can stall for seconds (process launches, folder
listings on network shares, device enumeration). Workers never touch the model. They hand
results to the GUI thread through queued signals or posted calls.

| Thread | Created by | What runs there | Hands results over by |
|---|---|---|---|
| GUI thread | `QApplication` | Model and undo; `CaptureController` and every session's signal forwarding; composition and preview painting; the 50 Hz audio mixer; the encoder's video clock, frame conversion, stderr parsing and stop sequence; `MediaController`; `RtmpKeyRelay` sockets; `RenderQueue`, `RenderPipeline` and registry process management; `HotkeyManager`'s `WM_HOTKEY` filter; Twitch network requests; status bar sampling. | (owner) |
| DXGI capture | `DxgiCaptureSession::startCapture` | `DxgiCapture::run`: acquire, copy and read back one output. One per display session on the DXGI backend. | Queued `frameReady(QImage)` |
| WGC frame pool | Windows (free-threaded frame pool) | `WgcCapture::Impl::onFrameArrived`: readback of each announced frame. | Posted call to `WgcCaptureSession` carrying the `CapturedFrame` |
| Window capture | `WindowCaptureSession::startCapture` | `WindowCapture::run`: `PrintWindow` at about 30 fps. One per window session on the PrintWindow path. | Queued `frameReady(QImage)` |
| Camera read | `CameraCapture::start` (`std::thread`) | `CameraCapture::captureLoop`: Media Foundation source reader. One per camera session. | Queued `frameReady(QImage)` |
| Camera enumeration | `CameraCapture::refreshDevicesAsync` (`std::thread`) | `availableDevices()`. At most one at a time. | Device cache under a mutex, then a posted call to a leaked notifier |
| WASAPI capture | `AudioController::startWorker` | `WasapiCapture::run`, registered with MMCSS as an "Audio" task. One per audio input. | Queued `samplesReady` and `levelsUpdated` |
| Audio notification | Windows audio service | `DefaultPlaybackWatcher::OnDefaultDeviceChanged`. | Posted call to `AudioController` |
| Pipe accept (x2 per run) | `EncoderPipeline::start` | Overlapped `ConnectNamedPipe`; the video one also writes the priming frame. | Queued `connectedOk` / `connectFailed` |
| Video pipe writer | `EncoderPipeline::onVideoPipeConnected` | Writes queued frames into the video pipe. | Counters read when retired |
| Audio pipe writer | `EncoderPipeline::onPipeConnected` | Writes queued PCM into the audio pipe. | Counters read when retired |
| Encoder check | `EncoderRegistry::startHardwareCheck` (`QThread::create`, low priority) | `ffmpeg -encoders` and one trial encode per hardware encoder, once per session. | Posted call to the registry's notifier |
| `OffThread` pool (up to 8) | `platform/OffThread.h` | Folder listings for `ProjectRegistry` and `MediaRegistry`; `QStorageInfo` queries for the status bar and dashboard. | `QFutureWatcher` owned by the requesting object |

Child processes:

| Process | Started by | Lifetime |
|---|---|---|
| `ffmpeg` (encode) | `EncoderPipeline::start` | One per recording, stream or replay save, for the length of the run. |
| `ffmpeg` (render) | `RenderPipeline::launch` | One per render job at a time. |
| `ffprobe` | `MediaRegistry::probeNext`, `RenderPipeline::probeNext` | One at a time per owner, each with a 10 s limit. |
| `ffmpeg -encoders` and trial encodes | `EncoderRegistry` check thread | Sequential, each with a 10 s limit. |
| `ffmpeg -version` | `FfmpegVersion::probe` | Once after startup, 5 s limit. |

Every child that can overrun a limit or must be stopped is ended with `ProcessTree::kill`,
which terminates its descendants too (section 10.10).

Waits that still happen on the GUI thread:

| Wait | Bound |
|---|---|
| Retiring a DXGI, window or WASAPI worker (`retireWorker`) | 4 s, then the worker is cut loose |
| Starting ffmpeg for a run (`waitForStarted`) | 3 s |
| Stopping a run: writer drain, then ffmpeg finalisation | 1 s and 5 s, with a local event loop running, then 1 s after the process tree is killed |
| Cancelling a render or its probe | 2 s after the kill |
| Killing a media probe (`MediaRegistry::killProbe`) | 1 s after the kill (#107) |
| Joining a camera read thread (`CameraCapture::stop`) | None; returns when the blocking read returns |
| Listing the recording folder for the dashboard, and several storage queries | None (#70) |
| Enumerating cameras in one `SourcesPanel` path before the cache is filled | None; seconds on some machines (#99) |

---

## 4. Recurring design patterns

The same few techniques recur across subsystems. Recognising them makes the code easier to
read and to change consistently.

- **Bounded queues that refuse rather than block.** Every hand-off that carries full frames
  or audio has a fixed bound, and a full queue drops and counts rather than growing or
  blocking the producer: two frames in flight per capture backend, three frames in the
  video transport, 1500 chunks (about 30 s) in the audio transport, about 160 ms per audio
  input, 2 MiB in the RTMP relay. An unbounded frame path once grew a stalled compositor
  into a 15 GB process.
- **Blocking work only on threads that exist for it.** The thread that composes and paints
  does not write to a pipe, and hands folder listings, storage queries, trial encodes and
  camera enumeration to other threads. The waits that remain on the GUI thread are listed at
  the end of section 3.
- **Generation tokens against late deliveries.** Queued work can arrive after the state it
  belonged to has gone. Retry and restart timers carry a token (`m_retryTokens`,
  `m_restartTokens`), window and camera sessions carry a generation, `EncoderPipeline`
  carries a run id and checks acceptor identity, `MediaRegistry` and `RenderPipeline` carry
  probe generations, and `TwitchAuth` carries session and flow generations. A delivery whose
  token no longer matches is dropped.
- **Disconnect before stop.** A session or worker is disconnected from its consumer before
  it is stopped, so a frame it had already queued cannot mark a source live again.
- **Retire, never destroy, a running thread.** Qt aborts the process when a running
  `QThread` is destroyed. `retireWorker` (section 7.5) cuts loose a worker that does not stop
  in time instead.
- **Stop before report.** A failing output is stopped first and its error emitted after, so
  an error handler never holds a failed pipeline open.
- **Pure rules, extracted.** Decisions that matter are static or free functions with no
  I/O (`shouldWriteFrame`, `compositionRequired`, `rankNativeFormats`, `downmixToStereo`,
  `TimelineGraphBuilder::build`, `MediaPathPolicy::isAllowed`, and more) so tests exercise the
  rule rather than the plumbing.
- **Resolved executables, killed as trees.** `ffmpeg` and `ffprobe` are located once with
  `QStandardPaths::findExecutable` and started by absolute path, so the Windows process
  search order (application directory, current directory) cannot substitute another binary;
  and they are ended with `ProcessTree::kill`, because a package manager's launcher on `PATH`
  otherwise leaves the real process running.
- **Measured or blank.** Status figures (CPU, memory, disk, bitrate, drop counters, ffmpeg
  version) are read from the source that knows them, and show as unknown rather than as a
  plausible default.

---

## 5. Composition root and startup

**Responsibility.** `main()` configures the process; `MainWindow` constructs every
subsystem, owns it through Qt parent ownership, and wires the signals between them. No
subsystem finds another on its own; each receives what it needs from `MainWindow`, which
is what lets tests assemble subsets.

### Startup sequence

`main()`:

1. Creates `QApplication` and sets organisation and application name `MalloyStudio` (which
   decides where `QSettings` and `QStandardPaths` point) and the application version from
   the CMake project version.
2. Enables frame profiling from `MALLOY_PROFILE_FRAMES` when set, otherwise from the setting
   `profile/frames` (section 16.4).
3. Applies the theme.
4. Starts `EncoderRegistry::startHardwareCheck(EncoderRegistry::systemProbe())` in the
   background (section 10.7).
5. Shows `MainWindow` and enters the event loop.

`MainWindow::MainWindow` then creates, in this order:

1. `SceneCollection` and its `QUndoStack`.
2. `CaptureController` (reconciles immediately; an empty collection starts nothing).
3. `AudioController` (starts the mixer timer, registers for default-device notifications,
   and starts the desktop audio worker).
4. `ClipsRegistry`, `ProjectRegistry`, `MediaRegistry` (the first folder scans are deferred
   to the event loop).
5. `RenderQueue`, which loads its store and starts the next pending job from its
   constructor.
6. `OutputSettings::load()` and `StreamSettings::load()`.
7. `setupUi()`: the two `PreviewWidget`s, the capture panels, `AppShell` and every workspace,
   `TwitchAuth` and `TwitchApi` (one application-wide pair, because Twitch refresh tokens
   are single use), `SettingsWorkspace`, the onboarding overlay and the command palette.
8. `MediaController`, constructed with the Program `PreviewWidget` as its
   `TimedFrameSource` and `AudioController` as its `TimedPcmSource`, and given
   `CaptureController::captureStats` as its capture statistics provider.
9. `HotkeyManager`, connected to `bindingFailed` before `loadBindings()` so a refused
   shortcut at startup is reported.
10. Menus and model signal wiring (`connectModelSignals`).
11. The replay buffer length and output frame rate are pushed to the previews and the
    audio controller.
12. The `ffmpeg -version` probe is posted to run once the event loop is up.
13. A first scene is created (`ensureCurrentScene`) and the undo stack is cleared.

### Key wiring

| From | To | Purpose |
|---|---|---|
| `SceneCollection` signals | `CaptureController::reconcile` (connected in its constructor) | Start and stop capture sessions to match the live scenes. |
| `SceneCollection` structural signals | `AudioController::reconcileInputs(gatherVisibleAudioIds())` | Start and stop microphone workers to match the program scene. Driven by every structural signal, not only `audioInputsChanged`, because most edits that change the set of microphones do not emit that one. |
| `CaptureController` frame and clear signals | Both `PreviewWidget`s | Deliver captured pictures to composition. |
| Program `PreviewWidget::consumerDemandChanged` | `CaptureController::setDelivering` | Suspend capture readback when nothing consumes frames. |
| `MediaController` started, finished and error signals | Program `PreviewWidget::setRecordingActive` / `setStreamingActive` | Tell composition a recording or stream needs frames. |
| `MediaController` progress signals | `ControlsBar`, `StreamingWorkspace`, `StudioStatusBar` | Show measured bitrate, drops and encode rate. |
| `MediaController::errorOccurred` | One non-modal message box per origin | Report failures after the output has stopped. |
| `HotkeyManager::triggered` | The same `ControlsBar` toggles, transition button and mixer calls the UI uses | Global shortcuts take the same path as clicks. |
| `MediaController::replaySaved` | `ClipsRegistry::registerFile` | Index saved replays. |

---

## 6. Model

### 6.1 Responsibility

`SceneCollection` is the document: the shared source library, the scenes, which scene is
current, on air and staged, studio mode, undo, and the device consent state. It is a plain
`QObject` on the GUI thread and knows nothing about capture, audio or encoding; those
subsystems observe its signals.

### 6.2 Key types

- **`Source`**: id (1 to `Source::MaxId`), name, type, and type-specific fields. Types:
  `DisplayCapture` (DXGI adapter and output index), `WindowCapture` (HWND and title),
  `Camera` (Media Foundation symbolic link and name), `AudioInput` (WASAPI device id),
  `Image` (path), `Text` (text and colour), `ColorBlock` (colour), and `Browser` (URL and
  refresh rate, stored only: a browser source is drawn as a placeholder).
- **`SceneItem`** (a layer): its own id, the `sourceId` it renders, a `QRectF` transform in
  canvas coordinates, visible and locked flags, a runtime selection flag, and a filter chain
  of `FilterEffect` children.
- **`Scene`**: a name and an ordered list of layers. Index 0 is the topmost layer; new layers
  are prepended, so they appear on top.
- **`FilterEffect`**: `CropFilter`, `OpacityFilter`, `ColorCorrectionFilter`,
  `ChromaKeyFilter`, `BlurFilter` and `ScrollFilter`, each with an `enabled` flag that is
  written to JSON only when false. `OpacityFilter::apply()` does nothing; composition
  multiplies the enabled opacity values into the painter's opacity instead of walking pixels.
  `ScrollFilter` keeps mutable offset state advanced by wall-clock time on each composition.
- **`MalloyCanvas`** (`model/Canvas.h`): the canvas constants, `clampRect` (minimum layer size
  16, kept inside the canvas) and `snapRect` (12 px snapping to edges and centre lines).

Sources are shared: a layer holds a source id, and changing a source (a camera, a text)
changes every layer that references it. `collectUnusedSources()` deletes sources no layer
references after structural edits and loads.

### 6.3 Program, current and staged scenes

Outside studio mode, `setCurrentIndex` changes both the current and the program scene. In
studio mode it changes the current and staged scene only, and `promotePreviewToProgram`
(the transition button, or the `studio.transition` hotkey) puts the staged scene on air.
Studio mode and the program and preview indices are session state: snapshots do not store
them, and an undo restores the document without changing what is on air.

Two consumers read these differently, and deliberately:

- `CaptureController::reconcile` runs the capture devices of the current scene and, in
  studio mode, of the program scene as well, so staging a scene does not stop the captures
  the live output depends on.
- `gatherVisibleAudioIds()` returns the microphones of visible layers in the program scene
  only, because the program bus is what is recorded.

### 6.4 Signals

Fine-grained signals (`itemAdded`, `itemTransformChanged`, `sourceChanged`, and so on) drive
panels. The structural ones that other subsystems rely on are `itemsChanged`,
`sourcesChanged`, `currentChanged`, `programChanged`, `previewChanged`,
`studioModeChanged`, `collectionReset`, `audioInputsChanged` and `deviceConsentChanged`.
Scene-level item signals are forwarded only for the current scene, and not while a restore
is in progress.

### 6.5 Undo

Undo is snapshot based. A mutating method takes `snapshot()` (the collection's own JSON)
before the change, applies it, and pushes a `SnapshotCommand` holding the before and after
objects; undo and redo restore a snapshot through `loadFromJson` with
`LoadOrigin::Internal`. Any edit becomes undoable without writing an inverse, at the cost of
serialising the collection per command. A command is not pushed while a restore is running,
while recording is disabled, or when the before and after snapshots are identical.

- **Edit sessions** (`beginEditSession`, `commitEditSession`, `cancelEditSession`) turn a run
  of small changes, such as a preview drag or typing, into one step. A command pushed while a
  session is open takes the session's changes so far with it, and the session continues
  from there, so undoing the later session step never undoes the earlier command. A load
  ends any open session, so its snapshot can never be restored into a different document.
- **Edit groups** (`beginEditGroup`, `endEditGroup`) wrap several commands in one
  `QUndoStack` macro, used when a layer is created and then configured.

### 6.6 Device consent holds

A project file names cameras, microphones, monitors and windows, and a file is not evidence
of intent: `ProjectRegistry` lists files from the user's Movies and Documents folders, so a
file dropped there is one click from opening the webcam. When `loadFromJson` runs with
`LoadOrigin::File`, it records every device-backed source (`DisplayCapture`,
`WindowCapture`, `AudioInput`, `Camera`, and any type added later unless it is explicitly
exempted) in `m_heldDeviceSources` before it emits a single signal, because those signals
drive the capture reconciler synchronously.

- `CaptureController::reconcile` skips held sources, and `gatherVisibleAudioIds` omits them.
- `MainWindow::loadProject` asks once for all of them, listing `pendingDeviceRequests()`;
  "No" is the default.
- A single source is released by its Allow button in `SourcesPanel` or `InspectorPanel`, or by
  switching its layer visible (`setCurrentItemVisible(true)`); both call
  `grantDeviceConsent(sourceId)` and release that source alone.
- Undo, redo and a cancelled edit leave the hold untouched, and it is not pruned when a
  held source is deleted, so an undo cannot bring a declined device back unheld. A new
  project clears it.

### 6.7 Failure handling

`loadFromJson` validates before it replaces anything: the `app` marker, a version of 1 or 2,
a `scenes` array, a `sources` array (version 2) whose ids are unique and within range and
whose types are known, and layers that reference declared sources. A refused load changes
nothing. File-origin image paths that `MediaPathPolicy` refuses are dropped with a warning
while the rest of the project loads. Version 1 files, which embedded a source in each layer,
are converted to the shared library on load. The format itself is specified in
[PROJECT_FORMAT.md](PROJECT_FORMAT.md).

### 6.8 Tests

`reusableSourcesShareSettingsAndGc`, `undoRedoAndEditCoalescing`,
`anEditSessionEndsWithTheStateItBeganIn`, `aCommandDuringAnEditSessionKeepsItsOwnUndoStep`,
`undoInStudioModeLeavesTheProgramOnAir`, `loadedProjectHoldsItsDevicesUntilAllowed`,
`fileDevicesAreHeldBeforeTheLoadIsAnnounced`, `undoAndRedoKeepADeclinedDeviceHeld`,
`projectMediaPathsMustBeLocalFiles`, `sourceIdsNearTheTopDoNotOverflow`,
`filterChainRoundTrips` and related cases in `tests/model_tests.cpp`.

---

## 7. Capture

### 7.1 Responsibility and structure

`CaptureController` keeps exactly the capture sessions the live scenes need and nothing
else, restarts the ones that fail, and keeps a cumulative account of what every backend
produced and lost. It owns `CaptureSession` objects; each session owns one backend.

```
CaptureController (GUI thread)
  m_sessions        QHash<"adapter:output", ActiveSession>        display
  m_windowSessions  QHash<"window:0x<hex>", ActiveWindowSession>   window
  m_cameraSessions  QHash<device id, ActiveCameraSession>          camera

CaptureSession (abstract QObject): startCapture, stopCapture, stats, setDelivering,
                                   signals frameReady(QImage), captureError(QString)
  DxgiCaptureSession   -> DxgiCapture   (QThread, Desktop Duplication)
  WgcCaptureSession    -> WgcCapture    (Windows.Graphics.Capture, display or window)
  WindowCaptureSession -> WindowCapture (QThread, PrintWindow)
  CameraCaptureSession -> CameraCapture (std::thread, Media Foundation)
```

The backend for displays and windows is chosen per session from the `capture/backend`
setting through `CaptureBackend::effective()`, which falls back to DXGI (and to
`PrintWindow` for windows) when WGC is unavailable. `configured()` and `effective()` are kept
apart so the interface can say when a request was downgraded. The two display backends
exist to be compared, not as a fallback chain: everything downstream of them is identical,
so a measurement differs in acquisition only. Changing the setting in Settings stops and
reconciles every session at once.

### 7.2 Reconcile

`reconcile()` runs on `currentChanged`, `itemsChanged`, `sourceChanged`, `sourcesChanged`,
`collectionReset`, `programChanged` and `studioModeChanged`, and once at construction. It:

1. Collects the live scenes: the current scene and, if different, the program scene.
2. Builds the required key sets from their visible layers, skipping sources without a
   configured device and sources held for consent.
3. Stops sessions no longer required, and drops the blocked state, pending retry and backoff
   of keys no longer required.
4. Starts required sessions that are neither running nor blocked.
5. Publishes a summary: `Idle`, `Capture error` (nothing running while a required display is
   blocked), or `LIVE: N sources`. Per-display status (`Starting`, `Live`, `Idle`,
   `Error: <message> (retrying)`) is shown in the Inspector.

A session is always disconnected from the controller before it is stopped, then its final
statistics are added to the retired totals and it is deleted later. Frames it had already
queued therefore cannot mark a source live again or restore a picture that was just cleared.

### 7.3 Retry with backoff, and blocked keys

A failed display, window or camera session is stopped and its key blocked; `scheduleRetry`
unblocks it after a delay and reconciles. The delay starts at 500 ms and doubles to a
10 s ceiling while the source keeps failing, and a delivered frame resets it. Each retry
carries a token, so a superseded or cancelled retry does nothing. The reasons:

- Desktop Duplication loses access every time the secure desktop shows (UAC, the lock
  screen, Ctrl+Alt+Del) or the display mode changes, and recreating the duplication is the
  documented recovery. Blocking it for good left a recording without its screen until the
  user happened to toggle the source.
- A camera comes back when it is plugged in again or another application releases it.
- A window capture reports only a run of failures, which can pass.

The exception is a window that no longer exists (`IsWindow` is false): its handle names
nothing now and could later name another application's window, so its key stays blocked
until its source is removed or pointed at another window.

Display error text is kept in the per-display status; window and camera error text is
discarded, and a failure that can never succeed (an unsupported camera format) is retried on
the same schedule as a transient one (#96).

### 7.4 Backends

**`DxgiCapture` (Desktop Duplication).** On its thread it creates a D3D11 device on the
source's adapter, duplicates the output, and loops on `AcquireNextFrame` with a 33 ms
timeout. A frame is read back only if it was actually presented, a consumer wants frames,
and fewer than two frames are in flight; the checks happen before the GPU copy, so a frame
that would be dropped costs nothing. The readback is `CopyResource` into a staging texture,
`Map`, and a row copy into a `QImage::Format_ARGB32` (BGRA in memory on little-endian x86).
When `DXGI_OUTDUPL_DESC.Rotation` reports a rotated monitor, the frame is turned upright with
an exact quarter-turn transform inside the timed readback, because duplication returns the
panel's unrotated scan-out; the rotation cannot change during a worker's life because
changing it loses access. The rotation direction is unit tested, and #56 stays open until it
is verified on a physically rotated monitor. `DXGI_ERROR_ACCESS_LOST` and any other acquire
failure end the worker with `captureError`, and the controller's retry recreates it.

**`WgcCapture` (Windows.Graphics.Capture).** Available when `GraphicsCaptureSession` reports
support and the free-threaded frame pool exists. It captures a monitor (translated from the
DXGI adapter and output pair by `monitorHandleFor`, so both backends name the same screen)
or a window. The frame pool is free-threaded, so `FrameArrived` is announced on a system
thread rather than polled, and no message loop is needed for frames. Frames are read back to
a `QImage` exactly as DXGI does, so the two backends differ only in acquisition. Cursor
capture and the capture border are turned off where supported, to match DXGI. A change of
content size is a state transition (`Running -> RecreatingPool -> Running`) handled after
the frame is delivered, not an error. A failed readback with a removed device ends the
session with an error. `SystemRelativeTime` is converted to the steady clock and checked
once per session; a stamp more than 5 s from now is rejected in favour of arrival time.
The `Closed` event (window closed, monitor removed) is delivered through the starting
thread's apartment, which is the GUI thread, and is reported as an error so the controller
reconsiders. `stop()` closes the callback gate (section 7.5) before revoking events and
releasing resources.

**`WindowCapture` (`PrintWindow`).** One window at about 30 fps (33 ms sleep per iteration):

1. A window that no longer exists emits `windowClosed` and ends the worker.
2. A hidden or minimised window holds its last frame until it is shown again.
3. A window whose application is hung (`IsHungAppWindow`) is skipped, because
   `PrintWindow` is serviced by that application and would block the worker.
4. The client area is drawn with `PrintWindow(PW_RENDERFULLCONTENT)`, falling back to
   `BitBlt`, and read with `GetDIBits` as top-down BGRA.
5. A failed copy holds the last frame; about a second of consecutive failures (30) ends the
   worker with `captureError`, and the controller retries if the window still exists.

It captures the client area only, and DRM-protected content renders black. It allocates and
copies three full frames per captured frame (#91). No capture path draws the mouse pointer
(#90). There is no game capture source, and window capture cannot substitute for one: a game
in exclusive fullscreen is not composited, and many swapchains read back black (#33).

**`CameraCapture` (Media Foundation).** `start()` spawns a read thread that initialises COM
and Media Foundation, opens the device by its symbolic link, and creates a source reader
with video processing enabled. It then chooses which of the camera's own formats to run:

- `rankNativeFormats` orders the native formats by frame rate (rounded, so 59.94 and 60 tie),
  then frame area, then conversion cost to RGB32 (RGB32 or ARGB32, then uncompressed YUV and
  RGB24, then MJPG, then unrecognised), then the device's order. Formats larger than
  1920x1080 or without a size are excluded.
- `negotiateFormat` tries them in order, then the device default. A subtype the reader could
  not convert is not retried at another size; a mode the device refused says nothing about
  its others. Each attempt is checked by reading back the reader's output type, which must be
  RGB32 with sane dimensions and stride; without that check a YUY2 or MJPG buffer would be
  wrapped as RGB32 and the source would stay black with no error.
- The chosen format is logged once, and a mid-stream format change re-reads the size and
  stride or ends the session with an error.

Frames are copied out of the locked buffer (flipped if bottom-up) after checking the buffer
is as long as the stride and height require. The reader still converts every frame to RGB32,
the picker offers no resolution or rate choice, and #52 stays open for those and for
verification on a real 60 fps camera. A read error or end of stream ends the loop with
`error`, and the controller retries.

Device enumeration takes seconds on some machines (about 3.5 s measured with no camera
attached), so the UI reads `cachedDevices()` and calls `refreshDevicesAsync(context, done)`.
At most one enumeration thread runs; requests made while it runs are answered by its result,
once per context, and a destroyed context never receives a callback. The thread is joinable
rather than detached and is joined as the application object is destroyed
(`qAddPostRoutine`), so it never writes the cache after static destruction. Some UI paths
still enumerate on the GUI thread, and enumeration does not initialise COM on its worker
(#99).

### 7.5 Bounded handoff, callback gate and worker retirement

Every backend bounds the frames it has handed over and not yet had taken. At about 8 MB a
frame, an unbounded path turns a stalled consumer into gigabytes of queued images; a bound
turns it into counted drops instead.

- **DXGI** keeps a shared `std::atomic<int>` in-flight counter, incremented when a frame is
  emitted and decremented by `DxgiCaptureSession` when the queued frame is taken off the GUI
  thread's event queue. The counter is shared so a queued event cannot dangle if the
  backend is destroyed first.
- **WGC** puts a release token in `CapturedFrame::inFlightSlot` and posts the whole frame to
  the GUI thread, so the slot is released when the delivery has run or has been discarded.
- **Window and camera** use `CaptureFrameHandoff` (`kMaxInFlightFrames = 2`). `tryAcquire`
  assigns the capture sequence and reserves a slot before any copy is made, and
  `imageForDelivery` wraps the image so the slot is released only when the last copy of the
  delivered `QImage` is destroyed. The bound therefore covers queued deliveries and the image
  a consumer keeps: one displayed and one pending.

All three report `CaptureStats{framesProduced, framesDropped}` in the same shape (SOURCE RX
and CAP DROP). `CaptureController::captureStats()` sums running sessions with the totals of
sessions already stopped and is never reset; consumers subtract a reading taken at their
start.

`setDelivering(false)` suspends readback when nothing consumes frames. It is not a stop:
sessions keep their devices, handles and statistics, DXGI and WGC still acquire and release
frames so the duplication and the pool stay healthy, and resuming is not a new epoch. With
the window minimised and nothing recording, capture had been spending about a fifth of a
core on readbacks nobody used.

`CaptureCallbackGate<Target>` makes WinRT event sinks safe against teardown. Each WGC
session creates a new gate; the frame and closed sinks hold it by `shared_ptr`, `invoke`
runs callbacks under a mutex while the gate is open, and `close()` (called from the owning
thread, never from a callback) marks it closed and waits for an admitted callback to return.
A handler Windows dispatched just before teardown therefore finds the gate closed instead of
a freed `Impl`, and frame and closed callbacks are serialised with each other.

`retireWorker(worker, timeoutMs, lastLook)` (`capture/WorkerRetirement.h`) stops a `QThread`
worker: it requests a stop, waits, lets the caller read final counters, and deletes the
worker if it stopped. A worker that did not stop in time, typically blocked in a call someone
else services (`PrintWindow` on a hung application, a driver hanging during activation), is
disconnected from everything, unparented, and deleted when its thread finally ends; if it
never ends, it is reclaimed with the process. DXGI, window and WASAPI workers are retired
this way with a 4 s wait. Their stop flags are armed from construction, so a stop requested
before `run()` begins is honoured. The camera read thread is a `std::thread` joined by
`CameraCapture::stop()` without a time limit.

### 7.6 Test seams and tests

`CaptureController(SceneCollection*, SessionFactory, QObject*)`,
`setWindowSessionFactoryForTesting` and `setCameraSessionFactoryForTesting` replace session
creation, and tests use `FakeCaptureSession`. The pure pieces are tested directly:
`DxgiCapture::clockwiseDegreesFor` and `upright`, `WgcCapture::normaliseBackendTime`,
`CaptureBackend::parse`, `CameraCapture::rankNativeFormats`, `negotiateFormat`,
`pickerLabels` and `setEnumeratorForTesting`. Relevant cases include
`captureControllerReconcilesVisibleDisplaySources`, `aLostDisplayCaptureIsTriedAgain`,
`aCameraThatStopsIsTriedAgain`, `aCameraWaitingToRetryIsLeftToItsBackoff`,
`aWindowCaptureThatFailsIsTriedAgain`, `aFrameQueuedBeforeAStopIsDropped`,
`stagingASceneKeepsTheProgramCaptureRunning`, `captureStatsAccumulateAcrossSessionChurn`,
`aRotatedMonitorIsCapturedUpright`, `aHiddenWindowIsHeldRatherThanClosed`,
`aWorkerThatWillNotStopIsCutLooseNotDestroyed` and `cameraEnumerationRunsOneAtATime`.
`MalloyCaptureHandoffTests` covers `CaptureFrameHandoff`, and `MalloyCaptureLifetimeTests`
covers `CaptureCallbackGate`.

---

## 8. Audio

### 8.1 Responsibility

`AudioController` owns one `WasapiCapture` worker per audio input, mixes them into the
program bus, applies per-input volume, balance and mute and an optional master limiter,
publishes meter levels, and keeps the replay PCM ring. It is a `TimedPcmSource`, so the
encoder subscribes to it directly. It knows nothing about the model: `MainWindow` tells it
which microphones the program scene wants.

### 8.2 Bus format

Every input is converted on its capture thread to the bus format: 48 kHz, 16-bit, stereo,
interleaved little-endian (s16le). The mixer and the encoder never negotiate formats.

### 8.3 `WasapiCapture`

One thread per endpoint, with every COM object on that thread:

- Desktop audio is shared-mode loopback on the console default render endpoint; a
  microphone opens its capture endpoint by device id. Both are event driven with a 200 ms
  buffer.
- `describeDeviceFormat` (`audio/StereoDownmix.h`) reads the mix format. 8-bit unsigned,
  16-bit, packed 24-bit, 32-bit integer (including 24-in-32) PCM and 32-bit float are read;
  anything else is refused through `captureError` before the stream starts, instead of being
  recorded as silence while the input looked connected.
- `downmixToStereo` folds every channel into the pair by its `dwChannelMask` position,
  following ITU-R BS.775: centre and surrounds at -3 dB, LFE omitted, positions BS.775 does
  not cover following ffmpeg's rematrixing. Mono goes to both sides at full level, and a
  device without speaker positions takes its first two channels as left and right. It
  returns the packet's peaks for the meters.
- `StereoResampler` (`audio/Resampler.h`) converts any other device rate to 48 kHz with
  Catmull-Rom interpolation, preceded by a second-order low-pass when downsampling. It
  carries state across packets, and a device already at 48 kHz is copied straight through.
- The thread registers with MMCSS as an "Audio" task so it is scheduled ahead of ordinary
  work, including ffmpeg; a failed registration is logged and capture continues.
- `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` (sound lost because the endpoint overran) is
  logged on first occurrence and totalled at exit; the first packet of a stream is exempt.
- Every failure that ends the thread (device lost, invalidated, read or wait failure) emits
  `captureError` with its HRESULT. A silent endpoint signals nothing, so each 200 ms wait
  timeout asks `GetNextPacketSize`, which fails for a removed device.

### 8.4 Inputs and reconcile

`m_inputs` and `m_workers` are parallel lists. The first input is always
`loopback:default` ("Desktop Audio") and is never removed. `reconcileInputs(deviceIds)` adds
an `input:<deviceId>` input and worker for each wanted microphone not yet present, naming it
from a single endpoint enumeration, and removes inputs no longer wanted along with their
FIFO, pending restart and backoff. Per-input volume (0 to 1.5), pan (-1 to 1) and mute are
persisted under `audio/inputs/<id>/` in `QSettings`, and every value read back or set is
passed through `boundedControl`, which replaces a NaN with a default: a NaN gain reaching the
float-to-integer conversion turned the mix into noise or silence.

### 8.5 Mixer

A `Qt::PreciseTimer` fires every 20 ms on the GUI thread. `mixAndEmit` computes how many
20 ms ticks are due from a `QElapsedTimer` started with the controller and emits that many,
not one per fire: fires come late, a stalled GUI thread skips them, and Qt does not replay
missed ones, and emitting one per fire produced 2 percent less audio than wall time (about
1.2 s per minute of drift against the picture). More than 500 ticks behind (10 s), the
backlog is dropped with a warning and the clock resynchronised.

Each tick:

1. Takes exactly 960 frames (3840 bytes) from each unmuted input's `PcmFifo`, keeping any
   remainder for the next tick; a short FIFO contributes what it has. A muted input's FIFO is
   cleared.
2. Applies the balance law (`balanceGains`: pan attenuates the opposite side and never folds
   one channel into the other, because every input is already stereo) and volume, and sums
   left into left and right into right in `int32` (`mixStereoInto`).
3. Applies the limiter when enabled: instant attack to the gain that brings the tick's peak
   to the threshold (bounded to -24 to 0 dB), release of 1/60 per tick.
4. Clamps to `int16` and emits `pcmReady`, as silence when there are no inputs, because
   ffmpeg's audio input must see steady bytes.

Worker chunks are appended to the per-input FIFO on arrival and trimmed from the oldest end
past 30,720 bytes (160 ms), so a stalled mixer keeps the mix near live. Chunks that a removed
input's worker had already queued are discarded rather than recreating its FIFO.

`pcmReady` is consumed by every running `EncoderPipeline` (queued) and by the replay ring,
which stores each chunk with a wall-clock timestamp and trims to the configured span.

### 8.6 Device loss and default device changes

A worker's `captureError` marks its input disconnected and schedules a restart with the same
500 ms to 10 s doubling backoff and token guard as capture; audio arriving marks it
connected again and resets the delay. Error text is discarded, and a permanent refusal is
retried like a transient failure (#96).

Desktop audio follows the Windows default playback device. The controller registers a
`DefaultPlaybackWatcher` (`IMMNotificationClient`) before the loopback worker first starts.
Windows calls it on a thread of its own; it ignores everything except the console default of
the render flow and posts the device id to the controller, which restarts every loopback
input that opens the default (a pending failure restart is superseded) and ignores a repeat
of the device it already follows. The watcher is reference counted because Windows holds its
own reference and may be mid-call during teardown; `stopFollowingDefaultPlayback()` runs
first in the destructor, detaches the watcher under its mutex (so nothing more is posted) and
then unregisters it.

### 8.7 Tests

`setWorkerFactoryForTesting` (tests use `FakeWasapiWorker`),
`defaultDeviceNotificationForTesting`, `followsDefaultPlaybackDeviceForTesting` and
`bufferedBytesForTesting` are the seams; `AudioMix.h`, `StereoDownmix.h` and `Resampler.h`
are pure headers tested directly. Cases include `theMixerKeepsTimeWithTheWallClock`,
`aMicrophoneThatFailsIsStartedAgain`, `desktopAudioFollowsTheDefaultPlaybackDevice`,
`audioFromARemovedInputIsNotKept`, `mixerKeepsStereoSeparation`,
`aSurroundDeviceIsFoldedIntoStereo`, `deviceSampleFormatsAreReadOrRefused`,
`resamplerPreservesPitchAcrossRates` and `pcmFifoKeepsTheSampleStreamContinuous`. Some audio
tests still touch persisted settings and real desktop capture (#98).

---

## 9. Composition

### 9.1 Responsibility

`PreviewWidget` composes a scene onto the canvas and shows it. Two instances exist:

| Role | Composes | Consumers | Notes |
|---|---|---|---|
| `Program` | the program scene | the encoder (it is the `TimedFrameSource`), the replay ring, its own painting | Read-only while studio mode is on. Its consumer demand drives capture delivery. |
| `Staged` | the current scene | its own painting | Visible only in studio mode. Never told about recordings or streams. |

Composition runs on the GUI thread. Painting is a consumer of the composed frame, not its
producer: composition used to be a side effect of `paintEvent`, so minimising the window
stopped it and a running recording silently held its last picture for as long as the window
stayed down (#38, closed).

### 9.2 Inputs

`CaptureController`'s frame and clear signals feed three frame caches (display frames by
`"adapter:output"`, window frames by HWND, camera frames by device id) under `m_frameMutex`.
Image sources are loaded synchronously into a path-keyed cache on first use, which is cleared
on `collectionReset`.

### 9.3 Composition sequence and the composition rule

Two counters decide when to compose:

- `m_contentSequence` is bumped by `markContentChanged()` whenever what the canvas would show
  may have changed: a captured frame arriving or being cleared, `itemsChanged`, a change of
  current, program or preview scene, a collection reset, a transition step, an animation
  tick, or the replay buffer being enabled. Selection changes do not bump it, because
  selection handles are drawn in widget coordinates and never reach the composed frame.
- `m_composedSequence` is set to the content sequence each time a composition is published.
  It is what `compositionSequence()` returns to the encoder.

The composition sequence is not the capture sequence. A composition can fold several
captured frames together, or happen with no capture at all because a layer moved; and any
captured frame the widget receives advances the content sequence, even from a source the
composed scene does not show. The encoder wants the
composition sequence, because it asks whether the encoded picture would differ. Capture
diagnostics want the capture sequence, which stays inside the backends and their counters.

`markContentChanged()` schedules one queued `composeIfNeeded()` per pass of the event loop,
so a burst of arriving frames composes once. That call composes only when the pure rule
`compositionRequired(recording, streaming, previewVisible, contentAdvanced, replayActive)`
holds: the content advanced, and something consumes it. `previewVisible()` is false when the
widget is hidden or its top-level window is minimised; the widget installs an event filter on
its window to notice `WindowStateChange`, because minimising stops paint events without
hiding the widget. `paintEvent` composes first if the cached frame is stale, since being asked
to paint proves someone is looking.

`consumersPresent()` is the same question without the content test. The Program instance
emits `consumerDemandChanged` when it changes, and `MainWindow` connects that to
`CaptureController::setDelivering`. The replay buffer counts as a consumer, because it is
used precisely while the window is minimised.

### 9.4 Drawing

`composeNow()` allocates a 1920x1080 `Format_ARGB32_Premultiplied` image, fills it black,
and draws the scene's visible layers from the last to index 0, so index 0 is on top. For each
layer:

- With no filters, the source is drawn straight into the canvas at its transform.
- With filters, the source is drawn into a temporary image at the layer's size; enabled
  non-opacity filters are applied in chain order (a `ScrollFilter` is advanced by wall-clock
  time first); the product of enabled opacity values becomes the painter opacity; and the
  image is drawn at the transform.

During a fade the previous frame is drawn over the result at `1 - factor` opacity, in canvas
coordinates, so the transition is recorded. The result is published under `m_composedMutex`
(`cachedComposedFrame()` and `currentFrame()` return it), the composition sequence is set,
and `composedFrameCount()` is incremented.

Display, window and camera sources without a frame draw a labelled placeholder. Audio sources
draw an indicator. Browser sources draw a placeholder.

### 9.5 Animation clock

A `ScrollFilter` with a non-zero speed changes the picture on its own, while nothing else marks
the content changed. When a composition draws such content, a `Qt::PreciseTimer` starts at the
output frame rate (`setOutputFrameRate`, interval `1000 / fps` rounded down so the picture
changes at least as often as the output samples it) and marks the content changed on each
tick. It is not restarted by later compositions, so fast captured frames cannot keep pushing
its next tick back. It stops when a composition draws nothing that moves, or on its next tick
when no consumer is present. Before this clock, a ticker on an otherwise still scene held one
picture until something unrelated recomposed it (#68, closed).

### 9.6 Transitions

A fade (`beginFadeTransition`) uses a `QTimeLine` with an in-out sine curve; each step marks
the content changed. The starting picture is `renderCurrentScene()`, the composed canvas (not a
screenshot of the widget, which carried the border, selection handles and badge). Outside
studio mode the starting picture is taken on `sceneAboutToChange` and the fade starts on
`currentChanged`; in studio mode the transition button takes it before
`promotePreviewToProgram`.

### 9.7 Drag tracking

A drag holds its layer by identity (`QPointer<SceneItem>`), not by index, and looks the index
up on every move, so a reordered layer is still the one moved and a layer that has left the
scene ends the drag. Press begins an edit session (`beginCurrentItemTransformEdit`), moves
update it (snapped for moves, clamped for resizes), and release commits it as one undo step.
A change of current, program or preview scene ends the drag and commits what it moved so far;
rolling it back would also restore the scene index recorded in the session's snapshot and
switch straight back. A collection reset ends it without committing, because the reset has
already closed the session. A release that never arrives leaves the drag running, and a
preview created over existing content shows nothing until something changes (#102).

### 9.8 Replay ring

Only the Program instance buffers replays. With the replay buffer enabled, a 200 ms timer
(5 fps) JPEG-encodes `cachedComposedFrame()` at quality 75 on the GUI thread and appends it
with a wall-clock timestamp, trimming to the configured span. That keeps a 30 s ring near
22 MB instead of gigabytes, at the cost of replays being 5 fps stills and of the encode
running on the GUI thread (#89).

### 9.9 Tests

`compositionFollowsConsumersNotPaintEvents`, `captureDemandFollowsConsumers`,
`theReplayBufferIsAFrameConsumer`, `aScrollingLayerMovesWithNothingElseChanging`,
`aFadeStartsFromTheComposedCanvas`, `undoKeepsTheLiveCaptureFrameOnAir`, and the
`aPreviewDrag...` cases. `PreviewWidget` is compiled into the test executable for this.

---

## 10. Encoding and outputs

### 10.1 Producer interfaces

`media/TimedSource.h` decouples the encoder from what produces media:

- `TimedFrameSource`: `currentFrame()` (thread safe), `compositionSequence()` (or
  `kUnsequenced`, which makes every observation count as new), `nativeWidth()` and
  `nativeHeight()`. Implemented by `PreviewWidget` (live) and `RingTimedFrameSource` (replay).
- `TimedPcmSource`: a `QObject` with `sampleRate()`, `channels()` and the `pcmReady(QByteArray)`
  signal. Implemented by `AudioController` (live) and `RingTimedPcmSource` (replay).

The encoder calls `currentFrame()` from the GUI thread, on its clock and once when priming.

### 10.2 `EncoderPipeline`

**Responsibility.** One run of ffmpeg fed with raw video and PCM over two named pipes, with
the sink's own clock, bounded transports, counters that make the run interpretable, and a
bounded, ordered stop. `RecorderPipeline` uses the base output arguments; `StreamingPipeline`
overrides `buildOutputArgs`.

#### 10.2.1 Start sequence

`start(target, frames, audio, &error)` fails synchronously, with a message, if the pipeline is
already running, ffmpeg was not found on `PATH`, a source is missing, the pipe security
descriptor cannot be built, a pipe cannot be created, or ffmpeg does not start within 3 s.
Otherwise, on the GUI thread:

1. Builds a `PipeSecurity` descriptor and creates the two pipes (10.2.2).
2. Chooses the cadence: `FollowSource` for `Target::Kind::File`, `ConstantRate` for `Rtmp`.
3. Resets every counter and takes the capture statistics baseline from the provider.
4. Launches ffmpeg with `QProcess::SeparateChannels` and with `FFREPORT` removed from its
   environment (an ffmpeg report file starts with the full command line, destination URL
   included).
5. Prepares the priming frame: the current composition converted to the pipe format and size
   and copied into a private buffer, or black when nothing has been composed. Its sequence is
   read before the picture, so a composition landing between the two reads is sent by the next
   tick rather than skipped. The priming frame is real media: it becomes the file's first
   frame, and the tick that follows will not send the same picture again.
6. Starts a `PipeAcceptThread` for each pipe; the video one writes the priming frame the moment
   ffmpeg connects. ffmpeg opens its inputs in order and blocks on each until bytes arrive,
   so priming on the accept thread means the audio input is reached even if the GUI thread is
   busy when the video connection is signalled.
7. Arms the video clock (10.2.4).
8. Starts a 5 s watchdog that fails the run if the audio pipe has not connected, which in
   practice means ffmpeg died parsing its arguments.
9. Sets running and emits `started()`.

When the video acceptor reports a connection, `onVideoPipeConnected` creates the
`VideoPipeWriter`, counting the priming frame as already written. When the audio acceptor
reports, `onPipeConnected` stops the watchdog, creates the `AudioPipeWriter`, and only then
subscribes to the audio source's `pcmReady` with a queued connection guarded by the run id.
Both handlers ignore a late delivery after the run has ended; acceptor signals are also
guarded by `QPointer` and identity, so a previous run's acceptor cannot reach this one.

#### 10.2.2 Named pipes and their security

Each run creates `\\.\pipe\malloy_video_<pid>_<salt>` and `\\.\pipe\malloy_audio_<pid>_<salt>`,
where the salt is 32 bits from `QRandomGenerator::system()`. Each pipe is:

- outbound only, byte mode, overlapped, one instance;
- created with `PIPE_REJECT_REMOTE_CLIENTS`, so it is not reachable over SMB;
- created with an explicit, protected DACL granting full access to `SYSTEM` and the current
  user only (`D:P(A;;GA;;;SY)(A;;GA;;;<user SID>)`). The default descriptor grants read
  access to Everyone and the anonymous account, which for an outbound pipe carrying the screen
  and the microphone is exactly the access that matters. If the descriptor cannot be built,
  the recording does not start rather than fall back to the default.

The name is not the defence, since `\\.\pipe\` can be enumerated; the salt only stops a
watcher from waiting on a predictable name. The connecting client is deliberately not
identified by process id: package managers install ffmpeg behind a launcher that starts the
real binary as a child, so an exact match refuses working installations, and walking the
parent chain fails when the launcher exits. A process already running as the same user
could therefore connect first; removing the published name entirely by handing ffmpeg an
inherited handle is tracked in #41.

The video pipe's outbound buffer is one frame (8,294,400 bytes); the audio pipe's is 1 MiB.

#### 10.2.3 Transport threads

All pipe I/O goes through `CancellablePipeIo` (`recording/CancellablePipeIo.h`): overlapped
`ConnectNamedPipe` and `WriteFile` that wait on the completion event and a stop event
together, cancel with `CancelIoEx`, and always wait for Windows to confirm completion before
the buffer can go away. The stop event stays signalled, so a stop cannot be missed in the
window between checking it and issuing I/O. That is what makes every join in the stop sequence
bounded.

- **`VideoPipeWriter`** holds at most 3 frames (about 25 MB). `trySubmit` never blocks: it
  refuses a frame whose format or size does not match what ffmpeg was told
  (`conformsToPipeDeclaration`), and refuses any frame when the queue is full. Frames are
  written whole when tightly packed and row by row otherwise (`isTightlyPacked`), because
  rawvideo has no stride and padded rows would shear the image. It reads only through const
  accessors, and the producer hands over a freshly converted image it keeps no reference to,
  so the implicitly shared frame is never detached. Video once went through `QProcess` to
  ffmpeg's stdin, whose buffer drains only when the GUI thread's event loop runs; a busy loop
  starved ffmpeg while the application looked healthy (measured: 896 of 900 frames refused).
- **`AudioPipeWriter`** holds up to 1500 chunks (about 30 s at 20 ms per chunk, under 6 MB)
  and drops the oldest only past that, counting the dropped bytes. ffmpeg derives audio time
  from the byte count, so dropped audio deletes time; holding it is cheap. Writing audio on the
  GUI thread once deadlocked the application against ffmpeg.

Neither writer reports a failed write while ffmpeg keeps running (#86).

#### 10.2.4 The video clock

The sink owns its cadence. The video timer is a single-shot `Qt::PreciseTimer` re-armed for
each absolute deadline `n / fps` seconds after the run started (`armVideoClock`), rounded up
to the next millisecond so it never fires early. A repeating timer runs in whole milliseconds,
so 60 fps ticked at 58.8 Hz and 120 fps at 125 Hz, which a stream then made up for with
duplicated or dropped frames. When a tick arrives more than a period late, `onVideoClock`
takes one tick and counts the ones it missed as TICK LATE, rather than writing the same picture
several times at the wrong moments.

#### 10.2.5 Cadence, still floor and repeats

Each tick asks the pure rule `shouldWriteFrame(cadence, sequence, lastSent, cadenceDue,
stillFloorDue)`:

- **`ConstantRate` (streams)**: write on every tick, whatever the source did. An ingest has
  negotiated a rate and needs it however seldom the screen changes; letting a new picture pull
  a frame forward would put the presentation clock in the source's hands.
- **`FollowSource` (files)**: write when the composition sequence differs from the last one
  written, or the source is unsequenced, or the still floor is due. The wall-clock timestamps
  carry the time between pictures.
- **Still floor** (`kStillFloorMs = 100`): a file that has written nothing for 100 ms writes
  the unchanged picture again. ffmpeg holds back an input that runs ahead of the others, so a
  video stream that stopped during a still scene stopped the audio being read as well; it
  queued, was eventually dropped, and put the rest of the recording out of sync. OBS never
  lets its video stream stop; this keeps that property at a tenth of a second rather than every
  frame because each frame here crosses a pipe at 8 MB.

A written frame whose sequence equals the one already written is a **repeat** (the still floor,
or a stream holding its rate). Repeats are counted as REPEAT and never as composed pictures,
and a repeat refused by a full transport is not counted as a drop, because the transport is
busy with pictures anyway.

#### 10.2.6 Frame preparation

After the rule says write: the frame is `currentFrame()`, or a cached black frame if the source
has none (a tick must still produce bytes, since ffmpeg prints nothing and makes no progress
until every input has delivered). A tick before the video writer exists counts as idle. The
transport's queue depth is checked before any conversion, so a frame that will be refused
costs nothing. Then the premultiplied canvas is converted to straight-alpha
`Format_ARGB32` (what ffmpeg's `bgra` means; a premultiplied frame would darken translucent
colours), scaled only if it is not 1920x1080, and moved into the transport.

#### 10.2.7 ffmpeg arguments

Input arguments are common to every run:

```
ffmpeg -y -hide_banner -loglevel error -stats
       -f rawvideo -pix_fmt bgra -s 1920x1080 -framerate <fps> -use_wallclock_as_timestamps 1
       -i \\.\pipe\malloy_video_<pid>_<salt>
       -f s16le -ar 48000 -ac 2
       -i \\.\pipe\malloy_audio_<pid>_<salt>
```

- `-stats` restores the progress line that `-loglevel error` suppresses.
- `-framerate` declares the output rate (`declaredInputFrameRate`, clamped to 1 to 1000). It
  sets the input time base; left undeclared, ffmpeg assumed 25 fps and every recording held
  25 frames a second. `-r` on the input is deliberately absent: it generates timestamps from
  the frame index, so a dropped frame deleted time (a 144 s run produced a 109 s file).
- `-use_wallclock_as_timestamps 1` makes ffmpeg stamp each frame with the wall clock when it
  reads it, so a dropped frame leaves a gap and the timeline stays true.

Output arguments:

| | File (`RecorderPipeline`, base class) | Stream (`StreamingPipeline`) |
|---|---|---|
| Frame rate mode | `-fps_mode vfr` (honour the input timestamps) | `-fps_mode cfr -r <fps>` (fill gaps by repeating, for the ingest) |
| Colour | `videoFilterArgs` | `videoFilterArgs` |
| Codec | `EncoderRegistry::find(codec)->buildArgs(s, Destination::File)` | `buildArgs(s, Destination::Stream)`, then `-tune <streamingTune>` when the encoder has one |
| Keyframes | encoder default | `-g <fps x keyframeSec>` (2 s when unset), appended last so it wins |
| Audio | `-c:a <codec> -b:a <kbps>k` | `-c:a <codec> -b:a <kbps>k` |
| Destination | the file path (muxer from its extension) | `-f flv <url>` |

`videoFilterArgs` scales to the output size and converts BGRA to YUV 4:2:0 with the BT.709
matrix at limited range in the scale filter, stamps the frames with `setparams`, and adds
matching `-colorspace`, `-color_primaries`, `-color_trc` and `-color_range` tags. ffmpeg's
defaults were a BT.601 conversion with no colour description, which players read as BT.709,
shifting saturated colours. An unknown codec id falls back to libx264-style arguments.
`-shortest` is not used: a brief startup stall on one input would end the output.

A stream uses the recording's codec and frame rate from `OutputSettings`, with only bitrate and
keyframe interval taken from `StreamSettings`, so a stream can go out as HEVC or at 120 fps
(#87).

#### 10.2.8 Progress and stderr

`onFfmpegStderrReady` splits stderr into lines on `\r` and `\n` (ffmpeg rewrites its progress
line in place) and parses each with `tryParseProgressLine`: `bitrate=` is required; `drop=`,
`fps=` and `dup=` are optional. Each parsed line emits
`progress(bitrateKbps, droppedFrames, encodeFps, composedFramesRejected)`. `dup=` is cumulative
in ffmpeg, so the latest value is kept, never summed.

The last 4096 characters of stderr are kept for error messages. For a stream, the tail is
passed through `redactDestination` after each append (not per chunk, since a URL can arrive
split across two reads): the URL's last path segment, which carries the key, becomes `***`,
and the bare segment is replaced too when it is at least 8 characters. A file path is not
redacted, because it is what a file error is about.

#### 10.2.9 Stop, drain and pipe closing order

`stop()` is not instantaneous and runs nested event loops, so `isRunning()` stays true while
`m_stopping` is set; a second stop or a new start arriving in that window cannot delete the
object from under the loop. The sequence:

1. Stop and delete the video timer and the watchdog; disconnect the audio source.
2. **Video first.** `finish()` the video writer and wait, with the event loop running, for it
   to deliver what it holds, within a 1 s budget shared by both sides (`kStopDrainMs`). Then
   retire the video workers (request stop, which cancels pending I/O; join the acceptor and the
   writer) and **close** the video pipe. Video goes first because ffmpeg does not read audio
   that runs ahead of the picture until the video input ends; draining audio while the video
   pipe is open would wait on a reader that is waiting on this side.
3. **Then audio**, the same way, and close the audio pipe.
4. Close ffmpeg's unused stdin.
5. Wait up to 5 s, with a local event loop, for ffmpeg to finalise the file; after that, end
   its process tree and wait a further second.
6. Log the run summary (below) and, when profiling, the stage report.
7. Clean up, clear `m_stopping`, and emit `finished(destination, bytes)` (bytes is the file size
   for a file, 0 for a stream).

Closing the handle, not `DisconnectNamedPipe`, is deliberate: disconnecting discards whatever
ffmpeg has not yet read (up to a megabyte of sound or a whole frame), while closing lets it
read the rest and then see a clean end of file. `FlushFileBuffers` is not called, because on a
pipe it blocks until the reader has taken everything and an ffmpeg that stopped reading would
hang the GUI thread.

#### 10.2.10 Failure handling

Four failures end a live run: a pipe that did not connect, a `QProcess` error, ffmpeg exiting
with a non-zero code, and the audio pipe watchdog. All go through `stopWithError(summary)`,
which composes the message (summary, then `\n\nLast stderr:\n` and the redacted tail), calls
`stop()`, and only then emits `errorOccurred`, guarded by a `QPointer` because a slot on
`finished()` may delete the pipeline. A handler therefore always finds the output stopped and
`finished()` already delivered, and a message box cannot hold a failed pipeline open. ffmpeg
exiting with code 0 while running is an ordinary stop. `MainWindow` splits the message at the
stderr marker into a summary and detail text and shows one non-modal box per origin.

Because `finished()` precedes `errorOccurred()`, subscribers that treat `finished()` as
success report a failed recording or replay as saved (#94).

#### 10.2.11 The run summary

Each stop logs one line through Qt's default message handler (no custom handler is installed):

```
capture stages: SOURCE RX n  CAP DROP n  COMPOSED n  ENC ACCEPT n  PIPE WRITE n  ENC DROP n
                ENC DROP BURST MAX s  CFR DUP n  IDLE n  REPEAT n  TICK LATE n  AUDIO DROP s
```

(one line in the log; wrapped here). The gaps between the figures are the diagnosis.

| Field | Meaning |
|---|---|
| SOURCE RX | Frames the capture backends produced during this run (provider reading at stop minus reading at start). Zero for a replay save, which has no provider. |
| CAP DROP | Frames the backends produced and dropped because their handoff was full, during this run. Loss before composition. |
| COMPOSED | New pictures the sink found on its ticks: ENC ACCEPT plus ENC DROP. Compositions superseded between two ticks are not seen and not counted; repeats are excluded. |
| ENC ACCEPT | New pictures accepted into the video transport. A promise to carry them, not proof ffmpeg encoded them. |
| PIPE WRITE | Frames the transport finished writing to the pipe, including a successful priming frame. The gap to ENC ACCEPT is the transport's own backlog at stop. |
| ENC DROP | New pictures refused because the transport queue was full or the frame did not match the declared shape. Media loss inside the application. |
| ENC DROP BURST MAX | The longest unbroken stretch, in seconds, in which every new picture was refused. Distinguishes a hole from scattered drops. |
| CFR DUP | ffmpeg's own `dup=` counter: frames the output side synthesised to hold a constant rate. |
| IDLE | Ticks with nothing new to write, plus ticks before the video writer existed. Not loss. |
| REPEAT | Frames that rewrote the picture already written (still floor or constant rate). |
| TICK LATE | Ticks missed because the clock fired more than a period late, meaning the GUI thread was busy. |
| AUDIO DROP | Seconds of sound the audio transport dropped because ffmpeg stopped reading for longer than its queue holds. Time missing from the sound. |

The counters are kept distinct on purpose: a single "dropped" number would hide whether a
recording lost content at the source, inside the application, or at the output.

### 10.3 `MediaController`

**Responsibility.** The single facade the UI uses for recording, streaming and replay saves,
and the owner of their pipelines. A recording and a stream can run at the same time, each with
its own pipeline subscribed to the same frame and PCM sources.

- `startRecording(path, settings)` and `startStreaming(stream, output)` create a fresh pipeline
  for each start (the previous one is disconnected and deleted later), pass the capture
  statistics provider, and forward `started`, `finished`, `progress` and `errorOccurred` as
  `recording...` / `streaming...` signals and `errorOccurred(origin, message)` with origin
  `recording` or `streaming`.
- `startStreaming` refuses a URL that is empty or still contains `{key}`, merges bitrate and
  keyframe interval from `StreamSettings` into the output settings, and, when
  `stream/useKeyRelay` is on, starts an `RtmpKeyRelay` and gives ffmpeg its loopback URL. A
  relay that cannot start fails the stream rather than fall back to the direct URL. A relay
  failure, or a relay that has made no substitution 5 s after the stream started, stops the
  stream and then reports it. There is no reconnection: a dropped connection makes ffmpeg
  exit, which ends the stream, and relay congestion shows up only indirectly as ENC DROP
  (#88).
- `saveReplay(path, settings, frames, w, h, pcm)` takes snapshots of the two rings (taken by
  `MainWindow` back to back), and runs a one-shot `RecorderPipeline` over a
  `RingTimedFrameSource` and a `RingTimedPcmSource`, all parented to a temporary object deleted
  when the pipeline finishes or fails. The frame source's `exhausted()` stops the pipeline
  through a queued connection. Origin `replay`.
- The destructor stops the recording, the stream and any replay save still running, while
  their sources still exist.

`RingTimedFrameSource` plays the JPEG ring back in real time from its first call (the priming
call starts it): `currentFrame()` decodes the newest buffered frame whose offset from the first
has elapsed, advances its sequence only when a new frame is served, and emits `exhausted()` once
the last frame has been shown for the buffer's average spacing. It used to hand out one frame
per call, so a 30 s buffer was consumed in 5 s of ticks. `RingTimedPcmSource` emits 20 ms
chunks by elapsed time and emits nothing until something is connected to `pcmReady`; since the
encoder subscribes only when ffmpeg opens the audio pipe, chunks emitted before then were lost
and the clip's sound started late.

### 10.4 `RtmpKeyRelay`

ffmpeg takes the RTMP destination, key included, as a command-line argument, and any process
running as the user can read another process's command line (#10). The relay, opt-in through
`stream/useKeyRelay`, keeps the key out of it:

1. `start(realUrl)` accepts only `rtmp` and `rtmps` (default ports 1935 and 443), requires a
   key segment of at least 8 characters, and refuses `rtmps` when the build has no TLS.
2. It listens on `127.0.0.1` on an ephemeral port and returns
   `rtmp://127.0.0.1:<port>/<app path>/<placeholder>`, where the placeholder is random and
   exactly as many bytes as the key, so no AMF string or RTMP message length changes.
3. It accepts one connection and closes the listener. An `rtmps` upstream is a `QSslSocket`
   with no `ignoreSslErrors`, so a certificate failure aborts the handshake; nothing is sent
   before `encrypted()`.
4. Outbound bytes have every placeholder occurrence replaced with the key
   (`substituteAll`); trailing bytes that could be the start of a split placeholder are held
   back (`holdBackLength`), and only a proper prefix is held, since holding a fixed length
   stalls the handshake, which contains no placeholder. Inbound bytes pass through untouched.
5. Reading from ffmpeg pauses while 2 MiB or more waits to go upstream, and the client socket's
   read buffer is bounded, so a slow uplink blocks ffmpeg as a direct connection would instead
   of growing memory.
6. `stop()` zeroes the key and any queued outbound bytes. The substitution count survives stop
   as the evidence the relay engaged.

Known limitation: ffmpeg derives the `tcUrl` in the RTMP `connect` command from the URL it was
given, so the ingest sees a loopback `tcUrl`. This has been verified against a local RTMP
server but not against Twitch or YouTube, which is why the relay is off by default.

### 10.5 `OutputSettings`

A header-only struct stored in `QSettings` under `output/`, not per project: width, height,
fps, video codec, CRF, preset, audio codec, audio bitrate, container, stream bitrate, replay
buffer seconds and keyframe interval. `normalized()` bounds every field (even dimensions,
fps 1 to 1000, CRF 0 to 51, and so on), accepts only the eight encoder ids the registry knows
without probing hardware, keeps a stored software preset while a hardware encoder is selected
(only software encoders pass it to ffmpeg), accepts MP4, MKV and MOV, and forces AAC in MOV,
where ffmpeg refuses Opus. `integerValue` rejects non-numeric, non-integral or non-finite stored
values. `toJson`/`fromJson` carry the same fields (except the replay length) on render jobs.
`frameRateChoices(configured)` always includes the stored rate, so a settings page cannot
silently rewrite 120 fps to 60 by failing to represent it. Settings pages that persist values
nothing reads are tracked in #66, and the several owners of these settings in #65.

### 10.6 `StreamSettings`

Service (Twitch, YouTube, Custom), custom URL template, bitrate (500 to 51000 kbps), keyframe
interval (1 to 10 s), relay opt-in and broadcast metadata (title, category, tags) are stored in
`QSettings` under `stream/`. The key is stored in the Windows Credential Manager as the generic
credential `MalloyStudio_StreamKey`, never in `QSettings`. `rtmpUrl()` substitutes the key
into the service template, leaving `{key}` in place when there is none so callers can detect
an unconfigured key. `isValidCustomUrl` requires a strictly parsed `rtmp` or `rtmps` URL with a
host; `normalized()` replaces a template that fails it. `MainWindow` reloads these settings at
each Go Live rather than trusting a copy taken at startup. The Credential Manager write's result
is not checked, and Settings and Go Live accept URLs they should refuse (#95).

### 10.7 `EncoderRegistry`

**Responsibility.** Knows every encoder the application can drive, builds its ffmpeg arguments
for a destination, and decides which ones this machine can actually run.

- The catalog is fixed: `libx264`, `libx265`, `h264_nvenc`, `hevc_nvenc`, `h264_qsv`,
  `hevc_qsv`, `h264_amf`, `hevc_amf`. `find(id)` returns any of them, checked or not, so a
  saved codec always gets its own arguments.
- `Encoder::buildArgs(settings, Destination)` holds quality for a file and bitrate for a
  stream. Software: `-preset -crf`, plus `-maxrate` and `-bufsize` (2x) for a stream. NVENC:
  `-preset p4`, then `-rc cbr -b:v -maxrate -bufsize` for a stream or `-rc constqp -qp <crf>`
  for a file. QSV and AMF: `-b:v -maxrate -bufsize` for a stream; `-global_quality` (QSV) or
  `-rc cqp -qp_i -qp_p` (AMF) for a file. All end with `-pix_fmt yuv420p`. Constant quality
  matters for files beyond preference: any rate-targeting mode divides its budget by the rate
  declared on the input, and a measured hevc_nvenc CBR target fell from 4508 to 178 kbps as the
  declared rate rose, while constant quality held steady.
- `streamingTune`: `zerolatency` for x264 and x265, `ull` for NVENC, empty for QSV and AMF,
  which reject `-tune`.
- **Hardware check.** Common Windows ffmpeg builds include NVENC, Quick Sync and AMF whatever
  GPU is installed, so `ffmpeg -encoders` is not evidence of hardware. `startHardwareCheck`
  runs once per session on a low-priority worker thread: it lists the build's encoders, then
  encodes a fifth of a second of 256x144 black test video (`-f lavfi -i color=... -c:v <id>
  -f null -`) with each hardware candidate, each run limited to 10 s and ended with
  `ProcessTree::kill` if it overruns. A family whose H.264 encoder fails has its HEVC encoder
  marked unavailable without a trial. Results are applied on the GUI thread and announced by
  `notifier()->hardwareChecked()`, which the Settings workspace and the dashboard refresh on.
- `available()` is what is offered and recommended: the software encoders always (even without
  ffmpeg, so a picker is never empty) and each hardware encoder whose trial passed; until the
  check reports, software only, so nothing unverified is recommended. `support(id)` is
  `Unchecked`, `Works` or `Unavailable`; `label(id)` says so; `choices(keep)` lists the offered
  encoders plus a saved one that is not offered, marked, instead of replacing it.
- Tests pass their own `Probe` (or `ffmpegProbe(program, leadingArgs, timeout)` with a stand-in
  program) and call `resetForTesting`, so no test runs a trial encode on the host's hardware.

Remaining gaps (Smart Config wording during the check, renders with an unavailable encoder, a
trial left running at quit) are tracked in #106.

### 10.8 `FfmpegVersion`

`probe(path, context, done)` runs `<path> -version` asynchronously on the context's thread with
a 5 s limit (tree-killed on overrun) and calls `done` once with the path and the version parsed
from the first line (`parse`: a release number such as `8.1.1`, or the whole build identifier
for development builds). The status bar shows it, so the ffmpeg named there is the binary
actually found on `PATH`.

### 10.9 Replay, recording and stream wiring in `MainWindow`

Recording asks for a path (expanding `recording/filenamePattern` with `{project}`, `{scene}`,
`{date}` and `{time}` and stripping characters Windows forbids) in `recording/lastDir`, then
starts the recording with the current `OutputSettings`. Go Live reloads `StreamSettings`,
prompts for a key if none is stored, starts the stream, and then, if Twitch is connected and
the service is Twitch, pushes the title and category without blocking the stream. A replay
save writes `replay-<timestamp>.mp4` to the Movies folder. Closing the main window asks before
ending a recording or stream, and stops outputs before captures (section 13).

### 10.10 `ProcessTree`

`descendants(pid)` walks a Toolhelp process snapshot breadth first and accepts a child only if
it was created after its parent, so a recycled parent id is not mistaken for a relationship.
`kill(pid)` terminates the descendants deepest first and the root last, counting an
already-exited process as ended. Used by `EncoderPipeline::stop`, the encoder check, the
version probe, `RenderPipeline` (cancel and probe timeouts) and `MediaRegistry` (probe timeouts
and teardown). `QProcess::kill()` alone ended a package manager's launcher and left the real
ffmpeg running with the file open.

### 10.11 Tests

Pure rules: `shouldWriteFrame` (`sinkCadenceDecidesWhenAFrameIsDue`), `videoFilterArgs`,
`redactDestination` (`encoderRedactsTheStreamKeyFromFfmpegOutput`), `conformsToPipeDeclaration`
and `isTightlyPacked` (`rawVideoDeclarationCoversSizeNotOnlyFormat`,
`rawVideoNeedsTightlyPackedRows`), `declaredInputFrameRate` (`inputDeclaresTheConfiguredFrameRate`),
`tryParseProgressLine` (`streamProgressLineParsesBitrateAndDrops`), and the relay helpers
(`rtmpRelaySubstitutesAcrossReadBoundaries`, `rtmpRelayTransportFollowsTheScheme`,
`aSlowUplinkBacksUpIntoFfmpegNotMemory`). Argument construction is checked through subclasses
that expose `buildOutputArgs` (`encoderPipelineRespectsRegistryPerCodecArgs`,
`streamingPipelineHonorsKeyframeSec`, `rateControlFollowsWhereTheMediaIsGoing`). Real encoder
runs (`aRecordingOfAStillSceneKeepsItsLengthAndAudio`, `aRecordingKeepsItsColours`,
`stoppingKeepsTheSoundAlreadyHandedOver`, `theSinkTicksAtItsConfiguredRate`,
`soundAndPictureStayTogether`, `aSavedReplayPlaysInRealTime` and others) run ffmpeg and
ffprobe from `PATH` and skip when they are absent. `MalloyPipeIoTests` covers
`CancellablePipeIo` cancellation and handle leaks.

---

## 11. Rendering the editor timeline

The editor timeline is part of the project (`timeline` in `.malloy.json`, owned by
`EditorWorkspace`). Rendering it is a separate background system that runs ffmpeg as fast as
the machine allows on file inputs, unrelated to the live pipeline's clocks. The decisions are
recorded in [ADR-0001](adr/0001-timeline-clip-source-reference.md),
[ADR-0002](adr/0002-render-job-contract.md) and [ADR-0003](adr/0003-render-worker-pipeline.md).

### 11.1 `RenderQueue`

Jobs carry everything needed to render them: `OutputSettings` as JSON and a snapshot of the
timeline taken when queued, so later edits do not change a queued job and a retry renders what
failed. The queue runs one job at a time and persists to `renderqueue.json` in the application
data folder (`{"schema": 1, "jobs": [...]}`; a bare array, schema 0, is still read; the read is
capped at 16 MiB). It saves on state transitions and adjustment notes, not on progress,
which changes many times a second.

- `enqueue` refuses, with a reason, a request with no timeline, no output path, an output path
  that is a folder, or an output folder that does not exist (`whyNotRunnable`).
- `load()` treats the store as untrusted (section 15). A job that was active when the
  application stopped becomes pending, because nothing resumes a killed render; a job without a
  snapshot is failed visibly; a restored job's output path must pass `MediaPathPolicy` before
  anything asks the file system about it; and every restored pending job gets the same
  `whyNotRunnable` check as an enqueued one. The constructor loads and immediately starts the
  next pending job, which is why those checks matter.
- `startNext` promotes the first pending job; a job that cannot start is failed and the next one
  tried, so one bad entry does not stall the queue. `setPaused` gates promotion only.
- `cancel` stops the active render first, which deletes its partial file.

### 11.2 `RenderPipeline`

1. `start(job)` refuses synchronously when a render is running, ffmpeg is missing, the job has
   no output path, the output file already exists (renders never overwrite), or
   `TimelineGraphBuilder` refuses the timeline without lengths.
2. If ffprobe is on `PATH`, each distinct source is measured one at a time
   (`-show_entries format=duration`), each run limited to 10 s and tree-killed on overrun. A
   source that does not answer with a finite positive number is treated as long enough. The
   first probe is queued, so nothing the probes cause is emitted before `start()` has returned.
3. `launch` rebuilds the graph with the measured lengths, rechecks that the output does not
   exist, writes the filter graph to `malloy_render_<uuid>.txt` in the temp folder (the command
   line is limited to about 32k characters), and starts ffmpeg with `-progress pipe:1`,
   `-filter_complex_script` and the output arguments. `adjusted(note)` reports clips that were
   cut at the end of their source.
4. Progress is `out_time_us` over the timeline duration, never decreasing. A non-zero exit
   removes the partial output and fails with the stderr tail; a zero exit without an output file
   also fails.
5. `cancel()` tree-kills ffmpeg (or the probe), removes the partial output, and emits nothing.

A failure can remove a file the render did not write (#93).

### 11.3 `TimelineGraphBuilder`

A pure function from a timeline snapshot, output settings and measured source lengths to input
arguments, a filter graph and output arguments:

- Refuses, naming the clip: an empty timeline, more than 32 clips, a clip with no source, a
  source path `MediaPathPolicy` refuses (checked before existence, because asking whether a UNC
  path exists authenticates to its host), a missing file, a duration, start or source-in that is
  not finite or outside zero to one day, scale outside 1 to 10000 percent, opacity outside 0 to
  100, gain outside -60 to 30 dB, speed outside 0.1 to 4, and rotation, pan or channel mapping,
  which are not supported yet.
- Clamps a clip that runs past the end of its measured source to what the source has left and
  records it (`ClampedClip`, `describeClamps`); a clip that starts after its source ends is
  refused.
- Builds a black background at the output size, rate and duration, so gaps render black and
  timing does not shift; trims, retimes and scales each video clip and overlays them in track
  order at positions scaled from canvas pixels to output pixels; trims, retempos (chained
  `atempo`), applies gain and delays each audio clip and mixes them with `amix` without
  normalisation.
- Puts media paths only in `-i` arguments, never in the graph text, where `:` and `'` are
  metacharacters a file name could inject. Clips are addressed by input index.
- Takes codec arguments from `EncoderRegistry` for `Destination::File`, maps `[vout]` and
  `[aout]`, and bounds the output with `-t <duration>`.

Unlike live outputs, renders do not use `videoFilterArgs`, so they are neither converted nor
tagged as BT.709 (#92). Editor media lengths are rounded to whole seconds and still images
render as one frame (#100).

### 11.4 Tests

`timelineGraphPlacesTrimsAndScalesClips`, `timelineGraphScalesPositionsFromCanvasToOutput`,
`timelineGraphClampsClipsThatRunPastTheirSource`, `timelineGraphRefusesWhatItCannotRender`,
`timelineGraphBoundsNumbersBeforeArithmetic`, `timelineGraphMixesAudioAndKeepsPathsOutOfTheGraph`,
`renderQueueProcessesAndPersists`, `restoredRenderJobsAreValidatedLikeEnqueuedOnes`,
`restoredRenderJobsMustWriteToALocalDrive`, `renderJobReportsAClipCutAtTheEndOfItsSource` and
`aRenderIsNotHeldUpByAProberThatHangs` (using `setProbeCommandForTesting`).

---

## 12. Timing and cadence rules

These invariants cut across subsystems. A change that breaks one of them produces files that
look valid and are wrong, so they are stated together here.

1. **Capture cadence belongs to the source.** DXGI produces a frame when the desktop presents
   one (33 ms acquire timeout), WGC when the system announces one, window capture about every
   33 ms, a camera at its negotiated native rate. Nothing upstream polls a backend on the
   encoder's schedule.
2. **Composition is driven by content and consumers.** It happens when the composition sequence
   has advanced and a consumer exists, coalesced to once per event-loop pass. It never depends
   on paint events, visibility or minimisation while a recording, stream or replay buffer is
   active. Content that moves on its own is recomposed at the output frame rate.
3. **Composition sequence and capture sequence are separate.** The encoder follows the
   composition sequence; capture diagnostics count the capture sequence. Neither is derived
   from the other.
4. **The sink owns its cadence.** The video clock ticks at absolute deadlines `n / fps`. A file
   writes new pictures and the still floor; a stream writes on every tick. A source producing a
   picture never advances the stream's clock, and a late tick is taken once and counted.
5. **Media timestamps are wall clock, assigned by ffmpeg when it reads the frame**
   (`-use_wallclock_as_timestamps 1`), with the output rate declared only as the time base
   (`-framerate`). Gaps stay gaps in files (`-fps_mode vfr`); streams are resampled to a
   constant rate (`-fps_mode cfr -r`). The timestamps in `CapturedFrame::capturedAt` are
   normalised at the capture boundary but are not used downstream.
6. **Audio time is the byte count.** The mixer emits one 20 ms tick per 20 ms of elapsed time,
   catching up after a late or stalled fire; the transport holds about 30 s before dropping;
   stop drains what was handed over.
7. **Video never stops for long in a file.** The still floor writes the unchanged picture after
   100 ms, so ffmpeg keeps reading audio.
8. **Replay rings are timestamped** with wall-clock microseconds since the epoch
   (`ReplayFrame::ptsUs`, `TimedPcm::ptsUs`), which can step backwards if the system clock
   changes. Replay video is played back against a steady clock from the frames' offsets from
   the first; replay audio is played back by elapsed time, 20 ms per chunk.

---

## 13. Lifetime and shutdown

### 13.1 Closing the window

`MainWindow::closeEvent`:

1. `confirmEndingOutputs()` asks before ending a recording or stream ("Stop and Quit" or
   Cancel, Cancel the default); `maybeSave()` asks about unsaved changes. Either can cancel the
   close.
2. Stops the recording and the stream while their sources are still running and the event loop
   still exists to finish the file. Left to the destructor, they ran on after the captures had
   stopped and were finalised only after the event loop had returned.
3. `CaptureController::stopAll()` stops every session and clears blocked keys and retries.
4. Saves window geometry and state.

### 13.2 Destruction

`~MainWindow` repeats the output stops and `stopAll()` defensively, then Qt destroys children:

- `MediaController` stops any pipeline still running, including a replay save, while its
  sources exist (a replay save's teardown once used a freed audio source).
- `AudioController` unregisters its device notification first, stops the mixer, and retires each
  worker with a 4 s wait.
- `CaptureController` stops all sessions; each session retires or joins its worker.
- `MediaRegistry` kills its in-flight probe; `RenderPipeline` cancels an active render, removing
  its partial output. The store still records the job as active, so it is re-queued as pending
  at the next launch.

### 13.3 Threads and statics that outlive their owners

- Workers that would not stop are cut loose by `retireWorker` and reclaimed with the process.
- The camera enumeration thread is joined as the application object is destroyed; its notifier
  is a leaked singleton, so a result posted from the worker never lands on a destroyed object.
- The encoder check's notifier is leaked for the same reason, and the check posts nothing once
  `aboutToQuit` has fired.
- The `OffThread` pool is never destroyed, so a thread blocked on a vanished network share does
  not hold the process open after its window has closed; its threads end with the process.
  `OffThread::run` owns its `QFutureWatcher` by the requesting object, so a result for a
  destroyed object is dropped.

### 13.4 Pipelines and sessions

`MediaController` creates a new pipeline for each start. Pipeline workers (acceptors and
writers) are owned by the pipeline and joined in `stop()` and `cleanup()`, never
`deleteLater`ed, because they hold waits on handles the pipeline closes. Capture sessions are
deleted later after being stopped; the camera session's `stop()` joins its read thread
without a time limit.

---

## 14. Persistence

| Store | Location | Owner | Notes |
|---|---|---|---|
| Project | User-chosen `*.malloy.json` | `ProjectDocument` | Written atomically with `QSaveFile`; read refused above 32 MiB; timeline stored under `timeline` when non-empty. See [PROJECT_FORMAT.md](PROJECT_FORMAT.md). |
| Application settings | `QSettings` with organisation and application `MalloyStudio` (the registry under `HKCU\Software\MalloyStudio\MalloyStudio`) | Several | Keys below. Not per project. |
| Stream key | Credential Manager, generic credential `MalloyStudio_StreamKey` | `StreamSettings` | UTF-16, `CRED_PERSIST_LOCAL_MACHINE`. |
| Twitch tokens | Credential Manager, generic credential `MalloyStudio_TwitchTokens` | `TwitchAuth` through `CredentialStore` | JSON; write failures reported. |
| Clip index | `clips.json` in `AppDataLocation`, with `clips.json.lock` and `clips.json.bad[.n]` | `ClipsRegistry` | Merged under a lock (section 14.2). |
| Render queue | `renderqueue.json` in `AppDataLocation` | `RenderQueue` | Schema 1; read capped at 16 MiB. |
| Media probe cache | `media-probe-cache.json` in `CacheLocation` | `MediaRegistry` | Only entries for files in the current scan; read capped at 8 MiB; treated as disposable. |
| Render filter graph | `malloy_render_<uuid>.txt` in `TempLocation` | `RenderPipeline` | Removed when the render ends. |

`QSettings` keys read by subsystems:

| Keys | Read by |
|---|---|
| `output/*` | `OutputSettings::load` (recording, streaming, render export) |
| `stream/service`, `customUrl`, `bitrateKbps`, `keyframeSec`, `title`, `category`, `tags`, `useKeyRelay` | `StreamSettings::load` |
| `stream/twitchClientId` | `MainWindow` and `SettingsWorkspace` for `TwitchAuth` |
| `audio/inputs/<input id>/volume`, `pan`, `muted`, where the input id is `loopback:default` or `input:<device id>` | `AudioController` |
| `audio/limiterEnabled`, `audio/limiterThresholdDb` | `MainWindow` at launch, `SettingsWorkspace` |
| `capture/backend` | `CaptureBackend` |
| `hotkeys/<actionId>` | `HotkeyManager` |
| `projects/searchDirs`, `media/searchDirs` | `ProjectRegistry`, `MediaRegistry` |
| `recording/lastDir`, `recording/filenamePattern` | `MainWindow`, `RecentRecordings` |
| `onboarding/completed` | `MainWindow` (first-run wizard) |
| `profile/frames` | `main()` |
| `geometry`, `windowState` | `MainWindow` |

Several other keys written by the Settings workspace are not read by anything (#66).

### 14.1 Project documents

`ProjectDocument::saveToFile` writes `SceneCollection::toJson()` plus the editor timeline;
`loadFromFile` bounds the size, parses, extracts the timeline and calls
`loadFromJson(..., LoadOrigin::File)`, which applies the file-origin rules (section 6.6 and 6.7).
`displayName` strips the compound `.malloy.json` extension.

### 14.2 Library registries

- **`ClipsRegistry`** indexes saved replays. Every window of the application shares one store
  and each keeps the list it read, so a save re-reads the store under a `QLockFile` (1 s wait)
  and lays only this instance's unsaved additions and edits (`m_unsaved`) over it; every other
  entry keeps the store's version. A store that cannot be parsed is renamed to
  `clips.json.bad[.n]` and never written over; one that cannot be read is left alone and the
  save fails with `saveFailed`, keeping the changes for the next save.
- **`ProjectRegistry`** and **`MediaRegistry`** index folders (`projects/searchDirs`, seeded
  with Movies and Documents; `media/searchDirs`). Each folder is listed off the GUI thread
  through `OffThread::run`, one listing per folder at a time (a folder asked for again while
  its listing is out is listed once more afterwards), and `changed()` follows each answer. A
  folder on a network share that has gone away answers late or as unavailable
  (`unavailableDirs()`), and holds up neither the window nor the other folders. Saving or
  recording into a folder lists that folder only. One file reached through two folders is listed
  once, by canonical path. The first scan waits for the event loop, so a test that sets its own
  folders never scans the user's.
- `ProjectRegistry` reads at most the first 1 MiB of each project to count its scenes.
- `MediaRegistry` classifies by extension, then probes duration and resolution with ffprobe, one
  file at a time on a `QProcess` (at most 200 files per scan, 10 s limit each, tree-killed on
  overrun), coalescing probe results into at most one `changed()` per 250 ms. Results,
  including failures, are cached by canonical path while size and modification time are
  unchanged. Files whose data is in the cloud or offline (`isCloudOnly`) are listed but never
  opened, because opening them downloads them. A folder answering restarts the probe chain,
  and killing a probe waits up to 1 s on the GUI thread (#107).
- **`RecentRecordings`** lists the newest files in `recording/lastDir` for the dashboard. It runs
  on the GUI thread, as do some storage queries elsewhere (#70).

---

## 15. Trust boundaries and security-relevant design

| Boundary | What crosses it | Controls | Residual risk |
|---|---|---|---|
| Project files | Scene descriptions, device identifiers, image paths, timeline clips | Size cap before reading; schema validation before replacing state; device-backed sources held until allowed, before any signal (6.6); image paths from files limited by `MediaPathPolicy`; browser URLs stored but never rendered; timeline clips validated at render time (11.3) | Holds depend on `needsDeviceConsent` classifying new source types; its default is to hold. |
| Render queue store | Jobs restored at launch and started without user action | Same `whyNotRunnable` checks as enqueue; output path must pass `MediaPathPolicy` before any file system query; timeline refusals and numeric bounds; paths only as `-i` arguments; never overwrites | A failed render can delete a file it did not write (#93). |
| Named pipes to ffmpeg | Live screen, window, camera and microphone media | Protected DACL (SYSTEM and current user), remote clients rejected, single instance, random salt, start refused without a valid descriptor (10.2.2) | A same-user process can connect before ffmpeg (#41). |
| ffmpeg command line | Stream destination URL including the key | Optional loopback key relay (10.4); `FFREPORT` stripped from the child environment | Without the relay the key is visible to same-user processes (#10). |
| ffmpeg stderr in error dialogs | Destination URL | `redactDestination` over the accumulated tail | |
| RTMP relay | ffmpeg's publish session and the real key | Loopback listener, one client then closed, same-length placeholder, TLS verification without overrides for `rtmps`, unsupported schemes refused, 2 MiB backpressure, key zeroed on stop, engagement check | Loopback `tcUrl`; not verified against Twitch or YouTube. |
| Credential Manager | Stream key, Twitch tokens | Kept out of settings files; failures logged without the value | Readable by any code running as the user; not a boundary against local code. The stream key write result is ignored (#95). |
| Child process launch | `ffmpeg` and `ffprobe` from `PATH` | Resolved to an absolute path once, so the application and current directories are not searched; ended as a tree | The binary on `PATH` is trusted. |
| Media paths | Image sources, timeline clips, restored render outputs | `MediaPathPolicy::isAllowed`: drive-absolute local paths only, refusing UNC paths (which trigger outbound authentication and leak the user's NTLM response) and protocol strings (which ffmpeg would treat as URLs) | Paths the user picks in a dialog are theirs and are not restricted. |
| ffprobe output and media files | Durations and resolutions a file claims | Parsed and bounded before narrowing; cloud-only files never opened | |
| Settings values | Anything written to the registry | `normalized()` and `integerValue` for output and stream settings; `boundedControl` for audio controls; strict custom URL validation | |
| Devices | Sample formats, camera buffers and formats, WGC timestamps | Unsupported sample formats refused; buffer length checked before wrapping; negotiated camera output type verified; WGC timestamps sanity checked | |
| Twitch | OAuth tokens and Helix responses | Device code flow (no client secret); single-use refresh tokens stored before reuse; generations drop replies after sign-out or cancel; 30 s transfer timeout | Not yet verified against the real API (#20). |

---

## 16. Platform services

### 16.1 `CredentialStore`

`save`, `load` and `erase` over generic Windows credentials. An empty value erases; failures
return false and are logged with the target and Windows error, never the value. Used for the
Twitch tokens; the stream key is stored by `StreamSettings` with its own Credential Manager
calls.

### 16.2 `TwitchAuth` and `TwitchApi`

`TwitchAuth` signs in with Twitch's device code flow against `https://id.twitch.tv`: Twitch
does not support PKCE, and the implicit flow returns no refresh token. The user is shown a code
and a URL, and the class polls at Twitch's interval until tokens arrive. Refresh tokens are
single use, so `MainWindow` owns the one instance and replacements are stored before use.
`withAccessToken` refreshes before a request when the token is about to expire; a refresh
Twitch does not answer keeps the account, and only a refused refresh signs it out. The client id
is per installation (`stream/twitchClientId`). `TwitchApi` calls Helix at
`https://api.twitch.tv` to fetch the user id (cached until the account changes), fetch the
stream key, and set the title and category. Both accept a base URL override for tests. Message
wording gaps are tracked in #105.

### 16.3 `HotkeyManager`

Registers global shortcuts with `RegisterHotKey` on the GUI thread and receives `WM_HOTKEY`
through a native event filter, so they work while another application has focus. Bindings are
stored under `hotkeys/<actionId>`. `applyBindings` changes several bindings as one operation:
every changed action is unregistered before any new shortcut is registered (Windows refuses a
combination still registered, which is every swap), duplicate shortcuts are refused before
anything changes, and a refusal by Windows restores every action. `bindingFailed` reports a
refused shortcut, including at startup. Action ids: `record.toggle`, `stream.toggle`,
`replay.save`, `studio.transition`, `scene.switch.<n>`, `audio.mute.<inputId>`. The Win32 calls
are injected through `Registrar` for tests. Remaining editing gaps are in #104.

### 16.4 `FrameProfile`

Opt-in stage timing (`MALLOY_PROFILE_FRAMES=1` for one run, or `profile/frames`). Stages:
CAPTURE READBACK, CAMERA ARRIVAL, HANDOFF TO GUI, COMPOSITION, WIDGET BLIT, ENCODER CONVERT,
ENCODER HANDOFF, PIPE WRITE, ENCODER BACKLOG (bytes, sampled per tick) and TICK GAP. Each keeps a
lock-free log-bucketed histogram (four buckets per octave) for the whole run and for the current
second. `MainWindow` logs a `frame stages:` line every second while profiling; each run's stop
logs the cumulative report, reset at each run's start. It measures the application, not the
file: on a still desktop a long gap between packets is correct behaviour.

### 16.5 `SmartConfig` and `MachineLoad`

`SystemProbe::detect` gathers a `SystemProfile` (offered encoders, CPU threads, monitors, the
configured microphone and whether it is connected, the camera cache, free space on the recording
volume) on the GUI thread. `SettingsRecommender::recommend` is a pure function of that profile
and the current settings, returning proposed settings, a reason for each, and warnings; it
never applies anything. Upload bandwidth is not measured (#19). `MachineLoad` reads CPU tick
counters and memory load; `cpuBusyPercent` is pure and returns -1 for an invalid, zero-length
or backwards interval rather than a spike.

### 16.6 `OffThread`

`OffThread::run(context, work, done)` runs `work` on a private pool of up to 8 threads and
`done(result)` on `context`'s thread; if `context` is destroyed first, the result is dropped.
Callers keep at most one call per folder or volume outstanding.

---

## 17. UI shell

`AppShell` is the central widget: an icon rail, a header, a stack of workspaces and the status
bar. Workspaces are registered by id: `dashboard`, `record`, `stream`, `editor`, `clips`,
`media`, `projects`, `render`, `ai` and `settings`. The Recording workspace hosts
`ScenesPanel`, `SourcesPanel`, the preview container (Staged pane, transition column, Program
pane), `ControlsBar`, `AudioMixerPanel` and `InspectorPanel`. The AI Lab workspace is a static
page with no connections to any subsystem, and the editor's program monitor is a placeholder
(#22).

Panels change the model through `SceneCollection` methods, which record undo. The one
exception is filter editing in `InspectorPanel`, which changes filter objects directly but
brackets every change with an edit session or a snapshot pushed through
`SceneCollection::pushSnapshotCommand`, so it is undoable in the same way. The Inspector
rewrites a field only when the layer shown changes or the model disagrees with it, so a field
being typed in keeps its caret. Known filter-control issues are in #97.

`StudioStatusBar` shows state, CPU, memory, free disk space (queried off the GUI thread), encode
rate, bitrate, encoder drops, capture drops (from the same capture statistics provider as the
run summary) and the ffmpeg version, each measured or blank; each session's timer runs from its
own start. With a recording and a stream running together, the encode figures alternate between
them (#101).

---

## 18. Test seams and test suites

The test targets are defined in `CMakeLists.txt`; how to build and run them is in
[DEVELOPMENT.md](DEVELOPMENT.md).

| Executable | Source | Scope |
|---|---|---|
| `MalloyStudioTests` | `tests/model_tests.cpp` | QTest suite of about 190 cases over the core sources and the UI sources listed in `CMakeLists.txt`. |
| `MalloyPipeIoTests` | `tests/pipe_io_tests.cpp` | `CancellablePipeIo` against real pipes: completion, cancellation of pending accepts and stalled writes, stop before start, handle leaks. No Qt. |
| `MalloyCaptureLifetimeTests` | `tests/capture_lifetime_tests.cpp` | `CaptureCallbackGate`: late events after teardown, draining, serialisation. No Qt. |
| `MalloyOutputSettingsTests` | `tests/output_settings_tests.cpp` | `OutputSettings` and `StreamSettings` bounds, malformed values, presets, containers, custom URLs. |
| `MalloyCaptureHandoffTests` | `tests/capture_handoff_tests.cpp` | `CaptureFrameHandoff` bounds and lease lifetime across queued delivery and fan-out. |

Seams, by subsystem:

| Subsystem | Seam |
|---|---|
| Capture | `SessionFactory` constructor, `setWindowSessionFactoryForTesting`, `setCameraSessionFactoryForTesting`, `CameraCapture::setEnumeratorForTesting`; pure `rankNativeFormats`, `negotiateFormat`, `clockwiseDegreesFor`, `upright`, `normaliseBackendTime` |
| Audio | `setWorkerFactoryForTesting`, `defaultDeviceNotificationForTesting`, `bufferedBytesForTesting`; pure `AudioMix.h`, `StereoDownmix.h`, `Resampler.h` |
| Composition | pure `PreviewWidget::compositionRequired`; `setOutputFrameRate`, `animatingContent`, `composedFrameCount` |
| Encoding | `TimedFrameSource` and `TimedPcmSource` test doubles; pure `EncoderPipeline` statics; `buildOutputArgs` exposed by test subclasses; `setSourceStatsProvider`; `RingTimedFrameSource::setClockForTesting` |
| Encoders | `EncoderRegistry::Probe`, `ffmpegProbe(program, leadingArgs, timeout)`, `resetForTesting` |
| Rendering | pure `TimelineGraphBuilder::build`; `RenderPipeline::setProbeCommandForTesting`; `RenderQueue::setStorePath` |
| Registries | `ClipsRegistry(storePath)`, `setSearchDirs`, `setFolderScanHookForTesting`, `MediaRegistry::setProbeCommandForTesting` and `setProbeCachePathForTesting` |
| Platform | `HotkeyManager::Registrar`, `TwitchAuth(credentialTarget, parent)`, `setAuthBase`, `TwitchApi::setApiBase`, pure `SettingsRecommender::recommend`, `MachineLoad::cpuBusyPercent`, `FfmpegVersion::parse`, `MediaPathPolicy::isAllowed`, `RtmpKeyRelay` helpers |

Tests that need ffmpeg or ffprobe skip when they are not on `PATH`; several process tests use
`cmd.exe` as a stand-in child. Some tests still touch persisted settings, real desktop audio
capture or helper processes (#98). `MainWindow` is not in any test target, so its wiring is
verified by running the application.

---

## 19. Known architectural limitations

Open issues that bear on the architecture, by area. Most are also described in the section
they affect.

| Area | Issue | Limitation |
|---|---|---|
| Capture | #33 | No game capture; window capture cannot substitute for it. |
| Capture | #34, #35 | The pre-WGC capture and encode baseline, and the WGC backend's acceptance boundary, are still open. |
| Capture | #52 | Camera: RGB32 conversion by the reader on every frame, no user choice of resolution or rate, awaiting verification on a 60 fps camera. |
| Capture | #56 | DXGI rotation handling awaits verification on a rotated monitor. |
| Capture | #90 | The mouse pointer is never captured. |
| Capture | #91 | Window capture allocates and copies three full frames per frame. |
| Capture, audio | #96 | Error text discarded; permanent failures retried forever. |
| Capture | #99 | Camera enumeration on the UI thread in one path; no COM initialisation on the worker; duplicate names in the Inspector. |
| Composition | #89 | Replays are 5 fps JPEG stills encoded on the GUI thread. |
| Composition | #102 | Lost mouse release continues a drag; a new preview over existing content shows nothing. |
| Encoding | #41 | Pipe connect race; the fix is an inherited handle instead of a named pipe. |
| Encoding | #86 | A pipe writer failure is not surfaced while ffmpeg keeps running. |
| Encoding | #87 | Streams inherit the recording's codec and frame rate. |
| Encoding | #88 | A dropped connection ends a stream for good; relay congestion is not shown. |
| Encoding | #94 | A failed recording or replay is reported as saved. |
| Encoding | #10 | The stream key is on ffmpeg's command line unless the relay is enabled. |
| Encoding | #26 | Stream latency has not been driven down. |
| Encoders | #103, #106 | Container and codec follow-ups; encoder availability follow-ups. |
| Rendering | #92, #93, #100 | Renders not BT.709; a failed render can delete another file; media lengths rounded and stills one frame long. |
| Settings | #65, #66, #95 | Several owners of settings; values persisted that nothing reads; unchecked stream URL and key writes. |
| Threading | #70, #107 | Remaining GUI-thread storage queries and folder listing; ffprobe restarts and a 1 s wait on the GUI thread. |
| UI | #22, #97, #101, #104, #105 | Editor program monitor and Effects tab; Inspector filter controls; status bar alternation; hotkey editing; Twitch messages. |
| Platform | #19, #20, #24, #25 | Upload not measured; Twitch unverified against the real API; no chat, alerts or plugin host. |
| Tests | #98 | Tests reach into real settings, devices and processes. |
| Build | #59 | Deployment does not refresh non-Qt runtime DLLs after an MSYS2 upgrade. |
