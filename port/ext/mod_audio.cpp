#include "mod_audio.h"

#include "mod_audio_mix.h"

#include <kf/audio/codec.h>
#include <kf/audio/sound.h>
#include <kf/game/audio.h>
#include <kf/lib/audio.h>
#include <kf/lib/sound_types.h>
#include <lauxlib.h>
#include <lua.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace kf::ext {
namespace {

// Mixed on the main thread, from the game's sound calls; the mods' sounds are only touched there.
ModAudioMixer mixer;

constexpr lua_Integer effect_max_program = KF_AUDIO_PROGRAM_COUNT - 1;
constexpr lua_Integer effect_max_tone = KF_SOUND_TONE_INDEX_MASK;
constexpr lua_Integer effect_max_note = 127;
constexpr lua_Integer effect_max_volume = KF_AUDIO_MAX_VOLUME;
constexpr lua_Integer music_max_index = 2;

// Mixes what has elapsed before a voice starts or stops, so the change is heard from now on.
void settle() {
    kf::sound_poll();
}

lua_Integer check_range(lua_State *L, int index, lua_Integer low, lua_Integer high, const char *name) {
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < low || value > high)
        luaL_error(L, "%s %I is not %I to %I", name, value, low, high);
    return value;
}

lua_Integer optional_range(lua_State *L, int index, lua_Integer fallback, lua_Integer low, lua_Integer high,
                           const char *name) {
    return lua_isnoneornil(L, index) ? fallback : check_range(L, index, low, high, name);
}

s32 check_position_part(lua_State *L, int index) {
    const lua_Integer value = luaL_checkinteger(L, index);
    if (!std::in_range<s32>(value))
        luaL_error(L, "position %I is not a 32-bit integer", value);
    return static_cast<s32>(value);
}

// A game sound from Lua's program, tone and note. The tone's flag bits are never set from Lua, so
// the game's pan-narrowing flag stays off.
SoundRef check_effect_sound(lua_State *L, int index) {
    const auto program = check_range(L, index, 0, effect_max_program, "program");
    const auto tone = check_range(L, index + 1, 0, effect_max_tone, "tone");
    const auto note = check_range(L, index + 2, 0, effect_max_note, "note");
    return {static_cast<u8>(program), static_cast<u8>(tone), static_cast<u8>(note)};
}

int audio_play_effect(lua_State *L) {
    const SoundRef sound = check_effect_sound(L, 1);
    const auto volume = optional_range(L, 4, effect_max_volume, 0, effect_max_volume, "volume");
    sound_ref_play(audio_playback(), &sound, static_cast<s16>(volume));
    return 0;
}

// The game takes the listener's distance in 32-bit arithmetic, which is undefined past its range.
// A position 16000 or more from the listener on any axis is not heard anyway, so it is not played;
// the difference is taken in 64 bits and nothing reaches the game that could overflow.
bool within_hearing(const VECTOR &at, const VECTOR &listener) {
    const std::int64_t reach = KF_AUDIO_DEFAULT_MAX_DISTANCE;
    const auto near = [reach](std::int64_t from, std::int64_t to) {
        return std::abs(from - to) < reach;
    };
    return near(at.vx, listener.vx) && near(at.vy, listener.vy) && near(at.vz, listener.vz);
}

int audio_play_effect_at(lua_State *L) {
    const VECTOR at{check_position_part(L, 1), check_position_part(L, 2), check_position_part(L, 3)};
    const SoundRef sound = check_effect_sound(L, 4);
    const auto volume = optional_range(L, 7, effect_max_volume, 0, effect_max_volume, "volume");
    if (within_hearing(at, audio_state.listener_position))
        audio_play_spatial_default_range(&sound, &at, static_cast<s16>(volume));
    return 0;
}

// The game's fade waits on frames, and mods draw during the wait, so the sequence may not change then.
void refuse_during_fade(lua_State *L) {
    if (audio_sequence_is_fading())
        luaL_error(L, "the music is fading out: try again when it has stopped");
}

