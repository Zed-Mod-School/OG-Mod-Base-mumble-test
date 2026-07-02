#pragma once

#include <cstdint>

#include "game/kernel/common/mumble_peers_shm.h"

/*!
 * @file mumble_link.h
 * Mumble "Link" positional-audio integration for proximity voice chat.
 * Writes the local player's position/orientation into the shared memory block
 * that a running Mumble client polls. Protocol: https://wiki.mumble.info/wiki/Link
 */

// Live tuning knobs, editable from the ImGui "Mumble" menu (debug_gui.cpp).
struct MumbleLinkTuning {
  // master switch - when off, updates stop and Mumble falls back to
  // non-positional audio after a moment
  bool enabled = true;
  // when false, the world scale GOAL passes each frame (*mumble-world-scale*)
  // is used and the slider just displays it; when true, the slider wins
  bool override_world_scale = false;
  float world_scale = 7.02f;
  // negate the X of all positions and directions (reflect across the YZ
  // plane). Flips the coordinate handedness - toggle this if voices pan to
  // the wrong side.
  bool mirror_x = false;
  // how often to retry opening Mumble's shared memory while not connected
  float retry_interval_s = 5.0f;
};

// Read-only view of the most recent update, for the ImGui menu.
struct MumbleLinkStatus {
  bool connected = false;
  float goal_scale = 0.f;       // scale GOAL passed this frame
  float effective_scale = 0.f;  // scale actually applied
  float avatar_pos[3] = {};     // meters, as written to Mumble
  float camera_pos[3] = {};
  float camera_front[3] = {};
  uint32_t updates_sent = 0;
};

extern MumbleLinkTuning g_mumble_link_tuning;
extern MumbleLinkStatus g_mumble_link_status;

// A voice peer whose position was received via the OpenGOAL Mumble plugin
// (see mumble-plugin/ and mumble_peers_shm.h).
struct MumbleLinkPeer {
  char name[32];
  float pos[3];  // raw game units, world space
};

// Snapshot the current (fresh) peers into `out` (size >= kMaxMumblePeers).
// Returns the number of peers written. Safe to call even if the plugin isn't
// running - returns 0.
int mumble_link_get_peers(MumbleLinkPeer* out);

// Push one frame of positional data to Mumble. Positions are in game units
// (4096 per meter); front/top are unit vectors, all using X=right, Y=up.
// goal_world_scale is the divisor GOAL wants applied on top of the
// units->meters conversion (values > 1 shrink the world so voices carry
// further; non-positive values are treated as 1).
// Safe to call even if Mumble isn't running - it connects lazily and retries.
void mumble_link_update(const float avatar_pos[3],
                        const float avatar_front[3],
                        const float avatar_top[3],
                        const float camera_pos[3],
                        const float camera_front[3],
                        const float camera_top[3],
                        float goal_world_scale);
