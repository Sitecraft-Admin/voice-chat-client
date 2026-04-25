#include "voice_overlay.hpp"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {
const wchar_t* kOverlayIni = L".\\voice_overlay.ini";

std::wstring read_ini_string(const wchar_t* section, const wchar_t* key, const wchar_t* def_value) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, def_value, buf, static_cast<DWORD>(std::size(buf)), kOverlayIni);
    return buf;
}

void write_ini_string(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), kOverlayIni);
}

std::vector<AudioDeviceItem> enumerate_devices(EDataFlow flow) {
    std::vector<AudioDeviceItem> out;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* default_dev = nullptr;
    IMMDevice* default_comm = nullptr;

    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (!enumerator) return out;

    enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_dev);
    enumerator->GetDefaultAudioEndpoint(flow, eCommunications, &default_comm);

    LPWSTR default_id = nullptr;
    LPWSTR default_comm_id = nullptr;
    if (default_dev) default_dev->GetId(&default_id);
    if (default_comm) default_comm->GetId(&default_comm_id);

    enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (!collection) {
        if (default_id) CoTaskMemFree(default_id);
        if (default_comm_id) CoTaskMemFree(default_comm_id);
        if (default_dev) default_dev->Release();
        if (default_comm) default_comm->Release();
        enumerator->Release();
        return out;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        IPropertyStore* props = nullptr;
        LPWSTR dev_id = nullptr;
        PROPVARIANT pv;
        PropVariantInit(&pv);

        if (FAILED(collection->Item(i, &dev)) || !dev)
            continue;

        if (FAILED(dev->GetId(&dev_id)) || !dev_id) {
            dev->Release();
            continue;
        }

        std::wstring name = L"Unknown device";
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                name = pv.pwszVal;
            }
        }

        AudioDeviceItem item;
        item.id = dev_id;
        item.name = name;
        item.is_default = (default_id && wcscmp(default_id, dev_id) == 0);
        item.is_default_comm = (default_comm_id && wcscmp(default_comm_id, dev_id) == 0);
        out.push_back(item);

        PropVariantClear(&pv);
        if (props) props->Release();
        CoTaskMemFree(dev_id);
        dev->Release();
    }

    if (default_id) CoTaskMemFree(default_id);
    if (default_comm_id) CoTaskMemFree(default_comm_id);
    if (default_dev) default_dev->Release();
    if (default_comm) default_comm->Release();
    collection->Release();
    enumerator->Release();
    return out;
}

std::string narrow_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

const char* device_label(const AudioDeviceItem& item, std::string& tmp) {
    tmp = narrow_utf8(item.name);
    if (item.is_default) tmp += " [Default]";
    if (item.is_default_comm) tmp += " [Comm]";
    return tmp.c_str();
}
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

VoiceOverlay& VoiceOverlay::instance() {
    static VoiceOverlay g;
    return g;
}

bool VoiceOverlay::initialize(HWND hwnd, IDirect3DDevice9* device) {
    if (initialized_) return true;

    hwnd_ = hwnd;
    device_ = device;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd_))
        return false;
    if (!ImGui_ImplDX9_Init(device_))
        return false;

    load_config();
    refresh_devices();

    initialized_ = true;
    return true;
}

void VoiceOverlay::shutdown() {
    if (!initialized_) return;

    save_config();

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    device_ = nullptr;
    hwnd_ = nullptr;
    initialized_ = false;
}

