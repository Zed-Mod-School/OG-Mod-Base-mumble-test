#include "debug_gui.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/string_util.h"

#include <vector>

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/screenshot.h"
#include "game/kernel/common/mumble_config.h"
#include "game/kernel/common/mumble_link.h"
#include "game/kernel/common/mumble_native.h"
#include "game/kernel/common/mumble_voice.h"
#include "game/system/hid/sdl_util.h"

#include "fmt/core.h"
#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_style.h"

void FrameTimeRecorder::finish_frame() {
  m_frame_times[m_idx++] = m_compute_timer.getMs();
  if (m_idx == SIZE) {
    m_idx = 0;
  }
}

void FrameTimeRecorder::start_frame() {
  m_compute_timer.start();
  float frame_time = m_fps_timer.getSeconds();
  m_last_frame_time = (0.9 * m_last_frame_time) + (0.1 * frame_time);
  m_fps_timer.start();
}

void FrameTimeRecorder::draw_window(const DmaStats& /*dma_stats*/) {
  auto* p_open = &m_open;
  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

  const float PAD = 10.0f;
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 work_pos = viewport->WorkPos;  // Use work area to avoid menu-bar/task-bar, if any!
  ImVec2 work_size = viewport->WorkSize;
  ImVec2 window_pos, window_pos_pivot;
  window_pos.x = (work_pos.x + work_size.x - PAD);
  window_pos.y = (work_pos.y + work_size.y - PAD);
  window_pos_pivot.x = 1.0f;
  window_pos_pivot.y = 1.0f;
  ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);

  ImGui::SetNextWindowBgAlpha(0.85f);  // Transparent background
  if (ImGui::Begin("Frame Timing", p_open, window_flags)) {
    //    ImGui::Text("DMA: sync ms %.1f, tc %4d, sz %3d KB, ch %d", dma_stats.sync_time_ms,
    //                dma_stats.num_tags, (dma_stats.num_data_bytes) / (1 << 10),
    //                dma_stats.num_chunks);
    float worst = 0, total = 0;
    for (auto x : m_frame_times) {
      worst = std::max(x, worst);
      total += x;
    }
    if (total / SIZE > 17.) {
      ImGui::TextColored(ImVec4(1.0, 0.3, 0.3, 1.0), "avg: %.1f", total / SIZE);
    } else {
      ImGui::Text("avg: %.1f", total / SIZE);
    }
    ImGui::SameLine();
    if (worst > 17.) {
      ImGui::TextColored(ImVec4(1.0, 0.3, 0.3, 1.0), "worst: %.1f", worst);
    } else {
      ImGui::Text("worst: %.1f", worst);
    }
    ImGui::SameLine();
    ImGui::Text("fps-avg: %.1f", 1.f / m_last_frame_time);

    ImGui::Separator();
    ImGui::PlotLines(
        "0-20ms",
        [](void* data, int idx) {
          auto* me = (FrameTimeRecorder*)data;
          return me->m_frame_times[(me->m_idx + idx) % SIZE];
        },
        (void*)this, SIZE, 0, nullptr, 0, 20., ImVec2(300, 40));

    ImGui::Checkbox("Run", &m_play);
    ImGui::SameLine();
    if (ImGui::Button("Single Frame Advance")) {
      m_single_frame = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("GLFinish", &do_gl_finish);
  }
  ImGui::End();
}

void OpenGlDebugGui::start_frame() {
  m_frame_timer.start_frame();
}

void OpenGlDebugGui::finish_frame() {
  m_frame_timer.finish_frame();
}

