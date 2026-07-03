/*!
 * @file mumble_link.cpp
 * Mumble "Link" positional-audio integration for proximity voice chat.
 * See https://wiki.mumble.info/wiki/Link for the shared-memory protocol.
 */

#include "mumble_link.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/kernel/common/mumble_native.h"
#include "game/kernel/common/mumble_peers_shm.h"
#include "game/kernel/common/mumble_voice.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#endif

MumbleLinkTuning g_mumble_link_tuning;
MumbleLinkStatus g_mumble_link_status;

namespace {

// Layout is fixed by the Mumble Link protocol. Mumble creates the block;
// we only open and write it. wchar_t is intentional - it differs in size
// between Windows (2) and Linux (4), and so does Mumble's own definition.
struct LinkedMem {
  uint32_t uiVersion;
  uint32_t uiTick;
  float fAvatarPosition[3];
  float fAvatarFront[3];
  float fAvatarTop[3];
  wchar_t name[256];
  float fCameraPosition[3];
  float fCameraFront[3];
  float fCameraTop[3];
  wchar_t identity[256];
  uint32_t context_len;
  unsigned char context[256];
  wchar_t description[2048];
};

LinkedMem* g_link = nullptr;

// Only players whose context matches hear each other positionally, so this
// must be identical across all clients of the mod.
constexpr char kContext[] = "OpenGOAL-Jak1";

bool try_open_link() {
#ifdef _WIN32
  HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"MumbleLink");
  if (!map) {
    return false;
  }
  void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkedMem));
  // the view holds its own reference to the mapping object, so the handle can go.
  CloseHandle(map);
  if (!mem) {
    return false;
  }
  g_link = (LinkedMem*)mem;
#else
  char memname[256];
  snprintf(memname, sizeof(memname), "/MumbleLink.%d", getuid());
  int fd = shm_open(memname, O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  void* mem = mmap(nullptr, sizeof(LinkedMem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) {
    return false;
  }
  g_link = (LinkedMem*)mem;
#endif
  return true;
}

// Mumble only creates the shared memory while it is running, so the game can't
// connect at boot if Mumble was started later. Retry quietly in the background.
bool link_ready() {
  if (g_link) {
    return true;
  }
  static auto last_attempt = std::chrono::steady_clock::time_point{};
  static bool logged_waiting = false;
  auto now = std::chrono::steady_clock::now();
  auto interval =
      std::chrono::milliseconds((int64_t)(g_mumble_link_tuning.retry_interval_s * 1000.f));
  if (last_attempt != std::chrono::steady_clock::time_point{} && now - last_attempt < interval) {
    return false;
  }
  last_attempt = now;
  if (try_open_link()) {
    lg::info("MumbleLink: connected to Mumble shared memory.");
    return true;
  }
  if (!logged_waiting) {
    lg::info("MumbleLink: Mumble not detected yet, will keep retrying in the background.");
    logged_waiting = true;
  }
  return false;
}

// The peers block is game-owned: created here, opened by the Mumble plugin.
// It carries our raw-unit position out and other players' positions back in.
MumblePeersShm* g_peers = nullptr;

void peers_shm_init() {
  if (g_peers) {
    return;
  }
#ifdef _WIN32
  HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(MumblePeersShm), kMumblePeersShmName);
  if (!map) {
    return;
  }
  void* mem = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MumblePeersShm));
  if (!mem) {
    CloseHandle(map);
    return;
  }
  // keep the handle open for the lifetime of the process - the mapping object
  // is destroyed when the last handle/view goes away.
#else
  char memname[256];
  snprintf(memname, sizeof(memname), "/%s.%d", kMumblePeersShmName, getuid());
  int fd = shm_open(memname, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return;
  }
  if (ftruncate(fd, sizeof(MumblePeersShm)) != 0) {
    close(fd);
    return;
  }
  void* mem = mmap(nullptr, sizeof(MumblePeersShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) {
    return;
  }
#endif
  auto* shm = (MumblePeersShm*)mem;
  if (shm->magic != kMumblePeersMagic) {
    memset(shm, 0, sizeof(MumblePeersShm));
    shm->magic = kMumblePeersMagic;
    shm->version = kMumblePeersVersion;
  }
  g_peers = shm;
  lg::info("MumbleLink: peers shared memory ready.");
}

}  // namespace

