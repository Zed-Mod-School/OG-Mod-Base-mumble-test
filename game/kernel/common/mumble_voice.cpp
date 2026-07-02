/*!
 * @file mumble_voice.cpp
 * Voice engine for the native Mumble client. See mumble_voice.h.
 */

#include "mumble_voice.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "game/kernel/common/mumble_config.h"
#include "game/kernel/common/mumble_native.h"

#include "opus.h"
#include "third-party/cubeb/cubeb/include/cubeb/cubeb.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace {

// cubeb's WASAPI backend requires COM on the calling thread; harmless to call
// repeatedly (S_FALSE) or if another apartment mode is already set.
void ensure_com_initialized() {
#ifdef _WIN32
  thread_local bool done = false;
  if (!done) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    done = true;
  }
#endif
}

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;  // 20 ms at 48 kHz
constexpr int kMaxDecodedSamples = 5760;
constexpr size_t kPeerBufferCap = kSampleRate / 2;  // 500 ms of backlog max
// keep transmitting briefly after the gate closes so words don't clip
constexpr int kGateHangoverFrames = 15;  // 300 ms

struct PeerAudio {
  OpusDecoder* decoder = nullptr;
  std::deque<float> pcm;  // decoded mono samples waiting to be mixed
  float pos[3] = {};      // Mumble meters
  bool has_pos = false;
  char name[32] = "";  // resolved lazily from the native client's user table
};

std::mutex g_mutex;  // guards everything below (brief holds only)
MumbleVoiceStatus g_status;
std::unordered_map<uint32_t, PeerAudio> g_peers;
std::unordered_map<std::string, float> g_user_volumes;  // username -> multiplier

// listener + transmitter transforms, refreshed by the game thread
float g_avatar_pos[3] = {};
float g_cam_pos[3] = {};
float g_cam_front[3] = {0, 0, 1};
float g_cam_top[3] = {0, 1, 0};

// capture state (only touched by the input callback thread)
OpusEncoder* g_encoder = nullptr;
std::vector<float> g_capture_accum;
uint32_t g_voice_sequence = 0;
int g_gate_hangover = 0;

cubeb* g_ctx = nullptr;
cubeb_stream* g_out_stream = nullptr;
cubeb_stream* g_in_stream = nullptr;
bool g_started = false;
// device ids the running streams were opened with, to detect config changes
char g_applied_input_id[256] = "";
char g_applied_output_id[256] = "";

// find the devid for a configured device_id string; null (default) if empty
// or no longer present
cubeb_devid resolve_device(cubeb* ctx, bool input, const char* wanted_id) {
  if (!wanted_id[0]) {
    return nullptr;
  }
  cubeb_device_collection coll = {};
  if (cubeb_enumerate_devices(ctx, input ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT,
                              &coll) != CUBEB_OK) {
    return nullptr;
  }
  cubeb_devid found = nullptr;
  for (size_t i = 0; i < coll.count; i++) {
    const auto& d = coll.device[i];
    if (d.device_id && strcmp(d.device_id, wanted_id) == 0 &&
        d.state == CUBEB_DEVICE_STATE_ENABLED) {
      found = d.devid;
      break;
    }
  }
  cubeb_device_collection_destroy(ctx, &coll);
  return found;
}

MumbleVoiceConfig snapshot_config() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mumble_voice_config;
}

// ---------------- receive path ----------------

void voice_rx(uint32_t session, uint32_t /*seq*/, const uint8_t* opus, size_t len,
              const float* pos) {
  // called on the native client thread
  float decoded[kMaxDecodedSamples];
  // resolve the sender's name before taking our own lock (native has its own)
  char name[32] = "";
  mumble_native_get_user_name(session, name, sizeof(name));

  std::lock_guard<std::mutex> lock(g_mutex);
  auto& peer = g_peers[session];
  if (name[0]) {
    memcpy(peer.name, name, sizeof(peer.name));
  }
  if (!peer.decoder) {
    int err = 0;
    peer.decoder = opus_decoder_create(kSampleRate, 1, &err);
    if (err != OPUS_OK) {
      g_peers.erase(session);
      return;
    }
  }
  int n = opus_decode_float(peer.decoder, opus, (opus_int32)len, decoded, kMaxDecodedSamples, 0);
  if (n <= 0) {
    return;
  }
  if (pos) {
    memcpy(peer.pos, pos, sizeof(peer.pos));
    peer.has_pos = true;
  }
  if (peer.pcm.size() + n > kPeerBufferCap) {
    // peer is outrunning playback (or we were paused) - drop the backlog
    peer.pcm.clear();
  }
  peer.pcm.insert(peer.pcm.end(), decoded, decoded + n);
  g_status.frames_received++;
}

