// SPDX-License-Identifier: GPL-3.0-or-later
#include "SwarmMorph.h"
#include <ffglex/FFGLScopedFBOBinding.h>
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include "AlembicLoader.h"
#include "CameraPaths.h"
#include "MathUtil.h"
#include "MeshSampler.h"
#include "Shaders.h"

using namespace ffglex;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< SwarmMorph >,                              // Create method
	"SwMp",                                                    // Plugin unique ID
	"SwarmMorph",                                              // Plugin name
	2,                                                         // API major version number
	1,                                                         // API minor version number
	SWARMMORPH_VERSION_MAJOR,                                  // Plugin major version number
	SWARMMORPH_VERSION_MINOR,                                  // Plugin minor version number
	FF_SOURCE,                                                 // Plugin type
	"Particle swarm that morphs between OBJ meshes",           // Plugin description
	"SwarmMorph - github.com/yastefan/SwarmMorph-ffgl"         // About
);

namespace
{
// Declared in menu order: Resolume shows parameters by ID and opens a new
// section whenever the group changes.
enum ParamID
{
	PID_MESH_FIRST = 0,
	PID_MESH_LAST  = PID_MESH_FIRST + SwarmMorph::kNumSlots - 1,
	PID_NORMALIZE,
	PID_SEQUENCES,

	PID_TARGET,
	PID_NEXT,
	PID_MORPH,
	PID_MORPH_TO,

	PID_SPEED,
	PID_ATTRACTION,
	PID_DAMPING,
	PID_CHAOS,
	PID_TURBULENCE,

	PID_SURFACE_FLOW,
	PID_HOVER,
	PID_NOISE_SCALE,
	PID_FLOW_SPEED,
	PID_BUZZ,
	PID_BUZZ_SPEED,

	PID_WIND_X,
	PID_WIND_Y,
	PID_WIND_Z,
	PID_GUSTS,
	PID_BLOW_AWAY,
	PID_WRAP,
	PID_VORTEX,
	PID_RADIAL,

	PID_BEAT_RATE,
	PID_HIT,
	PID_DECAY,
	PID_PULSE,
	PID_SHOCKWAVE,
	PID_BURST,
	PID_JITTER,
	PID_FLASH,
	PID_SPIN_STEP,
	PID_AUTO_NEXT,

	PID_ANIM_SPEED,
	PID_PING_PONG,
	PID_RESTART,

	PID_PARTICLES,
	PID_POINT_SIZE,
	PID_SOFTNESS,
	PID_STREAKS,
	PID_BRIGHTNESS,
	PID_COLOR_H,// hue, saturation, brightness and alpha in a row make one colour picker
	PID_COLOR_S,
	PID_COLOR_V,
	PID_COLOR_A,
	PID_COLOR2_H,
	PID_COLOR2_S,
	PID_COLOR2_V,
	PID_COLOR2_A,
	PID_COLOR_MODE,
	PID_COLOR_MIX,
	PID_HEAT,
	PID_TWINKLE,
	PID_DEPTH_FADE,

