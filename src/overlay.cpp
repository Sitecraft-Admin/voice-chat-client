#include "overlay.hpp"
#include "voice_client.hpp"
#include "audio.hpp"
#include "dbglog.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "ui_background_blob.hpp"
#include "ui_background_call_blob.hpp"
#include "ui_badge_icon_blobs.hpp"
#include "ui_badge_state_blobs.hpp"
#include "ui_close_btn_blob.hpp"
#include "ui_btn_blob.hpp"
#include "ui_call_btn_blobs.hpp"
#include "ui_whisper_call_blobs.hpp"
#include "ui_dropdown_btn_blobs.hpp"
#include "ui_settings_btn_blobs.hpp"
#include "ui_slider_blobs.hpp"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#pragma comment(lib, "gdiplus.lib")

namespace {

using namespace Gdiplus;

static bool    g_imgui_inited  = false;
static bool    g_visible       = true;
static bool    g_badge_visible = true;  // Scroll Lock toggles the compact badge
static bool    g_settings_open = false;
static HWND    g_hwnd          = nullptr;
static WNDPROC g_old_wndproc   = nullptr;
static LPDIRECT3DDEVICE9 g_ui_device = nullptr;
static ImVec2  g_badge_hit_min = ImVec2(0.f, 0.f);
static ImVec2  g_badge_hit_max = ImVec2(0.f, 0.f);
static ImVec2  g_channel_hit_min = ImVec2(0.f, 0.f);
static ImVec2  g_channel_hit_max = ImVec2(0.f, 0.f);

// Settings state (cached so we don't re-enumerate every frame)
static std::vector<AudioDeviceInfo> g_mic_devs;
static std::vector<AudioDeviceInfo> g_spk_devs;
static int g_sel_mic = 0;
static int g_sel_spk = 0;
static bool g_devs_loaded   = false;
static bool g_binding_key   = false; // waiting for key press
static bool g_whisper_popup = false; // true while incoming-call popup is visible
static bool g_call_popup    = false; // true while "call by name" input popup is open
static char g_call_name[64] = {};    // name input buffer
static bool g_lang_thai     = true;  // language toggle: true=Thai, false=English
static ULONG_PTR g_gdiplus_token = 0;
static int g_settings_tab   = 0;     // 0=voice, 1=players, 2=devices
static char g_bg_status[128]  = "BG idle";
static char g_btn_status[128] = "BTN idle";

struct UiTexture {
    LPDIRECT3DTEXTURE9 tex = nullptr;
    int width = 0;
    int height = 0;
    int tex_width = 0;
    int tex_height = 0;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

static UiTexture g_bg_tex;
static UiTexture g_call_bg_tex;
static UiTexture g_btn_tex;
static UiTexture g_btn_hover_tex;
static UiTexture g_btn_lang_tex;
static UiTexture g_btn_lang_hover_tex;
static UiTexture g_btn_exit_tex;
static UiTexture g_btn_exit_hover_tex;
static UiTexture g_btn_ptt_key_tex;
static UiTexture g_btn_ptt_key_hover_tex;
static UiTexture g_btn_call_tex;
static UiTexture g_btn_call_hover_tex;
static UiTexture g_whisper_call_bg_tex;
static UiTexture g_btn_whisper_call_tex;
static UiTexture g_btn_whisper_call_hover_tex;
static UiTexture g_btn_dropdown_tex;
static UiTexture g_close_tex;
static UiTexture g_mic_off_tex;
static UiTexture g_spk_off_tex;
static UiTexture g_mic_on_tex;
static UiTexture g_spk_on_tex;
static UiTexture g_badge_idle_tex;
static UiTexture g_badge_talk_tex;
static UiTexture g_badge_mute_tex;
static UiTexture g_badge_spk_off_tex;
static UiTexture g_badge_mic_off_tex;
static UiTexture g_badge_connecting_tex;
static UiTexture g_slider_left_tex;
static UiTexture g_slider_right_tex;
static UiTexture g_slider_knob_tex;

static bool point_in_rect(ImVec2 p, ImVec2 min, ImVec2 max) {
    return p.x >= min.x && p.y >= min.y && p.x <= max.x && p.y <= max.y;
}

static bool mouse_over_compact_overlay() {
    if (!g_visible || !g_badge_visible) return false;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p = io.MousePos;
    return point_in_rect(p, g_badge_hit_min, g_badge_hit_max) ||
           point_in_rect(p, g_channel_hit_min, g_channel_hit_max);
}

static void set_point_sampler_callback(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ui_device) return;
    g_ui_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_ui_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
}

static void set_linear_sampler_callback(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ui_device) return;
    g_ui_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_ui_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
}

// Pick Thai or English string at runtime
static const char* L(const char* th, const char* en) {
    return g_lang_thai ? th : en;
}

static void release_ui_texture(UiTexture& t) {
    if (t.tex) {
        t.tex->Release();
        t.tex = nullptr;
    }
    t.width = 0;
    t.height = 0;
    t.tex_width = 0;
    t.tex_height = 0;
    t.u1 = 1.0f;
    t.v1 = 1.0f;
}

static UINT next_pow2(UINT v) {
    UINT p = 1;
    while (p < v) p <<= 1;
    return p;
}

static const char* fmt_name(D3DFORMAT fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    default: return "UNKNOWN";
    }
}

static const char* pool_name(D3DPOOL pool) {
    switch (pool) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH: return "SCRATCH";
    default: return "UNKNOWN";
    }
}

