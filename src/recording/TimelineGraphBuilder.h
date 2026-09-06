#pragma once

// Turns a render job's timeline snapshot into ffmpeg arguments.
//
// This is the whole translation from "what the editor shows" to "what ffmpeg
// runs", and it is deliberately pure: no process, no files, no widgets. That is
// what lets the placement, trimming and refusal rules be tested without an
// encoder present (docs/adr/0003-render-worker-pipeline.md).
//
// Media paths are only ever emitted as `-i` arguments, never interpolated into
// the filter graph, where `:` and `'` are metacharacters that a user's file name
// could otherwise inject. Clips are addressed in the graph by input index.

#include "recording/OutputSettings.h"

#include <QJsonArray>
#include <QString>
#include <QStringList>

struct RenderGraph {
    bool        ok = false;
    QString     error;          // set when ok is false; names the offending clip

    QStringList inputArgs;      // "-i", path, "-i", path, ...
    QString     filterGraph;    // filter_complex text, written to a script file
    QStringList outputArgs;     // -map, codec and muxer args (no output path)
    double      durationSecs = 0;
    bool        hasAudio = false;
    int         videoClips = 0;
    int         audioClips = 0;
};

namespace TimelineGraphBuilder {

// Beyond this many clips the command line and the graph stop being reasonable,
// and the segment-and-concat fallback in the ADR is the answer instead. Refusing
// is better than emitting something ffmpeg will reject obscurely.
constexpr int kMaxInputs = 32;

// Builds the graph for `timeline` at `output` settings. Returns a RenderGraph
// with ok=false and a human-readable error when the timeline cannot be rendered:
// an unlinked clip, a source file that is gone, an unsupported per-clip
// parameter, or too many clips.
RenderGraph build(const QJsonArray& timeline, const OutputSettings& output);

}  // namespace TimelineGraphBuilder
