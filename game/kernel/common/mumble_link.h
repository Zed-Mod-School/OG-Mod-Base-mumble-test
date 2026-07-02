#pragma once

/*!
 * @file mumble_link.h
 * Mumble "Link" positional-audio integration for proximity voice chat.
 * Writes the local player's position/orientation into the shared memory block
 * that a running Mumble client polls. Protocol: https://wiki.mumble.info/wiki/Link
 */

// Push one frame of positional data to Mumble. All positions are in meters,
// all direction vectors are unit length, using Mumble's convention:
// X = right, Y = up, Z = front.
// Safe to call even if Mumble isn't running - it connects lazily and retries.
void mumble_link_update(const float avatar_pos[3],
                        const float avatar_front[3],
                        const float avatar_top[3],
                        const float camera_pos[3],
                        const float camera_front[3],
                        const float camera_top[3]);
