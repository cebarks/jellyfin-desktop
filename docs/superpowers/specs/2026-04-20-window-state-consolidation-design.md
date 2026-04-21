# Window State Consolidation — Design

**Date:** 2026-04-20
**Branch:** `window-state-consolidation`
**Status:** Proposed

## Problem

Window-state logic (fullscreen, maximize, physical/logical size, position, DPI scale, save/restore) is scattered across six layers of the codebase:

- `src/settings.{h,cpp}` — persisted `WindowGeometry` struct (six plain-`int` dimension fields plus a `float scale` and a `bool maximized`; no type safety between physical and logical pixels).
- `src/main.cpp` — startup restore math (lines 558-588), post-VO-init DPI-mismatch correction (lines 725-767), and shutdown save (lines 920-974) are all inline alongside unrelated orchestration.
- `src/platform/wayland.cpp` and `src/platform/windows.cpp` — each carries its own copy of a near-identical transition state machine (fields: `transitioning`, `transition_pw/ph`, `expected_w/h`, `pending_lw/lh`, `was_fullscreen`).
- `src/mpv/event.cpp` — authoritative atomics for `s_fullscreen`, `s_display_scale`, `s_window_pw/ph`.
- `src/cef/cef_client.cpp` + `src/browser/web_browser.cpp` — HTML5-fullscreen and OSD fullscreen requests pass through the platform layer.

This produces three concrete problems:

1. The transition frame-drop predicate — which enforces the CLAUDE.md "no texture stretching during resize" rule — exists in two independent copies. A fix or refinement must be applied twice and kept synchronized forever.
2. The physical↔logical pixel conversion is a plain-`int` discipline; transposition bugs are possible and consumers defensively check `saved.scale > 0`, `saved.logical_width > 0` at every call site.
3. None of this logic is unit-tested. Critical paths (DPI-mismatch recovery, transition state machine, shutdown-save branch selection) rely entirely on manual verification.

## Goal

Consolidate the portable logic (geometry math, transition state machine) into a single testable location, while respecting CLAUDE.md's "mpv is authoritative" principle by leaving the mpv property observers where they are.

## Non-goals

- Addressing Wayland's inability to query window position — documented as a known limitation (see [Wayland position-query gap](#wayland-position-query-gap)).
- Introducing cross-platform window-position restore via xdg-positioner — follow-up work.
- Moving the `src/mpv/event.cpp` atomics or the `main.cpp` FULLSCREEN event case into the new module. These stay put.
- Redesigning the `Settings::WindowGeometry` on-disk JSON shape. The struct keeps its current field set and types so upgrades and downgrades across this refactor are lossless.

## Architecture

New files:

- `src/window_state.h` — public surface.
- `src/window_state.cpp` — implementation.
- `tests/window_state_test.cpp` — doctest unit tests, wired into `tests/CMakeLists.txt`.

Surface layout:

- A `namespace window_state` of free functions for the pure portable math (`initial_geometry`, `corrected_size_for_scale`, `save_geometry`, `to_physical`, `to_logical`). No `WindowStateManager` class — there is no state to encapsulate.
- A standalone `class TransitionGuard` owning the portable state machine. Each platform embeds one by value in its surface state (replacing the duplicated fields).

The existing `Settings::WindowGeometry` struct (six `int` dimension fields, one `float scale`, one `bool maximized`) stays as-is — it is a JSON serialization shape and gains nothing from strong types. `window_state` converts at its boundary.

## Strong types

```cpp
// src/window_state.h
namespace window_state {

struct PhysicalSize  { int w = 0; int h = 0; };
struct PhysicalPoint { int x = 0; int y = 0; };
struct LogicalSize   { int w = 0; int h = 0; };

PhysicalSize to_physical(LogicalSize ls, float scale);
LogicalSize  to_logical (PhysicalSize ps, float scale);

} // namespace window_state
```

Rationale: separate types for position vs. size (not a single `Pixels<Tag>`) because the actual bug this prevents is transposing a position and a dimension; wrapping a scalar `scale` in a struct buys nothing.