	PID_SPIN,
	PID_ROTATE_X,
	PID_ROTATE_Y,
	PID_ZOOM,
	PID_PATH,
	PID_PATH_POSITION,
	PID_PATH_SPEED,
};

const float kPi              = 3.14159265358979f;
const float kFovY            = 40.0f * kPi / 180.0f;
const int kTextureWidth      = SwarmMorph::kTextureWidth;

GLuint CreateTexture( GLint internalFormat, GLenum format, GLenum type, int width, int height, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	Scoped2DTextureBinding binding( texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, pixels );
	return texture;
}

// Binds 2D textures to units 0, 1, 2, ... and unbinds every one of them when
// it goes out of scope. The SDK's ScopedSamplerActivation resets the active
// unit to 0 on destruction, so stacking several of them unbinds the wrong units.
class ScopedTextureUnits
{
public:
	ScopedTextureUnits( std::initializer_list< GLuint > textures ) :
		count( static_cast< GLuint >( textures.size() ) )
	{
		GLuint unit = 0;
		for( GLuint texture : textures )
		{
			glActiveTexture( GL_TEXTURE0 + unit++ );
			glBindTexture( GL_TEXTURE_2D, texture );
		}
		glActiveTexture( GL_TEXTURE0 );
	}
	~ScopedTextureUnits()
	{
		for( GLuint unit = 0; unit < count; ++unit )
		{
			glActiveTexture( GL_TEXTURE0 + unit );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		glActiveTexture( GL_TEXTURE0 );
	}
	ScopedTextureUnits( const ScopedTextureUnits& ) = delete;
	ScopedTextureUnits& operator=( const ScopedTextureUnits& ) = delete;

private:
	GLuint count;
};

// Particle-sized float texture: one texel per particle.
GLuint CreateFloatTexture( GLint internalFormat, GLenum format, const float* pixels )
{
	return CreateTexture( internalFormat, format, GL_FLOAT, kTextureWidth, kTextureWidth, pixels );
}

// Mappings from the host's normalised 0..1 values to physical quantities.
float SpeedValue( float v ) { return 2.0f * v; }// time scale of the swarm, 1 = normal
float AttractionValue( float v ) { return 1.0f + 40.0f * v * v; }
float DampingValue( float v ) { return 0.15f + 1.35f * v; }// fraction of critical damping
float TurbulenceValue( float v ) { return v; }
float NoiseScaleValue( float v ) { return 0.5f + 6.0f * v; }
float PointSizeValue( float v ) { return 0.5f + 12.0f * v; }
float BrightnessValue( float v ) { return 2.0f * v; }
float SpinValue( float v ) { return ( v - 0.5f ); }// revolutions per second
float AngleValue( float v ) { return ( v - 0.5f ) * 360.0f; }
float DistanceValue( float v ) { return 6.0f - 4.5f * v; }

// Centred sliders: 0.5 is no force. The small dead zone makes it easy to hit
// zero exactly with a MIDI fader.
float CentredForce( float v, float maxForce )
{
	float centred = ( v - 0.5f ) * 2.0f;
	const float deadZone = 0.02f;
	if( std::fabs( centred ) < deadZone )
		return 0.0f;
	float sign = centred < 0.0f ? -1.0f : 1.0f;
	return sign * ( std::fabs( centred ) - deadZone ) / ( 1.0f - deadZone ) * maxForce;
}
float WindValue( float v ) { return CentredForce( v, 10.0f ); }
float VortexValue( float v ) { return CentredForce( v, 6.0f ); }
float RadialValue( float v ) { return CentredForce( v, 6.0f ); }
// Beat Rate and Auto Next options, as intervals in bars (4/4).
const double kBeatIntervals[]  = { 0.0, 0.125, 0.25, 0.5, 1.0, 2.0 };
const double kAutoNextBars[]   = { 0.0, 1.0, 2.0, 4.0, 8.0 };
float DecaySeconds( float v ) { return 0.06f + 0.9f * v * v; }
float PulseValue( float v ) { return 0.15f * v; }
float ShockwaveValue( float v ) { return 0.4f * v; }
float BurstValue( float v ) { return v; }// share of particles
float JitterValue( float v ) { return 0.15f * v; }
float FlashValue( float v ) { return 3.0f * v; }
float SpinStepValue( float v ) { return 90.0f * v; }// degrees per beat
// Hue in [0, 1] (1 wraps to red), saturation and value in [0, 1].
void HsvToRgb( float h, float s, float v, float rgb[ 3 ] )
{
	h = ( h - std::floor( h ) ) * 6.0f;
	for( int i = 0; i < 3; ++i )
	{
		// Standard HSV: k runs round the hue wheel, offset per channel.
		float k       = std::fmod( h + static_cast< float >( 5 - 2 * i ), 6.0f );
		float channel = std::min( std::max( std::min( k, 4.0f - k ), 0.0f ), 1.0f );
		rgb[ i ]      = v * ( 1.0f - s * channel );
	}
}
float StreakValue( float v ) { return 2.0f * v; }// 1 = back to where the particle was a frame ago
float PathSpeedValue( float v ) { return 0.2f * v * v; }// loops per second; 1 = one loop in 5 s
float AnimSpeedValue( float v ) { return 3.0f * v; }
float SurfaceFlowValue( float v ) { return 0.4f * v; }
float HoverValue( float v ) { return 0.25f * v; }
float FlowSpeedValue( float v ) { return 2.0f * v; }
float BuzzValue( float v ) { return 0.15f * v; }
float BuzzSpeedValue( float v ) { return 4.0f * v; }

bool IsAlembicPath( const std::string& path )
{
	if( path.size() < 4 )
		return false;
	std::string extension = path.substr( path.size() - 4 );
	std::transform( extension.begin(), extension.end(), extension.begin(), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
	return extension == ".abc";
}

bool IsMeshParam( unsigned int index )
{
	return index >= PID_MESH_FIRST && index <= PID_MESH_LAST;
}
}// namespace

SwarmMorph::SwarmMorph()
{
	// The SDK leaves hostTime uninitialised until the host's first SetTime call.
	hostTime = 0.0;
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	for( int i = 0; i < kNumSlots; ++i )
	{
		std::string name = "Mesh " + std::to_string( i + 1 );
		std::vector< std::string > extensions{ "obj" };
		if( swarm::AlembicSupported() )
			extensions.push_back( "abc" );
		SetFileParamInfo( PID_MESH_FIRST + i, name.c_str(), extensions, "" );
		SetParamGroup( PID_MESH_FIRST + i, "Meshes" );
	}
	SetParamInfo( PID_NORMALIZE, "Normalize", FF_TYPE_BOOLEAN, normalizeMeshes );
	SetParamInfo( PID_SEQUENCES, "OBJ Sequences", FF_TYPE_BOOLEAN, detectSequences );
	SetParamGroup( PID_NORMALIZE, "Meshes" );
	SetParamGroup( PID_SEQUENCES, "Meshes" );

	SetParamInfo( PID_TARGET, "Target", FF_TYPE_INTEGER, 1.0f );
	SetParamRange( PID_TARGET, 1.0f, static_cast< float >( kNumSlots ) );
	SetParamInfo( PID_NEXT, "Next", FF_TYPE_EVENT, false );
	// A fader-driven morph: Morph slides the swarm's goal from Target to Morph To.
	SetParamInfof( PID_MORPH, "Morph", FF_TYPE_STANDARD );
	SetParamInfo( PID_MORPH_TO, "Morph To", FF_TYPE_INTEGER, 2.0f );
	SetParamRange( PID_MORPH_TO, 1.0f, static_cast< float >( kNumSlots ) );

	// How the swarm travels from one mesh to the next.
	SetParamInfof( PID_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PID_ATTRACTION, "Attraction", FF_TYPE_STANDARD );
	SetParamInfof( PID_DAMPING, "Damping", FF_TYPE_STANDARD );
	SetParamInfof( PID_CHAOS, "Chaos", FF_TYPE_STANDARD );
	SetParamInfof( PID_TURBULENCE, "Turbulence", FF_TYPE_STANDARD );
	for( unsigned int id = PID_SPEED; id <= PID_TURBULENCE; ++id )
		SetParamGroup( id, "Morph" );

	// How it moves once it has arrived.
	SetParamInfof( PID_SURFACE_FLOW, "Surface Flow", FF_TYPE_STANDARD );
	SetParamInfof( PID_HOVER, "Hover", FF_TYPE_STANDARD );
	SetParamInfof( PID_NOISE_SCALE, "Flow Scale", FF_TYPE_STANDARD );
	SetParamInfof( PID_FLOW_SPEED, "Flow Speed", FF_TYPE_STANDARD );
	SetParamInfof( PID_BUZZ, "Buzz", FF_TYPE_STANDARD );
	SetParamInfof( PID_BUZZ_SPEED, "Buzz Speed", FF_TYPE_STANDARD );
	for( unsigned int id = PID_SURFACE_FLOW; id <= PID_BUZZ_SPEED; ++id )
		SetParamGroup( id, "Swarm" );

	SetParamInfof( PID_WIND_X, "Wind X", FF_TYPE_STANDARD );
	SetParamInfof( PID_WIND_Y, "Wind Y", FF_TYPE_STANDARD );
	SetParamInfof( PID_WIND_Z, "Wind Z", FF_TYPE_STANDARD );
	SetParamInfof( PID_GUSTS, "Gusts", FF_TYPE_STANDARD );
	SetParamInfof( PID_BLOW_AWAY, "Blow Away", FF_TYPE_STANDARD );
	SetParamInfo( PID_WRAP, "Wrap Edges", FF_TYPE_BOOLEAN, wrapEdges );
	SetParamInfof( PID_VORTEX, "Vortex", FF_TYPE_STANDARD );
	SetParamInfof( PID_RADIAL, "Explode", FF_TYPE_STANDARD );
	for( unsigned int id = PID_WIND_X; id <= PID_RADIAL; ++id )
		SetParamGroup( id, "Forces" );

	// Effects that fire on Resolume's beat grid, or by hand with Hit.
	SetOptionParamInfo( PID_BEAT_RATE, "Beat Rate", 6, static_cast< float >( beatRate ) );
	const char* rates[] = { "Off", "1/2 Beat", "1 Beat", "2 Beats", "1 Bar", "2 Bars" };
	for( int i = 0; i < 6; ++i )
		SetParamElementInfo( PID_BEAT_RATE, i, rates[ i ], static_cast< float >( i ) );
	SetParamInfo( PID_HIT, "Hit", FF_TYPE_EVENT, false );
	SetParamInfof( PID_DECAY, "Decay", FF_TYPE_STANDARD );
	SetParamInfof( PID_PULSE, "Pulse", FF_TYPE_STANDARD );
	SetParamInfof( PID_SHOCKWAVE, "Shockwave", FF_TYPE_STANDARD );
	SetParamInfof( PID_BURST, "Burst", FF_TYPE_STANDARD );
	SetParamInfof( PID_JITTER, "Jitter", FF_TYPE_STANDARD );
	SetParamInfof( PID_FLASH, "Flash", FF_TYPE_STANDARD );
	SetParamInfof( PID_SPIN_STEP, "Spin Step", FF_TYPE_STANDARD );
	SetOptionParamInfo( PID_AUTO_NEXT, "Auto Next", 5, static_cast< float >( autoNext ) );
	const char* bars[] = { "Off", "1 Bar", "2 Bars", "4 Bars", "8 Bars" };
	for( int i = 0; i < 5; ++i )
		SetParamElementInfo( PID_AUTO_NEXT, i, bars[ i ], static_cast< float >( i ) );
	for( unsigned int id = PID_BEAT_RATE; id <= PID_AUTO_NEXT; ++id )
		SetParamGroup( id, "Beat" );

	SetParamInfof( PID_ANIM_SPEED, "Anim Speed", FF_TYPE_STANDARD );
	SetParamInfo( PID_PING_PONG, "Ping Pong", FF_TYPE_BOOLEAN, pingPong );
	SetParamInfo( PID_RESTART, "Restart On Switch", FF_TYPE_BOOLEAN, restartOnSwitch );
	for( unsigned int id = PID_ANIM_SPEED; id <= PID_RESTART; ++id )
		SetParamGroup( id, "Animation" );

	SetParamInfof( PID_PARTICLES, "Particles", FF_TYPE_STANDARD );
	SetParamInfof( PID_POINT_SIZE, "Point Size", FF_TYPE_STANDARD );
	SetParamInfof( PID_SOFTNESS, "Softness", FF_TYPE_STANDARD );
	SetParamInfof( PID_STREAKS, "Streaks", FF_TYPE_STANDARD );
	SetParamInfof( PID_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	// Resolume shows hue, saturation, brightness and alpha in a row as one
	// colour picker (named as in the FFGL SDK's example); alpha sets how
	// strongly that colour shines.
	SetParamInfof( PID_COLOR_H, "Color", FF_TYPE_HUE );
	SetParamInfof( PID_COLOR_S, "Color_Sat", FF_TYPE_SATURATION );
	SetParamInfof( PID_COLOR_V, "Color_Bri", FF_TYPE_BRIGHTNESS );
	SetParamInfof( PID_COLOR_A, "Color_Alpha", FF_TYPE_ALPHA );
	SetParamInfof( PID_COLOR2_H, "Color 2", FF_TYPE_HUE );
	SetParamInfof( PID_COLOR2_S, "Color 2_Sat", FF_TYPE_SATURATION );
	SetParamInfof( PID_COLOR2_V, "Color 2_Bri", FF_TYPE_BRIGHTNESS );
	SetParamInfof( PID_COLOR2_A, "Color 2_Alpha", FF_TYPE_ALPHA );
	SetOptionParamInfo( PID_COLOR_MODE, "Color Mode", 4, static_cast< float >( colorMode ) );
	SetParamElementInfo( PID_COLOR_MODE, 0, "Speed", 0.0f );
	SetParamElementInfo( PID_COLOR_MODE, 1, "Height", 1.0f );
	SetParamElementInfo( PID_COLOR_MODE, 2, "Depth", 2.0f );
	SetParamElementInfo( PID_COLOR_MODE, 3, "Random", 3.0f );
	SetParamInfof( PID_COLOR_MIX, "Color Mix", FF_TYPE_STANDARD );
	SetParamInfof( PID_HEAT, "Speed Glow", FF_TYPE_STANDARD );
	SetParamInfof( PID_TWINKLE, "Twinkle", FF_TYPE_STANDARD );
	SetParamInfof( PID_DEPTH_FADE, "Depth Fade", FF_TYPE_STANDARD );
	for( unsigned int id = PID_PARTICLES; id <= PID_DEPTH_FADE; ++id )
		SetParamGroup( id, "Look" );

	SetParamInfof( PID_SPIN, "Spin", FF_TYPE_STANDARD );
	SetParamInfof( PID_ROTATE_X, "Rotate X", FF_TYPE_STANDARD );
	SetParamInfof( PID_ROTATE_Y, "Rotate Y", FF_TYPE_STANDARD );
	SetParamInfof( PID_ZOOM, "Zoom", FF_TYPE_STANDARD );
	// Camera moves: pick a path, then drive Path Position from a fader or the
	// timeline, or let Path Speed run it.
	SetOptionParamInfo( PID_PATH, "Path", swarm::CameraPathCount() + 1, 0.0f );
	for( int i = 0; i <= swarm::CameraPathCount(); ++i )
		SetParamElementInfo( PID_PATH, i, swarm::CameraPathName( i ), static_cast< float >( i ) );
	SetParamInfof( PID_PATH_POSITION, "Path Position", FF_TYPE_STANDARD );
	SetParamInfof( PID_PATH_SPEED, "Path Speed", FF_TYPE_STANDARD );
	for( unsigned int id = PID_SPIN; id <= PID_PATH_SPEED; ++id )
		SetParamGroup( id, "Camera" );
}

SwarmMorph::~SwarmMorph() = default;

FFResult SwarmMorph::InitGL( const FFGLViewportStruct* vp )
{
	if( !CreateGLResources() )
	{
		FFGLLog::LogToHost( "SwarmMorph: failed to create GL resources" );
		DeInitGL();
		return FF_FAIL;
	}
	return CFFGLPlugin::InitGL( vp );
}

FFResult SwarmMorph::DeInitGL()
{
	ReleaseGLResources();
	return FF_SUCCESS;
}

bool SwarmMorph::CreateGLResources()
{
	// The host can hand us a context with its own error still pending (Resolume
	// does on the first instantiation). Drain it so we only see our own errors.
	while( glGetError() != GL_NO_ERROR )
	{
	}

	if( !updateShader.Compile( swarm::kFullscreenVS, swarm::kUpdateFS ) )
	{
		FFGLLog::LogToHost( "SwarmMorph: update shader failed to compile" );
		return false;
	}
	if( !renderShader.Compile( swarm::RenderShaderSource( swarm::kRenderVSBody, false ), swarm::RenderShaderSource( swarm::kRenderFSBody, false ) ) ||
		!streakShader.Compile( swarm::RenderShaderSource( swarm::kRenderVSBody, true ), swarm::RenderShaderSource( swarm::kRenderFSBody, true ) ) )
	{
		FFGLLog::LogToHost( "SwarmMorph: render shader failed to compile" );
		return false;
	}
	if( !skinShader.Compile( swarm::kFullscreenVS, swarm::kSkinFS ) )
	{
		FFGLLog::LogToHost( "SwarmMorph: skin shader failed to compile" );
		return false;
	}

	std::vector< float > sphere = swarm::SampleSphere( kMaxParticles, 7 );
	fallbackTexture             = CreateFloatTexture( GL_RGB32F, GL_RGB, sphere.data() );

	// Start as a loose cloud so the first target already looks like a morph.
	std::vector< float > positions( static_cast< size_t >( kMaxParticles ) * 4 );
	std::vector< float > velocities( positions.size(), 0.0f );
	std::mt19937 rng( 42 );
	std::uniform_real_distribution< float > unit( 0.0f, 1.0f );
	for( size_t i = 0; i < static_cast< size_t >( kMaxParticles ); ++i )
	{
		float* p     = &positions[ i * 4 ];
		float radius = 2.5f * std::cbrt( unit( rng ) );
		float z      = unit( rng ) * 2.0f - 1.0f;
		float angle  = unit( rng ) * 2.0f * kPi;
		float ring   = std::sqrt( 1.0f - z * z );
		p[ 0 ]       = radius * ring * std::cos( angle );
		p[ 1 ]       = radius * ring * std::sin( angle );
		p[ 2 ]       = radius * z;
		p[ 3 ]       = unit( rng );// seed
	}

	for( StateBuffer& buffer : state )
	{
		buffer.positions  = CreateFloatTexture( GL_RGBA32F, GL_RGBA, positions.data() );
		buffer.velocities = CreateFloatTexture( GL_RGBA32F, GL_RGBA, velocities.data() );
		glGenFramebuffers( 1, &buffer.fbo );
		ScopedFBOBinding fboBinding( buffer.fbo, ScopedFBOBinding::RB_REVERT );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.positions, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, buffer.velocities, 0 );
		const GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers( 2, drawBuffers );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			FFGLLog::LogToHost( "SwarmMorph: float framebuffer is not supported" );
			return false;
		}
	}
	glGenVertexArrays( 1, &emptyVAO );

