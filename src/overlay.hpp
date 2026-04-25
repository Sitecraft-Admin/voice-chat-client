#pragma once

#include <d3d9.h>

namespace Overlay {
    bool init(LPDIRECT3DDEVICE9 pDevice);
    void shutdown();
    void render(LPDIRECT3DDEVICE9 pDevice);
    void on_reset_before();
    void on_reset_after(LPDIRECT3DDEVICE9 pDevice);
}
