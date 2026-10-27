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

// Parses the vertices and faces of a Wavefront OBJ file. Materials, normals and
// texture coordinates are ignored. A file with vertices but no faces is valid
// and is treated as a point cloud. The path is UTF-8 on every platform.
bool LoadObj( const std::string& path, Mesh& mesh, std::string& error );

// Returns `count` points (xyz each) spread uniformly over the mesh surface,
// weighted by triangle area. Points are independent random samples, so any
// prefix of the result is itself a uniform sample of the surface; the plugin
// relies on this to change the particle count without resampling.
// Point clouds (no triangles) are sampled from their vertices.
// With `normalize`, the result is centred on the bounding box and scaled to
// fit inside the unit sphere.
std::vector< float > SampleSurface( const Mesh& mesh, size_t count, uint32_t seed, bool normalize );

// A unit sphere, used while no mesh is loaded.
std::vector< float > SampleSphere( size_t count, uint32_t seed );
}// namespace swarm
