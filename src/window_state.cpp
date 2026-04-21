#include "window_state.h"
#include <cmath>

namespace window_state {

PhysicalSize to_physical(LogicalSize ls, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    return { static_cast<int>(std::lround(ls.w * scale)),
             static_cast<int>(std::lround(ls.h * scale)) };
}

LogicalSize to_logical(PhysicalSize ps, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    return { static_cast<int>(std::lround(ps.w / scale)),
             static_cast<int>(std::lround(ps.h / scale)) };
}
MpvInitGeometry initial_geometry(
    const Settings::WindowGeometry& saved,
    std::function<void(int* w, int* h, int* x, int* y)> clamp_fn)
{
    using WG = Settings::WindowGeometry;
    MpvInitGeometry result;

    int w = (saved.width > 0 && saved.height > 0)
                ? saved.width  : WG::kDefaultPhysicalWidth;
    int h = (saved.width > 0 && saved.height > 0)
                ? saved.height : WG::kDefaultPhysicalHeight;
    int x = saved.x;
    int y = saved.y;

    if (clamp_fn) clamp_fn(&w, &h, &x, &y);

    result.size      = { w, h };
    result.maximized = saved.maximized;

    if (x >= 0 && y >= 0) {
        result.position     = { x, y };
        result.has_position = true;
    }
    return result;
}
std::optional<PhysicalSize> corrected_size_for_scale(
    const Settings::WindowGeometry& saved,
    double live_scale)
{
    using WG = Settings::WindowGeometry;
    if (live_scale <= 0.0) return std::nullopt;

    float saved_scale = saved.scale > 0.f ? saved.scale : WG::kDefaultScale;
    if (std::fabs(live_scale - saved_scale) < 0.01) return std::nullopt;

    int lw = saved.logical_width  > 0 ? saved.logical_width  : WG::kDefaultLogicalWidth;
    int lh = saved.logical_height > 0 ? saved.logical_height : WG::kDefaultLogicalHeight;

    return PhysicalSize{
        static_cast<int>(std::lround(lw * live_scale)),
        static_cast<int>(std::lround(lh * live_scale))
    };
}
Settings::WindowGeometry save_geometry(
    const Settings::WindowGeometry& previous,
    const SaveInputs& in)
{
    if (in.fullscreen) {
        auto geom = previous;
        geom.maximized = in.was_maximized_before_fullscreen;
        return geom;
    }
    if (in.maximized) {
        auto geom = previous;
        geom.maximized = true;
        return geom;
    }

    // Windowed branch: build a fresh geometry from live state.
    int pw = in.window_size.w;
    int ph = in.window_size.h;
    if (pw <= 0 || ph <= 0) {
        pw = in.osd_fallback.w;
        ph = in.osd_fallback.h;
    }
    if (pw <= 0 || ph <= 0) return previous;

    Settings::WindowGeometry geom;
    geom.width  = pw;
    geom.height = ph;

    float scale = in.scale;
    if (scale <= 0.0f) scale = 1.0f;
    geom.scale          = scale;
    geom.logical_width  = static_cast<int>(std::lround(pw / scale));
    geom.logical_height = static_cast<int>(std::lround(ph / scale));
    geom.maximized = false;

    if (in.query_position) {
        if (auto pos = in.query_position()) {
            geom.x = pos->x;
            geom.y = pos->y;
        }
    }
    return geom;
}

TransitionGuard::TransitionGuard(std::function<void()> on_begin_locked)
    : on_begin_locked_(std::move(on_begin_locked)) {}
void TransitionGuard::begin_locked(int pw, int ph) { (void)pw; (void)ph; }
void TransitionGuard::end_locked() {}
void TransitionGuard::set_expected_size_locked(int w, int h) { (void)w; (void)h; }
bool TransitionGuard::active() const { return false; }
int  TransitionGuard::transition_pw() const { return transition_pw_; }
int  TransitionGuard::transition_ph() const { return transition_ph_; }
bool TransitionGuard::should_drop_frame(int pw, int ph) const {
    (void)pw; (void)ph; return false;
}
bool TransitionGuard::maybe_end_on_frame(int pw, int ph) {
    (void)pw; (void)ph; return false;
}
int  TransitionGuard::pending_lw() const { return pending_lw_; }
int  TransitionGuard::pending_lh() const { return pending_lh_; }
void TransitionGuard::set_pending_logical(int lw, int lh) { (void)lw; (void)lh; }

} // namespace window_state
