# MalloyStudio Project Format

This document specifies the MalloyStudio project file (`.malloy.json`): its structure,
every key with its type, default and bounds, how versions are read and migrated, and the
rules applied when a project is opened from disk. It describes the code at the commit it
ships with. Where the code and this document disagree, the code is authoritative and this
document is wrong.

Related documents: [ARCHITECTURE.md](ARCHITECTURE.md) (how the model, capture and render
paths fit together), [DEVELOPMENT.md](DEVELOPMENT.md) (building and testing), the
[README](../README.md), and the decision records this format depends on:
[ADR-0001](adr/0001-timeline-clip-source-reference.md) (timeline clips reference their
media), [ADR-0002](adr/0002-render-job-contract.md) (render jobs) and
[ADR-0003](adr/0003-render-worker-pipeline.md) (the render worker).

The reader and writer are `ProjectDocument` (`src/project/ProjectDocument.cpp`),
`SceneCollection::toJson` and `SceneCollection::loadFromJson`
(`src/model/SceneCollection.cpp`), `FilterEffect::toJson` and `FilterEffect::fromJson`
(`src/model/FilterEffect.cpp`), and `clipToJson` and `clipFromJson`
(`src/ui/workspaces/EditorWorkspace.cpp`). The format is pinned by tests in
`tests/model_tests.cpp`, among them `v1ProjectMigratesAndV2RoundTrips`,
`projectDocumentV3TimelineRoundTrips`, `editorClipFromLegacyV3JsonAppliesDefaults`,
`filterChainRoundTrips`, `anOversizedProjectIsRefusedUnread`,
`projectMediaPathsMustBeLocalFiles`, `loadedProjectHoldsItsDevicesUntilAllowed` and
`sourceIdsNearTheTopDoNotOverflow`.

---

## 1. Conventions

**Outcomes.** When a project is opened, each rule below has one of these outcomes:

| Term | Meaning |
|---|---|
| Refused | The whole load fails with an error message. The project already open is left exactly as it was. |
| Clamped | The value is moved to the nearest value inside its bounds. |
| Defaulted | The key is read as though it were absent, and takes its default. |
| Replaced | The value is discarded and the reader supplies its own (used for item ids). |
| Dropped | The value is discarded and nothing takes its place. |
| Ignored | The value in the file is not read. Unknown keys are also not written back (section 4.3). |

**Types.** Values are read with Qt's typed JSON accessors. A key whose value has the wrong
type is read exactly as if it were absent, so the "default" column in each table covers
both cases unless it says otherwise.

| Type in this document | Accepted | Otherwise |
|---|---|---|
| string | a JSON string | default |
| number | any JSON number | default |
| integer | a JSON number with a whole value that fits a 32-bit signed integer | default (so `12.5` and `3000000000` both read as the default) |
| int64 | a JSON number with a whole value that fits a 64-bit signed integer | default |
| boolean | `true` or `false` (not `0`, `1` or a string) | default |
| object | a JSON object | read as an empty object |
| array | a JSON array | read as an empty array, except where a rule below refuses the file |

**Colours.** A colour string is parsed by `QColor`, which accepts `#RGB`, `#RRGGBB`,
`#AARRGGBB`, `#RRRGGGBBB`, `#RRRRGGGGBBBB`, an SVG colour keyword name, or `transparent`.
A string it cannot parse yields an invalid colour; writers should always supply a valid one.
The writer emits lowercase hexadecimal.

**Units.** Canvas positions and sizes are in canvas pixels on the fixed 1920x1080 canvas.
Timeline times are in seconds, as JSON numbers.

---

## 2. The file

| Property | Rule |
|---|---|
| Content | One JSON object (the root). A root that is not an object is refused: "Project root must be a JSON object." |
| Encoding | UTF-8. The writer emits indented UTF-8 without a byte order mark. |
| Size | A file larger than 32 MiB (33,554,432 bytes) is refused before any of it is read: "This project file is too large to open (N MB)." A file of exactly 32 MiB is read. |
| Syntax | Invalid JSON is refused with Qt's parse error message. |
| Key order | Carries no meaning. The writer emits the keys of every object in alphabetical order, because Qt's JSON object is sorted. |
| Writing | Atomic: the file is written through `QSaveFile` (a temporary file renamed over the target on commit), so a failed save leaves the previous file intact. |
| Extension | `.malloy.json`. Save As appends `.malloy.json` to the chosen name unless it already ends in `.json` (case-insensitive). Open accepts any file; its filter offers `*.malloy.json` and `*.json`. |
| Display name | The file name with `.malloy.json` removed (`ProjectDocument::displayName`); for any other name, the file name without its last extension. |
| Discovery | The Projects list shows `*.malloy.json` files in the searched folders: Movies and Documents by default, plus every folder a project has been opened from or saved to (kept in the `projects/searchDirs` setting). A file named only `*.json` can be opened but is not listed. The list reads at most the first 1 MiB of each file, to count its scenes. |

---

## 3. Top-level object

```text
root
  app            string
  version        integer
  canvas         object     informational
  currentScene   integer
  audio          object     reserved
  sources        array of Source      (section 5)
  scenes         array of Scene       (section 6)
  timeline       array of Clip        (section 9), only when the editor timeline has clips
```

