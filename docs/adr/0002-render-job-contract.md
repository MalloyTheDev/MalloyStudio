# ADR-0002: Render jobs carry structured settings and a timeline snapshot

## Status

Accepted. Implemented in b506592 (2026-09-06). Since amended: restored jobs are
validated like enqueued ones (f0e7a7a, 2026-09-13, #47); a restored job's output path is
held to the media path policy (7574924, 2026-09-21, which added the last three sentences
of decision 4); and jobs carry a `note` recording what a render had to change
(a47c7bb, 2026-09-21, #55). The Amendments section at the end gives the job schema as it
now stands and the rules that have moved on; the other sections are the decision as
recorded.

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
directory does not exist rather than failing minutes later inside ffmpeg. A restored
`Pending` job is held to the same checks, and first to the rule project media paths
follow (`MediaPathPolicy`): an `outputPath` that is not a drive-absolute local path is
set to `Failed` before the filesystem is asked about it, because asking about a UNC path
is what makes Windows authenticate to the host it names.

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
  compatible follow-up. *(2026-09-21: still so, with a gap. `clearCompleted` removes
  completed jobs only, and cancel removes only pending and active ones, so a failed job
  and its snapshot stay in the file until a retry of it completes; one that can never be
  rendered stays for good. On read the store is bounded at 16 MiB; see Amendments.)*
- The queue currently saves on every progress change. With snapshots embedded, progress
  updates should stop rewriting the whole file on every tick: persist on state
  transitions and throttle progress writes. *(Done, more strictly than proposed:
  progress changes are never saved on their own. The store is written on
  enqueue, on each state change, on retry, cancel and `clearCompleted`, and when a note
  is recorded, so a persisted `progress` is the value at the last such write.)*
- Export in the Editor (issue #3) becomes implementable: it has settings, an output path
  and a snapshot to hand over. *(Done: Export queues a render; #3 is closed.)*
- Retry becomes meaningful for the first time, because there is something deterministic
  to retry.

## Security / privacy impact

The job file gains absolute paths for media, project and output, with the same
disclosure property described in ADR-0001. No credentials are involved: unlike the
streaming path in issue #10, a render has no stream key, and nothing secret should ever
be placed in a job or in an ffmpeg argument list.

*(Amended 2026-09-13 and 2026-09-21: the store is also an input. The queue is restored
and started at launch, so anything able to write `renderqueue.json` could otherwise
choose an ffmpeg invocation that runs with no user action (#47). Restored jobs are
therefore held to the checks an enqueued job gets, and their output paths to the media
path policy; see Amendments.)*

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

*(2026-09-21: covered in `tests/model_tests.cpp` by `outputSettingsJsonRoundTrips`,
`renderJobCarriesSettingsAndSnapshot`, `renderQueueProcessesAndPersists`,
`renderQueueRetiresJobsWithoutASnapshot` and `renderQueueRejectsUnrenderableRequests`;
the restore rules by `restoredRenderJobsAreValidatedLikeEnqueuedOnes` and
`restoredRenderJobsMustWriteToALocalDrive`; and the note, with ffmpeg and ffprobe
present, by `renderJobReportsAClipCutAtTheEndOfItsSource`.)*

## Rollback or migration notes

`schema` starts at 1; a file without it is schema 0 and is read with the legacy rules
above. The added keys are additive, so an older build loads the file, ignores them, and
keeps its simulated behavior. Rollback is reverting the commit: existing queue files stay
readable because every new key is optional.

## Amendments

Dated records of where the implementation refines or extends the decision.

### The job schema as it stands (2026-09-21)

The store is `renderqueue.json` in the per-user application data directory
(`QStandardPaths::AppDataLocation`). Each job has this shape; the `note` key and
`output.keyframeSec` are additions to the schema shown in decision 2.

```jsonc
{
  "schema": 1,
  "jobs": [{
    "id": "8f3c2a9e-5b1d-4c7a-9e0f-2d6b8a4c1e37",
    "name": "Spire ep14-20260921-142000.mp4",
    "project": "Spire ep14",
    "projectPath": "F:/Projects/Spire ep14.malloy.json",
    "outputPath": "F:/Renders/Spire ep14-20260921-142000.mp4",
    "target": "1920x1080 30fps libx264 (H.264, software) CRF 23",
    "output": { "width": 1920, "height": 1080, "fps": 30, "videoCodec": "libx264",
                "crf": 23, "preset": "veryfast", "audioCodec": "aac",
                "audioBitratekbps": 192, "container": "mp4", "bitrateKbps": 4500,
                "keyframeSec": 0 },
    "timeline": [ /* clip objects, PROJECT_FORMAT.md section 9 */ ],
    "state": 2, "progress": 100, "error": "",
    "note": "\"spire-ep14.mkv\" runs 0.480 s past the end of its source, so it was cut from 55.500 s to 55.020 s.",
    "finishedAt": "2026-09-21T14:23:05"
  }]
}
```

| Key | Type | Meaning |
|---|---|---|
| `id` | string | A UUID without braces, assigned at enqueue. |
| `name` | string | The output file's name at enqueue. |
| `project` | string | The project's display name, or "Untitled" for a project never saved. |
| `projectPath` | string | The project file the timeline came from, empty for a project never saved. Informational: nothing reads it. |
| `outputPath` | string | Absolute path of the file to write. |
| `target` | string | Display string derived from `output` at enqueue: width x height, frame rate, the encoder's display name, then `CRF n` for a software encoder or `n kb/s` for a hardware one. |
| `output` | object | `OutputSettings` as JSON. `replayBufferSeconds` is not carried. Missing keys take their defaults, and on read every value is normalised as the application settings are (for example width 320 to 7680 and even, an unknown codec replaced with `libx264`). |
| `timeline` | array | The snapshot, in the clip format of PROJECT_FORMAT.md, section 9, as the editor serialised it at Export. |
| `state` | integer | 0 Pending, 1 Active, 2 Completed, 3 Failed. Not range-checked on read. |
| `progress` | integer | 0 to 100, as of the last write of the store. |
| `error` | string | Why the job failed; empty otherwise. |
| `note` | string | What the render had to change to render the timeline at all; empty when nothing was changed. See below. |
| `finishedAt` | string | ISO 8601 date and time of completion; empty until then. |

`schema` is written as 1 and is not read: an object's `jobs` array is read whatever
`schema` says, and a bare array is read as schema 0.

### `note` (a47c7bb, 2026-09-21, #55)

ADR-0001's contract 1 cuts a clip that runs past the end of its source and requires the
cut to be reported. The report is the job's `note`: one line per clip cut, of the form
`"<clip>" runs X s past the end of its source, so it was cut from Y s to Z s.`, with
times to the millisecond. It is cleared when a job becomes Active, so a retry reports
afresh; it is set, and the store written, when ffmpeg is launched after the sources have
been measured and at least one clip was cut; and it is kept whether the render then
completes or fails. The Render workspace shows it under an active job and as a tagged
tooltip on a completed one; a failed job keeps it but does not show it.

### Restoring the queue (f0e7a7a, 2026-09-13, #47; 7574924, 2026-09-21)

On launch the store is read and the queue started. A store that cannot be opened, is
larger than 16 MiB, or does not parse as JSON is read as an empty queue, and the next
write replaces it. Each job is then taken through these steps in order:

1. An `Active` job becomes `Pending` with progress 0, because a killed ffmpeg leaves
   nothing to resume.
2. A `Pending` job that is not renderable becomes `Failed` with the error in decision 3.
   Decision 3 names a missing `output` or `timeline`; the rule implemented is an empty
   `timeline` or an empty `outputPath`, so a job without `output` is not retired, and
   loads with default output settings.
3. A `Pending` job whose `outputPath` is not a local, drive-absolute path becomes
   `Failed`, before anything asks the filesystem about that path.
4. A `Pending` job that fails the enqueue checks (an empty timeline, no output path, an
   output path that is a folder, or an output folder that does not exist) becomes
   `Failed` with the reason.

Refused jobs stay in the queue, visible as failed, rather than being dropped. Retrying a
failed job that is not renderable leaves it failed with the same message.

### Output paths (d2717d2, 2026-09-06)

Decision 4 as implemented: the Export command offers a save dialog whose suggested name
is `<project>-<yyyyMMdd-HHmmss>.<container>` in the recordings folder (the
`recording/lastDir` setting, which recordings and exports share, and which Export
updates), or the Movies folder when none is set. The user may choose any name. Renders
never overwrite: Export refuses a file that already exists, and the render pipeline
checks again when a job starts and once more just before ffmpeg is launched. Enqueue
itself does not check for an existing file. A render that fails still deletes the file at
the output path, so a file created there while the render runs is lost (#93).
