/*!
 * @file opengoal_jak1_positions.cpp
 * Mumble client plugin that syncs OpenGOAL Jak 1 player positions between
 * everyone on the server, using Mumble's plugin data channel.
 *
 * Data flow:
 *   game (mumble_link.cpp) --[MumblePeersShm shared memory]--> this plugin
 *     --[sendData over the Mumble server]--> other players' plugins
 *     --[MumblePeersShm shared memory]--> their games (drawn as spheres)
 *
 * Positions are raw Jak game units (4096 per meter) end to end, so every
 * player's *mumble-world-scale* audio tuning is irrelevant here.
 *
 * Requires Mumble 1.5+ on every player's machine, with this plugin installed
 * (Configure -> Settings -> Plugins -> Install plugin...).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include "MumblePlugin.h"

#include "../game/kernel/common/mumble_peers_shm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#endif

namespace {

constexpr const char* kDataID = "opengoal-jak1-pos";
constexpr uint32_t kPayloadMagic = kMumblePeersMagic;
// keep well under Murmur's plugin-message rate limit (the API docs recommend
// spacing messages out; 5 Hz is plenty for spheres)
constexpr auto kSendInterval = std::chrono::milliseconds(200);

#pragma pack(push, 1)
struct PositionPayload {
  uint32_t magic;
  float pos[3];  // raw game units
};
#pragma pack(pop)

mumble_api_t g_api;
mumble_plugin_id_t g_plugin_id = 0;
std::atomic<mumble_connection_t> g_connection{-1};

MumblePeersShm* g_shm = nullptr;
std::mutex g_shm_write_mutex;  // orders the plugin's own writers (send thread vs callbacks)

std::thread g_send_thread;
std::atomic<bool> g_running{false};

// The game creates the shared memory block; retry until it exists (the player
// may start Mumble before the game).
bool try_open_shm() {
  if (g_shm) {
    return true;
  }
#ifdef _WIN32
  HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kMumblePeersShmName);
  if (!map) {
    return false;
  }
  void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MumblePeersShm));
  CloseHandle(map);
  if (!mem) {
    return false;
  }
#else
  char memname[256];
  snprintf(memname, sizeof(memname), "/%s.%d", kMumblePeersShmName, getuid());
  int fd = shm_open(memname, O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  void* mem = mmap(nullptr, sizeof(MumblePeersShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) {
    return false;
  }
#endif
  auto* shm = (MumblePeersShm*)mem;
  if (shm->magic != kMumblePeersMagic || shm->version != kMumblePeersVersion) {
    // game build with a different layout - don't touch it
#ifdef _WIN32
    UnmapViewOfFile(mem);
#else
    munmap(mem, sizeof(MumblePeersShm));
#endif
    return false;
  }
  g_shm = shm;
  return true;
}

void expire_stale_peers() {
  const uint32_t now = peers_now_ms();
  std::lock_guard<std::mutex> lock(g_shm_write_mutex);
  for (auto& slot : g_shm->peers) {
    if (slot.used && now - slot.last_update_ms > 5000) {
      slot.used = 0;
    }
  }
}

void broadcast_local_position() {
  mumble_connection_t conn = g_connection.load();
  if (conn < 0) {
    return;
  }

  // only send while the game is actively updating (tick moves each frame);
  // frozen tick = title screen / loading / game closed
  static uint32_t last_tick = 0;
  const uint32_t tick = g_shm->local_tick;
  if (tick == last_tick) {
    return;
  }
  last_tick = tick;

  PositionPayload payload;
  payload.magic = kPayloadMagic;
  memcpy(payload.pos, g_shm->local_pos, sizeof(payload.pos));

  mumble_userid_t local_id;
  if (g_api.getLocalUserID(g_plugin_id, conn, &local_id) != MUMBLE_STATUS_OK) {
    return;
  }
  mumble_userid_t* users = nullptr;
  size_t user_count = 0;
  if (g_api.getAllUsers(g_plugin_id, conn, &users, &user_count) != MUMBLE_STATUS_OK) {
    return;
  }
  // send to everyone but ourselves
  size_t target_count = 0;
  for (size_t i = 0; i < user_count; i++) {
    if (users[i] != local_id) {
      users[target_count++] = users[i];
    }
  }
  if (target_count > 0) {
    g_api.sendData(g_plugin_id, conn, users, target_count, (const uint8_t*)&payload,
                   sizeof(payload), kDataID);
  }
  g_api.freeMemory(g_plugin_id, users);
}

void send_thread_main() {
  auto last_shm_attempt = std::chrono::steady_clock::time_point{};
  while (g_running.load()) {
    std::this_thread::sleep_for(kSendInterval);
    if (!g_shm) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_shm_attempt < std::chrono::seconds(2)) {
        continue;
      }
      last_shm_attempt = now;
      if (!try_open_shm()) {
        continue;
      }
      g_api.log(g_plugin_id, "OpenGOAL Jak 1: connected to game shared memory.");
    }
    broadcast_local_position();
    expire_stale_peers();
  }
}

}  // namespace

//////////////////// mandatory plugin functions ////////////////////

MUMBLE_PLUGIN_EXPORT mumble_error_t MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_init(mumble_plugin_id_t id) {
  g_plugin_id = id;
  g_running = true;
  g_send_thread = std::thread(send_thread_main);
  return MUMBLE_STATUS_OK;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_shutdown() {
  g_running = false;
  if (g_send_thread.joinable()) {
    g_send_thread.join();
  }
  if (g_shm) {
#ifdef _WIN32
    UnmapViewOfFile(g_shm);
#else
    munmap(g_shm, sizeof(MumblePeersShm));
#endif
    g_shm = nullptr;
  }
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getName() {
  static const char* name = "OpenGOAL Jak 1 Positions";
  return {name, strlen(name), false};
}

MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getAPIVersion() {
  return MUMBLE_PLUGIN_API_VERSION;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_registerAPIFunctions(void* apiStruct) {
  g_api = MUMBLE_API_CAST(apiStruct);
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_releaseResource(const void* /*pointer*/) {
  // all returned resources are static - nothing to release
}