static bool load_png_texture_from_memory(LPDIRECT3DDEVICE9 device, const unsigned char* bytes, size_t size, UiTexture& out, char* status_buf, size_t status_cap) {
    release_ui_texture(out);
    if (!device || !bytes || size == 0) {
        sprintf_s(status_buf, status_cap, "bad input");
        return false;
    }

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) {
        sprintf_s(status_buf, status_cap, "GlobalAlloc failed");
        dbglog("[overlay] GlobalAlloc failed for image blob");
        return false;
    }

    void* mem_ptr = GlobalLock(mem);
    if (!mem_ptr) {
        GlobalFree(mem);
        sprintf_s(status_buf, status_cap, "GlobalLock failed");
        dbglog("[overlay] GlobalLock failed for image blob");
        return false;
    }
    memcpy(mem_ptr, bytes, size);
    GlobalUnlock(mem);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream)) || !stream) {
        GlobalFree(mem);
        sprintf_s(status_buf, status_cap, "CreateStream failed");
        dbglog("[overlay] CreateStreamOnHGlobal failed for image blob");
        return false;
    }

    Bitmap bmp(stream);
    if (bmp.GetLastStatus() != Ok) {
        char msg[96];
        sprintf_s(msg, "[overlay] GDI+ Bitmap(stream) failed status=%d", (int)bmp.GetLastStatus());
        sprintf_s(status_buf, status_cap, "Bitmap status=%d", (int)bmp.GetLastStatus());
        dbglog(msg);
        stream->Release();
        return false;
    }

    const UINT w = bmp.GetWidth();
    const UINT h = bmp.GetHeight();
    if (w == 0 || h == 0) {
        sprintf_s(status_buf, status_cap, "decoded zero size");
        dbglog("[overlay] image decoded with zero size");
        stream->Release();
        return false;
    }

    D3DCAPS9 caps{};
    device->GetDeviceCaps(&caps);
    UINT tex_w = w;
    UINT tex_h = h;
    if (caps.TextureCaps & D3DPTEXTURECAPS_POW2) {
        tex_w = next_pow2(w);
        tex_h = next_pow2(h);
    }

    const D3DFORMAT candidates[] = {
        D3DFMT_A8R8G8B8,
        D3DFMT_A4R4G4B4,
        D3DFMT_A1R5G5B5,
        D3DFMT_X8R8G8B8,
    };
    struct TextureAttempt {
        D3DPOOL pool;
        DWORD usage;
    };
    const TextureAttempt attempts[] = {
        { D3DPOOL_MANAGED, 0 },
        { D3DPOOL_DEFAULT, 0 },
        { D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC },
    };

    Rect rect(0, 0, (INT)w, (INT)h);
    BitmapData data{};
    if (bmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
        sprintf_s(status_buf, status_cap, "LockBits failed");
        dbglog("[overlay] Bitmap::LockBits failed for image blob");
        stream->Release();
        return false;
    }

    D3DFORMAT chosen_fmt = D3DFMT_UNKNOWN;
    D3DPOOL chosen_pool = D3DPOOL_MANAGED;
    DWORD chosen_usage = 0;
    LPDIRECT3DTEXTURE9 tex = nullptr;
    D3DLOCKED_RECT locked{};
    HRESULT last_create_hr = E_FAIL;
    HRESULT last_lock_hr = E_FAIL;

    for (const auto& attempt : attempts) {
        for (D3DFORMAT fmt : candidates) {
            LPDIRECT3DTEXTURE9 candidate_tex = nullptr;
            last_create_hr = device->CreateTexture(tex_w, tex_h, 1, attempt.usage, fmt, attempt.pool, &candidate_tex, nullptr);
            if (FAILED(last_create_hr) || !candidate_tex) {
                continue;
            }

            const DWORD lock_flags = (attempt.usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_DISCARD : 0;
            last_lock_hr = candidate_tex->LockRect(0, &locked, nullptr, lock_flags);
            if (SUCCEEDED(last_lock_hr)) {
                tex = candidate_tex;
                chosen_fmt = fmt;
                chosen_pool = attempt.pool;
                chosen_usage = attempt.usage;
                break;
            }

            candidate_tex->Release();
        }
        if (tex) break;
    }

    if (!tex) {
        bmp.UnlockBits(&data);
        if (FAILED(last_create_hr)) {
            sprintf_s(status_buf, status_cap, "CreateTexture hr=%08lX", (unsigned long)last_create_hr);
            dbglog("[overlay] CreateTexture failed for image blob");
        } else {
            sprintf_s(status_buf, status_cap, "LockRect hr=%08lX", (unsigned long)last_lock_hr);
            dbglog("[overlay] texture LockRect failed for image blob");
        }
        stream->Release();
        return false;
    }

    const UINT bytes_per_pixel =
        (chosen_fmt == D3DFMT_A8R8G8B8 || chosen_fmt == D3DFMT_X8R8G8B8) ? 4u : 2u;
    for (UINT y = 0; y < tex_h; ++y) {
        auto* dst = reinterpret_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
        memset(dst, 0, tex_w * bytes_per_pixel);
    }
    for (UINT y = 0; y < h; ++y) {
        const auto* src = reinterpret_cast<const uint8_t*>(data.Scan0) + y * data.Stride;
        auto* dst = reinterpret_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
        for (UINT x = 0; x < w; ++x) {
            const uint8_t b = src[x * 4 + 0];
            const uint8_t g = src[x * 4 + 1];
            const uint8_t r = src[x * 4 + 2];
            const uint8_t a = src[x * 4 + 3];
            if (chosen_fmt == D3DFMT_A8R8G8B8 || chosen_fmt == D3DFMT_X8R8G8B8) {
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = (chosen_fmt == D3DFMT_X8R8G8B8) ? 0xFF : a;
            } else {
                auto* dst16 = reinterpret_cast<uint16_t*>(dst);
                if (chosen_fmt == D3DFMT_A4R4G4B4) {
                    dst16[x] = (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                } else {
                    dst16[x] = (uint16_t)(((a >= 128 ? 1 : 0) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                }
            }
        }
    }

    tex->UnlockRect(0);
    bmp.UnlockBits(&data);
    stream->Release();

    out.tex = tex;
    out.width = (int)w;
    out.height = (int)h;
    out.tex_width = (int)tex_w;
    out.tex_height = (int)tex_h;
    out.u1 = (float)w / (float)tex_w;
    out.v1 = (float)h / (float)tex_h;
    sprintf_s(status_buf, status_cap, "OK %ux%u %s %s%s",
        w, h, fmt_name(chosen_fmt), pool_name(chosen_pool),
        chosen_usage == D3DUSAGE_DYNAMIC ? " DYN" : "");
    return true;
}

static void ensure_ui_assets(LPDIRECT3DDEVICE9 device) {
    if (!g_bg_tex.tex) {
        load_png_texture_from_memory(device, kUiBackgroundBmp, kUiBackgroundBmpSize, g_bg_tex, g_bg_status, sizeof(g_bg_status));
    }
    if (!g_call_bg_tex.tex) {
        load_png_texture_from_memory(device, kUiBackgroundCallPng, kUiBackgroundCallPngSize, g_call_bg_tex, g_bg_status, sizeof(g_bg_status));
    }
    if (!g_btn_tex.tex) load_png_texture_from_memory(device, kBtnSettingsPng, kBtnSettingsPngSize, g_btn_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_hover_tex.tex) load_png_texture_from_memory(device, kBtnSettingsHoverPng, kBtnSettingsHoverPngSize, g_btn_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_lang_tex.tex) load_png_texture_from_memory(device, kBtnLangPng, kBtnLangPngSize, g_btn_lang_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_lang_hover_tex.tex) load_png_texture_from_memory(device, kBtnLangHoverPng, kBtnLangHoverPngSize, g_btn_lang_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_exit_tex.tex) load_png_texture_from_memory(device, kBtnExitPng, kBtnExitPngSize, g_btn_exit_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_exit_hover_tex.tex) load_png_texture_from_memory(device, kBtnExitHoverPng, kBtnExitHoverPngSize, g_btn_exit_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_ptt_key_tex.tex) load_png_texture_from_memory(device, kBtnPttKeyPng, kBtnPttKeyPngSize, g_btn_ptt_key_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_ptt_key_hover_tex.tex) load_png_texture_from_memory(device, kBtnPttKeyHoverPng, kBtnPttKeyHoverPngSize, g_btn_ptt_key_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_call_tex.tex) load_png_texture_from_memory(device, kBtnCallPng, kBtnCallPngSize, g_btn_call_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_call_hover_tex.tex) load_png_texture_from_memory(device, kBtnCallHoverPng, kBtnCallHoverPngSize, g_btn_call_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_whisper_call_bg_tex.tex) load_png_texture_from_memory(device, kUiBackgroundCallWPng, kUiBackgroundCallWPngSize, g_whisper_call_bg_tex, g_bg_status, sizeof(g_bg_status));
    if (!g_btn_whisper_call_tex.tex) load_png_texture_from_memory(device, kBtnCallWPng, kBtnCallWPngSize, g_btn_whisper_call_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_whisper_call_hover_tex.tex) load_png_texture_from_memory(device, kBtnCallWHoverPng, kBtnCallWHoverPngSize, g_btn_whisper_call_hover_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_btn_dropdown_tex.tex) load_png_texture_from_memory(device, kBtnDropdownPng, kBtnDropdownPngSize, g_btn_dropdown_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_close_tex.tex) {
        load_png_texture_from_memory(device, kBtnClose1Png, kBtnClose1PngSize, g_close_tex, g_btn_status, sizeof(g_btn_status));
    }
    if (!g_mic_off_tex.tex) load_png_texture_from_memory(device, kMicOffPng, kMicOffPngSize, g_mic_off_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_spk_off_tex.tex) load_png_texture_from_memory(device, kSpkOffPng, kSpkOffPngSize, g_spk_off_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_mic_on_tex.tex)  load_png_texture_from_memory(device, kMicOnPng,  kMicOnPngSize,  g_mic_on_tex,  g_btn_status, sizeof(g_btn_status));
    if (!g_spk_on_tex.tex)  load_png_texture_from_memory(device, kSpkOnPng,  kSpkOnPngSize,  g_spk_on_tex,  g_btn_status, sizeof(g_btn_status));
    if (!g_badge_idle_tex.tex) load_png_texture_from_memory(device, kBadgeIdlePng, kBadgeIdlePngSize, g_badge_idle_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_badge_talk_tex.tex) load_png_texture_from_memory(device, kBadgeTalkPng, kBadgeTalkPngSize, g_badge_talk_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_badge_mute_tex.tex) load_png_texture_from_memory(device, kBadgeMutePng, kBadgeMutePngSize, g_badge_mute_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_badge_spk_off_tex.tex) load_png_texture_from_memory(device, kBadgeSpeakerOffPng, kBadgeSpeakerOffPngSize, g_badge_spk_off_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_badge_mic_off_tex.tex) load_png_texture_from_memory(device, kBadgeMicOffPng, kBadgeMicOffPngSize, g_badge_mic_off_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_badge_connecting_tex.tex) load_png_texture_from_memory(device, kBadgeConnectingPng, kBadgeConnectingPngSize, g_badge_connecting_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_slider_left_tex.tex) load_png_texture_from_memory(device, kSliderLeftPng, kSliderLeftPngSize, g_slider_left_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_slider_right_tex.tex) load_png_texture_from_memory(device, kSliderRightPng, kSliderRightPngSize, g_slider_right_tex, g_btn_status, sizeof(g_btn_status));
    if (!g_slider_knob_tex.tex) load_png_texture_from_memory(device, kSliderKnobPng, kSliderKnobPngSize, g_slider_knob_tex, g_btn_status, sizeof(g_btn_status));
}

static bool textured_label_button(const char* id, const char* label, ImVec2 size,
                                UiTexture* normal_tex, UiTexture* active_tex,
                                bool active = false, float text_y_offset = -1.f,
                                ImU32 text_col = IM_COL32(25, 27, 34, 255)) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##img", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    UiTexture* tex = (active || hovered || held) ? active_tex : normal_tex;
    if (!tex || !tex->tex) tex = normal_tex;

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex && tex->tex) {
        dl->AddImage((ImTextureID)tex->tex, p0, p1,
                     ImVec2(0.f, 0.f), ImVec2(tex->u1, tex->v1), IM_COL32_WHITE);
    } else {
        dl->AddRectFilled(p0, p1, active ? IM_COL32(194, 214, 247, 255) : IM_COL32(245, 245, 245, 255), 4.f);
        dl->AddRect(p0, p1, IM_COL32(156, 156, 156, 255), 4.f);
    }

    ImVec2 text_sz = ImGui::CalcTextSize(label);
    ImVec2 text_pos(
        floorf(p0.x + (size.x - text_sz.x) * 0.5f + 0.5f),
        floorf(p0.y + (size.y - text_sz.y) * 0.5f + text_y_offset + (held ? 1.f : 0.f) + 0.5f));
    dl->AddText(text_pos, text_col, label);

    ImGui::PopID();
    return clicked;
}

static bool image_button_with_label(const char* id, const char* label, ImVec2 size, bool active = false) {
    return textured_label_button(id, label, size, &g_btn_tex, &g_btn_hover_tex, active);
}

static bool ptt_key_button(const char* id, const char* label, ImVec2 size, bool active = false) {
    return textured_label_button(id, label, size, &g_btn_ptt_key_tex, &g_btn_ptt_key_hover_tex, active);
}

static bool call_button(const char* id, const char* label, ImVec2 size, bool active = false) {
    return textured_label_button(id, label, size, &g_btn_call_tex, &g_btn_call_hover_tex, active, -1.f, IM_COL32(18, 18, 18, 255));
}

static bool whisper_call_button(const char* id, const char* label, ImVec2 size) {
    return textured_label_button(id, label, size, &g_btn_whisper_call_tex, &g_btn_whisper_call_hover_tex, false, -1.f, IM_COL32(22, 22, 22, 255));
}

static bool begin_device_combo(const char* id, const char* preview, float width) {
    ImGui::PushID(id);
    const ImVec2 size(width, 18.f);
    ImGui::InvisibleButton("##combo", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float arrow_w = 18.f;
    const ImVec2 a0(p1.x - arrow_w, p0.y);
    const ImVec2 a1(p1.x, p1.y);

    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    dl->AddRect(p0, p1, IM_COL32(197, 197, 197, 255));
    dl->AddLine(ImVec2(a0.x, p0.y + 1.f), ImVec2(a0.x, p1.y - 1.f), IM_COL32(197, 197, 197, 255), 1.f);
    dl->AddRectFilled(a0, a1, IM_COL32(255, 255, 255, 255));

    const ImVec2 text_min(p0.x + 4.f, p0.y);
    const ImVec2 text_max(a0.x - 4.f, p1.y);
    dl->PushClipRect(text_min, text_max, true);
    const ImVec2 text_sz = ImGui::CalcTextSize(preview);
    dl->AddText(
        ImVec2(text_min.x, floorf(p0.y + (size.y - text_sz.y) * 0.5f + 0.5f)),
        IM_COL32(20, 20, 20, 255),
        preview);
    dl->PopClipRect();

    if (g_btn_dropdown_tex.tex) {
        const ImVec2 img_p0(
            a0.x,
            floorf(p0.y + (size.y - (float)g_btn_dropdown_tex.height) * 0.5f + 0.5f));
        dl->AddCallback(set_point_sampler_callback, nullptr);
        dl->AddImage((ImTextureID)g_btn_dropdown_tex.tex,
            img_p0,
            ImVec2(img_p0.x + (float)g_btn_dropdown_tex.width, img_p0.y + (float)g_btn_dropdown_tex.height),
            ImVec2(0.f, 0.f), ImVec2(g_btn_dropdown_tex.u1, g_btn_dropdown_tex.v1), IM_COL32_WHITE);
        dl->AddCallback(set_linear_sampler_callback, nullptr);
    }

    if (clicked) {
        ImGui::OpenPopup("##popup");
    }

    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y - 1.f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(197, 197, 197, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(63, 112, 218, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(63, 112, 218, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(63, 112, 218, 255));
    const bool opened = ImGui::BeginPopup("##popup", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (!opened) {
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        ImGui::PopID();
    }
    return opened;
}

static void end_device_combo() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

static bool tab_button(const char* id, const char* label, ImVec2 size, bool active) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##tab", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 tp0 = p0;
    const ImVec2 tp1 = p1;
    const float rounding = 4.f;
    const ImU32 border_col = IM_COL32(88, 88, 88, 255);
    const ImU32 fill_col = active
        ? IM_COL32(95, 136, 229, 255)
        : ((hovered || held) ? IM_COL32(246, 248, 252, 255) : IM_COL32(255, 255, 255, 255));
    const ImU32 text_col = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(18, 18, 18, 255);

    dl->AddRectFilled(tp0, tp1, fill_col, rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRect(tp0, tp1, border_col, rounding, ImDrawFlags_RoundCornersTop, 1.f);
    if (active) {
        dl->AddLine(ImVec2(tp0.x + 1.f, tp1.y - 1.f), ImVec2(tp1.x - 2.f, tp1.y - 1.f), fill_col, 1.f);
    }

    ImVec2 text_sz = ImGui::CalcTextSize(label);
    const ImVec2 text_pos(
        floorf(tp0.x + (size.x - text_sz.x) * 0.5f + 0.5f),
        floorf(tp0.y + (size.y - text_sz.y) * 0.5f - 2.f + (held ? 1.f : 0.f) + 0.5f));
    dl->AddText(text_pos, text_col, label);

    ImGui::PopID();
    return clicked;
}

static bool lang_button(const char* id, const char* label, ImVec2 size, bool active) {
    return textured_label_button(id, label, size, &g_btn_lang_tex, &g_btn_lang_hover_tex, active, -1.f);
}

static bool exit_button(const char* id, const char* label, ImVec2 size, bool active = false) {
    return textured_label_button(id, label, size, &g_btn_exit_tex, &g_btn_exit_hover_tex, active);
}

static bool pretty_volume_slider(const char* id, float* value, float min_v, float max_v, ImVec2 size) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##slider", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    bool changed = false;

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float left_w = g_slider_left_tex.tex ? (float)g_slider_left_tex.width : 8.f;
    const float left_h = g_slider_left_tex.tex ? (float)g_slider_left_tex.height : 12.f;
    const float right_w = g_slider_right_tex.tex ? (float)g_slider_right_tex.width : 8.f;
    const float right_h = g_slider_right_tex.tex ? (float)g_slider_right_tex.height : 12.f;
    const float knob_w = g_slider_knob_tex.tex ? (float)g_slider_knob_tex.width : 8.f;
    const float knob_h = g_slider_knob_tex.tex ? (float)g_slider_knob_tex.height : 8.f;
    const float center_y = floorf(p0.y + size.y * 0.5f + 0.5f);
    const float track_x0 = p0.x + left_w + knob_w * 0.5f + 1.f;
    const float track_x1 = p1.x - right_w - knob_w * 0.5f - 1.f;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool left_hit = hovered && mouse.x <= p0.x + left_w + 3.f;
    const bool right_hit = hovered && mouse.x >= p1.x - right_w - 3.f;
    const bool track_hit = hovered && mouse.x >= track_x0 && mouse.x <= track_x1;
    const float step = (max_v - min_v) * 0.05f;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float next = *value;
        if (left_hit) {
            next = *value - step;
        } else if (right_hit) {
            next = *value + step;
        } else if (track_hit) {
            const float t = (mouse.x - track_x0) / (track_x1 - track_x0);
            const float clamped = (t < 0.f) ? 0.f : (t > 1.f ? 1.f : t);
            next = min_v + (max_v - min_v) * clamped;
        }
        if (next < min_v) next = min_v;
        if (next > max_v) next = max_v;
        if (next != *value) {
            *value = next;
            changed = true;
        }
    } else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !left_hit && !right_hit) {
        const float t = (mouse.x - track_x0) / (track_x1 - track_x0);
        const float clamped = (t < 0.f) ? 0.f : (t > 1.f ? 1.f : t);
        const float next = min_v + (max_v - min_v) * clamped;
        if (next != *value) {
            *value = next;
            changed = true;
        }
    }

    const float ratio_raw = (*value - min_v) / (max_v - min_v);
    const float ratio = ratio_raw < 0.f ? 0.f : (ratio_raw > 1.f ? 1.f : ratio_raw);
    const float knob_cx = floorf(track_x0 + (track_x1 - track_x0) * ratio + 0.5f);

    dl->AddCallback(set_point_sampler_callback, nullptr);
    if (g_slider_left_tex.tex) {
        const ImVec2 a(p0.x, floorf(center_y - left_h * 0.5f + 0.5f));
        dl->AddImage((ImTextureID)g_slider_left_tex.tex, a, ImVec2(a.x + left_w, a.y + left_h),
                     ImVec2(0.f, 0.f), ImVec2(g_slider_left_tex.u1, g_slider_left_tex.v1), IM_COL32_WHITE);
    }
    if (g_slider_right_tex.tex) {
        const ImVec2 a(p1.x - right_w, floorf(center_y - right_h * 0.5f + 0.5f));
        dl->AddImage((ImTextureID)g_slider_right_tex.tex, a, ImVec2(a.x + right_w, a.y + right_h),
                     ImVec2(0.f, 0.f), ImVec2(g_slider_right_tex.u1, g_slider_right_tex.v1), IM_COL32_WHITE);
    }

    const ImU32 track_shadow = IM_COL32(145, 151, 151, 255);
    const ImU32 track_light = IM_COL32(236, 244, 241, 255);
    dl->AddRectFilled(ImVec2(track_x0, center_y - 3.f), ImVec2(track_x1, center_y + 2.f), IM_COL32(212, 221, 218, 255));
    dl->AddLine(ImVec2(track_x0, center_y - 3.f), ImVec2(track_x1, center_y - 3.f), track_shadow, 1.f);
    dl->AddLine(ImVec2(track_x0, center_y + 2.f), ImVec2(track_x1, center_y + 2.f), track_light, 1.f);

    if (g_slider_knob_tex.tex) {
        const ImVec2 k0(floorf(knob_cx - knob_w * 0.5f + 0.5f), floorf(center_y - knob_h * 0.5f + 0.5f));
        dl->AddImage((ImTextureID)g_slider_knob_tex.tex, k0, ImVec2(k0.x + knob_w, k0.y + knob_h),
                     ImVec2(0.f, 0.f), ImVec2(g_slider_knob_tex.u1, g_slider_knob_tex.v1), IM_COL32_WHITE);
    } else {
        dl->AddCircleFilled(ImVec2(knob_cx, center_y), 4.f, IM_COL32(130, 171, 240, 255));
    }
    dl->AddCallback(set_linear_sampler_callback, nullptr);

    if (active || hovered) {
        char pct[16];
        sprintf_s(pct, "%.0f", *value);
        const ImVec2 txt_sz = ImGui::CalcTextSize(pct);
        const ImVec2 pad(5.f, 2.f);
        ImVec2 tip0(
            floorf(knob_cx - (txt_sz.x + pad.x * 2.f) * 0.5f + 0.5f),
            floorf(p0.y - txt_sz.y - pad.y * 2.f - 3.f + 0.5f));
        ImVec2 tip1(tip0.x + txt_sz.x + pad.x * 2.f, tip0.y + txt_sz.y + pad.y * 2.f);
        if (tip0.x < p0.x) {
            tip1.x += p0.x - tip0.x;
            tip0.x = p0.x;
        }
        if (tip1.x > p1.x) {
            tip0.x -= tip1.x - p1.x;
            tip1.x = p1.x;
        }
        dl->AddRectFilled(tip0, tip1, IM_COL32(73, 73, 73, 235), 2.f);
        dl->AddRect(tip0, tip1, IM_COL32(25, 25, 25, 255), 2.f);
        dl->AddText(ImVec2(tip0.x + pad.x, tip0.y + pad.y - 1.f), IM_COL32(255, 255, 255, 255), pct);
    }

    ImGui::PopID();
    return changed;
}

enum class BadgeIconKind {
    Speaker,
    SpeakerMuted,
    Mic,
    MicMuted,
};

static bool icon_badge_button(const char* id, ImVec2 size, BadgeIconKind kind,
                              ImVec4 bg, ImVec4 bg_hover, ImU32 fg, bool disabled = false) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_hover);
    const bool clicked = ImGui::Button("##icon", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float alpha = disabled ? 0.45f : 1.0f;
    UiTexture* tex = nullptr;
    switch (kind) {
    case BadgeIconKind::Speaker:      tex = &g_spk_on_tex; break;
    case BadgeIconKind::SpeakerMuted: tex = &g_spk_off_tex; break;
    case BadgeIconKind::Mic:          tex = &g_mic_on_tex; break;
    case BadgeIconKind::MicMuted:     tex = &g_mic_off_tex; break;
    }
    if (tex && tex->tex) {
        const ImVec2 icon_size(11.f, 11.f);
        const ImVec2 icon_p0((p0.x + p1.x - icon_size.x) * 0.5f, (p0.y + p1.y - icon_size.y) * 0.5f);
        const ImVec2 icon_p1(icon_p0.x + icon_size.x, icon_p0.y + icon_size.y);
        dl->AddImage((ImTextureID)tex->tex, icon_p0, icon_p1,
                     ImVec2(0.f, 0.f),
                     ImVec2(tex->u1, tex->v1),
                     ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, alpha)));
    }

    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return clicked && !disabled;
}


