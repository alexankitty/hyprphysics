#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>

// Every tunable the plugin exposes under plugin:physics:*. Populated once in
// PLUGIN_INIT and read every tick — cheap SP<> dereferences, no re-parsing.
struct SPhysicsConfig {
    SP<Config::Values::CBoolValue>  enabled;
    SP<Config::Values::CBoolValue>  collisions;
    SP<Config::Values::CBoolValue>  affectTiled;
    SP<Config::Values::CBoolValue>  monitorTraversal; // let windows fall/bounce across monitor edges instead of stopping at them
    SP<Config::Values::CFloatValue> gravity;        // px/s^2
    SP<Config::Values::CFloatValue> restitution;    // 0..1, bounciness
    SP<Config::Values::CFloatValue> friction;       // 0..1 retained per second
    SP<Config::Values::CFloatValue> throwMultiplier;
    SP<Config::Values::CFloatValue> maxVelocity;     // px/s clamp
    SP<Config::Values::CFloatValue> sleepVelocity;    // px/s, below + resting = sleep
    SP<Config::Values::CFloatValue> grabReleaseTicks; // ticks of stillness before "released"
};

inline SPhysicsConfig g_config;

// Config::Values::* mirrors are read-only (they reflect whatever hyprland.conf
// or hyprland.lua last set), so `physics:toggle` can't write through them.
// This is the actual runtime switch; step() checks g_config.enabled->value()
// AND this, so either the config file or the dispatcher can turn things off.
inline bool g_runtimeEnabled = true;