	std::vector< float > indices( kMaxParticles );
	for( int i = 0; i < kMaxParticles; ++i )
		indices[ i ] = static_cast< float >( i );// exact: well below 2^24
	glGenBuffers( 1, &indexVBO );
	glGenVertexArrays( 1, &indexVAO );
	{
		ScopedVAOBinding vaoBinding( indexVAO );
		ScopedVBOBinding vboBinding( indexVBO );
		glBufferData( GL_ARRAY_BUFFER, indices.size() * sizeof( float ), indices.data(), GL_STATIC_DRAW );
		glEnableVertexAttribArray( 0 );
		glVertexAttribPointer( 0, 1, GL_FLOAT, GL_FALSE, 0, (const void*)0 );
	}
	// Streaks draw six corners per particle, instanced: the same indices, one per instance.
	glGenVertexArrays( 1, &streakVAO );
	{
		ScopedVAOBinding vaoBinding( streakVAO );
		ScopedVBOBinding vboBinding( indexVBO );
		glEnableVertexAttribArray( 0 );
		glVertexAttribPointer( 0, 1, GL_FLOAT, GL_FALSE, 0, (const void*)0 );
		glVertexAttribDivisor( 0, 1 );
	}
	front      = 0;
	activeSlot = -1;

	// Meshes that were set before the GL context existed (or before a context loss).
	for( int i = 0; i < kNumSlots; ++i )
	{
		slots[ i ].loaded = false;
		if( !slots[ i ].path.empty() && !slots[ i ].pending )
			StartLoad( i, slots[ i ].path );
	}
	GLenum error = glGetError();
	if( error != GL_NO_ERROR )
	{
		char message[ 64 ];
		snprintf( message, sizeof( message ), "SwarmMorph: GL error 0x%04X while creating resources", error );
		FFGLLog::LogToHost( message );
		return false;
	}
	return true;
}

void SwarmMorph::ReleaseGLResources()
{
	updateShader.FreeGLResources();
	renderShader.FreeGLResources();
	streakShader.FreeGLResources();
	skinShader.FreeGLResources();
	for( StateBuffer& buffer : state )
	{
		glDeleteFramebuffers( 1, &buffer.fbo );
		glDeleteTextures( 1, &buffer.positions );
		glDeleteTextures( 1, &buffer.velocities );
		buffer = StateBuffer();
	}
	glDeleteVertexArrays( 1, &emptyVAO );
	glDeleteVertexArrays( 1, &indexVAO );
	glDeleteVertexArrays( 1, &streakVAO );
	glDeleteBuffers( 1, &indexVBO );
	emptyVAO = indexVAO = streakVAO = indexVBO = 0;
	glDeleteTextures( 1, &fallbackTexture );
	fallbackTexture = 0;
	for( Slot& slot : slots )
	{
		ReleaseSlotGL( slot );
		slot.loaded = false;
	}
	activeSlot = -1;
}

void SwarmMorph::ReleaseSlotGL( Slot& slot )
{
	glDeleteFramebuffers( 1, &slot.fbo );
	glDeleteTextures( 1, &slot.texture );
	glDeleteTextures( 1, &slot.normals );
	glDeleteTextures( 1, &slot.vertexTexture );
	glDeleteTextures( 1, &slot.bindIndices );
	glDeleteTextures( 1, &slot.bindWeights );
	slot.fbo = slot.texture = slot.normals = slot.vertexTexture = slot.bindIndices = slot.bindWeights = 0;
}

void SwarmMorph::StartLoad( int slotIndex, const std::string& path )
{
	Slot& slot   = slots[ slotIndex ];
	slot.path    = path;
	slot.pending = nullptr;
	if( path.empty() )
	{
		slot.loaded = false;
		return;
	}

	auto job       = std::make_shared< LoadJob >();
	job->path      = path;
	slot.pending   = job;
	bool normalize       = normalizeMeshes;
	bool detectSequences = this->detectSequences;
	uint32_t seed        = 1000u + static_cast< uint32_t >( slotIndex );
	std::thread( [ job, normalize, detectSequences, seed ]() {
		try
		{
			swarm::MeshSequence sequence;
			bool loaded = false;
			if( IsAlembicPath( job->path ) )
			{
				loaded = swarm::LoadAlembic( job->path, sequence, job->error, job->warning );
			}
			else
			{
				std::vector< std::string > files = detectSequences ? swarm::FindObjSequence( job->path ) : std::vector< std::string >{ job->path };
				loaded = swarm::LoadObjSequence( files, sequence, job->error, job->warning );
			}
			if( loaded )
			{
				job->fps = sequence.fps;
				std::vector< const std::vector< float >* > allFrames;
				for( const std::vector< float >& frame : sequence.frames )
					allFrames.push_back( &frame );
				swarm::Normalization normalization = swarm::ComputeNormalization( allFrames );
				if( normalize )
					for( std::vector< float >& frame : sequence.frames )
						normalization.Apply( frame );

				swarm::SurfaceBinding binding = swarm::BindSurface( sequence.firstFrame, kMaxParticles, seed );
				if( sequence.frames.size() > 1 )
				{
					job->binding = std::move( binding );
					job->frames  = std::move( sequence.frames );
				}
				else
				{
					job->points  = swarm::EvaluateBinding( binding, sequence.frames[ 0 ] );
					job->normals = swarm::EvaluateNormals( binding, sequence.frames[ 0 ] );
				}
			}
		}
		catch( const std::exception& e )
		{
			job->error = std::string( "Failed to load mesh: " ) + e.what();
			job->points.clear();
			job->frames.clear();
		}
		job->done = true;
	} ).detach();
}

void SwarmMorph::UploadFinishedLoads()
{
	for( Slot& slot : slots )
	{
		if( !slot.pending || !slot.pending->done )
			continue;
		std::shared_ptr< LoadJob > job = std::move( slot.pending );
		if( !job->warning.empty() )
			FFGLLog::LogToHost( ( "SwarmMorph: " + job->path + ": " + job->warning ).c_str() );
		if( job->points.empty() && job->frames.empty() )
		{
			FFGLLog::LogToHost( ( "SwarmMorph: " + job->error ).c_str() );
			slot.loaded = false;
			continue;
		}

		// A slot can switch between static and animated, which use different texture formats.
		ReleaseSlotGL( slot );
		slot.frames.clear();
		slot.blended.clear();
		if( job->frames.empty() )
		{
			slot.texture = CreateFloatTexture( GL_RGB32F, GL_RGB, job->points.data() );
			slot.normals = CreateFloatTexture( GL_RGB32F, GL_RGB, job->normals.data() );
		}
		else
		{
			UploadAnimation( slot, *job );
			char fps[ 16 ];
			snprintf( fps, sizeof( fps ), "%.3g", slot.fps );
			std::string message = "SwarmMorph: loaded " + std::to_string( slot.frames.size() ) + "-frame animation at " + fps + " fps: " + job->path;
			FFGLLog::LogToHost( message.c_str() );
		}
		slot.loaded = true;
	}
}

void SwarmMorph::UploadAnimation( Slot& slot, LoadJob& job )
{
	slot.frames      = std::move( job.frames );
	size_t vertices  = slot.frames[ 0 ].size() / 3;
	slot.vertexRows  = static_cast< int >( ( vertices + kVertexTextureWidth - 1 ) / kVertexTextureWidth );
	slot.blended.assign( static_cast< size_t >( slot.vertexRows ) * kVertexTextureWidth * 3, 0.0f );
	slot.vertexTexture = CreateTexture( GL_RGB32F, GL_RGB, GL_FLOAT, kVertexTextureWidth, slot.vertexRows, nullptr );
	slot.bindIndices   = CreateTexture( GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, kTextureWidth, kTextureWidth, job.binding.indices.data() );
	slot.bindWeights   = CreateFloatTexture( GL_RGBA32F, GL_RGBA, job.binding.weights.data() );
	slot.texture       = CreateFloatTexture( GL_RGBA32F, GL_RGBA, nullptr );
	slot.normals       = CreateFloatTexture( GL_RGBA32F, GL_RGBA, nullptr );

	glGenFramebuffers( 1, &slot.fbo );
	ScopedFBOBinding fboBinding( slot.fbo, ScopedFBOBinding::RB_REVERT );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, slot.normals, 0 );
	const GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers( 2, drawBuffers );