// distance attenuation + equal-power stereo pan for one peer
void compute_gain_pan(const PeerAudio& peer, const MumbleVoiceConfig& cfg, float* gain_l,
                      float* gain_r) {
  float gain = cfg.volume;
  // per-user adjustment (caller holds g_mutex)
  if (peer.name[0]) {
    auto it = g_user_volumes.find(peer.name);
    if (it != g_user_volumes.end()) {
      gain *= it->second;
    }
  }
  float pan = 0.f;
  if (cfg.positional && peer.has_pos) {
    float rel[3] = {peer.pos[0] - g_cam_pos[0], peer.pos[1] - g_cam_pos[1],
                    peer.pos[2] - g_cam_pos[2]};
    float dist = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    if (dist > cfg.max_distance_m || cfg.max_distance_m <= cfg.min_distance_m) {
      if (dist > cfg.min_distance_m) {
        *gain_l = *gain_r = 0.f;
        return;
      }
    }
    if (dist > cfg.min_distance_m) {
      // falloff curve from min to max distance; exponent = aggression
      float t = (dist - cfg.min_distance_m) / (cfg.max_distance_m - cfg.min_distance_m);
      gain *= powf(1.f - t, cfg.falloff < 0.1f ? 0.1f : cfg.falloff);
    }
    if (dist > 0.01f) {
      // right axis = front x top (matches Mumble's left-handed convention)
      float right[3] = {
          g_cam_front[1] * g_cam_top[2] - g_cam_front[2] * g_cam_top[1],
          g_cam_front[2] * g_cam_top[0] - g_cam_front[0] * g_cam_top[2],
          g_cam_front[0] * g_cam_top[1] - g_cam_front[1] * g_cam_top[0],
      };
      float rlen = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
      if (rlen > 0.001f) {
        pan = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / (rlen * dist);
      }
    }
  }
  // equal-power panning, pan in [-1 left .. 1 right]
  float angle = (pan * 0.5f + 0.5f) * 1.5707963f;  // 0..pi/2
  *gain_l = gain * cosf(angle);
  *gain_r = gain * sinf(angle);
}

long output_cb(cubeb_stream* /*stream*/, void* /*user*/, const void* /*input*/, void* output,
               long nframes) {
  float* out = (float*)output;
  memset(out, 0, sizeof(float) * 2 * nframes);

  MumbleVoiceConfig cfg;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    cfg = g_mumble_voice_config;
    int talking = 0;
    for (auto& [session, peer] : g_peers) {
      if (peer.pcm.empty()) {
        continue;
      }
      talking++;
      float gl, gr;
      compute_gain_pan(peer, cfg, &gl, &gr);
      long n = (long)peer.pcm.size() < nframes ? (long)peer.pcm.size() : nframes;
      for (long i = 0; i < n; i++) {
        float s = peer.pcm.front();
        peer.pcm.pop_front();
        out[i * 2 + 0] += s * gl;
        out[i * 2 + 1] += s * gr;
      }
    }
    g_status.talking_peers = talking;
  }

  // soft clip
  for (long i = 0; i < nframes * 2; i++) {
    if (out[i] > 1.f) {
      out[i] = 1.f;
    } else if (out[i] < -1.f) {
      out[i] = -1.f;
    }
  }
  return nframes;
}

// ---------------- capture path ----------------

