#define WLR_USE_UNSTABLE

#include "RenderHook.hpp"
#include "../globals.hpp"
#include "Config.hpp"
#include "PhysicsWorld.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>

using namespace Render;

namespace {
    CFunctionHook* g_pRenderWindowHook = nullptr;

    typedef void (*origRenderWindow_t)(void* thisptr, PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode,
                                        bool ignorePosition, bool standalone);

    // Brackets the original renderWindow call with a pair of render-hint pass
    // elements: hyprland's renderer defers actual GL drawing (it's a
    // damage-tracked, pass-based renderer — renderWindow just enqueues pass
    // elements), so the modifier has to be injected into the queue at the
    // right position rather than set/restored synchronously around the call.
    void hkRenderWindow(void* thisptr, PHLWINDOW pWindow, PHLMONITOR pMonitor, const Time::steady_tp& time, bool decorate, eRenderPassMode mode, bool ignorePosition,
                         bool standalone) {
        Vector2D subpixel;
        double   angle = 0.0;

        const bool haveTransform = g_config.enabled->value() && pWindow && g_physicsWorld.visualTransform(pWindow, subpixel, angle) &&
            (std::fabs(subpixel.x) > 0.001 || std::fabs(subpixel.y) > 0.001 || std::fabs(angle) > 0.0005);

        if (haveTransform) {
            CRendererHintsPassElement::SData hints;
            hints.renderModif = SRenderModifData{};
            if (std::fabs(subpixel.x) > 0.001 || std::fabs(subpixel.y) > 0.001)
                hints.renderModif->modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, subpixel);
            if (std::fabs(angle) > 0.0005)
                hints.renderModif->modifs.emplace_back(SRenderModifData::RMOD_TYPE_ROTATECENTER, (float)angle);
            g_pHyprRenderer->draw(hints);
        }

        ((origRenderWindow_t)g_pRenderWindowHook->m_original)(thisptr, pWindow, pMonitor, time, decorate, mode, ignorePosition, standalone);

        if (haveTransform) {
            // explicit empty modifier, not a saved/restored one: renderWindow
            // calls aren't nested, and hyprland's own per-element hints
            // already reset this to neutral before any window's turn to
            // draw, so neutral is always the correct thing to hand back
            CRendererHintsPassElement::SData reset;
            reset.renderModif = SRenderModifData{};
            g_pHyprRenderer->draw(reset);
        }
    }
}

void initRenderHook() {
    const auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWindow");
    for (const auto& m : matches) {
        if (m.demangled.find("IHyprRenderer::renderWindow") == std::string::npos)
            continue;
        g_pRenderWindowHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, reinterpret_cast<void*>(&hkRenderWindow));
        break;
    }

    if (!g_pRenderWindowHook || !g_pRenderWindowHook->hook()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprphysics] Couldn't hook renderWindow — subpixel/rotation visuals disabled, physics itself still works",
                                      CHyprColor{1.0, 0.7, 0.2, 1.0}, 5000);
        g_pRenderWindowHook = nullptr;
    }
}

void destroyRenderHook() {
    if (g_pRenderWindowHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderWindowHook);
    g_pRenderWindowHook = nullptr;
}