int audio_play_music(lua_State *L) {
    refuse_during_fade(L);
    const auto index = check_range(L, 1, 0, music_max_index, "music index");
    // The game plays the floor's sequence only when a sound bank is loaded: with none, it halts.
    if (!audio_state.bank)
        luaL_error(L, "no sound bank is loaded: music plays on a floor, not on the title or the opening");
    // The game fades out the sequence it has playing over many frames, which would run the mods' draw
    // callbacks inside this call, so the sequence is released at once instead.
    if (audio_state.sequence_active == KF_AUDIO_SEQUENCE_ACTIVE)
        audio_release_sequence(audio_state);
    audio_play_map_sequence(static_cast<u8>(index));
    return 0;
}

int audio_stop_music(lua_State *L) {
    refuse_during_fade(L);
    if (audio_state.sequence_active == KF_AUDIO_SEQUENCE_ACTIVE)
        audio_release_sequence(audio_state);
    return 0;
}

// The mod's sound ids and voices are integers; a voice is its generation above its slot.
lua_Integer voice_to_lua(ModVoice voice) {
    return static_cast<lua_Integer>((static_cast<std::uint64_t>(voice.generation) << 32) | voice.slot);
}

ModVoice voice_from_lua(lua_Integer value) {
    const auto bits = static_cast<std::uint64_t>(value);
    return {static_cast<std::uint32_t>(bits & 0xffffffffu), static_cast<std::uint32_t>(bits >> 32)};
}

enum class LoadRefusal : std::uint8_t { None, Unreadable, NotWav, OverCap };
struct Loaded {
    LoadRefusal refusal;
    std::uint32_t id;
};

// Kept apart from the Lua call so its buffers are gone before any luaL_error.
Loaded load_sound(lua_State *L, std::string_view file) {
    const auto bytes = verdite::mod_file_bytes(L, file, mod_audio_sound_bytes_cap);
    if (!bytes)
        return {LoadRefusal::Unreadable, 0};
    auto pcm = verdite::wav_decode(*bytes);
    if (!pcm)
        return {LoadRefusal::NotWav, 0};
    const auto id = mixer.add_sound(verdite::mod_id(L), std::make_shared<const verdite::Pcm>(std::move(*pcm)));
    if (!id)
        return {LoadRefusal::OverCap, 0};
    return {LoadRefusal::None, *id};
}

int audio_load(lua_State *L) {
    const char *file = luaL_checkstring(L, 1);
    const Loaded loaded = load_sound(L, file);
    if (loaded.refusal == LoadRefusal::Unreadable)
        luaL_error(L, "cannot read '%s': it is missing, not a file, outside the mod's folder, or over 64 MB", file);
    if (loaded.refusal == LoadRefusal::NotWav)
        luaL_error(L, "'%s' is not a WAV the game can play: PCM at 8 or 16 bits, 1 or 2 channels, 8000 to 96000 Hz",
                   file);
    if (loaded.refusal == LoadRefusal::OverCap)
        luaL_error(L, "'%s' would take the mod over its 64 MB of sounds", file);
    lua_pushinteger(L, static_cast<lua_Integer>(loaded.id));
    return 1;
}

// A number option in low..high; nothing or nil takes the fallback.
float option_number(lua_State *L, int index, const char *key, float low, float high, float fallback) {
    lua_getfield(L, index, key);
    float value = fallback;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER)
            luaL_error(L, "option %s is not a number", key);
        const double number = lua_tonumber(L, -1);
        if (!std::isfinite(number) || number < low || number > high)
            luaL_error(L, "option %s must be a finite number from %f to %f", key, static_cast<double>(low),
                       static_cast<double>(high));
        value = static_cast<float>(number);
    }
    lua_pop(L, 1);
    return value;
}

bool option_boolean(lua_State *L, int index, const char *key, bool fallback) {
    lua_getfield(L, index, key);
    bool value = fallback;
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            luaL_error(L, "option %s is not a boolean", key);
        value = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    return value;
}

