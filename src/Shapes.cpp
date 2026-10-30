// SPDX-License-Identifier: GPL-3.0-or-later
#include "Shapes.h"
#include <cmath>
#include <functional>

namespace swarm
{
namespace
{
const float kPi = 3.14159265358979f;

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

// A tube of the given radius around a closed or open curve.
void AddTube( Mesh& mesh, int segments, int sides, float radius, bool closed, const std::function< Point( float ) >& curve )
{
	AddGrid( mesh, sides, segments, true, closed, [ & ]( float u, float v ) {
		const float e = 1e-3f;
		Point centre  = curve( v );
		Point tangent = Normalized( curve( v + e ) - curve( v - e ) );
		Point up      = std::fabs( tangent.y ) < 0.9f ? Point{ 0.0f, 1.0f, 0.0f } : Point{ 1.0f, 0.0f, 0.0f };
		Point side    = Normalized( Cross( tangent, up ) );
		Point normal  = Cross( side, tangent );
		float angle   = u * 2.0f * kPi;
		return centre + ( side * std::cos( angle ) + normal * std::sin( angle ) ) * radius;
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

Mesh Sphere()
{
	Mesh mesh;
	AddGrid( mesh, 64, 32, true, false, []( float u, float v ) {
		float theta = u * 2.0f * kPi, phi = v * kPi;
		return Point{ std::sin( phi ) * std::cos( theta ), std::cos( phi ), -std::sin( phi ) * std::sin( theta ) };
	}, true );
	return mesh;
}

Mesh Cube()
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

Mesh Torus()
{
	Mesh mesh;
	AddTube( mesh, 96, 32, 0.3f, true, []( float t ) {
		float angle = t * 2.0f * kPi;
		return Point{ 0.7f * std::cos( angle ), 0.0f, 0.7f * std::sin( angle ) };
	} );
	return mesh;
}

Mesh Knot()
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

Mesh Cylinder()
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

Mesh Cone()
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

Mesh Helix()
{
	Mesh mesh;
	AddTube( mesh, 320, 16, 0.12f, false, []( float t ) {
		float angle = t * 3.0f * 2.0f * kPi;
		return Point{ 0.55f * std::cos( angle ), 1.6f * t - 0.8f, 0.55f * std::sin( angle ) };
	} );
	return mesh;
}

Mesh Wave()
{
	Mesh mesh;
	AddGrid( mesh, 96, 96, false, false, []( float u, float v ) {
		float x = 2.0f * u - 1.0f, z = 2.0f * v - 1.0f;
		float r = std::sqrt( x * x + z * z );
		return Point{ 0.9f * x, 0.18f * std::cos( 7.0f * r ), 0.9f * z };
	}, true );
	return mesh;
}

Mesh Plane()
{
	Mesh mesh;
	// Facing the camera, like a screen.
	AddGrid( mesh, 32, 18, false, false, []( float u, float v ) {
		return Point{ 0.96f * ( 2.0f * u - 1.0f ), 0.54f * ( 2.0f * v - 1.0f ), 0.0f };
	} );
	return mesh;
}

struct ShapeEntry
{
	const char* name;
	Mesh ( *make )();
};

const ShapeEntry kShapes[] = { { "Sphere", Sphere }, { "Cube", Cube },     { "Torus", Torus }, { "Knot", Knot },   { "Cylinder", Cylinder },
							   { "Cone", Cone },     { "Helix", Helix },   { "Wave", Wave },   { "Plane", Plane } };
}// namespace

int ShapeCount()
{
	return static_cast< int >( sizeof( kShapes ) / sizeof( kShapes[ 0 ] ) );
}

const char* ShapeName( int shape )
{
	return shape >= 0 && shape < ShapeCount() ? kShapes[ shape ].name : "";
}

Mesh MakeShape( int shape )
{
	if( shape < 0 || shape >= ShapeCount() )
		shape = 0;
	return kShapes[ shape ].make();
}
}// namespace swarm
