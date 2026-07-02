#pragma once

#include <cstdint>

/*!
 * @file mumble_voice.h
 * Voice engine for the native Mumble client (phase 2).
 *
 * Capture: cubeb input stream (48 kHz mono) -> Opus encode (20 ms frames)
 *   -> mumble_native_send_voice (legacy packets over the TCP tunnel).
 * Playback: incoming opus frames -> per-peer decoder + jitter buffer
 *   -> positional mixer (distance attenuation + stereo pan relative to the
 *   game camera) -> cubeb output stream (48 kHz stereo).
 *
 * We do the spatialization ourselves, so none of Mumble's client audio
 * settings are needed - the ImGui sliders here are the whole config.
 */

// editable from the ImGui Mumble menu
struct MumbleVoiceConfig {
  bool transmit = true;        // master mic switch
  float mic_gate = 0.01f;      // RMS below this doesn't transmit (crude VAD)
  float volume = 1.0f;         // master receive volume
  bool positional = true;      // attenuate/pan by in-game distance
  float min_distance_m = 3.f;  // full volume within this (Mumble meters)
  float max_distance_m = 50.f; // silent beyond this
  // cubeb device_id strings; empty = system default. Changing these while
  // voice is running restarts the streams on the new devices.
  char input_device_id[256] = "";
  char output_device_id[256] = "";
};

struct MumbleVoiceDeviceInfo {
  char id[256];    // stable identifier, stored in the config
  char name[128];  // friendly name for the UI
};

// List available capture (input=true) or playback devices. Returns the count
// written to `out` (up to max_count). Safe to call whether or not voice is
// running.
int mumble_voice_enum_devices(bool input, MumbleVoiceDeviceInfo* out, int max_count);

struct MumbleVoiceStatus {
  bool capture_running = false;
  bool playback_running = false;
  float mic_level = 0.f;      // smoothed RMS 0..1
  bool transmitting = false;  // currently past the gate and sending
  int talking_peers = 0;
  uint32_t frames_sent = 0;
  uint32_t frames_received = 0;
  char message[128] = "voice idle";
};

extern MumbleVoiceConfig g_mumble_voice_config;

MumbleVoiceStatus mumble_voice_get_status();

// Call once per frame from the game thread (mumble_link_update). Starts the
// audio streams when the native client connects and stops them when it
// disconnects. Also refreshes the transforms used by the mixer and the
// transmitter: avatar + camera in Mumble meters (post scale + mirror).
void mumble_voice_maintain(const float avatar_pos[3],
                           const float cam_pos[3],
                           const float cam_front[3],
                           const float cam_top[3]);
