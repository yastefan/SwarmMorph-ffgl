#pragma once

// All shaders target GLSL 4.10 core, the highest version macOS offers, so the
// same code runs in Resolume on macOS and Windows.
//
// Particle state lives in float textures, one texel per particle: particle i
// sits at texel (i % width, i / width). Transform feedback would be the
// textbook alternative, but Apple's GL-on-Metal layer runs it about 15x slower
// than a fragment pass, so the simulation is a full-screen fragment pass.

namespace swarm
{
// Full-screen triangle generated from gl_VertexID; needs no vertex buffers.
static const char kFullscreenVS[] = R"(#version 410 core
void main()
{
	vec2 corner = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );
	gl_Position = vec4( corner * 2.0 - 1.0, 0.0, 1.0 );
}
)";

// Each particle is pulled towards its sample point on the current target mesh
// by a damped spring. A divergence-free flow field bends its path: strong while
// the particle is far away, fading to a gentle shimmer once it has arrived, so
// the shape always settles crisply.
static const char kUpdateFS[] = R"(#version 410 core
uniform sampler2D uPositions; // xyz, w = per-particle random seed in [0, 1)
uniform sampler2D uVelocities;// xyz, w unused
uniform sampler2D uTargets;   // xyz = this particle's point on the target mesh

uniform float uDeltaTime;
uniform float uTime;
uniform float uAttraction;
uniform float uDampingRatio;
uniform float uChaos;
uniform float uTurbulence;
uniform float uShimmer;
uniform float uNoiseScale;
uniform float uScatter;
uniform vec3 uWind;
uniform float uGusts;
uniform float uVortex;
uniform float uRadial;

layout( location = 0 ) out vec4 oPosition;
layout( location = 1 ) out vec4 oVelocity;

float hash( float n )
{
	return fract( sin( n ) * 43758.5453123 );
}

// Every component ignores its own axis, so the field has zero divergence:
// particles swirl instead of bunching up or thinning out.
vec3 flow( vec3 p, float t )
{
	vec3 a = vec3( sin( p.y + t ), sin( p.z + t * 1.13 ), sin( p.x + t * 0.87 ) );
	vec3 q = p * 2.17 + 1.7;
	vec3 b = vec3( cos( q.z - t * 1.31 ), cos( q.x + t * 0.73 ), cos( q.y - t * 1.07 ) );
	return ( a + 0.5 * b ) * 0.67;
}

void main()
{
	ivec2 texel = ivec2( gl_FragCoord.xy );
	vec4 state = texelFetch( uPositions, texel, 0 );
	float seed = state.w;
	vec3 position = state.xyz;
	vec3 velocity = texelFetch( uVelocities, texel, 0 ).xyz;
	vec3 target = texelFetch( uTargets, texel, 0 ).xyz;

	vec3 toTarget = target - position;
	float distance = length( toTarget );

	// Chaos gives each particle its own spring strength, so the swarm arrives in waves.
	float strength = uAttraction * mix( 1.0, 0.5 + hash( seed * 91.7 ), uChaos );
	float damping = 2.0 * sqrt( strength ) * uDampingRatio;

	// The swirl is relative to the remaining distance, so it bends the path without
	// ever overpowering the spring near the target.
	float swirlAmount = uShimmer + uTurbulence * min( distance, 1.5 );
	vec3 swirl = flow( position * uNoiseScale + seed * 0.5, uTime ) * swirlAmount;
	vec3 acceleration = strength * ( toTarget + swirl );

	// External forces. Against the spring they settle into an offset of
	// force / strength, so they smear and stretch the shape instead of blowing
	// it away. Each particle reacts differently, which turns wind into streaks.
	float response = 0.5 + hash( seed * 23.1 );
	float windSpeed = length( uWind );
	vec3 externalForce = uWind;
	if( windSpeed > 0.0 )
	{
		// Gusts travel through the shape along the wind direction.
		float wave = 0.5 + 0.5 * sin( dot( position, uWind / windSpeed ) * 2.0 - uTime * 3.0 + seed * 2.0 );
		externalForce *= mix( 1.0, 2.0 * wave, uGusts );
	}
	externalForce += uVortex * vec3( -position.z, 0.0, position.x );
	float radius = length( position );
	if( radius > 1e-4 )
		externalForce += uRadial * position / radius;
	acceleration += externalForce * response;

	if( uScatter > 0.0 )
	{
		vec3 direction = vec3( hash( seed * 12.9 + uTime ), hash( seed * 78.2 + uTime ), hash( seed * 37.7 + uTime ) ) * 2.0 - 1.0;
		velocity += normalize( direction + 1e-4 ) * uScatter * ( 0.3 + hash( seed * 5.3 ) );
	}

	velocity += acceleration * uDeltaTime;
	velocity *= exp( -damping * uDeltaTime );
	position += velocity * uDeltaTime;

	oPosition = vec4( position, seed );
	oVelocity = vec4( velocity, 0.0 );
}
)";

// One point per particle, positioned from the state texture. The particle index
// comes from a static vertex buffer rather than gl_VertexID: Apple's driver
// draws attribute-less points about 4x slower.
static const char kRenderVS[] = R"(#version 410 core
layout( location = 0 ) in float aIndex;

uniform sampler2D uPositions;
uniform sampler2D uVelocities;
uniform int uTextureWidth;
uniform mat4 uMVP;
uniform float uPointSize;// in pixels at the reference distance
uniform vec3 uColor;
uniform float uHeat;
uniform float uBrightness;

out vec4 vColor;

void main()
{
	int index = int( aIndex );
	ivec2 texel = ivec2( index % uTextureWidth, index / uTextureWidth );
	vec3 position = texelFetch( uPositions, texel, 0 ).xyz;
	vec3 velocity = texelFetch( uVelocities, texel, 0 ).xyz;

	vec4 clip = uMVP * vec4( position, 1.0 );
	gl_Position = clip;

	float size = uPointSize / max( clip.w, 0.05 );
	gl_PointSize = max( size, 1.0 );

	// Fast particles glow towards white.
	float speed = length( velocity );
	vec3 color = mix( uColor, vec3( 1.0 ), clamp( speed * 0.3, 0.0, 1.0 ) * uHeat );

	// Points smaller than a pixel are drawn at one pixel but dimmer, which avoids flicker.
	float coverage = clamp( size * size, 0.0, 1.0 );
	vColor = vec4( color, uBrightness * coverage );
}
)";

// Soft round points, premultiplied, for additive blending.
static const char kRenderFS[] = R"(#version 410 core
in vec4 vColor;
out vec4 fragColor;

void main()
{
	vec2 c = gl_PointCoord * 2.0 - 1.0;
	float r2 = dot( c, c );
	if( r2 > 1.0 )
		discard;
	float falloff = 1.0 - r2;
	float alpha = vColor.a * falloff * falloff;
	fragColor = vec4( vColor.rgb * alpha, alpha );
}
)";
}// namespace swarm
