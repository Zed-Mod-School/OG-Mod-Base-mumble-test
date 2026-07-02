#pragma once

#include <cstdint>

/*!
 * @file mumble_peers_shm.h
 * Layout of the shared memory block used to exchange player positions between
 * the game and the OpenGOAL Mumble plugin (mumble-plugin/ in this repo).
 *
 * The game CREATES this block and writes the local player's position into the
 * game->plugin half. The Mumble plugin opens it, broadcasts the local position
 * to other players over Mumble's plugin data channel, and writes the positions
 * it receives from other players' plugins into the plugin->game half.
 *
 * All positions are RAW game units (4096 per meter, world space) - no scaling
 * is applied anywhere in this path, so the values are unambiguous regardless
 * of each player's *mumble-world-scale* audio setting.
 *
 * There is intentionally no lock: writers bump the tick/stamp fields and both
 * sides tolerate the (rare, harmless) torn read of a float triplet.
 */

// bump kMumblePeersVersion when changing any of these structs
constexpr uint32_t kMumblePeersMagic = 0x4A315058;  // "J1PX"
constexpr uint32_t kMumblePeersVersion = 1;
constexpr int kMaxMumblePeers = 15;
constexpr const char* kMumblePeersShmName = "OpenGOAL-Jak1-MumblePeers";

struct MumblePeerSlot {
  uint32_t used;            // 0 = free slot
  uint32_t user_id;         // mumble user id, keys the slot
  uint32_t last_update_ms;  // system uptime ms when last updated (see peers_now_ms)
  char name[32];            // mumble user name, null terminated
  float pos[3];             // raw game units, world space
};

struct MumblePeersShm {
  uint32_t magic;
  uint32_t version;
  // game -> plugin
  uint32_t local_tick;  // bumped by the game every update; static = not in game
  float local_pos[3];   // raw game units
  // plugin -> game
  MumblePeerSlot peers[kMaxMumblePeers];
};

// Both processes stamp/compare peer ages with the same clock. steady_clock is
// system-wide on both Windows (QPC since boot) and Linux (CLOCK_MONOTONIC), so
// stamps from the plugin process compare cleanly in the game process. The
// 32-bit truncation wraps every ~49 days; the comparison window is seconds,
// so a wrap can only cause one brief flicker.
#include <chrono>
inline uint32_t peers_now_ms() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
