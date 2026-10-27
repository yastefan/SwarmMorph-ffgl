#pragma once
#include <FFGLSDK.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Resolume source that turns OBJ meshes into a particle swarm. Switching the
// target mesh makes every particle fly to its point on the new surface.
class SwarmMorph : public CFFGLPlugin
{
public:
	static constexpr int kNumSlots       = 8;
	static constexpr int kTextureWidth   = 1024;
	static constexpr int kMaxParticles   = kTextureWidth * kTextureWidth;
	static constexpr int kMinParticles   = 10000;

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
		std::vector< float > points;
		std::string error;
		std::atomic< bool > done{ false };
	};

	struct Slot
	{
		std::string path;
		std::shared_ptr< LoadJob > pending;
		GLuint texture = 0;// RGB32F target points, one texel per particle
		bool loaded    = false;
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
	void SelectNextLoadedSlot();
	GLuint CurrentTargetTexture();
	int ParticleCount() const;
	void Simulate( float deltaTime, GLuint targetTexture );
	void Render();

	Slot slots[ kNumSlots ];
	GLuint fallbackTexture   = 0;
	GLuint lastTargetTexture = 0;
	StateBuffer state[ 2 ];// ping-ponged
	int front                = 0;// index of the state holding the latest frame
	GLuint emptyVAO          = 0;// core profile needs a bound VAO even without attributes
	GLuint indexVBO          = 0;// particle index per vertex, for the render pass
	GLuint indexVAO          = 0;
	ffglex::FFGLShader updateShader;
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
	bool normalizeMeshes = true;
	bool nextPressed     = false;
	bool scatterPressed  = false;
	float scatterPending = 0.0f;

	double lastTime  = -1.0;
	float simTime    = 0.0f;
	float spinAngle  = 0.0f;
};
