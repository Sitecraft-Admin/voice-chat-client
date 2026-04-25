#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <functional>

struct AudioDeviceItem {
    std::wstring id;
    std::wstring name;
    bool is_default = false;
    bool is_default_comm = false;
};

class VoiceOverlay {
public:
    using DeviceChangedCallback = std::function<void(const std::wstring& device_id)>;

    static VoiceOverlay& instance();

    bool initialize(HWND hwnd, IDirect3DDevice9* device);
    void shutdown();

    void on_device_lost();
    void on_device_reset(IDirect3DDevice9* device);

    void begin_frame();
    void render();
    void end_frame();

    void toggle();
    bool visible() const { return visible_; }

    void refresh_devices();

    const std::wstring& selected_input_id() const { return selected_input_id_; }
    const std::wstring& selected_output_id() const { return selected_output_id_; }

    void set_input_changed_callback(DeviceChangedCallback cb) { on_input_changed_ = std::move(cb); }
    void set_output_changed_callback(DeviceChangedCallback cb) { on_output_changed_ = std::move(cb); }

    bool handle_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);

private:
    VoiceOverlay() = default;
    ~VoiceOverlay() = default;
    VoiceOverlay(const VoiceOverlay&) = delete;
    VoiceOverlay& operator=(const VoiceOverlay&) = delete;

    void load_config();
    void save_config() const;

    void draw_main_window();
    void draw_status_block();
    void draw_device_selectors();

    void apply_selected_input(int index);
    void apply_selected_output(int index);

private:
    HWND hwnd_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    bool initialized_ = false;
    bool visible_ = false;

    std::vector<AudioDeviceItem> input_devices_;
    std::vector<AudioDeviceItem> output_devices_;

    std::wstring selected_input_id_;
    std::wstring selected_output_id_;

    int selected_input_index_ = -1;
    int selected_output_index_ = -1;

    float mic_gain_ = 1.0f;
    float speaker_gain_ = 1.0f;
    int ptt_vk_ = 'V';

    DeviceChangedCallback on_input_changed_;
    DeviceChangedCallback on_output_changed_;
};
