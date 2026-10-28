// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <FFGLSDK.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "MeshSampler.h"

// Resolume source that turns OBJ meshes into a particle swarm. Switching the
// target mesh makes every particle fly to its point on the new surface.
class SwarmMorph : public CFFGLPlugin
{
public:
	static constexpr int kNumSlots       = 8;
	static constexpr int kTextureWidth   = 1024;
	static constexpr int kMaxParticles   = kTextureWidth * kTextureWidth;
	static constexpr int kMinParticles   = 10000;
	static constexpr int kVertexTextureWidth = 4096;// animated meshes: vertex positions per texture row

	SwarmMorph();
	~SwarmMorph() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

private:
	// A mesh being parsed and sampled on a worker thread. The thread holds its
	// own reference, so replacing a pending job never blocks the host.
	struct LoadJob
	{
		std::string path;
		std::vector< float > points;              // static mesh: target xyz per particle
		std::vector< float > normals;             // static mesh: surface normal per particle
		swarm::SurfaceBinding binding;            // animated mesh: where each particle sits
		std::vector< std::vector< float > > frames;// animated mesh: vertex positions per frame
		float fps = 24.0f;                         // animated mesh: playback rate at 1x
		std::string error;
		std::string warning;
		std::atomic< bool > done{ false };
	};

	struct Slot
	{
		std::string path;
		std::shared_ptr< LoadJob > pending;
		// One texel per particle: RGB32F for a static mesh, RGBA32F rendered
		// from the current animation frame by the skin pass.
		GLuint texture = 0;
		GLuint normals = 0;// surface normal per particle, same layout as texture
		bool loaded    = false;

		// Animation (empty for static meshes).
		std::vector< std::vector< float > > frames;
		std::vector< float > blended;// current frame, padded to whole texture rows
		GLuint vertexTexture = 0;
		GLuint bindIndices   = 0;
		GLuint bindWeights   = 0;
		GLuint fbo           = 0;
		int vertexRows       = 0;
		double playhead      = 0.0;// in frames
		float fps            = 24.0f;
		double evaluatedAt   = -1.0;
		int evaluatedRows    = 0;

		bool IsAnimated() const { return frames.size() > 1; }
	};

	// Position and velocity textures plus the framebuffer that writes both.
	struct StateBuffer
	{
		GLuint positions  = 0;
		GLuint velocities = 0;
		GLuint fbo        = 0;
	};

	bool CreateGLResources();
	void ReleaseGLResources();
	void StartLoad( int slot, const std::string& path );
	void UploadFinishedLoads();
	void UploadAnimation( Slot& slot, LoadJob& job );
	void ReleaseSlotGL( Slot& slot );
	void SelectNextLoadedSlot();
	void CurrentTarget( GLuint& targets, GLuint& normals );
	void AdvanceAnimation( float deltaTime );
	double FramePosition( const Slot& slot ) const;
	int ParticleCount() const;
	void Simulate( float deltaTime, GLuint targets, GLuint normals );
	void Render();

	Slot slots[ kNumSlots ];
	GLuint fallbackTexture   = 0;
	int activeSlot           = -1;// slot the swarm is heading to; -1 = fallback sphere
	StateBuffer state[ 2 ];// ping-ponged
	int front                = 0;// index of the state holding the latest frame
	GLuint emptyVAO          = 0;// core profile needs a bound VAO even without attributes
	GLuint indexVBO          = 0;// particle index per vertex, for the render pass
	GLuint indexVAO          = 0;
	ffglex::FFGLShader updateShader;
	ffglex::FFGLShader skinShader;
	ffglex::FFGLShader renderShader;

	// Parameters, stored as the host's normalised values.
	int targetSlot       = 0;
	float particles      = 0.5f;
	float attraction     = 0.35f;
	float damping        = 0.4f;
	float chaos          = 0.5f;
	float turbulence     = 0.5f;
	float shimmer        = 0.3f;
	float noiseScale     = 0.3f;
	float pointSize      = 0.12f;
	float colorR         = 0.55f;
	float colorG         = 0.8f;
	float colorB         = 1.0f;
	float heat           = 0.5f;
	float brightness     = 0.2f;
	float spin           = 0.55f;
	float rotateX        = 0.5f;
	float rotateY        = 0.5f;
	float zoom           = 0.5f;
	float windX          = 0.5f;
	float windY          = 0.5f;
	float windZ          = 0.5f;
	float gusts          = 0.3f;
	float vortex         = 0.5f;
	float radial         = 0.5f;
	float animSpeed      = 1.0f / 3.0f;// 1x
	bool pingPong        = false;
	bool restartOnSwitch = true;
	bool detectSequences = true;
	float surfaceFlow    = 0.25f;
	float hover          = 0.15f;
	float flowSpeed      = 0.25f;
	float buzzAmount     = 0.3f;
	float buzzSpeed      = 0.3f;
	bool normalizeMeshes = true;
	bool nextPressed     = false;
	bool scatterPressed  = false;
	float scatterPending = 0.0f;

	double lastTime  = -1.0;
	float simTime    = 0.0f;
	float noiseTime  = 0.0f;// advances with Flow Speed
	float buzzTime   = 0.0f;// advances with Buzz Speed
	float spinAngle  = 0.0f;
};