| Key | Type | Written | Read |
|---|---|---|---|
| `app` | string | Always `"MalloyStudio"`. | A string other than exactly `"MalloyStudio"` (case-sensitive) is refused: "This is not a MalloyStudio project." An absent key, or a value that is not a string, is accepted. |
| `version` | integer | Always `2`. | See section 4. |
| `canvas` | object | Always `{"width": 1920, "height": 1080}`. | Ignored. The canvas is fixed at 1920x1080 whatever this says. |
| `currentScene` | integer | Index of the scene being edited, or `-1` when there are no scenes. | Clamped into `[0, number of scenes - 1]`, so a negative value selects the first scene. Default `0`. Meaningless when there are no scenes. |
| `audio` | object | Always `{}`. | Ignored. Reserved for per-scene audio routing, which does not exist. See section 10. |
| `sources` | array | Always. | Version 2: must be an array, or the file is refused: "Project is missing a sources array." Version 1: not read. |
| `scenes` | array | Always. | Must be an array, or the file is refused: "Project is missing a scenes array." |
| `timeline` | array | Only when the editor timeline holds at least one clip; omitted when it is empty. | Absent or not an array: an empty timeline. Its content is not validated when the project is opened; see section 9.6. |

The following are not part of the project and are not stored in it: studio mode and the
program and preview scene selection, the undo history, the answers given to the device
consent prompt, output and encoder settings, and audio mixer levels. Section 12 says where
the persisted ones live.

---

## 4. Versioning and migration

### 4.1 Reading `version`

1. `version` is read as an integer. When it is absent or not an integer, it is inferred:
   `2` if the root has a `sources` key (of any type), otherwise `1`.
2. A value other than `1` or `2` is refused: "Unsupported MalloyStudio project version."

The writer always writes `2`, and every file this build saves is version 2. The format
version is unrelated to the application version. Comments in the source refer to
development increments as "v3", "v4" or "v7" (for example "v3 editor timeline"); those are
not values of `version`, and every file carrying the features they describe is version 2.

### 4.2 Version 1 (legacy, read only)

A version 1 file has no source library. Each item carries its own source inline:

```json
{
  "app": "MalloyStudio",
  "version": 1,
  "currentScene": 0,
  "scenes": [
    {
      "name": "Migrated Scene",
      "selectedItem": 0,
      "items": [
        {
          "id": 7, "visible": true, "locked": false,
          "transform": { "x": 10, "y": 20, "w": 300, "h": 80 },
          "source": { "name": "Title", "type": "text", "text": "from v1", "color": "#ff2e88ff" }
        }
      ]
    }
  ]
}
```

Migration happens on load and is transparent:

- Every item must have a non-empty `source` object whose `type` is known, or the file is
  refused ("Project contains an unknown source type." for an unknown or missing type).
- Each item gets a source of its own; nothing is shared. Sources receive fresh ids, and
  any `id` or `sourceId` in the version 1 data is not used.
- Only `name`, `type`, `text`, `color` and `monitor` are read from an inline source.
- Items are otherwise read as in version 2 (section 7), including `filters`.
- The migrated project is saved as version 2. The version 1 file is not rewritten until
  the user saves.

### 4.3 Compatibility within version 2

Everything added since the source library was split out has been added to version 2 as
optional keys with defaults, rather than by raising the version: the `filters` array, the
`enabled` flag and the `chroma_key`, `blur` and `scroll` filters, the `window`,
`audioDeviceId`, `camera` and `browser` source data, the `canvas` object, the `timeline`
array, and the clip keys `sourcePath`, `sourceIn`, `transform`, `audioParams` and `speed`.
A reader refuses any `version` above 2, so raising it would make new files unreadable by
every existing build.

How readers treat what they do not know, which is what extending the format depends on:

| Content | Reader behaviour |
|---|---|
| Unknown key in any object | Ignored, and therefore dropped on the next save: the writer serialises only the keys it knows. |
| Unknown filter `type` | That filter is skipped; the item loads with a shorter chain, and the filter is gone after the next save. |
| Unknown source `type` | The whole file is refused. Adding a source type is therefore a breaking change for every existing reader. |
| `version` above 2 | The whole file is refused. |

Rules for changing the format: add optional keys whose default reproduces the previous
behaviour; document the default here; treat a new source type as a compatibility break;
and reserve a version increase for a change an existing reader cannot read correctly.

---

## 5. Sources

`sources` is the source library. A source is the thing a layer shows or plays (a monitor,
a camera, an image, some text); items in scenes refer to it by `id`, so one source can
appear in several scenes and a change to it shows everywhere.

### 5.1 Source types

| `type` | What it is | Device-backed | Type-specific keys |
|---|---|---|---|
| `display_capture` | A whole monitor. | Yes | `monitor` |
| `window_capture` | One application window. | Yes | `window` |
| `camera` | A camera or capture device through Media Foundation. | Yes | `cameraDeviceId`, `cameraName` |
| `audio_input` | An audio capture device (WASAPI). | Yes | `audioDeviceId` |
| `image` | An image file, stretched to the item's rectangle. | No | `imagePath` |
| `text` | Text, drawn in white, word-wrapped and centred, over a translucent box. | No | `text` |
| `color_block` | A solid rectangle. | No | `color` |
| `browser` | A web page placeholder. The URL is stored but never loaded; the source draws a "Browser placeholder" box. | No | `browserUrl`, `browserRefreshHz` |

`type` is matched after trimming surrounding whitespace and ignoring case; the writer
emits the lowercase form above. A missing or unknown `type` refuses the file: "Project
contains an unknown source type." Device-backed sources are held for consent when a
project is opened from a file (section 8).

### 5.2 Source keys

The loader reads every key below for every source type, and the writer writes each
conditional key whenever its value is set, whatever the type. A key is only used by the
types named in section 5.1.

