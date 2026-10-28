// SPDX-License-Identifier: GPL-3.0-or-later
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
// by a damped spring. A curl-noise flow field bends its path: strong and fully
// 3D while the particle travels; once it has arrived, the field is projected
// onto the surface so particles stream along the skin of the shape instead of
// wobbling in and out, plus a soft hover layer above and below it. On top of
// that shared flow, each particle can buzz around its spot on its own.
//
// Blow Away lets particles go: they drop the spring, ride the wind and its
// turbulence, and with Wrap Edges re-enter on the far side of the picture.
static const char kUpdateFS[] = R"(#version 410 core
uniform sampler2D uPositions; // xyz, w = per-particle random seed in [0, 1)
uniform sampler2D uVelocities;// xyz, w unused
uniform sampler2D uTargets;   // xyz = this particle's point on the target mesh
uniform sampler2D uNormals;   // xyz = surface normal there; zero for point clouds

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
uniform float uBlowAway;
uniform float uWrap;
uniform mat3 uViewRotation;   // world to camera; the camera looks down -z
uniform vec3 uViewTranslation;
uniform vec2 uFrustum;        // half extent of the view per unit of depth
uniform vec2 uDepthRange;     // near and far depth of the wrap volume
uniform float uNoiseTime;
uniform float uSurfaceFlow;
uniform float uHover;
uniform float uBuzz;
uniform float uBuzzTime;

layout( location = 0 ) out vec4 oPosition;
layout( location = 1 ) out vec4 oVelocity;

float hash( float n )
{
	return fract( sin( n ) * 43758.5453123 );
}

uvec3 pcg3d( uvec3 v )
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

vec3 gradientAt( vec3 cell )
{
	return vec3( pcg3d( uvec3( ivec3( cell ) ) ) ) * ( 2.0 / 4294967295.0 ) - 1.0;
}

// Gradient noise with its analytic derivative: x = value, yzw = gradient.
vec4 noised( vec3 x )
{
	vec3 i = floor( x );
	vec3 f = fract( x );
	vec3 u = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );
	vec3 du = 30.0 * f * f * ( f * ( f - 2.0 ) + 1.0 );

	vec3 ga = gradientAt( i + vec3( 0.0, 0.0, 0.0 ) );
	vec3 gb = gradientAt( i + vec3( 1.0, 0.0, 0.0 ) );
	vec3 gc = gradientAt( i + vec3( 0.0, 1.0, 0.0 ) );
	vec3 gd = gradientAt( i + vec3( 1.0, 1.0, 0.0 ) );
	vec3 ge = gradientAt( i + vec3( 0.0, 0.0, 1.0 ) );
	vec3 gf = gradientAt( i + vec3( 1.0, 0.0, 1.0 ) );
	vec3 gg = gradientAt( i + vec3( 0.0, 1.0, 1.0 ) );
	vec3 gh = gradientAt( i + vec3( 1.0, 1.0, 1.0 ) );

	float va = dot( ga, f - vec3( 0.0, 0.0, 0.0 ) );
	float vb = dot( gb, f - vec3( 1.0, 0.0, 0.0 ) );
	float vc = dot( gc, f - vec3( 0.0, 1.0, 0.0 ) );
	float vd = dot( gd, f - vec3( 1.0, 1.0, 0.0 ) );
	float ve = dot( ge, f - vec3( 0.0, 0.0, 1.0 ) );
	float vf = dot( gf, f - vec3( 1.0, 0.0, 1.0 ) );
	float vg = dot( gg, f - vec3( 0.0, 1.0, 1.0 ) );
	float vh = dot( gh, f - vec3( 1.0, 1.0, 1.0 ) );

	float value = va + u.x * ( vb - va ) + u.y * ( vc - va ) + u.z * ( ve - va ) +
				  u.x * u.y * ( va - vb - vc + vd ) + u.y * u.z * ( va - vc - ve + vg ) +
				  u.z * u.x * ( va - vb - ve + vf ) + ( -va + vb + vc - vd + ve - vf - vg + vh ) * u.x * u.y * u.z;
	vec3 gradient = ga + u.x * ( gb - ga ) + u.y * ( gc - ga ) + u.z * ( ge - ga ) +
					u.x * u.y * ( ga - gb - gc + gd ) + u.y * u.z * ( ga - gc - ge + gg ) +
					u.z * u.x * ( ga - gb - ge + gf ) + ( -ga + gb + gc - gd + ge - gf - gg + gh ) * u.x * u.y * u.z +
					du * ( vec3( vb, vc, ve ) - va +
						   u.yzx * vec3( va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf ) +
						   u.zxy * vec3( va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg ) +
						   u.yzx * u.zxy * ( -va + vb + vc - vd + ve - vf - vg + vh ) );
	return vec4( value, gradient );
}

