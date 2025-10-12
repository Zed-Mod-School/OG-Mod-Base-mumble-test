/*!
 * @file kboot.cpp
 * GOAL Boot. Contains the "main" function to launch GOAL runtime
 * DONE!
 */

#include "kboot.h"

#include <chrono>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <thread>

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/common/game_common_types.h"
#include "game/graphics/gfx.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/ksocket.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kmachine.h"
#include "game/sce/libscf.h"

// Platform-specific headers for shared memory
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#endif  // _WIN32

using namespace ee;

// --- MumbleLink Core Definitions ---

// Mumble shared memory structure (must match Mumble's expectation)
struct LinkedMem {
#ifdef _WIN32
  UINT32 uiVersion;
  DWORD uiTick;
#else
  uint32_t uiVersion;
  uint32_t uiTick;
#endif
  float fAvatarPosition[3];
  float fAvatarFront[3];
  float fAvatarTop[3];
  wchar_t name[256];
  float fCameraPosition[3];
  float fCameraFront[3];
  float fCameraTop[3];
  wchar_t identity[256];
#ifdef _WIN32
  UINT32 context_len;
#else
  uint32_t context_len;
#endif
  unsigned char context[256];
  wchar_t description[2048];
};

static LinkedMem* lm = nullptr;

/*!
 * Initialize the Mumble shared memory block.
 * Includes debugging printouts for connection status.
 */
void MumbleLinkInit() {
#ifdef _WIN32
  // --- Debugging step: Try to open the shared memory object ---
  printf("MumbleLink: Attempting to open shared memory 'MumbleLink' (Windows)...\n");
  HANDLE hMapObject = OpenFileMappingW(FILE_MAP_ALL_ACCESS, false, L"MumbleLink");
  if (hMapObject == NULL) {
    printf(
        "MumbleLink: FAILED to open file mapping (Error %lu). Is Mumble running and linked to a "
        "game?\n",
        GetLastError());
    return;
  }
  printf("MumbleLink: File mapping successfully opened.\n");

  // --- Debugging step: Try to map the view of the file ---
  lm = (LinkedMem*)MapViewOfFile(hMapObject, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkedMem));
  if (lm == NULL) {
    printf("MumbleLink: FAILED to map view of file (Error %lu)...\n", GetLastError());
    CloseHandle(hMapObject);
    return;
  }
  printf("MumbleLink: View of file successfully mapped. Initialization complete.\n");

#else
  // Linux/Unix initialization
  char memname[256];
  snprintf(memname, 256, "/MumbleLink.%d", getuid());

  printf("MumbleLink: Attempting to open shared memory '%s' (Linux/Unix)...\n", memname);
  int shmfd = shm_open(memname, O_RDWR, S_IRUSR | S_IWUSR);

  if (shmfd < 0) {
    printf("MumbleLink: FAILED to open shared memory file descriptor. Is Mumble running?\n");
    return;
  }
  printf("MumbleLink: Shared memory file descriptor opened.\n");

  lm = (LinkedMem*)(mmap(NULL, sizeof(struct LinkedMem), PROT_READ | PROT_WRITE, MAP_SHARED, shmfd,
                         0));

  if (lm == MAP_FAILED) {
    printf("MumbleLink: FAILED to map shared memory into address space.\n");
    lm = nullptr;
    return;
  }
  printf("MumbleLink: Shared memory successfully mapped. Initialization complete.\n");
#endif
}

/*!
 * Update the Mumble shared memory block with new positional data.
 */