| Key | Type | Default | Written | Read and bounds |
|---|---|---|---|---|
| `id` | integer | none (required) | Always. | Required. Must be from 1 to 2147483646 inclusive, or the file is refused: "Project contains a source without a valid id." Must be unique, or the file is refused: "Project contains duplicate source ids." |
| `type` | string | none (required) | Always. | See section 5.1. |
| `name` | string | The type's display name: "Display Capture", "Window Capture", "Camera", "Audio Input", "Image", "Text", "Color Block" or "Browser". | Always. | Any string, including an empty one. |
| `text` | string | `"MalloyStudio"` for a `text` source, `""` for every other type. | Always. | The text a `text` source draws. |
| `color` | colour string | `"#ff2e88ff"` | Always, as `#aarrggbb`. | The fill of a `color_block` source. Stored for every type; no other type draws with it. |
| `monitor` | object | not configured | When configured (adapter 0 or greater), as `{"configured": true, "adapter": A, "output": O}`. | Applied only when `configured` is `true`. `adapter` (integer, default `-1`) is the DXGI adapter index and `output` (integer, default `0`) the output index on that adapter. An `adapter` below 0 leaves the source unconfigured. Not otherwise bounded: an index naming no monitor captures nothing. |
| `imagePath` | string | `""` | When not empty. | Path of the image file. Subject to the path policy when the project is opened from a file (section 11.2): a path the policy refuses is dropped. |
| `window` | object | not configured | When `hwnd` is not 0, as `{"hwnd": H, "title": T}`. | Applied when the object is not empty. `hwnd` (int64, default `0`) is the window handle, used as-is; `title` (string) is the window title when it was chosen, shown in the interface. Window handles are not stable across sessions and the title is not used to find the window again, so a restored handle may name no window, or a different one. |
| `audioDeviceId` | string | `""` | When not empty. | WASAPI endpoint id of the audio device. Applied when not empty. |
| `cameraDeviceId` | string | `""` | With `cameraName`, when not empty. | Media Foundation symbolic link of the camera. Applied, together with `cameraName`, when not empty. |
| `cameraName` | string | `""` | With `cameraDeviceId`. | Friendly name of the camera, for display. Read only when `cameraDeviceId` is not empty. |
| `browserUrl` | string | `""` | With `browserRefreshHz`, when not empty. | Stored only; nothing loads it. Applied when not empty. |
| `browserRefreshHz` | integer | `10` | With `browserUrl`. | Clamped to 1 to 60. Read only when `browserUrl` is not empty. |

### 5.3 Library rules

- Sources keep their array order, which the writer preserves.
- A source that no item in any scene refers to is discarded when the project is loaded,
  and so is not written back. The library holds only sources that are in use.
- Source ids are not renumbered. A source created later receives an id above the highest
  one loaded, skipping any id held for consent (section 8); when the counter passes
  2147483646 it takes the lowest id not in use.

---

## 6. Scenes

| Key | Type | Default | Written | Read |
|---|---|---|---|---|
| `name` | string | `"Scene"` | Always. | Any string, including an empty one. |
| `selectedItem` | integer | `-1` (none) | Index of the selected item in `items`, `-1` when none. | `-1` selects nothing. An index from 0 to `items.length - 1` selects that item. Any other value selects the first item, if there is one. |
| `items` | array of Item | `[]` | Always. | The scene's layers, top first (section 7.2). |

A scene element that is not an object loads as an empty scene named "Scene".

---

## 7. Items

An item is one layer in one scene: a reference to a source plus its placement, visibility
and filters.

### 7.1 Item keys

| Key | Type | Default | Written | Read and bounds |
|---|---|---|---|---|
| `id` | integer | a fresh id | Always. | Kept when from 1 to 2147483646; otherwise replaced with a fresh id. Need not be unique: nothing looks items up by id. |
| `sourceId` | integer | none (required) | Always. | Version 2: must be the `id` of a source in `sources`, or the file is refused: "Project contains a layer that references a missing source." Version 1: not read. |
| `visible` | boolean | `true` | Always. | Whether the layer is drawn. For an `audio_input` source it also decides whether the device is mixed: only sources with a visible item in the scene on air are opened. |
| `locked` | boolean | `false` | Always. | A locked layer cannot be dragged or resized in the preview, or nudged with the arrow keys. Its numbers can still be edited in the Inspector. |
| `transform` | object | full canvas | Always, as `{"x", "y", "w", "h"}`. | The layer's rectangle; see section 7.3. |
| `filters` | array of Filter | `[]` | Only when the chain is not empty. | See section 7.5. |
| `source` | object | | Never. | Version 1 only (section 4.2). |

### 7.2 Layer order

`items[0]` is the top layer and the last item is the bottom layer: the compositor draws
the list from the last element to the first, so each earlier item is drawn over the ones
after it. Order is preserved exactly on load and save.

### 7.3 Transform and the canvas

The canvas is always 1920x1080 canvas pixels, with the origin at the top left, x to the
right and y downwards. The project stores no output resolution; recordings and streams
scale the composed canvas to the output size configured in Settings.

| Key | Type | Default |
|---|---|---|
| `x` | number | `0` |
| `y` | number | `0` |
| `w` | number | `1920` |
| `h` | number | `1080` |

On load the rectangle is normalised (a negative width or height is flipped), its width is
clamped to 16 to 1920 and its height to 16 to 1080, and it is then moved, keeping its
size, so that it lies wholly inside the canvas. The same clamp applies to every edit, so a
file written by MalloyStudio always holds a rectangle inside the canvas. An item whose
`transform` is absent fills the canvas.

