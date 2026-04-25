#pragma once

#include <vector>
#include <string>
#include <cstdint>

struct map_session_data;

enum class VoiceTargetMode : uint8_t {
    Proximity = 0,
    Party     = 1,
    Guild     = 2,
    Room      = 3,
    Whisper   = 4,
};

struct VoiceNearbyPlayer {
    uint32_t account_id = 0;
    uint32_t char_id    = 0;
    std::string name;
    int16_t map_id      = 0;
    int16_t x           = 0;
    int16_t y           = 0;
    int32_t party_id    = 0;
    int32_t guild_id    = 0;
    int32_t fd          = 0;
};

struct VoiceQuery {
    VoiceTargetMode mode = VoiceTargetMode::Proximity;
    int16_t range        = 18;
    uint32_t room_id     = 0;
    uint32_t whisper_target_char_id = 0;
    bool include_self    = false;
};

std::vector<VoiceNearbyPlayer> voice_get_targets(map_session_data* sd, const VoiceQuery& query);