void MumbleLinkUpdate(const float avatar_pos[3],
                      const float avatar_front[3],
                      const float avatar_top[3],
                      const float camera_pos[3],
                      const float camera_front[3],
                      const float camera_top[3]) {
  if (!lm) {
    return;  // Link not initialized or failed
  }

  if (lm->uiVersion != 2) {
    // Set initial static information if link is new/reset
    wcsncpy(lm->name, L"OpenGOAL Jak 1", 256);
    wcsncpy(lm->description, L"Positional Audio for OpenGOAL Jak and Daxter: The Precursor Legacy",
            2048);
    lm->uiVersion = 2;

    // Set static context/identity (can be updated later if needed for multi-server/instance)
    wcsncpy(lm->identity, L"Jak1Player", 256);
    // Unique identifier for the game instance/context
    memcpy(lm->context, "OpenGOAL\x00\x01\x02\x03\x04\x05", 16);
    lm->context_len = 16;
  }

  // 1. Update tick count (Mumble uses this to know data has changed)
  lm->uiTick++;

  // 2. Copy Avatar Positional Data
  // Mumble Coordinate System: Left-handed, X=Right, Y=Up, Z=Front (1 unit = 1 meter)
  memcpy(lm->fAvatarPosition, avatar_pos, 3 * sizeof(float));
  memcpy(lm->fAvatarFront, avatar_front, 3 * sizeof(float));
  memcpy(lm->fAvatarTop, avatar_top, 3 * sizeof(float));

  // 3. Copy Camera Positional Data
  memcpy(lm->fCameraPosition, camera_pos, 3 * sizeof(float));
  memcpy(lm->fCameraFront, camera_front, 3 * sizeof(float));
  memcpy(lm->fCameraTop, camera_top, 3 * sizeof(float));
}

