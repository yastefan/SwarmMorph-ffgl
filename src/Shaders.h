// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

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

// Trails: the new frame of particles over the last trail image, faded and
// optionally zoomed. The brighter of the two wins, as with Resolume's Trails,
// so a shape that holds still doesn't burn out to white. Also copies the
// result to the host with uFade = 0.
static const char kTrailFS[] = R"(#version 410 core
uniform sampler2D uCurrent;
uniform sampler2D uPrevious;
uniform vec2 uOrigin;// the viewport's corner and size, in pixels
uniform vec2 uSize;
uniform float uFade; // what is left of the last image; 0 = none
uniform float uZoom; // how much bigger the last image is drawn
out vec4 fragColor;
void main()
{
	vec2 uv = ( gl_FragCoord.xy - uOrigin ) / uSize;
	vec4 now = texture( uCurrent, uv );
	vec2 from = 0.5 + ( uv - 0.5 ) / uZoom;
	vec4 before = vec4( 0.0 );
	if( uFade > 0.0 && all( greaterThanEqual( from, vec2( 0.0 ) ) ) && all( lessThanEqual( from, vec2( 1.0 ) ) ) )
		// The small step down clears the last faint haze, which would
		// otherwise linger for a long time.
		before = max( texture( uPrevious, from ) * uFade - 0.002, 0.0 );
	fragColor = max( now, before );
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
uniform sampler2D uMorphTargets;// the same for Mesh 2
uniform sampler2D uMorphNormals;
uniform float uMorph;         // 0 = Mesh 1, 1 = Mesh 2
uniform float uSplit;         // 0 = all glide together, 1 = each switches at its own point
uniform vec3 uAnchor;         // Mesh 1's point that sits at the centre
uniform vec3 uMorphAnchor;    // the same for Mesh 2
uniform float uScale;         // Mesh 1's size around its anchor
uniform float uMorphScale;
uniform float uBeatKick;      // 1 only on the frame of a beat
uniform float uSpawn;         // 1 only on the frame the swarm respawns
uniform int uSpawnFrom;       // 0 scatter, 1 centre, 2 far
uniform float uSpawnSeed;     // new random start per spawn
uniform float uBeatSeed;      // new random pick per beat
uniform float uBurst;         // share of particles

uniform float uDeltaTime;
uniform float uTime;
uniform float uAttraction;
uniform float uDampingRatio;
uniform float uChaos;
uniform float uTurbulence;
uniform float uNoiseScale;
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

// PCG hash to [0, 1).
float pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return float( ( word >> 22u ) ^ word ) / 4294967296.0;
}

