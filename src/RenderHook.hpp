#pragma once

// Hooks Hyprland's per-window render call to apply the cosmetic-only
// subpixel offset and rotation physics computes (see
// CPhysicsWorld::visualTransform). No-ops (with a one-time notification) if
// the internal symbol can't be found — physics itself doesn't depend on it.
void initRenderHook();
void destroyRenderHook();