Conversion helpers clamp `scale <= 0` to `1.0` internally — the invariant is enforced in one place rather than at every call site.

## Pure functions

### Startup: two-phase geometry

The startup restore has two phases because the live display scale is only known **after** mpv VO init. Keep them as two functions:

```cpp
struct MpvInitGeometry {
    PhysicalSize  size;
    PhysicalPoint position;      // ignored when has_position == false
    bool          has_position = false;
    bool          maximized    = false;
};

// Phase 1 — pre-mpv-init. Uses saved.scale as the reference. clamp_fn is
// the platform's clamp_window_geometry function pointer (may be null).
MpvInitGeometry initial_geometry(
    const Settings::WindowGeometry& saved,
    std::function<void(int* w, int* h, int* x, int* y)> clamp_fn);

// Phase 2 — post-VO-init. Returns a corrected physical size only if the
// live scale differs from the saved scale by >= 0.01. Returns nullopt if
// saved data is absent or scales match.
std::optional<PhysicalSize> corrected_size_for_scale(
    const Settings::WindowGeometry& saved,
    double live_scale);
```

`main.cpp` renders `MpvInitGeometry` into the mpv CLI flag strings itself (four lines of formatting). `window_state` does not know about the `--geometry` flag syntax; keeping mpv-specific serialization out of the portable layer keeps the module testable without mpv.

### Shutdown: save

Mirrors the three-branch logic at `main.cpp:920-974` (fullscreen, maximized, windowed) as a single pure function that takes the observed state via arguments and returns a new `Settings::WindowGeometry`:

```cpp
struct SaveInputs {
    bool fullscreen;                   // mpv::fullscreen()
    bool maximized;                    // mpv::window_maximized()
    bool was_maximized_before_fullscreen;  // latched by the FULLSCREEN event case
    PhysicalSize window_size;          // preferred (mpv::window_pw/ph)
    PhysicalSize osd_fallback;         // used only when window_size is 0×0
    float scale;                       // platform.get_scale()
    std::function<std::optional<PhysicalPoint>()> query_position;  // may be null
};

Settings::WindowGeometry save_geometry(
    const Settings::WindowGeometry& previous,
    const SaveInputs& in);
```

Branches (evaluated in this order — `fullscreen` wins if both `fullscreen` and `maximized` are true):

- `in.fullscreen` — returns `previous` with `maximized = in.was_maximized_before_fullscreen`. Size/position/scale/logical dims are unchanged from `previous`. (Fullscreen surface size is the monitor size, not a useful "saved" size; next launch should restore the pre-fullscreen dimensions.)
- `in.maximized` (and not fullscreen) — returns `previous` with `maximized = true`. Windowed size is preserved (maximized dimensions equal the monitor size; restoring them on next launch would strand the user if maximization is lost).
- Otherwise (windowed) — computes a fresh `WindowGeometry` from `in.window_size` (falling back to `in.osd_fallback` when either component is ≤ 0), sets `scale` (clamped ≥ 1.0 if input ≤ 0), fills logical dims via `to_logical`, sets `maximized = false`, and attaches position only if `query_position` returns a value. If `window_size` and `osd_fallback` are both unusable, returns `previous` unchanged.

## TransitionGuard

```cpp
// src/window_state.h
class TransitionGuard {
public:
    // on_begin_locked fires inside begin_locked() while the caller's
    // surface mutex is held. Must not acquire that mutex.
    explicit TransitionGuard(std::function<void()> on_begin_locked = nullptr);

    // All of these require the caller to hold its surface mutex.
    void begin_locked(int current_pw, int current_ph);
    void end_locked();
    void set_expected_size_locked(int w, int h);

    bool active() const;
    int  transition_pw() const;
    int  transition_ph() const;

    // Frame-delivery predicate — drop frame when active and size doesn't
    // match the expected post-transition size.
    bool should_drop_frame(int frame_pw, int frame_ph) const;

    // If active and frame matches expected size, calls end_locked() and
    // returns true.
    bool maybe_end_on_frame(int frame_pw, int frame_ph);

    // Pending logical dims preserved across transition (consumed by CEF
    // on transition end to resize browsers).
    int  pending_lw() const;
    int  pending_lh() const;
    void set_pending_logical(int lw, int lh);

private:
    std::function<void()> on_begin_locked_;
    bool transitioning_ = false;
    int  transition_pw_ = 0, transition_ph_ = 0;
    int  expected_w_    = 0, expected_h_    = 0;
    int  pending_lw_    = 0, pending_lh_    = 0;
};
```

