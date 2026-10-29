// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>

namespace swarm
{
struct Vec3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;

	Vec3() = default;
	Vec3( float x_, float y_, float z_ ) : x( x_ ), y( y_ ), z( z_ ) {}
	Vec3 operator+( const Vec3& b ) const { return { x + b.x, y + b.y, z + b.z }; }
	Vec3 operator-( const Vec3& b ) const { return { x - b.x, y - b.y, z - b.z }; }
	Vec3 operator*( float s ) const { return { x * s, y * s, z * s }; }
	float Dot( const Vec3& b ) const { return x * b.x + y * b.y + z * b.z; }
	Vec3 Cross( const Vec3& b ) const { return { y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x }; }
	float Length() const { return std::sqrt( Dot( *this ) ); }
	Vec3 Normalized() const
	{
		float length = Length();
		return length > 1e-12f ? *this * ( 1.0f / length ) : Vec3( 0.0f, 0.0f, 1.0f );
	}
};

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

	// World to camera for a camera at `eye` looking at `target`, Y up.
	static Mat4 LookAt( const Vec3& eye, const Vec3& target )
	{
		Vec3 forward = ( target - eye ).Normalized();
		Vec3 side    = forward.Cross( Vec3( 0.0f, 1.0f, 0.0f ) );
		if( side.Length() < 1e-4f )// looking straight up or down
			side = forward.Cross( Vec3( 0.0f, 0.0f, -1.0f ) );
		side    = side.Normalized();
		Vec3 up = side.Cross( forward );
		Mat4 r  = Identity();
		r.m[ 0 ]  = side.x;
		r.m[ 4 ]  = side.y;
		r.m[ 8 ]  = side.z;
		r.m[ 1 ]  = up.x;
		r.m[ 5 ]  = up.y;
		r.m[ 9 ]  = up.z;
		r.m[ 2 ]  = -forward.x;
		r.m[ 6 ]  = -forward.y;
		r.m[ 10 ] = -forward.z;
		r.m[ 12 ] = -side.Dot( eye );
		r.m[ 13 ] = -up.Dot( eye );
		r.m[ 14 ] = forward.Dot( eye );
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
