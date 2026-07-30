#define WLR_USE_UNSTABLE

#include "PhysicsWorld.hpp"
#include "Config.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

// Adds a tangential-impact torque, gated by the spin master switch and
// clamped the same way linear velocity is. Signs are chosen for a plausible
// "tumble in the direction of travel" look, not rigorous rigid-body physics —
// there's no real contact-point offset to work from once a bounce is reduced
// to a 1D axis flip or an overlap-resolution push.
static void addSpin(SPhysicsBody& body, double tangentialSpeed) {
    if (!g_config.spin->value())
        return;
    const double maxAngVel   = std::fabs(g_config.maxAngularVelocity->value());
    body.angularVelocity     = std::clamp(body.angularVelocity + tangentialSpeed * g_config.spinFactor->value(), -maxAngVel, maxAngVel);
}

static Vector2D clampLen(const Vector2D& v, double maxLen) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len <= maxLen || len <= 0.0001)
        return v;
    const double s = maxLen / len;
    return {v.x * s, v.y * s};
}

// monitor_traversal needs the *walkable extent* along each axis, not a
// single monitor's box: a whole-box lookup (whichever monitor contains the
// window) makes a body's own edge its own wall, so it can approach a shared
// seam but never actually cross it — the clamp that stops it exiting its
// current monitor never lets its center reach the neighbor's box in the
// first place. Splitting the two axes fixes that: the left/right extent
// comes from every monitor that shares this row (touching/overlapping
// monitors merged into one run), so a body walks straight over the seam
// between them, while the top/bottom extent still comes from whichever
// monitor is actually under it — so a shorter neighbor still gets its own
// floor instead of inheriting the tallest monitor's.
struct SSpan {
    double lo = 0.0, hi = 0.0;
};

// Merges touching/overlapping spans into contiguous runs, then returns the
// run containing `at` — or, if `at` falls in a gap between runs, whichever
// run is nearest, so a body crossing a gap still has something to land on.
static SSpan resolveSpan(std::vector<SSpan> spans, double at) {
    std::sort(spans.begin(), spans.end(), [](const SSpan& a, const SSpan& b) { return a.lo < b.lo; });

    constexpr double EPS = 1.0; // covers rounding noise in monitor placement
    std::vector<SSpan> merged;
    for (const auto& s : spans) {
        if (!merged.empty() && s.lo <= merged.back().hi + EPS)
            merged.back().hi = std::max(merged.back().hi, s.hi);
        else
            merged.push_back(s);
    }

    SSpan  best     = merged.front();
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& s : merged) {
        if (at >= s.lo && at <= s.hi)
            return s;
        const double dist = std::min(std::fabs(at - s.lo), std::fabs(at - s.hi));
        if (dist < bestDist) {
            bestDist = dist;
            best     = s;
        }
    }
    return best;
}

// Left/right walls: the merged x-span of every enabled monitor whose y-range
// contains `y` — i.e. every screen forming one contiguous row at this height.
static bool monitorRowSpan(double y, double x, SSpan& out) {
    std::vector<SSpan> spans;
    for (const auto& m : g_pCompositor->m_monitors) {
        if (!m->m_enabled || y < m->m_position.y || y > m->m_position.y + m->m_size.y)
            continue;
        spans.push_back({m->m_position.x, m->m_position.x + m->m_size.x});
    }
    if (spans.empty())
        return false;
    out = resolveSpan(std::move(spans), x);
    return true;
}

// Top/bottom (ceiling/floor): the merged y-span of every enabled monitor
// whose x-range contains `x` — the contiguous column of screens below/above.
static bool monitorColumnSpan(double x, double y, SSpan& out) {
    std::vector<SSpan> spans;
    for (const auto& m : g_pCompositor->m_monitors) {
        if (!m->m_enabled || x < m->m_position.x || x > m->m_position.x + m->m_size.x)
            continue;
        spans.push_back({m->m_position.y, m->m_position.y + m->m_size.y});
    }
    if (spans.empty())
        return false;
    out = resolveSpan(std::move(spans), y);
    return true;
}

