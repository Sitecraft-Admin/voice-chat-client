#pragma once

#include <d3d9.h>

namespace Overlay {
    bool init(LPDIRECT3DDEVICE9 pDevice);
    void shutdown();
    void render(LPDIRECT3DDEVICE9 pDevice);
    void on_reset_before();
    void on_reset_after(LPDIRECT3DDEVICE9 pDevice);

    // Frame-pacing stabilizer: number of nearly-invisible full-screen fill passes
    // drawn each frame to steady RO's frame timing (fixes map-scroll stutter).
    // 1 is enough on most machines; 0 disables. Configurable via overlay_pacing_fill.
    void set_pacing_fill(int passes);

    // ── External-overlay integration ─────────────────────────────────────────
    // Enable before the first render() so init() skips the Win32 input backend.
    void set_external_mode(bool on);
    // Call every frame BEFORE render() to supply input + display size.
    void feed_external_input(float w, float h, float mx, float my,
                             bool lmb, bool rmb, float dt);
    // True if ImGui wanted the mouse on the last rendered frame (hovering UI).
    bool external_wants_mouse();
    // Bounding box (framebuffer pixels) of everything drawn last frame. Returns
    // false if nothing was drawn. Lets the external overlay blit only this region.
    bool get_draw_bounds(int& x0, int& y0, int& x1, int& y1);
    // True if an ImGui text field is active (so the external hook swallows keys).
    bool external_wants_text_input();
    // True if the overlay has interactive/animating content this frame (hovering
    // UI, an open window, talking) — lets the external overlay idle slowly and
    // ramp to full rate only when needed.
    bool needs_high_fps();
    // True while the ImGui context is initialized (used during fullscreen handover).
    bool is_inited();
    // True if the active context was initialized in external-overlay mode (so the
    // game thread knows NOT to tear down the external overlay's ImGui).
    bool is_external_mode();

    // Keyboard / wheel input fed by the external overlay's low-level hooks.
    void feed_key_event(int win_vk, bool down);
    void feed_char_utf16(unsigned short c);
    void feed_mouse_wheel(float wheel_y);

    // Hotkeys (replicated from the old in-game wndproc hook).
    void toggle_badge();        // Scroll Lock — show/hide the compact badge
    void toggle_call_popup();   // Home — open/close "call by name"
}