void OpenGlDebugGui::draw(const DmaStats& dma_stats) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Debugging")) {
      ImGui::MenuItem("Frame Time Plot", nullptr, &m_draw_frame_time);
      ImGui::MenuItem("Render Debug", nullptr, &m_draw_debug);
      ImGui::MenuItem("Profiler", nullptr, &m_draw_profiler);
      ImGui::MenuItem("Small Profiler", nullptr, &small_profiler);
      ImGui::MenuItem("Loader", nullptr, &m_draw_loader);
      if (ImGui::MenuItem("Reboot In Debug Mode!")) {
        want_reboot_in_debug = true;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
      if (ImGui::BeginMenu("Screenshot")) {
        ImGui::MenuItem("Screenshot Next Frame!", nullptr, &m_want_screenshot);
        ImGui::InputText("File", g_screen_shot_settings->name,
                         sizeof(g_screen_shot_settings->name));
        ImGui::InputInt("Width", &g_screen_shot_settings->width);
        ImGui::InputInt("Height", &g_screen_shot_settings->height);
        ImGui::InputInt("MSAA", &g_screen_shot_settings->msaa);
        ImGui::Checkbox("Quick-Screenshot on F2", &screenshot_hotkey_enabled);
        ImGui::EndMenu();
      }
      ImGui::MenuItem("Subtitle Editor", nullptr, &m_subtitle_editor);
      ImGui::MenuItem("Debug Text Filter", nullptr, &m_filters_menu);
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Mumble")) {
      auto& tuning = g_mumble_link_tuning;
      const auto& status = g_mumble_link_status;

      if (status.connected) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Link: connected");
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Link: not connected");
      }
      ImGui::Separator();

      ImGui::Checkbox("Enabled", &tuning.enabled);
      ImGui::Checkbox("Override World Scale", &tuning.override_world_scale);
      ImGui::BeginDisabled(!tuning.override_world_scale);
      ImGui::SliderFloat("World Scale", &tuning.world_scale, 0.1f, 100.0f, "%.2f",
                         ImGuiSliderFlags_Logarithmic);
      ImGui::EndDisabled();
      ImGui::Text("GOAL scale: %.2f | effective: %.2f", status.goal_scale, status.effective_scale);
      ImGui::Checkbox("Mirror X (flip left/right audio)", &tuning.mirror_x);
      ImGui::SliderFloat("Reconnect Interval (s)", &tuning.retry_interval_s, 1.0f, 30.0f, "%.0f");

      ImGui::Separator();
      ImGui::Text("Avatar (m): %7.2f %7.2f %7.2f", status.avatar_pos[0], status.avatar_pos[1],
                  status.avatar_pos[2]);
      ImGui::Text("Camera (m): %7.2f %7.2f %7.2f", status.camera_pos[0], status.camera_pos[1],
                  status.camera_pos[2]);
      ImGui::Text("Updates sent: %u", status.updates_sent);

      ImGui::Separator();
      if (ImGui::TreeNode("Native Client (no Mumble app needed)")) {
        mumble_config_load();  // no-op after the first call
        bool save_settings = false;
        auto& cfg = g_mumble_native_config;
        auto native = mumble_native_get_status();
        ImGui::InputText("Server", cfg.host, sizeof(cfg.host));
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::InputInt("Port", &cfg.port);
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::InputText("Username", cfg.username, sizeof(cfg.username));
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::InputText("Password", cfg.password, sizeof(cfg.password),
                         ImGuiInputTextFlags_Password);
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        if (native.state == MumbleNativeState::Connected ||
            native.state == MumbleNativeState::Connecting) {
          if (ImGui::Button("Disconnect")) {
            mumble_native_disconnect();
          }
        } else {
          if (ImGui::Button("Connect")) {
            save_settings = true;
            mumble_native_connect();
          }
        }
        ImGui::SameLine();
        switch (native.state) {
          case MumbleNativeState::Connected:
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", native.message);
            break;
          case MumbleNativeState::Failed:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", native.message);
            break;
          default:
            ImGui::TextUnformatted(native.message);
            break;
        }
        if (native.state == MumbleNativeState::Connected) {
          ImGui::Text("session %u | %d user(s) | pos sent %u recv %u", native.session,
                      native.user_count, native.positions_sent, native.positions_received);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Voice");
        auto& vcfg = g_mumble_voice_config;
        auto voice = mumble_voice_get_status();

        // audio device pickers (cached; refresh on demand)
        static std::vector<MumbleVoiceDeviceInfo> s_in_devs, s_out_devs;
        static bool s_devs_loaded = false;
        if (!s_devs_loaded || ImGui::SmallButton("Refresh Devices")) {
          s_devs_loaded = true;
          s_in_devs.resize(32);
          s_in_devs.resize(mumble_voice_enum_devices(true, s_in_devs.data(), 32));
          s_out_devs.resize(32);
          s_out_devs.resize(mumble_voice_enum_devices(false, s_out_devs.data(), 32));
        }
        auto device_combo = [&save_settings](const char* label,
                                             const std::vector<MumbleVoiceDeviceInfo>& devs,
                                             char* cfg_id, size_t cfg_id_size) {
          const char* current = "(System Default)";
          for (const auto& d : devs) {
            if (strcmp(d.id, cfg_id) == 0) {
              current = d.name;
              break;
            }
          }
          if (ImGui::BeginCombo(label, current)) {
            if (ImGui::Selectable("(System Default)", cfg_id[0] == 0)) {
              cfg_id[0] = 0;
              save_settings = true;
            }
            for (const auto& d : devs) {
              if (ImGui::Selectable(d.name, strcmp(d.id, cfg_id) == 0)) {
                snprintf(cfg_id, cfg_id_size, "%s", d.id);
                save_settings = true;
              }
            }
            ImGui::EndCombo();
          }
        };
        device_combo("Microphone", s_in_devs, vcfg.input_device_id,
                     sizeof(vcfg.input_device_id));
        device_combo("Output Device", s_out_devs, vcfg.output_device_id,
                     sizeof(vcfg.output_device_id));

        save_settings |= ImGui::Checkbox("Mute Microphone", &vcfg.mic_muted);
        ImGui::SameLine();
        if (vcfg.mic_muted) {
          ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "muted");
        } else if (voice.transmitting) {
          ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "talking");
        } else {
          ImGui::TextUnformatted(voice.capture_running ? "quiet" : "mic off");
        }
        ImGui::ProgressBar(voice.mic_level * 10.f > 1.f ? 1.f : voice.mic_level * 10.f,
                           ImVec2(-1, 4), "");
        ImGui::SliderFloat("Mic Gate", &vcfg.mic_gate, 0.f, 0.2f, "%.3f");
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Volume", &vcfg.volume, 0.f, 3.f, "%.2f");
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        save_settings |= ImGui::Checkbox("Positional Audio", &vcfg.positional);
        ImGui::SliderFloat("Min Distance (m)", &vcfg.min_distance_m, 0.5f, 20.f, "%.1f");
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Max Distance (m)", &vcfg.max_distance_m, 5.f, 200.f, "%.1f");
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Falloff Aggression", &vcfg.falloff, 0.25f, 4.f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        save_settings |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::Text("%s | %d talking | tx %u rx %u", voice.message, voice.talking_peers,
                    voice.frames_sent, voice.frames_received);

        // per-user receive volumes for everyone else on the server
        char user_names[16][32];
        int user_count = mumble_native_get_user_list(user_names, 16);
        if (user_count > 0) {
          ImGui::Separator();
          ImGui::TextUnformatted("User Volumes");
          for (int i = 0; i < user_count; i++) {
            float vol = mumble_voice_get_user_volume(user_names[i]);
            if (ImGui::SliderFloat(user_names[i], &vol, 0.f, 2.f, "%.2f")) {
              mumble_voice_set_user_volume(user_names[i], vol);
            }
            save_settings |= ImGui::IsItemDeactivatedAfterEdit();
          }
        }

        if (save_settings) {
          mumble_config_save();
        }
        ImGui::TreePop();
      }

      ImGui::Separator();
      MumbleLinkPeer peers[kMaxMumblePeers];
      int peer_count = mumble_link_get_peers(peers);
      ImGui::Text("Voice peers: %d", peer_count);
      for (int i = 0; i < peer_count; i++) {
        // positions are raw game units; show meters for readability
        ImGui::Text("  %s: %.1f %.1f %.1f m", peers[i].name, peers[i].pos[0] / 4096.f,
                    peers[i].pos[1] / 4096.f, peers[i].pos[2] / 4096.f);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings")) {
      if (ImGui::TreeNode("ImGui Styling (restart required for these)")) {
        ImGui::InputInt("Font Size", &Gfx::g_debug_settings.imgui_font_size);
        ImGui::Checkbox("Monospaced Font", &Gfx::g_debug_settings.monospaced_font);
        if (ImGui::Checkbox("Alternate Style", &Gfx::g_debug_settings.alternate_style)) {
          if (Gfx::g_debug_settings.alternate_style) {
            ImGui::applyAlternateStyle();
          } else {
            ImGui::applyClassicStyle();
          }
        }
        ImGui::TreePop();
      }
      ImGui::Checkbox("Ignore Hide ImGui Bind", &Gfx::g_debug_settings.ignore_hide_imgui);
      if (ImGui::TreeNode("Frame Rate")) {
        ImGui::Checkbox("Framelimiter", &Gfx::g_global_settings.framelimiter);
        ImGui::InputFloat("Target FPS", &target_fps_input);
        if (ImGui::MenuItem("Apply")) {
          Gfx::g_global_settings.target_fps = target_fps_input;
        }
        ImGui::Separator();
        ImGui::Checkbox("Accurate Lag Mode", &Gfx::g_global_settings.experimental_accurate_lag);
        ImGui::Checkbox("Sleep in Frame Limiter", &Gfx::g_global_settings.sleep_in_frame_limiter);
        ImGui::TreePop();
      }
      ImGui::Checkbox("Treat Pad0 as Pad1", &Gfx::g_debug_settings.treat_pad0_as_pad1);
      auto is_keyboard_enabled =
          Display::GetMainDisplay()->get_input_manager()->is_keyboard_enabled();
      if (ImGui::Checkbox("Enable Keyboard (forced on if no controllers detected)",
                          &is_keyboard_enabled)) {
        Display::GetMainDisplay()->get_input_manager()->enable_keyboard(is_keyboard_enabled);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Event Profiler")) {
      if (ImGui::Checkbox("Record Events", &record_events)) {
        prof().set_enable(record_events);
      }
      ImGui::SameLine();
      ImGui::Text(fmt::format("({}/{})", prof().get_next_idx(), prof().get_max_events()).c_str());
      ImGui::InputInt("Event Buffer Size", &max_event_buffer_size);
      if (ImGui::Button("Resize")) {
        prof().update_event_buffer_size(max_event_buffer_size);
      }
      if (ImGui::Button("Reset Events")) {
        prof().clear();
      }
      ImGui::Separator();
      ImGui::Checkbox("Enable Compression", &prof().m_enable_compression);
      if (ImGui::Button("Dump to File")) {
        record_events = false;
        prof().dump_to_json();
      }
      // if (ImGui::Button("Open dump folder")) {
      //  // TODO - https://github.com/mlabbe/nativefiledialog
      // }
      ImGui::EndMenu();
    }

    if (!Gfx::g_debug_settings.ignore_hide_imgui) {
      ImGui::Text("%s", fmt::format("Toggle toolbar with {}",
                                    sdl_util::get_keyboard_button_name(
                                        Gfx::g_debug_settings.hide_imgui_key, InputModifiers()))
                            .c_str());
    }
  }
  ImGui::EndMainMenuBar();

  if (m_draw_frame_time) {
    m_frame_timer.draw_window(dma_stats);
  }
}
