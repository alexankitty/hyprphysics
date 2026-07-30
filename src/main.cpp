#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprutils/string/VarList.hpp>

#include "../globals.hpp"
#include "Config.hpp"
#include "PhysicsWorld.hpp"

#include <sstream>

using namespace Hyprutils::String;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// There's no "floating toggled" / "tiled -> floating" event on this Hyprland
// version, so instead of trying to catch every state transition individually
// we just resync against the full window list once a tick. syncBody() is a
// cheap no-op for windows that are already tracked and already eligible.
static void rescanWindows() {
    for (const auto& w : g_pCompositor->m_windows)
        g_physicsWorld.syncBody(w);
}

static SDispatchResult dispatchThrow(std::string arg) {
    // physics:throw <vx> <vy>  — throws the currently active window
    CVarList args{arg, 0, ' ', true};
    if (args.size() < 2)
        return {.success = false, .error = "usage: physics:throw <vx> <vy>"};

    const auto window = Desktop::focusState()->window();
    if (!window)
        return {.success = false, .error = "no active window"};

    double vx = 0, vy = 0;
    try {
        vx = std::stod(args[0]);
        vy = std::stod(args[1]);
    } catch (...) { return {.success = false, .error = "invalid velocity"}; }

    g_physicsWorld.throwWindow(window, Vector2D{vx, vy} * g_config.throwMultiplier->value());
    return {};
}

static SDispatchResult dispatchToggle(std::string arg) {
    g_runtimeEnabled = !g_runtimeEnabled;
    if (!g_runtimeEnabled)
        g_physicsWorld.stopAll();
    return {};
}

static SDispatchResult dispatchStop(std::string arg) {
    g_physicsWorld.stopAll();
    return {};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprphysics] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprphysics] Version mismatch");
    }

    g_config.enabled          = makeShared<Config::Values::CBoolValue>("plugin:physics:enabled", "master on/off switch", true);
    g_config.collisions       = makeShared<Config::Values::CBoolValue>("plugin:physics:collisions", "let windows push each other around", true);
    g_config.affectTiled      = makeShared<Config::Values::CBoolValue>("plugin:physics:affect_tiled", "also simulate tiled windows (rarely wanted)", false);
    g_config.gravity          = makeShared<Config::Values::CFloatValue>("plugin:physics:gravity", "downward acceleration, px/s^2", 1400.F,
                                                                        Config::Values::SFloatValueOptions{.min = 0.F, .max = 20000.F});
    g_config.restitution      = makeShared<Config::Values::CFloatValue>("plugin:physics:restitution", "bounciness on impact, 0..1", 0.45F,
                                                                        Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
    g_config.friction         = makeShared<Config::Values::CFloatValue>("plugin:physics:friction", "velocity retained per second, 0..1", 0.86F,
                                                                        Config::Values::SFloatValueOptions{.min = 0.F, .max = 1.F});
    g_config.throwMultiplier  = makeShared<Config::Values::CFloatValue>("plugin:physics:throw_multiplier", "scales physics:throw's requested velocity", 1.F,
                                                                        Config::Values::SFloatValueOptions{.min = 0.F, .max = 100.F});
    g_config.maxVelocity      = makeShared<Config::Values::CFloatValue>("plugin:physics:max_velocity", "hard speed cap, px/s", 6000.F,
                                                                        Config::Values::SFloatValueOptions{.min = 1.F, .max = 100000.F});
    g_config.sleepVelocity    = makeShared<Config::Values::CFloatValue>("plugin:physics:sleep_velocity", "below this speed a resting window stops simulating, px/s", 12.F,
                                                                        Config::Values::SFloatValueOptions{.min = 0.F, .max = 1000.F});
    g_config.grabReleaseTicks = makeShared<Config::Values::CFloatValue>("plugin:physics:grab_release_ticks", "ticks of stillness after a drag before gravity resumes", 2.F,
                                                                        Config::Values::SFloatValueOptions{.min = 1.F, .max = 60.F});

    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.enabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.collisions);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.affectTiled);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.gravity);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.restitution);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.friction);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.throwMultiplier);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.maxVelocity);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.sleepVelocity);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.grabReleaseTicks);

    HyprlandAPI::addDispatcherV2(PHANDLE, "physics:throw", dispatchThrow);
    HyprlandAPI::addDispatcherV2(PHANDLE, "physics:toggle", dispatchToggle);
    HyprlandAPI::addDispatcherV2(PHANDLE, "physics:stop", dispatchStop);

    static auto P_TICK    = Event::bus()->m_events.tick.listen([&] {
        rescanWindows();
        g_physicsWorld.step();
    });
    static auto P_OPEN    = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { g_physicsWorld.syncBody(w); });
    static auto P_CLOSE   = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { g_physicsWorld.removeBody(w); });
    static auto P_DESTROY = Event::bus()->m_events.window.destroy.listen([&](PHLWINDOW w) { g_physicsWorld.removeBody(w); });

    rescanWindows();

    return {"hyprphysics", "Gravity, bouncing, and throwable windows for Hyprland", "Claude", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_physicsWorld.removeAll();
}
