#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <vector>
#include <chrono>

// One simulated window. Everything here is in absolute (layout) px.
struct SPhysicsBody {
    PHLWINDOWREF window;

    Vector2D     velocity   = {0, 0};
    Vector2D     lastKnownPos;  // the position we last wrote (or last saw the compositor write)
    Vector2D     lastKnownSize; // ditto, for size — a resize counts as "hands off" too
    bool         grabbed    = false; // true while an external actor (the user, a layout) is
                                      // actively repositioning/resizing this window
    int          idleTicks  = 0;     // ticks since the last external move/resize, while grabbed
    bool         asleep     = false; // resting on the floor / another window, velocity ~ 0
    bool         initialised = false;
};

class CPhysicsWorld {
  public:
    // (Re)creates or updates the body list to match every currently mapped,
    // floating, non-pinned window. Call on window open/close/floating toggle.
    void syncBody(PHLWINDOW window);
    void removeBody(PHLWINDOW window);
    void removeAll();

    // Give a window an initial velocity and hand it over to the simulation
    // immediately (used by the physics:throw dispatcher).
    void throwWindow(PHLWINDOW window, const Vector2D& velocity);

    // Zero every body's velocity in place, without moving anything.
    void stopAll();

    // Advance the whole world by one compositor tick.
    void step();

    size_t bodyCount() const {
        return m_bodies.size();
    }

  private:
    SPhysicsBody*                        find(PHLWINDOW window);
    bool                                 eligible(const PHLWINDOW& window) const;
    bool                                 resolveBounds(SPhysicsBody& body, Vector2D& pos, const Vector2D& size);
    void                                 resolvePairs(double dt);

    std::vector<SPhysicsBody>            m_bodies;
    std::chrono::steady_clock::time_point m_lastStep{};
    bool                                  m_haveLastStep = false;
};

inline CPhysicsWorld g_physicsWorld;
