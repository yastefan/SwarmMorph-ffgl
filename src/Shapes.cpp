// SPDX-License-Identifier: GPL-3.0-or-later
#include "Shapes.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace swarm
{
namespace
{
const float kPi = 3.14159265358979f;
const float kTau = 2.0f * kPi;

// Animated shapes: one seamless 6-second cycle, played as a looping
// sequence; slow, so the swarm can follow the surface.
const int kCycleFrames = 144;
const float kCycleFps  = 24.0f;

struct Point
{
	float x, y, z;
};

Point operator+( Point a, Point b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Point operator-( Point a, Point b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Point operator*( Point a, float s ) { return { a.x * s, a.y * s, a.z * s }; }
Point Cross( Point a, Point b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Point Normalized( Point a )
{
	float length = std::sqrt( a.x * a.x + a.y * a.y + a.z * a.z );
	return length > 1e-12f ? a * ( 1.0f / length ) : a;
}

// A grid of (columns + 1) x (rows + 1) vertices from f(u, v), u and v in
// [0, 1]. A closed direction shares its first and last vertices, so the
// normals blend across the seam. The surface faces along df/du x df/dv;
// flip turns it round.
void AddGrid( Mesh& mesh, int columns, int rows, bool closedU, bool closedV, const std::function< Point( float, float ) >& f,
			  bool flip = false )
{
	uint32_t base = static_cast< uint32_t >( mesh.positions.size() / 3 );
	int uCount    = closedU ? columns : columns + 1;
	int vCount    = closedV ? rows : rows + 1;
	for( int j = 0; j < vCount; ++j )
		for( int i = 0; i < uCount; ++i )
		{
			Point p = f( static_cast< float >( i ) / columns, static_cast< float >( j ) / rows );
			mesh.positions.insert( mesh.positions.end(), { p.x, p.y, p.z } );
		}
	auto index = [ & ]( int i, int j ) {
		return base + static_cast< uint32_t >( ( j % vCount ) * uCount + ( i % uCount ) );
	};
	for( int j = 0; j < rows; ++j )
		for( int i = 0; i < columns; ++i )
		{
			uint32_t a = index( i, j ), b = index( i + 1, j ), c = index( i + 1, j + 1 ), d = index( i, j + 1 );
			if( flip )
				mesh.triangles.insert( mesh.triangles.end(), { a, c, b, a, d, c } );
			else
				mesh.triangles.insert( mesh.triangles.end(), { a, b, c, a, c, d } );
		}
}

// A tube around a closed or open curve, its radius varying along it (v)
// and around it (u).
void AddTube( Mesh& mesh, int segments, int sides, const std::function< float( float, float ) >& radius, bool closed,
			  const std::function< Point( float ) >& curve )
{
	AddGrid( mesh, sides, segments, true, closed, [ & ]( float u, float v ) {
		const float e = 1e-3f;
		Point centre  = curve( v );
		Point tangent = Normalized( curve( v + e ) - curve( v - e ) );
		Point up      = std::fabs( tangent.y ) < 0.9f ? Point{ 0.0f, 1.0f, 0.0f } : Point{ 1.0f, 0.0f, 0.0f };
		Point side    = Normalized( Cross( tangent, up ) );
		Point normal  = Cross( side, tangent );
		float angle   = u * 2.0f * kPi;
		return centre + ( side * std::cos( angle ) + normal * std::sin( angle ) ) * radius( u, v );
	}, true );
}

void AddTube( Mesh& mesh, int segments, int sides, float radius, bool closed, const std::function< Point( float ) >& curve )
{
	AddTube( mesh, segments, sides, [ radius ]( float, float ) { return radius; }, closed, curve );
}

// A sphere of the given resolution, each point moved by place(n), where n is
// the point's direction from the centre.
void AddSphere( Mesh& mesh, const std::function< Point( Point ) >& place, int columns = 64, int rows = 32 )
{
	AddGrid( mesh, columns, rows, true, false, [ & ]( float u, float v ) {
		float theta = u * kTau, phi = v * kPi;
		return place( { std::sin( phi ) * std::cos( theta ), std::cos( phi ), -std::sin( phi ) * std::sin( theta ) } );
	}, true );
}

// A flat disc at height y facing up (or down).
void AddDisc( Mesh& mesh, float y, float radius, bool up )
{
	AddGrid( mesh, 48, 1, true, false, [ & ]( float u, float v ) {
		float angle = ( up ? -u : u ) * 2.0f * kPi;
		float r     = radius * ( 1.0f - v );
		return Point{ r * std::cos( angle ), y, r * std::sin( angle ) };
	} );
}

Mesh Sphere( float )
{
	Mesh mesh;
	AddSphere( mesh, []( Point n ) { return n; } );
	return mesh;
}

Mesh Cube( float )
{
	Mesh mesh;
	const float s = 0.62f;
	// Six faces with their own vertices, so the edges stay sharp.
	const Point axes[ 6 ][ 3 ] = { { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
								   { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
								   { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
	for( const auto& face : axes )
		AddGrid( mesh, 8, 8, false, false, [ & ]( float u, float v ) {
			return ( face[ 0 ] + face[ 1 ] * ( 2.0f * u - 1.0f ) + face[ 2 ] * ( 2.0f * v - 1.0f ) ) * s;
		} );
	return mesh;
}

Mesh Torus( float )
{
	Mesh mesh;
	AddTube( mesh, 96, 32, 0.3f, true, []( float t ) {
		float angle = t * 2.0f * kPi;
		return Point{ 0.7f * std::cos( angle ), 0.0f, 0.7f * std::sin( angle ) };
	} );
	return mesh;
}

Mesh Knot( float )
{
	Mesh mesh;
	// A (2, 3) torus knot: the trefoil.
	AddTube( mesh, 256, 24, 0.16f, true, []( float t ) {
		float angle = t * 2.0f * kPi;
		float r     = 0.55f + 0.25f * std::cos( 3.0f * angle );
		return Point{ r * std::cos( 2.0f * angle ), 0.25f * std::sin( 3.0f * angle ), r * std::sin( 2.0f * angle ) };
	} );
	return mesh;
}

Mesh Cylinder( float )
{
	Mesh mesh;
	const float r = 0.55f, h = 0.8f;
	AddGrid( mesh, 64, 8, true, false, [ & ]( float u, float v ) {
		float angle = -u * 2.0f * kPi;
		return Point{ r * std::cos( angle ), h * ( 2.0f * v - 1.0f ), r * std::sin( angle ) };
	} );
	AddDisc( mesh, h, r, true );
	AddDisc( mesh, -h, r, false );
	return mesh;
}

Mesh Cone( float )
{
	Mesh mesh;
	const float r = 0.75f, h = 0.75f;
	AddGrid( mesh, 64, 8, true, false, [ & ]( float u, float v ) {
		float angle = -u * 2.0f * kPi;
		float radius = r * ( 1.0f - v );
		return Point{ radius * std::cos( angle ), h * ( 2.0f * v - 1.0f ), radius * std::sin( angle ) };
	} );
	AddDisc( mesh, -h, r, false );
	return mesh;
}

Mesh Helix( float )
{
	Mesh mesh;
	AddTube( mesh, 320, 16, 0.12f, false, []( float t ) {
		float angle = t * 3.0f * 2.0f * kPi;
		return Point{ 0.55f * std::cos( angle ), 1.6f * t - 0.8f, 0.55f * std::sin( angle ) };
	} );
	return mesh;
}

Mesh Wave( float )
{
	Mesh mesh;
	AddGrid( mesh, 96, 96, false, false, []( float u, float v ) {
		float x = 2.0f * u - 1.0f, z = 2.0f * v - 1.0f;
		float r = std::sqrt( x * x + z * z );
		return Point{ 0.9f * x, 0.18f * std::cos( 7.0f * r ), 0.9f * z };
	}, true );
	return mesh;
}

Mesh Plane( float )
{
	Mesh mesh;
	// Facing the camera, like a screen.
	AddGrid( mesh, 32, 18, false, false, []( float u, float v ) {
		return Point{ 0.96f * ( 2.0f * u - 1.0f ), 0.54f * ( 2.0f * v - 1.0f ), 0.0f };
	} );
	return mesh;
}

// The animated shapes take t, the time through their cycle in [0, 1); every
// motion repeats a whole number of times per cycle, so the loop is seamless.

// One beat of a heart: a strong "lub" and a softer "dub", 0 to 1, for the
// phase p in [0, 1).
float HeartPulse( float p )
{
	auto peak = [ p ]( float centre, float width ) {
		float d = p - centre;
		d -= std::floor( d + 0.5f );// nearest way round the cycle
		return std::exp( -d * d / ( width * width ) );
	};
	return peak( 0.1f, 0.09f ) + 0.6f * peak( 0.36f, 0.1f );
}

Mesh Heartbeat( float t )
{
	Mesh mesh;
	// Four slow beats per cycle; the sphere swells a little wider than tall.
	float beat = HeartPulse( std::fmod( 4.0f * t, 1.0f ) );
	AddSphere( mesh, [ beat ]( Point n ) {
		float swell = 0.3f * beat;
		return Point{ n.x * ( 1.0f + swell ), n.y * ( 1.0f + 0.7f * swell ), n.z * ( 1.0f + swell ) } * 0.75f;
	} );
	return mesh;
}

Mesh Superform( float t )
{
	Mesh mesh;
	// A superquadric |x|^p + |y|^p + |z|^p = 1: sphere at p = 2, a rounded
	// cube above, an octahedron at 1 and a spiky star below. The cycle runs
	// sphere, cube, sphere, octahedron, star and back.
	float p     = 2.0f * std::exp( 1.2f * std::sin( kTau * t ) );
	float reach = std::max( 1.0f, std::pow( 3.0f, 0.5f - 1.0f / p ) );// the farthest point, to keep the size steady
	AddSphere( mesh, [ p, reach ]( Point n ) {
		float sum = std::pow( std::fabs( n.x ), p ) + std::pow( std::fabs( n.y ), p ) + std::pow( std::fabs( n.z ), p );
		return n * ( 0.9f * std::pow( sum, -1.0f / p ) / reach );
	}, 64, 48 );
	return mesh;
}

Mesh Jellyfish( float t )
{
	Mesh mesh;
	// Two strokes per cycle: the bell squeezes quickly, then relaxes slowly,
	// and rises a little with each stroke.
	float phase    = std::fmod( 2.0f * t, 1.0f );
	float squeeze  = phase < 0.25f ? std::sin( phase / 0.25f * kPi * 0.5f ) : 0.5f + 0.5f * std::cos( ( phase - 0.25f ) / 0.75f * kPi );
	float rise     = 0.06f * std::sin( kTau * phase - 1.0f );
	float opening  = 1.75f - 0.45f * squeeze;// how far the bell reaches down, from the top
	float width    = 0.75f - 0.15f * squeeze;
	AddGrid( mesh, 64, 24, true, false, [ & ]( float u, float v ) {
		float theta = u * kTau, phi = v * opening;
		return Point{ width * std::sin( phi ) * std::cos( theta ), 0.45f * std::cos( phi ) + 0.25f + rise,
					  -width * std::sin( phi ) * std::sin( theta ) };
	}, true );
	// Tentacles hanging from the rim, a wave running down each.
	float rimY = 0.45f * std::cos( opening ) + 0.25f + rise, rimR = 0.85f * width * std::sin( opening );
	for( int k = 0; k < 8; ++k )
	{
		float angle = ( k + 0.5f ) * kTau / 8.0f;
		AddTube( mesh, 48, 8, []( float, float v ) { return 0.045f * ( 1.0f - 0.7f * v ); }, false, [ = ]( float v ) {
			float sway = 0.1f * v * std::sin( 5.0f * v - kTau * 2.0f * t + angle );
			float r    = rimR * ( 1.0f - 0.35f * v ) + sway;
			return Point{ r * std::cos( angle ), rimY - 1.0f * v, r * std::sin( angle ) };
		} );
	}
	return mesh;
}

Mesh Bloom( float t )
{
	Mesh mesh;
	// Six petals opening flat and closing to a bud once per cycle.
	float open     = 0.5f - 0.5f * std::cos( kTau * t );
	float lean     = 0.25f + 1.15f * open;// the petals' angle from upright
	const int count = 6;
	for( int k = 0; k < count; ++k )
	{
		float a     = k * kTau / count;
		Point out   = { std::cos( a ), 0.0f, std::sin( a ) };
		Point side  = { -std::sin( a ), 0.0f, std::cos( a ) };
		Point up    = { 0.0f, 1.0f, 0.0f };
		AddGrid( mesh, 12, 24, false, false, [ & ]( float u, float v ) {
			// The petal curls outwards towards its tip, and is cupped across.
			float beta   = lean * ( 0.6f + 0.6f * v );
			Point centre = ( out * std::sin( beta ) + up * std::cos( beta ) ) * ( 0.85f * v ) + out * 0.08f;
			Point inward = up * std::sin( beta ) - out * std::cos( beta );
			float across = 2.0f * u - 1.0f;
			float width  = 0.32f * std::sin( kPi * std::pow( v, 0.7f ) );
			return centre + side * ( width * across ) + inward * ( 0.12f * width * across * across / 0.32f ) + up * -0.3f;
		} );
	}
	// The heart of the flower.
	AddSphere( mesh, []( Point n ) { return Point{ n.x * 0.14f, n.y * 0.1f - 0.27f, n.z * 0.14f }; }, 24, 12 );
	return mesh;
}

Mesh Ocean( float t )
{
	Mesh mesh;
	// Swells from several directions, each a whole number of periods per
	// cycle, with crests drawn sharp the way water waves are.
	struct Swell
	{
		float kx, kz, height, cycles, phase;
	};
	const Swell swells[] = { { 3.0f, 0.8f, 0.07f, 1.0f, 0.0f },
							 { -1.2f, 3.6f, 0.05f, 1.0f, 1.3f },
							 { 4.5f, -3.0f, 0.03f, 2.0f, 2.1f },
							 { -6.0f, -5.0f, 0.015f, 3.0f, 0.7f } };
	AddGrid( mesh, 96, 96, false, false, [ & ]( float u, float v ) {
		float x = 0.95f * ( 2.0f * u - 1.0f ), z = 0.95f * ( 2.0f * v - 1.0f );
		Point p = { x, 0.0f, z };
		for( const Swell& w : swells )
		{
			float k     = std::sqrt( w.kx * w.kx + w.kz * w.kz );
			float angle = w.kx * x + w.kz * z - kTau * w.cycles * t + w.phase;
			p.y += w.height * std::cos( angle );
			p.x -= 0.6f * w.height * w.kx / k * std::sin( angle );
			p.z -= 0.6f * w.height * w.kz / k * std::sin( angle );
		}
		return p;
	}, true );
	return mesh;
}

struct ShapeEntry
{
	const char* name;
	Mesh ( *make )( float );
	bool animated;
};

const ShapeEntry kShapes[] = { { "Sphere", Sphere, false },         { "Cube", Cube, false },           { "Torus", Torus, false },
							   { "Knot", Knot, false },             { "Cylinder", Cylinder, false },   { "Cone", Cone, false },
							   { "Helix", Helix, false },           { "Wave", Wave, false },           { "Plane", Plane, false },
							   { "Heartbeat", Heartbeat, true },    { "Superform", Superform, true },  { "Jellyfish", Jellyfish, true },
							   { "Bloom", Bloom, true },            { "Ocean", Ocean, true } };
}// namespace

int ShapeCount()
{
	return static_cast< int >( sizeof( kShapes ) / sizeof( kShapes[ 0 ] ) );
}

const char* ShapeName( int shape )
{
	return shape >= 0 && shape < ShapeCount() ? kShapes[ shape ].name : "";
}

MeshSequence MakeShape( int shape )
{
	if( shape < 0 || shape >= ShapeCount() )
		shape = 0;
	const ShapeEntry& entry = kShapes[ shape ];
	MeshSequence sequence;
	sequence.firstFrame = entry.make( 0.0f );
	sequence.frames.push_back( sequence.firstFrame.positions );
	if( entry.animated )
	{
		for( int frame = 1; frame < kCycleFrames; ++frame )
			sequence.frames.push_back( entry.make( static_cast< float >( frame ) / kCycleFrames ).positions );
		sequence.fps = kCycleFps;
	}
	return sequence;
}
}// namespace swarm