//////////////////// general info functions ////////////////////

MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getVersion() {
  return {0, 1, 0};
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getAuthor() {
  static const char* author = "Zed";
  return {author, strlen(author), false};
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_getDescription() {
  static const char* description =
      "Relays OpenGOAL Jak 1 player positions between everyone on the server so the "
      "mod can draw other players in the world. Pairs with the Mumble proximity chat mod.";
  return {description, strlen(description), false};
}

MUMBLE_PLUGIN_EXPORT uint32_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getFeatures() {
  return MUMBLE_FEATURE_NONE;
}

//////////////////// callbacks ////////////////////

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onServerSynchronized(mumble_connection_t connection) {
  g_connection = connection;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onServerDisconnected(mumble_connection_t connection) {
  if (g_connection.load() == connection) {
    g_connection = -1;
  }
  if (g_shm) {
    std::lock_guard<std::mutex> lock(g_shm_write_mutex);
    for (auto& slot : g_shm->peers) {
      slot.used = 0;
    }
  }
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onReceiveData(mumble_connection_t connection,
                     mumble_userid_t sender,
                     const uint8_t* data,
                     size_t dataLength,
                     const char* dataID) {
  if (strcmp(dataID, kDataID) != 0) {
    return false;  // not ours - let other plugins look at it
  }
  if (dataLength != sizeof(PositionPayload) || !g_shm) {
    return true;
  }
  PositionPayload payload;
  memcpy(&payload, data, sizeof(payload));
  if (payload.magic != kPayloadMagic) {
    return true;
  }

  std::lock_guard<std::mutex> lock(g_shm_write_mutex);

  // find this sender's slot, or claim a free one
  MumblePeerSlot* slot = nullptr;
  for (auto& s : g_shm->peers) {
    if (s.used && s.user_id == sender) {
      slot = &s;
      break;
    }
  }
  if (!slot) {
    for (auto& s : g_shm->peers) {
      if (!s.used) {
        slot = &s;
        break;
      }
    }
  }
  if (!slot) {
    return true;  // table full
  }

  if (!slot->used || slot->user_id != sender) {
    slot->user_id = sender;
    slot->name[0] = 0;
    const char* name = nullptr;
    if (g_api.getUserName(g_plugin_id, connection, sender, &name) == MUMBLE_STATUS_OK) {
      snprintf(slot->name, sizeof(slot->name), "%s", name);
      g_api.freeMemory(g_plugin_id, name);
    }
  }
  memcpy(slot->pos, payload.pos, sizeof(slot->pos));
  slot->last_update_ms = peers_now_ms();
  slot->used = 1;
  return true;
}
