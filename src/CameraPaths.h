// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MathUtil.h"

namespace swarm
{
// Camera moves along closed splines, so position 0 and 1 are the same shot
// and a looping timeline never jumps. Paths are laid out for a shape inside
// the unit sphere, seen from the default camera distance.
struct CameraPose
{
	Vec3 eye;
	Vec3 target;
	float fovY = 0.0f;// radians
};

int CameraPathCount();// not counting "Off"
const char* CameraPathName( int path );// 1-based; 0 is "Off"

// `position` in [0, 1) runs along the path at constant speed.
CameraPose EvaluateCameraPath( int path, float position );
}// namespace swarm