ModVoiceOptions check_voice_options(lua_State *L, int index) {
    ModVoiceOptions options;
    if (lua_isnoneornil(L, index))
        return options;
    luaL_checktype(L, index, LUA_TTABLE);
    options.volume = option_number(L, index, "volume", 0.0f, 1.0f, options.volume);
    options.pan = option_number(L, index, "pan", -1.0f, 1.0f, options.pan);
    options.loop = option_boolean(L, index, "loop", options.loop);
    return options;
}

int audio_play(lua_State *L) {
    const lua_Integer id = luaL_checkinteger(L, 1);
    const ModVoiceOptions options = check_voice_options(L, 2);
    const std::string_view mod = verdite::mod_id(L);
    const bool known = id >= 0 && std::in_range<std::uint32_t>(id) &&
                       mixer.has_sound(mod, static_cast<std::uint32_t>(id));
    if (!known)
        luaL_error(L, "sound %I is not one of this mod's sounds", id);
    settle();
    const auto voice = mixer.play(mod, static_cast<std::uint32_t>(id), options);
    if (!voice) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, voice_to_lua(*voice));
    return 1;
}

int audio_stop(lua_State *L) {
    const ModVoice voice = voice_from_lua(luaL_checkinteger(L, 1));
    settle();
    mixer.stop(voice);
    return 0;
}

int audio_playing(lua_State *L) {
    const ModVoice voice = voice_from_lua(luaL_checkinteger(L, 1));
    settle();
    lua_pushboolean(L, mixer.playing(voice));
    return 1;
}

} // namespace

std::vector<verdite::ModFunction> mod_audio_functions() {
    using verdite::ModFunction;
    return {
        {"play_effect", "fun(program: integer, tone: integer, note: integer, volume?: integer)",
         "Plays a game sound effect, from the sound bank (program 0 to 127, tone 0 to 15, note 0 to 127, "
         "volume 0 to 127). Nothing while no bank is loaded.",
         audio_play_effect},
        {"play_effect_at", "fun(x: integer, y: integer, z: integer, program: integer, tone: integer, note: integer, volume?: integer)",
         "As play_effect, placed at a world position: the game's distance and pan from the listener.",
         audio_play_effect_at},
        {"play_music", "fun(index: integer)",
         "Plays the current floor's music, SND<index>.SEQ (index 0 to 2), in place of what plays. A Lua error "
         "while no sound bank is loaded.",
         audio_play_music},
        {"stop_music", "fun()", "Stops the music that is playing.", audio_stop_music},
        {"load", "fun(file: string): integer",
         "Loads a WAV file from the mod's folder (PCM at 8 or 16 bits, 1 or 2 channels, 8000 to 96000 Hz) and "
         "returns its sound id. A Lua error with the reason if it cannot.",
         audio_load},
        {"play", "fun(sound: integer, options?: {volume?: number, pan?: number, loop?: boolean}): integer?",
         "Plays one of the mod's sounds on a voice, and returns the voice; nil when all 16 voices are busy. "
         "volume 0 to 1, pan -1 (left) to 1 (right), loop. Voices stop while mods are off or the mod is held.",
         audio_play},
        {"stop", "fun(voice: integer)", "Stops a voice. A voice that has ended does nothing.", audio_stop},
        {"playing", "fun(voice: integer): boolean", "Whether a voice is still playing.", audio_playing},
    };
}

void mod_audio_mix_frame(float &left, float &right) {
    mixer.mix(left, right);
}

void mod_audio_stop_mod(std::string_view id) {
    settle();
    mixer.stop_mod(id);
}

void mod_audio_stop_all() {
    settle();
    mixer.stop_all();
}

bool mod_audio_any_playing() {
    return mixer.any_playing();
}

void mod_audio_forget(std::string_view id) {
    settle();
    mixer.forget(id);
}

} // namespace kf::ext
