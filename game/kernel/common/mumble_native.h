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
  char host[128] = "127.0.0.1";
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
