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

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

// A clip the build shortened because its source ends before the clip does
// (docs/adr/0001-timeline-clip-source-reference.md, contract 1). It renders up
// to the end of its source instead of being refused, and is listed so the job
// can say what was cut.
struct ClampedClip {
    int     index = 0;             // position in the timeline array
    QString name;                  // the clip's label, or "clip N" without one
    double  requestedSecs = 0.0;   // dur on the timeline
    double  renderedSecs  = 0.0;   // what its source has left after sourceIn
};

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
    QVector<ClampedClip> clamped;  // clips cut at the end of their source
};

namespace TimelineGraphBuilder {

// Beyond this many clips the command line and the graph stop being reasonable,
// and the segment-and-concat fallback in the ADR is the answer instead. Refusing
// is better than emitting something ffmpeg will reject obscurely.
constexpr int kMaxInputs = 32;

// The longest a clip may be, and the latest it may start, in seconds: a day.
// The editor's timeline is no longer than this, so a clip it places whole can
// also be rendered whole.
constexpr double kMaxClipSeconds = 86400.0;

// Builds the graph for `timeline` at `output` settings. Returns a RenderGraph
// with ok=false and a human-readable error when the timeline cannot be rendered:
// an unlinked clip, a source file that is gone, an unsupported per-clip
// parameter, a clip that starts after its source ends, or too many clips.
//
// `sourceSeconds` is the length of each source file, keyed by sourcePath as the
// clips name it. The builder does not measure anything itself; the caller
// probes the files. A clip that runs past the end of its source is cut there
// and listed in RenderGraph::clamped. A path that is absent, or whose length is
// not a positive number, is taken to be long enough, which is how a render
// goes when the lengths could not be read.
RenderGraph build(const QJsonArray& timeline, const OutputSettings& output,
                  const QHash<QString, double>& sourceSeconds = {});

// One line per clamped clip, naming it and saying how much of it was cut: the
// text a render job records. Empty when nothing was clamped.
QString describeClamps(const QVector<ClampedClip>& clamped);

}  // namespace TimelineGraphBuilder
