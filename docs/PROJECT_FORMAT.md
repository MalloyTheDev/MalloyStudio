# MalloyStudio Project Format

MalloyStudio projects are stored as UTF-8 JSON with the extension `.malloy.json`.
The file is written atomically via `QSaveFile` (temp-file + rename).

---

## Top-level structure

```jsonc
{
  "app":          "MalloyStudio",  // always this literal string
  "version":      2,               // format version (see migration notes below)
  "currentScene": 0,               // index into "scenes" array
  "audio":        {},              // reserved — present but empty in v5
  "sources":      [ ...Source ],   // shared source library
  "scenes":       [ ...Scene ]     // ordered scene list
}
```

---

## Source object

Every source in the library has a stable integer `id`. Items reference it via `sourceId`.

```jsonc
{
  "id":   1,                      // positive integer, unique within the project
  "name": "Camera",               // user-visible label
  "type": "display_capture",      // one of the type IDs below

  // Always present (may be empty / default):
  "text":  "",
  "color": "#ff2e88ff",           // #AARRGGBB hex (QColor::HexArgb format)

  // Conditional — only present when populated:
  "monitor": {                    // DisplayCapture only
    "configured": true,
    "adapter":    0,              // DXGI adapter index
    "output":     0              // DXGI output index on that adapter
  },

  "imagePath": "C:/pics/bg.png", // Image only

  "window": {                     // WindowCapture only
    "hwnd":  67832,               // HWND as a signed 64-bit integer
    "title": "Google Chrome"      // last-known window title (informational)
  },

  "audioDeviceId": "{0.0.1.00000000}.{some-guid}"  // AudioInput only — WASAPI device ID
}
```

### Source type IDs

| `"type"` string | `Source::Type` | Description |
|---|---|---|
| `"display_capture"` | `DisplayCapture` | DXGI full-monitor capture |
| `"image"` | `Image` | Static image from disk |
| `"text"` | `Text` | Rendered text with colour |
| `"color_block"` | `ColorBlock` | Solid-colour rectangle |
| `"browser"` | `Browser` | Browser source (v5 placeholder) |
| `"window_capture"` | `WindowCapture` | Specific app window via HWND |
| `"audio_input"` | `AudioInput` | Microphone / line-in source |

---

## Scene object

```jsonc
{
  "name":         "Main Scene",
  "selectedItem": 0,              // index of the selected item (-1 = none)
  "items":        [ ...Item ]     // ordered layer list; item[0] is drawn first (bottom)
}
```

---

## Item object

Items reference a source by `sourceId`. Multiple items across scenes may share the
same `sourceId`.

```jsonc
{
  "id":       12,               // positive integer, unique within the project
  "sourceId": 1,                // references a source in "sources"

  "visible":  true,
  "locked":   false,

  "transform": {
    "x": 0.0,                   // canvas X (0 = left edge, 1920 = right edge)
    "y": 0.0,                   // canvas Y (0 = top, 1080 = bottom)
    "w": 1920.0,                // width in canvas pixels
    "h": 1080.0                 // height in canvas pixels
  },

  // Optional — omitted when the filter chain is empty:
  "filters": [ ...Filter ]
}
```

### Canvas coordinate system

The canvas is always **1920 × 1080** logical pixels regardless of output resolution.
Output resolution (set in Output Settings) is applied by ffmpeg at encode time. Items
whose transform extends outside `[0,0,1920,1080]` are clamped to the canvas bounds.

---

## Filter object

Each filter is a JSON object with a `"type"` discriminator:

### Crop filter

```jsonc
{
  "type":   "crop",
  "top":    0.0,    // fraction of item height to remove from top (0..1)
  "left":   0.0,
  "bottom": 0.0,
  "right":  0.0
}
```

The remaining inner rectangle is scaled back to fill the original item rect using
bicubic interpolation.

### Opacity filter

```jsonc
{
  "type":    "opacity",
  "opacity": 1.0    // 0.0 = fully transparent, 1.0 = fully opaque
}
```

Multiple opacity filters in the same chain are multiplied together.

### Color correction filter

```jsonc
{
  "type":       "color_correction",
  "brightness": 1.0,   // 0..2, neutral = 1.0
  "contrast":   1.0,   // 0..2, neutral = 1.0
  "saturation": 1.0    // 0..2, neutral = 1.0; 0 = greyscale
}
```

Implementation: a 256-entry LUT is built from brightness + contrast, then applied
per-pixel alongside a Rec.601 luma-based saturation blend.

---

## Complete minimal example

```json
{
  "app": "MalloyStudio",
  "version": 2,
  "currentScene": 0,
  "audio": {},
  "sources": [
    {
      "id": 1,
      "name": "Desktop",
      "type": "display_capture",
      "text": "",
      "color": "#ff2e88ff",
      "monitor": { "configured": true, "adapter": 0, "output": 0 }
    },
    {
      "id": 2,
      "name": "Webcam window",
      "type": "window_capture",
      "text": "",
      "color": "#ff2e88ff",
      "window": { "hwnd": 198472, "title": "Camera" }
    }
  ],
  "scenes": [
    {
      "name": "Main Scene",
      "selectedItem": 0,
      "items": [
        {
          "id": 1,
          "sourceId": 1,
          "visible": true,
          "locked": false,
          "transform": { "x": 0, "y": 0, "w": 1920, "h": 1080 }
        },
        {
          "id": 2,
          "sourceId": 2,
          "visible": true,
          "locked": false,
          "transform": { "x": 1440, "y": 810, "w": 480, "h": 270 },
          "filters": [
            { "type": "opacity", "opacity": 0.9 }
          ]
        }
      ]
    }
  ]
}
```

---

## Version history

### v2 (current)

- Separate `"sources"` array at root level; items hold `"sourceId"` references.
- Top-level `"audio": {}` reserved key.
- `"filters"` array on items (omitted when empty — backwards-compatible with early v2 files).
- `"window"` and `"audioDeviceId"` fields on sources (v5, omitted when not set).

### v1 (legacy — read-only, auto-migrated)

- No `"sources"` array; each item embedded a full `"source"` sub-object.
- No `"audio"` key.
- No `"filters"` key.
- Loading a v1 file triggers transparent migration: inline sources are extracted into
  the library and assigned fresh IDs. The saved copy is always v2.

---

## Forwards compatibility

Unknown top-level keys are ignored on load. Unknown source types cause a load error.
Unknown filter types are silently skipped (the item loads with a shorter filter chain).
This means future versions can add optional top-level metadata without breaking v5 readers.
