# ADR-0003: Render worker is a per-job ffmpeg process driven by a generated filter graph

## Status

Accepted. Implemented in d2717d2 (2026-09-06) as a first slice narrower than the
decision below, and amended since. The Amendments section at the end records each change
with its date; the sections before it are the decision as recorded. As of 2026-09-21 the
implementation differs from the decision in these ways:

- A clip with a non-zero rotation, audio pan or channel mapping fails the render with a
  message naming it; those mappings are not built yet.
- Opacity is applied through `format=yuva420p` and `colorchannelmixer=aa=`.
- A render takes at most 32 clips. The segment-and-concat fallback is not built, so a
  longer timeline cannot be rendered.
- Every `sourcePath` must be a local, drive-absolute path (`MediaPathPolicy`), and
  numeric clip values are range-checked before any arithmetic; see PROJECT_FORMAT.md,
  sections 9.6 and 11.2.
- Before ffmpeg starts, each source is measured with ffprobe, and a clip that runs past
  the end of its source is cut and reported (ADR-0001, contract 1).
- Cancel ends ffmpeg and every process it started at once, rather than terminating and
  then killing it after a grace period.
- A render reaches 100 percent when ffmpeg exits with code 0 and the output file exists;
  `progress=end` is not consulted.
- Only clips marked `audio` contribute sound, mixed together with `amix`; a video clip's
  own sound is not used.

## Context

`RenderQueue::tick()` advances the active job by a random 3 to 10 percent every 400 ms
and always completes. Issue #4 proposes replacing it with "a rendering instance of
`EncoderPipeline` that reads frames and audio samples from the `EditorWorkspace` timeline
model". That is not achievable with the current `EncoderPipeline`:

- `start()` takes a `TimedFrameSource` and a `TimedPcmSource` and pulls frames on a
  `QTimer` at the output frame rate. It is a realtime capture stack: no file inputs, no
  seeking. Driving it from a timeline caps rendering at 1x realtime and still needs a
  decoder in front of it.
- Its progress parser extracts `bitrate=` and `drop=` only, never stream position, so it
  cannot report a percentage.

Measurements taken on this machine with ffmpeg 8.1.1, which set the expectations below:

| Check | Result |
|---|---|
| Two video layers with per-clip `trim`/`setpts` placement, `scale`, `overlay`, plus a two-stream `amix`, 6 s of 1920x1080p60, libx264 veryfast | exit 0 in 2.54 s wall, about 2.4x realtime; output duration 6.000 s confirmed by ffprobe |
| `-progress pipe:1 -nostats` | emits `out_time_us`, `out_time`, `speed`, `fps`, `frame`, `total_size`, `progress=continue|end` |
| Final progress block | reported `out_time=00:00:04.0` for a 6.0 s output |
| `-filter_complex_script <file>` | exit 0, output identical in shape to the inline form |
| Path containing spaces, an apostrophe and brackets passed as `-i` | exit 0 |

## Decision

Add a render path that is a sibling of the capture path rather than a subclass of it.

**Two units, split so that most of the logic is testable without ffmpeg.**

1. `TimelineGraphBuilder` (pure). Input: a timeline snapshot (ADR-0002) plus
   `OutputSettings`. Output: the ffmpeg argument list, the filter graph text, and a
   diagnostic list. It resolves clip order, computes each clip's source segment from
   `sourceIn`, `dur` and `speed.factor` (ADR-0001), maps `transform` to `scale`,
   `overlay` and `rotate`, `opacity` to `format=rgba,colorchannelmixer=aa=`,
   `audioParams` to `volume` and `pan`, and mixes tracks with `amix`. It refuses a
   timeline that contains an unlinked clip and names the clip.
2. `RenderPipeline` (process lifecycle). Owns one `QProcess`, wires progress, failure,
   cancellation and cleanup, and reports to `RenderQueue`.

**Process shape.** One ffmpeg per job, one active job at a time, which is the queue's
existing behavior:

- media files enter as `-i` arguments only, never interpolated into the graph. The graph
  addresses them as `[0:v]`, `[1:a]` and so on. This is both the correct escaping story
  and the security boundary: `:` and `'` are filter metacharacters, and a user file name
  containing them would otherwise change the graph.
- the graph is written to a temporary file and passed with `-filter_complex_script`. A
  long timeline can produce a graph of tens of kilobytes, and the Windows command line is
  capped at about 32767 characters, so the inline form has a size cliff that the script
  form does not.
- progress comes from `-progress pipe:1 -nostats`. Percentage is
  `out_time_us / total_timeline_us`, clamped monotonically, because the closing block was
  observed reporting a lower time than the peak. `progress=end` plus exit code 0 forces
  100.

**Failure and cancellation.**

- ffmpeg missing (`QStandardPaths::findExecutable("ffmpeg")` empty): the job fails
  immediately with a message saying so, matching how record and stream buttons already
  degrade.
