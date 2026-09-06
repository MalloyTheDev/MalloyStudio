# ADR-0003: Render worker is a per-job ffmpeg process driven by a generated filter graph

## Status

Proposed

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
  `error`, so error display in the workspace needs a pass.
- `RenderQueue` stops owning a simulated worker and becomes a scheduler over
  `RenderPipeline`. `advanceForTest()` and the tests that use it need replacing with
  tests that drive the state machine directly.
- Rendering competes with capture for CPU. Rendering while recording or streaming is a
  real risk on a busy machine; the queue's existing pause is the mitigation offered to
  the user, and automatic pausing during capture is a follow-up decision, not part of
  this ADR.

## Security / privacy impact

- No secrets ever reach the argument list. Issue #10 records that the streaming path
  leaks the stream key through the process command line; renders have no key, and this
  design must not introduce one.
- User-controlled file names are confined to argv, which is verified to handle spaces,
  apostrophes and brackets, and are never concatenated into the filter graph.
- The temporary graph file is written to the application temp directory and deleted when
  the job ends, including on failure and cancellation. It contains paths only.
- Output paths are validated before the process starts, so ffmpeg is never asked to
  create a file in a directory the app has not checked.

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

## Rollback or migration notes

The change is additive at the file-format level (ADR-0002 covers the schema). Rollback is
reverting to the timer-driven `tick()`, which needs no data migration: jobs left
`Pending` are simply picked up by whichever worker is present. A job left `Active` when
the app exits is already resumed as `Pending` on the next launch, and that behavior must
be preserved, since a killed ffmpeg leaves no resumable state.