namespace jak1 {
VideoMode BootVideoMode;

void kboot_init_globals() {}

/*!
 * Launch the GOAL Kernel (EE).
 * DONE!
 * See InitParms for launch argument details.
 * @param argc : argument count
 * @param argv : argument list
 * @return 0 on success, otherwise failure.
 */
s32 goal_main(int argc, const char* const* argv) {
  InitParms(argc, argv);

  // Initialize CRC32 table for string hashing
  init_crc();

  // Call the Mumble initialization function
  MumbleLinkInit();

  // NTSC V1, NTSC v2, PAL CD Demo, PAL Retail
  // Set up game configurations
  masterConfig.aspect = (u16)sceScfGetAspect();
  masterConfig.language = (u16)sceScfGetLanguage();
  masterConfig.inactive_timeout = 0;  // demo thing
  masterConfig.timeout = 0;           // demo thing
  masterConfig.volume = 100;

  // Set up language configuration
  if (masterConfig.language == SCE_SPANISH_LANGUAGE) {
    masterConfig.language = (u16)Language::Spanish;
  } else if (masterConfig.language == SCE_FRENCH_LANGUAGE) {
    masterConfig.language = (u16)Language::French;
  } else if (masterConfig.language == SCE_GERMAN_LANGUAGE) {
    masterConfig.language = (u16)Language::German;
  } else if (masterConfig.language == SCE_ITALIAN_LANGUAGE) {
    masterConfig.language = (u16)Language::Italian;
  } else if (masterConfig.language == SCE_JAPANESE_LANGUAGE) {
    // Note: this case was added so it is easier to test Japanese fonts.
    masterConfig.language = (u16)Language::Japanese;
  } else {
    // pick english by default, if language is not supported.
    masterConfig.language = (u16)Language::English;
  }

  // Set up aspect ratio override in demo
  if (!strcmp(DebugBootMessage, "demo") || !strcmp(DebugBootMessage, "demo-shared")) {
    masterConfig.aspect = SCE_ASPECT_FULL;
  }

  // Launch GOAL!
  if (InitMachine() >= 0) {    // init kernel
    KernelCheckAndDispatch();  // run kernel
    ShutdownMachine();         // kernel died, we should too.
  } else {
    fprintf(stderr, "InitMachine failed\n");
    exit(1);
  }

  return 0;
}

/*!
 * Main loop to dispatch the GOAL kernel.
 */
void KernelCheckAndDispatch() {
  u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 8;

  // --- Mumble Link: Dynamic Test Data Setup ---
  // Static variable to track position over time. This value persists across frames.
  static float current_x_pos = 0.5f;
  static float current_y_pos = 0.5f;
  static float current_z_pos = 0.5f;
  // Static time point for periodic console printing (new addition)
  static auto last_print_time = std::chrono::high_resolution_clock::now();
  // -------------------------------------------

  // Hardcoded test vectors for Mumble (Replace these with actual game data!)
  // The Z-component of these arrays will be updated inside the loop.
  float avatar_pos[3] = {0.001f, 0.0f, 0.0f};  // Z will be updated
  float avatar_front[3] = {0.0f, 0.0f, 1.0f};  // Forward vector (0,0,1 is Z-forward)
  float avatar_top[3] = {0.0f, 1.0f, 0.0f};    // Up vector (0,1,0 is Y-up)
  float camera_pos[3] = {0.0f, 0.0f, 0.0f};    // Z will be updated
  float camera_front[3] = {0.0f, 0.0f, 1.0f};
  float camera_top[3] = {0.0f, 1.0f, 0.0f};

  while (MasterExit == RuntimeExitStatus::RUNNING) {
    // try to get a message from the listener, and process it if needed
    Ptr<char> new_message = WaitForMessageAndAck();
    if (new_message.offset) {
      ProcessListenerMessage(new_message);
    }

    // remember the old listener function
    auto old_listener = ListenerFunction->value;
    // dispatch the kernel
    //(**kernel_dispatcher)();

    Timer kernel_dispatch_timer;
    if (MasterUseKernel) {
      // use the GOAL kernel.
      call_goal_on_stack(Ptr<Function>(kernel_dispatcher->value), goal_stack, s7.offset,
                         g_ee_main_mem);
    } else {
      // use a hack to just run the listener function if there's no GOAL kernel.
      if (ListenerFunction->value != s7.offset) {
        auto result = call_goal_on_stack(Ptr<Function>(ListenerFunction->value), goal_stack,
                                         s7.offset, g_ee_main_mem);
#ifdef __linux__
        cprintf("%ld\n", result);
#else
        cprintf("%lld\n", result);
#endif
        ListenerFunction->value = s7.offset;
      }
    }

    auto time_ms = kernel_dispatch_timer.getMs();
    if (time_ms > 50) {
      lg::print("Kernel dispatch time: {:.3f} ms\n", time_ms);
    }

    ClearPending();

    // --- Mumble Link: Dynamic Test Data Update ---
    // 1. Increment Z slightly each frame to simulate forward movement.
    current_x_pos = Gfx::g_global_settings.target_x;
    current_y_pos = Gfx::g_global_settings.target_y;
    current_z_pos = Gfx::g_global_settings.target_z;

    // Avatar position (follows player directly)
    avatar_pos[0] = current_x_pos;  // X
    avatar_pos[1] = current_y_pos;  // Y
    avatar_pos[2] = current_z_pos;  // Z

    // Camera position (slightly behind avatar)
    camera_pos[0] = current_x_pos - 0.5f;
    ;  // X
    camera_pos[1] = current_y_pos - 0.5f;
    ;                                      // Y
    camera_pos[2] = current_z_pos - 0.5f;  // Z offset to trail slightly
    // ---------------------------------------------

    // Call the Mumble update function with test data
    MumbleLinkUpdate(avatar_pos, avatar_front, avatar_top, camera_pos, camera_front, camera_top);

    // --- Periodic Console Print (Every 30 seconds) ---
    auto current_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = current_time - last_print_time;

    if (elapsed.count() >= 30.0) {
      printf("MumbleLink Debug: Positional Update (30s interval):\n");
      printf("  Avatar Pos (X, Y, Z): %.4f, %.4f, %.4f\n", avatar_pos[0], avatar_pos[1],
             avatar_pos[2]);
      printf("  Camera Pos (X, Y, Z): %.4f, %.4f, %.4f\n", camera_pos[0], camera_pos[1],
             camera_pos[2]);
      last_print_time = current_time;  // Reset the timer
    }
    // ---------------------------------------------------

    // if the listener function changed, it means the kernel ran it, so we should notify compiler.
    if (MasterDebug && ListenerFunction->value != old_listener) {
      SendAck();
    }

    if (time_ms < 4) {
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  }
}

/*!
 * Stop running the GOAL Kernel.
 * DONE, EXACT
 */
void KernelShutdown() {
  MasterExit = RuntimeExitStatus::EXIT;  // GOAL Kernel Dispatch loop will stop now.
}
}  // namespace jak1