// Ground truth, not a heuristic: is the user *right now* interactively
// moving or resizing this exact window via Hyprland's own drag controller?
// Using this instead of inferring "did the position change" avoids the
// one-frame lag a delta-based guess has — that lag was exactly what let
// physics fight the drag and cause the wiggle/offset-from-cursor behavior.
static bool isInteractivelyControlled(const PHLWINDOW& window) {
    if (!g_layoutManager)
        return false;

    const auto& drag = g_layoutManager->dragController();
    if (!drag || drag->mode() == MBIND_INVALID)
        return false;

    const auto target = drag->target();
    if (!target || target->type() != Layout::TARGET_TYPE_WINDOW)
        return false;

    const auto windowTarget = dynamicPointerCast<Layout::CWindowTarget>(target);
    return windowTarget && windowTarget->window() == window;
}

// Move a floating window through the layout system rather than poking
// m_realPosition directly. Hyprland's drag controller computes where to pick
// a window up from its layout target's *own* cached box (ITarget::position(),
// separate from m_realPosition), and only setTargetGeom()/setPositionGlobal()
// keep that cache in sync. Writing m_realPosition alone left that cache
// stale, so grabbing a window that physics had moved would jump it back to
// wherever it was last positioned "properly" (plus the mouse delta) — the
// large-displacement-on-grab bug.
static void moveWindowTo(const PHLWINDOW& window, const Vector2D& pos, const Vector2D& size) {
    if (!window->m_target) {
        window->m_realPosition->setValueAndWarp(pos);
        return;
    }

    g_layoutManager->setTargetGeom(CBox{pos, size}, window->m_target);
    window->m_realPosition->warp(); // physics recomputes every tick; skip the move-animation curve
}

bool CPhysicsWorld::eligible(const PHLWINDOW& window) const {
    if (!window || !validMapped(window))
        return false;
    if (window->m_pinned)
        return false;
    if (window->isFullscreen())
        return false;
    if (!window->m_isFloating && !g_config.affectTiled->value())
        return false;
    if (!window->m_workspace || !window->m_workspace->m_visible)
        return false;
    // Grouped/tabbed windows all share the exact same geometry; the inactive
    // tabs are just marked hidden rather than removed. Without this check
    // every window in a group perfectly overlaps its tab-mates and the
    // collision resolver "resolves" that every tick, i.e. a window fighting
    // its own hidden tabs.
    if (!window->visible())
        return false;

    return true;
}

SPhysicsBody* CPhysicsWorld::find(PHLWINDOW window) {
    for (auto& b : m_bodies) {
        if (b.window.lock() == window)
            return &b;
    }
    return nullptr;
}

bool CPhysicsWorld::visualTransform(PHLWINDOW window, Vector2D& subpixelOffset, double& angleRad) {
    auto* b = find(window);
    if (!b)
        return false;

    subpixelOffset = b->subpixelOffset;
    angleRad       = b->angle;
    return true;
}

void CPhysicsWorld::syncBody(PHLWINDOW window) {
    if (!window)
        return;

    if (!eligible(window)) {
        removeBody(window);
        return;
    }

    if (find(window))
        return; // already tracked

    SPhysicsBody body;
    body.window        = window;
    body.lastKnownPos  = window->m_realPosition->goal();
    body.lastKnownSize = window->m_realSize->goal();
    body.initialised   = true;
    m_bodies.push_back(body);
}

void CPhysicsWorld::removeBody(PHLWINDOW window) {
    std::erase_if(m_bodies, [&](const SPhysicsBody& b) { return !b.window.lock() || b.window.lock() == window; });
}

void CPhysicsWorld::removeAll() {
    m_bodies.clear();
}

void CPhysicsWorld::throwWindow(PHLWINDOW window, const Vector2D& velocity) {
    if (!window)
        return;

    syncBody(window);
    auto* b = find(window);
    if (!b)
        return;

    b->velocity  = clampLen(velocity, g_config.maxVelocity->value());
    b->grabbed   = false;
    b->idleTicks = 0;
    b->asleep    = false;
}

void CPhysicsWorld::stopAll() {
    for (auto& b : m_bodies) {
        b.velocity = {0, 0};
        b.asleep   = true;
    }
}