// Gradient noise value only, for when the derivative isn't needed.
float gnoise( vec3 x )
{
	vec3 i = floor( x );
	vec3 f = fract( x );
	vec3 u = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );
	float va = dot( gradientAt( i + vec3( 0.0, 0.0, 0.0 ) ), f - vec3( 0.0, 0.0, 0.0 ) );
	float vb = dot( gradientAt( i + vec3( 1.0, 0.0, 0.0 ) ), f - vec3( 1.0, 0.0, 0.0 ) );
	float vc = dot( gradientAt( i + vec3( 0.0, 1.0, 0.0 ) ), f - vec3( 0.0, 1.0, 0.0 ) );
	float vd = dot( gradientAt( i + vec3( 1.0, 1.0, 0.0 ) ), f - vec3( 1.0, 1.0, 0.0 ) );
	float ve = dot( gradientAt( i + vec3( 0.0, 0.0, 1.0 ) ), f - vec3( 0.0, 0.0, 1.0 ) );
	float vf = dot( gradientAt( i + vec3( 1.0, 0.0, 1.0 ) ), f - vec3( 1.0, 0.0, 1.0 ) );
	float vg = dot( gradientAt( i + vec3( 0.0, 1.0, 1.0 ) ), f - vec3( 0.0, 1.0, 1.0 ) );
	float vh = dot( gradientAt( i + vec3( 1.0, 1.0, 1.0 ) ), f - vec3( 1.0, 1.0, 1.0 ) );
	return mix( mix( mix( va, vb, u.x ), mix( vc, vd, u.x ), u.y ), mix( mix( ve, vf, u.x ), mix( vg, vh, u.x ), u.y ), u.z );
}

// Each particle's own wandering offset, roughly in [-1, 1]: it walks its own
// lane far away from everyone else's in the noise field, at its own pace, so
// neighbours move independently instead of together.
vec3 buzz( float seed, float t )
{
	vec3 lane = vec3( hash( seed * 13.1 ), hash( seed * 57.3 ), hash( seed * 91.9 ) ) * 512.0;
	float pace = t * ( 0.6 + 0.8 * hash( seed * 3.7 ) );
	return vec3( gnoise( lane + vec3( pace, 0.0, 0.0 ) ),
				 gnoise( lane + vec3( 17.0, pace, 0.0 ) ),
				 gnoise( lane + vec3( 0.0, 41.0, pace ) ) ) * 2.0;
}

// Curl of three noise potentials that drift in different directions, so the
// flow keeps changing shape instead of repeating. Being a curl, it has zero
// divergence: particles stream past each other instead of clumping.
vec3 curlNoise( vec3 p, float t, out float potential )
{
	vec4 a = noised( p + vec3( 0.31, -0.17, 0.23 ) * t );
	vec4 b = noised( p + vec3( 31.4, 17.9, -9.2 ) + vec3( -0.19, 0.27, 0.13 ) * t );
	vec4 c = noised( p + vec3( -23.1, 45.6, 12.7 ) + vec3( 0.11, 0.21, -0.29 ) * t );
	potential = a.x;
	return vec3( c.z - b.w, a.w - c.y, b.y - a.z ) * 0.5;
}

