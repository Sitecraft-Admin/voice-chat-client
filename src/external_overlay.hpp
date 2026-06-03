#pragma once

// External Discord-style overlay: a separate transparent, topmost, click-through
// layered window drawn OVER the game window. Because it never touches the game's
// D3D9 render pipeline, it cannot disrupt the game's frame pacing (no stutter).
//
// Milestone 1: window creation + game-window tracking + a GDI test box, to
// validate the window architecture (transparency, topmost, click-through,
// tracking) before wiring up the real D3D9/ImGui renderer.

namespace ExternalOverlay {
    // Starts the overlay window + tracking thread. Returns false on failure.
    bool start();
    // Tears everything down (safe to call multiple times).
    void stop();
    bool is_running();

    // True once the external overlay has a working D3D9 device and is actively
    // rendering the UI. While this is true the in-process D3D9-hook overlay must
    // stay silent (so the game's render pipeline is never touched). If external
    // init fails, this stays false and the in-process overlay remains the
    // fallback renderer.
    bool owns_ui();
}
