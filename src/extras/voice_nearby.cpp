#include "voice_nearby.hpp"

#include "map.hpp"
#include "pc.hpp"
#include "party.hpp"
#include "guild.hpp"

#include <cstdarg>
#include <utility>

namespace {

struct VoiceCollectContext {
    map_session_data* src = nullptr;
    const VoiceQuery* query = nullptr;
    std::vector<VoiceNearbyPlayer>* out = nullptr;
};

static bool voice_accept_target(map_session_data* src, map_session_data* tsd, const VoiceQuery& query) {
    if (src == nullptr || tsd == nullptr)
        return false;

    if (!query.include_self && src == tsd)
        return false;

    if (tsd->fd <= 0)
        return false;

    if (src->bl.m != tsd->bl.m)
        return false;

    switch (query.mode) {
        case VoiceTargetMode::Proximity:
            return true;

        case VoiceTargetMode::Party:
            return src->status.party_id > 0 && tsd->status.party_id == src->status.party_id;

        case VoiceTargetMode::Guild:
            return src->status.guild_id > 0 && tsd->status.guild_id == src->status.guild_id;

        case VoiceTargetMode::Room:
            // TODO: bind room membership from your voice service / script layer.
            // For now, deny by default so it is explicit and safe.
            return false;

        case VoiceTargetMode::Whisper:
            return query.whisper_target_char_id > 0 &&
                   static_cast<uint32_t>(tsd->status.char_id) == query.whisper_target_char_id;
    }

    return false;
}

static int32 voice_collect_sub(struct block_list* bl, va_list ap) {
    auto* ctx = va_arg(ap, VoiceCollectContext*);
    if (ctx == nullptr || ctx->src == nullptr || ctx->query == nullptr || ctx->out == nullptr)
        return 0;

    if (bl == nullptr || bl->type != BL_PC)
        return 0;

    auto* tsd = reinterpret_cast<map_session_data*>(bl);
    if (!voice_accept_target(ctx->src, tsd, *ctx->query))
        return 0;

    VoiceNearbyPlayer p;
    p.account_id = static_cast<uint32_t>(tsd->status.account_id);
    p.char_id    = static_cast<uint32_t>(tsd->status.char_id);
    p.name       = tsd->status.name;
    p.map_id     = tsd->bl.m;
    p.x          = tsd->x;
    p.y          = tsd->y;
    p.party_id   = tsd->status.party_id;
    p.guild_id   = tsd->status.guild_id;
    p.fd         = tsd->fd;

    ctx->out->push_back(std::move(p));
    return 0;
}

} // namespace

std::vector<VoiceNearbyPlayer> voice_get_targets(map_session_data* sd, const VoiceQuery& query) {
    std::vector<VoiceNearbyPlayer> out;
    if (sd == nullptr || sd->fd <= 0)
        return out;

    VoiceCollectContext ctx;
    ctx.src = sd;
    ctx.query = &query;
    ctx.out = &out;

    switch (query.mode) {
        case VoiceTargetMode::Proximity:
            map_foreachinallrange(voice_collect_sub, &sd->bl, query.range, BL_PC, &ctx);
            break;

        case VoiceTargetMode::Party: {
            // gather same-party players anywhere on the same map server.
            for (const auto& it : mapit_getallusers()) {
                map_session_data* tsd = static_cast<map_session_data*>(it);
                if (tsd == nullptr)
                    continue;
                if (voice_accept_target(sd, tsd, query)) {
                    VoiceNearbyPlayer p;
                    p.account_id = static_cast<uint32_t>(tsd->status.account_id);
                    p.char_id    = static_cast<uint32_t>(tsd->status.char_id);
                    p.name       = tsd->status.name;
                    p.map_id     = tsd->bl.m;
                    p.x          = tsd->x;
                    p.y          = tsd->y;
                    p.party_id   = tsd->status.party_id;
                    p.guild_id   = tsd->status.guild_id;
                    p.fd         = tsd->fd;
                    out.push_back(std::move(p));
                }
            }
            break;
        }

        case VoiceTargetMode::Guild: {
            for (const auto& it : mapit_getallusers()) {
                map_session_data* tsd = static_cast<map_session_data*>(it);
                if (tsd == nullptr)
                    continue;
                if (voice_accept_target(sd, tsd, query)) {
                    VoiceNearbyPlayer p;
                    p.account_id = static_cast<uint32_t>(tsd->status.account_id);
                    p.char_id    = static_cast<uint32_t>(tsd->status.char_id);
                    p.name       = tsd->status.name;
                    p.map_id     = tsd->bl.m;
                    p.x          = tsd->x;
                    p.y          = tsd->y;
                    p.party_id   = tsd->status.party_id;
                    p.guild_id   = tsd->status.guild_id;
                    p.fd         = tsd->fd;
                    out.push_back(std::move(p));
                }
            }
            break;
        }

        case VoiceTargetMode::Room:
        case VoiceTargetMode::Whisper: {
            for (const auto& it : mapit_getallusers()) {
                map_session_data* tsd = static_cast<map_session_data*>(it);
                if (tsd == nullptr)
                    continue;
                if (voice_accept_target(sd, tsd, query)) {
                    VoiceNearbyPlayer p;
                    p.account_id = static_cast<uint32_t>(tsd->status.account_id);
                    p.char_id    = static_cast<uint32_t>(tsd->status.char_id);
                    p.name       = tsd->status.name;
                    p.map_id     = tsd->bl.m;
                    p.x          = tsd->x;
                    p.y          = tsd->y;
                    p.party_id   = tsd->status.party_id;
                    p.guild_id   = tsd->status.guild_id;
                    p.fd         = tsd->fd;
                    out.push_back(std::move(p));
                }
            }
            break;
        }
    }

    return out;
}
