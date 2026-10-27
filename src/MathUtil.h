// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>

namespace swarm
{
// Column-major 4x4 matrix, laid out the way glUniformMatrix4fv expects it.
struct Mat4
{
	float m[ 16 ];

	static Mat4 Identity()
	{
		Mat4 r{};
		r.m[ 0 ] = r.m[ 5 ] = r.m[ 10 ] = r.m[ 15 ] = 1.0f;
		return r;
	}

	static Mat4 Perspective( float fovYRadians, float aspect, float zNear, float zFar )
	{
		Mat4 r{};
		float f   = 1.0f / std::tan( fovYRadians * 0.5f );
		r.m[ 0 ]  = f / aspect;
		r.m[ 5 ]  = f;
		r.m[ 10 ] = ( zFar + zNear ) / ( zNear - zFar );
		r.m[ 11 ] = -1.0f;
		r.m[ 14 ] = 2.0f * zFar * zNear / ( zNear - zFar );
		return r;
	}

	static Mat4 Translation( float x, float y, float z )
	{
		Mat4 r    = Identity();
		r.m[ 12 ] = x;
		r.m[ 13 ] = y;
		r.m[ 14 ] = z;
		return r;
	}

	static Mat4 RotationX( float radians )
	{
		Mat4 r    = Identity();
		float c   = std::cos( radians ), s = std::sin( radians );
		r.m[ 5 ]  = c;
		r.m[ 6 ]  = s;
		r.m[ 9 ]  = -s;
		r.m[ 10 ] = c;
		return r;
	}

	static Mat4 RotationY( float radians )
	{
		Mat4 r    = Identity();
		float c   = std::cos( radians ), s = std::sin( radians );
		r.m[ 0 ]  = c;
		r.m[ 2 ]  = -s;
		r.m[ 8 ]  = s;
		r.m[ 10 ] = c;
		return r;
	}

	Mat4 operator*( const Mat4& b ) const
	{
		Mat4 r{};
		for( int col = 0; col < 4; ++col )
			for( int row = 0; row < 4; ++row )
				for( int k = 0; k < 4; ++k )
					r.m[ col * 4 + row ] += m[ k * 4 + row ] * b.m[ col * 4 + k ];
		return r;
	}
};
}// namespace swarm