// Returns true if the body is resting on the floor this tick (only then is
// falling asleep in step() appropriate — mid-arc, gravity naturally drives
// vertical speed through ~0 at the apex, and sleeping there would freeze the
// window in mid-air instead of letting it fall back down).
bool CPhysicsWorld::resolveBounds(SPhysicsBody& body, Vector2D& pos, const Vector2D& size) {
    const auto window = body.window.lock();
    if (!window || !window->m_monitor)
        return false;

    const auto restitution = std::clamp((double)g_config.restitution->value(), 0.0, 1.0);
    const Vector2D center  = pos + size * 0.5;

    SSpan row, column;
    const bool traverse = g_config.monitorTraversal->value() && monitorRowSpan(center.y, center.x, row) && monitorColumnSpan(center.x, center.y, column);

    double left, top, right, bottom;
    if (traverse) {
        left   = row.lo;
        right  = row.hi - size.x;
        top    = column.lo;
        bottom = column.hi - size.y;
    } else {
        const auto monitor = window->m_monitor.lock();
        left               = monitor->m_position.x;
        top                = monitor->m_position.y;
        right              = monitor->m_position.x + monitor->m_size.x - size.x;
        bottom             = monitor->m_position.y + monitor->m_size.y - size.y;
    }

    if (pos.x < left) {
        pos.x           = left;
        body.velocity.x = -body.velocity.x * restitution;
        addSpin(body, body.velocity.y);
    } else if (pos.x > right) {
        pos.x           = right;
        body.velocity.x = -body.velocity.x * restitution;
        addSpin(body, -body.velocity.y);
    }

    bool grounded = false;
    if (pos.y < top) {
        pos.y           = top;
        body.velocity.y = -body.velocity.y * restitution;
        addSpin(body, -body.velocity.x);
    } else if (pos.y > bottom) {
        pos.y           = bottom;
        body.velocity.y = -body.velocity.y * restitution;
        grounded        = true;
        addSpin(body, body.velocity.x);

        // resting on the floor: kill vertical jitter once it is basically zero
        if (std::fabs(body.velocity.y) < g_config.sleepVelocity->value())
            body.velocity.y = 0;
    }

    return grounded;
}

