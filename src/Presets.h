// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string>

namespace swarm
{
// The Resolume presets from presets/, built into the plugin.
struct EmbeddedPreset
{
	const char* fileName;
	const unsigned char* data;
	size_t size;
};
extern const EmbeddedPreset kEmbeddedPresets[];
extern const size_t kEmbeddedPresetCount;

// Copies the built-in presets into Resolume's preset folder for this source
// (Arena and Avenue, whichever exist), so they show up in the preset menu.
// Each preset is installed once: files that are already there are never
// overwritten, and a preset the user deleted doesn't come back. Returns a
// message for the host log, empty when nothing was installed.
std::string InstallPresets();
}// namespace swarm