long input_cb(cubeb_stream* /*stream*/, void* /*user*/, const void* input, void* /*output*/,
              long nframes) {
  const float* in = (const float*)input;
  g_capture_accum.insert(g_capture_accum.end(), in, in + nframes);

  MumbleVoiceConfig cfg = snapshot_config();

  while (g_capture_accum.size() >= kFrameSamples) {
    float rms = 0.f;
    for (int i = 0; i < kFrameSamples; i++) {
      rms += g_capture_accum[i] * g_capture_accum[i];
    }
    rms = sqrtf(rms / kFrameSamples);

    bool open = !cfg.mic_muted && rms >= cfg.mic_gate;
    if (open) {
      g_gate_hangover = kGateHangoverFrames;
    } else if (g_gate_hangover > 0) {
      g_gate_hangover--;
      open = !cfg.mic_muted;
    }

    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_status.mic_level = g_status.mic_level * 0.8f + rms * 0.2f;
      g_status.transmitting = open;
    }

    if (open && g_encoder) {
      uint8_t packet[1500];
      opus_int32 len = opus_encode_float(g_encoder, g_capture_accum.data(), kFrameSamples, packet,
                                         sizeof(packet));
      if (len > 0) {
        bool last = g_gate_hangover == 1;  // gate closing - mark end of transmission
        float pos[3];
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          memcpy(pos, g_avatar_pos, sizeof(pos));
          g_status.frames_sent++;
        }
        mumble_native_send_voice(packet, (size_t)len, g_voice_sequence, pos, last);
      }
      g_voice_sequence += 2;  // sequence counts 10 ms units; we send 20 ms frames
    }

    g_capture_accum.erase(g_capture_accum.begin(), g_capture_accum.begin() + kFrameSamples);
  }
  return nframes;
}

void state_cb(cubeb_stream* /*stream*/, void* /*user*/, cubeb_state /*state*/) {}

// ---------------- start/stop ----------------

void set_message(const char* msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  snprintf(g_status.message, sizeof(g_status.message), "%s", msg);
}

void voice_stop() {
  if (!g_started) {
    return;
  }
  mumble_native_set_voice_rx(nullptr);
  if (g_in_stream) {
    cubeb_stream_stop(g_in_stream);
    cubeb_stream_destroy(g_in_stream);
    g_in_stream = nullptr;
  }
  if (g_out_stream) {
    cubeb_stream_stop(g_out_stream);
    cubeb_stream_destroy(g_out_stream);
    g_out_stream = nullptr;
  }
  if (g_ctx) {
    cubeb_destroy(g_ctx);
    g_ctx = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [session, peer] : g_peers) {
      if (peer.decoder) {
        opus_decoder_destroy(peer.decoder);
      }
    }
    g_peers.clear();
    g_status.capture_running = false;
    g_status.playback_running = false;
    g_status.talking_peers = 0;
    g_status.transmitting = false;
    snprintf(g_status.message, sizeof(g_status.message), "voice idle");
  }
  if (g_encoder) {
    opus_encoder_destroy(g_encoder);
    g_encoder = nullptr;
  }
  g_capture_accum.clear();
  g_started = false;
}

void voice_start() {
  if (g_started) {
    return;
  }
  g_started = true;  // even on partial failure; stop() cleans up whatever exists

  ensure_com_initialized();
  if (cubeb_init(&g_ctx, "OpenGOAL-Voice", nullptr) != CUBEB_OK) {
    set_message("voice: audio context failed");
    return;
  }

  MumbleVoiceConfig cfg = snapshot_config();
  snprintf(g_applied_input_id, sizeof(g_applied_input_id), "%s", cfg.input_device_id);
  snprintf(g_applied_output_id, sizeof(g_applied_output_id), "%s", cfg.output_device_id);

  // playback: 48 kHz stereo float
  {
    cubeb_devid dev = resolve_device(g_ctx, false, cfg.output_device_id);
    cubeb_stream_params outparam = {};
    outparam.channels = 2;
    outparam.format = CUBEB_SAMPLE_FLOAT32NE;
    outparam.rate = kSampleRate;
    outparam.layout = CUBEB_LAYOUT_STEREO;
    outparam.prefs = CUBEB_STREAM_PREF_NONE;
    uint32_t latency = 480;
    cubeb_get_min_latency(g_ctx, &outparam, &latency);
    if (cubeb_stream_init(g_ctx, &g_out_stream, "OpenGOAL-Voice-Out", nullptr, nullptr, dev,
                          &outparam, latency, output_cb, state_cb, nullptr) == CUBEB_OK &&
        cubeb_stream_start(g_out_stream) == CUBEB_OK) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_status.playback_running = true;
    } else {
      set_message("voice: playback stream failed");
    }
  }

  // capture: 48 kHz mono float
  {
    cubeb_devid dev = resolve_device(g_ctx, true, cfg.input_device_id);
    cubeb_stream_params inparam = {};
    inparam.channels = 1;
    inparam.format = CUBEB_SAMPLE_FLOAT32NE;
    inparam.rate = kSampleRate;
    inparam.layout = CUBEB_LAYOUT_MONO;
    inparam.prefs = CUBEB_STREAM_PREF_NONE;
    if (cubeb_stream_init(g_ctx, &g_in_stream, "OpenGOAL-Voice-In", dev, &inparam, nullptr,
                          nullptr, 480, input_cb, state_cb, nullptr) == CUBEB_OK &&
        cubeb_stream_start(g_in_stream) == CUBEB_OK) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_status.capture_running = true;
    } else {
      set_message("voice: mic stream failed (no input device?)");
    }
  }

  int err = 0;
  g_encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
  if (err == OPUS_OK) {
    opus_encoder_ctl(g_encoder, OPUS_SET_BITRATE(40000));
    opus_encoder_ctl(g_encoder, OPUS_SET_INBAND_FEC(1));
  } else {
    g_encoder = nullptr;
    set_message("voice: opus encoder failed");
  }

  g_voice_sequence = 0;
  mumble_native_set_voice_rx(voice_rx);

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.capture_running && g_status.playback_running) {
      snprintf(g_status.message, sizeof(g_status.message), "voice active");
    }
  }
}

}  // namespace