	slot.fps           = job.fps > 0.0f ? job.fps : 24.0f;
	slot.playhead      = 0.0;
	slot.evaluatedAt   = -1.0;
	slot.evaluatedRows = 0;
}

double SwarmMorph::FramePosition( const Slot& slot ) const
{
	double frameCount = static_cast< double >( slot.frames.size() );
	if( pingPong )
	{
		double period   = 2.0 * ( frameCount - 1.0 );
		double position = std::fmod( slot.playhead, period );
		return position <= frameCount - 1.0 ? position : period - position;
	}
	// Looping blends the last frame back into the first, so sequences exported
	// as a seamless cycle play without a hitch.
	return std::fmod( slot.playhead, frameCount );
}

void SwarmMorph::AdvanceAnimation( int slotIndex, float deltaTime )
{
	if( slotIndex < 0 || !slots[ slotIndex ].IsAnimated() )
		return;
	Slot& slot = slots[ slotIndex ];
	slot.playhead += deltaTime * slot.fps * AnimSpeedValue( animSpeed );

	double position = FramePosition( slot );
	int rows        = ( ParticleCount() + kTextureWidth - 1 ) / kTextureWidth;
	if( position == slot.evaluatedAt && rows <= slot.evaluatedRows )
		return;

	// Blend the two neighbouring frames on the CPU; it is only the vertices,
	// not the particles, so it stays cheap.
	size_t frameCount       = slot.frames.size();
	size_t first            = static_cast< size_t >( position ) % frameCount;
	size_t second           = ( first + 1 ) % frameCount;
	if( pingPong && second == 0 )
		second = first;
	float blend             = static_cast< float >( position - std::floor( position ) );
	const std::vector< float >& a = slot.frames[ first ];
	const std::vector< float >& b = slot.frames[ second ];
	for( size_t i = 0; i < a.size(); ++i )
		slot.blended[ i ] = a[ i ] + ( b[ i ] - a[ i ] ) * blend;
	{
		Scoped2DTextureBinding binding( slot.vertexTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, kVertexTextureWidth, slot.vertexRows, GL_RGB, GL_FLOAT, slot.blended.data() );
	}

	// Skin pass: evaluate every particle's binding against the blended frame.
	ScopedFBOBinding fboBinding( slot.fbo, ScopedFBOBinding::RB_REVERT );
	glViewport( 0, 0, kTextureWidth, rows );
	ScopedShaderBinding shaderBinding( skinShader.GetGLID() );
	skinShader.Set( "uVertices", 0 );
	skinShader.Set( "uBindIndices", 1 );
	skinShader.Set( "uBindWeights", 2 );
	skinShader.Set( "uVertexTextureWidth", kVertexTextureWidth );
	ScopedTextureUnits textures( { slot.vertexTexture, slot.bindIndices, slot.bindWeights } );
	ScopedVAOBinding vaoBinding( emptyVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glViewport( currentViewport.x, currentViewport.y, currentViewport.width, currentViewport.height );

	slot.evaluatedAt   = position;
	slot.evaluatedRows = rows;
}

void SwarmMorph::SelectNextLoadedSlot()
{
	for( int step = 1; step <= kNumSlots; ++step )
	{
		int candidate = ( targetSlot + step ) % kNumSlots;
		if( slots[ candidate ].loaded || slots[ candidate ].pending )
		{
			targetSlot = candidate;
			return;
		}
	}
}

void SwarmMorph::CurrentTarget( GLuint& targets, GLuint& normals )
{
	// An empty or still-loading slot keeps the swarm heading where it was.
	int previous = activeSlot;
	if( slots[ targetSlot ].loaded )
		activeSlot = targetSlot;
	if( activeSlot >= 0 && !slots[ activeSlot ].loaded )
		activeSlot = -1;
	if( activeSlot != previous && activeSlot >= 0 && restartOnSwitch )
		slots[ activeSlot ].playhead = 0.0;

	if( activeSlot >= 0 )
	{
		targets = slots[ activeSlot ].texture;
		normals = slots[ activeSlot ].normals;
	}
	else
	{
		// On the unit sphere, the position is the normal.
		targets = normals = fallbackTexture;
	}
}

float SwarmMorph::MorphTarget( GLuint& targets, GLuint& normals )
{
	int previous = morphActiveSlot;
	morphActiveSlot = morph > 0.0f && slots[ morphSlot ].loaded && morphSlot != activeSlot ? morphSlot : -1;
	if( morphActiveSlot != previous && morphActiveSlot >= 0 && restartOnSwitch )
		slots[ morphActiveSlot ].playhead = 0.0;
	if( morphActiveSlot < 0 )
		return 0.0f;
	targets = slots[ morphActiveSlot ].texture;
	normals = slots[ morphActiveSlot ].normals;
	return morph;
}

void SwarmMorph::UpdateBeat( float deltaTime )
{
	bool kick  = hitPending;
	hitPending = false;

	// Resolume sends the tempo and the phase within the bar every frame.
	if( bpm > 0.0f && std::isfinite( barPhase ) )
	{
		if( lastBarPhase >= 0.0f && barPhase < lastBarPhase - 0.5f )
			++barCount;
		double position = static_cast< double >( barCount ) + barPhase;
		if( lastBarPhase >= 0.0f )
		{
			auto crossed = [ & ]( double interval ) {
				return interval > 0.0 && std::floor( position / interval ) != std::floor( lastBarPosition / interval );
			};
			if( crossed( kBeatIntervals[ beatRate ] ) )
				kick = true;
			if( crossed( kAutoNextBars[ autoNext ] ) )
				SelectNextLoadedSlot();
		}
		lastBarPhase    = barPhase;
		lastBarPosition = position;
	}

	beatKick = kick;
	if( kick )
	{
		beatAge  = 0.0f;
		beatSeed = std::fmod( beatSeed + 0.618034f, 1.0f );// a new random pick per beat
		stepTarget += SpinStepValue( spinStep ) * kPi / 180.0f;
	}
	else
		beatAge += deltaTime;
	beatEnvelope = std::exp( -beatAge / DecaySeconds( decay ) );

	// The shockwave takes most of the time until the next beat to cross the
	// shape (one beat when triggered by hand), so it moves with the music.
	double beatSeconds = bpm > 0.0f ? 60.0 / bpm : 0.5;
	double interval    = beatRate > 0 ? kBeatIntervals[ beatRate ] * 4.0 * beatSeconds : beatSeconds;
	waveDuration       = static_cast< float >( std::min( std::max( 0.9 * interval, 0.3 ), 2.0 ) );

	// Spin Step: the camera springs to the new angle with a little overshoot.
	float omega = std::min( 2.0f * kPi / ( 0.25f + DecaySeconds( decay ) ), 20.0f );
	for( int i = 0; i < 4; ++i )
	{
		float step = deltaTime / 4.0f;
		stepVelocity += ( omega * omega * ( stepTarget - stepAngle ) - 1.2f * omega * stepVelocity ) * step;
		stepAngle += stepVelocity * step;
	}
	// Keep both angles small without changing their difference.
	float turns = std::floor( stepTarget / ( 2.0f * kPi ) ) * 2.0f * kPi;
	stepTarget -= turns;
	stepAngle -= turns;
}

int SwarmMorph::ParticleCount() const
{
	return kMinParticles + static_cast< int >( ( kMaxParticles - kMinParticles ) * particles * particles );
}

FFResult SwarmMorph::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( !updateShader.IsReady() || !renderShader.IsReady() )
		return FF_FAIL;

	float deltaTime = 0.0f;
	if( lastTime >= 0.0 )
		deltaTime = static_cast< float >( ( hostTime - lastTime ) / 1000.0 );
	lastTime  = hostTime;
	// Big jumps (clip retriggered, host stalled) would make the springs explode.
	if( !std::isfinite( deltaTime ) )
		deltaTime = 0.0f;
	deltaTime = std::min( std::max( deltaTime, 0.0f ), 1.0f / 20.0f );
	simTime += deltaTime;
	spinAngle += SpinValue( spin ) * 2.0f * kPi * deltaTime;
	pathTravel = std::fmod( pathTravel + deltaTime * PathSpeedValue( pathSpeed ), 1.0f );

	UploadFinishedLoads();
	// Speed slows or speeds up the particles only; beat, animation and
	// camera keep running in real time.
	float swarmDelta = deltaTime * SpeedValue( speed );
	swarmTime += swarmDelta;
	noiseTime += swarmDelta * FlowSpeedValue( flowSpeed );
	buzzTime += swarmDelta * BuzzSpeedValue( buzzSpeed );
	UpdateBeat( deltaTime );
	GLuint targets, normals;
	CurrentTarget( targets, normals );
	GLuint morphTargets = targets, morphNormals = normals;
	float morphAmount   = MorphTarget( morphTargets, morphNormals );
	AdvanceAnimation( activeSlot, deltaTime );
	AdvanceAnimation( morphActiveSlot, deltaTime );
	Simulate( swarmDelta, targets, normals, morphTargets, morphNormals, morphAmount );
	Render( morphAmount > 0.5f ? morphNormals : normals );
	return FF_SUCCESS;
}