void CPhysicsWorld::resolvePairs(double dt) {
    if (!g_config.collisions->value())
        return;

    for (size_t i = 0; i < m_bodies.size(); ++i) {
        auto wa = m_bodies[i].window.lock();
        if (!wa)
            continue;

        for (size_t j = i + 1; j < m_bodies.size(); ++j) {
            auto wb = m_bodies[j].window.lock();
            if (!wb)
                continue;

            // Windows on different monitors can never overlap in absolute
            // layout px anyway (monitors don't share screen space), except
            // when monitor_traversal lets a body's box straddle the seam
            // between two adjacent monitors — so only skip the pair when
            // traversal is off, as a cheap early-out for the common case.
            if (!g_config.monitorTraversal->value() && wa->m_monitor.lock() != wb->m_monitor.lock())
                continue;

            auto&          a     = m_bodies[i];
            auto&          b     = m_bodies[j];
            const Vector2D posA  = wa->m_realPosition->goal();
            const Vector2D posB  = wb->m_realPosition->goal();
            const Vector2D sizeA = wa->m_realSize->goal();
            const Vector2D sizeB = wb->m_realSize->goal();

            const double overlapX = std::min(posA.x + sizeA.x, posB.x + sizeB.x) - std::max(posA.x, posB.x);
            const double overlapY = std::min(posA.y + sizeA.y, posB.y + sizeB.y) - std::max(posA.y, posB.y);

            if (overlapX <= 0 || overlapY <= 0)
                continue; // no intersection

            const double restitution = std::clamp((double)g_config.restitution->value(), 0.0, 1.0);
            // crude mass proxy: bigger windows push smaller ones around less
            const double massA     = std::max(sizeA.x * sizeA.y, 1.0);
            const double massB     = std::max(sizeB.x * sizeB.y, 1.0);
            const double totalMass = massA + massB;

            Vector2D normal;
            double   push;
            if (overlapX < overlapY) {
                normal = {(posA.x < posB.x) ? -1.0 : 1.0, 0.0};
                push   = overlapX;
            } else {
                normal = {0.0, (posA.y < posB.y) ? -1.0 : 1.0};
                push   = overlapY;
            }

            // separate along the shallowest axis, weighted by the other body's mass
            // (a grabbed / sleeping body of "infinite" effective mass barely moves)
            const double shareA = a.grabbed ? 0.0 : massB / totalMass;
            const double shareB = b.grabbed ? 0.0 : massA / totalMass;

            // Round for the same reason as the gravity path above: what we
            // write here must match what m_realPosition->goal() reports back
            // next tick, or the externallyMoved check misfires on rounding noise.
            const Vector2D newPosA = (posA + normal * (push * shareA)).round();
            const Vector2D newPosB = (posB - normal * (push * shareB)).round();

            if (!a.grabbed) {
                moveWindowTo(wa, newPosA, sizeA);
                a.lastKnownPos = newPosA;
            }
            if (!b.grabbed) {
                moveWindowTo(wb, newPosB, sizeB);
                b.lastKnownPos = newPosB;
            }

            // simple 1D elastic-ish impulse along the collision normal
            const double relVel = (a.velocity.x - b.velocity.x) * normal.x + (a.velocity.y - b.velocity.y) * normal.y;
            if (relVel < 0) {
                const double impulse = -(1.0 + restitution) * relVel / (1.0 / massA + 1.0 / massB);
                if (!a.grabbed)
                    a.velocity = a.velocity + normal * (impulse / massA);
                if (!b.grabbed)
                    b.velocity = b.velocity - normal * (impulse / massB);

                // slip along the contact (tangent to the normal) becomes spin —
                // a glancing hit tumbles the windows, a head-on one doesn't
                const Vector2D tangent          = {-normal.y, normal.x};
                const double   tangentialRelVel = (a.velocity.x - b.velocity.x) * tangent.x + (a.velocity.y - b.velocity.y) * tangent.y;
                if (!a.grabbed)
                    addSpin(a, tangentialRelVel);
                if (!b.grabbed)
                    addSpin(b, -tangentialRelVel);
            }

            a.asleep = false;
            b.asleep = false;
        }
    }
}