### 7.4 What an item draws

Each visible item draws its source stretched to its rectangle; aspect ratio is not
preserved. A device-backed source that is held for consent, or whose device is absent,
draws a placeholder in its rectangle rather than capturing anything. An `audio_input`
item draws a labelled box.

### 7.5 Filters

`filters` is an ordered chain applied to the item's image. Each element is an object with
a `type` discriminator, matched exactly and case-sensitively. An element whose `type` is
unknown, or which is not an object, is skipped (section 4.3).

Every filter may also carry:

| Key | Type | Default | Written |
|---|---|---|---|
| `enabled` | boolean | `true` | Only as `"enabled": false`, for a disabled filter. |

A disabled filter keeps its settings and has no effect.

**Application.** The source is drawn into an image the size of the item's rectangle; the
enabled filters other than `opacity` are applied to it in chain order; the result is drawn
at the product of every enabled `opacity` filter's value, wherever those filters sit in
the chain.

**Precision.** Filter values are held as 32-bit floating point numbers, and written back
as the double nearest that float, so a value that is not exactly representable changes in
its low digits on the first save: `0.1` is written back as `0.10000000149011612`.

#### `crop`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `top` | number | `0` | clamped to 0 to 1 |
| `left` | number | `0` | clamped to 0 to 1 |
| `bottom` | number | `0` | clamped to 0 to 1 |
| `right` | number | `0` | clamped to 0 to 1 |

Each value is the fraction of the item's height (`top`, `bottom`) or width (`left`,
`right`) removed from that edge. The remaining region is scaled back up, with smooth
scaling, to fill the item's rectangle, so cropping works as a zoom. If the edges remove
the whole width or height, the item draws nothing.

#### `opacity`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `opacity` | number | `1` | clamped to 0 (transparent) to 1 (opaque) |

#### `color_correction`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `brightness` | number | `1` | clamped to 0 to 2; 1 is neutral |
| `contrast` | number | `1` | clamped to 0 to 2; 1 is neutral |
| `saturation` | number | `1` | clamped to 0 to 2; 1 is neutral, 0 is greyscale |

Per pixel, saturation is applied first, as a blend towards the Rec. 601 luma; brightness
then scales each channel, and contrast scales it about mid-grey. Alpha is unchanged.

#### `chroma_key`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `key` | colour string | `"#00ff00"` | Written as `#rrggbb`; alpha is not used. |
| `tolerance` | number | `0.2` | clamped to 0 to 1 |
| `smoothness` | number | `0.1` | clamped to 0 to 1 |

A pixel's distance from `key` is measured in RGB and expressed as a fraction of the
largest possible distance. Pixels within `tolerance` become transparent; over the next
`smoothness` their alpha rises along a smoothstep curve to fully opaque.

#### `blur`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `radius` | integer | `4` | clamped to 0 to 32 pixels; 0 has no effect |

A separable box blur, including alpha.

#### `scroll`

| Key | Type | Default | Bounds |
|---|---|---|---|
| `speedX` | number | `0` | not bounded on load; the Inspector offers -2000 to 2000 |
| `speedY` | number | `0` | not bounded on load; the Inspector offers -2000 to 2000 |

Moves the item's image continuously, wrapping at its edges, at the given speed in canvas
pixels per second. A positive `speedX` moves the content left and a positive `speedY`
moves it up.

---

## 8. Device consent

A project names cameras, microphones, monitors and windows. Opening a file is not taken as
consent to open them.

- When a project is opened from a file, every source of a device-backed type
  (`display_capture`, `window_capture`, `audio_input`, `camera`) that survives the load is
  **held**: it stays in its scenes, draws a placeholder and captures nothing. The hold is
  applied before the load is announced to the rest of the application, so no device
  starts even briefly.
- If anything is held, the user is asked once whether to allow the project to use the
  devices it names, and the question lists them. Yes releases every held source. No
  leaves them all held.
- A held source can be released on its own, later: with the Allow button on its layer in
  the Sources panel or in the Inspector, or by switching one of its layers on. Releasing
  one source does not release any other.
- `image`, `text`, `color_block` and `browser` sources are never held. A browser source's
  URL is not loaded at all.
- The hold is session state. It is not stored in the file, and every open of the file
  holds again.

The hold is applied by loads from a file only. Undo and redo, which restore the model
through the same loader, neither add a hold nor release one, so undoing past a declined
prompt does not start a device. A new project clears the hold.

---

## 9. Timeline

`timeline` is the editor's timeline: a flat array of clips, each referencing a span of a
media file. The editor keeps the array in the order clips were created and split; the
array order has no meaning of its own except as a tie-break in rendering (section 9.5).

### 9.1 Clip keys

