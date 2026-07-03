#pragma once

#include <cstdint>

#include "game/kernel/common/mumble_link.h"

/*!
 * @file mumble_native.h
 * Native Mumble client embedded in gk - no external Mumble application needed.
 *
 * Phase 1 (this file): full control-channel client. TLS connection (via
 * libcurl CONNECT_ONLY), authentication, keepalive, user tracking, and
 * position sync over Mumble's plugin data channel. Uses the same dataID and
 * payload as the external plugin (mumble-plugin/), so native players and
 * external-Mumble players see each other's positions.
 *
 * Phase 2 (TODO): voice - Opus encode/decode over UDPTunnel messages with
 * mic capture and spatialized playback via miniaudio.
 */

// editable from the ImGui Mumble menu
struct MumbleNativeConfig {
  // default: the mod's Oracle VPS proximity server
  char host[128] = "150.136.225.222";
  int port = 64738;
  char username[64] = "";
  char password[64] = "";
};

enum class MumbleNativeState : int {
  Disconnected = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3,
};

struct MumbleNativeStatus {
  MumbleNativeState state = MumbleNativeState::Disconnected;
  char message[128] = "not connected";
  uint32_t session = 0;  // our session id on the server
  int user_count = 0;    // users visible on the server (including us)
  uint32_t positions_sent = 0;
  uint32_t positions_received = 0;
};

extern MumbleNativeConfig g_mumble_native_config;

// Start/stop the client thread. connect() reads g_mumble_native_config.
void mumble_native_connect();
void mumble_native_disconnect();

bool mumble_native_connected();
MumbleNativeStatus mumble_native_get_status();

// Called once per frame by mumble_link_update with the local player's
// position in raw game units. Broadcast happens at a fixed rate off-thread.
void mumble_native_update_position(const float pos[3]);

// Peers seen through the native connection (same contract as
// mumble_link_get_peers; out must hold >= kMaxMumblePeers entries).
int mumble_native_get_peers(MumbleLinkPeer* out);

// Name of a connected user by session id. Returns false if unknown.
bool mumble_native_get_user_name(uint32_t session, char* out, size_t out_size);

// All connected users except ourselves (names only, for UI lists like
// per-user volume). Returns count written, up to max_count.
int mumble_native_get_user_list(char (*names)[32], int max_count);

// ---------------- voice transport (used by mumble_voice.cpp) ----------------

// Incoming voice frame from another user: opus payload plus the sender's
// position in Mumble meters if the packet carried one (pos == nullptr if not).
// Called from the client thread.
using MumbleVoiceRxFn = void (*)(uint32_t session,
                                 uint32_t sequence,
                                 const uint8_t* opus,
                                 size_t opus_len,
                                 const float* pos);
void mumble_native_set_voice_rx(MumbleVoiceRxFn fn);

// Queue one encoded opus frame for transmission (thread-safe). `pos` is our
// position in Mumble meters, or nullptr to send non-positional audio.
// `end_of_transmission` marks the last frame when the mic closes.
void mumble_native_send_voice(const uint8_t* opus,
                              size_t opus_len,
                              uint32_t sequence,
                              const float pos[3],
                              bool end_of_transmission);
