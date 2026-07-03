/*!
 * @file mumble_config.cpp
 * JSON persistence for the native Mumble client. See mumble_config.h.
 */

#include "mumble_config.h"

#include <mutex>
#include <string>

#include "common/util/FileUtil.h"

#include "game/kernel/common/mumble_native.h"
#include "game/kernel/common/mumble_voice.h"

#include "third-party/json.hpp"

namespace {

using json = nlohmann::json;

fs::path config_path() {
  return file_util::get_user_config_dir() / "mumble-native.json";
}

void copy_str(char* dst, size_t dst_size, const json& j, const char* key) {
  if (j.contains(key) && j[key].is_string()) {
    snprintf(dst, dst_size, "%s", j[key].get<std::string>().c_str());
  }
}

template <typename T>
void copy_num(T* dst, const json& j, const char* key) {
  if (j.contains(key) && j[key].is_number()) {
    *dst = j[key].get<T>();
  }
}

void copy_bool(bool* dst, const json& j, const char* key) {
  if (j.contains(key) && j[key].is_boolean()) {
    *dst = j[key].get<bool>();
  }
}

std::once_flag g_load_once;

void load_impl() {
  auto path = config_path();
  if (!fs::exists(path)) {
    return;
  }
  json j;
  try {
    j = json::parse(file_util::read_text_file(path));
  } catch (const std::exception&) {
    return;  // corrupt config - keep defaults, it'll be rewritten on save
  }

  auto& n = g_mumble_native_config;
  copy_str(n.host, sizeof(n.host), j, "host");
  copy_num(&n.port, j, "port");
  copy_str(n.username, sizeof(n.username), j, "username");
  copy_str(n.password, sizeof(n.password), j, "password");

  auto& v = g_mumble_voice_config;
  copy_bool(&v.mic_muted, j, "mic_muted");
  copy_num(&v.mic_gate, j, "mic_gate");
  copy_num(&v.volume, j, "volume");
  copy_bool(&v.positional, j, "positional");
  copy_num(&v.min_distance_m, j, "min_distance_m");
  copy_num(&v.max_distance_m, j, "max_distance_m");
  copy_num(&v.falloff, j, "falloff");
  copy_str(v.input_device_id, sizeof(v.input_device_id), j, "input_device_id");
  copy_str(v.output_device_id, sizeof(v.output_device_id), j, "output_device_id");

  if (j.contains("user_volumes") && j["user_volumes"].is_object()) {
    for (auto& [name, vol] : j["user_volumes"].items()) {
      if (vol.is_number()) {
        mumble_voice_set_user_volume(name.c_str(), vol.get<float>());
      }
    }
  }
}

}  // namespace

void mumble_config_load() {
  std::call_once(g_load_once, load_impl);
}

void mumble_config_save() {
  const auto& n = g_mumble_native_config;
  const auto& v = g_mumble_voice_config;
  json j = {
      {"host", n.host},
      {"port", n.port},
      {"username", n.username},
      {"password", n.password},
      {"mic_muted", v.mic_muted},
      {"mic_gate", v.mic_gate},
      {"volume", v.volume},
      {"positional", v.positional},
      {"min_distance_m", v.min_distance_m},
      {"max_distance_m", v.max_distance_m},
      {"falloff", v.falloff},
      {"input_device_id", v.input_device_id},
      {"output_device_id", v.output_device_id},
  };
  json volumes = json::object();
  for (auto& [name, vol] : mumble_voice_get_user_volumes()) {
    volumes[name] = vol;
  }
  j["user_volumes"] = volumes;
  auto path = config_path();
  file_util::create_dir_if_needed_for_file(path);
  file_util::write_text_file(path, j.dump(2));
}