**Thread-safety contract:** `TransitionGuard` carries no mutex. Every `_locked` method documents "caller holds the surface mutex." This is the same implicit contract that exists in `wl_begin_transition_locked` / `win_begin_transition_locked` today — we just make it explicit and shared.

**Platform injection:** `std::function on_begin_locked` (not a virtual interface). There is no polymorphism hierarchy here — just two static-linkage call sites (Wayland's Wayland-protocol calls, Windows' DComp teardown). `std::function` is trivially testable without a platform: unit tests pass a lambda that sets a flag.

## Replacing sync `mpv_get_property` calls

`src/main.cpp:726` and `:729` currently make synchronous `mpv_get_property` calls for `display-hidpi-scale` and `fullscreen`. Both values are already tracked by observed atomics in `src/mpv/event.cpp`:

- `mpv::display_scale()` (atomic seeded via `MPV_OBSERVE_DISPLAY_SCALE`, registered **before** `osd-dimensions` at `event.cpp:55` specifically so it is populated by the time the osd-dims wait loop at `main.cpp:651` finishes).
- `mpv::fullscreen()` (atomic seeded via `MPV_OBSERVE_FULLSCREEN`).

Replace the two sync calls with the atomic accessors. This aligns with CLAUDE.md's event-driven rule and eliminates the only remaining sync `mpv_get_property` calls in the startup path. Ships as step 1 of the migration (below), decoupled from the module changes.

## Wayland position-query gap

`wayland.cpp` returns `false` from `query_window_position` unconditionally. On Wayland this is correct — there is no standard protocol that reliably exposes toplevel position. Behavior today: `x = y = -1` in settings; restore centers. Keep this behavior. Add `// TODO(wayland-position):` markers at the query site and in the spec so a future pass pursuing `xdg-activation-token` or compositor-specific protocols (KDE `org_kde_plasma_window`) has an entry point. Out of scope for this refactor.

## Migration plan

Each step is independently mergeable and keeps `main`, `just build`, and `just test` green.

### Step 1 — Replace sync mpv property calls
Edit `src/main.cpp:726,729` to use `mpv::display_scale()` and `mpv::fullscreen()`. No new files. One commit.

### Step 2 — Land types, pure functions, tests
Create `src/window_state.{h,cpp}` with `PhysicalSize`, `PhysicalPoint`, `LogicalSize`, `to_physical`, `to_logical`, `initial_geometry`, `corrected_size_for_scale`, `save_geometry`. Create `tests/window_state_test.cpp`. Wire into `tests/CMakeLists.txt` linking only `src/window_state.cpp` — `Settings::WindowGeometry` is declared inline in `settings.h`, so the test binary can consume the type without pulling in `Settings::load`/`save` (which would drag in cJSON and filesystem paths). `main.cpp` unchanged. Tests pass.

### Step 3 — Cut main.cpp over
Replace startup block (558-588) with `initial_geometry()` + inline formatting of mpv flags. Replace post-VO-init block (741-767) with `corrected_size_for_scale()`. Replace shutdown block (920-974) with `save_geometry()`. Only `main.cpp` is changed; tests from step 2 already cover the math.

### Step 4 — Land TransitionGuard
Add `class TransitionGuard` to `src/window_state.h/.cpp`. Extend `tests/window_state_test.cpp` (or add `tests/transition_guard_test.cpp`) with the full test list. No platform files change yet.

### Step 5 — Cut Wayland and Windows over to TransitionGuard
In `src/platform/wayland.cpp`: remove the duplicated state fields from `WlState`, add a `TransitionGuard` member, wire `wl_begin_transition_locked` / `wl_end_transition_locked` / frame-drop checks to the guard API. Wayland-specific protocol calls move into the `on_begin_locked` lambda. In `src/platform/windows.cpp`: identical shape, DComp teardown moves into its `on_begin_locked` lambda. Both in one PR is acceptable since they are independent translation units.

