#pragma once

// Pure timeline edit math for the editor's clip model.
//
// A clip occupies [start, start + dur) on the timeline and consumes
// [sourceIn, sourceIn + dur * speedFactor) of its source file. Keeping the
// arithmetic here, out of the widget, is what makes the source-consumption
// contract in docs/adr/0001-timeline-clip-source-reference.md testable without
// simulating mouse drags.

#include <algorithm>

struct TimelineTrim {
    double start    = 0.0;
    double dur      = 0.0;
    double sourceIn = 0.0;
};

// Left-edge trim. The clip's end stays fixed, so dragging the edge right
// shortens the clip and walks the in-point forward by the matching amount of
// source time. A linked clip cannot walk past the head of its source, so the
// edge stops where sourceIn would go negative; an unlinked clip has no source
// to run out of and is bounded only by the timeline and the minimum duration.
inline TimelineTrim trimLeftEdge(double origStart, double origDur, double origSourceIn,
                                 double speedFactor, double requestedStart,
                                 bool linked, double minDur) {
    const double end = origStart + origDur;
    double start = std::min(requestedStart, end - minDur);
    if (start < 0.0) start = 0.0;
    if (linked && speedFactor > 0.0) {
        // Timeline seconds of source still available to the left of the clip.
        const double headroom = origSourceIn / speedFactor;
        start = std::max(start, origStart - headroom);
    }

    TimelineTrim out;
    out.start    = start;
    out.dur      = end - start;
    out.sourceIn = origSourceIn + (start - origStart) * speedFactor;
    if (out.sourceIn < 0.0) out.sourceIn = 0.0;   // absorbs float drift at the clamp
    return out;
}

// Right-edge trim. The in-point does not move; only the consumed length changes.
inline TimelineTrim trimRightEdge(double start, double sourceIn, double requestedEnd,
                                  double minDur, double timelineLen) {
    double end = std::min(requestedEnd, timelineLen);
    end = std::max(end, start + minDur);

    TimelineTrim out;
    out.start    = start;
    out.dur      = end - start;
    out.sourceIn = sourceIn;
    return out;
}

// In-point of the right-hand clip produced by splitting at `cut`. The left-hand
// clip keeps its own start and in-point, so only this side has to be derived.
inline double splitSourceIn(double start, double sourceIn, double speedFactor, double cut) {
    const double consumed = (cut - start) * speedFactor;
    return sourceIn + std::max(0.0, consumed);
}