- Non-zero exit: the job becomes `Failed` and `error` carries a short summary plus the
  tail of stderr, the same convention `EncoderPipeline::errorOccurred` uses for the
  recording path, and the partial output file is deleted.
- Cancel: `terminate()`, then `kill()` after a short grace period, then delete the
  partial file. Cancelling an active job must not leave a half-written file that
  `RecentRecordings` or `MediaRegistry` would later present as media.
- Output collisions: refuse to overwrite an existing file; the enqueue-side timestamped
  name makes this rare, and silently clobbering a previous render is worse than failing.

**Fallback for graphs that are too large.** Above a threshold of inputs, render each
track segment to an intermediate file and join with the concat demuxer. This is slower
and touches disk more, so it is a fallback rather than the default, and it is not part of
the first slice.

## Alternatives considered

**Reuse `EncoderPipeline`,** as issue #4 proposes. Rejected on the evidence above: wrong
grain, capped at realtime, no position reporting, and it would need a decoder bolted onto
a class whose whole design is "pull the latest live frame".

**Segment and concat as the default.** Simpler individual graphs and easier to reason
about per clip, at the cost of extra encode passes, generation loss unless intermediates
are lossless, more disk traffic and more processes to supervise. Kept as the escape hatch
for oversized graphs.

**A C++ frame server: composite in the app, pipe rawvideo to ffmpeg.** Gives exact parity
with the editor preview, since the same compositing code would produce both. Rejected for
now because it reimplements compositing that ffmpeg already does, and it is the slowest
path to a first working render. It becomes the right answer if per-clip effects grow past
what filter graphs express cleanly, and the `TimelineGraphBuilder` boundary is where that
swap would happen.

## Consequences

- Renders run faster than realtime rather than pretending to run at all, and progress and
  ETA become real numbers.
- This introduces the first genuine `RenderJob::Failed` state. The queue model and the
  Render workspace already have retry, cancel and pause, but nothing has ever set
  `error`, so error display in the workspace needs a pass. *(Done: the Render workspace
  lists failed jobs with their error and a Retry button. A failed job cannot be removed
  from the queue; see ADR-0002.)*
- `RenderQueue` stops owning a simulated worker and becomes a scheduler over
  `RenderPipeline`. `advanceForTest()` and the tests that use it need replacing with
  tests that drive the state machine directly. *(Done in d2717d2: the simulated tick and
  `advanceForTest()` are gone, and the queue tests drive jobs that fail to start
  deterministically without ffmpeg.)*
- Rendering competes with capture for CPU. Rendering while recording or streaming is a
  real risk on a busy machine; the queue's existing pause is the mitigation offered to
  the user, and automatic pausing during capture is a follow-up decision, not part of
  this ADR. *(2026-09-21: automatic pausing has not been built. Pausing the queue stops
  the next job from starting; a render already running continues.)*

## Security / privacy impact

- No secrets ever reach the argument list. Issue #10 records that the streaming path
  leaks the stream key through the process command line; renders have no key, and this
  design must not introduce one.
- User-controlled file names are confined to argv, which is verified to handle spaces,
  apostrophes and brackets, and are never concatenated into the filter graph.