## Testing

Tests live in `tests/window_state_test.cpp`, using doctest (already vendored at `third_party/doctest`, and already the test framework for `tests/jellyfin_api_test.cpp`). Build wiring in `tests/CMakeLists.txt` follows the existing pattern: link only the sources under test.

### Test cases

**`to_physical` / `to_logical`**
- `scale=1` is identity
- `scale=2` doubles/halves dimensions
- `scale=0` clamps to 1 (no divide-by-zero)
- `to_logical` round-trips through `to_physical` within 1px tolerance

**`initial_geometry`**
- No saved data returns defaults
- Valid saved size returned as-is
- Negative `x` or `y` sets `has_position = false`
- `maximized` flag propagated
- Zero-width or zero-height saved size falls back to defaults
- `clamp_fn` is called when provided
- `clamp_fn == nullptr` is safe

**`corrected_size_for_scale`**
- Same scale returns `nullopt`
- Scale change below 0.01 threshold returns `nullopt`
- Scale change above threshold returns resized physical size
- `live_scale == 0` returns `nullopt`
- `saved.scale == 0` uses `kDefaultScale` as reference
- Absent saved logical dims uses defaults
- Result is `lround(logical * live_scale)`

**`save_geometry`**
- Fullscreen preserves saved size, sets `maximized = in.was_maximized_before_fullscreen`
- Fullscreen + `maximized` both true → fullscreen branch wins
- Maximized (not fullscreen) preserves saved windowed size, sets `maximized = true`
- Windowed saves `pw/ph` from `window_size`
- Windowed falls back to `osd_fallback` when either `window_size` component is ≤ 0
- Windowed with both zero does not overwrite previous geometry
- Position from `query_position` stored when it returns a value
- Position not stored when `query_position` returns `nullopt`
- `query_position == nullptr` is safe
- `scale ≤ 0` from caller is clamped to 1.0
- Logical dims computed as `lround(pw / scale)`

**`TransitionGuard`**
- Initial state not active
- `begin_locked` sets active, captures `current_pw/ph` as transition size
- `end_locked` clears active and expected size
- `on_begin_locked` callback invoked on begin
- Null `on_begin_locked` is safe
- Double `begin_locked` (no prior end) resets state cleanly
- `should_drop_frame`: inactive never drops
- `should_drop_frame`: active, no expected size, drops all
- `should_drop_frame`: active, frame matches transition size, drops
- `should_drop_frame`: active, frame matches expected size, does not drop
- `maybe_end_on_frame`: frame matches expected → `end_locked` + return true
- `maybe_end_on_frame`: frame doesn't match → return false
- `maybe_end_on_frame`: inactive returns false
- `set_expected_size_locked` updates drop predicate
- `set_pending_logical` preserved through end
- Out-of-order `end_locked` when inactive is a no-op

Approximately 45 cases total.

## Risks

- **Scale-atomic staleness.** Step 1 assumes `s_display_scale` is populated by the time `main.cpp:726` runs. This is true by construction (`display-hidpi-scale` is observed before `osd-dimensions`, and the wait loop only exits on an osd-dims event, after which initial property values have been delivered in registration order). If mpv's delivery order ever changes, the atomic would return 0 and the rescale branch would no-op — safe failure mode (same behavior as a missing saved scale).
- **TransitionGuard cross-platform fidelity.** The senior consult confirmed by reading both platforms' transition code that the state machine is structurally identical; the divergence is purely in platform-specific I/O inside `begin`. The `on_begin_locked` callback preserves that flexibility. Step 5 is the verification step — if a platform's wiring reveals an unexpected asymmetry, the guard API can grow (e.g., an `on_end_locked`) without changing the callers.
- **Settings JSON shape unchanged.** Step 2 does not migrate `WindowGeometry`'s on-disk shape — same keys, same types. Upgrades and downgrades across this refactor are lossless.
