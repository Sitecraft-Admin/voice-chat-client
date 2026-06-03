#pragma once
#include <d3d9.h>

class D3D9Hook {
public:
    static bool install();
    static void uninstall();
    static bool is_installed();

    // True when the game's swapchain is windowed (incl. borderless). False means
    // exclusive fullscreen, where a layered overlay window cannot be shown — the
    // external overlay then hands the UI back to the in-process renderer.
    static bool is_game_windowed();
};
