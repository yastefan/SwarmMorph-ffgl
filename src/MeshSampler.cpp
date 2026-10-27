// SPDX-License-Identifier: GPL-3.0-or-later
#include "MeshSampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <random>

#if defined( _WIN32 )
#include <windows.h>
#endif

namespace swarm
{
namespace
{
FILE* OpenUtf8( const std::string& path )
{
#if defined( _WIN32 )
	int length = MultiByteToWideChar( CP_UTF8, 0, path.c_str(), -1, nullptr, 0 );
	if( length <= 0 )
		return nullptr;
	std::wstring widePath( length, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, path.c_str(), -1, &widePath[ 0 ], length );
	return _wfopen( widePath.c_str(), L"rb" );
#else
	return fopen( path.c_str(), "rb" );
#endif
}

bool ReadFile( const std::string& path, std::string& contents )
{
	FILE* file = OpenUtf8( path );
	if( !file )
		return false;
	fseek( file, 0, SEEK_END );
	long size = ftell( file );
	fseek( file, 0, SEEK_SET );
	if( size < 0 )
	{
		fclose( file );
		return false;
	}
	contents.resize( static_cast< size_t >( size ) );
	size_t read = size > 0 ? fread( &contents[ 0 ], 1, contents.size(), file ) : 0;
	fclose( file );
	contents.resize( read );
	return true;
}

inline const char* SkipSpaces( const char* p, const char* end )
{
	while( p < end && ( *p == ' ' || *p == '\t' ) )
		++p;
	return p;
}

inline const char* NextLine( const char* p, const char* end )
{
	while( p < end && *p != '\n' )
		++p;
	return p < end ? p + 1 : end;
}

// Resolves a 1-based (or negative, relative) OBJ index to a 0-based one.
inline bool ResolveIndex( long index, size_t vertexCount, uint32_t& out )
{
	if( index > 0 && static_cast< size_t >( index ) <= vertexCount )
	{
		out = static_cast< uint32_t >( index - 1 );
		return true;
	}
	if( index < 0 && static_cast< size_t >( -index ) <= vertexCount )
	{
		out = static_cast< uint32_t >( vertexCount + index );
		return true;
	}
	return false;
}

}// namespace

bool LoadObj( const std::string& path, Mesh& mesh, std::string& error )
{
	mesh = Mesh();
	std::string contents;
	if( !ReadFile( path, contents ) )
	{
		error = "Cannot open file: " + path;
		return false;
	}

	const char* p   = contents.data();
	const char* end = p + contents.size();
	std::vector< uint32_t > face;
	// strtof/strtol need a terminator; std::string guarantees one after end.
	while( p < end )
	{
		p = SkipSpaces( p, end );
		if( p + 1 < end && p[ 0 ] == 'v' && ( p[ 1 ] == ' ' || p[ 1 ] == '\t' ) )
		{
			char* cursor = const_cast< char* >( p + 2 );
			for( int axis = 0; axis < 3; ++axis )
				mesh.positions.push_back( strtof( cursor, &cursor ) );
			p = cursor;
		}
		else if( p + 1 < end && p[ 0 ] == 'f' && ( p[ 1 ] == ' ' || p[ 1 ] == '\t' ) )
		{
			face.clear();
			const char* cursor = p + 2;
			size_t vertexCount = mesh.positions.size() / 3;
			while( true )
			{
				cursor = SkipSpaces( cursor, end );
				if( cursor >= end || *cursor == '\n' || *cursor == '\r' || *cursor == '#' )
					break;
				char* after;
				long index = strtol( cursor, &after, 10 );
				if( after == cursor )
					break;
				uint32_t resolved;
				if( ResolveIndex( index, vertexCount, resolved ) )
					face.push_back( resolved );
				// Skip the /vt/vn part of "v/vt/vn".
				cursor = after;
				while( cursor < end && *cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r' )
					++cursor;
			}
			for( size_t i = 2; i < face.size(); ++i )
			{
				mesh.triangles.push_back( face[ 0 ] );
				mesh.triangles.push_back( face[ i - 1 ] );
				mesh.triangles.push_back( face[ i ] );
			}
			p = cursor;
		}
		p = NextLine( p, end );
	}

	if( mesh.positions.empty() )
	{
		error = "No vertices found in: " + path;
		return false;
	}
	return true;
}

std::vector< std::string > FindObjSequence( const std::string& path )
{
	namespace fs = std::filesystem;
	std::vector< std::string > single{ path };
	std::error_code ec;
	fs::path file = fs::u8path( path );
	std::string stem = file.stem().u8string();

	size_t digits = 0;
	while( digits < stem.size() && std::isdigit( static_cast< unsigned char >( stem[ stem.size() - 1 - digits ] ) ) )
		++digits;
	if( digits < 3 )
		return single;
	std::string prefix    = stem.substr( 0, stem.size() - digits );
	std::string extension = file.extension().u8string();

	std::vector< std::pair< long, std::string > > frames;
	for( const fs::directory_entry& entry : fs::directory_iterator( file.parent_path(), ec ) )
	{
		if( !entry.is_regular_file( ec ) || entry.path().extension().u8string() != extension )
			continue;
		std::string candidate = entry.path().stem().u8string();
		if( candidate.size() <= prefix.size() || candidate.compare( 0, prefix.size(), prefix ) != 0 )
			continue;
		std::string number = candidate.substr( prefix.size() );
		if( !std::all_of( number.begin(), number.end(), []( char c ) { return std::isdigit( static_cast< unsigned char >( c ) ); } ) )
			continue;
		frames.emplace_back( std::stol( number ), entry.path().u8string() );
	}
	if( ec || frames.size() < 2 )
		return single;

	std::sort( frames.begin(), frames.end() );
	std::vector< std::string > files;
	for( auto& frame : frames )
		files.push_back( std::move( frame.second ) );
	return files;
}

bool LoadObjSequence( const std::vector< std::string >& files, MeshSequence& sequence, std::string& error, std::string& warning )
{
	sequence = MeshSequence();
	if( files.empty() || !LoadObj( files[ 0 ], sequence.firstFrame, error ) )
		return false;
	sequence.frames.push_back( sequence.firstFrame.positions );

	size_t skipped = 0;
	for( size_t i = 1; i < files.size(); ++i )
	{
		Mesh frame;
		std::string frameError;
		if( LoadObj( files[ i ], frame, frameError ) && frame.positions.size() == sequence.firstFrame.positions.size() )
			sequence.frames.push_back( std::move( frame.positions ) );
		else
			++skipped;
	}
	if( skipped > 0 )
		warning = std::to_string( skipped ) + " of " + std::to_string( files.size() ) +
				  " frames skipped (unreadable or a different vertex count than the first frame)";
	return true;
}

SurfaceBinding BindSurface( const Mesh& mesh, size_t count, uint32_t seed )
{
	SurfaceBinding binding;
	binding.indices.assign( count * 4, 0 );
	binding.weights.assign( count * 4, 0.0f );
	if( mesh.positions.empty() || count == 0 )
		return binding;

	std::mt19937 rng( seed );
	std::uniform_real_distribution< float > unit( 0.0f, 1.0f );
	const float* v = mesh.positions.data();

	// Cumulative triangle areas for area-weighted picking.
	std::vector< double > cumulative;
	cumulative.reserve( mesh.triangles.size() / 3 );
	double total = 0.0;
	for( size_t t = 0; t + 2 < mesh.triangles.size(); t += 3 )
	{
		const float* a = v + 3 * mesh.triangles[ t ];
		const float* b = v + 3 * mesh.triangles[ t + 1 ];
		const float* c = v + 3 * mesh.triangles[ t + 2 ];
		double ab[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] };
		double ac[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
		double cx      = ab[ 1 ] * ac[ 2 ] - ab[ 2 ] * ac[ 1 ];
		double cy      = ab[ 2 ] * ac[ 0 ] - ab[ 0 ] * ac[ 2 ];
		double cz      = ab[ 0 ] * ac[ 1 ] - ab[ 1 ] * ac[ 0 ];
		total += 0.5 * std::sqrt( cx * cx + cy * cy + cz * cz );
		cumulative.push_back( total );
	}

	if( total > 0.0 )
	{
		std::uniform_real_distribution< double > pick( 0.0, total );
		for( size_t i = 0; i < count; ++i )
		{
			size_t t = std::lower_bound( cumulative.begin(), cumulative.end(), pick( rng ) ) - cumulative.begin();
			t        = std::min( t, cumulative.size() - 1 );
			// Uniform barycentric coordinates.
			float r1 = std::sqrt( unit( rng ) );
			float r2 = unit( rng );
			for( int corner = 0; corner < 3; ++corner )
				binding.indices[ 4 * i + corner ] = mesh.triangles[ 3 * t + corner ];
			binding.weights[ 4 * i ]     = 1.0f - r1;
			binding.weights[ 4 * i + 1 ] = r1 * ( 1.0f - r2 );
			binding.weights[ 4 * i + 2 ] = r1 * r2;
		}
	}
	else
	{
		// Point cloud, or only degenerate faces: bind to the vertices themselves.
		size_t vertexCount = mesh.positions.size() / 3;
		std::uniform_int_distribution< size_t > pick( 0, vertexCount - 1 );
		for( size_t i = 0; i < count; ++i )
		{
			binding.indices[ 4 * i ] = static_cast< uint32_t >( pick( rng ) );
			binding.weights[ 4 * i ] = 1.0f;
		}
	}
	return binding;
}

std::vector< float > EvaluateBinding( const SurfaceBinding& binding, const std::vector< float >& positions )
{
	size_t count = binding.indices.size() / 4;
	std::vector< float > points( count * 3, 0.0f );
	for( size_t i = 0; i < count; ++i )
	{
		for( int corner = 0; corner < 3; ++corner )
		{
			float weight = binding.weights[ 4 * i + corner ];
			if( weight == 0.0f )
				continue;
			const float* vertex = &positions[ 3 * binding.indices[ 4 * i + corner ] ];
			for( int axis = 0; axis < 3; ++axis )
				points[ 3 * i + axis ] += weight * vertex[ axis ];
		}
	}
	return points;
}

void Normalization::Apply( std::vector< float >& positions ) const
{
	for( size_t i = 0; i + 2 < positions.size(); i += 3 )
		for( int axis = 0; axis < 3; ++axis )
			positions[ i + axis ] = ( positions[ i + axis ] - center[ axis ] ) * scale;
}

Normalization ComputeNormalization( const std::vector< const std::vector< float >* >& frames )
{
	float lo[ 3 ] = { INFINITY, INFINITY, INFINITY };
	float hi[ 3 ] = { -INFINITY, -INFINITY, -INFINITY };
	for( const std::vector< float >* positions : frames )
		for( size_t i = 0; i + 2 < positions->size(); i += 3 )
			for( int axis = 0; axis < 3; ++axis )
			{
				lo[ axis ] = std::min( lo[ axis ], ( *positions )[ i + axis ] );
				hi[ axis ] = std::max( hi[ axis ], ( *positions )[ i + axis ] );
			}

	Normalization normalization;
	if( !( lo[ 0 ] <= hi[ 0 ] ) )
		return normalization;
	for( int axis = 0; axis < 3; ++axis )
		normalization.center[ axis ] = 0.5f * ( lo[ axis ] + hi[ axis ] );
	float radiusSq = 0.0f;
	for( const std::vector< float >* positions : frames )
		for( size_t i = 0; i + 2 < positions->size(); i += 3 )
		{
			float dx = ( *positions )[ i ] - normalization.center[ 0 ];
			float dy = ( *positions )[ i + 1 ] - normalization.center[ 1 ];
			float dz = ( *positions )[ i + 2 ] - normalization.center[ 2 ];
			radiusSq = std::max( radiusSq, dx * dx + dy * dy + dz * dz );
		}
	normalization.scale = radiusSq > 0.0f ? 1.0f / std::sqrt( radiusSq ) : 1.0f;
	return normalization;
}

std::vector< float > SampleSphere( size_t count, uint32_t seed )
{
	std::vector< float > points( count * 3 );
	std::mt19937 rng( seed );
	std::normal_distribution< float > gauss( 0.0f, 1.0f );
	for( size_t i = 0; i < count; ++i )
	{
		float x = gauss( rng ), y = gauss( rng ), z = gauss( rng );
		float length = std::sqrt( x * x + y * y + z * z );
		if( length < 1e-6f )
			x = length = 1.0f;
		points[ 3 * i ]     = x / length;
		points[ 3 * i + 1 ] = y / length;
		points[ 3 * i + 2 ] = z / length;
	}
	return points;
}
}// namespace swarm