//
struct PlayerListCache {
    std::vector<uint32_t> active;     // char_ids that sent audio recently
    std::vector<uint32_t> muted;      // client-side muted char_ids
    std::vector<uint32_t> all_ids;    // union of active + muted
    double last_refresh = -1.0;       // ImGui::GetTime() of last refresh
    static constexpr double REFRESH_INTERVAL = 0.25; // seconds (4 Hz)

    void refresh_if_needed(VoiceClient& vc) {
        double now = ImGui::GetTime();
        if (now - last_refresh < REFRESH_INTERVAL) return;
        last_refresh = now;

        active = vc.get_active_speakers();
        muted  = vc.get_muted_players();

        // Authoritative source: server sends only players within 14 cells.
        // Do NOT merge active/muted back in — a player who walked out of range
        // must not reappear just because they spoke recently or are locally muted.
        all_ids = vc.get_nearby_players();
    }
};
static PlayerListCache g_player_cache;

static void close_settings();   // forward declaration

HWND detect_hwnd(LPDIRECT3DDEVICE9 pDevice) {
    if (!pDevice) return nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(pDevice->GetCreationParameters(&cp)) && cp.hFocusWindow)
        return cp.hFocusWindow;
    return GetForegroundWindow();
}

LRESULT CALLBACK hkOverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_visible && g_imgui_inited) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        const bool in_game = VoiceClient::get().is_in_game();

        // Show Windows arrow cursor whenever ImGui wants the mouse
        if (msg == WM_SETCURSOR) {
            ImGuiIO& io2 = ImGui::GetIO();
            if (g_settings_open || g_call_popup || g_whisper_popup || io2.WantCaptureMouse) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
        }

        // Toggle badge visibility with Scroll Lock
        if (msg == WM_KEYDOWN && wparam == VK_SCROLL) {
            g_badge_visible = !g_badge_visible;
            return 1;
        }

//
        if (in_game && msg == WM_KEYDOWN && wparam == VK_HOME) {
            if (!g_call_popup) {
                g_call_popup = true;
                g_call_name[0] = '\0';
            } else {
                g_call_popup = false;
            }
            return 1;
        }

        // Block game input whenever any ImGui overlay control is capturing input.
        // The compact badge/channel buttons are always-on UI too; without this,
        // clicking them also reaches the game and can make the character walk.
        ImGuiIO& io = ImGui::GetIO();
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEMOVE:   case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (io.WantCaptureMouse || mouse_over_compact_overlay()) return 1;
            break;
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if ((g_settings_open || g_whisper_popup || g_call_popup) && io.WantCaptureKeyboard) return 1;
            break;
        }
    }
    return CallWindowProcW(g_old_wndproc, hwnd, msg, wparam, lparam);
}

//
static const ImVec4 COL_GREEN  = { 0.20f, 0.90f, 0.30f, 1.f };
static const ImVec4 COL_RED    = { 0.90f, 0.20f, 0.20f, 1.f };
static const ImVec4 COL_GRAY   = { 0.55f, 0.55f, 0.55f, 1.f };
static const ImVec4 COL_YELLOW = { 1.00f, 0.85f, 0.00f, 1.f };
static const ImVec4 COL_BLUE   = { 0.30f, 0.80f, 1.00f, 1.f };

//
static std::string vk_name(int vk) {
    if (vk >= 'A' && vk <= 'Z') { char s[2] = { (char)vk, 0 }; return s; }
    if (vk >= '0' && vk <= '9') { char s[2] = { (char)vk, 0 }; return s; }
    if (vk >= VK_F1 && vk <= VK_F12) {
        char s[4]; sprintf_s(s, "F%d", vk - VK_F1 + 1); return s;
    }
    switch (vk) {
        case VK_SPACE:    return "Space";
        case VK_SHIFT:    return "Shift";
        case VK_LSHIFT:   return "LShift";
        case VK_RSHIFT:   return "RShift";
        case VK_CONTROL:  return "Ctrl";
        case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_MENU:     return "Alt";
        case VK_LMENU:    return "LAlt";
        case VK_RMENU:    return "RAlt";
        case VK_LWIN:     return "LWin";
        case VK_RWIN:     return "RWin";
        case VK_ESCAPE:   return "Esc";
        case VK_CAPITAL:  return "CapsLk";
        case VK_TAB:      return "Tab";
        case VK_BACK:     return "Backspace";
        case VK_RETURN:   return "Enter";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "PgUp";
        case VK_NEXT:     return "PgDn";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_PAUSE:    return "Pause";
        case VK_SNAPSHOT: return "PrtScr";
        case VK_SCROLL:   return "ScrLk";
        case VK_NUMLOCK:  return "NumLk";
        case VK_APPS:     return "Menu";
        case VK_NUMPAD0:  return "Num0";
        case VK_NUMPAD1:  return "Num1";
        case VK_NUMPAD2:  return "Num2";
        case VK_NUMPAD3:  return "Num3";
        case VK_NUMPAD4:  return "Num4";
        case VK_NUMPAD5:  return "Num5";
        case VK_NUMPAD6:  return "Num6";
        case VK_NUMPAD7:  return "Num7";
        case VK_NUMPAD8:  return "Num8";
        case VK_NUMPAD9:  return "Num9";
        case VK_MULTIPLY: return "Num*";
        case VK_ADD:      return "Num+";
        case VK_SUBTRACT: return "Num-";
        case VK_DECIMAL:  return "Num.";
        case VK_DIVIDE:   return "Num/";
        default: { char s[8]; sprintf_s(s, "0x%02X", vk); return s; }
    }
}

