#pragma once
#include <d3d9.h>

// Uses IDirect3DDevice9::SetCursorProperties to install the RO cursor sprite
// as a hardware cursor — it renders above ALL D3D content including ImGui.

class CursorSprite {
public:
    static CursorSprite& get();

    // Call once when the D3D9 device is first available.
    bool load(IDirect3DDevice9* dev);

    // Show or hide the D3D hardware cursor.
    void show(bool visible);

    bool is_ready() const { return ready_; }

private:
    IDirect3DDevice9* dev_        = nullptr;
    bool              ready_      = false;
    bool              load_tried_ = false;
};
