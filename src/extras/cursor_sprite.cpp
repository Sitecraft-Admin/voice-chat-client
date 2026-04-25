#include "cursor_sprite.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

CursorSprite& CursorSprite::get() {
    static CursorSprite inst;
    return inst;
}

static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> data((size_t)sz);
    fread(data.data(), 1, (size_t)sz, f);
    fclose(f);
    return data;
}

static std::string exe_dir() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    auto p = s.rfind('\\');
    return (p != std::string::npos) ? s.substr(0, p) : ".";
}

// ─────────────────────────────────────────────────────────────────────────────
// SPR parser — returns RGBA pixels for sprite at index spr_idx
// ─────────────────────────────────────────────────────────────────────────────
static bool spr_decode(const std::vector<uint8_t>& data, int spr_idx,
                       std::vector<uint8_t>& out_rgba, int& out_w, int& out_h)
{
    if (data.size() < 6) return false;
    size_t pos = 0;
    auto r8  = [&]() -> uint8_t  { return (pos < data.size()) ? data[pos++] : 0; };
    auto r16 = [&]() -> uint16_t { uint16_t v; memcpy(&v, data.data()+pos, 2); pos+=2; return v; };

    if (data[0]!='S' || data[1]!='P') return false;
    pos = 2;
    uint16_t ver      = r16();
    uint16_t idx_cnt  = r16();
    uint16_t rgba_cnt = 0;
    if (ver >= 0x0201) rgba_cnt = r16();

    // Palette is the last 1024 bytes of the file
    uint8_t pal[1024] = {};
    if (data.size() >= 1024)
        memcpy(pal, data.data() + data.size() - 1024, 1024);

    bool  want_rgba = (spr_idx >= (int)idx_cnt);
    int   local     = want_rgba ? (spr_idx - idx_cnt) : spr_idx;

    // ── Indexed images ──────────────────────────────────────────────────────
    for (int i = 0; i < (int)idx_cnt; i++) {
        if (pos + 4 > data.size()) return false;
        uint16_t w = r16(), h = r16();
        int total = w * h;
        std::vector<uint8_t> indexed(total, 0);
        int p = 0;
        while (p < total && pos < data.size()) {
            uint8_t c = r8();
            if (c != 0) { indexed[p++] = c; }
            else {
                uint8_t n = r8();
                if (n == 0) { indexed[p++] = 0; }
                else { int end = (p+n < total) ? p+n : total; while (p < end) indexed[p++] = 0; }
            }
        }
        if (!want_rgba && i == local) {
            out_w = w; out_h = h;
            out_rgba.resize(w * h * 4);
            for (int px = 0; px < w*h; px++) {
                uint8_t idx = indexed[px];
                out_rgba[px*4+0] = pal[idx*4+2]; // R
                out_rgba[px*4+1] = pal[idx*4+1]; // G
                out_rgba[px*4+2] = pal[idx*4+0]; // B
                out_rgba[px*4+3] = (idx == 0) ? 0 : 255;
            }
            return true;
        }
    }

    // ── RGBA images ─────────────────────────────────────────────────────────
    if (ver >= 0x0201) {
        for (int i = 0; i < (int)rgba_cnt; i++) {
            if (pos + 4 > data.size()) return false;
            uint16_t w = r16(), h = r16();
            int bytes = w * h * 4;
            if (i == local && want_rgba) {
                out_w = w; out_h = h;
                out_rgba.resize(bytes);
                for (int px = 0; px < w*h; px++) {
                    out_rgba[px*4+0] = data[pos + px*4+2];
                    out_rgba[px*4+1] = data[pos + px*4+1];
                    out_rgba[px*4+2] = data[pos + px*4+0];
                    out_rgba[px*4+3] = data[pos + px*4+3];
                }
                return true;
            }
            pos += bytes;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ACT parser — action[act_idx], frame 0, layer 0
// ─────────────────────────────────────────────────────────────────────────────
struct ActLayer { int spr_num = 0; int cx = 0; int cy = 0; };

static bool act_read_layer(const std::vector<uint8_t>& data, int act_idx, ActLayer& out)
{
    if (data.size() < 8 || data[0]!='A' || data[1]!='C') return false;
    size_t pos = 2;
    auto r8  = [&]() -> uint8_t  { return (pos < data.size()) ? data[pos++] : 0; };
    auto r32 = [&]() -> int32_t  { int32_t  v; memcpy(&v,data.data()+pos,4); pos+=4; return v; };
    auto ru32= [&]() -> uint32_t { uint32_t v; memcpy(&v,data.data()+pos,4); pos+=4; return v; };
    auto rf  = [&]() -> float    { float    v; memcpy(&v,data.data()+pos,4); pos+=4; return v; };
    auto r16 = [&]() -> uint16_t { uint16_t v; memcpy(&v,data.data()+pos,2); pos+=2; return v; };

    uint16_t ver     = r16();
    uint32_t act_cnt = ru32();
    for (int i = 0; i < 10; i++) r8();
    if ((uint32_t)act_idx >= act_cnt) return false;

    bool found = false;
    for (uint32_t ai = 0; ai < act_cnt && pos < data.size(); ai++) {
        uint32_t frame_cnt = ru32();
        for (uint32_t fi = 0; fi < frame_cnt && pos < data.size(); fi++) {
            uint32_t layer_cnt = ru32();
            for (uint32_t li = 0; li < layer_cnt && pos < data.size(); li++) {
                int32_t  x       = r32();
                int32_t  y       = r32();
                int32_t  spr_num = r32();
                ru32(); // mirror
                if (ver >= 0x0200) { ru32(); rf(); rf(); r32(); ru32(); }
                if (ver >= 0x0204) { r32(); r32(); }
                if ((int)ai == act_idx && fi == 0 && li == 0 && !found) {
                    out.spr_num = spr_num;
                    out.cx = (int)x;
                    out.cy = (int)y;
                    found = true;
                }
            }
            if (ver >= 0x0200) r32();
            if (ver >= 0x0203) { rf(); rf(); }
        }
        if ((int)ai == act_idx) break;
    }
    return found;
}

// ─────────────────────────────────────────────────────────────────────────────
// load — parse SPR/ACT and call SetCursorProperties on the D3D9 device
// D3D hardware cursor renders above ALL D3D rendering including ImGui overlay
// ─────────────────────────────────────────────────────────────────────────────
bool CursorSprite::load(IDirect3DDevice9* dev)
{
    if (ready_)      return true;
    if (load_tried_) return false;
    load_tried_ = true;
    dev_ = dev;

    std::string base     = exe_dir();
    std::string spr_path = base + "\\data\\sprite\\cursors.spr";
    std::string act_path = base + "\\data\\sprite\\cursors.act";

    auto spr_data = read_file(spr_path.c_str());
    if (spr_data.empty()) return false;
    auto act_data = read_file(act_path.c_str());

    int spr_idx = 0;
    int cx = 0, cy = 0;
    ActLayer layer;
    if (!act_data.empty() && act_read_layer(act_data, 0, layer) && layer.spr_num >= 0) {
        spr_idx = layer.spr_num;
        cx = layer.cx; cy = layer.cy;
    }

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!spr_decode(spr_data, spr_idx, rgba, w, h)) return false;

    // D3D hardware cursor requires exactly 32×32 A8R8G8B8 surface
    const int CS = 32;

    // hotspot: ACT cx,cy = offset from attachment point to sprite center
    // hot = sprite center - cx/cy offset
    int hot_x = w/2 - cx;
    int hot_y = h/2 - cy;
    // clamp to [0, CS-1]
    if (hot_x < 0) hot_x = 0; if (hot_x >= CS) hot_x = 0;
    if (hot_y < 0) hot_y = 0; if (hot_y >= CS) hot_y = 0;

    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = dev->CreateOffscreenPlainSurface(CS, CS, D3DFMT_A8R8G8B8,
                                                   D3DPOOL_SCRATCH, &surf, nullptr);
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT lr;
    if (FAILED(surf->LockRect(&lr, nullptr, 0))) { surf->Release(); return false; }

    // Clear to transparent
    memset(lr.pBits, 0, (size_t)CS * lr.Pitch);

    // Copy sprite pixels (clamped to CS×CS)
    int copy_w = (w < CS) ? w : CS;
    int copy_h = (h < CS) ? h : CS;
    for (int row = 0; row < copy_h; row++) {
        auto* dst = (uint8_t*)lr.pBits + row * lr.Pitch;
        auto* src = rgba.data() + row * w * 4;
        for (int col = 0; col < copy_w; col++) {
            dst[col*4+0] = src[col*4+2]; // B
            dst[col*4+1] = src[col*4+1]; // G
            dst[col*4+2] = src[col*4+0]; // R
            dst[col*4+3] = src[col*4+3]; // A
        }
    }
    surf->UnlockRect();

    hr = dev->SetCursorProperties(hot_x, hot_y, surf);
    surf->Release();

    ready_ = SUCCEEDED(hr);
    return ready_;
}

// ─────────────────────────────────────────────────────────────────────────────
// show / hide — wraps IDirect3DDevice9::ShowCursor
// ─────────────────────────────────────────────────────────────────────────────
void CursorSprite::show(bool visible)
{
    if (dev_ && ready_) dev_->ShowCursor(visible ? TRUE : FALSE);
}