//
static void push_ro_style() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg,         ImVec4(0.95f, 0.95f, 0.95f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border,           ImVec4(0.60f, 0.60f, 0.65f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,          ImVec4(0.12f, 0.24f, 0.58f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    ImVec4(0.16f, 0.30f, 0.68f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.82f, 0.82f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.75f, 0.80f, 0.92f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.70f, 0.76f, 0.92f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.22f, 0.42f, 0.82f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.32f, 0.56f, 1.00f, 1.00f));
//
    ImGui::PushStyleColor(ImGuiCol_Button,           ImVec4(0.68f, 0.70f, 0.78f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    ImVec4(0.78f, 0.80f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,     ImVec4(0.50f, 0.52f, 0.62f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,          ImVec4(0.82f, 0.82f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Header,           ImVec4(0.22f, 0.42f, 0.82f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    ImVec4(0.32f, 0.54f, 0.92f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Separator,        ImVec4(0.50f, 0.50f, 0.55f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,             ImVec4(0.05f, 0.05f, 0.05f, 1.00f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    10.f); // pill-shaped buttons/fields
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(6.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(8.f, 4.f));
}

static void pop_ro_style() {
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(17);
}

//
static void close_settings() {
    if (!g_settings_open) return;
//
    g_settings_open = false;
    g_binding_key   = false;
    VoiceClient::get().save_settings();   // persist PTT key, gains, devices, channel
}

//
void draw_settings_window() {
    auto& vc = VoiceClient::get();
    ensure_ui_assets(g_ui_device);

    if (!g_devs_loaded) {
        g_mic_devs = vc.get_mic_devices();
        g_spk_devs = vc.get_speaker_devices();
        const std::wstring cur_mic = vc.get_mic_device_id();
        const std::wstring cur_spk = vc.get_speaker_device_id();
        g_sel_mic = -1;
        for (int i = 0; i < (int)g_mic_devs.size(); i++)
            if (g_mic_devs[i].id == cur_mic) { g_sel_mic = i; break; }
        g_sel_spk = -1;
        for (int i = 0; i < (int)g_spk_devs.size(); i++)
            if (g_spk_devs[i].id == cur_spk) { g_sel_spk = i; break; }
        g_devs_loaded = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const float win_w = g_bg_tex.tex ? (float)g_bg_tex.width : 360.0f;
    const float win_h = g_bg_tex.tex ? (float)g_bg_tex.height : 520.0f;
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoBackground;

    push_ro_style();

    char win_title[64];
    snprintf(win_title, sizeof(win_title), "%s###vc_settings",
        L("\xe0\xb8\x95\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice Settings"));
    bool open = true;
    if (!ImGui::Begin(win_title, &open, flags)) {
        ImGui::End();
        pop_ro_style();
        if (!open) close_settings();
        return;
    }
    if (!open) close_settings();

    ImVec2 win_pos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_bg_tex.tex) {
        dl->PushClipRect(win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h), false);
        dl->AddImage((ImTextureID)g_bg_tex.tex, win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h),
                     ImVec2(0, 0), ImVec2(g_bg_tex.u1, g_bg_tex.v1), IM_COL32_WHITE);
        dl->PopClipRect();
    }
    const ImVec4 hdr = ImVec4(0.00f, 0.00f, 0.00f, 1.f);
    const ImVec4 sub = ImVec4(0.00f, 0.00f, 0.00f, 1.f);
    const float content_left = 14.f;
    const float content_right = 346.f;
    const float content_w = content_right - content_left;
    const float content_h = 408.f;
    const float footer_y = 494.f;
    const float top_lang_x = 304.f;
    const float top_lang_y = 22.f;
    const float footer_close_x = 312.f;
    const float section_line_left = 14.f;
    const float section_line_right = 346.f;
    const ImVec2 title_pos(win_pos.x + 16.f, win_pos.y + 1.f);

    dl->AddText(title_pos, IM_COL32(32, 44, 92, 255),
        L("\xe0\xb8\x81\xe0\xb8\xb2\xe0\xb8\xa3\xe0\xb8\x95\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice Settings"));

    ImGui::SetCursorPos(ImVec2(win_w - 17.f, 3.f));
    ImGui::PushID("settings_close_top");
    ImGui::InvisibleButton("##btn", ImVec2(12.f, 12.f));
    const bool close_hovered = ImGui::IsItemHovered();
    const bool close_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 close_p0 = ImGui::GetItemRectMin();
    const ImVec2 close_p1 = ImGui::GetItemRectMax();
    if (close_hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(close_p0, close_p1, IM_COL32(255, 255, 255, 20), 2.0f);
    }
    if (g_close_tex.tex) {
        const float img_w = (float)g_close_tex.width;
        const float img_h = (float)g_close_tex.height;
        const ImVec2 img_p0(
            floorf(close_p0.x + ((close_p1.x - close_p0.x) - img_w) * 0.5f + 0.5f),
            floorf(close_p0.y + ((close_p1.y - close_p0.y) - img_h) * 0.5f + 0.5f));
        dl->AddCallback(set_point_sampler_callback, nullptr);
        dl->AddImage((ImTextureID)g_close_tex.tex, img_p0, ImVec2(img_p0.x + img_w, img_p0.y + img_h),
                     ImVec2(0, 0), ImVec2(g_close_tex.u1, g_close_tex.v1), IM_COL32_WHITE);
        dl->AddCallback(set_linear_sampler_callback, nullptr);
    }
    ImGui::PopID();
    if (close_clicked) {
        ImGui::End();
        pop_ro_style();
        close_settings();
        return;
    }

    dl->AddRectFilled(ImVec2(win_pos.x + section_line_left, win_pos.y + 28.f),
                      ImVec2(win_pos.x + section_line_right, win_pos.y + 51.f),
                      IM_COL32(255, 255, 255, 255));

    ImGui::SetCursorPos(ImVec2(content_left, 30.f));
    if (tab_button("##tab_voice", L("\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice"), ImVec2(46.f, 22.f), g_settings_tab == 0)) g_settings_tab = 0;
    ImGui::SameLine(0.f, 4.f);
    if (tab_button("##tab_players", L("\xe0\xb8\x9c\xe0\xb8\xb9\xe0\xb9\x89\xe0\xb9\x80\xe0\xb8\xa5\xe0\xb9\x88\xe0\xb8\x99", "Players"), ImVec2(54.f, 22.f), g_settings_tab == 1)) g_settings_tab = 1;
    ImGui::SameLine(0.f, 4.f);
    if (tab_button("##tab_devices", L("\xe0\xb8\xad\xe0\xb8\xb8\xe0\xb8\x9b\xe0\xb8\x81\xe0\xb8\xa3\xe0\xb8\x93\xe0\xb9\x8c", "Devices"), ImVec2(58.f, 22.f), g_settings_tab == 2)) g_settings_tab = 2;
    ImGui::SetCursorPos(ImVec2(top_lang_x, top_lang_y));
    if (lang_button("##lang_top", g_lang_thai ? "TH" : "EN", ImVec2(42.f, 24.f), false)) g_lang_thai = !g_lang_thai;

    dl->AddLine(ImVec2(win_pos.x + section_line_left, win_pos.y + 51.f), ImVec2(win_pos.x + section_line_right, win_pos.y + 51.f), IM_COL32(30, 30, 30, 255), 1.0f);

    if (g_settings_tab == 0) {
        if (g_binding_key) {
            for (int vk = 8; vk < 256; vk++) {
                if (vk == VK_ESCAPE) {
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        g_binding_key = false;
                        break;
                    }
                    continue;
                }
                if (vk == VK_INSERT) continue;
                if (GetAsyncKeyState(vk) & 0x8000) {
                    vc.set_ptt_key(vk);
                    g_binding_key = false;
                    break;
                }
            }
        }

        ImGui::SetCursorPos(ImVec2(content_left, 62.f));
        ImGui::TextColored(hdr, L("\xe0\xb9\x82\xe0\xb8\xab\xe0\xb8\xa1\xe0\xb8\x94\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Mic Mode"));
        dl->AddLine(ImVec2(win_pos.x + section_line_left, win_pos.y + 82.f), ImVec2(win_pos.x + section_line_right, win_pos.y + 82.f), IM_COL32(165, 170, 181, 255), 1.0f);
        const bool open_mic = vc.is_open_mic();
        ImGui::SetCursorPos(ImVec2(content_left, 90.f));
        if (image_button_with_label("##btn_ptt_mode", L("\xe0\xb8\x81\xe0\xb8\x94\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\x9e\xe0\xb8\xb9\xe0\xb8\x94", "Push to Talk"), ImVec2(157.f, 24.f), !open_mic)) vc.set_open_mic(false);
        ImGui::SetCursorPos(ImVec2(190.f, 90.f));
        if (image_button_with_label("##btn_open_mic_mode", L("\xe0\xb9\x80\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Open Mic"), ImVec2(157.f, 24.f), open_mic)) vc.set_open_mic(true);

        ImGui::SetCursorPos(ImVec2(content_left, 121.f));
        ImGui::TextColored(hdr, L("\xe0\xb8\x9b\xe0\xb8\xb8\xe0\xb9\x88\xe0\xb8\xa1 PTT", "PTT Key"));
        dl->AddLine(ImVec2(win_pos.x + section_line_left, win_pos.y + 141.f), ImVec2(win_pos.x + section_line_right, win_pos.y + 141.f), IM_COL32(165, 170, 181, 255), 1.0f);
        std::string bind_label;
        if (g_binding_key) {
            bind_label = L("\xe0\xb8\x81\xe0\xb8\x94\xe0\xb8\x9b\xe0\xb8\xb8\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x95\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\x87\xe0\xb8\x81\xe0\xb8\xb2\xe0\xb8\xa3...", "Press a key...");
        } else {
            bind_label = "PTT: " + vk_name(vc.get_ptt_key()) + "  (" +
                L("\xe0\xb8\x84\xe0\xb8\xa5\xe0\xb8\xb4\xe0\xb8\x81\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb9\x80\xe0\xb8\x9b\xe0\xb8\xa5\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\xa2\xe0\xb8\x99", "click to change") + ")";
        }
        ImGui::SetCursorPos(ImVec2(content_left, 150.f));
        if (ptt_key_button("##btn_bind_key", bind_label.c_str(), ImVec2(content_w, 24.f), g_binding_key) && !g_binding_key) {
            g_binding_key = true;
        }

        ImGui::SetCursorPos(ImVec2(content_left, 181.f));
        ImGui::TextColored(hdr, L("\xe0\xb8\xa3\xe0\xb8\xb0\xe0\xb8\x94\xe0\xb8\xb1\xe0\xb8\x9a\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Volume"));
        dl->AddLine(ImVec2(win_pos.x + section_line_left, win_pos.y + 201.f), ImVec2(win_pos.x + section_line_right, win_pos.y + 201.f), IM_COL32(165, 170, 181, 255), 1.0f);
        ImGui::SetCursorPos(ImVec2(content_left, 211.f));
        ImGui::TextColored(sub, L("\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Mic"));
        ImGui::SetCursorPos(ImVec2(content_left, 230.f));
        float mic_pct = vc.get_mic_gain() * 100.f;
        if (pretty_volume_slider("mic_voice", &mic_pct, 0.f, 100.f, ImVec2(content_w, 22.f)))
            vc.set_mic_gain(mic_pct / 100.f);

        ImGui::SetCursorPos(ImVec2(content_left, 261.f));
        ImGui::TextColored(sub, L("\xe0\xb8\xa5\xe0\xb8\xb3\xe0\xb9\x82\xe0\xb8\x9e\xe0\xb8\x87", "Speaker"));
        ImGui::SetCursorPos(ImVec2(content_left, 280.f));
        float spk_pct = vc.get_speaker_gain() * 100.f;
        if (pretty_volume_slider("spk_voice", &spk_pct, 0.f, 100.f, ImVec2(content_w, 22.f)))
            vc.set_speaker_gain(spk_pct / 100.f);

    } else if (g_settings_tab == 1) {
        ImGui::SetCursorPos(ImVec2(content_left, 64.f));
        ImGui::BeginChild("##players_tab_fixed", ImVec2(content_w, content_h), false, ImGuiWindowFlags_HorizontalScrollbar);

        float avw = ImGui::GetContentRegionAvail().x;
        g_player_cache.refresh_if_needed(vc);
        const auto& active = g_player_cache.active;
        const auto& all_ids = g_player_cache.all_ids;

        const float ptt_h = g_btn_ptt_key_tex.tex ? (float)g_btn_ptt_key_tex.height : 24.f;
        const float ex_w  = g_btn_exit_tex.tex    ? (float)g_btn_exit_tex.width     : 42.f;
        const float ex_h  = g_btn_exit_tex.tex    ? (float)g_btn_exit_tex.height    : 24.f;

        if (!all_ids.empty()) {
            bool any_unmuted = false;
            for (uint32_t id : all_ids) {
                if (!vc.is_player_muted(id)) { any_unmuted = true; break; }
            }
            const char* mute_all_lbl = any_unmuted
                ? L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87\xe0\xb8\x97\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb8\xab\xe0\xb8\xa1\xe0\xb8\x94", "Mute All")
                : L("\xe0\xb9\x80\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87\xe0\xb8\x97\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb8\xab\xe0\xb8\xa1\xe0\xb8\x94", "Unmute All");
            if (ptt_key_button("##btn_mute_all", mute_all_lbl, ImVec2(avw, ptt_h), !any_unmuted)) {
                for (uint32_t id : all_ids) {
                    if (any_unmuted) vc.mute_player(id); else vc.unmute_player(id);
                }
                g_player_cache.last_refresh = -1.0;
            }
            ImGui::Spacing();
        }

        if (all_ids.empty()) {
            ImGui::TextColored(sub, L("(\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\xb5\xe0\xb8\x9c\xe0\xb8\xb9\xe0\xb9\x89\xe0\xb9\x80\xe0\xb8\xa5\xe0\xb9\x88\xe0\xb8\x99\xe0\xb9\x83\xe0\xb8\x81\xe0\xb8\xa5\xe0\xb9\x89\xe0\xb9\x80\xe0\xb8\x84\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87)", "(no players nearby)"));
        } else {
            const bool can_wsp = (vc.get_whisper_state() == VoiceClient::WhisperState::None);
            for (uint32_t id : all_ids) {
                const bool is_muted = vc.is_player_muted(id);
                std::string name = vc.get_speaker_name(id);
                if (name.empty()) { char tmp[16]; sprintf_s(tmp, "#%u", id); name = tmp; }

                bool speaking = false;
                for (uint32_t a : active) if (a == id) { speaking = true; break; }
                ImGui::TextColored(is_muted ? ImVec4(0.85f, 0.15f, 0.15f, 1.f) : (speaking ? ImVec4(0.3f, 1.f, 0.3f, 1.f) : ImVec4(0.f, 0.f, 0.f, 1.f)), "%s%s", is_muted ? "x " : (speaking ? "* " : "  "), name.c_str());

                ImGui::SameLine((std::max)(120.0f, avw - ex_w * 2.f - 4.f));
                char w_id[32]; sprintf_s(w_id, "##w%u", id);
                if (exit_button(w_id, L("\xe0\xb9\x82\xe0\xb8\x97\xe0\xb8\xa3", "Call"), ImVec2(ex_w, ex_h), false) && can_wsp) vc.whisper_request(id);
                ImGui::SameLine(0, 4.f);

                char m_id[32]; sprintf_s(m_id, "##m%u", id);
                const char* m_lbl = is_muted ? L("\xe0\xb9\x80\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94", "Unmute") : L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94", "Mute");
                if (exit_button(m_id, m_lbl, ImVec2(ex_w, ex_h), is_muted)) {
                    if (is_muted) vc.unmute_player(id); else vc.mute_player(id);
                    g_player_cache.last_refresh = -1.0;
                }
            }
        }
        ImGui::EndChild();
    } else {
        ImGui::SetCursorPos(ImVec2(content_left, 64.f));
        ImGui::BeginChild("##devices_tab_fixed", ImVec2(content_w, content_h), false, 0);

        float avw = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(hdr, L("\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x82\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb9\x82\xe0\xb8\x9f\xe0\xb8\x99", "Microphone"));
        ImGui::Separator();
        ImGui::Spacing();

        const char* mic_preview = g_mic_devs.empty() ? "(none)"
            : (g_sel_mic >= 0 && g_sel_mic < (int)g_mic_devs.size()) ? g_mic_devs[g_sel_mic].name.c_str() : "Default";
        if (begin_device_combo("##mic_dev", mic_preview, avw)) {
            if (ImGui::Selectable(L("\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb9\x80\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\x95\xe0\xb9\x89\xe0\xb8\x99", "Default"), g_sel_mic == -1)) { g_sel_mic = -1; vc.set_mic_device(L""); }
            for (int i = 0; i < (int)g_mic_devs.size(); i++) {
                if (ImGui::Selectable(g_mic_devs[i].name.c_str(), g_sel_mic == i)) {
                    g_sel_mic = i;
                    vc.set_mic_device(g_mic_devs[i].id);
                }
            }
            end_device_combo();
        }

        ImGui::Spacing();
        ImGui::TextColored(hdr, L("\xe0\xb8\xa5\xe0\xb8\xb3\xe0\xb9\x82\xe0\xb8\x9e\xe0\xb8\x87", "Speaker"));
        ImGui::Separator();
        ImGui::Spacing();

        const char* spk_preview = g_spk_devs.empty() ? "(none)"
            : (g_sel_spk >= 0 && g_sel_spk < (int)g_spk_devs.size()) ? g_spk_devs[g_sel_spk].name.c_str() : "Default";
        if (begin_device_combo("##spk_dev", spk_preview, avw)) {
            if (ImGui::Selectable(L("\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb9\x80\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\x95\xe0\xb9\x89\xe0\xb8\x99", "Default"), g_sel_spk == -1)) { g_sel_spk = -1; vc.set_speaker_device(L""); }
            for (int i = 0; i < (int)g_spk_devs.size(); i++) {
                if (ImGui::Selectable(g_spk_devs[i].name.c_str(), g_sel_spk == i)) {
                    g_sel_spk = i;
                    vc.set_speaker_device(g_spk_devs[i].id);
                }
            }
            end_device_combo();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const float meter = (std::min)(1.0f, vc.get_mic_rms() * 7.5f);
        const ImVec4 status_bg = ImVec4(0.16f, 0.18f, 0.22f, 0.92f);
        const ImVec4 status_border = ImVec4(0.34f, 0.38f, 0.46f, 0.90f);
        const ImVec4 status_text = ImVec4(0.96f, 0.96f, 0.98f, 1.0f);
        ImGui::TextColored(hdr, L("\xe0\xb8\xaa\xe0\xb8\x96\xe0\xb8\xb2\xe0\xb8\x99\xe0\xb8\xb0\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Mic Status"));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, status_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, status_border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 5.f));
        ImGui::BeginChild("##voice_status_chip_devices", ImVec2(0.f, 38.f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        const ImVec2 chip_pos = ImGui::GetWindowPos();
        const ImVec2 chip_size = ImGui::GetWindowSize();
        const float dot_y = chip_pos.y + 12.f;
        ImU32 dot_col = IM_COL32(255, 210, 40, 255);
        if (!vc.is_connected()) dot_col = IM_COL32(120, 180, 255, 255);
        else if (vc.is_muted() && vc.is_deafened()) dot_col = IM_COL32(230, 95, 95, 255);
        else if (vc.is_muted() || vc.is_deafened()) dot_col = IM_COL32(255, 170, 70, 255);
        else if (!vc.is_locally_talking()) dot_col = IM_COL32(185, 190, 205, 255);
        sdl->AddCircleFilled(ImVec2(chip_pos.x + 10.f, dot_y), 5.f, dot_col);
        ImGui::SetCursorPos(ImVec2(20.f, 2.f));
        const char* device_status = !vc.is_connected() ? L("\xe0\xb8\x81\xe0\xb8\xb3\xe0\xb8\xa5\xe0\xb8\xb1\xe0\xb8\x87\xe0\xb9\x80\xe0\xb8\x8a\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\xa1\xe0\xb8\x95\xe0\xb9\x88\xe0\xb8\xad", "Connecting")
    : (vc.is_muted() && vc.is_deafened()) ? L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb8\x97\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb8\xab\xe0\xb8\xa1\xe0\xb8\x94", "Muted")
    : vc.is_muted() ? L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Mic Off")
    : vc.is_deafened() ? L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Speaker Off")
    : vc.is_locally_talking() ? L("\xe0\xb8\x81\xe0\xb8\xb3\xe0\xb8\xa5\xe0\xb8\xb1\xe0\xb8\x87\xe0\xb8\x9e\xe0\xb8\xb9\xe0\xb8\x94", "Talking")
    : L("\xe0\xb8\x9e\xe0\xb8\xa3\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\xa1", "Ready");
        ImGui::TextColored(status_text, "%s", device_status);
        const ImVec2 track_p0(chip_pos.x + 20.f, chip_pos.y + chip_size.y - 9.f);
        const ImVec2 track_p1(chip_pos.x + chip_size.x - 10.f, chip_pos.y + chip_size.y - 5.f);
        sdl->AddRectFilled(track_p0, track_p1, IM_COL32(82, 88, 104, 210), 2.f);
        if (meter > 0.001f) {
            const float fill_x = track_p0.x + (track_p1.x - track_p0.x) * meter;
            sdl->AddRectFilled(track_p0, ImVec2(fill_x, track_p1.y), IM_COL32(255, 210, 40, 255), 2.f);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::EndChild();
    }

    ImGui::SetCursorPos(ImVec2(footer_close_x, footer_y));
    if (exit_button("##settings_close_footer", L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94", "Close"), ImVec2(42.f, 24.f))) close_settings();

    ImGui::End();
    pop_ro_style();
}
void draw_call_popup() {
    auto& vc = VoiceClient::get();
    ensure_ui_assets(g_ui_device);

    // Don't allow new call while already in whisper
    bool busy = (vc.get_whisper_state() != VoiceClient::WhisperState::None);

    ImGuiIO& io = ImGui::GetIO();
    const float win_w = g_call_bg_tex.tex ? (float)g_call_bg_tex.width : 260.f;
    const float win_h = g_call_bg_tex.tex ? (float)g_call_bg_tex.height : 150.f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.38f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);

    constexpr ImGuiWindowFlags kF =
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoScrollbar      |
        ImGuiWindowFlags_NoCollapse       |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoNav            |
        ImGuiWindowFlags_NoTitleBar       |
        ImGuiWindowFlags_NoBackground;

    push_ro_style();
    ImGui::Begin("##call_popup", nullptr, kF);

    ImVec2 win_pos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_call_bg_tex.tex) {
        dl->PushClipRect(win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h), false);
        dl->AddImage((ImTextureID)g_call_bg_tex.tex, win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h),
                     ImVec2(0, 0), ImVec2(g_call_bg_tex.u1, g_call_bg_tex.v1), IM_COL32_WHITE);
        dl->PopClipRect();
    }

    const float content_left = 10.f;
    const float content_right = win_w - 10.f;
    const float content_w = content_right - content_left;
    const ImVec4 hdr = ImVec4(0.f, 0.f, 0.f, 1.f);
    const ImVec4 hint = ImVec4(0.50f, 0.50f, 0.50f, 1.f);

    ImGui::SetCursorPos(ImVec2(win_w - 17.f, 3.f));
    ImGui::PushID("call_close_top");
    ImGui::InvisibleButton("##btn", ImVec2(12.f, 12.f));
    const bool close_hovered = ImGui::IsItemHovered();
    const bool close_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 close_p0 = ImGui::GetItemRectMin();
    const ImVec2 close_p1 = ImGui::GetItemRectMax();
    if (close_hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(close_p0, close_p1, IM_COL32(255, 255, 255, 20), 2.0f);
    }
    if (g_close_tex.tex) {
        const float img_w = (float)g_close_tex.width;
        const float img_h = (float)g_close_tex.height;
        const ImVec2 img_p0(
            floorf(close_p0.x + ((close_p1.x - close_p0.x) - img_w) * 0.5f + 0.5f),
            floorf(close_p0.y + ((close_p1.y - close_p0.y) - img_h) * 0.5f + 0.5f));
        dl->AddCallback(set_point_sampler_callback, nullptr);
        dl->AddImage((ImTextureID)g_close_tex.tex, img_p0, ImVec2(img_p0.x + img_w, img_p0.y + img_h),
                     ImVec2(0, 0), ImVec2(g_close_tex.u1, g_close_tex.v1), IM_COL32_WHITE);
        dl->AddCallback(set_linear_sampler_callback, nullptr);
    }
    ImGui::PopID();
    if (close_clicked) {
        g_call_popup = false;
    }

    ImGui::SetCursorPos(ImVec2(content_left + 6.f, 2.f));
    ImGui::TextColored(hdr, L("\xe0\xb9\x82\xe0\xb8\x97\xe0\xb8\xa3\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice Call"));

    ImGui::SetCursorPos(ImVec2(content_left, 28.f));
    ImGui::TextColored(hdr, L("\xe0\xb8\x81\xe0\xb8\xa3\xe0\xb8\xad\xe0\xb8\x81\xe0\xb8\x8a\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\x95\xe0\xb8\xb1\xe0\xb8\xa7\xe0\xb8\xa5\xe0\xb8\xb0\xe0\xb8\x84\xe0\xb8\xa3", "Character name"));

    ImGui::SetCursorPos(ImVec2(content_left, 48.f));
    ImGui::SetNextItemWidth(content_w);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(232, 232, 236, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(232, 232, 236, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(232, 232, 236, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(188, 188, 192, 255));
    bool enter_pressed = ImGui::InputText("##callname", g_call_name, sizeof(g_call_name),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere(-1);

    const float bw = content_w;
    ImGui::SetCursorPos(ImVec2(content_left, 84.f));
    if (!busy) {
        bool call_clicked = call_button("##call_btn", L("\xe0\xb9\x82\xe0\xb8\x97\xe0\xb8\xa3", "Call"), ImVec2(bw, 26.f)) || enter_pressed;

        if (call_clicked && g_call_name[0] != '\0') {
            vc.whisper_lookup(g_call_name);
            g_call_popup = false;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.35f, 0.40f, 0.7f));
        ImGui::Button(L("\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb8\xa7\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x87", "Busy"), ImVec2(bw, 26.f));
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorPos(ImVec2(win_w - 53.f, win_h - 26.f));
    if (exit_button("##call_close_footer", L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94", "Close"), ImVec2(42.f, 24.f))) {
        g_call_popup = false;
    }

    // Close on Escape
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_call_popup = false;
    }

    ImGui::End();
    pop_ro_style();
}

//
void draw_whisper_popup() {
    auto& vc = VoiceClient::get();
    ensure_ui_assets(g_ui_device);

    ImGuiIO& io = ImGui::GetIO();
    const float win_w = g_whisper_call_bg_tex.tex ? (float)g_whisper_call_bg_tex.width : 136.f;
    const float win_h = g_whisper_call_bg_tex.tex ? (float)g_whisper_call_bg_tex.height : 80.f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - 10.f, io.DisplaySize.y - 80.f),
        ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);

    constexpr ImGuiWindowFlags kPFlags =
        ImGuiWindowFlags_NoDecoration     |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoScrollbar      |
        ImGuiWindowFlags_NoSavedSettings  |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav             |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::Begin("##wsp_popup", nullptr, kPFlags);

    ImVec2 win_pos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bg_p1(win_pos.x + win_w, win_pos.y + win_h);
    if (g_whisper_call_bg_tex.tex) {
        dl->AddCallback(set_point_sampler_callback, nullptr);
        dl->PushClipRect(win_pos, bg_p1, false);
        dl->AddImage((ImTextureID)g_whisper_call_bg_tex.tex,
                     win_pos,
                     bg_p1,
                     ImVec2(0.f, 0.f),
                     ImVec2(g_whisper_call_bg_tex.u1, g_whisper_call_bg_tex.v1),
                     IM_COL32_WHITE);
        dl->PopClipRect();
        dl->AddCallback(set_linear_sampler_callback, nullptr);
    } else {
        dl->AddRectFilled(win_pos, bg_p1, IM_COL32(245, 245, 245, 245), 3.f);
        dl->AddRect(win_pos, bg_p1, IM_COL32(116, 137, 178, 255), 3.f);
    }

    float t = (float)ImGui::GetTime();

    float pulse = 0.70f + 0.30f * sinf(t * 5.f);
    std::string peer = vc.get_whisper_peer();
    if (peer.empty()) peer = "???";
    DWORD elapsed = GetTickCount() - vc.get_whisper_tick();
    int remain = 30 - (int)(elapsed / 1000u);
    if (remain < 0) remain = 0;
    char cd[12]; sprintf_s(cd, "(%ds)", remain);

    const char* incoming_label = L("\xe0\xb8\xaa\xe0\xb8\xb2\xe0\xb8\xa2\xe0\xb9\x80\xe0\xb8\x82\xe0\xb9\x89\xe0\xb8\xb2", "Incoming call");
    dl->AddText(ImVec2(win_pos.x + 15.f, win_pos.y + 2.f), IM_COL32(38, 54, 96, 255), incoming_label);
    const ImVec2 cd_sz = ImGui::CalcTextSize(cd);
    dl->AddText(ImVec2(win_pos.x + win_w - cd_sz.x - 8.f, win_pos.y + 2.f), IM_COL32(92, 92, 92, 255), cd);

    const ImVec2 star_pos(win_pos.x + 6.f, win_pos.y + 28.f);
    dl->AddText(star_pos, ImColor(ImVec4(0.58f, 0.18f, 0.82f, pulse)), "*");
    const float peer_max_w = 74.f;
    ImVec2 peer_min(win_pos.x + 16.f, win_pos.y + 28.f);
    ImVec2 peer_max(peer_min.x + peer_max_w, peer_min.y + ImGui::GetTextLineHeight());
    dl->PushClipRect(peer_min, peer_max, true);
    dl->AddText(peer_min, IM_COL32(20, 20, 20, 255), peer.c_str());
    dl->PopClipRect();

    const float btn_y = 54.f;
    ImGui::SetCursorPos(ImVec2(7.f, btn_y));
    if (whisper_call_button("##wsp_accept", L("\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a", "Accept"), ImVec2(60.f, 24.f)))
        vc.whisper_accept();

    ImGui::SetCursorPos(ImVec2(69.f, btn_y));
    if (whisper_call_button("##wsp_decline", L("\xe0\xb8\x9b\xe0\xb8\x8f\xe0\xb8\xb4\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\x98", "Decline"), ImVec2(60.f, 24.f)))
        vc.whisper_reject();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

//
//
//
//
//
//
// Single-window voice bar — no stutter (replaces ##vc + ##ch pair)
//
void draw_voicebar_call_window() {
    ImGuiIO& io = ImGui::GetIO();
    ensure_ui_assets(g_ui_device);

    auto& vc        = VoiceClient::get();
    bool  connected = vc.is_connected();
    bool  ptt       = vc.is_locally_talking();
    bool  muted     = vc.is_muted();
    bool  deafened  = vc.is_deafened();
    Channel ch      = vc.get_channel();

    UiTexture* state_tex = &g_badge_idle_tex;
    if (!connected)            state_tex = &g_badge_connecting_tex;
    else if (muted && deafened) state_tex = &g_badge_mute_tex;
    else if (muted)             state_tex = &g_badge_mic_off_tex;
    else if (deafened)          state_tex = &g_badge_spk_off_tex;
    else if (ptt)               state_tex = &g_badge_talk_tex;

    const float badge_w = state_tex && state_tex->tex ? (float)state_tex->width  : 70.f;
    const float badge_h = state_tex && state_tex->tex ? (float)state_tex->height : 54.f;

    // Always anchor to bottom-right corner
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - 5.f, io.DisplaySize.y - 2.f),
        ImGuiCond_Always, ImVec2(1.f, 1.f));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration       |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_AlwaysAutoResize   |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(6.f, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);

    ImGui::Begin("##vcbar2", nullptr, kFlags);

    const ImVec2 win_pos = ImGui::GetWindowPos();

    // Draw background rect (buttons area only, NOT behind slime).
    // Use GetWindowDrawList so the rect is drawn BEFORE items in draw order —
    // buttons/icons appear ON TOP; slime is outside the rect's x-range entirely.
    static ImVec2 s_bar_min{0.f,0.f}, s_bar_max{0.f,0.f};
    if (s_bar_max.x > s_bar_min.x) {
        auto* dl = ImGui::GetWindowDrawList();
        // Push clip rect that covers the full bar width so the left rounded corner
        // isn't clipped by the window's inner content clip (win_pos.x + WindowPadding).
        dl->PushClipRect(s_bar_min, s_bar_max, false);
        dl->AddRectFilled(s_bar_min, s_bar_max, IM_COL32(35,38,43,int(0.72f*255)), 4.f);
        dl->PopClipRect();
    }

    // Push buttons DOWN to match original ##ch placement.
    // ##ch was anchored: bottom = slime_bottom - 2px  →  button strip in slime's lower area.
    // Derived: CursorPosY = badge_h - 16  (= 38 for 54px slime)
    ImGui::SetCursorPosY(badge_h - 16.f);

    const ImVec2 icon_btn_size(24.f, 18.f);
    const ImVec4 icon_bg        = ImVec4(0.20f, 0.23f, 0.27f, 0.95f);
    const ImVec4 icon_bg_hover  = ImVec4(0.27f, 0.31f, 0.36f, 1.00f);
    const ImVec4 icon_off_bg    = ImVec4(0.45f, 0.15f, 0.15f, 0.95f);
    const ImVec4 icon_off_hover = ImVec4(0.56f, 0.20f, 0.20f, 1.00f);

    // [Speaker toggle]
    if (icon_badge_button("##spk_toggle", icon_btn_size,
            deafened ? BadgeIconKind::SpeakerMuted : BadgeIconKind::Speaker,
            deafened ? icon_off_bg : icon_bg,
            deafened ? icon_off_hover : icon_bg_hover,
            IM_COL32_WHITE)) {
        const bool next_deafen = !deafened;
        vc.set_deafen(next_deafen);
        deafened = next_deafen;
        muted = vc.is_muted();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", deafened ? "Enable speaker" : "Disable speaker");
    ImGui::SameLine(0, 4.f);

    // [Channel buttons / whisper strip]
    auto ws2 = vc.get_whisper_state();
    bool in_whisper_ui = (ch == Channel::Whisper)
                      || (ws2 == VoiceClient::WhisperState::Calling);
    const bool war_mode = vc.is_war_mode();

    if (in_whisper_ui) {
        float ct = (float)ImGui::GetTime();
        if (ws2 == VoiceClient::WhisperState::Calling) {
            float pulse = 0.70f + 0.30f * sinf(ct * 5.f);
            std::string peer = vc.get_whisper_peer();
            ImGui::TextColored(ImVec4(0.80f, 0.40f, 1.00f, pulse),
                               ">> %s", peer.empty() ? "..." : peer.c_str());
        } else {
            float pulse = 0.75f + 0.25f * sinf(ct * 2.f);
            ImGui::TextColored(ImVec4(0.80f, 0.40f, 1.00f, pulse), "[W]");
            ImGui::SameLine(0, 4.f);
            std::string peer = vc.get_whisper_peer();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.f),
                               "%s", peer.empty() ? "..." : peer.c_str());
        }
        ImGui::SameLine(0, 8.f);
        const char* end_lbl = (ws2 == VoiceClient::WhisperState::Calling)
            ? L("\xe0\xb8\xa2\xe0\xb8\x81\xe0\xb9\x80\xe0\xb8\xa5\xe0\xb8\xb4\xe0\xb8\x81", "Cancel")
            : L("\xe0\xb8\xa7\xe0\xb8\xb2\xe0\xb8\x87\xe0\xb8\xaa\xe0\xb8\xb2\xe0\xb8\xa2", "Hang up");
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.58f, 0.12f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.18f, 0.18f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.f,   1.f,   1.f,   1.f));
        if (ImGui::Button(end_lbl, ImVec2(0.f, 18.f)))
            vc.whisper_end();
        ImGui::PopStyleColor(3);
    } else {
        struct ChTab { const char* label; Channel ch; };
        ChTab tabs[4] = {
            { L("\xe0\xb8\x9b\xe0\xb8\x81\xe0\xb8\x95\xe0\xb8\xb4",            "Normal"), Channel::Normal },
            { L("\xe0\xb8\x9b\xe0\xb8\xb2\xe0\xb8\xa3\xe0\xb9\x8c\xe0\xb8\x95\xe0\xb8\xb5\xe0\xb9\x89", "Party"),  Channel::Party  },
            { L("\xe0\xb8\x81\xe0\xb8\xb4\xe0\xb8\xa5\xe0\xb8\x94\xe0\xb9\x8c", "Guild"),  Channel::Guild  },
            { L("\xe0\xb8\xab\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\x87",             "Room"),   Channel::Room   },
        };
        for (int i = 0; i < 4; i++) {
            bool sel = (ch == tabs[i].ch);
            const bool room_unavail = (tabs[i].ch == Channel::Room) && (ch != Channel::Room);
            const bool blocked = room_unavail
                              || (war_mode && (tabs[i].ch == Channel::Normal || tabs[i].ch == Channel::Room));
            ImGui::PushStyleColor(ImGuiCol_Button,
                sel ? ImVec4(0.18f, 0.50f, 0.18f, 1.f)
                    : blocked ? ImVec4(0.18f, 0.18f, 0.18f, 0.55f)
                              : ImVec4(0.25f, 0.25f, 0.25f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                sel ? ImVec4(0.22f, 0.60f, 0.22f, 1.f)
                    : blocked ? ImVec4(0.24f, 0.24f, 0.24f, 0.60f)
                              : ImVec4(0.38f, 0.38f, 0.38f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                sel ? ImVec4(1.f, 1.f, 1.f, 1.f)
                    : blocked ? ImVec4(0.45f, 0.45f, 0.45f, 0.90f)
                              : ImVec4(0.72f, 0.72f, 0.72f, 1.f));
            if (ImGui::Button(tabs[i].label, ImVec2(0.f, 18.f)) && !blocked)
                vc.set_channel(tabs[i].ch);
            if (room_unavail && ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto-assigned when in a chat room");
            ImGui::PopStyleColor(3);
            if (i < 3) ImGui::SameLine(0, 4.f);
        }
    }

    ImGui::SameLine(0, 4.f);
    // [Mic toggle]
    if (icon_badge_button("##mic_toggle", icon_btn_size,
            muted ? BadgeIconKind::MicMuted : BadgeIconKind::Mic,
            muted ? icon_off_bg : icon_bg,
            muted ? icon_off_hover : icon_bg_hover,
            IM_COL32_WHITE)) {
        vc.set_mute(!muted);
        muted = !muted;
    }
    // Capture button strip right edge BEFORE tooltip changes last-item state
    const ImVec2 bar_end_scr = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", muted ? "Enable mic" : "Disable mic");

    // [Slime badge — right side, no background behind it]
    // Move cursor back UP to window top so slime sits above the button strip,
    // exactly as in the original ##vc window which was anchored at bottom-right.
    if (state_tex && state_tex->tex) {
        ImGui::SameLine(0, 12.f); // +6px right
        ImGui::SetCursorPosY(10.f); // 4 + 6px down
        ImGui::Image((ImTextureID)state_tex->tex,
                     ImVec2(badge_w, badge_h),
                     ImVec2(0.f, 0.f), ImVec2(state_tex->u1, state_tex->v1));
        if (ImGui::IsItemHovered()) {
            const char* status = L("\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice");
            if (!connected) status = L("\xe0\xb8\xad\xe0\xb8\xad\xe0\xb8\x9f\xe0\xb9\x84\xe0\xb8\xa5\xe0\xb8\x99\xe0\xb9\x8c", "Offline");
            else if (deafened) status = L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb8\xab\xe0\xb8\xb9", "Deafened");
            else if (muted) status = L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Muted");
            else if (ptt) status = L("\xe0\xb8\x81\xe0\xb8\xb3\xe0\xb8\xa5\xe0\xb8\xb1\xe0\xb8\x87\xe0\xb8\x9e\xe0\xb8\xb9\xe0\xb8\x94", "Talking");
            ImGui::SetTooltip("%s", status);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (!g_settings_open) {
                g_settings_open = true;
                g_devs_loaded = false;
            } else {
                close_settings();
            }
        }
    }

    // Update hit rects (win_pos captured earlier, right after Begin)
    const ImVec2 win_size = ImGui::GetWindowSize();
    g_badge_hit_min   = win_pos;
    g_badge_hit_max   = ImVec2(win_pos.x + win_size.x, win_pos.y + win_size.y);
    g_channel_hit_min = g_badge_hit_min;
    g_channel_hit_max = g_badge_hit_max;

    // Background rect: button strip only (spk→mic), NOT behind slime.
    // y-range mirrors original ##ch window: top = btn_top - WindowPadding.y (4),
    //                                        bottom = btn_bottom + WindowPadding.y (4)
    // bar_end_scr.y = btn_bottom → top = bar_end_scr.y - 18 - 4 = bar_end_scr.y - 22
    s_bar_min = ImVec2(win_pos.x,            bar_end_scr.y - 22.f);
    s_bar_max = ImVec2(bar_end_scr.x + 5.f, bar_end_scr.y + 4.f);

    ImGui::End();
    ImGui::PopStyleVar(5);

    // Whisper notice toast
    {
        auto& vn = VoiceClient::get();
        std::string notice = vn.get_whisper_notice();
        if (!notice.empty()) {
            DWORD age = GetTickCount() - vn.get_whisper_notice_tick();
            if (age < 3000) {
                float alpha = (age < 2500) ? 1.f : 1.f - (float)(age - 2500) / 500.f;
                ImGui::SetNextWindowPos(
                    ImVec2(win_pos.x + win_size.x, win_pos.y - 30.f),
                    ImGuiCond_Always, ImVec2(1.f, 1.f));
                ImGui::SetNextWindowBgAlpha(0.75f * alpha);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(7.f, 4.f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::Begin("##wsp_notice", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
                if (notice == "War Mode") {
                    ImGui::TextColored(ImVec4(1.f, 0.78f, 0.20f, alpha), "%s", notice.c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, alpha),
                        "Whisper: %s", notice.c_str());
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            } else {
                vn.clear_whisper_notice();
            }
        }
    }
}

//
//
void draw_voice_window() {
    ImGuiIO& io = ImGui::GetIO();
    ensure_ui_assets(g_ui_device);

    auto& vc        = VoiceClient::get();
    bool  connected = vc.is_connected();
    bool  ptt       = vc.is_locally_talking();
    bool  muted     = vc.is_muted();
    bool  deafened  = vc.is_deafened();
    Channel ch      = vc.get_channel();

    UiTexture* state_tex = &g_badge_idle_tex;
    if (!connected) {
        state_tex = &g_badge_connecting_tex;
    } else if (muted && deafened) {
        state_tex = &g_badge_mute_tex;
    } else if (muted) {
        state_tex = &g_badge_mic_off_tex;
    } else if (deafened) {
        state_tex = &g_badge_spk_off_tex;
    } else if (ptt) {
        state_tex = &g_badge_talk_tex;
    }

    const ImVec2 badge_size(
        state_tex && state_tex->tex ? (float)state_tex->width : 70.f,
        state_tex && state_tex->tex ? (float)state_tex->height : 54.f);

    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - 10.f, io.DisplaySize.y - 10.f),
        ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.0f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration       |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_AlwaysAutoResize   |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);

    ImGui::Begin("##vc", nullptr, kFlags);
    ImGui::InvisibleButton("##slime_badge", badge_size);
    const bool badge_hovered = ImGui::IsItemHovered();
    const bool badge_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 badge_pos = ImGui::GetItemRectMin();
    if (badge_clicked) {
        if (!g_settings_open) {
            g_settings_open = true;
            g_devs_loaded = false;
        } else {
            close_settings();
        }
    }
    if (state_tex && state_tex->tex) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImage((ImTextureID)state_tex->tex, badge_pos,
                     ImVec2(badge_pos.x + badge_size.x, badge_pos.y + badge_size.y),
                     ImVec2(0.f, 0.f), ImVec2(state_tex->u1, state_tex->v1), IM_COL32_WHITE);
    }

    if (badge_hovered) {
        const char* status = L("\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\xa2\xe0\xb8\x87", "Voice");
        if (!connected) status = L("\xe0\xb8\xad\xe0\xb8\xad\xe0\xb8\x9f\xe0\xb9\x84\xe0\xb8\xa5\xe0\xb8\x99\xe0\xb9\x8c", "Offline");
        else if (deafened) status = L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb8\xab\xe0\xb8\xb9", "Deafened");
        else if (muted) status = L("\xe0\xb8\x9b\xe0\xb8\xb4\xe0\xb8\x94\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb8\x84\xe0\xb9\x8c", "Muted");
        else if (ptt) status = L("\xe0\xb8\x81\xe0\xb8\xb3\xe0\xb8\xa5\xe0\xb8\xb1\xe0\xb8\x87\xe0\xb8\x9e\xe0\xb8\xb9\xe0\xb8\x94", "Talking");
        ImGui::SetTooltip("%s", status);
    }

    g_badge_hit_min = badge_pos;
    g_badge_hit_max = ImVec2(badge_pos.x + badge_size.x, badge_pos.y + badge_size.y);

    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::SetNextWindowPos(
        ImVec2(badge_pos.x - 6.f, badge_pos.y + badge_size.y - 2.f),
        ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.72f);

    constexpr ImGuiWindowFlags kChFlags =
        ImGuiWindowFlags_NoDecoration       |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_AlwaysAutoResize   |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(5.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(4.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.f, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);

    ImGui::Begin("##ch", nullptr, kChFlags);

    struct ChTab { const char* label; Channel ch; };
    ChTab tabs[4] = {
        { L("\xe0\xb8\x9b\xe0\xb8\x81\xe0\xb8\x95\xe0\xb8\xb4",            "Normal"), Channel::Normal },
        { L("\xe0\xb8\x9b\xe0\xb8\xb2\xe0\xb8\xa3\xe0\xb9\x8c\xe0\xb8\x95\xe0\xb8\xb5\xe0\xb9\x89", "Party"),  Channel::Party  },
        { L("\xe0\xb8\x81\xe0\xb8\xb4\xe0\xb8\xa5\xe0\xb8\x94\xe0\xb9\x8c", "Guild"),  Channel::Guild  },
        { L("\xe0\xb8\xab\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\x87",             "Room"),   Channel::Room   },
    };

    auto ws2 = vc.get_whisper_state();
    bool in_whisper_ui = (ch == Channel::Whisper)
                      || (ws2 == VoiceClient::WhisperState::Calling);
    const bool war_mode = vc.is_war_mode();

    const ImVec2 icon_btn_size(24.f, 18.f);
    const ImVec4 icon_bg        = ImVec4(0.20f, 0.23f, 0.27f, 0.95f);
    const ImVec4 icon_bg_hover  = ImVec4(0.27f, 0.31f, 0.36f, 1.00f);
    const ImVec4 icon_off_bg    = ImVec4(0.45f, 0.15f, 0.15f, 0.95f);
    const ImVec4 icon_off_hover = ImVec4(0.56f, 0.20f, 0.20f, 1.00f);

    if (icon_badge_button("##spk_toggle", icon_btn_size,
            deafened ? BadgeIconKind::SpeakerMuted : BadgeIconKind::Speaker,
            deafened ? icon_off_bg : icon_bg,
            deafened ? icon_off_hover : icon_bg_hover,
            IM_COL32_WHITE)) {
        const bool next_deafen = !deafened;
        vc.set_deafen(next_deafen);
        deafened = next_deafen;
        muted = vc.is_muted();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", deafened ? "Enable speaker" : "Disable speaker");
    }
    ImGui::SameLine(0, 4.f);

    if (in_whisper_ui) {
        float ct = (float)ImGui::GetTime();
        if (ws2 == VoiceClient::WhisperState::Calling) {
            float pulse = 0.70f + 0.30f * sinf(ct * 5.f);
            std::string peer = vc.get_whisper_peer();
            ImGui::TextColored(ImVec4(0.80f, 0.40f, 1.00f, pulse),
                               ">> %s", peer.empty() ? "..." : peer.c_str());
        } else {
            float pulse = 0.75f + 0.25f * sinf(ct * 2.f);
            ImGui::TextColored(ImVec4(0.80f, 0.40f, 1.00f, pulse), "[W]");
            ImGui::SameLine(0, 4.f);
            std::string peer = vc.get_whisper_peer();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.f),
                               "%s", peer.empty() ? "..." : peer.c_str());
        }
        ImGui::SameLine(0, 8.f);
        const char* end_lbl = (ws2 == VoiceClient::WhisperState::Calling)
            ? L("\xe0\xb8\xa2\xe0\xb8\x81\xe0\xb9\x80\xe0\xb8\xa5\xe0\xb8\xb4\xe0\xb8\x81", "Cancel")
            : L("\xe0\xb8\xa7\xe0\xb8\xb2\xe0\xb8\x87\xe0\xb8\xaa\xe0\xb8\xb2\xe0\xb8\xa2", "Hang up");
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.58f, 0.12f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.18f, 0.18f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.f,   1.f,   1.f,   1.f));
        if (ImGui::Button(end_lbl, ImVec2(0.f, 18.f)))
            vc.whisper_end();
        ImGui::PopStyleColor(3);
    } else {
        for (int i = 0; i < 4; i++) {
            bool sel = (ch == tabs[i].ch);
            // Room is assigned by the map server (chat_join/chat_leave) — the player
            // cannot manually enter a room.  Disable the button unless already in one.
            const bool room_unavail = (tabs[i].ch == Channel::Room) && (ch != Channel::Room);
            const bool blocked = room_unavail
                              || (war_mode && (tabs[i].ch == Channel::Normal || tabs[i].ch == Channel::Room));
            ImGui::PushStyleColor(ImGuiCol_Button,
                sel ? ImVec4(0.18f, 0.50f, 0.18f, 1.f)
                    : blocked ? ImVec4(0.18f, 0.18f, 0.18f, 0.55f)
                              : ImVec4(0.25f, 0.25f, 0.25f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                sel ? ImVec4(0.22f, 0.60f, 0.22f, 1.f)
                    : blocked ? ImVec4(0.24f, 0.24f, 0.24f, 0.60f)
                              : ImVec4(0.38f, 0.38f, 0.38f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                sel ? ImVec4(1.f, 1.f, 1.f, 1.f)
                    : blocked ? ImVec4(0.45f, 0.45f, 0.45f, 0.90f)
                              : ImVec4(0.72f, 0.72f, 0.72f, 1.f));
            if (ImGui::Button(tabs[i].label, ImVec2(0.f, 18.f)) && !blocked)
                vc.set_channel(tabs[i].ch);
            if (room_unavail && ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto-assigned when in a chat room");
            ImGui::PopStyleColor(3);
            if (i < 3) ImGui::SameLine(0, 4.f);
        }
    }

    ImGui::SameLine(0, 4.f);
    if (icon_badge_button("##mic_toggle", icon_btn_size,
            muted ? BadgeIconKind::MicMuted : BadgeIconKind::Mic,
            muted ? icon_off_bg : icon_bg,
            muted ? icon_off_hover : icon_bg_hover,
            IM_COL32_WHITE)) {
        vc.set_mute(!muted);
        muted = !muted;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", muted ? "Enable mic" : "Disable mic");
    }
    const ImVec2 channel_pos = ImGui::GetWindowPos();

    const ImVec2 channel_size = ImGui::GetWindowSize();
    g_channel_hit_min = channel_pos;
    g_channel_hit_max = ImVec2(channel_pos.x + channel_size.x, channel_pos.y + channel_size.y);

    ImGui::End();
    ImGui::PopStyleVar(5);

//
    {
        auto& vn = VoiceClient::get();
        std::string notice = vn.get_whisper_notice();
        if (!notice.empty()) {
            DWORD age = GetTickCount() - vn.get_whisper_notice_tick();
            if (age < 3000) {
                float alpha = (age < 2500) ? 1.f : 1.f - (float)(age - 2500) / 500.f;
                ImGui::SetNextWindowPos(
                    ImVec2(badge_pos.x + badge_size.x, badge_pos.y - 30.f),
                    ImGuiCond_Always, ImVec2(1.f, 1.f));
                ImGui::SetNextWindowBgAlpha(0.75f * alpha);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(7.f, 4.f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::Begin("##wsp_notice", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
                if (notice == "War Mode") {
                    ImGui::TextColored(ImVec4(1.f, 0.78f, 0.20f, alpha), "%s", notice.c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, alpha),
                        "Whisper: %s", notice.c_str());
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            } else {
                vn.clear_whisper_notice();
            }
        }
    }
}

} // namespace

namespace Overlay {

bool init(LPDIRECT3DDEVICE9 pDevice) {
    if (g_imgui_inited) return true;

    dbglog("[overlay] init start");
    g_ui_device = pDevice;

    if (!g_gdiplus_token) {
        GdiplusStartupInput gdiplus_input;
        const auto gdist = GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr);
        if (gdist != Ok) {
            g_gdiplus_token = 0;
            sprintf_s(g_bg_status, sizeof(g_bg_status), "GDI startup=%d", (int)gdist);
            sprintf_s(g_btn_status, sizeof(g_btn_status), "GDI startup=%d", (int)gdist);
        }
    }

    g_hwnd = detect_hwnd(pDevice);
    if (!g_hwnd) { dbglog("[overlay] hwnd not found"); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // Load font with Thai glyph support (Tahoma ships with Windows and has Thai)
    static const ImWchar font_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0E00, 0x0E7F, // Thai
        0,
    };
    const char* thai_font = "C:\\Windows\\Fonts\\tahoma.ttf";
    bool font_ok = (GetFileAttributesA(thai_font) != INVALID_FILE_ATTRIBUTES)
                && (io.Fonts->AddFontFromFileTTF(thai_font, 14.0f, nullptr, font_ranges) != nullptr);
    if (!font_ok)
        io.Fonts->AddFontDefault();

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        dbglog("[overlay] ImGui_ImplWin32_Init FAILED");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(pDevice)) {
        dbglog("[overlay] ImGui_ImplDX9_Init FAILED");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_old_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(hkOverlayWndProc)));
    g_imgui_inited = true;
    g_visible      = true;


    dbglog("[overlay] init done");
    return true;
}

void shutdown() {
    if (!g_imgui_inited) return;
    dbglog("[overlay] shutdown");
    if (g_old_wndproc && g_hwnd)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_old_wndproc));
    g_old_wndproc  = nullptr;
    release_ui_texture(g_bg_tex);
    release_ui_texture(g_call_bg_tex);
    release_ui_texture(g_btn_tex);
    release_ui_texture(g_btn_hover_tex);
    release_ui_texture(g_btn_lang_tex);
    release_ui_texture(g_btn_lang_hover_tex);
    release_ui_texture(g_btn_exit_tex);
    release_ui_texture(g_btn_exit_hover_tex);
    release_ui_texture(g_btn_ptt_key_tex);
    release_ui_texture(g_btn_ptt_key_hover_tex);
    release_ui_texture(g_btn_call_tex);
    release_ui_texture(g_btn_call_hover_tex);
    release_ui_texture(g_whisper_call_bg_tex);
    release_ui_texture(g_btn_whisper_call_tex);
    release_ui_texture(g_btn_whisper_call_hover_tex);
    release_ui_texture(g_btn_dropdown_tex);
    release_ui_texture(g_close_tex);
    release_ui_texture(g_mic_off_tex);
    release_ui_texture(g_spk_off_tex);
    release_ui_texture(g_mic_on_tex);
    release_ui_texture(g_spk_on_tex);
    release_ui_texture(g_badge_idle_tex);
    release_ui_texture(g_badge_talk_tex);
    release_ui_texture(g_badge_mute_tex);
    release_ui_texture(g_badge_spk_off_tex);
    release_ui_texture(g_badge_mic_off_tex);
    release_ui_texture(g_badge_connecting_tex);
    release_ui_texture(g_slider_left_tex);
    release_ui_texture(g_slider_right_tex);
    release_ui_texture(g_slider_knob_tex);
    if (g_gdiplus_token) {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0;
    }
    g_ui_device     = nullptr;

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_hwnd         = nullptr;
    g_imgui_inited = false;
}

void on_reset_before() {
    if (g_imgui_inited) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void on_reset_after(LPDIRECT3DDEVICE9 pDevice) {
    g_ui_device = pDevice;
    if (g_imgui_inited) ImGui_ImplDX9_CreateDeviceObjects();
}

void render(LPDIRECT3DDEVICE9 pDevice) {
    g_ui_device = pDevice;
    if (!g_imgui_inited) {
        if (!init(pDevice)) return;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    auto& vc = VoiceClient::get();
    bool in_game = vc.is_in_game();
    bool on_map = vc.is_on_map();
    if (!on_map) {
        if (g_settings_open) close_settings();
        g_call_popup = false;
        g_whisper_popup = false;
        g_badge_hit_min = g_badge_hit_max = ImVec2(0.f, 0.f);
        g_channel_hit_min = g_channel_hit_max = ImVec2(0.f, 0.f);
    }

    g_whisper_popup = in_game &&
        (vc.get_whisper_state() == VoiceClient::WhisperState::Incoming);

    if (g_visible && in_game && g_badge_visible) draw_voicebar_call_window();
    if (g_settings_open && in_game) draw_settings_window();
    if (g_whisper_popup)             draw_whisper_popup();
    if (g_call_popup && in_game)     draw_call_popup();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

} // namespace Overlay














