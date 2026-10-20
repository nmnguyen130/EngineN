#pragma once
#include "SDL_common.h"
struct SDL_AudioStream { int dummy{0}; };
using SDL_AudioFormat = int;
inline constexpr SDL_AudioFormat SDL_AUDIO_F32 = 0x8120;
inline constexpr unsigned int SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFFu;
struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
};
using SDL_AudioStreamCallback = void (*)(void*, SDL_AudioStream*, int, int);
inline SDL_AudioStream* SDL_OpenAudioDeviceStream(unsigned int, const SDL_AudioSpec*, SDL_AudioStreamCallback, void*) noexcept {
    return new SDL_AudioStream{};
}
inline bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*) noexcept { return true; }
inline void SDL_DestroyAudioStream(SDL_AudioStream* s) noexcept { delete s; }
inline bool SDL_PutAudioStreamData(SDL_AudioStream*, const void*, int) noexcept { return true; }
