// SPDX-License-Identifier: GPL-3.0-or-later
#include "AlembicLoader.h"

#if defined( SWARMMORPH_WITH_ALEMBIC )

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace swarm
{
namespace
{
namespace Abc     = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;

// Everything is preloaded so playback never touches the disk; these caps keep
// a huge cache from exhausting memory.
const size_t kMaxFrames      = 10000;
const size_t kMaxFrameFloats = size_t( 256 ) << 20;// 1 GiB of vertex positions

struct Shape
{
	Abc::IObject object;
	bool isPointCloud = false;
	Abc::IP3fArrayProperty positions;
	Abc::IInt32ArrayProperty faceCounts; // invalid for point clouds
	Abc::IInt32ArrayProperty faceIndices;// invalid for point clouds
};

void CollectShapes( const Abc::IObject& parent, std::vector< Shape >& shapes, std::vector< Abc::IObject >& xforms )
{
	for( size_t i = 0; i < parent.getNumChildren(); ++i )
	{
		const Abc::ObjectHeader& header = parent.getChildHeader( i );
		Abc::IObject child( parent, header.getName() );
		if( AbcGeom::IPolyMesh::matches( header ) )
		{
			AbcGeom::IPolyMesh mesh( child, Abc::kWrapExisting );
			AbcGeom::IPolyMeshSchema& schema = mesh.getSchema();
			shapes.push_back( { child, false, schema.getPositionsProperty(), schema.getFaceCountsProperty(), schema.getFaceIndicesProperty() } );
		}
		else if( AbcGeom::ISubD::matches( header ) )
		{
			AbcGeom::ISubD subd( child, Abc::kWrapExisting );
			AbcGeom::ISubDSchema& schema = subd.getSchema();
			shapes.push_back( { child, false, schema.getPositionsProperty(), schema.getFaceCountsProperty(), schema.getFaceIndicesProperty() } );
		}
		else if( AbcGeom::IPoints::matches( header ) )
		{
			AbcGeom::IPoints points( child, Abc::kWrapExisting );
			AbcGeom::IPointsSchema& schema = points.getSchema();
			shapes.push_back( { child, true, schema.getPositionsProperty(), {}, {} } );
		}
		else if( AbcGeom::IXform::matches( header ) )
		{
			xforms.push_back( child );
		}
		CollectShapes( child, shapes, xforms );
	}
}

// Alembic matrices use row vectors (v * M), so the world matrix is the
// object's parents multiplied from the inside out.
Abc::M44d WorldMatrix( const Abc::IObject& object, const Abc::ISampleSelector& selector )
{
	Abc::M44d world;// identity
	for( Abc::IObject parent = object.getParent(); parent.valid(); parent = parent.getParent() )
	{
		if( !AbcGeom::IXform::matches( parent.getHeader() ) )
			continue;
		AbcGeom::XformSample sample = AbcGeom::IXform( parent, Abc::kWrapExisting ).getSchema().getValue( selector );
		world = world * sample.getMatrix();
		if( !sample.getInheritsXforms() )
			break;
	}
	return world;
}

// The animated time span of an archive and the rate it was sampled at.
struct TimeRange
{
	double start  = 0.0;
	double end    = 0.0;
	double fps    = 0.0;
	bool animated = false;

	void Include( const AbcGeom::TimeSamplingPtr& sampling, size_t numSamples )
	{
		if( !sampling || numSamples < 2 )
			return;
		double first = sampling->getSampleTime( 0 );
		double last  = sampling->getSampleTime( numSamples - 1 );
		start        = animated ? std::min( start, first ) : first;
		end          = animated ? std::max( end, last ) : last;
		animated     = true;

		const AbcGeom::TimeSamplingType& type = sampling->getTimeSamplingType();
		double rate = type.isAcyclic() ? ( numSamples - 1 ) / std::max( last - first, 1e-6 )
									   : type.getNumSamplesPerCycle() / type.getTimePerCycle();
		fps         = std::max( fps, rate );
	}
};

// Appends the shape's fan-triangulated faces, with vertex indices offset by `base`.
bool AppendTriangles( const Shape& shape, const Abc::ISampleSelector& selector, uint32_t base, size_t vertexCount, std::vector< uint32_t >& triangles )
{
	Abc::Int32ArraySamplePtr counts  = shape.faceCounts.getValue( selector );
	Abc::Int32ArraySamplePtr indices = shape.faceIndices.getValue( selector );
	size_t cursor = 0;
	for( size_t face = 0; face < counts->size(); ++face )
	{
		int count = ( *counts )[ face ];
		if( count < 0 || cursor + count > indices->size() )
			return false;
		for( int corner = 2; corner < count; ++corner )
		{
			int32_t a = ( *indices )[ cursor ], b = ( *indices )[ cursor + corner - 1 ], c = ( *indices )[ cursor + corner ];
			if( a < 0 || b < 0 || c < 0 || size_t( a ) >= vertexCount || size_t( b ) >= vertexCount || size_t( c ) >= vertexCount )
				return false;
			triangles.push_back( base + a );
			triangles.push_back( base + b );
			triangles.push_back( base + c );
		}
		cursor += count;
	}
	return true;
}
}// namespace

bool AlembicSupported()
{
	return true;
}

bool LoadAlembic( const std::string& path, MeshSequence& sequence, std::string& error, std::string& warning )
{
	sequence = MeshSequence();
	try
	{
		// Opening through a stream handles non-ASCII paths on Windows, where
		// Alembic's own file name overload expects the ANSI code page.
		std::ifstream stream( std::filesystem::u8path( path ), std::ios::binary );
		if( !stream )
		{
			error = "Cannot open file: " + path;
			return false;
		}
		std::vector< std::istream* > streams{ &stream };
		Alembic::AbcCoreFactory::IFactory factory;
		factory.setPolicy( Abc::ErrorHandler::kThrowPolicy );
		Alembic::AbcCoreFactory::IFactory::CoreType coreType;
		Abc::IArchive archive = factory.getArchive( streams, coreType );
		if( !archive.valid() )
		{
			error = "Not an Alembic file, or an old HDF5 one (only Ogawa is supported): " + path;
			return false;
		}

		std::vector< Shape > shapes;
		std::vector< Abc::IObject > xforms;
		CollectShapes( archive.getTop(), shapes, xforms );
		// Meshes win; point objects only count when there is nothing else.
		bool hasMeshes = std::any_of( shapes.begin(), shapes.end(), []( const Shape& shape ) { return !shape.isPointCloud; } );
		shapes.erase( std::remove_if( shapes.begin(), shapes.end(), [ hasMeshes ]( const Shape& shape ) { return shape.isPointCloud == hasMeshes; } ), shapes.end() );
		if( shapes.empty() )
		{
			error = "No meshes or points found in: " + path;
			return false;
		}

		TimeRange range;
		for( const Shape& shape : shapes )
			range.Include( shape.positions.getTimeSampling(), shape.positions.getNumSamples() );
		for( const Abc::IObject& object : xforms )
		{
			AbcGeom::IXform xform( object, Abc::kWrapExisting );
			AbcGeom::IXformSchema& schema = xform.getSchema();
			range.Include( schema.getTimeSampling(), schema.getNumSamples() );
		}

		size_t frameCount = 1;
		if( range.animated )
		{
			sequence.fps = static_cast< float >( range.fps );
			frameCount   = static_cast< size_t >( std::floor( ( range.end - range.start ) * range.fps + 0.5 ) ) + 1;
		}
		if( frameCount > kMaxFrames )
		{
			warning    = "only the first " + std::to_string( kMaxFrames ) + " of " + std::to_string( frameCount ) + " frames are loaded";
			frameCount = kMaxFrames;
		}

		size_t skipped = 0;
		for( size_t frame = 0; frame < frameCount; ++frame )
		{
			Abc::ISampleSelector selector = range.animated
												? Abc::ISampleSelector( range.start + frame / range.fps, Abc::ISampleSelector::kNearIndex )
												: Abc::ISampleSelector( Abc::index_t( 0 ) );

			std::vector< float > positions;
			for( const Shape& shape : shapes )
			{
				Abc::P3fArraySamplePtr points = shape.positions.getValue( selector );
				Abc::M44d world               = WorldMatrix( shape.object, selector );
				uint32_t base                 = static_cast< uint32_t >( positions.size() / 3 );
				if( frame == 0 && !shape.isPointCloud &&
					!AppendTriangles( shape, selector, base, points->size(), sequence.firstFrame.triangles ) )
				{
					error = "Invalid faces in " + shape.object.getFullName() + " in: " + path;
					return false;
				}
				positions.reserve( positions.size() + points->size() * 3 );
				for( size_t i = 0; i < points->size(); ++i )
				{
					Imath::V3d local( ( *points )[ i ] );
					Imath::V3d global;
					world.multVecMatrix( local, global );
					positions.push_back( static_cast< float >( global.x ) );
					positions.push_back( static_cast< float >( global.y ) );
					positions.push_back( static_cast< float >( global.z ) );
				}
			}

			if( frame == 0 )
			{
				if( positions.empty() )
				{
					error = "The first frame has no vertices in: " + path;
					return false;
				}
				sequence.firstFrame.positions = positions;
			}
			else if( positions.size() != sequence.firstFrame.positions.size() )
			{
				++skipped;
				continue;
			}
			if( ( sequence.frames.size() + 1 ) * positions.size() > kMaxFrameFloats )
			{
				warning = "animation truncated to " + std::to_string( sequence.frames.size() ) + " frames to limit memory use";
				break;
			}
			sequence.frames.push_back( std::move( positions ) );
		}
		if( skipped > 0 )
		{
			if( !warning.empty() )
				warning += "; ";
			warning += std::to_string( skipped ) + " of " + std::to_string( frameCount ) +
					   " frames skipped (a different vertex count than the first frame)";
		}
		return true;
	}
	catch( const std::exception& e )
	{
		error = "Failed to read Alembic file " + path + ": " + e.what();
		return false;
	}
}
}// namespace swarm

#else

namespace swarm
{
bool AlembicSupported()
{
	return false;
}

bool LoadAlembic( const std::string& path, MeshSequence& sequence, std::string& error, std::string& )
{
	sequence = MeshSequence();
	error    = "This build has no Alembic support: " + path;
	return false;
}
}// namespace swarm

#endif