int mumble_link_get_peers(MumbleLinkPeer* out) {
  // the native in-game client takes precedence over the external
  // Mumble-plus-plugin path when it's connected
  if (mumble_native_connected()) {
    return mumble_native_get_peers(out);
  }
  if (!g_peers) {
    return 0;
  }
  const uint32_t now = peers_now_ms();
  int count = 0;
  for (int i = 0; i < kMaxMumblePeers && count < kMaxMumblePeers; i++) {
    const auto& slot = g_peers->peers[i];
    if (!slot.used) {
      continue;
    }
    // drop peers the plugin hasn't refreshed recently (left the server,
    // closed their game, etc.)
    if (now - slot.last_update_ms > 3000) {
      continue;
    }
    auto& peer = out[count++];
    memcpy(peer.name, slot.name, sizeof(peer.name));
    peer.name[sizeof(peer.name) - 1] = 0;
    memcpy(peer.pos, slot.pos, sizeof(peer.pos));
  }
  return count;
}

void mumble_link_update(const float avatar_pos[3],
                        const float avatar_front[3],
                        const float avatar_top[3],
                        const float camera_pos[3],
                        const float camera_front[3],
                        const float camera_top[3],
                        float goal_world_scale) {
  auto& tuning = g_mumble_link_tuning;
  auto& status = g_mumble_link_status;

  status.goal_scale = goal_world_scale;
  status.connected = g_link != nullptr;

  if (!tuning.enabled) {
    return;  // stale tick -> Mumble falls back to non-positional audio
  }

  // publish our raw-unit position for the Mumble plugin to broadcast to other
  // players (independent of the audio link below being connected)
  peers_shm_init();
  if (g_peers) {
    memcpy(g_peers->local_pos, avatar_pos, sizeof(g_peers->local_pos));
    g_peers->local_tick++;
    g_peers->echo_self = tuning.echo_self_test ? 1 : 0;
  }
  // feed the native in-game client too (no-op unless connected)
  mumble_native_update_position(avatar_pos);

  float scale = tuning.override_world_scale ? tuning.world_scale : goal_world_scale;
  if (!(scale > 0.f)) {
    scale = 1.f;
  }
  if (!tuning.override_world_scale) {
    // keep the (disabled) slider showing the value currently in effect
    tuning.world_scale = scale;
  }
  status.effective_scale = scale;

  const float units_per_meter = (float)METER_LENGTH * scale;
  const float x_sign = tuning.mirror_x ? -1.f : 1.f;

  float a_pos[3], a_front[3], a_top[3], c_pos[3], c_front[3], c_top[3];
  for (int i = 0; i < 3; i++) {
    const float pos_sign = (i == 0) ? x_sign : 1.f;
    a_pos[i] = pos_sign * avatar_pos[i] / units_per_meter;
    c_pos[i] = pos_sign * camera_pos[i] / units_per_meter;
    a_front[i] = pos_sign * avatar_front[i];
    a_top[i] = pos_sign * avatar_top[i];
    c_front[i] = pos_sign * camera_front[i];
    c_top[i] = pos_sign * camera_top[i];
  }

  memcpy(status.avatar_pos, a_pos, sizeof(a_pos));
  memcpy(status.camera_pos, c_pos, sizeof(c_pos));
  memcpy(status.camera_front, c_front, sizeof(c_front));

  // native voice engine: start/stop with the native connection and keep the
  // mixer's listener + transmitter transforms fresh (Mumble-meter space)
  mumble_voice_maintain(a_pos, c_pos, c_front, c_top);

  if (!link_ready()) {
    return;  // no external Mumble running - native path already handled above
  }
  status.connected = true;

  // Mumble zeroes uiVersion when the link is (re)initialized, so re-send the
  // static info whenever it isn't set.
  if (g_link->uiVersion != 2) {
    wcsncpy(g_link->name, L"OpenGOAL Jak 1", 256);
    wcsncpy(g_link->description, L"Positional audio for OpenGOAL Jak and Daxter", 2048);
    memcpy(g_link->context, kContext, sizeof(kContext));
    g_link->context_len = sizeof(kContext);
    g_link->uiVersion = 2;
  }

  g_link->uiTick++;

  memcpy(g_link->fAvatarPosition, a_pos, sizeof(a_pos));
  memcpy(g_link->fAvatarFront, a_front, sizeof(a_front));
  memcpy(g_link->fAvatarTop, a_top, sizeof(a_top));
  memcpy(g_link->fCameraPosition, c_pos, sizeof(c_pos));
  memcpy(g_link->fCameraFront, c_front, sizeof(c_front));
  memcpy(g_link->fCameraTop, c_top, sizeof(c_top));

  status.updates_sent++;
}
