// SPDX-License-Identifier: GPL-3.0-or-later
#include "SwarmMorph.h"
#include <ffglex/FFGLScopedFBOBinding.h>
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
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
enum ParamID
{
	PID_MESH_FIRST = 0,
	PID_MESH_LAST  = PID_MESH_FIRST + SwarmMorph::kNumSlots - 1,
	PID_TARGET,
	PID_NEXT,
	PID_NORMALIZE,

	PID_PARTICLES,
	PID_ATTRACTION,
	PID_DAMPING,
	PID_CHAOS,
	PID_TURBULENCE,
	PID_SHIMMER,
	PID_NOISE_SCALE,
	PID_SCATTER,

	PID_POINT_SIZE,
	PID_COLOR_R,
	PID_COLOR_G,
	PID_COLOR_B,
	PID_HEAT,
	PID_BRIGHTNESS,

	PID_SPIN,
	PID_ROTATE_X,
	PID_ROTATE_Y,
	PID_ZOOM,

	// Appended so the IDs above stay stable for saved compositions.
	PID_WIND_X,
	PID_WIND_Y,
	PID_WIND_Z,
	PID_GUSTS,
	PID_VORTEX,
	PID_RADIAL,

	PID_ANIM_SPEED,
	PID_PING_PONG,
	PID_RESTART,
	PID_SEQUENCES,

	PID_SURFACE_FLOW,
	PID_HOVER,
	PID_FLOW_SPEED,
};