// How far this particle has gone over to Mesh 2. Without Split all glide
// together with the fader; with Split each one switches at its own point of
// the fader, so halfway half the swarm sits on each mesh. Same in the render
// shader, so the light knows which mesh a particle belongs to.
float MorphWeight( float seed )
{
	float threshold = 0.02 + 0.96 * hash( seed * 41.3 );
	float switched = smoothstep( threshold - 0.02, threshold + 0.02, uMorph );
	return mix( uMorph, switched, uSplit );
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
	// Anchor: each mesh is moved so its anchor point sits at the centre, and
	// scaled around it.
	vec3 target = ( texelFetch( uTargets, texel, 0 ).xyz - uAnchor ) * uScale;
	vec3 normal = texelFetch( uNormals, texel, 0 ).xyz;
	// Morph fader: the particle's goal slides from its spot on one mesh to its
	// spot on the other, so the swarm follows the fader with its usual physics.
	if( uMorph > 0.0 )
	{
		float weight = MorphWeight( seed );
		target = mix( target, ( texelFetch( uMorphTargets, texel, 0 ).xyz - uMorphAnchor ) * uMorphScale, weight );
		vec3 blended = mix( normal, texelFetch( uMorphNormals, texel, 0 ).xyz, weight );
		float lengthSq = dot( blended, blended );
		normal = lengthSq > 1e-8 ? blended * inversesqrt( lengthSq ) : vec3( 0.0 );
	}

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

	// Burst: on the beat, a different share of the particles flies off. (The
	// other beat effects are drawn by the render pass.)
	if( uBeatKick > 0.0 && hash( seed * 53.1 + uBeatSeed * 17.3 ) < uBurst )
	{
		vec3 outward = dot( normal, normal ) > 0.0 ? normal : target / max( length( target ), 1e-4 );
		vec3 random = vec3( hash( seed * 12.9 + uBeatSeed * 91.0 ), hash( seed * 78.2 + uBeatSeed * 47.0 ),
							hash( seed * 37.7 + uBeatSeed * 13.0 ) ) * 2.0 - 1.0;
		velocity += normalize( outward * 1.5 + random ) * ( 2.0 + 2.0 * hash( seed * 5.3 ) );
	}

	vec3 toTarget = target - position;
	float distance = length( toTarget );

	// All swirl terms are offsets from the target that the spring settles into,
	// so they shape the swarm without ever overpowering the spring.
	float potential;
	vec3 field = curlNoise( position * uNoiseScale, uNoiseTime, potential );

	// In flight: full 3D swirl, relative to the remaining distance.
	vec3 swirl = field * ( uTurbulence * min( distance, 1.5 ) );
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

	// Spawn: a fresh start, from where the springs pull the swarm back in.
	if( uSpawn > 0.0 )
	{
		// An integer hash of the particle's texel: the sine hash lines up in
		// stripes when it fills the whole picture.
		uint state = uint( texel.y ) * 4096u + uint( texel.x ) + uint( uSpawnSeed * 1048576.0 ) * 2654435761u;
		vec3 random = vec3( pcg( state ), pcg( state + 0x9E3779B9u ), pcg( state + 0x3C6EF372u ) );
		if( uSpawnFrom == 1 )
		{
			// Center: a burst out of the middle.
			vec3 direction = normalize( random * 2.0 - 1.0 + vec3( 1e-4 ) );
			position = direction * 0.05;
			velocity = direction * ( 3.0 + 4.0 * hash( seed * 5.9 + uSpawnSeed ) );
		}
		else
		{
			// Scatter: all over the picture. Far: a wide cloud far behind the
			// shape, flying in towards the camera.
			float depth = uSpawnFrom == 0 ? mix( uDepthRange.x, uDepthRange.y, random.z ) : uDepthRange.y + 3.0 + 6.0 * random.z;
			vec2 spread = uSpawnFrom == 0 ? vec2( 1.0 ) : vec2( 1.6 );
			vec2 xy = ( random.xy * 2.0 - 1.0 ) * uFrustum * depth * spread;
			position = transpose( uViewRotation ) * ( vec3( xy, -depth ) - uViewTranslation );
			velocity = vec3( 0.0 );
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
uniform sampler2D uVertexNormals;// smooth normal per vertex, same layout
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

vec3 vertexNormalAt( uint index )
{
	int i = int( index );
	return texelFetch( uVertexNormals, ivec2( i % uVertexTextureWidth, i / uVertexTextureWidth ), 0 ).xyz;
}

// A corner's vertex normal, or the face normal at a sharp edge (as on the CPU).
vec3 cornerNormal( uint index, vec3 face )
{
	vec3 vn = vertexNormalAt( index );
	return dot( vn, face ) > 0.7 ? vn : face;
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

	// Smooth normal, blended from the corners; zero for point clouds (all
	// three indices equal).
	vec3 n = cross( b - a, c - a );
	float lengthSq = dot( n, n );
	if( lengthSq > 1e-24 )
	{
		vec3 face = n * inversesqrt( lengthSq );
		vec3 blended = cornerNormal( indices.x, face ) * weights.x + cornerNormal( indices.y, face ) * weights.y +
					   cornerNormal( indices.z, face ) * weights.z;
		oNormal = vec4( dot( blended, blended ) > 1e-12 ? normalize( blended ) : face, 0.0 );
	}
	else
		oNormal = vec4( 0.0 );
}
)";

// One point per particle, positioned from the state texture. The particle index
// comes from a static vertex buffer rather than gl_VertexID: Apple's driver
// draws attribute-less points about 4x slower. Colour is worked out here, per
// particle, so the extra look options cost next to nothing.
//
// Compiled twice: as round points, and with STREAKS defined as capsules that
// reach back to where the particle was a frame ago. Streaks close the gaps a
// fast particle leaves between frames, so frame-blending effects such as
// Resolume's Trails draw continuous curves instead of dotted lines. A streak
// is a quad per particle (instanced, six corners), shaped into a capsule by
// the fragment shader with the same falloff as a point.
static const char kRenderVSBody[] = R"(
layout( location = 0 ) in float aIndex;

uniform sampler2D uPositions;
uniform sampler2D uVelocities;
uniform sampler2D uNormals;// surface normal of each particle's spot; zero for point clouds
uniform int uTextureWidth;
uniform mat4 uMVP;
uniform float uPointSize;// in pixels at the reference distance
uniform float uRefDistance;
uniform vec3 uColor;
uniform vec3 uColor2;
uniform int uColorMode;  // 0 speed, 1 height, 2 depth, 3 random
uniform float uColorMix; // how much of Color 2
uniform float uHeat;
uniform float uBrightness;
uniform float uTwinkle;
uniform float uDepthFade;
uniform float uDistance; // camera distance to the centre of the shape
// Light, in the shape's own space.
// Both meshes, to tell surface from free flight. uNormals is Mesh 1's.
uniform sampler2D uTargets;
uniform sampler2D uMorphTargets;
uniform sampler2D uMorphNormals;
uniform float uMorph;     // 0 = Mesh 1, 1 = Mesh 2
uniform float uSplit;
uniform vec3 uAnchor;     // each mesh's point that sits at the centre
uniform vec3 uMorphAnchor;
uniform float uScale;
uniform float uMorphScale;
uniform float uSelfGlow;// the particles' own light, in their colour
uniform float uKeyLight;
uniform vec3 uKeyColor;
uniform vec3 uLightDirection;// towards the key light
uniform float uRim;
uniform vec3 uRimColor;
uniform vec3 uEyePosition;
uniform float uCenterLight;
uniform vec3 uCenterColor;
uniform float uTime;
// Beat effects, drawn as an offset from the simulated position so they land
// exactly on the beat and fade with Decay, without waiting for the spring.
uniform float uBeat;     // 1 on the beat, decaying
uniform float uBeatSeed; // new random pick per beat
uniform float uPulse;
uniform float uShockwave;
uniform float uWave;     // shockwave progress since the beat: 0 to 1
uniform float uJitter;

#ifdef STREAKS
uniform sampler2D uPreviousPositions;// the state a frame ago
uniform float uStreakLength;         // 1 = back to where the particle was a frame ago
uniform mat4 uPreviousMVP;           // the camera a frame ago, so Spin and camera moves draw streaks too
uniform vec2 uViewport;              // in pixels
noperspective out vec2 vLocal;       // pixels: along the streak from its tail, and across it
flat out float vLength;
flat out float vRadius;
#endif

out vec4 vColor;

float hash( float n )
{
	return fract( sin( n ) * 43758.5453123 );
}

// As in the update shader: how far this particle has gone over to Mesh 2.
float MorphWeight( float seed )
{
	float threshold = 0.02 + 0.96 * hash( seed * 41.3 );
	float switched = smoothstep( threshold - 0.02, threshold + 0.02, uMorph );
	return mix( uMorph, switched, uSplit );
}

// A particle's spot on a mesh, moved by the mesh's anchor and scaled, and the surface
// normal there; point clouds point away from the mesh's centre.
vec3 MeshSpot( sampler2D targets, sampler2D normals, vec3 anchor, float scale, ivec2 texel, out vec3 normal )
{
	vec3 spot = texelFetch( targets, texel, 0 ).xyz;
	normal = texelFetch( normals, texel, 0 ).xyz;
	if( dot( normal, normal ) == 0.0 )
		normal = spot / max( length( spot ), 1e-4 );
	return ( spot - anchor ) * scale;
}

// Moves a simulated position by the beat effects. Head and tail of a streak
// get the same offset, so the effects themselves don't smear.
vec3 BeatOffset( vec3 position, ivec2 texel, float seed, out float waveGlow )
{
	if( uBeat > 0.001 )
	{
		vec3 normal = mix( texelFetch( uNormals, texel, 0 ).xyz, texelFetch( uMorphNormals, texel, 0 ).xyz, MorphWeight( seed ) );
		vec3 outward = dot( normal, normal ) > 1e-8 ? normalize( normal ) : position / max( length( position ), 1e-4 );
		// Pulse: the whole shape swells along its surface.
		vec3 offset = outward * ( uPulse * uBeat );
		// Jitter: every particle twitches to its own side.
		vec3 random = vec3( hash( seed * 12.9 + uBeatSeed * 91.0 ), hash( seed * 78.2 + uBeatSeed * 47.0 ),
							hash( seed * 37.7 + uBeatSeed * 13.0 ) ) * 2.0 - 1.0;
		position += offset + random * ( uJitter * uBeat );
	}

	// Shockwave: a pressure ring runs out from the centre, timed to reach the
	// edge just before the next beat. It slows down as it travels, like a real
	// wave, and grows wider and fainter; the ring itself glows a little.
	waveGlow = 0.0;
	if( uShockwave > 0.0 && uWave < 1.0 )
	{
		float radius = length( position );
		float remaining = 1.0 - uWave;
		float front = 1.4 * ( 1.0 - remaining * remaining );
		float x = ( radius - front ) / ( 0.15 + 0.3 * uWave );
		waveGlow = exp( -x * x ) * pow( remaining, 1.5 ) * smoothstep( 0.0, 0.08, uWave );
		position += position / max( radius, 1e-4 ) * ( uShockwave * waveGlow );
	}
	return position;
}

#ifdef STREAKS
vec2 ToPixels( vec4 clip )
{
	return ( clip.xy / clip.w * 0.5 + 0.5 ) * uViewport;
}
#endif

void main()
{
	int index = int( aIndex );
	ivec2 texel = ivec2( index % uTextureWidth, index / uTextureWidth );
	vec4 state = texelFetch( uPositions, texel, 0 );
	float seed = state.w;
	vec3 velocity = texelFetch( uVelocities, texel, 0 ).xyz;

	float waveGlow;
	vec3 position = BeatOffset( state.xyz, texel, seed, waveGlow );
	vec4 clip = uMVP * vec4( position, 1.0 );

	// Points grow more slowly than true perspective as the camera comes
	// closer, so close-ups show finer grain instead of a mush of big blobs.
	// Capped, so a camera flying through the swarm isn't blinded by points
	// right in front of the lens.
	float closeness = uRefDistance / max( clip.w, 0.05 );
	float size = uPointSize * pow( closeness, 0.6 );
	float drawn = clamp( size, 1.0, 48.0 );

#ifdef STREAKS
	// The tail: where the particle was a frame ago. A jump (Wrap Edges, a new
	// particle count) is not a movement, so it gets no streak across the picture.
	vec3 previous = texelFetch( uPreviousPositions, texel, 0 ).xyz;
	if( distance( previous, state.xyz ) > 1.5 )
		previous = state.xyz;
	float unused;
	// The tail is where the particle was on screen a frame ago, seen by last
	// frame's camera: the streak follows the movement in the picture, whether
	// the particle moved or the shape turned under it.
	vec4 previousClip = uPreviousMVP * vec4( BeatOffset( previous, texel, seed, unused ), 1.0 );
	vec4 tailClip = mix( clip, previousClip, uStreakLength );
	if( tailClip.w < 0.05 )// behind the camera: no streak
		tailClip = clip;

	vec2 head = ToPixels( clip ), start = ToPixels( tailClip );
	// A jump on screen (Path Position moved, Rotate snapped) is not a
	// movement either.
	if( distance( head, start ) > 0.35 * max( uViewport.x, uViewport.y ) )
		start = head;
	vec2 axis = head - start;
	float streak = length( axis );
	vec2 direction = streak > 1e-3 ? axis / streak : vec2( 1.0, 0.0 );
	vec2 across = vec2( -direction.y, direction.x );
	float radius = drawn * 0.5;

	// Two triangles: x picks the tail or head end, y the side.
	const vec2 corners[ 6 ] = vec2[]( vec2( 0.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( 0.0, 1.0 ),
									  vec2( 0.0, 1.0 ), vec2( 1.0, -1.0 ), vec2( 1.0, 1.0 ) );
	vec2 corner = corners[ gl_VertexID ];
	float distanceAlong = corner.x < 0.5 ? -radius : streak + radius;
	vec2 pixel = start + direction * distanceAlong + across * ( corner.y * radius );
	vLocal = vec2( distanceAlong, corner.y * radius );
	vLength = streak;
	vRadius = radius;
	gl_Position = clip.w > 0.05 ? vec4( pixel / uViewport * 2.0 - 1.0, 0.0, 1.0 ) : vec4( 2.0, 2.0, 2.0, 1.0 );
#else
	gl_Position = clip;
	gl_PointSize = drawn;
#endif

	// 0 at the front of the shape, 1 at the back.
	float depth = clamp( ( clip.w - uDistance + 1.2 ) / 2.4, 0.0, 1.0 );
	float speed = length( velocity );

	// Share of Color 2. For the gradients, Color Mix slides the balance:
	// 0 is all Color 1, 0.5 the full gradient, 1 all Color 2.
	float weight = 0.0;
	if( uColorMix > 0.0 )
	{
		if( uColorMode == 0 )
			weight = clamp( speed * 4.0 * uColorMix * uColorMix, 0.0, 1.0 );
		else if( uColorMode == 3 )
			weight = fract( seed * 7919.37 ) < uColorMix ? 1.0 : 0.0;
		else
		{
			float along = uColorMode == 1 ? clamp( 0.5 - 0.5 * position.y, 0.0, 1.0 ) : depth;
			weight = smoothstep( 0.0, 1.0, along - 1.0 + 2.0 * uColorMix );
		}
	}
	vec3 color = mix( uColor, uColor2, weight );

	// Light. Self Glow is how much each particle shines on its own; the
	// lights add to that. Shading needs a surface, so key light and rim fade
	// out for particles far from their spot (blown away, bursting, in
	// mid-morph); the centre light still reaches those by distance. Lit
	// parts take on each light's colour; under white light they become a
	// paler shade of the particle colour.
	vec3 ownColor = color * uSelfGlow;
	if( uKeyLight > 0.0 || uRim > 0.0 || uCenterLight > 0.0 )
	{
		// On the surface means near the spot the Morph fader blends to, or
		// still near the mesh the fader is moving away from, so the light
		// hands over smoothly when the fader moves.
		vec3 normal1, normal2;
		vec3 spot1 = MeshSpot( uTargets, uNormals, uAnchor, uScale, texel, normal1 );
		vec3 spot2 = MeshSpot( uMorphTargets, uMorphNormals, uMorphAnchor, uMorphScale, texel, normal2 );
		float weight = MorphWeight( seed );
		vec3 goal = mix( spot1, spot2, weight );
		vec3 goalNormal = mix( normal1, normal2, weight );
		vec3 behind = weight < 0.5 ? spot2 : spot1;
		vec3 behindNormal = weight < 0.5 ? normal2 : normal1;
		float atGoal = 1.0 - smoothstep( 0.3, 0.8, distance( state.xyz, goal ) );
		float atBehind = ( 1.0 - smoothstep( 0.3, 0.8, distance( state.xyz, behind ) ) ) * ( 1.0 - atGoal );
		vec3 n = normalize( goalNormal * ( atGoal + 1e-3 ) + behindNormal * atBehind + vec3( 0.0, 1e-6, 0.0 ) );
		float onSurface = max( atGoal, atBehind );
		vec3 tint = 0.35 + 0.65 * color;
		vec3 lit = vec3( 0.0 );

		// Key light: a little wrap-around, then the side turned away drops
		// almost to black, for a hard light-to-shadow edge.
		float key = uKeyLight * onSurface;
		float facing = clamp( ( dot( n, uLightDirection ) + 0.2 ) / 1.2, 0.0, 1.0 );
		lit += uKeyColor * tint * ( 2.5 * facing * facing * key );

		// Rim: the outline, where the surface turns away from the camera.
		float edge = 1.0 - abs( dot( n, normalize( uEyePosition - position ) ) );
		lit += uRimColor * tint * ( 3.0 * uRim * onSurface * edge * edge * edge );

		// Centre light: a lamp inside the shape, as if in fog. It lights
		// particles by their distance from the centre, whichever way they face.
		lit += uCenterColor * tint * ( 6.0 * uCenterLight * exp( -3.5 * dot( position, position ) ) );

		ownColor += lit;
	}
	color = ownColor;

	// Points smaller than a pixel are drawn at one pixel but dimmer, which avoids flicker.
	// The smaller close-up points get some of their light back, so zooming in
	// doesn't dim the shape.
	float intensity = uBrightness * clamp( size * size, 0.0, 1.0 ) * smoothstep( 0.05, 0.4, clip.w ) * min( pow( closeness, 0.5 ), 3.0 );
#ifdef STREAKS
	// A streak spreads the particle's light along its length, like motion
	// blur, but keeps a little extra so trails stay visible: a long streak
	// gives off somewhat more light than a round point, not many times more,
	// which would flood the picture with glaring lines.
	intensity /= pow( 1.0 + 2.0 * streak / ( 3.14159 * radius ), 0.75 );
#endif

	// Speed Glow: moving particles light up and burn towards white. It is
	// part of their own light, so it dims with Self Glow.
	float glow = smoothstep( 0.15, 1.2, speed ) * uHeat;
	color = mix( color, vec3( uSelfGlow ), glow * 0.85 );
	intensity *= 1.0 + 2.5 * glow;

	// Twinkle: each particle flares now and then at its own rate.
	if( uTwinkle > 0.0 )
	{
		float rate = 1.5 + 3.0 * fract( seed * 3571.13 );
		float flare = pow( 0.5 + 0.5 * sin( uTime * rate + seed * 6283.0 ), 6.0 );
		intensity *= mix( 1.0, 0.25 + 3.0 * flare, uTwinkle );
	}
	intensity *= 1.0 + waveGlow * min( uShockwave * 5.0, 1.5 );
	// Depth Fade: the back of the shape sinks into darkness, like fog.
	intensity *= 1.0 - uDepthFade * 0.9 * depth;

	// A single particle can't shine brighter than white: above 0.7 its light
	// eases off towards 1, keeping the colour. Stacked lights would otherwise
	// clip the soft falloff into hard flat blobs and shift the hue. Dense
	// areas still add up to white, as before.
	float peak = max( max( color.r, color.g ), color.b ) * intensity;
	if( peak > 0.7 )
		intensity *= ( 0.7 + 0.3 * ( 1.0 - exp( ( 0.7 - peak ) / 0.3 ) ) ) / peak;

	vColor = vec4( color, intensity );
}
)";

// Soft round points (or capsules), premultiplied, for additive blending.
static const char kRenderFSBody[] = R"(
in vec4 vColor;
#ifdef STREAKS
noperspective in vec2 vLocal;
flat in float vLength;
flat in float vRadius;
#endif
out vec4 fragColor;

uniform float uSoftness;// falloff exponent: below 1 a crisp dot, above 2 a soft glow

void main()
{
#ifdef STREAKS
	// Distance to the streak's centre line, relative to its radius.
	float beyond = vLocal.x - clamp( vLocal.x, 0.0, vLength );
	float r2 = ( beyond * beyond + vLocal.y * vLocal.y ) / ( vRadius * vRadius );
#else
	vec2 c = gl_PointCoord * 2.0 - 1.0;
	float r2 = dot( c, c );
#endif
	if( r2 > 1.0 )
		discard;
	// Scaled so every softness gives a point the same total light.
	float alpha = vColor.a * pow( 1.0 - r2, uSoftness ) * ( uSoftness + 1.0 ) / 3.0;
	fragColor = vec4( vColor.rgb * alpha, alpha );
}
)";

// Full render shader source: points, or capsule streaks.
inline std::string RenderShaderSource( const char* body, bool streaks )
{
	return std::string( "#version 410 core\n" ) + ( streaks ? "#define STREAKS\n" : "" ) + body;
}
}// namespace swarm