void VoiceOverlay::on_device_lost() {
    if (initialized_)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

void VoiceOverlay::on_device_reset(IDirect3DDevice9* device) {
    device_ = device;
    if (initialized_)
        ImGui_ImplDX9_CreateDeviceObjects();
}

void VoiceOverlay::begin_frame() {
    if (!initialized_) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void VoiceOverlay::render() {
    if (!initialized_ || !visible_) return;
    draw_main_window();
}

void VoiceOverlay::end_frame() {
    if (!initialized_) return;
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void VoiceOverlay::toggle() {
    visible_ = !visible_;
}

bool VoiceOverlay::handle_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (!initialized_) return false;

    if (msg == WM_KEYUP && wparam == VK_INSERT) {
        toggle();
        result = 0;
        return true;
    }

    if (visible_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        result = 1;
        return true;
    }

    return false;
}

void VoiceOverlay::refresh_devices() {
    input_devices_ = enumerate_devices(eCapture);
    output_devices_ = enumerate_devices(eRender);

    selected_input_index_ = -1;
    selected_output_index_ = -1;

    for (int i = 0; i < static_cast<int>(input_devices_.size()); ++i) {
        if (!selected_input_id_.empty() && input_devices_[i].id == selected_input_id_) {
            selected_input_index_ = i;
            break;
        }
        if (selected_input_id_.empty() && input_devices_[i].is_default) {
            selected_input_index_ = i;
        }
    }

    for (int i = 0; i < static_cast<int>(output_devices_.size()); ++i) {
        if (!selected_output_id_.empty() && output_devices_[i].id == selected_output_id_) {
            selected_output_index_ = i;
            break;
        }
        if (selected_output_id_.empty() && output_devices_[i].is_default) {
            selected_output_index_ = i;
        }
    }

    if (selected_input_index_ >= 0)
        selected_input_id_ = input_devices_[selected_input_index_].id;
    if (selected_output_index_ >= 0)
        selected_output_id_ = output_devices_[selected_output_index_].id;
}

void VoiceOverlay::load_config() {
    selected_input_id_ = read_ini_string(L"voice", L"input_id", L"");
    selected_output_id_ = read_ini_string(L"voice", L"output_id", L"");
    mic_gain_ = static_cast<float>(GetPrivateProfileIntW(L"voice", L"mic_gain_x100", 100, kOverlayIni)) / 100.0f;
    speaker_gain_ = static_cast<float>(GetPrivateProfileIntW(L"voice", L"speaker_gain_x100", 100, kOverlayIni)) / 100.0f;
    ptt_vk_ = GetPrivateProfileIntW(L"voice", L"ptt_vk", 'V', kOverlayIni);
}

void VoiceOverlay::save_config() const {
    write_ini_string(L"voice", L"input_id", selected_input_id_);
    write_ini_string(L"voice", L"output_id", selected_output_id_);
    WritePrivateProfileStringW(L"voice", L"mic_gain_x100", std::to_wstring(static_cast<int>(mic_gain_ * 100.0f)).c_str(), kOverlayIni);
    WritePrivateProfileStringW(L"voice", L"speaker_gain_x100", std::to_wstring(static_cast<int>(speaker_gain_ * 100.0f)).c_str(), kOverlayIni);
    WritePrivateProfileStringW(L"voice", L"ptt_vk", std::to_wstring(ptt_vk_).c_str(), kOverlayIni);
}

void VoiceOverlay::draw_status_block() {
    ImGui::Text("Insert : toggle overlay");
    ImGui::Text("PTT key : %c", ptt_vk_ >= 32 && ptt_vk_ <= 126 ? ptt_vk_ : '?');
    ImGui::SliderFloat("Mic gain", &mic_gain_, 0.1f, 10.0f, "%.1fx");
    ImGui::SliderFloat("Speaker gain", &speaker_gain_, 0.1f, 10.0f, "%.1fx");
    ImGui::Separator();
}

void VoiceOverlay::apply_selected_input(int index) {
    if (index < 0 || index >= static_cast<int>(input_devices_.size())) return;
    selected_input_index_ = index;
    selected_input_id_ = input_devices_[index].id;
    if (on_input_changed_) on_input_changed_(selected_input_id_);
    save_config();
}

void VoiceOverlay::apply_selected_output(int index) {
    if (index < 0 || index >= static_cast<int>(output_devices_.size())) return;
    selected_output_index_ = index;
    selected_output_id_ = output_devices_[index].id;
    if (on_output_changed_) on_output_changed_(selected_output_id_);
    save_config();
}

void VoiceOverlay::draw_device_selectors() {
    std::string tmp;

    if (!input_devices_.empty()) {
        const char* preview = "Select input";
        if (selected_input_index_ >= 0 && selected_input_index_ < static_cast<int>(input_devices_.size()))
            preview = device_label(input_devices_[selected_input_index_], tmp);

        if (ImGui::BeginCombo("Input device", preview)) {
            for (int i = 0; i < static_cast<int>(input_devices_.size()); ++i) {
                std::string label_buf;
                bool selected = (i == selected_input_index_);
                if (ImGui::Selectable(device_label(input_devices_[i], label_buf), selected))
                    apply_selected_input(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (!output_devices_.empty()) {
        const char* preview = "Select output";
        if (selected_output_index_ >= 0 && selected_output_index_ < static_cast<int>(output_devices_.size()))
            preview = device_label(output_devices_[selected_output_index_], tmp);

        if (ImGui::BeginCombo("Output device", preview)) {
            for (int i = 0; i < static_cast<int>(output_devices_.size()); ++i) {
                std::string label_buf;
                bool selected = (i == selected_output_index_);
                if (ImGui::Selectable(device_label(output_devices_[i], label_buf), selected))
                    apply_selected_output(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::Button("Refresh devices")) {
        refresh_devices();
    }
}

void VoiceOverlay::draw_main_window() {
    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Voice Settings Overlay", &visible_, ImGuiWindowFlags_NoCollapse);

    draw_status_block();
    draw_device_selectors();

    ImGui::Spacing();
    ImGui::TextWrapped("Note: after changing mic or speaker, reconnect or rebuild the audio stream in your VoiceClient/Audio layer.");

    ImGui::End();
}