const float kPi              = 3.14159265358979f;
const float kFovY            = 40.0f * kPi / 180.0f;
const float kScatterImpulse  = 3.0f;
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
float AttractionValue( float v ) { return 1.0f + 40.0f * v * v; }
float DampingValue( float v ) { return 0.15f + 1.35f * v; }// fraction of critical damping
float TurbulenceValue( float v ) { return v; }
float ShimmerValue( float v ) { return 0.05f * v; }
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
float AnimSpeedValue( float v ) { return 3.0f * v; }
float SurfaceFlowValue( float v ) { return 0.4f * v; }
float HoverValue( float v ) { return 0.25f * v; }
float FlowSpeedValue( float v ) { return 2.0f * v; }

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
		SetFileParamInfo( PID_MESH_FIRST + i, name.c_str(), { "obj" }, "" );
		SetParamGroup( PID_MESH_FIRST + i, "Meshes" );
	}
	SetParamInfo( PID_TARGET, "Target", FF_TYPE_INTEGER, 1.0f );
	SetParamRange( PID_TARGET, 1.0f, static_cast< float >( kNumSlots ) );
	SetParamInfo( PID_NEXT, "Next", FF_TYPE_EVENT, false );
	SetParamInfo( PID_NORMALIZE, "Normalize", FF_TYPE_BOOLEAN, normalizeMeshes );
	SetParamGroup( PID_NORMALIZE, "Meshes" );

	SetParamInfof( PID_PARTICLES, "Particles", FF_TYPE_STANDARD );
	SetParamInfof( PID_ATTRACTION, "Attraction", FF_TYPE_STANDARD );
	SetParamInfof( PID_DAMPING, "Damping", FF_TYPE_STANDARD );
	SetParamInfof( PID_CHAOS, "Chaos", FF_TYPE_STANDARD );
	SetParamInfof( PID_TURBULENCE, "Turbulence", FF_TYPE_STANDARD );
	SetParamInfof( PID_SHIMMER, "Shimmer", FF_TYPE_STANDARD );
	SetParamInfof( PID_NOISE_SCALE, "Noise Scale", FF_TYPE_STANDARD );
	SetParamInfo( PID_SCATTER, "Scatter", FF_TYPE_EVENT, false );
	for( unsigned int id = PID_PARTICLES; id <= PID_SCATTER; ++id )
		SetParamGroup( id, "Motion" );

	SetParamInfof( PID_POINT_SIZE, "Point Size", FF_TYPE_STANDARD );
	SetParamInfof( PID_COLOR_R, "Color", FF_TYPE_RED );
	SetParamInfof( PID_COLOR_G, "Color Green", FF_TYPE_GREEN );
	SetParamInfof( PID_COLOR_B, "Color Blue", FF_TYPE_BLUE );
	SetParamInfof( PID_HEAT, "Speed Glow", FF_TYPE_STANDARD );
	SetParamInfof( PID_BRIGHTNESS, "Brightness", FF_TYPE_STANDARD );
	for( unsigned int id = PID_POINT_SIZE; id <= PID_BRIGHTNESS; ++id )
		SetParamGroup( id, "Look" );

	SetParamInfof( PID_SPIN, "Spin", FF_TYPE_STANDARD );
	SetParamInfof( PID_ROTATE_X, "Rotate X", FF_TYPE_STANDARD );
	SetParamInfof( PID_ROTATE_Y, "Rotate Y", FF_TYPE_STANDARD );
	SetParamInfof( PID_ZOOM, "Zoom", FF_TYPE_STANDARD );
	for( unsigned int id = PID_SPIN; id <= PID_ZOOM; ++id )
		SetParamGroup( id, "Camera" );

	SetParamInfof( PID_WIND_X, "Wind X", FF_TYPE_STANDARD );
	SetParamInfof( PID_WIND_Y, "Wind Y", FF_TYPE_STANDARD );
	SetParamInfof( PID_WIND_Z, "Wind Z", FF_TYPE_STANDARD );
	SetParamInfof( PID_GUSTS, "Gusts", FF_TYPE_STANDARD );
	SetParamInfof( PID_VORTEX, "Vortex", FF_TYPE_STANDARD );
	SetParamInfof( PID_RADIAL, "Explode", FF_TYPE_STANDARD );
	for( unsigned int id = PID_WIND_X; id <= PID_RADIAL; ++id )
		SetParamGroup( id, "Forces" );

	SetParamInfof( PID_ANIM_SPEED, "Anim Speed", FF_TYPE_STANDARD );
	SetParamInfo( PID_PING_PONG, "Ping Pong", FF_TYPE_BOOLEAN, pingPong );
	SetParamInfo( PID_RESTART, "Restart On Switch", FF_TYPE_BOOLEAN, restartOnSwitch );
	SetParamInfo( PID_SEQUENCES, "OBJ Sequences", FF_TYPE_BOOLEAN, detectSequences );
	for( unsigned int id = PID_ANIM_SPEED; id <= PID_SEQUENCES; ++id )
		SetParamGroup( id, "Animation" );

	SetParamInfof( PID_SURFACE_FLOW, "Surface Flow", FF_TYPE_STANDARD );
	SetParamInfof( PID_HOVER, "Hover", FF_TYPE_STANDARD );
	SetParamInfof( PID_FLOW_SPEED, "Flow Speed", FF_TYPE_STANDARD );
	for( unsigned int id = PID_SURFACE_FLOW; id <= PID_FLOW_SPEED; ++id )
		SetParamGroup( id, "Swarm" );

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
	if( !renderShader.Compile( swarm::kRenderVS, swarm::kRenderFS ) )
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
	glDeleteBuffers( 1, &indexVBO );
	emptyVAO = indexVAO = indexVBO = 0;
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
			std::vector< std::string > files = detectSequences ? swarm::FindObjSequence( job->path ) : std::vector< std::string >{ job->path };
			swarm::MeshSequence sequence;
			if( swarm::LoadObjSequence( files, sequence, job->error, job->warning ) )
			{
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
			std::string message = "SwarmMorph: loaded " + std::to_string( slot.frames.size() ) + "-frame sequence " + job->path;
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

void SwarmMorph::AdvanceAnimation( float deltaTime )
{
	if( activeSlot < 0 || !slots[ activeSlot ].IsAnimated() )
		return;
	Slot& slot = slots[ activeSlot ];
	slot.playhead += deltaTime * kSequenceFps * AnimSpeedValue( animSpeed );

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

	UploadFinishedLoads();
	noiseTime += deltaTime * FlowSpeedValue( flowSpeed );
	GLuint targets, normals;
	CurrentTarget( targets, normals );
	AdvanceAnimation( deltaTime );
	Simulate( deltaTime, targets, normals );
	Render();
	return FF_SUCCESS;
}

void SwarmMorph::Simulate( float deltaTime, GLuint targets, GLuint normals )
{
	const StateBuffer& source = state[ front ];
	const StateBuffer& dest   = state[ 1 - front ];

	ScopedFBOBinding fboBinding( dest.fbo, ScopedFBOBinding::RB_REVERT );
	// Only the rows that hold live particles need updating.
	int rows = ( ParticleCount() + kTextureWidth - 1 ) / kTextureWidth;
	glViewport( 0, 0, kTextureWidth, rows );

	ScopedShaderBinding shaderBinding( updateShader.GetGLID() );
	updateShader.Set( "uDeltaTime", deltaTime );
	updateShader.Set( "uTime", simTime );
	updateShader.Set( "uAttraction", AttractionValue( attraction ) );
	updateShader.Set( "uDampingRatio", DampingValue( damping ) );
	updateShader.Set( "uChaos", chaos );
	updateShader.Set( "uTurbulence", TurbulenceValue( turbulence ) );
	updateShader.Set( "uShimmer", ShimmerValue( shimmer ) );
	updateShader.Set( "uNoiseScale", NoiseScaleValue( noiseScale ) );
	updateShader.Set( "uScatter", scatterPending );
	updateShader.Set( "uWind", WindValue( windX ), WindValue( windY ), WindValue( windZ ) );
	updateShader.Set( "uGusts", gusts );
	updateShader.Set( "uVortex", VortexValue( vortex ) );
	updateShader.Set( "uRadial", RadialValue( radial ) );
	updateShader.Set( "uPositions", 0 );
	updateShader.Set( "uVelocities", 1 );
	updateShader.Set( "uTargets", 2 );
	updateShader.Set( "uNormals", 3 );
	updateShader.Set( "uNoiseTime", noiseTime );
	updateShader.Set( "uSurfaceFlow", SurfaceFlowValue( surfaceFlow ) );
	updateShader.Set( "uHover", HoverValue( hover ) );
	scatterPending = 0.0f;

	ScopedTextureUnits textures( { source.positions, source.velocities, targets, normals } );

	ScopedVAOBinding vaoBinding( emptyVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	glViewport( currentViewport.x, currentViewport.y, currentViewport.width, currentViewport.height );
	front = 1 - front;
}

void SwarmMorph::Render()
{
	float width    = static_cast< float >( std::max< GLuint >( currentViewport.width, 1 ) );
	float height   = static_cast< float >( std::max< GLuint >( currentViewport.height, 1 ) );
	float distance = DistanceValue( zoom );

	swarm::Mat4 projection = swarm::Mat4::Perspective( kFovY, width / height, 0.05f, 100.0f );
	swarm::Mat4 view       = swarm::Mat4::Translation( 0.0f, 0.0f, -distance ) *
						   swarm::Mat4::RotationX( AngleValue( rotateX ) * kPi / 180.0f ) *
						   swarm::Mat4::RotationY( AngleValue( rotateY ) * kPi / 180.0f + spinAngle );
	swarm::Mat4 mvp        = projection * view;

	// Point size is given in pixels at 1080p and at the default camera distance,
	// so the look stays the same across output resolutions.
	float pointScale = PointSizeValue( pointSize ) * ( height / 1080.0f ) * DistanceValue( 0.5f );

	glClear( GL_COLOR_BUFFER_BIT );

	ScopedShaderBinding shaderBinding( renderShader.GetGLID() );
	glUniformMatrix4fv( renderShader.FindUniform( "uMVP" ), 1, GL_FALSE, mvp.m );
	renderShader.Set( "uPointSize", pointScale );
	renderShader.Set( "uColor", colorR, colorG, colorB );
	renderShader.Set( "uHeat", heat );
	renderShader.Set( "uBrightness", BrightnessValue( brightness ) );

	renderShader.Set( "uPositions", 0 );
	renderShader.Set( "uVelocities", 1 );
	renderShader.Set( "uTextureWidth", kTextureWidth );

	ScopedTextureUnits textures( { state[ front ].positions, state[ front ].velocities } );

	ScopedVAOBinding vaoBinding( indexVAO );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	glDrawArrays( GL_POINTS, 0, ParticleCount() );
	// FFGL requires the context to be returned in its default state.
	glBlendFunc( GL_ONE, GL_ZERO );
	glDisable( GL_BLEND );
	glDisable( GL_PROGRAM_POINT_SIZE );
}

FFResult SwarmMorph::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PID_TARGET:
		targetSlot = std::min( std::max( static_cast< int >( std::lround( value ) ) - 1, 0 ), kNumSlots - 1 );
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
	case PID_ATTRACTION: attraction = value; break;
	case PID_DAMPING: damping = value; break;
	case PID_CHAOS: chaos = value; break;
	case PID_TURBULENCE: turbulence = value; break;
	case PID_SHIMMER: shimmer = value; break;
	case PID_NOISE_SCALE: noiseScale = value; break;
	case PID_SCATTER:
		if( value > 0.5f && !scatterPressed )
			scatterPending = kScatterImpulse;
		scatterPressed = value > 0.5f;
		break;
	case PID_POINT_SIZE: pointSize = value; break;
	case PID_COLOR_R: colorR = value; break;
	case PID_COLOR_G: colorG = value; break;
	case PID_COLOR_B: colorB = value; break;
	case PID_HEAT: heat = value; break;
	case PID_BRIGHTNESS: brightness = value; break;
	case PID_SPIN: spin = value; break;
	case PID_ROTATE_X: rotateX = value; break;
	case PID_ROTATE_Y: rotateY = value; break;
	case PID_ZOOM: zoom = value; break;
	case PID_WIND_X: windX = value; break;
	case PID_WIND_Y: windY = value; break;
	case PID_WIND_Z: windZ = value; break;
	case PID_GUSTS: gusts = value; break;
	case PID_VORTEX: vortex = value; break;
	case PID_RADIAL: radial = value; break;
	case PID_ANIM_SPEED: animSpeed = value; break;
	case PID_SURFACE_FLOW: surfaceFlow = value; break;
	case PID_HOVER: hover = value; break;
	case PID_FLOW_SPEED: flowSpeed = value; break;
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
	case PID_NORMALIZE: return normalizeMeshes ? 1.0f : 0.0f;
	case PID_PARTICLES: return particles;
	case PID_ATTRACTION: return attraction;
	case PID_DAMPING: return damping;
	case PID_CHAOS: return chaos;
	case PID_TURBULENCE: return turbulence;
	case PID_SHIMMER: return shimmer;
	case PID_NOISE_SCALE: return noiseScale;
	case PID_SCATTER: return scatterPressed ? 1.0f : 0.0f;
	case PID_POINT_SIZE: return pointSize;
	case PID_COLOR_R: return colorR;
	case PID_COLOR_G: return colorG;
	case PID_COLOR_B: return colorB;
	case PID_HEAT: return heat;
	case PID_BRIGHTNESS: return brightness;
	case PID_SPIN: return spin;
	case PID_ROTATE_X: return rotateX;
	case PID_ROTATE_Y: return rotateY;
	case PID_ZOOM: return zoom;
	case PID_WIND_X: return windX;
	case PID_WIND_Y: return windY;
	case PID_WIND_Z: return windZ;
	case PID_GUSTS: return gusts;
	case PID_VORTEX: return vortex;
	case PID_RADIAL: return radial;
	case PID_ANIM_SPEED: return animSpeed;
	case PID_SURFACE_FLOW: return surfaceFlow;
	case PID_HOVER: return hover;
	case PID_FLOW_SPEED: return flowSpeed;
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
	case PID_ATTRACTION: snprintf( buffer, sizeof( buffer ), "%.2f", AttractionValue( attraction ) ); return buffer;
	case PID_DAMPING: snprintf( buffer, sizeof( buffer ), "%.2f", DampingValue( damping ) ); return buffer;
	case PID_TURBULENCE: snprintf( buffer, sizeof( buffer ), "%.2f", TurbulenceValue( turbulence ) ); return buffer;
	case PID_NOISE_SCALE: snprintf( buffer, sizeof( buffer ), "%.2f", NoiseScaleValue( noiseScale ) ); return buffer;
	case PID_POINT_SIZE: snprintf( buffer, sizeof( buffer ), "%.1f px", PointSizeValue( pointSize ) ); return buffer;
	case PID_SPIN: snprintf( buffer, sizeof( buffer ), "%.2f rps", SpinValue( spin ) ); return buffer;
	case PID_ROTATE_X: snprintf( buffer, sizeof( buffer ), "%.0f deg", AngleValue( rotateX ) ); return buffer;
	case PID_ROTATE_Y: snprintf( buffer, sizeof( buffer ), "%.0f deg", AngleValue( rotateY ) ); return buffer;
	case PID_ZOOM: snprintf( buffer, sizeof( buffer ), "%.2f", DistanceValue( zoom ) ); return buffer;
	case PID_WIND_X: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windX ) ); return buffer;
	case PID_WIND_Y: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windY ) ); return buffer;
	case PID_WIND_Z: snprintf( buffer, sizeof( buffer ), "%+.1f", WindValue( windZ ) ); return buffer;
	case PID_VORTEX: snprintf( buffer, sizeof( buffer ), "%+.1f", VortexValue( vortex ) ); return buffer;
	case PID_RADIAL: snprintf( buffer, sizeof( buffer ), "%+.1f", RadialValue( radial ) ); return buffer;
	case PID_ANIM_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", AnimSpeedValue( animSpeed ) ); return buffer;
	case PID_FLOW_SPEED: snprintf( buffer, sizeof( buffer ), "%.2fx", FlowSpeedValue( flowSpeed ) ); return buffer;
	default: return CFFGLPlugin::GetParameterDisplay( index );
	}
}
