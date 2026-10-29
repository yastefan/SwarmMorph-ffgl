// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraPaths.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace swarm
{
namespace
{
const float kPi            = 3.14159265358979f;
const float kDefaultFov    = 40.0f;
const float kHalfHeightAt1 = std::tan( kDefaultFov * 0.5f * kPi / 180.0f ) * 3.75f;// framing of the default camera
const int kSamples         = 1024;

// A control point. Focus 1 keeps the camera on the centre of the shape; 0
// lets it look where it is going, for flying through. Pace below 1 slows the
// camera down around this point.
struct Key
{
	Vec3 eye;
	float focus  = 1.0f;
	float fovDeg = kDefaultFov;
	float pace   = 1.0f;
};

struct Path
{
	const char* name;
	std::vector< Key > keys;
};

Vec3 OnSphere( float radius, float elevationDeg, float azimuthDeg, float lift = 0.0f )
{
	float e = elevationDeg * kPi / 180.0f, a = azimuthDeg * kPi / 180.0f;
	return Vec3( radius * std::cos( e ) * std::sin( a ), radius * std::sin( e ) + lift, radius * std::cos( e ) * std::cos( a ) );
}

std::vector< Path > BuildPaths()
{
	std::vector< Path > paths;

	Path orbit{ "Orbit", {} };
	for( int i = 0; i < 8; ++i )
		orbit.keys.push_back( { OnSphere( 3.7f, 9.0f, i * 45.0f ) } );
	paths.push_back( orbit );

	// Circles the shape, swinging over and under it twice per loop.
	Path wave{ "Wave Orbit", {} };
	for( int i = 0; i < 12; ++i )
	{
		float a = i * 30.0f * kPi / 180.0f;
		wave.keys.push_back( { Vec3( 3.6f * std::sin( a ), 1.8f * std::sin( 2.0f * a ), 3.6f * std::cos( a ) ) } );
	}
	paths.push_back( wave );

	// Two turns rising from below to above, then two turns back down.
	Path spiral{ "Spiral", {} };
	for( int i = 0; i < 16; ++i )
	{
		float a = i * 45.0f * kPi / 180.0f;
		float h = -std::cos( 2.0f * kPi * i / 16.0f );
		float r = 3.5f - 0.5f * h * h;
		spiral.keys.push_back( { Vec3( r * std::sin( a ), 2.1f * h, r * std::cos( a ) ) } );
	}
	paths.push_back( spiral );

	// Glides into the middle, slows down and turns round inside the swarm,
	// drifts out backwards while still looking at it, and swings round the
	// side to the front again.
	paths.push_back( { "Fly Through",
					   { { Vec3( 0.0f, 0.3f, 5.0f ), 1.0f, 40.0f, 1.0f },
						 { Vec3( 0.35f, 0.15f, 2.4f ), 0.6f, 44.0f, 0.8f },
						 { Vec3( 0.45f, 0.05f, 0.4f ), 0.0f, 54.0f, 0.45f },
						 { Vec3( 0.7f, 0.0f, -1.0f ), 0.4f, 56.0f, 0.4f },
						 { Vec3( 1.2f, 0.05f, -1.9f ), 1.0f, 52.0f, 0.3f },
						 { Vec3( 2.6f, 0.4f, -2.5f ), 1.0f, 46.0f, 1.0f },
						 { Vec3( 4.0f, 0.9f, 0.0f ), 1.0f, 40.0f, 1.0f },
						 { Vec3( 2.8f, 0.7f, 3.8f ), 1.0f, 40.0f, 1.0f } } } );

	// Skims close past the surface, then pulls far back and comes in again.
	Path close{ "Close Pass", {} };
	const float radius[]    = { 1.8f, 1.7f, 1.9f, 3.0f, 5.2f, 6.0f, 4.6f, 2.6f };
	const float height[]    = { -0.15f, 0.0f, 0.25f, 0.7f, 1.5f, 1.8f, 1.1f, 0.3f };
	const float fov[]       = { 50.0f, 50.0f, 48.0f, 44.0f, 38.0f, 36.0f, 40.0f, 46.0f };
	for( int i = 0; i < 8; ++i )
	{
		float a = i * 45.0f * kPi / 180.0f;
		close.keys.push_back( { Vec3( radius[ i ] * std::sin( a ), height[ i ], radius[ i ] * std::cos( a ) ), 1.0f, fov[ i ] } );
	}
	paths.push_back( close );

	// Rises from just below the shape to almost straight above it and back.
	Path crane{ "Crane", {} };
	const float elevation[] = { -15.0f, 5.0f, 30.0f, 60.0f, 75.0f, 60.0f, 30.0f, 5.0f };
	const float azimuth[]   = { 0.0f, 10.0f, 25.0f, 40.0f, 55.0f, 40.0f, 25.0f, 10.0f };
	const float distance[]  = { 3.6f, 3.7f, 3.9f, 4.2f, 4.4f, 4.2f, 3.9f, 3.7f };
	for( int i = 0; i < 8; ++i )
		crane.keys.push_back( { OnSphere( distance[ i ], elevation[ i ], azimuth[ i ] ) } );
	paths.push_back( crane );

	// Dolly zoom: the camera moves in while the lens widens, so the shape keeps
	// its size and the space around it stretches.
	Path vertigo{ "Vertigo", {} };
	const float depth[] = { 6.0f, 5.0f, 3.6f, 2.4f, 1.7f, 2.4f, 3.6f, 5.0f };
	for( int i = 0; i < 8; ++i )
	{
		float a   = i * 45.0f * kPi / 180.0f;
		float fov = 2.0f * std::atan( kHalfHeightAt1 / depth[ i ] ) * 180.0f / kPi;
		vertigo.keys.push_back( { Vec3( 0.35f * std::sin( a ), 0.2f * std::cos( a ), depth[ i ] ), 1.0f, fov } );
	}
	paths.push_back( vertigo );

	return paths;
}

float CatmullRom( float a, float b, float c, float d, float t )
{
	return 0.5f * ( 2.0f * b + ( c - a ) * t + ( 2.0f * a - 5.0f * b + 4.0f * c - d ) * t * t + ( 3.0f * b - a - 3.0f * c + d ) * t * t * t );
}

// Monotone cubic between b and c (a and d are the neighbours): smooth like a
// spline, but never overshoots, keeps moving through in-between keys and
// comes to rest softly where the values level off.
float Monotone( float a, float b, float c, float d, float t )
{
	auto slope = []( float before, float after ) {
		return before * after <= 0.0f ? 0.0f : 2.0f / ( 1.0f / before + 1.0f / after );
	};
	float mb = slope( b - a, c - b ), mc = slope( c - b, d - c );
	float t2 = t * t, t3 = t2 * t;
	return ( 2.0f * t3 - 3.0f * t2 + 1.0f ) * b + ( t3 - 2.0f * t2 + t ) * mb + ( -2.0f * t3 + 3.0f * t2 ) * c + ( t3 - t2 ) * mc;
}

float Wrap( float angle )
{
	return std::remainder( angle, 2.0f * kPi );
}

// The path sampled densely: where the camera is, where it looks and how wide,
// plus the (pace-weighted) distance travelled so the fader moves it evenly.
struct Table
{
	std::vector< Vec3 > eye, view;
	std::vector< float > fov, travelled;
};

Table Sample( const Path& path )
{
	const std::vector< Key >& k = path.keys;
	int count = static_cast< int >( k.size() );
	Table table;
	std::vector< float > focus, pace;
	for( int j = 0; j < kSamples; ++j )
	{
		float u = static_cast< float >( j ) / kSamples * count;
		int i   = std::min( static_cast< int >( u ), count - 1 );
		float t = u - i;
		const Key& a = k[ ( i + count - 1 ) % count ];
		const Key& b = k[ i ];
		const Key& c = k[ ( i + 1 ) % count ];
		const Key& d = k[ ( i + 2 ) % count ];
		table.eye.push_back( Vec3( CatmullRom( a.eye.x, b.eye.x, c.eye.x, d.eye.x, t ), CatmullRom( a.eye.y, b.eye.y, c.eye.y, d.eye.y, t ),
								   CatmullRom( a.eye.z, b.eye.z, c.eye.z, d.eye.z, t ) ) );
		table.fov.push_back( Monotone( a.fovDeg, b.fovDeg, c.fovDeg, d.fovDeg, t ) * kPi / 180.0f );
		focus.push_back( Monotone( a.focus, b.focus, c.focus, d.focus, t ) );
		pace.push_back( Monotone( a.pace, b.pace, c.pace, d.pace, t ) );
	}

	// View direction: between where the camera is heading and the centre of
	// the shape, by turning (yaw and tilt) rather than blending points, which
	// also works when the two are opposite. The turn angle is unwrapped from
	// sample to sample, so the way round never flips.
	float previousTurn = 0.0f;
	for( int j = 0; j < kSamples; ++j )
	{
		const Vec3& eye = table.eye[ j ];
		Vec3 heading    = ( table.eye[ ( j + 1 ) % kSamples ] - table.eye[ ( j + kSamples - 1 ) % kSamples ] ).Normalized();
		Vec3 inward     = eye.Length() > 1e-3f ? ( Vec3() - eye ).Normalized() : heading;
		float yawA = std::atan2( heading.x, heading.z ), pitchA = std::asin( std::min( std::max( heading.y, -1.0f ), 1.0f ) );
		float yawB = std::atan2( inward.x, inward.z ), pitchB = std::asin( std::min( std::max( inward.y, -1.0f ), 1.0f ) );
		float turn = Wrap( yawB - yawA );
		if( j > 0 )
			turn = previousTurn + Wrap( turn - previousTurn );
		previousTurn = turn;
		float yaw    = yawA + turn * focus[ j ];
		float pitch = pitchA + ( pitchB - pitchA ) * focus[ j ];
		table.view.push_back( Vec3( std::cos( pitch ) * std::sin( yaw ), std::sin( pitch ), std::cos( pitch ) * std::cos( yaw ) ) );
	}

	// Close the loop: the last entry repeats the first.
	table.eye.push_back( table.eye[ 0 ] );
	table.view.push_back( table.view[ 0 ] );
	table.fov.push_back( table.fov[ 0 ] );
	pace.push_back( pace[ 0 ] );
	table.travelled.push_back( 0.0f );
	for( int j = 1; j <= kSamples; ++j )
	{
		float step = ( table.eye[ j ] - table.eye[ j - 1 ] ).Length();
		table.travelled.push_back( table.travelled.back() + step / std::max( 0.5f * ( pace[ j ] + pace[ j - 1 ] ), 0.05f ) );
	}
	return table;
}

const std::vector< Path >& Paths()
{
	static const std::vector< Path > paths = BuildPaths();
	return paths;
}

const std::vector< Table >& Tables()
{
	static const std::vector< Table > tables = [] {
		std::vector< Table > result;
		for( const Path& path : Paths() )
			result.push_back( Sample( path ) );
		return result;
	}();
	return tables;
}
}// namespace

int CameraPathCount()
{
	return static_cast< int >( Paths().size() );
}

const char* CameraPathName( int path )
{
	return path >= 1 && path <= CameraPathCount() ? Paths()[ path - 1 ].name : "Off";
}

CameraPose EvaluateCameraPath( int path, float position )
{
	const Table& table = Tables()[ std::min( std::max( path, 1 ), CameraPathCount() ) - 1 ];
	position   = position - std::floor( position );
	float goal = position * table.travelled.back();
	size_t hi  = std::upper_bound( table.travelled.begin(), table.travelled.end(), goal ) - table.travelled.begin();
	hi         = std::min( std::max< size_t >( hi, 1 ), table.travelled.size() - 1 );
	size_t lo  = hi - 1;
	float span = table.travelled[ hi ] - table.travelled[ lo ];
	float t    = span > 1e-6f ? ( goal - table.travelled[ lo ] ) / span : 0.0f;

	CameraPose pose;
	pose.eye    = table.eye[ lo ] + ( table.eye[ hi ] - table.eye[ lo ] ) * t;
	pose.fovY   = table.fov[ lo ] + ( table.fov[ hi ] - table.fov[ lo ] ) * t;
	Vec3 view   = ( table.view[ lo ] + ( table.view[ hi ] - table.view[ lo ] ) * t ).Normalized();
	pose.target = pose.eye + view * 3.0f;
	return pose;
}
}// namespace swarm