A clip is read by the editor with the defaults below. The editor re-serialises every clip
on save, writing every key, so a saved clip always has the full shape shown in the
example (section 13).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `track` | integer | `0` | Track index, 0 to 5 (section 9.2). |
| `start` | number | `0` | Position of the clip's first frame on the timeline, in seconds. |
| `dur` | number | `0` | Length of the clip on the timeline, in seconds. |
| `label` | string | `""` | Shown on the clip, and used to name it in render messages. An empty label is reported as "clip N" (1-based array position). |
| `tag` | string | `""` | Short kind label drawn on the clip: `VID`, `AUD`, `IMG` or `FILE` for clips the editor creates. Display only. |
| `color` | colour string | none | The clip's colour in the editor, written as `#rrggbb`. Display only. A missing or unparsable value is not replaced by the tag's colour; writers should always supply it. |
| `audio` | boolean | `false` | `true` for an audio clip. Decides how the clip renders (section 9.5), not where it is drawn. |
| `sourcePath` | string | `""` | The media file this clip plays. Empty means unlinked (section 9.4). |
| `sourceIn` | number | `0` | Offset into the media file, in seconds, of the clip's first frame. |
| `transform` | object | see below | Placement of a video clip. |
| `transform.x` | integer | `0` | Left edge, in canvas pixels. |
| `transform.y` | integer | `0` | Top edge, in canvas pixels. |
| `transform.scale` | number | `100` | Size as a percentage of the canvas (and of the output, when rendered). |
| `transform.rotation` | number | `0` | Degrees. Stored and editable; a render refuses any value but 0. |
| `transform.opacity` | integer | `100` | Percent, 0 (transparent) to 100 (opaque). |
| `audioParams` | object | see below | Audio settings of an audio clip. |
| `audioParams.gainDb` | integer | `0` | Gain in decibels. |
| `audioParams.pan` | integer | `0` | -100 (left) to 100 (right). Stored and editable; a render refuses any value but 0. |
| `audioParams.channels` | integer | `0` | 0 stereo, 1 mono left, 2 mono right. Stored and editable; a render refuses any value but 0. |
| `speed` | object | see below | Playback speed. |
| `speed.factor` | number | `1.0` | Source seconds consumed per timeline second. |

When `transform`, `audioParams` or `speed` is absent, or is an empty object, every key in
it takes its default; so does each key absent from a non-empty one. `transform.x` and
`transform.y` are integers: a fractional value reads as 0. A timeline element that is not
an object loads as a clip with every key at its default.

### 9.2 Tracks

| `track` | Id | Name | Kind |
|---|---|---|---|
| 0 | V3 | Titles | video |
| 1 | V2 | Webcam | video |
| 2 | V1 | Gameplay | video |
| 3 | A1 | Mic | audio |
| 4 | A2 | Game Audio | audio |
| 5 | A3 | Music Bed | audio |

The editor lists the tracks in this order, top to bottom. It places an audio clip only on
an audio track and any other clip only on a video track, and a clip moved by the mouse
changes track only to one of the same kind. The file format does not enforce either: a
clip whose `track` is outside 0 to 5 loads and renders, but is drawn outside the track
area and cannot be reached with the mouse.

### 9.3 Source consumption and editing

A clip occupies `[start, start + dur)` on the timeline and consumes
`[sourceIn, sourceIn + dur * speed.factor)` of its media file
([ADR-0001](adr/0001-timeline-clip-source-reference.md)). The editor keeps this true
through every edit; the arithmetic is in `src/ui/workspaces/TimelineEdits.h`:

- **Drop.** A file dragged from the media bin becomes a clip with `sourceIn` 0, the file's
  length in the media library (whole seconds; 4 seconds when unknown) as `dur`, the file
  name as `label`, and a `tag` and colour by kind: `VID` `#5a96cd`, `AUD` `#5fbe82`,
  `IMG` `#966ed2`, `FILE` `#788291`.
- **Move.** Changes `start` only. `start` is kept within 0 and `86400 - dur`.
- **Left-edge trim.** Moves `start`, `dur` and `sourceIn` together, keeping the clip's end
  fixed. A linked clip stops where `sourceIn` would become negative.
- **Right-edge trim.** Changes `dur` only. The clip's end is kept within the timeline
  limit of 86400 seconds.
- **Minimum length.** A trim never makes a clip shorter than 0.25 seconds.
- **Split** at time `t`, which must be more than 0.1 seconds from either end: the
  left-hand clip keeps its `start` and `sourceIn` with `dur = t - start`; the right-hand
  clip gets `start = t`, `dur = old end - t` and
  `sourceIn = old sourceIn + (t - start) * speed.factor`.

The editor bounds what it creates by 86400 seconds (a day), the same limit a render
accepts, but does not clamp values read from a file.

### 9.4 Unlinked and missing media

A clip whose `sourcePath` is empty or absent is **unlinked**. It remains editable, is drawn
with a dashed outline while not selected, and cannot be rendered: a render that contains
it fails with a message naming the clip. Clips written before `sourcePath` existed load
this way, and nothing tries to infer a path for them.

`sourcePath` is not checked when the project is opened, and not rewritten, so opening a
project whose media drive is unplugged does not change it. Whether the path is acceptable
(section 11.2) and whether the file exists are decided when a render starts.

### 9.5 How a timeline renders

Rendering is described in [ADR-0003](adr/0003-render-worker-pipeline.md); the parts that
give the clip keys their meaning are:

- **Snapshot.** Export copies the timeline, as the editor serialises it, into a render job
  ([ADR-0002](adr/0002-render-job-contract.md)). Later edits do not change a queued job.
- **Video and audio.** A clip with `audio` `false` contributes its picture only; its
  file's sound is not used. A clip with `audio` `true` contributes its sound only.
  A timeline needs at least one video clip to render.
- **Background.** Video is composited onto black at the output size and frame rate, so
  gaps render black and every clip keeps its timeline position.
- **Order.** Video clips are composited in descending `track` order, ties in array order,
  each over the ones before it. With the editor's track list, V3 (`track` 0) is therefore
  drawn over V2 (`track` 1), which is drawn over V1 (`track` 2), matching the order in
  which the editor lists them.
