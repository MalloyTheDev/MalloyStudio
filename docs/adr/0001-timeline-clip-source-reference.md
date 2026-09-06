# ADR-0001: Timeline clips reference their source media

## Status

Accepted, implemented in 7fe0923

## Context

The editor timeline cannot be rendered today, and the reason is the data model rather
than the encoder.

`Clip` in `src/ui/workspaces/EditorWorkspace.cpp` holds `track`, `start`, `dur`,
`label`, `tag`, `color`, `audio` and the per-clip transform, audio and speed
parameters. It holds no reference to a media file. `TimelineCanvas::dropEvent`
receives the media path in the drag payload (`application/x-malloy-mediapath`), uses
it to derive a label, and discards it. `clipToJson` never writes a path. There is
also no source in-point: trimming a clip adjusts `start` and `dur` only, so the offset
into the source file is lost as soon as the user trims from the left edge.

Two constraints shape the change:

- The persisted format field `"version"` is still `2`, and
  `SceneCollection::loadFromJson` rejects any file with `version > 2` outright. A
  version bump therefore makes new files unreadable by every existing build, which is
  a hard break.
- The same loader ignores unknown keys, and `docs/PROJECT_FORMAT.md` states that
  forwards compatibility relies on that. The editor timeline itself was already added
  this way: `ProjectDocument::saveToFile` inserts a `"timeline"` array without bumping
  the version.

## Decision

Add two fields to each clip object inside the `timeline` array, and keep the file
`"version"` at `2`.

```jsonc
{
  "track": 2, "start": 8.0, "dur": 132.0,
  "label": "Spire phase 1", "tag": "VID", "color": "#5a87c3", "audio": false,

  "sourcePath": "F:/Captures/spire-ep14.mkv",  // NEW: absolute path, "" = unlinked
  "sourceIn":   12.5,                           // NEW: seconds into the source, default 0

  "transform":   { "x": 0, "y": 0, "scale": 100.0, "rotation": 0.0, "opacity": 100 },
  "audioParams": { "gainDb": 0, "pan": 0, "channels": 0 },
  "speed":       { "factor": 1.0 }
}
```

Contracts that go with the fields:

1. **Source consumption.** `dur` is duration on the timeline. The segment consumed from
   the source is `[sourceIn, sourceIn + dur * speed.factor)`. A clip whose computed end
   exceeds the source duration is clamped at render time and reported, not silently
   padded.
2. **Trimming.** Dragging the left edge changes `start`, `dur` and `sourceIn` together;
   dragging the right edge changes `dur` only. Razor splits assign the right-hand clip
   `sourceIn + (splitTime - start) * speed.factor`.
3. **Unlinked clips.** A clip with an empty or missing `sourcePath` is *unlinked*. It
   remains editable and keeps rendering in the timeline UI, but it is not renderable.
   The editor marks it, and a render that contains one fails with a message naming the
   clip rather than producing a silently incomplete file.
4. **Missing media.** A `sourcePath` that no longer resolves is also unlinked, decided
   at render start rather than at load, so opening a project with an unplugged drive
   does not mutate the project.

The demo clip set built into `TimelineCanvas` has no paths and therefore becomes
explicitly unrenderable. New projects should start with an empty timeline instead of
demo clips; that is a small behavior change carried by the implementation slice.

## Alternatives considered

**Project-level media table with clip-side ids.** Clips would carry `mediaId` into a
`media` array holding path, duration and probe results. This is what mature editors do,
and it makes a relink flow a single edit per file instead of per clip. Rejected for now
as machinery ahead of a requirement: there is no stated portability or relink
requirement, and the change stays available later because a clip can gain `mediaId`
while `sourcePath` remains the fallback.

**Project-relative paths.** Better portability when media sits beside the project file,
but `MediaRegistry` indexes Movies, Pictures and Music, and recordings land in a
configurable folder that is usually on a different volume from the project. Relative
paths would be relative to nothing useful in the common case.

**No in-point, re-derive at render.** Only workable if trims never touch the left edge.
The editor already supports left-edge trimming, so the information has to be stored.

**Bump the format version to 3.** Rejected: the loader's `version > 2` guard turns a
bump into a hard break for existing builds, and buys nothing that additive keys do not
already give.

## Consequences

- Renders become expressible. This ADR is a prerequisite for ADR-0003 and for issue #3.
- Projects saved by the current build keep loading in older builds, which ignore the new
  keys; those builds simply lose the source links on the next save.
- Every existing saved project loads with all clips unlinked. There is no automatic
  repair, and none should be attempted: guessing a path from a label would be wrong more
  often than right.
- The editor gains an unlinked state to display, and `MediaRegistry` probe results
  (duration, resolution) become load-bearing rather than decorative.
- `docs/PROJECT_FORMAT.md` is currently stale: it documents neither the shipped
  `timeline` key nor these fields, and its version history is out of date. It has to be
  corrected in the same change.

## Security / privacy impact

A project file starts carrying absolute local file-system paths, including user names in
paths such as `C:/Users/<name>/Videos/...`. Project files are shared and are checked into
repositories. This is disclosure of local layout, not of file contents, and it is the
same class of data the recording settings already persist to the registry. It is
accepted here and named so it is a known property rather than a surprise. If project
sharing becomes a supported flow, the media-table alternative above is the place to add
path redaction or relinking.

No credentials or tokens are involved.

## Validation plan

Model tests, none of which need ffmpeg:

- Clip JSON round-trip preserves `sourcePath` and `sourceIn`.
- A legacy clip object with neither key loads with `sourcePath` empty and `sourceIn` 0.
- Left-edge trim moves `start`, `dur` and `sourceIn` consistently; right-edge trim leaves
  `sourceIn` untouched.
- Razor split produces a right-hand clip whose `sourceIn` accounts for `speed.factor`.
- A timeline containing an unlinked clip is rejected by the graph builder from ADR-0003
  with an error naming the clip.

Runtime check: drop a media file from the bin onto the timeline, save, reopen, and
confirm the clip still resolves to the same file.

## Rollback or migration notes

No migration runs on load: absent keys mean unlinked, which is the correct reading of an
older file. Rollback is removing the keys again; files written in the meantime stay
loadable because unknown keys are ignored. Nothing rewrites existing project files until
the user saves.
