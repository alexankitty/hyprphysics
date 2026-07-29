#define WLR_USE_UNSTABLE

#include "PhysicsWorld.hpp"
#include "Config.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <algorithm>
#include <cmath>

using namespace Desktop::View;

static Vector2D clampLen(const Vector2D& v, double maxLen) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len <= maxLen || len <= 0.0001)
        return v;
    const double s = maxLen / len;
    return {v.x * s, v.y * s};
}

bool CPhysicsWorld::eligible(const PHLWINDOW& window) const {
    if (!window || !Desktop::View::validMapped(window))
        return false;
    if (window->m_pinned)
        return false;
    if (Fullscreen::controller()->isFullscreen(window))
        return false;
    if (!window->m_isFloating && !g_config.affectTiled->value())
        return false;
    if (!window->m_workspace || !window->m_workspace->m_visible)
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
    body.window       = window;
    body.lastKnownPos  = window->position(GEOMETRIC_GOAL);
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

void CPhysicsWorld::resolveBounds(SPhysicsBody& body, Vector2D& pos, const Vector2D& size) {
    const auto window = body.window.lock();
    if (!window || !window->m_monitor)
        return;

    const auto  monitor    = window->m_monitor.lock();
    const auto  restitution = std::clamp(g_config.restitution->value(), 0.0, 1.0);

    const double left   = monitor->m_position.x;
    const double top    = monitor->m_position.y;
    const double right  = monitor->m_position.x + monitor->m_size.x - size.x;
    const double bottom = monitor->m_position.y + monitor->m_size.y - size.y;

    if (pos.x < left) {
        pos.x         = left;
        body.velocity.x = -body.velocity.x * restitution;
    } else if (pos.x > right) {
        pos.x         = right;
        body.velocity.x = -body.velocity.x * restitution;
    }

    if (pos.y < top) {
        pos.y         = top;
        body.velocity.y = -body.velocity.y * restitution;
    } else if (pos.y > bottom) {
        pos.y         = bottom;
        body.velocity.y = -body.velocity.y * restitution;

        // resting on the floor: kill vertical jitter once it is basically zero,
        // and bleed a bit of horizontal speed off, like real friction.
        if (std::fabs(body.velocity.y) < g_config.sleepVelocity->value())
            body.velocity.y = 0;
    }
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

            if (wa->m_monitor.lock() != wb->m_monitor.lock())
                continue;

            auto&        a    = m_bodies[i];
            auto&        b    = m_bodies[j];
            const Vector2D posA = wa->position(GEOMETRIC_GOAL);
            const Vector2D posB = wb->position(GEOMETRIC_GOAL);
            const Vector2D sizeA = wa->size(GEOMETRIC_GOAL);
            const Vector2D sizeB = wb->size(GEOMETRIC_GOAL);

            const double overlapX = std::min(posA.x + sizeA.x, posB.x + sizeB.x) - std::max(posA.x, posB.x);
            const double overlapY = std::min(posA.y + sizeA.y, posB.y + sizeB.y) - std::max(posA.y, posB.y);

            if (overlapX <= 0 || overlapY <= 0)
                continue; // no intersection

            const double        restitution = std::clamp(g_config.restitution->value(), 0.0, 1.0);
            // crude mass proxy: bigger windows push smaller ones around less
            const double massA = std::max(sizeA.x * sizeA.y, 1.0);
            const double massB = std::max(sizeB.x * sizeB.y, 1.0);
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

            Vector2D newPosA = posA + normal * (push * shareA);
            Vector2D newPosB = posB - normal * (push * shareB);

            if (!a.grabbed) {
                *wa->positionAnimation() = newPosA;
                wa->positionAnimation()->warp();
                a.lastKnownPos = newPosA;
            }
            if (!b.grabbed) {
                *wb->positionAnimation() = newPosB;
                wb->positionAnimation()->warp();
                b.lastKnownPos = newPosB;
            }

            // simple 1D elastic-ish impulse along the collision normal
            const double relVel   = (a.velocity.x - b.velocity.x) * normal.x + (a.velocity.y - b.velocity.y) * normal.y;
            if (relVel < 0) {
                const double impulse = -(1.0 + restitution) * relVel / (1.0 / massA + 1.0 / massB);
                if (!a.grabbed)
                    a.velocity = a.velocity + normal * (impulse / massA);
                if (!b.grabbed)
                    b.velocity = b.velocity - normal * (impulse / massB);
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

    double dt = std::chrono::duration<double>(now - m_lastStep).count();
    m_lastStep = now;
    // guard against huge dt after a stall (e.g. compositor was suspended)
    dt = std::clamp(dt, 0.0, 0.05);

    if (!g_config.enabled->value() || !g_runtimeEnabled || dt <= 0.0)
        return;

    const double gravity          = g_config.gravity->value();
    const double frictionPerSec   = std::clamp(g_config.friction->value(), 0.0, 1.0);
    const double frictionThisTick = std::pow(frictionPerSec, dt);
    const double maxVel           = g_config.maxVelocity->value();
    const double sleepVel         = g_config.sleepVelocity->value();
    const int    releaseTicks     = std::max(1, (int)g_config.grabReleaseTicks->value());

    // prune dead / no-longer-eligible bodies first
    std::erase_if(m_bodies, [&](SPhysicsBody& b) {
        auto w = b.window.lock();
        return !w || !eligible(w);
    });

    for (auto& body : m_bodies) {
        auto window = body.window.lock();
        if (!window)
            continue;

        const Vector2D goalPos = window->position(GEOMETRIC_GOAL);
        const Vector2D size    = window->size(GEOMETRIC_GOAL);

        // Did something other than us move this window since the last tick?
        // (interactive drag, a tiling layout, a workspace switch snapping it back, ...)
        const bool externallyMoved = (goalPos - body.lastKnownPos).size() > 0.5;

        if (externallyMoved) {
            if (dt > 0.0)
                body.velocity = (goalPos - body.lastKnownPos) * (1.0 / dt);
            body.lastKnownPos = goalPos;
            body.grabbed      = true;
            body.idleTicks    = 0;
            body.asleep       = false;
            continue; // let the other actor keep driving this tick; we just watched
        }

        if (body.grabbed) {
            body.idleTicks++;
            if (body.idleTicks < releaseTicks)
                continue; // still settling right after a drag, don't fight it yet
            body.grabbed = false; // released: hand off to gravity with the last measured velocity
        }

        if (body.asleep)
            continue;

        body.velocity.y += gravity * dt;
        body.velocity      = body.velocity * frictionThisTick;
        body.velocity      = clampLen(body.velocity, maxVel);

        Vector2D newPos = goalPos + body.velocity * dt;
        resolveBounds(body, newPos, size);

        if (newPos != goalPos) {
            *window->positionAnimation() = newPos;
            window->positionAnimation()->warp();
        }
        body.lastKnownPos = newPos;

        if (body.velocity.size() < sleepVel)
            body.asleep = true;
    }

    resolvePairs(dt);
}
