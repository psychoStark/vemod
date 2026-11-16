#include "nb.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace nb {

namespace {

enum ObjectType {
    TYPE_ENGINE = 1,
    TYPE_OUTPUT_MIX = 2,
    TYPE_AUDIO_PLAYER = 3
};

enum InterfaceType {
    ITF_OBJECT = 0,
    ITF_ENGINE = 1,
    ITF_PLAY = 2,
    ITF_BUFFERQUEUE = 3,
    ITF_VOLUME = 4,
    ITF_ANDROIDCONFIGURATION = 5,
    ITF_SEEK = 6,
    ITF_PLAYBACKRATE = 7,
    ITF_PREFETCHSTATUS = 8,
    ITF_OUTPUTMIX = 9
};

// UUID definitions for matching guest IIDs
static const SLInterfaceID_ ID_ENGINE_VAL = { 0x1a736640, 0x32f1, 0x11e0, 0xbf4d, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_PLAY_VAL = { 0xef0bd920, 0x32f1, 0x11e0, 0xa4d3, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_BQ_VAL = { 0x291fd740, 0x32f2, 0x11e0, 0x8a75, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_ANDROIDSIMPLEBQ_VAL = { 0x33572936, 0x0350, 0x43e4, 0x9782, { 0xe2, 0x59, 0x40, 0x4b, 0xd0, 0x74 } };
static const SLInterfaceID_ ID_VOLUME_VAL = { 0x09e86a00, 0x32f3, 0x11e0, 0xbe4c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_CONFIG_VAL = { 0x8926aa60, 0xc079, 0x11df, 0xbb82, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_OBJECT_VAL = { 0x792e6220, 0x32f1, 0x11e0, 0xbf52, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_OUTPUTMIX_VAL = { 0xc7546a80, 0x32f1, 0x11e0, 0x9815, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_SEEK_VAL = { 0xd311c6e0, 0x32f1, 0x11e0, 0xb802, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_PLAYBACKRATE_VAL = { 0x2e307c80, 0x32f2, 0x11e0, 0x83e2, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID_ ID_PREFETCHSTATUS_VAL = { 0x5b36fa00, 0x32f2, 0x11e0, 0xbf5d, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

int match_iid(u32 iid_ptr) {
    if (!iid_ptr) return -1;
    const void* p = h(iid_ptr);
    if (!p) return -1;

    auto check_uuid = [](const void* ptr) -> int {
        if (!ptr) return -1;
        if (std::memcmp(ptr, &ID_ENGINE_VAL, 16) == 0) return ITF_ENGINE;
        if (std::memcmp(ptr, &ID_PLAY_VAL, 16) == 0) return ITF_PLAY;
        if (std::memcmp(ptr, &ID_ANDROIDSIMPLEBQ_VAL, 16) == 0) return ITF_BUFFERQUEUE;
        if (std::memcmp(ptr, &ID_BQ_VAL, 16) == 0) return ITF_BUFFERQUEUE;
        if (std::memcmp(ptr, &ID_VOLUME_VAL, 16) == 0) return ITF_VOLUME;
        if (std::memcmp(ptr, &ID_CONFIG_VAL, 16) == 0) return ITF_ANDROIDCONFIGURATION;
        if (std::memcmp(ptr, &ID_OBJECT_VAL, 16) == 0) return ITF_OBJECT;
        if (std::memcmp(ptr, &ID_OUTPUTMIX_VAL, 16) == 0) return ITF_OUTPUTMIX;
        if (std::memcmp(ptr, &ID_SEEK_VAL, 16) == 0) return ITF_SEEK;
        if (std::memcmp(ptr, &ID_PLAYBACKRATE_VAL, 16) == 0) return ITF_PLAYBACKRATE;
        if (std::memcmp(ptr, &ID_PREFETCHSTATUS_VAL, 16) == 0) return ITF_PREFETCHSTATUS;
        return -1;
    };

    int res = check_uuid(p);
    if (res >= 0) return res;

    // 1st dereference
    if (is_mapped(iid_ptr)) {
        u32 deref = rd<u32>(iid_ptr);
        if (deref && is_mapped(deref)) {
            res = check_uuid(h(deref));
            if (res >= 0) return res;
            // 2nd dereference
            u32 deref2 = rd<u32>(deref);
            if (deref2 && is_mapped(deref2)) {
                res = check_uuid(h(deref2));
                if (res >= 0) return res;
            }
        }
    }
    return -1;
}

struct HostAudioPlayer {
    u32 host_id = 0;
    u32 guest_obj = 0;
    u32 guest_play_itf = 0;
    u32 guest_bq_itf = 0;
    u32 guest_vol_itf = 0;
    u32 guest_cfg_itf = 0;
    u32 guest_seek_itf = 0;
    u32 guest_rate_itf = 0;
    u32 guest_prefetch_itf = 0;

    SLObjectItf host_player_obj = nullptr;
    SLPlayItf host_play = nullptr;
    SLAndroidSimpleBufferQueueItf host_bq = nullptr;
    SLVolumeItf host_volume = nullptr;

    u32 guest_callback = 0;
    u32 guest_context = 0;
    u32 current_state = SL_PLAYSTATE_STOPPED;
};

static std::mutex g_sl_mu;
static SLObjectItf g_host_engine_obj = nullptr;
static SLEngineItf g_host_engine = nullptr;
static SLObjectItf g_host_output_mix = nullptr;
static u32 g_next_host_id = 1;
static std::unordered_map<u32, std::shared_ptr<HostAudioPlayer>> g_players;

static gaddr s_vtable_object = 0;
static gaddr s_vtable_engine = 0;
static gaddr s_vtable_play = 0;
static gaddr s_vtable_bq = 0;
static gaddr s_vtable_volume = 0;
static gaddr s_vtable_config = 0;
static gaddr s_vtable_seek = 0;
static gaddr s_vtable_rate = 0;
static gaddr s_vtable_prefetch = 0;

static void host_bq_callback(SLAndroidSimpleBufferQueueItf, void* context) {
    auto* player = static_cast<HostAudioPlayer*>(context);
    if (!player) return;
    u32 cb = 0, ctx = 0, guest_bq = 0;
    {
        std::lock_guard lk(g_sl_mu);
        cb = player->guest_callback;
        ctx = player->guest_context;
        guest_bq = player->guest_bq_itf;
    }
    if (cb && guest_bq) {
        call_guest(cb, {ga(guest_bq), ga(ctx)});
    }
}

// ------------------------------------------------------------------ Object Interface
void sl_Object_Realize(Ctx& c) {
    c.a32(); // self
    c.a32(); // async
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Object_Resume(Ctx& c) {
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Object_GetState(Ctx& c) {
    c.a32(); // self
    u32 pState = c.a32();
    if (pState) wr<u32>(pState, SL_OBJECT_STATE_REALIZED);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Object_GetInterface(Ctx& c) {
    u32 self = c.a32();
    u32 iid_ptr = c.a32();
    u32 pItf = c.a32();
    if (!self || !iid_ptr || !pItf) {
        c.ret(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    u32 host_id = rd<u32>(self + 4);
    u32 type = rd<u32>(self + 8);

    int itf_type = match_iid(iid_ptr);
    if (itf_type < 0) {
        NB_LOGW("sl_GetInterface: unrecognized IID at %#x", iid_ptr);
        c.ret(SL_RESULT_FEATURE_UNSUPPORTED);
        return;
    }

    std::lock_guard lk(g_sl_mu);
    if (type == TYPE_ENGINE) {
        if (itf_type == ITF_ENGINE || itf_type == ITF_OBJECT) {
            gaddr gitf = misc_alloc(16, 4);
            wr<u32>(gitf + 0, s_vtable_engine);
            wr<u32>(gitf + 4, host_id);
            wr<u32>(gitf + 8, ITF_ENGINE);
            wr<u32>(pItf, gitf);
            c.ret(SL_RESULT_SUCCESS);
            return;
        }
    } else if (type == TYPE_AUDIO_PLAYER) {
        auto it = g_players.find(host_id);
        if (it != g_players.end()) {
            auto player = it->second;
            if (itf_type == ITF_PLAY) {
                if (!player->guest_play_itf) {
                    player->guest_play_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_play_itf + 0, s_vtable_play);
                    wr<u32>(player->guest_play_itf + 4, host_id);
                    wr<u32>(player->guest_play_itf + 8, ITF_PLAY);
                }
                wr<u32>(pItf, player->guest_play_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_BUFFERQUEUE) {
                if (!player->guest_bq_itf) {
                    player->guest_bq_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_bq_itf + 0, s_vtable_bq);
                    wr<u32>(player->guest_bq_itf + 4, host_id);
                    wr<u32>(player->guest_bq_itf + 8, ITF_BUFFERQUEUE);
                }
                wr<u32>(pItf, player->guest_bq_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_VOLUME) {
                if (!player->guest_vol_itf) {
                    player->guest_vol_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_vol_itf + 0, s_vtable_volume);
                    wr<u32>(player->guest_vol_itf + 4, host_id);
                    wr<u32>(player->guest_vol_itf + 8, ITF_VOLUME);
                }
                wr<u32>(pItf, player->guest_vol_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_ANDROIDCONFIGURATION) {
                if (!player->guest_cfg_itf) {
                    player->guest_cfg_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_cfg_itf + 0, s_vtable_config);
                    wr<u32>(player->guest_cfg_itf + 4, host_id);
                    wr<u32>(player->guest_cfg_itf + 8, ITF_ANDROIDCONFIGURATION);
                }
                wr<u32>(pItf, player->guest_cfg_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_SEEK) {
                if (!player->guest_seek_itf) {
                    player->guest_seek_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_seek_itf + 0, s_vtable_seek);
                    wr<u32>(player->guest_seek_itf + 4, host_id);
                    wr<u32>(player->guest_seek_itf + 8, ITF_SEEK);
                }
                wr<u32>(pItf, player->guest_seek_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_PLAYBACKRATE) {
                if (!player->guest_rate_itf) {
                    player->guest_rate_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_rate_itf + 0, s_vtable_rate);
                    wr<u32>(player->guest_rate_itf + 4, host_id);
                    wr<u32>(player->guest_rate_itf + 8, ITF_PLAYBACKRATE);
                }
                wr<u32>(pItf, player->guest_rate_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_PREFETCHSTATUS) {
                if (!player->guest_prefetch_itf) {
                    player->guest_prefetch_itf = misc_alloc(16, 4);
                    wr<u32>(player->guest_prefetch_itf + 0, s_vtable_prefetch);
                    wr<u32>(player->guest_prefetch_itf + 4, host_id);
                    wr<u32>(player->guest_prefetch_itf + 8, ITF_PREFETCHSTATUS);
                }
                wr<u32>(pItf, player->guest_prefetch_itf);
                c.ret(SL_RESULT_SUCCESS);
                return;
            } else if (itf_type == ITF_OBJECT) {
                wr<u32>(pItf, self);
                c.ret(SL_RESULT_SUCCESS);
                return;
            }
        }
    } else if (type == TYPE_OUTPUT_MIX) {
        if (itf_type == ITF_OBJECT || itf_type == ITF_OUTPUTMIX) {
            wr<u32>(pItf, self);
            c.ret(SL_RESULT_SUCCESS);
            return;
        }
    }
    c.ret(SL_RESULT_FEATURE_UNSUPPORTED);
}

void sl_Object_RegisterCallback(Ctx& c) {
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Object_AbortAsyncOperation(Ctx&) {}

void sl_Object_Destroy(Ctx& c) {
    u32 self = c.a32();
    if (!self) return;
    u32 host_id = rd<u32>(self + 4);
    u32 type = rd<u32>(self + 8);
    std::lock_guard lk(g_sl_mu);
    if (type == TYPE_AUDIO_PLAYER) {
        auto it = g_players.find(host_id);
        if (it != g_players.end()) {
            auto player = it->second;
            if (player->host_player_obj) {
                (*player->host_player_obj)->Destroy(player->host_player_obj);
            }
            g_players.erase(it);
        }
    }
}

void sl_Object_SetPriority(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Object_GetPriority(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Object_SetLossOfControlInterfaces(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }

// ------------------------------------------------------------------ Engine Interface
void sl_Engine_CreateAudioPlayer(Ctx& c) {
    c.a32(); // self
    u32 pPlayer = c.a32();
    u32 pAudioSrc = c.a32();
    c.a32(); // pAudioSnk
    c.a32(); // numInterfaces
    c.a32(); // pInterfaceIds
    c.a32(); // pInterfaceRequired

    u32 numChannels = 2;
    u32 samplesPerSec = 44100000;
    u32 bitsPerSample = 16;

    if (pAudioSrc && is_mapped(pAudioSrc)) {
        gaddr pFormat = rd<u32>(pAudioSrc + 4);
        if (pFormat && is_mapped(pFormat)) {
            u32 formatType = rd<u32>(pFormat);
            if (formatType == SL_DATAFORMAT_PCM /* 2 */) {
                numChannels = rd<u32>(pFormat + 4);
                samplesPerSec = rd<u32>(pFormat + 8);
                bitsPerSample = rd<u32>(pFormat + 12);
            }
        }
    }
    if (numChannels == 0 || numChannels > 8) numChannels = 2;
    if (samplesPerSec == 0) samplesPerSec = 44100000;
    if (samplesPerSec < 1000000) samplesPerSec *= 1000;
    if (bitsPerSample == 0 || (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)) {
        bitsPerSample = 16;
    }

    std::lock_guard lk(g_sl_mu);
    auto player = std::make_shared<HostAudioPlayer>();
    player->host_id = g_next_host_id++;

    if (g_host_engine && g_host_output_mix) {
        gaddr pLocator = (pAudioSrc && is_mapped(pAudioSrc)) ? rd<u32>(pAudioSrc + 0) : 0;
        u32 locatorType = (pLocator && is_mapped(pLocator)) ? rd<u32>(pLocator) : 0;

        SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX, g_host_output_mix };
        SLDataSink audioSnk = { &loc_outmix, nullptr };

        SLresult res = SL_RESULT_UNKNOWN_ERROR;
        if (locatorType == 1 /* SL_DATALOCATOR_URI */) {
            gaddr uri_addr = rd<u32>(pLocator + 4);
            const char* uri_str = uri_addr ? hs(uri_addr) : "";
            SLDataLocator_URI loc_uri = { SL_DATALOCATOR_URI, (SLchar*)uri_str };
            SLDataFormat_MIME mime = { SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED };
            SLDataSource audioSrc = { &loc_uri, &mime };
            const SLInterfaceID ids[2] = { SL_IID_SEEK, SL_IID_VOLUME };
            const SLboolean req[2] = { SL_BOOLEAN_FALSE, SL_BOOLEAN_FALSE };
            res = (*g_host_engine)->CreateAudioPlayer(
                g_host_engine, &player->host_player_obj, &audioSrc, &audioSnk, 2, ids, req
            );
        } else if (locatorType == 0x800007BE /* SL_DATALOCATOR_ANDROIDFD */) {
            SLDataLocator_AndroidFD loc_fd = {
                SL_DATALOCATOR_ANDROIDFD,
                rd<s32>(pLocator + 4),
                static_cast<SLAint64>(rd<u64>(pLocator + 8)),
                static_cast<SLAint64>(rd<u64>(pLocator + 16))
            };
            SLDataFormat_MIME mime = { SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED };
            SLDataSource audioSrc = { &loc_fd, &mime };
            const SLInterfaceID ids[2] = { SL_IID_SEEK, SL_IID_VOLUME };
            const SLboolean req[2] = { SL_BOOLEAN_FALSE, SL_BOOLEAN_FALSE };
            res = (*g_host_engine)->CreateAudioPlayer(
                g_host_engine, &player->host_player_obj, &audioSrc, &audioSnk, 2, ids, req
            );
        } else {
            SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
                SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
                4
            };
            SLDataFormat_PCM pcm = {
                SL_DATAFORMAT_PCM,
                numChannels,
                samplesPerSec,
                bitsPerSample,
                bitsPerSample,
                numChannels == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
                SL_BYTEORDER_LITTLEENDIAN
            };
            SLDataSource audioSrc = { &loc_bq, &pcm };
            const SLInterfaceID ids[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME };
            const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE };
            res = (*g_host_engine)->CreateAudioPlayer(
                g_host_engine, &player->host_player_obj, &audioSrc, &audioSnk, 2, ids, req
            );
        }

        if (res == SL_RESULT_SUCCESS && player->host_player_obj) {
            (*player->host_player_obj)->Realize(player->host_player_obj, SL_BOOLEAN_FALSE);
            (*player->host_player_obj)->GetInterface(player->host_player_obj, SL_IID_PLAY, &player->host_play);
            (*player->host_player_obj)->GetInterface(player->host_player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &player->host_bq);
            (*player->host_player_obj)->GetInterface(player->host_player_obj, SL_IID_VOLUME, &player->host_volume);
            NB_LOGI("Created host OpenSL ES AudioPlayer (rate=%u, ch=%u, bits=%u)", samplesPerSec / 1000, numChannels, bitsPerSample);
        } else {
            NB_LOGE("CreateAudioPlayer failed: res=%u", res);
        }
    }

    gaddr gobj = misc_alloc(16, 4);
    wr<u32>(gobj + 0, s_vtable_object);
    wr<u32>(gobj + 4, player->host_id);
    wr<u32>(gobj + 8, TYPE_AUDIO_PLAYER);
    player->guest_obj = gobj;

    g_players[player->host_id] = player;
    if (pPlayer) wr<u32>(pPlayer, gobj);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Engine_CreateAudioRecorder(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_CreateMidiPlayer(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_CreateListener(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_Create3DEnvironment(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }

void sl_Engine_CreateOutputMix(Ctx& c) {
    c.a32(); // self
    u32 pMix = c.a32();
    u32 host_id = g_next_host_id++;
    gaddr gobj = misc_alloc(16, 4);
    wr<u32>(gobj + 0, s_vtable_object);
    wr<u32>(gobj + 4, host_id);
    wr<u32>(gobj + 8, TYPE_OUTPUT_MIX);
    if (pMix) wr<u32>(pMix, gobj);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Engine_CreateMetadataExtractor(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_CreateExtensionObject(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_QueryNumSupportedEngineInterfaces(Ctx& c) {
    u32 pNum = c.a32();
    if (pNum) wr<u32>(pNum, 1);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Engine_QuerySupportedEngineInterfaces(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Engine_QueryNumSupportedExtensionProperties(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_QuerySupportedExtensionProperties(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }
void sl_Engine_QueryExtensionConfiguration(Ctx& c) { c.ret(SL_RESULT_FEATURE_UNSUPPORTED); }

// ------------------------------------------------------------------ Play Interface
void sl_Play_SetPlayState(Ctx& c) {
    u32 self = c.a32();
    u32 state = c.a32();
    if (!self) {
        c.ret(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    u32 host_id = rd<u32>(self + 4);
    std::shared_ptr<HostAudioPlayer> player;
    {
        std::lock_guard lk(g_sl_mu);
        auto it = g_players.find(host_id);
        if (it != g_players.end()) {
            player = it->second;
            player->current_state = state;
        }
    }
    if (player && player->host_play) {
        (*player->host_play)->SetPlayState(player->host_play, state);
    }
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Play_GetPlayState(Ctx& c) {
    u32 self = c.a32();
    u32 pState = c.a32();
    u32 st = SL_PLAYSTATE_STOPPED;
    if (self) {
        u32 host_id = rd<u32>(self + 4);
        std::lock_guard lk(g_sl_mu);
        auto it = g_players.find(host_id);
        if (it != g_players.end()) st = it->second->current_state;
    }
    if (pState) wr<u32>(pState, st);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Play_GetDuration(Ctx& c) {
    c.a32();
    u32 pMsec = c.a32();
    if (pMsec) wr<u32>(pMsec, SL_TIME_UNKNOWN);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Play_GetPosition(Ctx& c) {
    c.a32();
    u32 pMsec = c.a32();
    if (pMsec) wr<u32>(pMsec, 0);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Play_RegisterCallback(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_SetCallbackEventsMask(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_GetCallbackEventsMask(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_SetMarkerPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_ClearMarkerPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_GetMarkerPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_SetPositionUpdatePeriod(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Play_GetPositionUpdatePeriod(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }

// ------------------------------------------------------------------ BufferQueue Interface
void sl_Bq_Enqueue(Ctx& c) {
    u32 self = c.a32();
    u32 pBuffer = c.a32();
    u32 size = c.a32();
    if (!self || !pBuffer || size == 0) {
        c.ret(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    u32 host_id = rd<u32>(self + 4);
    std::shared_ptr<HostAudioPlayer> player;
    {
        std::lock_guard lk(g_sl_mu);
        auto it = g_players.find(host_id);
        if (it != g_players.end()) player = it->second;
    }
    if (player && player->host_bq) {
        SLresult res = (*player->host_bq)->Enqueue(player->host_bq, h(pBuffer), size);
        c.ret(res);
        return;
    }
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Bq_Clear(Ctx& c) {
    u32 self = c.a32();
    if (self) {
        u32 host_id = rd<u32>(self + 4);
        std::shared_ptr<HostAudioPlayer> player;
        {
            std::lock_guard lk(g_sl_mu);
            auto it = g_players.find(host_id);
            if (it != g_players.end()) player = it->second;
        }
        if (player && player->host_bq) {
            (*player->host_bq)->Clear(player->host_bq);
        }
    }
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Bq_GetState(Ctx& c) {
    u32 self = c.a32();
    u32 pState = c.a32();
    if (self && pState) {
        u32 host_id = rd<u32>(self + 4);
        std::shared_ptr<HostAudioPlayer> player;
        {
            std::lock_guard lk(g_sl_mu);
            auto it = g_players.find(host_id);
            if (it != g_players.end()) player = it->second;
        }
        if (player && player->host_bq) {
            SLAndroidSimpleBufferQueueState st;
            if ((*player->host_bq)->GetState(player->host_bq, &st) == SL_RESULT_SUCCESS) {
                wr<u32>(pState, st.count);
                wr<u32>(pState + 4, st.index);
                c.ret(SL_RESULT_SUCCESS);
                return;
            }
        }
        wr<u32>(pState, 0);
        wr<u32>(pState + 4, 0);
    }
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Bq_RegisterCallback(Ctx& c) {
    u32 self = c.a32();
    u32 callback = c.a32();
    u32 context = c.a32();
    if (!self) {
        c.ret(SL_RESULT_PARAMETER_INVALID);
        return;
    }
    u32 host_id = rd<u32>(self + 4);
    std::shared_ptr<HostAudioPlayer> player;
    {
        std::lock_guard lk(g_sl_mu);
        auto it = g_players.find(host_id);
        if (it != g_players.end()) {
            player = it->second;
            player->guest_callback = callback;
            player->guest_context = context;
        }
    }
    if (player && player->host_bq) {
        (*player->host_bq)->RegisterCallback(player->host_bq, host_bq_callback, player.get());
    }
    c.ret(SL_RESULT_SUCCESS);
}

// ------------------------------------------------------------------ Volume Interface
void sl_Volume_SetVolumeLevel(Ctx& c) {
    u32 self = c.a32();
    SLmillibel level = static_cast<SLmillibel>(c.a32());
    if (self) {
        u32 host_id = rd<u32>(self + 4);
        std::shared_ptr<HostAudioPlayer> player;
        {
            std::lock_guard lk(g_sl_mu);
            auto it = g_players.find(host_id);
            if (it != g_players.end()) player = it->second;
        }
        if (player && player->host_volume) {
            (*player->host_volume)->SetVolumeLevel(player->host_volume, level);
        }
    }
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Volume_GetVolumeLevel(Ctx& c) {
    c.a32();
    u32 pLevel = c.a32();
    if (pLevel) wr<s32>(pLevel, 0);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Volume_GetMaxVolumeLevel(Ctx& c) {
    c.a32();
    u32 pLevel = c.a32();
    if (pLevel) wr<s32>(pLevel, 0);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Volume_SetMute(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Volume_GetMute(Ctx& c) {
    c.a32();
    u32 pMute = c.a32();
    if (pMute) wr<u32>(pMute, 0);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Volume_EnableStereoPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Volume_IsEnabledStereoPosition(Ctx& c) {
    c.a32();
    u32 pEnable = c.a32();
    if (pEnable) wr<u32>(pEnable, 0);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Volume_SetStereoPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Volume_GetStereoPosition(Ctx& c) {
    c.a32();
    u32 pPos = c.a32();
    if (pPos) wr<s32>(pPos, 0);
    c.ret(SL_RESULT_SUCCESS);
}

// ------------------------------------------------------------------ AndroidConfiguration Interface
void sl_Config_SetConfiguration(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Config_GetConfiguration(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }

// ------------------------------------------------------------------ Seek, PlaybackRate, Prefetch Interfaces
void sl_Seek_SetPosition(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Seek_SetLoop(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Seek_GetLoop(Ctx& c) {
    c.a32();
    u32 pEnable = c.a32();
    u32 pStart = c.a32();
    u32 pEnd = c.a32();
    if (pEnable) wr<u32>(pEnable, 0);
    if (pStart) wr<u32>(pStart, 0);
    if (pEnd) wr<u32>(pEnd, 0);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Rate_SetRate(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Rate_GetRate(Ctx& c) {
    c.a32();
    u32 pRate = c.a32();
    if (pRate) wr<s32>(pRate, 1000);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Rate_SetPropertyConstraints(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Rate_GetProperties(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Rate_GetCapabilitiesOfRate(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Rate_GetRateRange(Ctx& c) {
    c.a32(); c.a32();
    u32 pMin = c.a32();
    u32 pMax = c.a32();
    u32 pStep = c.a32();
    u32 pCaps = c.a32();
    if (pMin) wr<s32>(pMin, 500);
    if (pMax) wr<s32>(pMax, 2000);
    if (pStep) wr<s32>(pStep, 0);
    if (pCaps) wr<u32>(pCaps, 0);
    c.ret(SL_RESULT_SUCCESS);
}

void sl_Prefetch_GetPrefetchStatus(Ctx& c) {
    c.a32();
    u32 pStatus = c.a32();
    if (pStatus) wr<u32>(pStatus, 0x03);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Prefetch_GetFillLevel(Ctx& c) {
    c.a32();
    u32 pLevel = c.a32();
    if (pLevel) wr<s32>(pLevel, 1000);
    c.ret(SL_RESULT_SUCCESS);
}
void sl_Prefetch_RegisterCallback(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Prefetch_SetCallbackEventsMask(Ctx& c) { c.ret(SL_RESULT_SUCCESS); }
void sl_Prefetch_GetCallbackEventsMask(Ctx& c) {
    c.a32();
    u32 pMask = c.a32();
    if (pMask) wr<u32>(pMask, 0);
    c.ret(SL_RESULT_SUCCESS);
}

// ------------------------------------------------------------------ Global slCreateEngine
void x_slCreateEngine(Ctx& c) {
    u32 pEngine = c.a32();
    c.a32(); // numOptions
    c.a32(); // pOptions
    c.a32(); // numInterfaces
    c.a32(); // pInterfaceIds
    c.a32(); // pInterfaceRequired

    std::lock_guard lk(g_sl_mu);
    if (!g_host_engine_obj) {
        SLresult res = slCreateEngine(&g_host_engine_obj, 0, nullptr, 0, nullptr, nullptr);
        if (res == SL_RESULT_SUCCESS && g_host_engine_obj) {
            (*g_host_engine_obj)->Realize(g_host_engine_obj, SL_BOOLEAN_FALSE);
            (*g_host_engine_obj)->GetInterface(g_host_engine_obj, SL_IID_ENGINE, &g_host_engine);
            if (g_host_engine) {
                (*g_host_engine)->CreateOutputMix(g_host_engine, &g_host_output_mix, 0, nullptr, nullptr);
                if (g_host_output_mix) {
                    (*g_host_output_mix)->Realize(g_host_output_mix, SL_BOOLEAN_FALSE);
                }
            }
            NB_LOGI("OpenSL ES host engine and output mix initialized successfully");
        } else {
            NB_LOGE("Failed to create host OpenSL ES engine: %u", res);
        }
    }

    u32 host_id = g_next_host_id++;
    gaddr gobj = misc_alloc(16, 4);
    wr<u32>(gobj + 0, s_vtable_object);
    wr<u32>(gobj + 4, host_id);
    wr<u32>(gobj + 8, TYPE_ENGINE);
    wr<u32>(gobj + 12, 0);

    if (pEngine) wr<u32>(pEngine, gobj);
    NB_LOGI("slCreateEngine -> guest obj %#x (host_id %u)", gobj, host_id);
    c.ret(SL_RESULT_SUCCESS);
}

void x_slQueryNumSupportedEngineInterfaces(Ctx& c) {
    u32 pNum = c.a32();
    if (pNum) wr<u32>(pNum, 1);
    c.ret(SL_RESULT_SUCCESS);
}

void x_slQuerySupportedEngineInterfaces(Ctx& c) {
    c.a32(); // index
    u32 pIid = c.a32();
    if (pIid) wr<u32>(pIid, 0);
    c.ret(SL_RESULT_SUCCESS);
}

}  // namespace

void register_opensles() {
    // Build permanent vtables
    s_vtable_object = misc_alloc(10 * 4, 4);
    wr<u32>(s_vtable_object + 0 * 4, make_stub("sl_Object_Realize", sl_Object_Realize));
    wr<u32>(s_vtable_object + 1 * 4, make_stub("sl_Object_Resume", sl_Object_Resume));
    wr<u32>(s_vtable_object + 2 * 4, make_stub("sl_Object_GetState", sl_Object_GetState));
    wr<u32>(s_vtable_object + 3 * 4, make_stub("sl_Object_GetInterface", sl_Object_GetInterface));
    wr<u32>(s_vtable_object + 4 * 4, make_stub("sl_Object_RegisterCallback", sl_Object_RegisterCallback));
    wr<u32>(s_vtable_object + 5 * 4, make_stub("sl_Object_AbortAsyncOperation", sl_Object_AbortAsyncOperation));
    wr<u32>(s_vtable_object + 6 * 4, make_stub("sl_Object_Destroy", sl_Object_Destroy));
    wr<u32>(s_vtable_object + 7 * 4, make_stub("sl_Object_SetPriority", sl_Object_SetPriority));
    wr<u32>(s_vtable_object + 8 * 4, make_stub("sl_Object_GetPriority", sl_Object_GetPriority));
    wr<u32>(s_vtable_object + 9 * 4, make_stub("sl_Object_SetLossOfControlInterfaces", sl_Object_SetLossOfControlInterfaces));

    s_vtable_engine = misc_alloc(15 * 4, 4);
    wr<u32>(s_vtable_engine + 0 * 4, make_stub("sl_Engine_CreateLEDDevice", sl_Engine_CreateAudioRecorder));
    wr<u32>(s_vtable_engine + 1 * 4, make_stub("sl_Engine_CreateVibraDevice", sl_Engine_CreateAudioRecorder));
    wr<u32>(s_vtable_engine + 2 * 4, make_stub("sl_Engine_CreateAudioPlayer", sl_Engine_CreateAudioPlayer));
    wr<u32>(s_vtable_engine + 3 * 4, make_stub("sl_Engine_CreateAudioRecorder", sl_Engine_CreateAudioRecorder));
    wr<u32>(s_vtable_engine + 4 * 4, make_stub("sl_Engine_CreateMidiPlayer", sl_Engine_CreateMidiPlayer));
    wr<u32>(s_vtable_engine + 5 * 4, make_stub("sl_Engine_CreateListener", sl_Engine_CreateListener));
    wr<u32>(s_vtable_engine + 6 * 4, make_stub("sl_Engine_Create3DEnvironment", sl_Engine_Create3DEnvironment));
    wr<u32>(s_vtable_engine + 7 * 4, make_stub("sl_Engine_CreateOutputMix", sl_Engine_CreateOutputMix));
    wr<u32>(s_vtable_engine + 8 * 4, make_stub("sl_Engine_CreateMetadataExtractor", sl_Engine_CreateMetadataExtractor));
    wr<u32>(s_vtable_engine + 9 * 4, make_stub("sl_Engine_CreateExtensionObject", sl_Engine_CreateExtensionObject));
    wr<u32>(s_vtable_engine + 10 * 4, make_stub("sl_Engine_QueryNumSupportedEngineInterfaces", sl_Engine_QueryNumSupportedEngineInterfaces));
    wr<u32>(s_vtable_engine + 11 * 4, make_stub("sl_Engine_QuerySupportedEngineInterfaces", sl_Engine_QuerySupportedEngineInterfaces));
    wr<u32>(s_vtable_engine + 12 * 4, make_stub("sl_Engine_QueryNumSupportedExtensionProperties", sl_Engine_QueryNumSupportedExtensionProperties));
    wr<u32>(s_vtable_engine + 13 * 4, make_stub("sl_Engine_QuerySupportedExtensionProperties", sl_Engine_QuerySupportedExtensionProperties));
    wr<u32>(s_vtable_engine + 14 * 4, make_stub("sl_Engine_QueryExtensionConfiguration", sl_Engine_QueryExtensionConfiguration));

    s_vtable_play = misc_alloc(12 * 4, 4);
    wr<u32>(s_vtable_play + 0 * 4, make_stub("sl_Play_SetPlayState", sl_Play_SetPlayState));
    wr<u32>(s_vtable_play + 1 * 4, make_stub("sl_Play_GetPlayState", sl_Play_GetPlayState));
    wr<u32>(s_vtable_play + 2 * 4, make_stub("sl_Play_GetDuration", sl_Play_GetDuration));
    wr<u32>(s_vtable_play + 3 * 4, make_stub("sl_Play_GetPosition", sl_Play_GetPosition));
    wr<u32>(s_vtable_play + 4 * 4, make_stub("sl_Play_RegisterCallback", sl_Play_RegisterCallback));
    wr<u32>(s_vtable_play + 5 * 4, make_stub("sl_Play_SetCallbackEventsMask", sl_Play_SetCallbackEventsMask));
    wr<u32>(s_vtable_play + 6 * 4, make_stub("sl_Play_GetCallbackEventsMask", sl_Play_GetCallbackEventsMask));
    wr<u32>(s_vtable_play + 7 * 4, make_stub("sl_Play_SetMarkerPosition", sl_Play_SetMarkerPosition));
    wr<u32>(s_vtable_play + 8 * 4, make_stub("sl_Play_ClearMarkerPosition", sl_Play_ClearMarkerPosition));
    wr<u32>(s_vtable_play + 9 * 4, make_stub("sl_Play_GetMarkerPosition", sl_Play_GetMarkerPosition));
    wr<u32>(s_vtable_play + 10 * 4, make_stub("sl_Play_SetPositionUpdatePeriod", sl_Play_SetPositionUpdatePeriod));
    wr<u32>(s_vtable_play + 11 * 4, make_stub("sl_Play_GetPositionUpdatePeriod", sl_Play_GetPositionUpdatePeriod));

    s_vtable_bq = misc_alloc(4 * 4, 4);
    wr<u32>(s_vtable_bq + 0 * 4, make_stub("sl_Bq_Enqueue", sl_Bq_Enqueue));
    wr<u32>(s_vtable_bq + 1 * 4, make_stub("sl_Bq_Clear", sl_Bq_Clear));
    wr<u32>(s_vtable_bq + 2 * 4, make_stub("sl_Bq_GetState", sl_Bq_GetState));
    wr<u32>(s_vtable_bq + 3 * 4, make_stub("sl_Bq_RegisterCallback", sl_Bq_RegisterCallback));

    s_vtable_volume = misc_alloc(9 * 4, 4);
    wr<u32>(s_vtable_volume + 0 * 4, make_stub("sl_Volume_SetVolumeLevel", sl_Volume_SetVolumeLevel));
    wr<u32>(s_vtable_volume + 1 * 4, make_stub("sl_Volume_GetVolumeLevel", sl_Volume_GetVolumeLevel));
    wr<u32>(s_vtable_volume + 2 * 4, make_stub("sl_Volume_GetMaxVolumeLevel", sl_Volume_GetMaxVolumeLevel));
    wr<u32>(s_vtable_volume + 3 * 4, make_stub("sl_Volume_SetMute", sl_Volume_SetMute));
    wr<u32>(s_vtable_volume + 4 * 4, make_stub("sl_Volume_GetMute", sl_Volume_GetMute));
    wr<u32>(s_vtable_volume + 5 * 4, make_stub("sl_Volume_EnableStereoPosition", sl_Volume_EnableStereoPosition));
    wr<u32>(s_vtable_volume + 6 * 4, make_stub("sl_Volume_IsEnabledStereoPosition", sl_Volume_IsEnabledStereoPosition));
    wr<u32>(s_vtable_volume + 7 * 4, make_stub("sl_Volume_SetStereoPosition", sl_Volume_SetStereoPosition));
    wr<u32>(s_vtable_volume + 8 * 4, make_stub("sl_Volume_GetStereoPosition", sl_Volume_GetStereoPosition));

    s_vtable_config = misc_alloc(2 * 4, 4);
    wr<u32>(s_vtable_config + 0 * 4, make_stub("sl_Config_SetConfiguration", sl_Config_SetConfiguration));
    wr<u32>(s_vtable_config + 1 * 4, make_stub("sl_Config_GetConfiguration", sl_Config_GetConfiguration));

    s_vtable_seek = misc_alloc(3 * 4, 4);
    wr<u32>(s_vtable_seek + 0 * 4, make_stub("sl_Seek_SetPosition", sl_Seek_SetPosition));
    wr<u32>(s_vtable_seek + 1 * 4, make_stub("sl_Seek_SetLoop", sl_Seek_SetLoop));
    wr<u32>(s_vtable_seek + 2 * 4, make_stub("sl_Seek_GetLoop", sl_Seek_GetLoop));

    s_vtable_rate = misc_alloc(6 * 4, 4);
    wr<u32>(s_vtable_rate + 0 * 4, make_stub("sl_Rate_SetRate", sl_Rate_SetRate));
    wr<u32>(s_vtable_rate + 1 * 4, make_stub("sl_Rate_GetRate", sl_Rate_GetRate));
    wr<u32>(s_vtable_rate + 2 * 4, make_stub("sl_Rate_SetPropertyConstraints", sl_Rate_SetPropertyConstraints));
    wr<u32>(s_vtable_rate + 3 * 4, make_stub("sl_Rate_GetProperties", sl_Rate_GetProperties));
    wr<u32>(s_vtable_rate + 4 * 4, make_stub("sl_Rate_GetCapabilitiesOfRate", sl_Rate_GetCapabilitiesOfRate));
    wr<u32>(s_vtable_rate + 5 * 4, make_stub("sl_Rate_GetRateRange", sl_Rate_GetRateRange));

    s_vtable_prefetch = misc_alloc(5 * 4, 4);
    wr<u32>(s_vtable_prefetch + 0 * 4, make_stub("sl_Prefetch_GetPrefetchStatus", sl_Prefetch_GetPrefetchStatus));
    wr<u32>(s_vtable_prefetch + 1 * 4, make_stub("sl_Prefetch_GetFillLevel", sl_Prefetch_GetFillLevel));
    wr<u32>(s_vtable_prefetch + 2 * 4, make_stub("sl_Prefetch_RegisterCallback", sl_Prefetch_RegisterCallback));
    wr<u32>(s_vtable_prefetch + 3 * 4, make_stub("sl_Prefetch_SetCallbackEventsMask", sl_Prefetch_SetCallbackEventsMask));
    wr<u32>(s_vtable_prefetch + 4 * 4, make_stub("sl_Prefetch_GetCallbackEventsMask", sl_Prefetch_GetCallbackEventsMask));

    // Exported function registrations
    reg("slCreateEngine", x_slCreateEngine);
    reg("slQueryNumSupportedEngineInterfaces", x_slQueryNumSupportedEngineInterfaces);
    reg("slQuerySupportedEngineInterfaces", x_slQuerySupportedEngineInterfaces);
}

void init_opensles_iids() {
    struct IidDef {
        const char* name;
        SLInterfaceID_ uuid;
    };
    static const IidDef kDefs[] = {
        {"SL_IID_ENGINE", ID_ENGINE_VAL},
        {"SL_IID_PLAY", ID_PLAY_VAL},
        {"SL_IID_BUFFERQUEUE", ID_BQ_VAL},
        {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", ID_ANDROIDSIMPLEBQ_VAL},
        {"SL_IID_VOLUME", ID_VOLUME_VAL},
        {"SL_IID_ANDROIDCONFIGURATION", ID_CONFIG_VAL},
        {"SL_IID_OBJECT", ID_OBJECT_VAL},
        {"SL_IID_OUTPUTMIX", ID_OUTPUTMIX_VAL},
        {"SL_IID_SEEK", ID_SEEK_VAL},
        {"SL_IID_PLAYBACKRATE", ID_PLAYBACKRATE_VAL},
        {"SL_IID_PREFETCHSTATUS", ID_PREFETCHSTATUS_VAL},
    };

    for (const auto& def : kDefs) {
        gaddr struct_addr = misc_alloc(16, 4);
        std::memcpy(h(struct_addr), &def.uuid, 16);
        u32 sym_addr = guest_dlsym(0, def.name);
        if (sym_addr && is_mapped(sym_addr)) {
            wr<u32>(sym_addr, struct_addr);
            NB_LOGI("Initialized OpenSL ES IID %s at %#x -> struct %#x", def.name, sym_addr, struct_addr);
        }
    }
}

}  // namespace nb
