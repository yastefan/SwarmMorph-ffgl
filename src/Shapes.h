// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MeshSampler.h"

namespace swarm
{
// Built-in shapes for the Shape menus, so the plugin does something without
// any files. Each is a real triangle mesh, roughly filling the unit sphere,
// so lighting, anchors and morphing work as with a loaded OBJ. The animated
// ones come as a seamless looping sequence, like an animated file.
int ShapeCount();
const char* ShapeName( int shape );
MeshSequence MakeShape( int shape );
}// namespace swarm
