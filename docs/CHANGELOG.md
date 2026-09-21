# Changelog

All notable changes to MalloyStudio are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

The repository has no release tags. v7 is commit 2c8b3a0 and everything after it
is unreleased. v6 and earlier predate the Git history: their code arrived in the
initial import, ad01b7a. Each entry ends with the short ids of the commits it
comes from. Work added since v7 is described as it now stands; the Fixed lists
cover defects that were present in v7 and significant defects found in the new
work before release.

Related documents: [README](../README.md), [DEVELOPMENT](DEVELOPMENT.md),
[ARCHITECTURE](ARCHITECTURE.md), [PROJECT_FORMAT](PROJECT_FORMAT.md) and the
[architecture decision records](adr/).

## Unreleased

Changes since v7
([compare](https://github.com/MalloyTheDev/MalloyStudio/compare/2c8b3a0...master)).

### Capture

#### Added

- Camera sources, captured through Media Foundation and shown live in the program
  and staged previews. A camera is added from Sources > Add and can be changed
  from the Inspector's device list (a27c962, 3628eaa).
- A Windows.Graphics.Capture backend for display and window sources beside DXGI
  Desktop Duplication, chosen in Settings > Performance. Switching takes effect at
  once, and a machine that cannot run it keeps Desktop Duplication (174613c).

#### Changed

- A camera runs its best native mode instead of the device's default: the highest
  frame rate, then the largest frame up to 1080p, then the format cheapest to
  convert. The mode it runs is logged once per session (83c143a, 6f3f4b7).
- Capture stops reading frames back while nothing uses them (no recording, stream,
  replay buffer or visible preview) and resumes in the same session when something
  does (9239817, 18dee41).
- Display, window and camera capture bound the frames waiting for the compositor,
  so a slow consumer costs frames, counted as CAP DROP, rather than memory
  (47dec02, b594ab9).

#### Fixed

- A display capture that loses access (a UAC prompt, the lock screen, a display
  mode change) or fails for any other reason is reported and tried again after
  half a second, backing off to ten seconds, instead of staying blank until the
  source is toggled (9d6685d, bdefab4).
- A camera that is unplugged, taken by another application or refused access is
  reported and retried with the same backoff, so plugging it back in restores it
  (55c39bf).
- Window capture holds its last frame while the window is hidden or minimised,
  survives isolated failed copies, and is retried after a failure unless the
  window has closed. An unrelated scene edit no longer restarts a failing camera
  ahead of its backoff (7660b81).
- A monitor rotated to portrait is captured upright by the DXGI backend instead of
  on its side (1e3a07e).
- A camera whose chosen mode the reader cannot convert tries its other modes and
  then the device default, and reports "Camera format not supported" instead of
  staying black; a format change mid-stream is picked up (4819cc5).
- Looking up cameras no longer freezes the window for seconds: the list comes from
  a cache refreshed on a worker thread, and one lookup runs at a time. The camera
  picker numbers devices that share a name, so the one chosen is the one bound
  (27daeb9, 78e0587, 4819cc5).
- Stopping a window source whose application has hung no longer aborts
  MalloyStudio, and a hung window is skipped with its last frame held (bfc8a7c).
- A frame queued before its session stopped no longer reaches the preview after
  the source was hidden, switched away from or held (fe651ad).
- Capture no longer uses a null texture when the DXGI device is lost or reset,
  reads past a short camera buffer, or runs a Windows.Graphics.Capture callback
  after its session has gone (73489a6, b594ab9).

### Audio

#### Added

- Volume and mute changes made in the Recording mixer or the Streaming workspace's
  mix show in the other at once (5665e78).

#### Changed

- Audio capture threads run under the MMCSS "Audio" task, and sound the device
  reports as lost is logged, with a total when the capture ends (1259f1b).
- Desktop audio follows Windows's default playback device when it changes, for
  example from speakers to headphones (d877aed).

#### Fixed

- Stereo inputs stay stereo through the mix, and pan acts as a balance control;
  every input used to be folded to mono, inverting out-of-phase content. Centred
  material is quieter than before, since the old compensating gain is gone
  (64d9cd7).
- Audio no longer crackles or drifts where a device's buffer size and the mixer's
  tick disagree: each input is buffered and mixed by the byte (6013ef8).
- Devices not running at 48 kHz are resampled; a 44.1 kHz microphone was recorded
  about 9 percent fast and drifted from the picture (3d211f2).
- Recorded sound no longer runs ahead of the picture, by about 1.2 s a minute plus
  every stall: the mixer and the replay audio are paced by elapsed time rather
  than by timer fires (92c0d45).
- Surround playback devices are folded into stereo (centre and surrounds at
  -3 dB, LFE left out) instead of losing everything but the front pair. Packed
  24-bit PCM is read instead of recorded as silence, and a format that cannot be
  read is refused with an error (57b645b).
- An input whose device fails or is removed is reported and restarted with a
  backoff from half a second to ten, and shows as connected again when sound
  returns (f7e842c).
- A microphone taken out of the scene by removing its layer or scene, undo, redo,
  opening a project or leaving studio mode stops capturing, where it used to stay
  in recordings and streams. Audio it had already queued is dropped instead of
  playing late if it is added back (d6e3854, 57510da).
- Volume, pan and the limiter threshold replace a stored value that is not a
  number with a default; a NaN could produce full-scale noise or switch the
  limiter off (eeb2cc7).

### Composition and preview

#### Fixed

- A recording or stream keeps moving while the window is minimised or another
  workspace is showing: the scene is composed whenever a recording, stream,
  replay buffer or visible preview needs it, not only when the preview paints
  (f43b644, 18dee41).
- Undo, redo and a cancelled edit no longer replace captured sources in the output
  with a waiting placeholder on a still desktop (18dee41).
- The program scene stays on air: undo no longer leaves studio mode and cuts the
  staged scene to air, removing a scene no longer points the output at nothing or
  at another scene, and staging a scene no longer stops the captures the program
  depends on (83f6df7).
- Fades start from the composed canvas instead of an upscaled grab of the preview
  carrying its border, selection handles and LIVE badge (ebaa167).
- A scroll filter moves at the output frame rate on an otherwise still scene, in
  the preview and in recordings (995d98f).
- Dragging a layer in the preview follows that layer. Switching scenes, entering
  studio mode or undoing mid-drag ends the drag instead of moving whichever layer
  took its place (fd178dc).
- Sources can be added on first run: a fresh launch and a new project start with
  one scene, where Add Source and Add Microphone used to do nothing (e55c878).
- Adding an image or window layer is undone in one step instead of first leaving
  an unconfigured layer behind (b378323).
- The blur filter no longer reads out of bounds on a source smaller than its
  radius (73489a6).

### Recording and streaming

#### Added

- Recording file names follow a pattern set in Settings > Recording, built from
  {project}, {scene}, {date} and {time} (eb0f450).
- The Streaming workspace: destination, go-live controls, stream health from
  ffmpeg's measured bitrate and dropped frames, and the live audio mix, with the
  title, category and tags saved in the stream settings. Figures nothing measures,
  such as viewers, show as unavailable rather than invented, and the chat and
  alert panels say they are not connected (25e863d, 399c3b1, 5665e78, c604b38).
- Each recording or stream ends with a summary in the log counting frames at every
  stage (SOURCE RX, CAP DROP, COMPOSED, ENC ACCEPT, PIPE WRITE, ENC DROP and its
  longest burst, CFR DUP, IDLE, REPEAT, TICK LATE) and the seconds of AUDIO DROP
  (0bcdf7c, 6978d67, cd3a3ae, a653078, 2874669, 3298f1e, 447ce90).
- An opt-in frame profiler, switched on by the MALLOY_PROFILE_FRAMES environment
  variable or the profile/frames setting, logs how long each stage a frame passes
  through takes (25fb138, 6f3f4b7).

#### Changed

- Video reaches ffmpeg over its own named pipe, written by a dedicated thread as
  audio already was, instead of through the GUI thread to ffmpeg's stdin. A full
  queue refuses the frame and counts it as ENC DROP, and recordings under load no
  longer contain frozen spans of several seconds (2f499c7, 1ef24c9, 635e098).
- Frames carry wall-clock timestamps and the video input declares the output's
  frame rate, so a dropped frame or a stall leaves a gap instead of shortening the
  file, and recordings are no longer held to about 25 fps (bbfa085, da26718).
- A file writes a frame when the picture changes, and repeats an unchanged one
  after 100 ms so its audio keeps flowing; a stream holds the constant rate it
  declared to the ingest (e065892, cd3a3ae, 79f9812).
- Recordings hold a constant quality, the CRF or QP value in settings, on every
  encoder, so file size follows the content; streams hold the configured bitrate
  (da26718).
- Recordings and streams are converted with the BT.709 matrix and tagged as
  BT.709; players shifted saturated colours in the untagged BT.601 output
  (be8c8e8).
- A hardware encoder is offered only after a short trial encode succeeds on this
  machine, checked once per session off the GUI thread, so an AMD or Intel machine
  is no longer offered NVENC just because the ffmpeg build includes it. A saved
  codec that is not offered stays selected and is marked (def8333).

#### Fixed

- A recording no longer hangs the application after about half a minute: the
  audio pipe is written from its own thread instead of blocking the GUI thread
  (96723ef).
- Bitrate, encode rate and ffmpeg's error output reach the status bar, the
  Streaming workspace and error dialogs: ffmpeg is fed a black frame until the
  first composition instead of waiting silently, and is asked to print its
  progress (7dcfa8a, 32244a3).
- A recording no longer fails at start when the first video frame is late; the
  video pipe is primed as soon as ffmpeg connects, so the audio watchdog is not
  tripped (3efa8e1).
- Stopping keeps the last frames and sound already handed to ffmpeg. They were
  discarded, up to the audio pipe's whole 1 MB buffer when sound ran ahead of the
  picture (cdac802).
- Races between starting, stopping and failing are closed: a second Stop or a Start
  during finalising no longer deletes the pipeline in use, a stale writer can no
  longer put a previous session's frame into a new file, and a failed output is
  stopped before its error is shown, one dialog per output (8b97d77, 2051bdf,
  19b08fa, 40d1598).
- A recording killed at the stop timeout, a cancelled render and a hung media probe
  end ffmpeg or ffprobe together with any process a package manager's launcher
  started, and cancelling a render no longer blocks the window (1d857cd).
- The record and stream hotkeys start and stop for real instead of only resetting
  the buttons, and a failed replay save no longer resets the stream button. Go
  Live and Stream Settings use the current settings, including the key relay
  opt-in, instead of those read at startup (e6f2a4b).
- Saved replays play in real time instead of several times too fast, keep their
  first few hundred milliseconds of audio, keep moving while the window is
  minimised, and finish saving at shutdown instead of crashing it (61a9abc,
  9112313, 18dee41).
- Software streams are capped at the configured bitrate while keeping their CRF
  quality target; the bitrate never reached libx264 or libx265 (cc98877).
- The video clock ticks at the configured rate against absolute deadlines. 60 fps
  had run at 62.5 Hz and then 58.8 Hz, and 120 fps at 125 Hz, which an ingest saw
  as duplicated or dropped frames (bd75eb0, 3298f1e).
- Output settings keep what was chosen: Settings > Recording shows a configured
  frame rate it does not list instead of saving 60 over it, the Output Settings
  dialog no longer resets the audio codec, the keyframe interval or a rate above
  120, and MOV and a software preset survive saving (06a5f5d, b1f1814, 33a7eef).

### Editor and render

#### Added

- A multi-track timeline with select and razor tools, trimming, snapping to clip
  edges, the playhead and a one-second grid, and moving clips between tracks of the
  same kind (ff52a38, 4ea476b).
- Per-clip transform, audio and speed settings in the editor's Inspector, transport
  playback with frame stepping and split at the playhead, and a media bin that
  lists the Media library and drags files onto the timeline (64868c7, dacbc3f).
- The timeline is saved in the project file, and each clip records its source file
  and in-point. A clip without a source is drawn dashed and cannot be rendered, and
  a new timeline starts empty (bf2c3ff, 7fe0923).
- Rendering: the editor's Export queues a snapshot of the timeline with its output
  settings, and the Render Queue runs ffmpeg for one job at a time with progress,
  pause, retry and cancel. A failed render reports ffmpeg's error, and a failed or
  cancelled one deletes its partial file (0baeec2, b506592, d2717d2).

#### Changed

- The timeline extends to the end of its last clip plus a minute instead of a fixed
  six minutes, so long recordings can be edited and rendered whole. Only a clip
  beyond the one-day render limit is shortened, and the editor says which
  (a1bea0f).
- A clip that runs past the end of its source is cut there and the render notes
  which clip and by how much; a clip that starts past the end is refused by name
  (a47c7bb).

#### Fixed

- Clip positions scale to the render's output size; at any size other than
  1920x1080, positioned clips landed in the wrong place (ce40a16).
- A render stacks video tracks as the editor shows them, V3 over V2 over V1; it drew
  them the other way up, so gameplay on V1 covered titles on V3.
- Dropping or moving media longer than the timeline no longer aborts the
  application (43f152f).
- Timeline edits mark the project modified, so closing prompts to save them, and
  the editor toolbar reports the real save state instead of a fixed "Autosaved"
  message (05a7503).
- The media bin scrolls instead of growing the window past the screen, which
  pushed the controls and status bars off it (5be1e19).

### Projects and persistence

#### Added

- The Clips workspace lists replay clips as they are saved, with favourites,
  archived clips and counts that follow the list (6ed6740, 4425628, 86f1d12).
- The Projects workspace lists the projects in Movies, Documents and every folder a
  project was opened from or saved to, and opens or creates them (4729961).
- The Media workspace lists the video, audio and image files in Movies, Pictures,
  Music and the recording folders, with duration and resolution from ffprobe when
  it is installed (00438de, ad4bc10, 6be7f74, 86f1d12).
- A Folders button in the Projects and Media workspaces lists the folders searched,
  marks those that cannot be reached, and removes a folder or looks again
  (9a7f6da).

#### Changed

- Media probe results are kept between launches while a file's size and time are
  unchanged. Cloud-only files are listed but never opened, which would download
  them, and a probe is ended after ten seconds (3ae6cb1).
- Library folders are listed and free space is queried off the GUI thread, so an
  unreachable network share no longer freezes the window; saving a project or
  finishing a recording lists only its own folder (9a7f6da).

#### Fixed

- Project names no longer keep ".malloy" in titles and recording file names, and
  renaming a scene to nothing restores its name in the list (a03bc33).
- The clip list no longer overwrites a clip store it could not read or that another
  window changed: a damaged store is moved aside, saves merge under a lock, and a
  failed save is reported (ec0ea97).
- An edit session left open by a slider is closed by opening a project, undo or
  redo, so a later undo can no longer load the previous project into the current
  window (3d9224c).
- A file that turns out not to be a project no longer leaves capture stopped
  (54a2c84).
- Output and stream settings are validated as they are read, so a corrupt stored
  value is clamped or replaced by a default instead of reaching ffmpeg (43a3e06).
- Undo no longer drops a network path the user picked in a file dialog; the
  local-path rule applies only to what is loaded from a file (cd57095).

### Platform and integrations

#### Added

- Twitch sign-in by device code, from Settings > Streaming with a per-install
  client ID. Signing in fetches the stream key, and going live sends the title and
  category to the channel (95a4fec).

#### Fixed

- A Twitch refresh that fails because of the network or an unavailable service no
  longer signs the user out; only a refusal from Twitch does. Closing the sign-in
  dialog any way stops the sign-in, and signing in as another account no longer
  talks to the previous channel (1b8a66b, 6227246).
- A global shortcut Windows refuses is reported and not saved, and shortcuts can be
  swapped or moved between actions in one change. A clash with another of the
  application's actions names that action, and numpad digits bind as numpad keys
  (70e24f8, e6f2a4b, 5a548f8).
- The build deploys the MinGW runtime beside the executable, so the application
  starts even when another MinGW toolchain is earlier on PATH (63bd4a4).

### Interface

#### Added

- Ten workspaces on an icon rail (Dashboard, Recording, Streaming, Editor, Clips,
  Media, Projects, Render Queue, AI Lab and Settings), switched with the number
  keys, with a bundled SVG icon set and a dark theme. AI Lab is a dimmed preview
  with nothing behind it (060a15a, 0c45de8, 25e863d, dec0d53, 3f2a7a9).
- A Ctrl+K command palette for switching workspace, recording, streaming, the
  project commands and the setup wizard (9c608d9, 08f41c8).
- A first-run setup wizard, reopened from Help > Setup Wizard or the palette. It
  shows the microphones, desktop audio, displays and cameras it finds without
  opening them, and saves the recording folder and a quality choice (Keep current,
  Standard, Archival, Lightweight) only when finished (a4d8a0f, e24f60b, fffa5f2).
- A Dashboard showing the output settings, audio levels, recent recordings,
  projects and clips, the render queue and eight readiness checks. Its quick
  actions start and stop recording and streaming, save a replay and start a
  project (eea8b33, a1de34e, a503571, 9f6dbc7, 776f79c, 1d7fb14).
- Settings in twelve sections, which set the recording folder and file name, the
  replay buffer length, encoder and quality, stream destination, master limiter,
  hotkeys, capture backend and whether the setup wizard shows. The remaining
  choices (the Recording page's automatic actions and most of the General,
  Performance, Appearance and Experimental pages) are stored, but nothing acts on
  them in this build (ccfae37, bbdb673, eb0f450, 351beb2, 60ab0f0).
- Recommend settings for this PC, in Settings > Video, proposes an encoder,
  bitrate, resolution and frame rate from the detected hardware with a reason for
  each, and listens to the configured microphone to tell a silent one from a
  working one. Nothing changes until the proposal is accepted (b186382, ec98aa7,
  c01cb27, 07c3540).
- Right-click menus on the Scenes and Sources lists (2ec016e).
- The status bar shows measured processor and memory use, free space on the
  recording volume, ffmpeg's bitrate and encode rate, ENC DROP and CAP DROP once
  they are non-zero, and the application and ffmpeg versions (32244a3, 2f499c7,
  174613c, bf3c66a).

#### Fixed

- Closing the window during a recording or stream asks first, then stops them
  before the captures they depend on (f01d158).
- Record and stream times count from when each output actually starts, not from
  the click before the save dialog, and a stream ending no longer resets the
  recording's time in the status bar (3147426).
- Typing a digit into a spin box, text area or the shortcut editor no longer
  switches workspace, and renaming a scene inline no longer touches a deleted list
  item (43f152f).
- Panels no longer show stale state: the Inspector clears a deleted layer's
  fields, the Recording header's source count follows removals, and the
  Dashboard's recent recordings update when a recording or replay is saved
  (45b07b6, 566c6f6).
- Typing in an Inspector text, URL or transform field keeps the caret and the
  partial value; a width typed as 800 used to commit as 1060 (d606f0d).
- Sliders show stored levels rounded instead of truncated, so 53 no longer shows as
  52 and a mixer knob no longer steps back under the cursor (75f67b6).
- The audio mixers scroll instead of growing the window past the screen when there
  are many inputs (ee55edc).

### Security

#### Added

- An opt-in relay in Settings > Streaming keeps the stream key off ffmpeg's command
  line: ffmpeg publishes to a loopback listener with a placeholder of the same
  length, and the relay substitutes the key upstream. It carries rtmps over
  verified TLS rather than falling back to cleartext, and applies backpressure so
  a slow uplink cannot grow its buffer without limit (1ae226c, d4c00ba, 3cbcd06).
- Opening a project file no longer starts the cameras, microphones, displays and
  windows it names: they are held until the user allows them, all at once from the
  prompt or one at a time from the Held badge's Allow button in Sources or the
  Inspector. The hold takes effect before any capture starts and survives undo and
  redo (ad90551, 0a46032, 7e6b395, cd57095, 6e7ff43).
- Image and timeline media paths from a project file must be local drive-absolute
  paths, checked before the file system is asked about them, so a UNC path cannot
  make Windows authenticate to another host and a protocol string cannot reach
  ffmpeg as an input. A restored render job's output path must pass the same rule
  (a60fec6, 7574924).

#### Fixed

- The encoder's named pipes, which carry the screen and microphone, admit only
  SYSTEM and the current user and refuse remote clients; the default descriptor
  let any local account, including anonymous, read them. Failing to build the
  descriptor aborts the recording (8b97d77).
- The stream key is removed from ffmpeg output shown in error dialogs, including a
  key split across two reads, and FFREPORT is removed from ffmpeg's environment so
  no log file records the command line (69e66b9, 8b97d77).
- ffprobe is started by its resolved path, so a file named ffprobe.exe in the
  application or current directory cannot run in its place (172d7f2).
- Values from project and media files are bounded before use: timeline numbers,
  probed durations, source ids (1 to INT_MAX - 1) and a 32 MB cap on project
  files. A source id at the top of the range used to make the saved project
  impossible to open (bd75eb0, c50879d).
- Render jobs restored at launch are checked by the same rules as new ones, and a
  job that fails them is marked failed instead of run (f0e7a7a).
- The Twitch sign-in opens only an https verification address and shows it as plain
  text (bd75eb0).
- A custom stream URL must be rtmp or rtmps with a host. An unusable entry was
  stored, then replaced on the next load by the placeholder template, so the next
  Go Live sent the stream key to that placeholder host (31768e5).
- A credential the Windows Credential Manager refuses to store is reported and the
  tokens are kept for the session, instead of losing a rotated Twitch refresh token
  silently (28c903d).
- Signing out of Twitch while a token refresh is in flight no longer stores the
  tokens again (1b8a66b).

### Documentation

- The README, ARCHITECTURE and PROJECT_FORMAT documents describe the current code,
  including the named-pipe recording path, the capture backends and retries, the
  audio bus, the project format's trust rules and the layer order, which two of
  them had backwards (3bfb243, c258ba9, 60dc5b3, ce8fa83).
- ADR-0001 to ADR-0003 record the timeline source references, the render job
  snapshot and the render worker, and their status notes say what the first render
  slice does not implement (4a7be26, 6b0a5d4, 46cc483, e9d2d71, 3bfb243).
- Source comments that described removed designs were corrected (a183158).
- The HTML and JSX design prototype the Qt interface was converted from is kept in
  MalloyStudioJS/ (0c45de8).
- This changelog gained an Unreleased section (e60bf41, adbf1c1, ce8fa83).

### Tests and tooling

- Four test executables join the model tests: capture handoff and capture lifetime
  (b594ab9), pipe I/O (2051bdf) and output settings (43a3e06).
- Model tests cover the render queue's retry, cancel and pause, the timeline
  inside the project file, and registries given missing or corrupt stores
  (6be7f74).
- Tests no longer depend on the machine or the user's data: the mixer silence test
  mutes its own inputs, and the registry tests keep out of the user's folders and
  run no ffprobe (4ef796d, 53c1386).
- A recording test holds sound and picture within 60 ms of each other, and the
  recording tests pace their audio by elapsed time with bounds taken from repeated
  runs (58ddc3c, 450d731, c4130c5, adbf1c1).
- Tests that passed whatever the behaviour now fail without it: shortcut
  registration, the process tree kill and the capture gate's serialisation
  (70e24f8, b92eb5f, d3f1693). The encoder registry test expects a software stream
  to add only -maxrate and -bufsize to a file's arguments (48c9a3e).

## v7 - 2026-05-18

Released as commit 2c8b3a0.

### Recording and streaming

#### Added

- Each encoder has its own low-latency tune for streaming: `zerolatency` for
  libx264 and libx265, `ull` for NVENC, and none for Quick Sync and AMF, which
  reject `-tune`.
- Recording and streaming errors carry the last 4 KB or so of ffmpeg's error
  output behind the error dialog's Show Details button. ffmpeg's error output is
  read by the application instead of being passed to the console.
- While streaming, the controls bar shows the bitrate and dropped frames parsed
  from ffmpeg's progress lines beside the LIVE timer.
- Quality presets in Output Settings (1080p60 High Quality, 1080p30 Balanced,
  720p60 Streaming, 720p30 Fast, 4K30 Cinematic) fill resolution, frame rate, CRF
  and bitrate; editing any of those fields returns the preset to Custom.
- Quality presets in Stream Settings for Twitch, YouTube and low latency set the
  bitrate and keyframe interval. A hint gives the service's recommended bitrate
  and warns when the bitrate is above the platform's cap.

#### Fixed

- Start Stream no longer fails with ffmpeg error -22 on a hardware encoder (NVENC,
  Quick Sync, AMF): streaming builds its encoder arguments from the encoder
  registry, as recording already did, instead of libx264-only flags.
- The keyframe interval set in Stream Settings reaches the stream; it was dropped,
  and every stream used a fixed 2 second interval.
- Streams always set a keyframe interval (`-g`, the frame rate times the keyframe
  seconds), because the software encoders' arguments omitted it and an ingest can
  end a stream that has no keyframes.

### Sources and audio

#### Added

- Microphone and Window Capture in the Sources Add dialog. The model already
  supported both; a microphone is chosen from the WASAPI capture devices and
  becomes an audio input source in the current scene.
- A + Add Microphone button in the audio mixer, as a second way to add one.
- Per-input mute hotkeys: the hotkeys dialog lists a Mute row for each mixer input,
  named after its device (desktop audio shows as "Mute: Desktop Audio"), and the
  bindings are saved in the settings.
- Each filter can be switched off with an Enabled checkbox in the Inspector. A
  disabled filter is skipped when composing, and project files without the flag
  load with every filter enabled.

### Interface

#### Added

- Empty-state hints in the Scenes, Sources and audio mixer panels.
- Sources can be reordered by dragging and renamed with F2, as scenes already
  could.

### Tests and tooling

- Nine model tests cover the changes above, taking the suite from 22 to 31 test
  functions. The streaming pipeline is no longer `final`, so a test can subclass
  it, and the ffmpeg progress-line parser is a public static function that can be
  tested on its own.

## v6

No entry was written for v6 at the time. The v7 notes refer to it, and the
imported tree (ad01b7a) is the record of its state. That tree has the following,
which no earlier entry records:

- Live streaming over RTMP to Twitch, YouTube or a custom server, with the stream
  key kept in the Windows Credential Manager.
- Hardware encoders (NVENC, Quick Sync and AMF, each for H.264 and HEVC) offered
  beside libx264 and libx265 when ffmpeg lists them, through one encoder registry
  used for recording and streaming.
- A replay buffer that keeps the last seconds of program video and audio in memory
  and saves them to a file on a hotkey.
- Studio mode: a staged scene beside the program output, taken to air with a
  transition.
- Global hotkeys for recording, streaming, saving a replay, the studio transition
  and switching to scenes one to nine.
- Chroma key, blur and scroll filters.
- A 50 Hz program audio mixer with per-input volume, mute and pan, and a master
  limiter.

## v5

### Window Capture

- New source type `WindowCapture` captures a specific application window via HWND.
- `WindowCapture` worker thread uses `PrintWindow(PW_RENDERFULLCONTENT)` (with
  `BitBlt` fallback) and `GetDIBits` to produce a `QImage` at about 30 fps.
- `WindowPickerDialog` lists all visible top-level windows with process names (via
  `QueryFullProcessImageNameW`) for easy selection.
- Inspector shows the current window title and a "Change Window..." button.
- `CaptureController` manages window sessions in a separate `QHash` alongside the
  existing display sessions; existing tests are unaffected.
- Windows that close or become invalid emit `windowClosed`, and the item shows a
  placeholder instead of crashing.

### Source Filters

- New `FilterEffect` hierarchy: `CropFilter`, `OpacityFilter`,
  `ColorCorrectionFilter`.
- Filter chain stored on `SceneItem` (`QList<FilterEffect*>`, QObject-parented).
- Serialized as an optional `"filters": [...]` array per item; files without it
  load with empty chains (no schema version bump).
- `duplicate()` deep-copies the filter chain via `FilterEffect::clone()`.
- Rendering: `PreviewWidget::drawItem()` paints directly when the chain is empty,
  and otherwise renders to a temporary `QImage`, applies the filters and blits the
  result.
- `OpacityFilter::apply()` is a no-op; `PreviewWidget` accumulates opacity values
  and calls `painter.setOpacity()` to avoid a pixel loop.
- `ColorCorrectionFilter` uses a 256-entry LUT for brightness and contrast and a
  Rec.601 luma-weighted saturation loop.
- Inspector Filters section: `QGroupBox` with list, Add/Remove/Up/Down toolbar, and
  per-type property pages for Crop (4 spinboxes), Opacity (slider), and Color
  Correction (3 sliders). All edits undo correctly.

### Encoder / Output Settings

- New `OutputSettings` struct (header-only) persisted in `QSettings` (app-wide, not
  per-project).
- Fields: width, height, fps, videoCodec, crf, preset, audioCodec,
  audioBitratekbps, container.
- Defaults match the v4 hard-coded values, so existing workflows are unchanged.
- `OutputSettingsDialog` (Edit > Output Settings...): resolution spinboxes with
  presets, FPS, video codec, CRF slider, preset, audio bitrate, container.
- `Recorder::start()` now accepts `OutputSettings` and drives all ffmpeg arguments
  from it. A `-vf scale=W:H` filter is added when the requested output resolution
  differs from the canvas native 1920x1080.

### Per-Source Audio (AudioInput)

- New source type `AudioInput` links a WASAPI capture device to the Audio Mixer.
- `AudioController::reconcileInputs(QStringList)` dynamically starts and stops
  `WasapiCapture` workers based on the device IDs of visible AudioInput sources in
  the current scene.
- `AudioController::enumerateInputDevices()` enumerates active eCapture endpoints
  via `IMMDeviceEnumerator` and returns `{deviceId, friendlyName}` pairs.
- `SceneCollection::audioInputsChanged` is emitted on scene switch and item
  visibility changes; `MainWindow` wires it to `reconcileInputs`.
- Inspector shows a `QComboBox` populated with available capture devices.

### Other changes

- `CaptureController::windowFrameReady` / `windowFrameCleared` signals wired to
  `PreviewWidget::updateWindowFrame` / `clearWindowFrame` in `MainWindow`.
- Inspector panel wrapped in a `QScrollArea` to accommodate the new Filters
  section.
- `propsys`, `user32`, and `gdi32` added to both CMake link targets.
- 4 new unit tests: `windowCaptureKeyIsStable`, `filterChainRoundTrips`,
  `outputSettingsRoundtrip`, `perSourceAudioReconcileActivates`.
- Test count: 12 (was 8).

## v4

- WASAPI desktop-audio loopback capture (`WasapiCapture` QThread worker).
- `AudioController` owns the loopback worker and exposes a
  `mixedSamples(QByteArray pcm)` bus.
- `AudioMixerPanel` with per-input VU meter (`VuMeter`), volume slider and mute
  button.
- Real recording pipeline via `Recorder`: ffmpeg subprocess, BGRA video via stdin,
  PCM audio via a Windows named pipe, muxed to MP4 (libx264 and AAC).
- `FrameSupplier` abstraction in `Recorder` (enables test injection without a real
  GPU).
- Volume and mute state persisted in `QSettings` per input ID.
- `"audio": {}` reserved key added to project JSON (v2, no version bump) for future
  routing.

## v3

- DXGI Desktop Duplication capture (`DxgiCapture` QThread, `DxgiCaptureSession`).
- `CaptureController` with `SessionFactory` injection and `reconcile()`
  auto-management.
- `MonitorPickerDialog` for selecting adapter and output via `MonitorInfo`
  enumeration.
- Image source (renders a static image from disk, cached per path in
  `PreviewWidget`).
- Text source (QPainter-rendered text).
- Color Block source (solid fill).
- Browser source (placeholder grey box).
- Cut and Fade scene transitions: `PreviewWidget::beginFadeTransition()` and
  `QTimeLine`.
- Off-screen 1920x1080 frame cache in `PreviewWidget::cachedComposedFrame()` for
  the recorder (avoids extra paint cycles).
- `ControlsBar`: transition type, duration, and a Record button with an elapsed
  timer.

## v2

- Source library separated from scene items; items hold `sourceId` integer
  references.
- Multiple items can reference the same source (shared source pattern).
- Source GC: `collectUnusedSources()` removes sources with zero references.
- Snapshot-based undo and redo via `QUndoStack` and `SnapshotCommand`.
- Edit-session coalescing for text editing (`beginEditSession` /
  `commitEditSession`).
- Drag-resize in `PreviewWidget` with corner handles and a `DragMode` enum.
- Canvas snap guides (centre snap and edge snap via `MalloyCanvas::snapRect()`).
- Fit / Fill / Center / Reset transform buttons in `InspectorPanel`.
- Project JSON v2: `"sources"` array and `"scenes"` with `"sourceId"` references.
- Auto-migration of v1 projects (inline source objects to library).

## v1

- Initial prototype.
- `SceneCollection`, `Scene`, `SceneItem`, `Source` model.
- `ScenesPanel`, `SourcesPanel`, `InspectorPanel` (read-only).
- `PreviewWidget` basic paint (no drag, no capture, placeholder rendering).
- `ProjectDocument` load/save (`.malloy.json`).
- Project JSON v1: source embedded inside each item object.