void CPhysicsWorld::step() {
    const auto now = std::chrono::steady_clock::now();
    if (!m_haveLastStep) {
        m_lastStep     = now;
        m_haveLastStep = true;
        return;
    }

    double dt  = std::chrono::duration<double>(now - m_lastStep).count();
    m_lastStep = now;
    // guard against huge dt after a stall (e.g. compositor was suspended)
    dt = std::clamp(dt, 0.0, 0.05);

    if (!g_config.enabled->value() || !g_runtimeEnabled || dt <= 0.0)
        return;

    const double gravity                 = g_config.gravity->value();
    const double frictionPerSec          = std::clamp((double)g_config.friction->value(), 0.0, 1.0);
    const double frictionThisTick        = std::pow(frictionPerSec, dt);
    const double angularFrictionPerSec   = std::clamp((double)g_config.angularFriction->value(), 0.0, 1.0);
    const double angularFrictionThisTick = std::pow(angularFrictionPerSec, dt);
    const double maxVel                  = g_config.maxVelocity->value();
    const double maxAngVel               = std::fabs(g_config.maxAngularVelocity->value());
    const double sleepVel                = g_config.sleepVelocity->value();
    const int    releaseTicks            = std::max(1, (int)g_config.grabReleaseTicks->value());

    // prune dead / no-longer-eligible bodies first
    std::erase_if(m_bodies, [&](SPhysicsBody& b) {
        auto w = b.window.lock();
        return !w || !eligible(w);
    });

    for (auto& body : m_bodies) {
        auto window = body.window.lock();
        if (!window)
            continue;

        const Vector2D goalPos = window->m_realPosition->goal();
        const Vector2D size    = window->m_realSize->goal();

        // Ground truth first: if Hyprland's drag controller currently owns
        // this window (interactive move or resize in progress), don't touch
        // its position at all — just keep a running estimate of velocity in
        // case the user throws it, for when the drag ends.
        if (isInteractivelyControlled(window)) {
            if (body.grabbed && dt > 0.0)
                body.velocity = (goalPos - body.lastKnownPos) * (1.0 / dt);
            else if (!body.grabbed)
                body.velocity = {0, 0}; // just picked up: don't inherit a stale throw

            body.lastKnownPos    = goalPos;
            body.lastKnownSize   = size;
            body.grabbed         = true;
            body.idleTicks       = 0;
            body.asleep          = false;
            // a window being carried by the user should look level and
            // pixel-exact under the cursor, not visually offset from it
            body.subpixelOffset  = {0, 0};
            body.angle           = 0.0;
            body.angularVelocity = 0.0;
            continue;
        }

        // Did something other than us move or resize this window since the
        // last tick? (a tiling layout, a workspace switch snapping it back,
        // a dispatcher like movewindowpixel, ...) A pure resize (edges
        // dragged, no move) must count here too — otherwise gravity/bounds
        // keep nudging the window mid-resize since its position alone
        // hasn't changed. This is a fallback for repositions that don't go
        // through the drag controller above.
        const bool externallyMoved  = (goalPos - body.lastKnownPos).size() > 0.5;
        const bool externallyResized = (size - body.lastKnownSize).size() > 0.5;

        if (externallyMoved || externallyResized) {
            if (dt > 0.0 && externallyMoved)
                body.velocity = (goalPos - body.lastKnownPos) * (1.0 / dt);
            else if (externallyResized)
                body.velocity = {0, 0}; // resizing in place: don't carry over stale velocity
            body.lastKnownPos    = goalPos;
            body.lastKnownSize   = size;
            body.grabbed         = true;
            body.idleTicks       = 0;
            body.asleep          = false;
            body.subpixelOffset  = {0, 0};
            body.angle           = 0.0;
            body.angularVelocity = 0.0;
            continue; // let the other actor keep driving this tick; we just watched
        }

        // releaseTicks no longer holds gravity off: it only keeps the body
        // exempt from collision push (see resolvePairs' shareA/shareB) for a
        // couple of ticks right after a drag, in case Hyprland's drag
        // controller needs a beat to fully let go. Gravity must start the
        // instant the drag ends, or the window visibly hangs in place for
        // those ticks before falling.
        if (body.grabbed) {
            body.idleTicks++;
            if (body.idleTicks >= releaseTicks)
                body.grabbed = false; // released: hand off to gravity with the last measured velocity
        }

        if (body.asleep)
            continue;

        body.velocity.y += gravity * dt;
        body.velocity = body.velocity * frictionThisTick;
        body.velocity = clampLen(body.velocity, maxVel);

        if (g_config.spin->value()) {
            body.angularVelocity = std::clamp(body.angularVelocity * angularFrictionThisTick, -maxAngVel, maxAngVel);
            body.angle += body.angularVelocity * dt;
        }

        Vector2D newPos     = goalPos + body.velocity * dt;
        const bool grounded = resolveBounds(body, newPos, size);
        // Hyprland snaps window positions to whole pixels, so round before
        // writing: otherwise we store the unrounded float here while the
        // compositor reports back a rounded value next tick, and the
        // externallyMoved sub-pixel mismatch is misread as someone else
        // moving the window — causing a velocity/grabbed flip-flop (jiggle).
        // The fractional remainder isn't thrown away though — it's kept as a
        // cosmetic nudge for the render hook, so motion still reads as
        // continuous instead of snapping between integer pixels.
        const Vector2D unroundedPos = newPos;
        newPos                      = newPos.round();
        body.subpixelOffset         = unroundedPos - newPos;

        if (newPos != goalPos)
            moveWindowTo(window, newPos, size);
        body.lastKnownPos = newPos;

        if (grounded && body.velocity.size() < sleepVel) {
            body.asleep = true;
            // come to rest level — a permanently tilted resting window would
            // leave its (unrotated) click hitbox visibly mismatched forever,
            // not just for the duration of a bounce/tumble
            body.angle           = 0.0;
            body.angularVelocity = 0.0;
        }
    }

    resolvePairs(dt);
}