- **Placement.** `transform.x` and `transform.y` are canvas pixels, scaled to the output
  (`x * output width / 1920`, `y * output height / 1080`, rounded), so a clip lands in the
  same place at any output size.
- **Size.** The clip is scaled to `transform.scale` percent of the output width and of the
  output height, each rounded and at least 2 pixels. The source's aspect ratio is not
  preserved.
- **Opacity.** Below 100, the clip is drawn at `transform.opacity` percent.
- **Speed.** Video is retimed by `speed.factor`; audio is changed in tempo (pitch kept) by
  the same factor.
- **Gain.** `audioParams.gainDb` is applied to an audio clip; audio clips are mixed without
  normalisation.
- **Length.** The output is as long as the latest clip end, `max(start + dur)`, after any
  clip has been cut to its source.

### 9.6 Render-time checks

A timeline is validated when a render starts, not when a project is opened. Failing any
of the following fails the render, and a message about one clip names that clip. The
ranges keep the render's arithmetic defined and its length bounded; they do not describe
what is useful.

| Check | Accepted |
|---|---|
| Clips in the timeline | 1 to 32 |
| Video clips | at least 1 |
| `sourcePath` | not empty; allowed by the path policy (section 11.2); an existing file |
| `dur` | finite, greater than 0, at most 86400 |
| `start` | finite, 0 to 86400 |
| `sourceIn` | finite, 0 to 86400 |
| `transform.scale` | finite, 1 to 10000 |
| `transform.opacity` | 0 to 100 |
| `transform.rotation` | 0 (rotation is not implemented) |
| `audioParams.gainDb` | -60 to 30 |
| `audioParams.pan` | 0 (pan is not implemented) |
| `audioParams.channels` | 0 (channel mapping is not implemented) |
| `speed.factor` | finite, 0.1 to 4.0 |

The editor's Inspector offers narrower ranges than a render accepts (scale 1 to 1000,
gain -30 to 12 dB, speed 0.10 to 4.00) and wider ones for what a render refuses (rotation
-360 to 360, pan -100 to 100, three channel modes).

**Measured sources.** Before ffmpeg starts, each distinct `sourcePath` is measured with
ffprobe, one at a time, with a limit of 10 seconds each. Then, for each clip whose source
was measured at a positive length:

- if less than 1 millisecond of the source remains after `sourceIn` (at the clip's
  speed), the render fails, naming the clip;
- if the clip asks for more than remains, by more than 1 millisecond, it is cut to
  `(length - sourceIn) / speed.factor` seconds, the render continues, and the job records
  a note naming each clip cut and by how much (the render job's `note`,
  [ADR-0002](adr/0002-render-job-contract.md)).

A source that cannot be measured, because ffprobe is not installed, times out, fails, or
reports no positive length (a still image reports none), is rendered as though it were
long enough.

---

## 10. Audio

The project stores audio in three places, none of them the top-level `audio` object:

- **Scene audio.** An `audio_input` source names a capture device (`audioDeviceId`). The
  device is opened while a visible item for it is in the scene on air and the source is
  not held for consent (section 8). Desktop audio is not a source and is not stored in
  the project.
- **Timeline audio.** Clips with `audio` `true`, with their `audioParams` (section 9.1).
- **Reserved.** The top-level `audio` object is written empty and ignored on load. It is
  reserved for per-scene audio routing, which does not exist.

Mixer volume, pan and mute are per device, not per project: they are kept in the
application settings under `audio/inputs/<input id>`, where the input id is
`loopback:default` or `input:<device id>` (section 12).

---

## 11. Trust rules on opening a project

A project file may come from anywhere, so its content is checked rather than trusted.
The file is the trust boundary: the path policy and the device hold apply to a load from
a file (`ProjectDocument::loadFromFile`), while undo, redo and a new project, which go
through the same loader with the application's own state, get neither. Every other rule
in this section applies to every load.

### 11.1 Summary

| Outcome | What |
|---|---|
| Refused | File larger than 32 MiB. Invalid JSON. Root not an object. `app` a string other than `"MalloyStudio"`. `version` other than 1 or 2. `scenes` not an array. Version 2: `sources` not an array; a source without an integer `id` from 1 to 2147483646; two sources with the same `id`; a source whose `type` is missing or unknown; an item whose `sourceId` names no source. Version 1: an item without a non-empty `source` object of a known type. |
| Clamped | Item `transform` (section 7.3). `currentScene`. Filter values (section 7.5). `browserRefreshHz` to 1 to 60. |
| Replaced | Item `id` outside 1 to 2147483646. |
| Dropped | An `imagePath` the path policy refuses (file loads only; a warning is logged). A filter of unknown type. A source no item refers to. Unknown keys (on the next save). |
| Held | Every device-backed source (section 8; file loads only). |
| Checked at render | Every timeline value (section 9.6), including `sourcePath` against the path policy. |
| Not checked | Strings (names, text, device ids, URLs, window titles) and the number of scenes, items and sources are bounded only by the file size cap. `scroll` speeds, monitor indices and `hwnd` are not bounded. |

When a load is refused, nothing in the open project changes, the editor timeline
included.

### 11.2 Path policy

A project names media by path, and those paths are opened without the user seeing them:
an image source is drawn when its scene is composed, and a clip's file is probed and read
when a render starts. Two kinds of path are dangerous in a file that may not be the
user's. A UNC path (`\\host\share\file`) makes Windows connect and authenticate to the
host it names, handing over the user's NTLM credentials, and asking whether such a path
exists is itself enough. A URL or protocol string (`http:`, `concat:`, `tee:`) given to
ffmpeg as an input is read as an input of the file author's choosing.

