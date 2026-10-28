// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include "MeshSampler.h"

namespace swarm
{
// Whether this build can read Alembic files.
bool AlembicSupported();

// Reads every polygon and subdivision mesh in an Alembic (.abc) archive,
// with its parent transforms applied, and merges them into one mesh per frame.
// Archives without meshes fall back to their point objects as a point cloud.
//
// Frames are sampled at the archive's own frame rate across its full time
// range, which also lands in sequence.fps. A single frame means a static mesh.
// As with OBJ sequences the first frame defines the topology; frames with a
// different vertex count (fluid sims, remeshing) are dropped with a warning.
// The path is UTF-8 on every platform.
bool LoadAlembic( const std::string& path, MeshSequence& sequence, std::string& error, std::string& warning );
}// namespace swarm
