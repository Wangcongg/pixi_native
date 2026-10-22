#pragma once

namespace spinex {

// Keep AnimationState update/apply and lifecycle events independent from world-pose/quad emission.
// A render bound is only a snapshot of the last emitted pose. It cannot be used to decide whether
// a running animation needs another pose: an authored entrance may legitimately stay outside the
// viewport for several seconds before moving in. If native switches that instance to StateOnly,
// the skeleton/world pose and emitted geometry never move, so its stale offscreen bound becomes a
// self-sustaining culling gate. Match spine-pixi here: a visible display-tree branch keeps producing
// poses; only a genuinely hidden parent chain is allowed to suppress pose/quad work.
enum class ClockAction {
    Frozen,
    StateOnly,
    Full
};

// A newly-created native Spine can outlive the soft GPU residency of its atlas pages. The first
// instance then starts an asynchronous rehydrate. Until both the display chain and those pages are
// ready, there is no drawable frame to which animation time can belong: consuming the business
// delta here makes timers/UI run ahead of an invisible skeleton. Drop that pre-roll time and emit
// one setup-time pose when the gate first opens; subsequent frames preserve the normal clock.
enum class TextureGateAction {
    Waiting,
    FirstDrawable,
    Ready
};

inline TextureGateAction textureGateAction(bool canDraw, bool &gateOpen, float &pendingDt) {
    if (gateOpen) return TextureGateAction::Ready;
    pendingDt = 0.0f;
    if (!canDraw) return TextureGateAction::Waiting;
    gateOpen = true;
    return TextureGateAction::FirstDrawable;
}

inline ClockAction clockAction(bool manualClock, float pendingDt, bool chainVisible,
                               bool outsideViewport, bool primed) {
    (void)outsideViewport;
    if (manualClock) {
        if (!chainVisible)
            return pendingDt > 0.0f ? ClockAction::StateOnly : ClockAction::Frozen;
        // A zero-dt full pass is intentional for a newly assigned manual-clock animation: it
        // establishes the first pose even before the business clock supplies its first delta.
        if (!primed) return ClockAction::Full;
        return pendingDt > 0.0f ? ClockAction::Full : ClockAction::Frozen;
    }
    if (!chainVisible) return ClockAction::Frozen;
    return ClockAction::Full;
}

// A manual-clock Spine can be replaced by cacheAsBitmap while its display chain is hidden.
// Its native node TYPE then belongs to the cached sprite, even though Instance::primed still
// describes the last live Spine quads. The first visible presentation must therefore rebuild
// the current zero-time pose before it is allowed to draw. Remember the observed visibility
// edge instead of clearing primed on every hidden frame: isDrawable() keeps its established
// meaning and a steady visible/frozen Spine remains the zero-work fast path.
inline bool visibilityReactivation(bool chainVisible, bool &visibilityKnown,
                                   bool &wasChainVisible) {
    const bool reactivated = visibilityKnown && chainVisible && !wasChainVisible;
    visibilityKnown = true;
    wasChainVisible = chainVisible;
    return reactivated;
}

// JS manual-clock deltas are submitted in a batch, so a microtask may replace a track after its
// delta has crossed the bridge but before native present consumes it. A destructive track change
// is an ordering fence: browser Spine already spent that delta on the previous state; applying it
// to the replacement animation would make the new animation start partway through.
inline void fenceTrackReplacement(float &pendingDt) {
    pendingDt = 0.0f;
}

}  // namespace spinex