The rule (`MediaPathPolicy::isAllowed`, `src/project/MediaPathPolicy.h`) is therefore
positive: a path is allowed only if it is a local, drive-absolute path, which means all of:

1. it has at least 3 characters;
2. its first character is an ASCII letter, its second a colon, and its third `\` or `/`;
3. its fourth character, if any, is not `\` or `/`.

| Path | Allowed |
|---|---|
| `C:/Users/me/Videos/clip.mp4`, `C:\Users\me\Videos\clip.mp4`, `e:/media/logo.png` | Yes |
| `\\host\share\x.png`, `//host/share/x.png` | No (UNC) |
| `\\?\C:\Users\me\clip.mp4` | No (long path prefix) |
| `C:\\host\share\x.png` | No (separator after the drive) |
| `http://host/x.mp4`, `concat:a.mp4\|b.mp4`, `file:C:/x.mp4`, `data:text/plain,hello` | No (scheme) |
| `clip.mp4`, `..\secrets\x`, `/etc/passwd`, `C:`, empty | No (relative or incomplete) |

Where it applies:

| Path | When checked | Outcome when refused |
|---|---|---|
| Source `imagePath` | When the project is opened from a file. | Dropped, and a warning is logged. The source loads empty and the rest of the project opens. The path is not kept, so the next save removes it from the file. |
| Clip `sourcePath` | When a render starts, before anything asks whether the file exists. | The render fails, naming the clip. |
| Render job `outputPath` | When a pending job is restored from the render queue at launch ([ADR-0002](adr/0002-render-job-contract.md)). | The job is marked failed. |

The policy does not apply to paths the user picks in the application's own file dialogs,
or to undo and redo. An image on a network share chosen in the Inspector is therefore
used and saved, but is dropped the next time the project is opened from its file.

---

## 12. Where related data is stored

| Data | Location | Relationship to the project |
|---|---|---|
| Output and encoder settings (resolution, frame rate, codec, quality, container, audio codec) | Application settings (`QSettings`, organisation and application `MalloyStudio`; on Windows the registry key `HKEY_CURRENT_USER\Software\MalloyStudio\MalloyStudio`), group `output`. | Not in the project. Export copies the settings current at that moment into the render job. |
| Render queue | `renderqueue.json` in the per-user application data directory (`QStandardPaths::AppDataLocation`). Specified in [ADR-0002](adr/0002-render-job-contract.md). | Each job holds a copy of the timeline in the clip format of section 9, and the path of the project it came from in `projectPath`, for information only. A job never reads the project file. |
| Audio mixer levels | Application settings, `audio/inputs/<input id>` (the input id is `loopback:default` or `input:<device id>`). | Not in the project; shared by every project using the device. |
| Projects list search folders | Application settings, `projects/searchDirs`. | Lists where projects are looked for (section 2). |
| Recordings folder | Application settings, `recording/lastDir`, shared by recordings and Export. | Where Export suggests writing a render. |

---

## 13. Complete example

This file is valid version 2 and loads. It has two scenes that share a text source, filters
on three items, a camera, a microphone and a monitor that are held for consent when it is
opened, and a three-clip timeline: one recording split in two, and a commentary track.

