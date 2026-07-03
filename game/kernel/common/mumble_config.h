#pragma once

/*!
 * @file mumble_config.h
 * Persistence for the native Mumble client settings (server info, audio
 * devices, voice tuning). Stored as JSON in the user config dir
 * (<config>/OpenGOAL/mumble-native.json).
 *
 * Note: the server password is stored in plain text, like most game
 * voice-server configs.
 */

// Fill g_mumble_native_config / g_mumble_voice_config from disk. Safe to call
// repeatedly; only the first call reads the file.
void mumble_config_load();

// Write the current configs to disk.
void mumble_config_save();