- The temporary graph file is written to the application temp directory and deleted when
  the job ends, including on failure and cancellation. It contains paths only.
  *(Amended: it is written to the system temporary directory
  (`QStandardPaths::TempLocation`) as `malloy_render_<uuid>.txt`. It holds the filter
  graph, which addresses inputs by index and contains no paths; the paths are only in
  ffmpeg's argument list.)*
- Output paths are validated before the process starts, so ffmpeg is never asked to
  create a file in a directory the app has not checked.
- *(Amended 2026-09-13, a60fec6: media paths come from project files, which may not be
  the user's. A UNC `sourcePath` would make Windows authenticate to the host it names as
  soon as the path is checked, and a URL would be read by ffmpeg as an input of the file
  author's choosing, so every `sourcePath` must pass `MediaPathPolicy` before anything,
  ffprobe included, touches it.)*

## Validation plan

Without ffmpeg, in the model test suite:

- Graph builder: a single video clip with an in-point produces the expected `trim` and
  `setpts` values; two overlapping clips produce two `overlay` stages in track order; a
  speed factor of 2.0 consumes twice the source span; an unlinked clip is refused with the
  clip named; an empty timeline is refused.
- Progress: `out_time_us` maps to percentage against a known timeline duration; a
  decreasing sample does not lower reported progress; `progress=end` with exit 0 yields
  100.
- Queue: a failing pipeline moves the job to `Failed` with a non-empty `error`; cancel
  removes the job and deletes the partial file.

With ffmpeg, as a runtime check rather than a unit test: enqueue a two-clip timeline from
the Editor, watch the Render workspace reach 100 percent, and confirm with ffprobe that
the output duration equals the timeline duration. The commands used to establish the
numbers in this ADR are the model for that check.

*(2026-09-21: the graph builder is covered in `tests/model_tests.cpp` by
`timelineGraphPlacesTrimsAndScalesClips`, `timelineGraphScalesPositionsFromCanvasToOutput`,
`timelineGraphMixesAudioAndKeepsPathsOutOfTheGraph`, `timelineGraphRefusesWhatItCannotRender`,
`timelineGraphBoundsNumbersBeforeArithmetic` and
`timelineGraphClampsClipsThatRunPastTheirSource`; the queue by
`renderQueueProcessesAndPersists`, `renderQueueRetryCancelClear` and
`renderQueuePauseHoldsPendingJobs`; source measurement by
`aRenderIsNotHeldUpByAProberThatHangs`; and a real render, when ffmpeg and ffprobe are
present, by `renderJobReportsAClipCutAtTheEndOfItsSource`. No test asserts the progress
percentages described under Validation plan.)*

## Rollback or migration notes

The change is additive at the file-format level (ADR-0002 covers the schema). Rollback is
reverting to the timer-driven `tick()`, which needs no data migration: jobs left
`Pending` are simply picked up by whichever worker is present. A job left `Active` when
the app exits is already resumed as `Pending` on the next launch, and that behavior must
be preserved, since a killed ffmpeg leaves no resumable state.

## Amendments

Dated records of where the implementation refines or extends the decision.

**2026-09-06, d2717d2: first slice.** Implemented with the limits the Status lists.

**Implementation details as of 2026-09-21**, which the decision left open. Not all of
them date from the first slice.

- Video clips are composited onto the black background in descending `track` order, ties
  in timeline order, each over the ones before it. The editor lists its tracks as V3
  (`track` 0), V2 (1) and V1 (2) from top to bottom, so a render draws V3 over V1, as the
  editor shows them. Until 2026-09-21 the order was ascending, which drew V1 over V3.
- Only clips marked `audio` contribute sound. Each is trimmed, retimed with a chain of
  `atempo` filters, given its gain with `volume` and placed with `adelay`; the clips are
  mixed with `amix` without normalisation, or passed through when there is only one.
- A clip is scaled to `transform.scale` percent of the output width and height, so the
  source's aspect ratio is not preserved.
- Encoder arguments come from `EncoderRegistry`, as for recordings, followed by
  `-pix_fmt yuv420p`, the audio codec and bitrate, and `-t` set to the timeline length.
  A codec the registry does not know fails the render.
- ffmpeg is started with `-hide_banner -loglevel error -nostdin -progress pipe:1 -nostats`,
  and a failure message carries the last 4000 characters of its standard error.
- Progress is `out_time_us` over the timeline length, never lower than the last value
  reported. On exit code 0 the job reaches 100 percent if the output file exists, and
  fails with "ffmpeg reported success but wrote no file." if it does not.
- An existing output file is refused when the job starts and again just before ffmpeg is
  launched. ffmpeg is given neither `-y` nor `-n`, and a failed render deletes the file at
  the output path, so a file created there during the render is lost (#93).

**2026-09-13, a60fec6 and bd75eb0: untrusted input.** Every `sourcePath` must pass
`MediaPathPolicy` before the existence check, and clip numbers are bounded and required
to be finite before any arithmetic (ADR-0001, Amendments).

**2026-09-21, 1d857cd: cancellation (#85).** `terminate()` asks a process to close its
windows, which a windowless ffmpeg does not have, so every cancel waited out the grace
period on the GUI thread; and `kill()` ended only the process started, which for an
ffmpeg installed through a package manager's launcher left the real encoder running.
Cancel now ends ffmpeg with every process it started (`ProcessTree::kill`), waits up to
2 seconds for it to finish, and deletes the partial output. The same applies to a probe
cancelled or timed out.

**2026-09-21, ce40a16: placement.** `transform.x` and `transform.y` are canvas pixels and
are scaled to the output size before `overlay`, rounded in 64-bit arithmetic.

**2026-09-21, a47c7bb: measuring sources (ADR-0001 contract 1, #55).** The pipeline gains
a stage before ffmpeg:

1. `start()` builds the graph once without source lengths, so everything that can be
   refused without them (an unlinked clip, a refused or missing path, a value out of
   range, too many clips) is still refused synchronously.
2. Each distinct `sourcePath` is then measured with
   `ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1`,
   one file at a time from the event loop. A probe that has not answered after 10 seconds
   is ended with its process tree. Only a finite, positive answer is used.
3. The graph is built again with the lengths, clips that run past their source are cut,
   and ffmpeg is launched. The cut is reported through a new `adjusted(note)` signal and
   stored on the job (ADR-0002, Amendments).

When ffprobe is not installed, the render is launched at once without measuring, as
before. Cancelling while sources are being measured ends the probe; no output file has
been started, so there is nothing to delete.

Known gaps in the render path, tracked as issues: still images render as a single frame
(#100); output is neither converted to nor tagged as BT.709 (#92); a job whose hardware
encoder is unavailable on this machine starts and then fails in ffmpeg instead of being
refused (#106); and a failed render can delete a file it did not write (#93).