void SwarmMorph::Simulate( float deltaTime, GLuint targets, GLuint normals, GLuint morphTargets, GLuint morphNormals, float morphAmount )
{
	const StateBuffer& source = state[ front ];
	const StateBuffer& dest   = state[ 1 - front ];

	ScopedFBOBinding fboBinding( dest.fbo, ScopedFBOBinding::RB_REVERT );
	// Only the rows that hold live particles need updating.
	int rows = ( ParticleCount() + kTextureWidth - 1 ) / kTextureWidth;
	glViewport( 0, 0, kTextureWidth, rows );

	ScopedShaderBinding shaderBinding( updateShader.GetGLID() );
	updateShader.Set( "uDeltaTime", deltaTime );
	updateShader.Set( "uTime", swarmTime );
	updateShader.Set( "uAttraction", AttractionValue( attraction ) );
	updateShader.Set( "uDampingRatio", DampingValue( damping ) );
	updateShader.Set( "uChaos", chaos );
	updateShader.Set( "uTurbulence", TurbulenceValue( turbulence ) );
	updateShader.Set( "uNoiseScale", NoiseScaleValue( noiseScale ) );
	// Wind is set relative to the screen (X = right), so it keeps blowing the
	// same way while the camera spins; the shader works in world space.
	Camera camera    = CurrentCamera();
	const swarm::Mat4& view = camera.view;
	float wind[ 3 ]  = { WindValue( windX ), WindValue( windY ), WindValue( windZ ) };
	float worldWind[ 3 ];
	for( int i = 0; i < 3; ++i )
		worldWind[ i ] = view.m[ i * 4 + 0 ] * wind[ 0 ] + view.m[ i * 4 + 1 ] * wind[ 1 ] + view.m[ i * 4 + 2 ] * wind[ 2 ];
	updateShader.Set( "uWind", worldWind[ 0 ], worldWind[ 1 ], worldWind[ 2 ] );
	updateShader.Set( "uGusts", gusts );
	updateShader.Set( "uVortex", VortexValue( vortex ) );
	updateShader.Set( "uRadial", RadialValue( radial ) );
	updateShader.Set( "uBlowAway", blowAway );
	updateShader.Set( "uWrap", wrapEdges ? 1.0f : 0.0f );
	const float rotation[ 9 ] = { view.m[ 0 ], view.m[ 1 ], view.m[ 2 ], view.m[ 4 ], view.m[ 5 ], view.m[ 6 ], view.m[ 8 ], view.m[ 9 ], view.m[ 10 ] };
	glUniformMatrix3fv( updateShader.FindUniform( "uViewRotation" ), 1, GL_FALSE, rotation );
	updateShader.Set( "uViewTranslation", view.m[ 12 ], view.m[ 13 ], view.m[ 14 ] );
	float tanHalfFov = std::tan( camera.fovY * 0.5f );
	updateShader.Set( "uFrustum", tanHalfFov * ViewportAspect(), tanHalfFov );
	updateShader.Set( "uDepthRange", std::max( camera.distance - 3.0f, 0.3f ), camera.distance + 3.0f );
	updateShader.Set( "uPositions", 0 );
	updateShader.Set( "uVelocities", 1 );
	updateShader.Set( "uTargets", 2 );
	updateShader.Set( "uNormals", 3 );
	updateShader.Set( "uMorphTargets", 4 );
	updateShader.Set( "uMorphNormals", 5 );
	updateShader.Set( "uMorph", morphAmount );
	updateShader.Set( "uBeatKick", beatKick ? 1.0f : 0.0f );
	updateShader.Set( "uBeatSeed", beatSeed );
	updateShader.Set( "uBurst", BurstValue( burst ) );
	updateShader.Set( "uNoiseTime", noiseTime );
	updateShader.Set( "uBuzz", BuzzValue( buzzAmount ) );
	updateShader.Set( "uBuzzTime", buzzTime );
	updateShader.Set( "uSurfaceFlow", SurfaceFlowValue( surfaceFlow ) );
	updateShader.Set( "uHover", HoverValue( hover ) );

	ScopedTextureUnits textures( { source.positions, source.velocities, targets, normals, morphTargets, morphNormals } );

	ScopedVAOBinding vaoBinding( emptyVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	glViewport( currentViewport.x, currentViewport.y, currentViewport.width, currentViewport.height );
	front = 1 - front;
}

float SwarmMorph::ViewportAspect() const
{
	return static_cast< float >( std::max< GLuint >( currentViewport.width, 1 ) ) /
		   static_cast< float >( std::max< GLuint >( currentViewport.height, 1 ) );
}

SwarmMorph::Camera SwarmMorph::CurrentCamera() const
{
	// Rotate X/Y, Spin and Spin Step turn the shape; the camera itself either
	// sits in front of it or travels along a path.
	swarm::Mat4 turn = swarm::Mat4::RotationX( AngleValue( rotateX ) * kPi / 180.0f ) *
					   swarm::Mat4::RotationY( AngleValue( rotateY ) * kPi / 180.0f + spinAngle + stepAngle );
	Camera camera;
	if( cameraPath > 0 )
	{
		// Zoom scales the whole path, so it frames tighter or wider.
		float scale            = DistanceValue( zoom ) / DistanceValue( 0.5f );
		swarm::CameraPose pose = swarm::EvaluateCameraPath( cameraPath, pathPosition + pathTravel );
		camera.view            = swarm::Mat4::LookAt( pose.eye * scale, pose.target * scale ) * turn;
		camera.fovY            = pose.fovY;
		camera.distance        = ( pose.eye * scale ).Length();
	}
	else
	{
		camera.view     = swarm::Mat4::Translation( 0.0f, 0.0f, -DistanceValue( zoom ) ) * turn;
		camera.fovY     = kFovY;
		camera.distance = DistanceValue( zoom );
	}
	return camera;
}

void SwarmMorph::Render( GLuint normals )
{
	float height = static_cast< float >( std::max< GLuint >( currentViewport.height, 1 ) );

	Camera camera          = CurrentCamera();
	swarm::Mat4 projection = swarm::Mat4::Perspective( camera.fovY, ViewportAspect(), 0.05f, 100.0f );
	swarm::Mat4 mvp        = projection * camera.view;

	// Point size is given in pixels at 1080p and at the default camera distance,
	// so the look stays the same across output resolutions.
	// A wider lens shows the shape smaller, so points shrink with it.
	float pointScale = PointSizeValue( pointSize ) * ( height / 1080.0f ) * std::tan( kFovY * 0.5f ) / std::tan( camera.fovY * 0.5f );

	glClear( GL_COLOR_BUFFER_BIT );

	bool streaks           = streakLength > 0.0f;
	ffglex::FFGLShader& shader = streaks ? streakShader : renderShader;
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	glUniformMatrix4fv( shader.FindUniform( "uMVP" ), 1, GL_FALSE, mvp.m );
	shader.Set( "uPointSize", pointScale );
	shader.Set( "uRefDistance", DistanceValue( 0.5f ) );
	float rgb[ 3 ];
	HsvToRgb( colorH, colorS, colorV * colorA, rgb );
	shader.Set( "uColor", rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
	shader.Set( "uHeat", heat );
	shader.Set( "uBrightness", BrightnessValue( brightness ) * ( 1.0f + FlashValue( flash ) * beatEnvelope ) );
	HsvToRgb( color2H, color2S, color2V * color2A, rgb );
	shader.Set( "uColor2", rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
	shader.Set( "uSoftness", 0.35f + 3.65f * softness );// falloff exponent of each point
	shader.Set( "uColorMode", colorMode );
	shader.Set( "uColorMix", colorMix );
	shader.Set( "uTwinkle", twinkle );
	shader.Set( "uDepthFade", depthFade );
	shader.Set( "uDistance", camera.distance );
	shader.Set( "uTime", simTime );
	shader.Set( "uBeat", beatEnvelope );
	shader.Set( "uBeatSeed", beatSeed );
	shader.Set( "uPulse", PulseValue( pulse ) );
	shader.Set( "uShockwave", ShockwaveValue( shockwave ) );
	shader.Set( "uWave", beatAge / waveDuration );
	shader.Set( "uJitter", JitterValue( jitter ) );
	shader.Set( "uPositions", 0 );
	shader.Set( "uVelocities", 1 );
	shader.Set( "uNormals", 2 );
	shader.Set( "uTextureWidth", kTextureWidth );

	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	if( streaks )
	{
		// The other state buffer still holds the previous frame.
		shader.Set( "uPreviousPositions", 3 );
		shader.Set( "uStreakLength", StreakValue( streakLength ) );
		shader.Set( "uViewport", static_cast< float >( currentViewport.width ), static_cast< float >( currentViewport.height ) );
		ScopedTextureUnits textures( { state[ front ].positions, state[ front ].velocities, normals, state[ 1 - front ].positions } );
		ScopedVAOBinding vaoBinding( streakVAO );
		glDrawArraysInstanced( GL_TRIANGLES, 0, 6, ParticleCount() );
	}
	else
	{
		ScopedTextureUnits textures( { state[ front ].positions, state[ front ].velocities, normals } );
		ScopedVAOBinding vaoBinding( indexVAO );
		glEnable( GL_PROGRAM_POINT_SIZE );
		glDrawArrays( GL_POINTS, 0, ParticleCount() );
		glDisable( GL_PROGRAM_POINT_SIZE );
	}
	// FFGL requires the context to be returned in its default state.
	glBlendFunc( GL_ONE, GL_ZERO );
	glDisable( GL_BLEND );
}

FFResult SwarmMorph::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PID_TARGET:
		targetSlot = std::min( std::max( static_cast< int >( std::lround( value ) ) - 1, 0 ), kNumSlots - 1 );
		break;
	case PID_MORPH: morph = value; break;
	case PID_BEAT_RATE: beatRate = std::min( std::max( static_cast< int >( std::lround( value ) ), 0 ), 5 ); break;
	case PID_HIT:
		if( value > 0.5f && !hitPressed )
			hitPending = true;
		hitPressed = value > 0.5f;
		break;
	case PID_DECAY: decay = value; break;
	case PID_PULSE: pulse = value; break;
	case PID_SHOCKWAVE: shockwave = value; break;
	case PID_BURST: burst = value; break;
	case PID_JITTER: jitter = value; break;
	case PID_FLASH: flash = value; break;
	case PID_SPIN_STEP: spinStep = value; break;
	case PID_AUTO_NEXT: autoNext = std::min( std::max( static_cast< int >( std::lround( value ) ), 0 ), 4 ); break;
	case PID_MORPH_TO:
		morphSlot = std::min( std::max( static_cast< int >( std::lround( value ) ) - 1, 0 ), kNumSlots - 1 );
		break;
	case PID_NEXT:
		if( value > 0.5f && !nextPressed )
			SelectNextLoadedSlot();
		nextPressed = value > 0.5f;
		break;
	case PID_NORMALIZE:
		if( ( value > 0.5f ) != normalizeMeshes )
		{
			normalizeMeshes = value > 0.5f;
			for( int i = 0; i < kNumSlots; ++i )
				if( !slots[ i ].path.empty() )
					StartLoad( i, slots[ i ].path );
		}
		break;
	case PID_PARTICLES: particles = value; break;
	case PID_SPEED: speed = value; break;
	case PID_ATTRACTION: attraction = value; break;
	case PID_DAMPING: damping = value; break;
	case PID_CHAOS: chaos = value; break;
	case PID_TURBULENCE: turbulence = value; break;
	case PID_NOISE_SCALE: noiseScale = value; break;
	case PID_POINT_SIZE: pointSize = value; break;
	case PID_COLOR_H: colorH = value; break;
	case PID_COLOR_A: colorA = value; break;
	case PID_COLOR2_A: color2A = value; break;
	case PID_SOFTNESS: softness = value; break;
	case PID_STREAKS: streakLength = value; break;
	case PID_COLOR_S: colorS = value; break;
	case PID_COLOR_V: colorV = value; break;
	case PID_HEAT: heat = value; break;
	case PID_BRIGHTNESS: brightness = value; break;
	case PID_COLOR2_H: color2H = value; break;
	case PID_COLOR2_S: color2S = value; break;
	case PID_COLOR2_V: color2V = value; break;
	case PID_COLOR_MODE: colorMode = std::min( std::max( static_cast< int >( std::lround( value ) ), 0 ), 3 ); break;
	case PID_COLOR_MIX: colorMix = value; break;
	case PID_TWINKLE: twinkle = value; break;
	case PID_DEPTH_FADE: depthFade = value; break;
	case PID_SPIN: spin = value; break;
	case PID_ROTATE_X: rotateX = value; break;
	case PID_ROTATE_Y: rotateY = value; break;
	case PID_ZOOM: zoom = value; break;
	case PID_PATH: cameraPath = std::min( std::max( static_cast< int >( std::lround( value ) ), 0 ), swarm::CameraPathCount() ); break;
	case PID_PATH_POSITION: pathPosition = value; break;
	case PID_PATH_SPEED: pathSpeed = value; break;
	case PID_WIND_X: windX = value; break;
	case PID_WIND_Y: windY = value; break;
	case PID_WIND_Z: windZ = value; break;
	case PID_GUSTS: gusts = value; break;
	case PID_VORTEX: vortex = value; break;
	case PID_RADIAL: radial = value; break;
	case PID_BLOW_AWAY: blowAway = value; break;
	case PID_WRAP: wrapEdges = value > 0.5f; break;
	case PID_ANIM_SPEED: animSpeed = value; break;
	case PID_SURFACE_FLOW: surfaceFlow = value; break;
	case PID_HOVER: hover = value; break;
	case PID_FLOW_SPEED: flowSpeed = value; break;
	case PID_BUZZ: buzzAmount = value; break;
	case PID_BUZZ_SPEED: buzzSpeed = value; break;
	case PID_PING_PONG: pingPong = value > 0.5f; break;
	case PID_RESTART: restartOnSwitch = value > 0.5f; break;
	case PID_SEQUENCES:
		if( ( value > 0.5f ) != detectSequences )
		{
			detectSequences = value > 0.5f;
			for( int i = 0; i < kNumSlots; ++i )
				if( !slots[ i ].path.empty() )
					StartLoad( i, slots[ i ].path );
		}
		break;
	default:
		return FF_FAIL;
	}
	return FF_SUCCESS;
}

float SwarmMorph::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PID_TARGET: return static_cast< float >( targetSlot + 1 );
	case PID_NEXT: return nextPressed ? 1.0f : 0.0f;
	case PID_MORPH: return morph;
	case PID_BEAT_RATE: return static_cast< float >( beatRate );
	case PID_HIT: return hitPressed ? 1.0f : 0.0f;
	case PID_DECAY: return decay;
	case PID_PULSE: return pulse;
	case PID_SHOCKWAVE: return shockwave;
	case PID_BURST: return burst;
	case PID_JITTER: return jitter;
	case PID_FLASH: return flash;
	case PID_SPIN_STEP: return spinStep;
	case PID_AUTO_NEXT: return static_cast< float >( autoNext );
	case PID_MORPH_TO: return static_cast< float >( morphSlot + 1 );
	case PID_NORMALIZE: return normalizeMeshes ? 1.0f : 0.0f;
	case PID_PARTICLES: return particles;
	case PID_SPEED: return speed;
	case PID_ATTRACTION: return attraction;
	case PID_DAMPING: return damping;
	case PID_CHAOS: return chaos;
	case PID_TURBULENCE: return turbulence;
	case PID_NOISE_SCALE: return noiseScale;
	case PID_POINT_SIZE: return pointSize;
	case PID_COLOR_H: return colorH;
	case PID_COLOR_A: return colorA;
	case PID_COLOR2_A: return color2A;
	case PID_SOFTNESS: return softness;
	case PID_STREAKS: return streakLength;
	case PID_COLOR_S: return colorS;
	case PID_COLOR_V: return colorV;
	case PID_HEAT: return heat;
	case PID_BRIGHTNESS: return brightness;
	case PID_COLOR2_H: return color2H;
	case PID_COLOR2_S: return color2S;
	case PID_COLOR2_V: return color2V;
	case PID_COLOR_MODE: return static_cast< float >( colorMode );
	case PID_COLOR_MIX: return colorMix;
	case PID_TWINKLE: return twinkle;
	case PID_DEPTH_FADE: return depthFade;
	case PID_SPIN: return spin;
	case PID_ROTATE_X: return rotateX;
	case PID_ROTATE_Y: return rotateY;
	case PID_ZOOM: return zoom;
	case PID_PATH: return static_cast< float >( cameraPath );
	case PID_PATH_POSITION: return pathPosition;
	case PID_PATH_SPEED: return pathSpeed;
	case PID_WIND_X: return windX;
	case PID_WIND_Y: return windY;
	case PID_WIND_Z: return windZ;
	case PID_GUSTS: return gusts;
	case PID_VORTEX: return vortex;
	case PID_RADIAL: return radial;
	case PID_BLOW_AWAY: return blowAway;
	case PID_WRAP: return wrapEdges ? 1.0f : 0.0f;
	case PID_ANIM_SPEED: return animSpeed;
	case PID_SURFACE_FLOW: return surfaceFlow;
	case PID_HOVER: return hover;
	case PID_FLOW_SPEED: return flowSpeed;
	case PID_BUZZ: return buzzAmount;
	case PID_BUZZ_SPEED: return buzzSpeed;
	case PID_PING_PONG: return pingPong ? 1.0f : 0.0f;
	case PID_RESTART: return restartOnSwitch ? 1.0f : 0.0f;
	case PID_SEQUENCES: return detectSequences ? 1.0f : 0.0f;
	default: return 0.0f;
	}
}