// ---------------- public interface ----------------

MumbleVoiceConfig g_mumble_voice_config;

MumbleVoiceStatus mumble_voice_get_status() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

void mumble_voice_set_user_volume(const char* name, float volume) {
  if (!name || !name[0]) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (volume == 1.f) {
    g_user_volumes.erase(name);  // 1.0 = no adjustment, don't store
  } else {
    g_user_volumes[name] = volume;
  }
}

float mumble_voice_get_user_volume(const char* name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_user_volumes.find(name);
  return it != g_user_volumes.end() ? it->second : 1.f;
}

std::unordered_map<std::string, float> mumble_voice_get_user_volumes() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_user_volumes;
}

int mumble_voice_enum_devices(bool input, MumbleVoiceDeviceInfo* out, int max_count) {
  ensure_com_initialized();
  cubeb* ctx = g_ctx;
  cubeb* temp = nullptr;
  if (!ctx) {
    if (cubeb_init(&temp, "OpenGOAL-Voice-Enum", nullptr) != CUBEB_OK) {
      return 0;
    }
    ctx = temp;
  }
  int count = 0;
  cubeb_device_collection coll = {};
  if (cubeb_enumerate_devices(ctx, input ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT,
                              &coll) == CUBEB_OK) {
    for (size_t i = 0; i < coll.count && count < max_count; i++) {
      const auto& d = coll.device[i];
      if (d.state != CUBEB_DEVICE_STATE_ENABLED || !d.device_id) {
        continue;
      }
      snprintf(out[count].id, sizeof(out[count].id), "%s", d.device_id);
      snprintf(out[count].name, sizeof(out[count].name), "%s",
               d.friendly_name ? d.friendly_name : d.device_id);
      count++;
    }
    cubeb_device_collection_destroy(ctx, &coll);
  }
  if (temp) {
    cubeb_destroy(temp);
  }
  return count;
}

void mumble_voice_maintain(const float avatar_pos[3],
                           const float cam_pos[3],
                           const float cam_front[3],
                           const float cam_top[3]) {
  mumble_config_load();  // no-op after the first call
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    memcpy(g_avatar_pos, avatar_pos, sizeof(g_avatar_pos));
    memcpy(g_cam_pos, cam_pos, sizeof(g_cam_pos));
    memcpy(g_cam_front, cam_front, sizeof(g_cam_front));
    memcpy(g_cam_top, cam_top, sizeof(g_cam_top));
  }
  if (mumble_native_connected()) {
    // restart the streams if the user picked different devices
    if (g_started) {
      MumbleVoiceConfig cfg = snapshot_config();
      if (strcmp(cfg.input_device_id, g_applied_input_id) != 0 ||
          strcmp(cfg.output_device_id, g_applied_output_id) != 0) {
        voice_stop();
      }
    }
    voice_start();
  } else {
    voice_stop();
  }
}
