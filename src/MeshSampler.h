// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace swarm
{
struct Mesh
{
	std::vector< float > positions;   //!< xyz per vertex
	std::vector< uint32_t > triangles;//!< 3 vertex indices per triangle, fan-triangulated from the OBJ faces
};

// An animation exported as one OBJ per frame. All frames share the topology of
// the first one; frames whose vertex count differs are dropped on load.
struct MeshSequence
{
	Mesh firstFrame;
	std::vector< std::vector< float > > frames;//!< xyz per vertex, per frame
};

// Ties each particle to a fixed spot on the surface: three vertices and their
// barycentric weights. Evaluating it against any frame of a sequence yields
// that particle's target, so particles stick to the same spot as the surface
// deforms.
struct SurfaceBinding
{
	std::vector< uint32_t > indices;// 4 per particle, the 4th unused (matches an RGBA texture)
	std::vector< float > weights;   // 4 per particle, the 4th unused
};

// Parses the vertices and faces of a Wavefront OBJ file. Materials, normals and
// texture coordinates are ignored. A file with vertices but no faces is valid
// and is treated as a point cloud. The path is UTF-8 on every platform.
bool LoadObj( const std::string& path, Mesh& mesh, std::string& error );

// Finds the numbered sequence `path` belongs to, e.g. walk_0001.obj ..
// walk_0120.obj next to each other, ordered by frame number. Needs at least
// three digits before the extension and at least two files; otherwise
// `files` holds just `path`.
std::vector< std::string > FindObjSequence( const std::string& path );

bool LoadObjSequence( const std::vector< std::string >& files, MeshSequence& sequence, std::string& error, std::string& warning );

// Picks `count` random spots spread uniformly over the surface, weighted by
// triangle area. Spots are independent samples, so any prefix of the result
// is itself uniform; the plugin relies on this to change the particle count
// without resampling. Point clouds (no triangles) bind to single vertices.
SurfaceBinding BindSurface( const Mesh& mesh, size_t count, uint32_t seed );

// Positions (xyz per particle) of a binding on one set of vertex positions.
std::vector< float > EvaluateBinding( const SurfaceBinding& binding, const std::vector< float >& positions );

// Translation and scale that centre the given frames on their combined
// bounding box and fit them inside the unit sphere.
struct Normalization
{
	float center[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float scale       = 1.0f;
	void Apply( std::vector< float >& positions ) const;
};
Normalization ComputeNormalization( const std::vector< const std::vector< float >* >& frames );

// A unit sphere, used while no mesh is loaded.
std::vector< float > SampleSphere( size_t count, uint32_t seed );
}// namespace swarm