```json
{
  "app": "MalloyStudio",
  "version": 2,
  "canvas": { "width": 1920, "height": 1080 },
  "currentScene": 0,
  "audio": {},
  "sources": [
    {
      "id": 1,
      "name": "Main Monitor",
      "type": "display_capture",
      "text": "",
      "color": "#ff2e88ff",
      "monitor": { "configured": true, "adapter": 0, "output": 0 }
    },
    {
      "id": 2,
      "name": "Facecam",
      "type": "camera",
      "text": "",
      "color": "#ff2e88ff",
      "cameraDeviceId": "\\\\?\\usb#vid_046d&pid_0893&mi_00#6&2b5ba6a3&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global",
      "cameraName": "Logitech StreamCam"
    },
    {
      "id": 3,
      "name": "Desk Mic",
      "type": "audio_input",
      "text": "",
      "color": "#ff2e88ff",
      "audioDeviceId": "{0.0.1.00000000}.{8f5b5e3a-2c4e-4d0b-9a57-3f4c2d1e0b6a}"
    },
    {
      "id": 4,
      "name": "Title",
      "type": "text",
      "text": "Spire of the Hollow Sun",
      "color": "#ff2e88ff"
    },
    {
      "id": 5,
      "name": "Logo",
      "type": "image",
      "text": "",
      "color": "#ff2e88ff",
      "imagePath": "C:/Users/me/Pictures/logo.png"
    },
    {
      "id": 6,
      "name": "Backdrop",
      "type": "color_block",
      "text": "",
      "color": "#ff101820"
    }
  ],
  "scenes": [
    {
      "name": "Gameplay",
      "selectedItem": 1,
      "items": [
        {
          "id": 11,
          "sourceId": 5,
          "visible": true,
          "locked": true,
          "transform": { "x": 1712, "y": 32, "w": 176, "h": 176 },
          "filters": [
            { "type": "opacity", "opacity": 0.75 }
          ]
        },
        {
          "id": 12,
          "sourceId": 2,
          "visible": true,
          "locked": false,
          "transform": { "x": 1440, "y": 750, "w": 448, "h": 300 },
          "filters": [
            { "type": "crop", "top": 0, "left": 0.125, "bottom": 0, "right": 0.125 },
            { "type": "chroma_key", "key": "#00ff00", "tolerance": 0.25, "smoothness": 0.125 }
          ]
        },
        {
          "id": 13,
          "sourceId": 4,
          "visible": true,
          "locked": false,
          "transform": { "x": 480, "y": 40, "w": 960, "h": 140 }
        },
        {
          "id": 14,
          "sourceId": 3,
          "visible": true,
          "locked": false,
          "transform": { "x": 672, "y": 432, "w": 576, "h": 80 }
        },
        {
          "id": 15,
          "sourceId": 1,
          "visible": true,
          "locked": false,
          "transform": { "x": 0, "y": 0, "w": 1920, "h": 1080 },
          "filters": [
            { "type": "color_correction", "brightness": 1, "contrast": 1.25, "saturation": 1.5, "enabled": false }
          ]
        }
      ]
    },
    {
      "name": "Be Right Back",
      "selectedItem": -1,
      "items": [
        {
          "id": 21,
          "sourceId": 4,
          "visible": true,
          "locked": false,
          "transform": { "x": 480, "y": 470, "w": 960, "h": 140 }
        },
        {
          "id": 22,
          "sourceId": 6,
          "visible": true,
          "locked": true,
          "transform": { "x": 0, "y": 0, "w": 1920, "h": 1080 }
        }
      ]
    }
  ],
  "timeline": [
    {
      "track": 2,
      "start": 0,
      "dur": 40,
      "label": "spire-ep14.mkv",
      "tag": "VID",
      "color": "#5a96cd",
      "audio": false,
      "sourcePath": "F:/Captures/spire-ep14.mkv",
      "sourceIn": 12.5,
      "transform": { "x": 0, "y": 0, "scale": 100, "rotation": 0, "opacity": 100 },
      "audioParams": { "gainDb": 0, "pan": 0, "channels": 0 },
      "speed": { "factor": 1 }
    },
    {
      "track": 2,
      "start": 40,
      "dur": 55.5,
      "label": "spire-ep14.mkv",
      "tag": "VID",
      "color": "#5a96cd",
      "audio": false,
      "sourcePath": "F:/Captures/spire-ep14.mkv",
      "sourceIn": 52.5,
      "transform": { "x": 0, "y": 0, "scale": 100, "rotation": 0, "opacity": 100 },
      "audioParams": { "gainDb": 0, "pan": 0, "channels": 0 },
      "speed": { "factor": 1 }
    },
    {
      "track": 3,
      "start": 0,
      "dur": 95.5,
      "label": "commentary.wav",
      "tag": "AUD",
      "color": "#5fbe82",
      "audio": true,
      "sourcePath": "F:/Captures/commentary.wav",
      "sourceIn": 0,
      "transform": { "x": 0, "y": 0, "scale": 100, "rotation": 0, "opacity": 100 },
      "audioParams": { "gainDb": -6, "pan": 0, "channels": 0 },
      "speed": { "factor": 1 }
    }
  ]
}
```

Notes on the example:

1. **Layer order.** In "Gameplay" the logo is the top layer and the full-screen monitor
   capture the bottom one (section 7.2). `selectedItem` 1 selects the camera layer.
2. **Shared source.** Items 13 and 21 both show source 4, so editing the title text
   changes it in both scenes.
3. **Held devices.** Sources 1, 2 and 3 are device-backed. Opening this file holds all
   three and asks once; the logo, title and backdrop are never held.
4. **Filters.** The camera is cropped to the middle three quarters of its width and keyed
   on green.
   The monitor's colour correction is stored but disabled. All filter values here are
   exactly representable, so they are written back unchanged.
5. **Paths.** `imagePath` and every `sourcePath` are drive-absolute, so they pass the path
   policy. Had `imagePath` been `\\nas\art\logo.png`, the file would still open, with an
   empty logo.
6. **Split.** The two video clips are one 95.5 second clip split at 40 seconds: the
   right-hand clip's `sourceIn` is `12.5 + (40 - 0) * 1 = 52.5`, so together they play
   `spire-ep14.mkv` from 12.5 seconds to 108 seconds without a gap.
7. **Render.** If both files exist, a render produces 95.5 seconds of the recording with
   the commentary mixed in at -6 dB; the recording's own sound is not used, because only
   clips with `audio` `true` contribute sound. If `spire-ep14.mkv` turns out to be
   shorter than 108 seconds, the second clip is cut where the file ends and the job
   records a note saying so.
8. **Round trip.** Saving this project writes the same content with the keys of each
   object in alphabetical order.

---

## 14. Known limitations

These are properties of the current implementation, recorded so that a reader of the
format is not surprised by them. Open issues are linked where they exist.

- A still image placed on the timeline renders as a single frame, and media library
  lengths used for dropped clips are rounded to whole seconds
  ([#100](https://github.com/MalloyTheDev/MalloyStudio/issues/100)).
- Timeline renders are neither converted to nor tagged as BT.709
  ([#92](https://github.com/MalloyTheDev/MalloyStudio/issues/92)).
- Clip rotation, audio pan and channel mapping are stored and editable but not rendered;
  a render refuses a clip that uses them (section 9.6).
- The editor's Effects tab is not connected to anything, and clips carry no effects in
  the format ([#22](https://github.com/MalloyTheDev/MalloyStudio/issues/22)).
- `scroll` filter speeds are not bounded on load (section 7.5).
- A browser source stores a URL that nothing loads (section 5.1).
