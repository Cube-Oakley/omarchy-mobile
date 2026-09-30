// Per-finger touch routing for Hyprland 0.56.
//
// Hyprland keeps one touched surface for all fingers: each new finger moves
// it, and the moves and lift of every finger go to wherever the last finger
// landed (and with that surface's origin). A palm across the shell's status
// bar and an app's window leaves the shell without the lift of its finger,
// and Qt then counts that finger as held and ignores every later tap.
//
// This plugin remembers where each finger went down: its surface, that
// surface's origin and its client's seat. A finger whose surface is no longer
// the touched one has its moves and lift sent to its own client, through the
// seat as if that client still had the touch focus. Hyprland's own handling
// is left alone for the finger on the touched surface, so one finger at a
// time behaves exactly as before.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace {
    struct SFinger {
        WP<CWLSurfaceResource> surface;
        WP<CWLSeatResource>    seat;
        Vector2D               origin;
        // Whether a client was sent this finger's down: not for a finger on no
        // surface, or one that started a workspace swipe.
        bool delivered = false;
    };

    HANDLE                               g_handle = nullptr;
    std::unordered_map<int32_t, SFinger> g_fingers;
    std::optional<int32_t>               g_pending;
    CHyprSignalListener                  g_down, g_up, g_motion;

    bool swiping() {
        return g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
    }

    // Hyprland handles a down after its bus event, so the finger is recorded
    // at the next touch event, before anything else can change the touch
    // focus.
    void resolvePending() {
        if (!g_pending)
            return;
        const auto& touch = g_pInputManager->m_touchData;
        SFinger     finger;
        finger.surface   = touch.touchFocusSurface;
        finger.origin    = touch.touchSurfaceOrigin;
        finger.seat      = g_pSeatManager->m_state.touchFocusResource;
        // A swipe in progress now either began with this finger or dropped it.
        finger.delivered = touch.touchFocusSurface && finger.seat && !swiping();
        g_fingers[*g_pending] = finger;
        g_pending.reset();
    }

    bool onTouchedSurface(const SFinger& finger) {
        const auto surface = finger.surface.lock();
        return surface && surface == g_pInputManager->m_touchData.touchFocusSurface.lock();
    }

    // Sends through the seat to this finger's client, as if it had the touch
    // focus, with a frame of its own; the seat's focus is then put back.
    template <typename F>
    void sendAs(const SFinger& finger, F&& send) {
        const auto saved                            = g_pSeatManager->m_state.touchFocusResource;
        g_pSeatManager->m_state.touchFocusResource = finger.seat;
        send();
        g_pSeatManager->sendTouchFrame();
        g_pSeatManager->m_state.touchFocusResource = saved;
    }

    void onDown(ITouch::SDownEvent event, Event::SCallbackInfo&) {
        resolvePending();
        g_pending = event.touchID;
    }

    void onUp(ITouch::SUpEvent event, Event::SCallbackInfo& info) {
        resolvePending();
        const auto it = g_fingers.find(event.touchID);
        if (it == g_fingers.end())
            return; // down before the plugin loaded
        const SFinger finger = it->second;
        g_fingers.erase(it);
        if (swiping())
            return;
        if (!finger.delivered) {
            // Hyprland would send this lift to whichever client has the focus,
            // and count it against that client's fingers.
            info.cancelled = true;
            return;
        }
        if (onTouchedSurface(finger))
            return;
        info.cancelled = true;
        if (finger.seat)
            sendAs(finger, [&] { g_pSeatManager->sendTouchUp(event.timeMs, event.touchID); });
    }

    void onMotion(ITouch::SMotionEvent event, Event::SCallbackInfo& info) {
        resolvePending();
        if (swiping() || (PROTO::data && PROTO::data->dndActive()))
            return;
        const auto it = g_fingers.find(event.touchID);
        if (it == g_fingers.end())
            return;
        const SFinger& finger = it->second;
        if (!finger.delivered) {
            info.cancelled = true;
            return;
        }
        if (onTouchedSurface(finger))
            return;
        info.cancelled = true;
        const auto monitor = Desktop::focusState()->monitor();
        if (!finger.seat || !monitor)
            return;
        const Vector2D local = monitor->m_position + event.pos * monitor->m_size - finger.origin;
        sendAs(finger, [&] { g_pSeatManager->sendTouchMotion(event.timeMs, event.touchID, local); });
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(g_handle, "[touch-fingers] Built for another Hyprland version; not loaded", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[touch-fingers] Version mismatch");
    }

    g_down   = Event::bus()->m_events.input.touch.down.listen(onDown);
    g_up     = Event::bus()->m_events.input.touch.up.listen(onUp);
    g_motion = Event::bus()->m_events.input.touch.motion.listen(onMotion);

    return {"touch-fingers", "Sends each finger's moves and lift to the surface it went down on", "omarchy-mobile", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_down.reset();
    g_up.reset();
    g_motion.reset();
    g_fingers.clear();
    g_pending.reset();
}
