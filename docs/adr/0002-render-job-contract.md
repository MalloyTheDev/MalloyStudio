# ADR-0002: Render jobs carry structured settings and a timeline snapshot

## Status

Proposed

## Context

`RenderJob` (`src/recording/RenderQueue.h`) describes a job the way a mockup does:

- `target` is a display string, `"1080p60 NVENC 24 Mb/s"`, with no machine-readable
  settings behind it;
- `outputPath` is a directory in the only caller, not a file;
- nothing identifies *what* is being rendered. There is no project reference and no
  timeline.

The only producer is the Render workspace "New render" button, which passes hardcoded
values. The Editor's Export button is unwired (issue #3) because there is nothing valid
for it to enqueue.

A real worker needs an unambiguous answer to two questions at the moment it starts:
which encoder settings, and which timeline. The second question is where the design
choice lives, because the user can keep editing after enqueuing.

## Decision

**1. Encoder settings become structured, reusing `OutputSettings`.**

`OutputSettings` already models exactly these fields and is already the shared vocabulary
for recording and streaming. Give it `toJson()` / `fromJson()` and embed it in the job as
`output`. `target` stays in the file as a display string for history rows, but it is
derived from `output` when a job is created, never parsed.

**2. The job embeds a snapshot of the timeline taken at enqueue time.**

```jsonc
{
  "schema": 1,                                   // NEW: renderqueue.json schema
  "jobs": [{
    "id": "8f3c...", "name": "Spire ep14.mp4",
    "project": "Spire of the Hollow Sun",
    "projectPath": "F:/Projects/spire.malloy.json",   // NEW: provenance, informational
    "outputPath": "F:/Renders/spire-ep14-20260906-1420.mp4",  // NEW: a file, not a dir
    "target": "1920x1080 60fps libx264 CRF 23",   // derived display string
    "output": { "width": 1920, "height": 1080, "fps": 60, "videoCodec": "libx264",
                "crf": 23, "preset": "veryfast", "audioCodec": "aac",
                "audioBitratekbps": 192, "container": "mp4", "bitrateKbps": 4500 },
    "timeline": [ /* clip objects, ADR-0001 shape */ ],  // NEW: snapshot
    "state": 0, "progress": 0, "error": "", "finishedAt": ""
  }]
}
```

A queued render is therefore reproducible: it renders what the timeline looked like when
the user pressed Export, no matter what they do next, and a retry after a failure renders
the same thing rather than whatever is on screen now.

**3. Legacy jobs are retired, not guessed at.**

On load, a `Pending` or `Active` job with no `output` or no `timeline` is set to `Failed`
with `error = "Job predates render settings and cannot be rendered. Enqueue it again."`
`Completed` and `Failed` jobs load unchanged, because they are history and never re-run
without an explicit retry.

**4. `outputPath` is a full file path**, produced at enqueue from the project name, a
timestamp and the container extension, following the pattern
`MainWindow` already expands for recordings. Enqueue refuses a path whose parent
directory does not exist rather than failing minutes later inside ffmpeg.

## Alternatives considered

**Render the live timeline when the job starts.** Simplest to write and wrong in
practice: the output depends on when the worker happened to reach the job, editing during
a render changes the result mid-flight, and a retry can produce a different file from the
one that failed.

**Store a project path and re-read the file at start.** Better than reading live state,
but it still races: the project may be unsaved, saved with different content, moved, or
deleted between enqueue and start. It also makes "render this unsaved timeline"
impossible, which is the common case right after an edit.

**Per-job sidecar files for the snapshot.** Keeps `renderqueue.json` small, at the cost
of a second atomicity problem (job row and snapshot must stay in step) and orphan cleanup.
Timelines are small JSON; revisit only if job files grow past the point where rewriting
the queue file per progress update is measurable.

## Consequences

- `renderqueue.json` grows by roughly the size of a timeline per queued job. Completed
  jobs keep their snapshot, so `clearCompleted` becomes the mechanism that bounds the
  file. That is acceptable; if it is not, dropping the snapshot on completion is a
  compatible follow-up.
- The queue currently saves on every progress change. With snapshots embedded, progress
  updates should stop rewriting the whole file on every tick: persist on state
  transitions and throttle progress writes.
- Export in the Editor (issue #3) becomes implementable: it has settings, an output path
  and a snapshot to hand over.
- Retry becomes meaningful for the first time, because there is something deterministic
  to retry.

## Security / privacy impact

The job file gains absolute paths for media, project and output, with the same
disclosure property described in ADR-0001. No credentials are involved: unlike the
streaming path in issue #10, a render has no stream key, and nothing secret should ever
be placed in a job or in an ffmpeg argument list.

## Validation plan

Model tests, no ffmpeg required:

- Job JSON round-trips `output`, `timeline`, `projectPath` and a file-shaped
  `outputPath`.
- `OutputSettings::toJson` / `fromJson` round-trip, including defaults for missing keys.
- A legacy pending job (no `output`, no `timeline`) loads as `Failed` with the stated
  error, while a legacy completed job loads unchanged.
- Enqueue rejects an output path whose parent directory is missing.
- The snapshot is independent: enqueue, mutate the source timeline, and confirm the job's
  copy is unchanged.

## Rollback or migration notes

`schema` starts at 1; a file without it is schema 0 and is read with the legacy rules
above. The added keys are additive, so an older build loads the file, ignores them, and
keeps its simulated behavior. Rollback is reverting the commit: existing queue files stay
readable because every new key is optional.