void main()
{
	ivec2 texel = ivec2( gl_FragCoord.xy );
	vec4 state = texelFetch( uPositions, texel, 0 );
	float seed = state.w;
	vec3 position = state.xyz;
	vec3 velocity = texelFetch( uVelocities, texel, 0 ).xyz;
	vec3 target = texelFetch( uTargets, texel, 0 ).xyz;

	// Chaos gives each particle its own spring strength, so the swarm arrives in waves.
	float strength = uAttraction * mix( 1.0, 0.5 + hash( seed * 91.7 ), uChaos );
	float damping = 2.0 * sqrt( strength ) * uDampingRatio;

	// Blow Away: a particle lets go once the slider passes its ticket; the
	// short ramp makes the release smooth and the slider reversible.
	float hold = 1.0;
	if( uBlowAway > 0.0 )
	{
		float edge = uBlowAway * 1.06 - 0.03;
		hold = smoothstep( edge - 0.03, edge + 0.03, hash( seed * 61.3 ) );
	}

	// Wind arrives in world space but is set relative to the screen. Gusts are
	// patches of stronger and weaker air drifting downstream with it.
	vec3 wind = uWind;
	if( uGusts > 0.0 && dot( uWind, uWind ) > 0.0 )
	{
		float gust = gnoise( position * 0.7 - uWind * ( uTime * 0.12 ) + vec3( 0.0, 0.0, uTime * 0.4 ) );
		wind *= max( 1.0 + uGusts * gust * 2.0, 0.0 );
	}
	// Held particles lean with the wind: their spot moves downstream by the
	// offset the force would settle into against the spring. Moving the spot
	// rather than pushing the particle keeps them counted as arrived, so the
	// in-flight turbulence stays out of it and the shape stays readable. A
	// small spread per particle stretches it a little along the wind.
	// The shared attraction, not this particle's own, so Chaos doesn't tear it.
	float lean = 0.4 * ( 0.9 + 0.2 * hash( seed * 23.1 ) );
	target += wind * ( lean / uAttraction );

	vec3 toTarget = target - position;
	float distance = length( toTarget );

	// All swirl terms are offsets from the target that the spring settles into,
	// so they shape the swarm without ever overpowering the spring.
	float potential;
	vec3 field = curlNoise( position * uNoiseScale, uNoiseTime, potential );
	vec3 normal = texelFetch( uNormals, texel, 0 ).xyz;

	// In flight: full 3D swirl, relative to the remaining distance.
	vec3 swirl = field * ( uShimmer + uTurbulence * min( distance, 1.5 ) );
	// Near the surface: stream along it, and hover in a loose layer around it.
	float nearSurface = 1.0 - smoothstep( 0.5, 1.5, distance );
	vec3 alongSurface = field - normal * dot( field, normal );
	float layer = potential * 2.0 + hash( seed * 7.7 ) - 0.5;
	swirl += ( alongSurface * uSurfaceFlow + normal * layer * uHover ) * nearSurface;
	// Individual buzz: mostly along the surface, a little in and out of it.
	if( uBuzz > 0.0 )
	{
		vec3 wander = buzz( seed, uBuzzTime );
		float depth = dot( wander, normal );
		swirl += ( wander - normal * depth * 0.65 ) * uBuzz * nearSurface;
	}

	vec3 acceleration = strength * ( toTarget + swirl ) * hold;

	// Vortex and Explode push; against the spring they bend and inflate the
	// shape, each particle a little differently.
	float response = 0.5 + hash( seed * 23.1 );
	vec3 externalForce = uVortex * vec3( -position.z, 0.0, position.x );
	float radius = length( position );
	if( radius > 1e-4 )
		externalForce += uRadial * position / radius;
	acceleration += externalForce * response;

	// Released particles: drag pulls them to the speed of the air, which is
	// the wind plus the curl flow, livelier the stronger the wind blows.
	if( hold < 1.0 )
	{
		vec3 air = wind * response + field * ( 0.9 + 0.6 * length( wind ) );
		acceleration += ( air - velocity ) * ( 1.5 * response * ( 1.0 - hold ) );
	}

	if( uScatter > 0.0 )
	{
		vec3 direction = vec3( hash( seed * 12.9 + uTime ), hash( seed * 78.2 + uTime ), hash( seed * 37.7 + uTime ) ) * 2.0 - 1.0;
		velocity += normalize( direction + 1e-4 ) * uScatter * ( 0.3 + hash( seed * 5.3 ) );
	}

	velocity += acceleration * uDeltaTime;
	velocity *= exp( -damping * hold * uDeltaTime );
	position += velocity * uDeltaTime;

	// Wrap Edges: a released particle that leaves the picture comes back in on
	// the opposite side, at the same depth (or at the same spot on screen when
	// it leaves through the front or back of the volume).
	if( uWrap > 0.5 && hold < 0.5 )
	{
		vec3 view = uViewRotation * position + uViewTranslation;
		float depth = -view.z;
		float span = uDepthRange.y - uDepthRange.x;
		float wrappedDepth = depth < uDepthRange.x ? depth + span : ( depth > uDepthRange.y ? depth - span : depth );
		vec2 xy = view.xy * ( wrappedDepth / max( depth, 1e-3 ) );
		// A small margin so points have fully left the picture before they jump.
		vec2 halfExtent = uFrustum * wrappedDepth * 1.08;
		bool outside = any( greaterThan( abs( xy ), halfExtent ) );
		if( outside || wrappedDepth != depth )
		{
			if( outside )
				xy = mod( xy + halfExtent, 2.0 * halfExtent ) - halfExtent;
			position = transpose( uViewRotation ) * ( vec3( xy, -wrappedDepth ) - uViewTranslation );
		}
	}

	oPosition = vec4( position, seed );
	oVelocity = vec4( velocity, 0.0 );
}
)";

// Animated meshes: evaluates each particle's surface binding (three vertices
// and weights) against the current frame's vertex positions.
static const char kSkinFS[] = R"(#version 410 core
uniform sampler2D uVertices;    // xyz per vertex, uVertexTextureWidth per row
uniform usampler2D uBindIndices;// xyz = vertex indices
uniform sampler2D uBindWeights; // xyz = barycentric weights
uniform int uVertexTextureWidth;

layout( location = 0 ) out vec4 oTarget;
layout( location = 1 ) out vec4 oNormal;

vec3 vertexAt( uint index )
{
	int i = int( index );
	return texelFetch( uVertices, ivec2( i % uVertexTextureWidth, i / uVertexTextureWidth ), 0 ).xyz;
}

void main()
{
	ivec2 texel = ivec2( gl_FragCoord.xy );
	uvec4 indices = texelFetch( uBindIndices, texel, 0 );
	vec4 weights = texelFetch( uBindWeights, texel, 0 );
	vec3 a = vertexAt( indices.x );
	vec3 b = vertexAt( indices.y );
	vec3 c = vertexAt( indices.z );
	oTarget = vec4( a * weights.x + b * weights.y + c * weights.z, 1.0 );

	// Face normal of the bound triangle; zero for point clouds (all three indices equal).
	vec3 n = cross( b - a, c - a );
	float lengthSq = dot( n, n );
	oNormal = vec4( lengthSq > 1e-24 ? n * inversesqrt( lengthSq ) : vec3( 0.0 ), 0.0 );
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
