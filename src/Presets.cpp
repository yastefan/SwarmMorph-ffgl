// SPDX-License-Identifier: GPL-3.0-or-later
#include "Presets.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace swarm
{
namespace
{
fs::path DocumentsFolder()
{
#ifdef _WIN32
	// Documents may be redirected (OneDrive), so ask Windows rather than
	// assuming it under the user profile.
	PWSTR folder = nullptr;
	fs::path path;
	if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_Documents, 0, nullptr, &folder ) ) )
		path = folder;
	CoTaskMemFree( folder );
	return path;
#else
	const char* home = std::getenv( "HOME" );
	return home ? fs::path( home ) / "Documents" : fs::path();
#endif
}

// The file that remembers which presets were installed, one name per line.
const char* kInstalledList = ".swarmmorph-installed";

int InstallInto( const fs::path& folder )
{
	fs::create_directories( folder );
	std::set< std::string > installed;
	{
		std::ifstream list( folder / kInstalledList );
		for( std::string line; std::getline( list, line ); )
			if( !line.empty() )
				installed.insert( line );
	}
	int count = 0;
	bool changed = false;
	for( size_t i = 0; i < kEmbeddedPresetCount; ++i )
	{
		const EmbeddedPreset& preset = kEmbeddedPresets[ i ];
		if( installed.count( preset.fileName ) )
			continue;
		fs::path file = folder / fs::u8path( preset.fileName );
		if( !fs::exists( file ) )
		{
			std::ofstream out( file, std::ios::binary );
			out.write( reinterpret_cast< const char* >( preset.data ), static_cast< std::streamsize >( preset.size ) );
			if( !out )
				continue;
			++count;
		}
		installed.insert( preset.fileName );
		changed = true;
	}
	if( changed )
	{
		std::ofstream list( folder / kInstalledList, std::ios::trunc );
		for( const std::string& name : installed )
			list << name << "\n";
	}
	return count;
}
}// namespace

std::string InstallPresets()
{
	std::string message;
	try
	{
		fs::path documents = DocumentsFolder();
		if( documents.empty() )
			return message;
		for( const char* app : { "Resolume Arena", "Resolume Avenue" } )
		{
			fs::path root = documents / app;
			if( !fs::is_directory( root ) )
				continue;
			fs::path folder = root / "Presets" / "Sources" / "SwarmMorph";
			int count = InstallInto( folder );
			if( count > 0 )
				message += ( message.empty() ? "" : "; " ) + std::to_string( count ) + " presets to " + folder.u8string();
		}
	}
	catch( const std::exception& e )
	{
		// Presets are a bonus; never let them stop the plugin.
		return std::string( "could not install presets: " ) + e.what();
	}
	return message.empty() ? message : "installed " + message;
}
}// namespace swarm
