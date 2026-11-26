// Sound: the game's Vox engine mixes everything in software and hands the result to an
// Android output. Its AudioTrack backend (Java android.media.AudioTrack through JNI) is
// emulated here on SDL3 audio streams. Vox picks that backend when its Android API level is
// 8 or lower (set right after JNI_OnLoad).
#pragma once

#include <cstdint>
#include <string>

namespace audio {

// After jni::init(): register the android.media.AudioTrack methods. SDL audio must be initialised.
void register_java();
// After JNI_OnLoad: make Vox use the AudioTrack backend.
void select_backend();
// Master volume, 0..1 (applies to current and future output streams).
void set_volume(float gain);
#if !defined(__SWITCH__) && !defined(__3DS__) && !defined(__GAMECUBE__)
// Before the game opens its sound: the playback device to use, by name ("": the system's
// default output; not connected: the default too). Split screen: [launcher] shared_audio / own_audio.
void set_output_device(const std::string &name);
#endif
#ifdef __3DS__
// Before the process ends: CSND's channels (the output without the DSP firmware) would go on
// looping their last sound.
void stop_output();
// Hang reports: how many times ndsp's feeder thread has woken (once per DSP frame, ~5 ms).
uint32_t feeder_wakes();
#endif

}  // namespace audio