FFResult SwarmMorph::SetTextParameter( unsigned int index, const char* value )
{
	if( !IsMeshParam( index ) )
		return FF_FAIL;
	int slotIndex    = static_cast< int >( index - PID_MESH_FIRST );
	std::string path = value ? value : "";
	if( path != slots[ slotIndex ].path )
		StartLoad( slotIndex, path );
	return FF_SUCCESS;
}

char* SwarmMorph::GetTextParameter( unsigned int index )
{
	if( !IsMeshParam( index ) )
		return CFFGLPlugin::GetTextParameter( index );
	return const_cast< char* >( slots[ index - PID_MESH_FIRST ].path.c_str() );
}

char* SwarmMorph::GetParameterDisplay( unsigned int index )
{
	static char buffer[ 32 ];
	switch( index )
	{
	case PID_PARTICLES: snprintf( buffer, sizeof( buffer ), "%d", ParticleCount() ); return buffer;
	case PID_MORPH: snprintf( buffer, sizeof( buffer ), "%.0f%%", morph * 100.0f ); return buffer;
	case PID_DECAY: snprintf( buffer, sizeof( buffer ), "%.2f s", DecaySeconds( decay ) ); return buffer;
	case PID_BURST: snprintf( buffer, sizeof( buffer ), "%.0f%%", BurstValue( burst ) * 100.0f ); return buffer;
	case PID_SPIN_STEP: snprintf( buffer, sizeof( buffer ), "%.0f deg", SpinStepValue( spinStep ) ); return buffer;
	case PID_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", SpeedValue( speed ) ); return buffer;
	case PID_ATTRACTION: snprintf( buffer, sizeof( buffer ), "%.2f", AttractionValue( attraction ) ); return buffer;
	case PID_DAMPING: snprintf( buffer, sizeof( buffer ), "%.2f", DampingValue( damping ) ); return buffer;
	case PID_TURBULENCE: snprintf( buffer, sizeof( buffer ), "%.2f", TurbulenceValue( turbulence ) ); return buffer;
	case PID_NOISE_SCALE: snprintf( buffer, sizeof( buffer ), "%.2f", NoiseScaleValue( noiseScale ) ); return buffer;
	case PID_POINT_SIZE: snprintf( buffer, sizeof( buffer ), "%.1f px", PointSizeValue( pointSize ) ); return buffer;
	case PID_STREAKS:
		if( streakLength <= 0.0f )
			return const_cast< char* >( "off" );
		snprintf( buffer, sizeof( buffer ), "%.1fx", StreakValue( streakLength ) );
		return buffer;
	case PID_SPIN: snprintf( buffer, sizeof( buffer ), "%.2f rps", SpinValue( spin ) ); return buffer;
	case PID_ROTATE_X: snprintf( buffer, sizeof( buffer ), "%.0f deg", AngleValue( rotateX ) ); return buffer;
	case PID_ROTATE_Y: snprintf( buffer, sizeof( buffer ), "%.0f deg", AngleValue( rotateY ) ); return buffer;
	case PID_ZOOM: snprintf( buffer, sizeof( buffer ), "%.2f", DistanceValue( zoom ) ); return buffer;
	case PID_PATH_POSITION: snprintf( buffer, sizeof( buffer ), "%.0f%%", pathPosition * 100.0f ); return buffer;
	case PID_PATH_SPEED:
		if( pathSpeed <= 0.0f )
			return const_cast< char* >( "off" );
		snprintf( buffer, sizeof( buffer ), "%.1f s/loop", 1.0f / PathSpeedValue( pathSpeed ) );
		return buffer;
	case PID_WIND_X: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windX ) ); return buffer;
	case PID_WIND_Y: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windY ) ); return buffer;
	case PID_WIND_Z: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windZ ) ); return buffer;
	case PID_VORTEX: snprintf( buffer, sizeof( buffer ), "%+.1f", VortexValue( vortex ) ); return buffer;
	case PID_RADIAL: snprintf( buffer, sizeof( buffer ), "%+.1f", RadialValue( radial ) ); return buffer;
	case PID_BLOW_AWAY: snprintf( buffer, sizeof( buffer ), "%.0f%%", blowAway * 100.0f ); return buffer;
	case PID_ANIM_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", AnimSpeedValue( animSpeed ) ); return buffer;
	case PID_FLOW_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", FlowSpeedValue( flowSpeed ) ); return buffer;
	case PID_BUZZ_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", BuzzSpeedValue( buzzSpeed ) ); return buffer;
	default: return CFFGLPlugin::GetParameterDisplay( index );
	}
}
