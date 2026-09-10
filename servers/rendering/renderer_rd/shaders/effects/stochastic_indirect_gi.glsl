#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : enable

// Ray-traced indirect lighting ("Lumen-lite" final gather).
// Per pixel: cosine-sampled hemisphere rays traced against the scene BVH,
// shaded at the hit point from the SDFGI cascades (direct light volumes),
// optionally boosted with last frame's on-screen radiance, and falling back
// to the sky on miss. Optionally one GGX-sampled ray feeds a rough specular
// term. Outputs demodulated irradiance (no albedo) and specular radiance;
// the stochastic denoiser filters both like the direct lighting pair.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"
#include "surface_cache_inc.glsl"

#define SDFGI_MAX_CASCADES 8

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;

layout(set = 0, binding = 3, std140) uniform Params {
	mat4 view_from_ndc; // Inverse of the (depth-corrected) projection.
	mat4 ndc_from_view; // The (depth-corrected) projection, for screen traces.
	mat4 world_from_view; // Camera transform.
	mat4 reproject; // Current NDC -> previous frame NDC, for screen radiance.
	ivec2 screen_size;
	ivec2 full_screen_size;
	uint depth_scale; // 2 when sampling at half resolution.
	uint frame_index;
	uint ray_count; // Diffuse rays per pixel.
	uint flags; // FLAG_*.
	vec4 sky_quat_or_color; // Sky orientation quaternion, or flat sky color.
	float sky_energy;
	float ray_bias;
	vec2 sky_border; // x: octmap border size, y: 1 - 2 * border.
	float z_far;
	uint voxel_gi_count;
	float ao_range; // Hit distances are normalized and clamped against this.
	float inv_ao_range;
	float screen_radiance_border_fade;
	float screen_radiance_clamp;
	float probe_floor; // Neutral albedo the probe irradiance is turned into radiance with.
	float cache_scale; // Multiplies the solid cache tier (see FLAG_CALIBRATE_CACHE).
	float probe_scale; // Multiplies the probe tier.
	uint surface_cache_atlas_size;
	uint surface_cache_frame; // The cache's own clock, stamped on the sets hits reach.
	uint hit_capacity; // Packets the deferred hit shading has room for this frame.
	float card_cone_tan; // The diffuse rays' cone (tangent of the half-angle); a hit reads its card through the mip its footprint covers.
	float card_youth_lod; // The mip a texel relit once is read through (0 disables); a level less per doubling of its relights (see surface_cache_lookup).
	uint fallback_parts; // Diagnostics: which histories the fallback shows (0 all; 1 static, 2 dynamic first bounce, 4 later bounces).
	uint pad2;
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
}
params;

#define FLAG_SCREEN_RADIANCE 1u
#define FLAG_SPECULAR 2u
#define FLAG_SDFGI 4u
#define FLAG_SKY_MODE_SKY 8u
#define FLAG_SKY_MODE_COLOR 16u
#define FLAG_SCREEN_TRACES 32u
#define FLAG_VOXEL_GI 64u
#define FLAG_LIGHT_CASCADE_RADIANCE 128u
#define FLAG_CALIBRATE_CACHE 256u
#define FLAG_SURFACE_CACHE 512u // Hits read the surface cache's lit cards where one covers them.
#define FLAG_MIRROR 1024u // Smooth surfaces trace a mirror ray instead of leaving reflections to probes.
#define FLAG_HIT_SHADING 2048u // Hits without a card are deferred to their materials (scene_hit_shade.glsl).
#define FLAG_HIT_ALL 4096u // Every hit is, cards or not.
#define FLAG_HIT_MIRROR 8192u // The mirror ray's hits are, cards or not.
#define FLAG_HIT_DEBUG_CONSTANT 16384u // Debug: a constant radiance in place of the deferral, to check the resolve against.
#define FLAG_FALLBACK_ALL 32768u // Diagnostics: the cards' fallback for every pixel, not only the young.
#define FLAG_FALLBACK_OFF 65536u // Diagnostics: no fallback, the young keep their own filtered history.
#define FLAG_FALLBACK_EVERY 131072u // The fallback for every pixel: the temporal pass modulates the history by its change (GODOT_GI_MOD).
#define FLAG_TIER_STATS 262144u // Diagnostics (GODOT_GI_TIER_PRINT): count which tier answered each ray, and with how much light.

layout(set = 0, binding = 4) uniform sampler2DArray stbn_texture;

// The SDFGI radiance cache: distance fields for hit normals, direct light
// volumes (with anisotropy) for hit shading. Bound to defaults when inactive.
layout(set = 0, binding = 5) uniform texture3D sdf_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 6) uniform texture3D light_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 7) uniform texture3D aniso0_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 8) uniform texture3D aniso1_cascades[SDFGI_MAX_CASCADES];

struct ProbeCascadeData {
	vec3 position; // Offset of (0,0,0), camera-relative, y_mult applied.
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell; // 1/bounds * grid_size
	vec3 pad;
	float exposure_normalization;
};

// Same block the deferred GI resolve uses (GI::SDFGIData).
layout(set = 0, binding = 9, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	ProbeCascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 10) uniform texture2DArray sky_radiance;
#else
layout(set = 0, binding = 10) uniform texture2D sky_radiance;
#endif

layout(set = 0, binding = 11) uniform sampler linear_sampler_mipmaps;

// Last frame's rendered scene, for on-screen hit radiance (texture detail and
// emissive surfaces the cache lacks). Default black when disabled.
layout(set = 0, binding = 12) uniform sampler2D screen_radiance_texture;

// VoxelGI volumes as a fallback radiance cache for scenes without SDFGI
// (same data the cone-traced resolve uses; the xform expects camera-relative
// world positions).
#define MAX_VOXEL_GI_INSTANCES 8

struct VoxelGIData {
	mat4 xform; // World (camera-relative) to probe cell space.

	vec3 bounds;
	float dynamic_range;

	float bias;
	float normal_bias;
	bool blend_ambient;
	uint mipmaps;

	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 13, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 0, binding = 14) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];

// The SDFGI lightprobes: irradiance, octahedrally encoded per probe, already
// converged over the integrator's accumulation window. Unlike the light
// cascades these cover all space rather than only solid cells, which is what
// makes them usable as a floor under a one-ray-per-pixel gather.
layout(set = 0, binding = 15) uniform texture2DArray lightprobe_texture;
layout(set = 0, binding = 16) uniform texture3D occlusion_texture;

// Calibration of the cache tier against the screen tier: on-screen hits see
// both values for the same point, and their summed ratio (read back on the
// CPU, smoothed, handed back as params.cache_scale) is what the off-screen
// hits are missing. Fixed point in 1/1024 luminance units over a subsample of
// pixels, so a frame's sums fit 32 bits.
layout(set = 0, binding = 17, std430) restrict buffer CalibrationBuffer {
	// [0]: solid tier (cascades / VoxelGI), [1]: probe tier.
	uint sum_screen[2];
	uint sum_cache[2];
	uint samples[2];
	// Diagnostics (FLAG_TIER_STATS): per TIER_SRC_* value, how many rays it
	// answered and their summed luminance in 1/16 units.
	uint tier_count[8];
	uint tier_lum[8];
}
calibration;

// The surface cache (see surface_cache.cpp): per TLAS instance the record the
// ray query's custom index names, per card set its six captures, and the lit
// radiance atlas those captures were shaded into. Hits that land on a card
// read it instead of the coarse caches below, and stamp the set so the
// lighting pass relights it next frame.
layout(set = 0, binding = 18, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;

layout(set = 0, binding = 19, std430) restrict readonly buffer CardSets {
	CardSet data[];
}
card_sets;

layout(set = 0, binding = 20, std430) restrict writeonly buffer CardRequests {
	uint frame[];
}
card_requests;

layout(set = 0, binding = 21) uniform sampler2D card_lighting_atlas;
layout(set = 0, binding = 22) uniform sampler2D card_depth_atlas;
// g: how much the card's lighting changed at its last relight (see
// surface_cache_light.glsl), the temporal gradient a hit hands its pixel.
layout(set = 0, binding = 23) uniform usampler2D card_change_atlas; // Packed halves; the gradient is the second half of the second uint.
// Last frame's GI temporal output, whose alpha carries a pixel's change mark
// with its decay: an on-screen hit reads last frame's colour, so it inherits
// that pixel's mark and a restart propagates through the screen bounces.
layout(set = 0, binding = 24) uniform sampler2D prev_gi_history;

// Deferred hit shading (see rt_hit_inc.glsl): the material slot of every
// instance geometry, the packets this pass appends for the hits it hands
// to the materials, their per-slot counts, and the pixel's result slots the
// resolve pass folds back after the materials ran. Dummies without
// FLAG_HIT_SHADING.
layout(set = 0, binding = 25, std430) restrict readonly buffer HitMaterials {
	uint data[];
}
hit_materials;

layout(set = 0, binding = 26, std430) restrict writeonly buffer HitPackets {
	uint data[];
}
hit_packets;

layout(set = 0, binding = 27, std430) restrict buffer HitCounts {
	uint data[];
}
hit_counts;

layout(set = 0, binding = 28, std430) restrict writeonly buffer HitResults {
	uvec4 data[];
}
hit_results;

// Set per ray in main(): where a deferred hit's result goes (the specular
// ray's to the reflection), and whether it is the mirror ray, whose hits go
// to the material before the cards.
ivec2 hit_pixel = ivec2(0);
uint hit_slot = 0u;
bool hit_specular = false;
bool hit_mirror = false;

// Set per pixel in main(): the largest lighting change a ray of this pixel
// landed on. The temporal pass restarts the history in proportion.
float pixel_change = 0.0;

// Set per pixel in main(): this pixel's hits contribute to the calibration.
bool calibrate_pixel = false;

// Diagnostics (FLAG_TIER_STATS): where the last trace_radiance() got its
// answer. The chain sets trace_source where it leaves without a cache tier
// (a deferred hit, a miss, a scene with no cache); otherwise the tier that
// answered is cache_tier, and boost_from_screen says whether last frame's
// screen then stood in for it.
#define TIER_SRC_SCREEN 0u
#define TIER_SRC_CARD 1u
#define TIER_SRC_HIT_SHADED 2u
#define TIER_SRC_CASCADE 3u // The SDFGI light cascades, or a VoxelGI volume.
#define TIER_SRC_PROBE 4u
#define TIER_SRC_SKY 5u
#define TIER_SRC_NONE 6u
#define TIER_SRC_UNSET 7u
uint trace_source = TIER_SRC_UNSET;
bool boost_from_screen = false;

#define SDFGI_OCT_SIZE 6

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_ambient;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_reflection;
// View depth of the shaded texel, for the half-resolution upsample.
layout(set = 1, binding = 2, r16f) uniform restrict writeonly image2D out_view_depth;
// xyz: the first spherical-harmonic moment of the incoming radiance,
// luminance weighted, in world space. Deliberately left unnormalized: because
// |sum(lum_i * dir_i)| <= sum(lum_i) = luminance(irradiance) * ray_count, the
// ratio |xyz| / luminance(irradiance) is bounded by 1, and that bound is what
// keeps the reconstruction stable. It also survives any non-negative weighted
// average, so both denoiser passes preserve it.
// w: mean hit distance normalized against ao_range (0 = contact, 1 = far or
// sky). Serves as both the ambient visibility term and the denoiser's
// hit-distance edge stop.
// Last frame's temporal meta (r: history frames / 64), read at the pixel's
// reprojection to tell a young pixel, and the cards' accumulated bounce
// irradiance (a: relights / 64), the young pixel's stand-in (see the end of
// main).
layout(set = 0, binding = 29) uniform sampler2D prev_gi_meta;
layout(set = 0, binding = 30) uniform sampler2D card_indirect_atlas;
layout(set = 0, binding = 31) uniform sampler2D card_indirect_dyn_atlas; // The dynamic lights' bounce, apart (surface_cache_light.glsl trace_dynamic).
layout(set = 0, binding = 32) uniform sampler2D card_indirect_dyn2_atlas; // Their second bounce.
// The cards' albedo and captured normal, and the dynamic lights (world
// space, pad 1 for a spot, with their weights): the lighting atlas holds no
// dynamic light's direct term (surface_cache_light.glsl accumulate), a hit
// adds it from the light's current state.
layout(set = 0, binding = 33) uniform sampler2D card_albedo_atlas;
layout(set = 0, binding = 34) uniform sampler2D card_normal_atlas;
layout(set = 0, binding = 36) uniform sampler2D card_static_atlas; // Alpha: the dynamic lights' visibility ratio.
layout(set = 0, binding = 35, std430) restrict readonly buffer DynamicLights {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
	vec4 weights[2];
	LightData data[8];
}
dyn_lights;

float card_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

// The dynamic histories' age at a card texel, in relights, divided by their
// share of the texel's bounce: a young dynamic term that is a tenth of the
// light counts as ten times its age (the youth tent and level are blurs,
// and after any move every texel's dynamic history is young).
float card_dynamic_age(ivec2 tex0, vec3 static_bounce) {
	vec4 dyn2 = texelFetch(card_indirect_dyn2_atlas, tex0, 0);
	vec3 dyn = max(texelFetch(card_indirect_dyn_atlas, tex0, 0).rgb, vec3(0.0)) + max(dyn2.rgb, vec3(0.0));
	float dyn_lum = dot(dyn, vec3(0.2126, 0.7152, 0.0722));
	float share = dyn_lum / max(dyn_lum + dot(max(static_bounce, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), 1e-4);
	return dyn2.a * 64.0 / max(share, 0.05);
}

// The dynamic lights' unshadowed direct term at a point (the card lighting's
// light_contribution_world, over pi).
vec3 card_dynamic_direct(vec3 world_pos, vec3 n) {
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		vec3 rel = ld.position - world_pos;
		float len = length(rel);
		float attenuation = card_omni_attenuation(len, ld.inv_radius, ld.attenuation);
		vec3 l = rel / max(len, 1e-5);
		if (ld.pad > 0.5) {
			float scos = max(dot(-l, normalize(ld.direction)), ld.cone_angle);
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
			attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
		}
		sum += ld.color * (max(dot(n, l), 0.0) * attenuation * (1.0 / 3.14159265359)) * dyn_lights.weights[i >> 2u][i & 3u];
	}
	return sum;
}

layout(set = 1, binding = 3, rgba16f) uniform restrict writeonly image2D out_directional;
// The young pixel's fallback: the bounce irradiance of the card under its
// own surface (rgb, the cards' convention, the gather's own) and the
// card's relight count (a, / 64); zero where there is none.
layout(set = 1, binding = 4, rgba16f) uniform restrict writeonly image2D out_fallback;
// The rough reflection ray, for the resolve before the temporal pass
// (stochastic_reflection_resolve.glsl): xy its direction in view space
// (octahedral), z the density it was drawn with (0: no rough ray, a
// mirror's included), w the hit distance (1e4 and above: a miss).
layout(set = 1, binding = 5, rgba16f) uniform restrict writeonly image2D out_spec_ray;
// History frames under which the gather spends the primary ray on it.
#define FALLBACK_FRAMES 8.0

#define M_PI 3.14159265359

float luminance(vec3 c) {
	return dot(c, params.luma_weights.rgb);
}

// STBN lookup, one stream per random decision (see stochastic_direct_lighting;
// this copy carries the same per-64x64-tile hashed offset so the pattern does
// not repeat across the screen).
uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

vec2 stbn_sample(ivec2 pixel, uint stream) {
	uint epoch = params.frame_index >> 4;
	uint k = stream + epoch * 8u;
	ivec2 shift = ivec2(fract(vec2(k) * vec2(0.7548776662, 0.5698402909)) * 64.0);
	uint tile_hash = pcg_hash(uint(pixel.x >> 6) ^ (uint(pixel.y >> 6) * 0x9E3779B9u));
	ivec2 tile_shift = ivec2(tile_hash & 63u, (tile_hash >> 6) & 63u);
	ivec2 p = (pixel + shift + tile_shift) & 63;
	return texelFetch(stbn_texture, ivec3(p, int(params.frame_index & 15u)), 0).rg;
}

mat3 basis_around(vec3 n) {
	vec3 t = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), n));
	return mat3(t, cross(n, t), n);
}

vec3 cosine_hemisphere(vec3 n, vec2 rnd) {
	float ang = rnd.x * 2.0 * M_PI;
	float r = sqrt(rnd.y);
	return normalize(basis_around(n) * vec3(r * cos(ang), r * sin(ang), sqrt(max(1.0 - rnd.y, 0.0))));
}

vec3 sky_eval(vec3 world_dir) {
	if (bool(params.flags & FLAG_SKY_MODE_SKY)) {
		vec4 q = params.sky_quat_or_color;
		vec3 t = cross(q.xyz, world_dir);
		vec3 dir = world_dir + ((t * q.w) + cross(q.xyz, t)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sampler2DArray(sky_radiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(dir, params.sky_border), 0.0), 2.0).rgb * params.sky_energy;
#else
		return textureLod(sampler2D(sky_radiance, linear_sampler_mipmaps), vec3_to_oct_with_border(dir, params.sky_border), 2.0).rgb * params.sky_energy;
#endif
	} else if (bool(params.flags & FLAG_SKY_MODE_COLOR)) {
		return params.sky_quat_or_color.rgb * params.sky_energy;
	}
	return vec3(0.0);
}

// Lit-voxel radiance from a VoxelGI volume containing the hit, used as the
// cache when no SDFGI is active. Samples the voxel the hit surface occupies
// (stepped slightly off along the reversed ray so the lookup lands on the
// solid cell's lit side).
vec3 voxel_cache_radiance(vec3 rel_pos, vec3 ray_dir) {
	for (uint i = 0u; i < params.voxel_gi_count; i++) {
		vec3 pos = (voxel_gi_instances.data[i].xform * vec4(rel_pos, 1.0)).xyz;
		if (any(lessThan(pos, vec3(0.0))) || any(greaterThan(pos, voxel_gi_instances.data[i].bounds))) {
			continue;
		}
		vec3 n = normalize((voxel_gi_instances.data[i].xform * vec4(-ray_dir, 0.0)).xyz);
		pos += n * (voxel_gi_instances.data[i].normal_bias + 1.0);
		vec3 uvw = pos / voxel_gi_instances.data[i].bounds;
		vec4 light = textureLod(sampler3D(voxel_gi_textures[i], linear_sampler_mipmaps), uvw, 1.0);
		return light.rgb * voxel_gi_instances.data[i].dynamic_range * voxel_gi_instances.data[i].exposure_normalization;
	}
	return vec3(0.0);
}

vec2 octahedron_wrap(vec2 v) {
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

vec2 octahedron_encode(vec3 n) {
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : octahedron_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

// Irradiance arriving at a camera-relative world position, trilinearly
// interpolated from the eight surrounding lightprobes and weighted by their
// occlusion. This is the same evaluation the deferred SDFGI resolve performs
// (sdfvoxel_gi_process in gi.glsl), diffuse only.
vec3 sdfgi_probe_irradiance(vec3 rel_pos, vec3 normal) {
	if (!bool(params.flags & FLAG_SDFGI)) {
		return vec3(0.0);
	}
	vec3 p = vec3(rel_pos.x, rel_pos.y * sdfgi.y_mult, rel_pos.z);
	vec3 n = normalize(vec3(normal.x, normal.y * sdfgi.y_mult, normal.z));

	for (uint c = 0u; c < sdfgi.max_cascades; c++) {
		vec3 cascade_pos = (p - sdfgi.cascades[c].position) * sdfgi.cascades[c].to_probe;
		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue;
		}
		cascade_pos += n * sdfgi.normal_bias;

		ivec3 probe_base_pos = ivec3(floor(cascade_pos));
		ivec3 tex_pos = ivec3(probe_base_pos.xy, int(c));
		tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
		tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);
		vec3 diffuse_posf = (vec3(tex_pos) + vec3(octahedron_encode(n) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

		vec4 accum = vec4(0.0);
		for (uint j = 0u; j < 8u; j++) {
			ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
			ivec3 probe_posi = probe_base_pos + offset;

			vec3 probe_pos = vec3(probe_posi);
			vec3 probe_to_pos = cascade_pos - probe_pos;
			vec3 probe_dir = normalize(-probe_to_pos);
			vec3 trilinear = vec3(1.0) - abs(probe_to_pos);
			float weight = trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(n, probe_dir));

			if (sdfgi.use_occlusion) {
				ivec3 occ_indexv = abs((sdfgi.cascades[c].probe_world_offset + probe_posi) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
				vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

				vec3 occ_pos = clamp(cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
				occ_pos.z += float(c);
				if (occ_indexv.z != 0) {
					occ_pos.x += 1.0;
				}
				occ_pos *= sdfgi.occlusion_renormalize;
				float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler_mipmaps), occ_pos, 0.0), occ_mask);
				weight *= max(occlusion, 0.01);
			}

			vec3 pos_uvw = diffuse_posf;
			pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
			pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
			accum += vec4(textureLod(sampler2DArray(lightprobe_texture, linear_sampler_mipmaps), pos_uvw, 0.0).rgb * weight, weight);
		}

		if (accum.a > 0.0) {
			accum.rgb /= accum.a;
		}
		return accum.rgb * sdfgi.cascades[c].exposure_normalization * sdfgi.energy;
	}
	return vec3(0.0);
}

// The surface normal at a hit, from the distance field's gradient in the
// finest cascade holding the point, faced against the ray. The reversed ray
// direction is not a normal: a grazing hit read the probes along the wall,
// with the normal bias sliding along it and half the irradiance cone inside
// it, and came back at a fraction of what the resolve puts on screen for the
// same wall.
vec3 sdfgi_hit_normal(vec3 rel_pos, vec3 ray_dir) {
	vec3 p = vec3(rel_pos.x, rel_pos.y * sdfgi.y_mult, rel_pos.z);
	vec3 d = normalize(vec3(ray_dir.x, ray_dir.y * sdfgi.y_mult, ray_dir.z));
	for (uint c = 0u; c < sdfgi.max_cascades; c++) {
		vec3 cell_pos = (p - sdfgi.cascades[c].position) * sdfgi.cascades[c].to_cell;
		if (any(lessThan(cell_pos, vec3(0.0))) || any(greaterThanEqual(cell_pos, sdfgi.grid_size))) {
			continue;
		}
		vec3 uvw = cell_pos / sdfgi.grid_size;
		const float EPSILON = 0.001;
		vec3 n = vec3(
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(EPSILON, 0.0, 0.0)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(EPSILON, 0.0, 0.0)).r,
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(0.0, EPSILON, 0.0)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(0.0, EPSILON, 0.0)).r,
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(0.0, 0.0, EPSILON)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(0.0, 0.0, EPSILON)).r);
		float nl = length(n);
		if (nl < 1e-6) {
			break;
		}
		n /= nl;
		// Back to world orientation (the y scale is undone on the way out).
		n = normalize(vec3(n.x, n.y / sdfgi.y_mult, n.z));
		return dot(n, ray_dir) > 0.0 ? -n : n;
	}
	return -ray_dir;
}

// Radiance leaving a camera-relative world position, resolved through the
// cache chain the idTech 8 gather uses (Sousa 2025, slide 21): a tier is taken
// only when it reports a valid entry, and an invalid one hands over to the
// next rather than contributing a value. Here that is the SDFGI light
// cascades, then the lightprobes. Blending the two instead would put a second
// voxel-scale pattern on top of the first, since they disagree per hit.
// Which tier the last sdfgi_cache_radiance() call answered from: 0 the light
// cascades or a VoxelGI volume (albedo x direct light and some bounce, at
// solid cells), 1 the lightprobes (bounce light only, everywhere). The two
// are different quantities with different deficits, and the calibration
// against the screen tier keeps one scale for each.
#define CACHE_TIER_SOLID 0u
#define CACHE_TIER_PROBE 1u
#define CACHE_TIER_CARD 2u // Surface cache: already outgoing radiance, never calibrated.
uint cache_tier = CACHE_TIER_PROBE;

vec3 sdfgi_cache_radiance(vec3 rel_pos, vec3 ray_dir) {
	cache_tier = CACHE_TIER_PROBE;
	if (!bool(params.flags & FLAG_SDFGI)) {
		if (bool(params.flags & FLAG_VOXEL_GI)) {
			cache_tier = CACHE_TIER_SOLID;
			return voxel_cache_radiance(rel_pos, ray_dir);
		}
		trace_source = TIER_SRC_NONE;
		return vec3(0.0);
	}
	// The light cascades hold albedo x (direct + feedback x probe) at solid
	// cells; the probes hold the bounce irradiance everywhere, and nothing of
	// the direct light falling on the hit, which in a lit room is most of what
	// leaves it. Against a radiosity solve of a closed box neither tier is
	// near the rendered colour on its own (cascades 1/13 of it, probes 1/15),
	// so both are calibrated against the screen tier; the cascades stay the
	// first choice because they carry the hit's albedo and where the light
	// actually falls, which a scale on a flat probe field cannot recover.
	if (!bool(params.flags & FLAG_LIGHT_CASCADE_RADIANCE)) {
		return sdfgi_probe_irradiance(rel_pos, sdfgi_hit_normal(rel_pos, ray_dir)) * params.probe_floor;
	}

	vec3 p = vec3(rel_pos.x, rel_pos.y * sdfgi.y_mult, rel_pos.z);
	vec3 d = normalize(vec3(ray_dir.x, ray_dir.y * sdfgi.y_mult, ray_dir.z));

	for (uint c = 0u; c < sdfgi.max_cascades; c++) {
		// At the hit, not half a cell back along the ray. The pull-back put
		// the tap on the air side of the surface, where the cascade holds
		// nothing: filtered against empty neighbours every tap came back
		// scaled by how the ray happened to enter the cell, which is a
		// voxel-scale mottle rather than a radiance.
		vec3 cell_pos = (p - sdfgi.cascades[c].position) * sdfgi.cascades[c].to_cell;
		if (any(lessThan(cell_pos, vec3(0.0))) || any(greaterThanEqual(cell_pos, sdfgi.grid_size))) {
			continue;
		}
		vec3 uvw = cell_pos / sdfgi.grid_size;

		// The gradient wants filtering; the light and coverage do not.
		const float EPSILON = 0.001;
		vec3 hit_normal = vec3(
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(EPSILON, 0.0, 0.0)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(EPSILON, 0.0, 0.0)).r,
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(0.0, EPSILON, 0.0)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(0.0, EPSILON, 0.0)).r,
				texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw + vec3(0.0, 0.0, EPSILON)).r - texture(sampler3D(sdf_cascades[c], linear_sampler_mipmaps), uvw - vec3(0.0, 0.0, EPSILON)).r);
		float nl = length(hit_normal);
		if (nl < 1e-6) {
			hit_normal = -d;
		} else {
			hit_normal /= nl;
		}

		// Point-fetch the cell the hit landed in. The anisotropic coverage is
		// written only where geometry was voxelized, so a zero sum is the
		// cache saying it has no entry here -- the distinction between "no
		// data" and "black surface" the chain needs to pick a tier. The hit
		// lies on the surface, and the voxelized cell is the one just inside
		// it, so the search steps into the surface along its normal and along
		// the ray before the entry is called invalid. With the single half-step
		// along the ray this used to take, most hits on a wall found an empty
		// cell and the tier fell through to the probes, which carry no direct
		// light at all.
		ivec3 grid_max = ivec3(sdfgi.grid_size) - ivec3(1);
		vec3 probes_at[6] = vec3[](vec3(0.0), -hit_normal * 0.5, d * 0.5, -hit_normal * 1.0, d * 1.0, -hit_normal * 1.5);
		ivec3 celli = ivec3(0);
		vec4 aniso0 = vec4(0.0);
		vec2 aniso1 = vec2(0.0);
		for (int k = 0; k < 6; k++) {
			celli = clamp(ivec3(cell_pos + probes_at[k]), ivec3(0), grid_max);
			aniso0 = texelFetch(sampler3D(aniso0_cascades[c], linear_sampler_mipmaps), celli, 0);
			aniso1 = texelFetch(sampler3D(aniso1_cascades[c], linear_sampler_mipmaps), celli, 0).rg;
			if (dot(aniso0, vec4(1.0)) + dot(aniso1, vec2(1.0)) > 0.0) {
				break;
			}
		}
		if (dot(aniso0, vec4(1.0)) + dot(aniso1, vec2(1.0)) <= 0.0) {
			// No entry at this resolution. Coarser cascades have larger cells
			// and so are likelier to have voxelized the surface at all; only
			// once every one of them comes up empty is the tier exhausted.
			continue;
		}

		vec3 hit_light = texelFetch(sampler3D(light_cascades[c], linear_sampler_mipmaps), celli, 0).rgb;
		vec3 hit_aniso0 = aniso0.rgb;
		vec3 hit_aniso1 = vec3(aniso0.a, aniso1);

		vec3 radiance = hit_light * (dot(max(vec3(0.0), (hit_normal * hit_aniso0)), vec3(1.0)) + dot(max(vec3(0.0), (-hit_normal * hit_aniso1)), vec3(1.0)));
		cache_tier = CACHE_TIER_SOLID;
		return radiance * sdfgi.cascades[c].exposure_normalization * sdfgi.energy;
	}

	// Last tier. Always valid where the probe grid reaches, so a hit never
	// falls through to a spurious zero.
	return sdfgi_probe_irradiance(rel_pos, sdfgi_hit_normal(rel_pos, ray_dir)) * params.probe_floor;
}

// On-screen hits can read last frame's rendered radiance, which carries the
// texture detail and emissive surfaces the cache lacks. Luminance-clamped
// against the cache value so screen feedback cannot run away.
vec3 screen_radiance_boost(vec3 view_hit, vec3 raw_cache_radiance) {
	// The cache tier as the hits that have nothing else see it. The scale is
	// the calibration's estimate of the screen tier over the cache tier (1
	// when off), applied before the screen lookup so both fallbacks below and
	// the border hand-back agree.
	uint tier = cache_tier;
	vec3 cache_radiance = raw_cache_radiance * (tier == CACHE_TIER_SOLID ? params.cache_scale : (tier == CACHE_TIER_PROBE ? params.probe_scale : 1.0));
	if (!bool(params.flags & FLAG_SCREEN_RADIANCE)) {
		return cache_radiance;
	}
	vec4 ndc = params.ndc_from_view * vec4(view_hit, 1.0);
	if (ndc.w <= 0.0) {
		return cache_radiance;
	}
	ndc.xyz /= ndc.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return cache_radiance;
	}
	float scene_depth = textureLod(depth_texture, uv, 0.0).r;
	if (scene_depth == 0.0) {
		return cache_radiance;
	}
	vec4 scene_view = params.view_from_ndc * vec4(ndc.xy, scene_depth, 1.0);
	float scene_z = scene_view.z / scene_view.w;
	if (abs(scene_z - view_hit.z) > max(abs(view_hit.z) * 0.1, 0.25)) {
		return cache_radiance; // The visible surface is not the hit surface.
	}
	// The color buffer holds last frame's scene: look the hit up where it was.
	vec4 prev_ndc = params.reproject * vec4(ndc.xy, scene_depth, 1.0);
	if (prev_ndc.w <= 0.0) {
		return cache_radiance;
	}
	vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
	if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
		return cache_radiance;
	}
	vec3 col = textureLod(screen_radiance_texture, prev_uv, 0.0).rgb;
	// Weaker by a quarter per bounce: two pixels whose rays keep landing on
	// each other would otherwise hand the mark back and forth forever.
	pixel_change = max(pixel_change, textureLod(prev_gi_history, prev_uv, 0.0).a - 0.25);
	float l = luminance(col);
	// Both tiers for the same point: what the calibration is made of. The raw
	// cache value, not the scaled one, or the estimate would chase itself.
	// Sampled before the firefly ceiling, which is keyed to the very scale
	// being estimated. Hits at the frame border are left out along with the
	// rest of what the hand-back below distrusts.
	if (calibrate_pixel && tier < 2u) {
		vec2 border_c = min(min(uv, vec2(1.0) - uv), min(prev_uv, vec2(1.0) - prev_uv));
		if (min(border_c.x, border_c.y) >= params.screen_radiance_border_fade) {
			atomicAdd(calibration.sum_screen[tier], uint(min(l, 64.0) * 1024.0));
			atomicAdd(calibration.sum_cache[tier], uint(min(luminance(raw_cache_radiance), 64.0) * 1024.0));
			atomicAdd(calibration.samples[tier], 1u);
		}
	}
	// Firefly ceiling. Keying this to the cache alone closes a loop: the gather
	// writes the buffer this reads, so where the cache is dim the ceiling caps
	// the screen term below the light actually in the room, the image dims, and
	// the next frame reads the dimmer image -- a wall lit only by bounce
	// ratchets down to the cache value over the accumulation window. The
	// absolute term is what the cache cannot drag down.
	float clamp_l = max(luminance(cache_radiance) * 4.0, params.screen_radiance_clamp) + 0.5;
	if (l > clamp_l) {
		col *= clamp_l / l;
	}
	// Both lookups have to be well inside for the sample to be trustworthy:
	// uv carries the depth test, prev_uv the colour fetch. Whichever is nearer
	// an edge decides how much of the boost survives.
	vec2 border = min(min(uv, vec2(1.0) - uv), min(prev_uv, vec2(1.0) - prev_uv));
	// A fade of 0 collapses the smoothstep back to the hard switch at the edge.
	float screen_share = smoothstep(0.0, max(params.screen_radiance_border_fade, 1e-5), min(border.x, border.y));
	boost_from_screen = screen_share > 0.5;
	return mix(cache_radiance, col, screen_share);
}

// Radiance from the surface cache at a hit, if a captured card covers it.
// The hit goes into the instance's local space and onto each of the six
// cards; a card counts where the hit lies inside its frame, faces the ray
// (a card only saw surfaces facing its own axis) and stored a depth within
// a texel or two of the hit's, which is what keeps a hit on one wall from
// reading the card of the wall behind it. Among the valid cards the one
// facing the ray most squarely wins.

// The atlas position the last successful lookup read (bilinear, in texels),
// and the card it lies in.
vec2 card_atlas_texel = vec2(0.0);
ivec2 card_atlas_origin = ivec2(0);
ivec2 card_atlas_dims = ivec2(1);
// The world radius of the ray's footprint at the hit, set by the caller: the
// card is read through the mip level that footprint covers (never coarser
// than a quarter of the card), so one sample of a rough lobe or of the
// hemisphere lands on an average of the region rather than on whatever
// bright texel it happened to touch.
float card_lookup_footprint = 0.0;
float specular_cone_tan = 0.0; // The reflection ray's, from the lobe (main).
#define CARD_LIGHTING_MIPS 6.0
// How squarely the chosen card faced the lookup direction, and how far
// inside its depth tolerance the surface sat: the fallback's confidence.
float card_lookup_confidence = 0.0;

bool surface_cache_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance, out uint r_set) {
	r_radiance = vec3(0.0);
	r_set = SURFACE_CACHE_INVALID;
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardSet s = card_sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return false;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	vec2 best_uv = vec2(0.0);
	uint best_packed = 0u;
	uint best_k = 0u;
	float best_mismatch = 0.0;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = -dot(axis, local_dir);
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = card_sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(card_depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		// Two texels of the card's own footprint, or a slice of the box.
		// The box's longest extent over the card's longer edge, as when the
		// cards were square: a card's own (shorter) texel made the tolerance
		// reject grazing hits that then paid for the probe fallback.
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_uv = uv01;
			best_packed = packed;
			best_k = k;
			best_mismatch = abs(stored - depth) / tolerance;
		}
	}
	if (best_w <= 0.0) {
		return false;
	}
	// Bilinear inside the card, never across its border, at the mip the
	// footprint covers (its texels are wider, so the border margin is too).
	vec2 best_dims = vec2(card_dims_packed(best_packed));
	float best_texel_world = (longest + 2.0 * s.margin) / max(best_dims.x, best_dims.y);
	float lod = 0.0;
	float max_lod = max(min(CARD_LIGHTING_MIPS - 1.0, log2(min(best_dims.x, best_dims.y)) - 2.0), 0.0);
	if (card_lookup_footprint > 0.0) {
		lod = clamp(log2(max(card_lookup_footprint / best_texel_world, 1.0)), 0.0, max_lod);
	}
	// A young texel is read through a coarser level. The bounce a texel
	// accumulates restarts when the light on it changes (the temporal
	// gradient), and for the relights after that it is one or a few samples
	// -- every ray landing near it reads the same sample, so the noise is
	// not per pixel but a mottle over the whole surface that no screen-space
	// filter averages (a flashlight sweeping the room left the ceiling and
	// walls blotched for the cards' whole window). The level halves the
	// noise per step: a texel relit once reads eight by eight of its
	// neighbours, and the level falls half a step per doubling of its
	// relights until the texel stands on its own at sixty-four.
	if (params.card_youth_lod > 0.0) {
		ivec2 tex0 = card_origin_packed(best_packed) + clamp(ivec2(best_uv * best_dims), ivec2(0), ivec2(best_dims) - ivec2(1));
		vec4 ind0 = texelFetch(card_indirect_atlas, tex0, 0);
		float relights = ind0.a * 64.0;
		if (dyn_lights.count > 0u) {
			relights = min(relights, card_dynamic_age(tex0, ind0.rgb));
		}
		if (relights > 0.0) {
			float youth_lod = params.card_youth_lod * (1.0 - log2(max(relights, 1.0)) / 6.0);
			lod = clamp(max(lod, youth_lod), 0.0, max_lod);
		}
	}
	float margin = 0.5 * exp2(lod);
	vec2 atlas_texel = vec2(card_origin_packed(best_packed)) + clamp(best_uv * best_dims, vec2(margin), best_dims - margin);
	r_radiance = textureLod(card_lighting_atlas, atlas_texel / float(params.surface_cache_atlas_size), lod).rgb;
	if (dyn_lights.count > 0u) {
		// The dynamic lights' direct term at the hit (see the buffer above):
		// the card's albedo and normal at the texel, its accumulated
		// visibility ratio for the shadow.
		ivec2 tex0 = card_origin_packed(best_packed) + clamp(ivec2(best_uv * best_dims), ivec2(0), ivec2(best_dims) - ivec2(1));
		vec3 albedo = texelFetch(card_albedo_atlas, tex0, 0).rgb;
		vec3 n_cam = normalize(texelFetch(card_normal_atlas, tex0, 0).rgb * 2.0 - 1.0);
		vec3 axis, u, v;
		card_basis(best_k, axis, u, v);
		vec3 n_world = normalize(mat3(s.world_from_local) * (u * n_cam.x + v * n_cam.y + axis * n_cam.z));
		float vis = texelFetch(card_static_atlas, tex0, 0).a;
		r_radiance += albedo * card_dynamic_direct(p_world_hit, n_world) * vis;
	}
	if (params.card_cone_tan < 0.0) {
		// Diagnostics (GODOT_GI_CONE < 0): the level picked, as the radiance.
		r_radiance = vec3(lod / 5.0, card_lookup_footprint, best_texel_world * 10.0);
	}
	pixel_change = max(pixel_change, float((texelFetch(card_change_atlas, ivec2(atlas_texel), 0).y >> 16u) & 0xFFu) / 255.0);
	card_atlas_texel = atlas_texel;
	card_atlas_origin = card_origin_packed(best_packed);
	card_lookup_confidence = best_w * (1.0 - best_mismatch * best_mismatch);
	card_atlas_dims = ivec2(best_dims);
	r_set = inst.set;
	return true;
}

#define SCREEN_TRACE_STEPS 6
#define SCREEN_TRACE_DISTANCE 0.4
#define SCREEN_TRACE_THICKNESS 0.25
#define SCREEN_TRACE_BIAS 0.02

// Short screen-space march for contact occlusion the biased BVH ray misses.
// Returns true and the view-space hit point when the depth buffer blocks the
// first stretch of the ray.
bool screen_trace_hit(vec3 view_origin, vec3 view_normal, vec3 view_dir, float jitter, out vec3 hit_view_pos) {
	// Start off the surface, as the BVH ray does. Starting on it, a ray leaving
	// at a grazing angle stays within the acceptance window of the very surface
	// it left, and the first step reports a hit against it -- which lands in
	// r_hit_distance as a contact at a few centimetres and drives this pixel's
	// visibility term to zero while its neighbour's stays at one.
	view_origin += view_normal * params.ray_bias;
	float trace_dist = SCREEN_TRACE_DISTANCE;
	for (int i = 0; i < SCREEN_TRACE_STEPS; i++) {
		float t = trace_dist * (float(i) + jitter + 0.5) / float(SCREEN_TRACE_STEPS);
		vec3 p = view_origin + view_dir * t;
		vec4 ndc = params.ndc_from_view * vec4(p, 1.0);
		if (ndc.w <= 0.0) {
			return false;
		}
		ndc.xyz /= ndc.w;
		vec2 suv = ndc.xy * 0.5 + 0.5;
		if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
			return false;
		}
		float scene_depth = textureLod(depth_texture, suv, 0.0).r;
		if (scene_depth == 0.0) {
			continue; // Sky.
		}
		vec4 scene_view = params.view_from_ndc * vec4(ndc.xy, scene_depth, 1.0);
		float scene_z = scene_view.z / scene_view.w;
		if (scene_z > p.z + SCREEN_TRACE_BIAS && scene_z < p.z + SCREEN_TRACE_THICKNESS) {
			hit_view_pos = vec3(p.xy, scene_z);
			return true;
		}
	}
	return false;
}

// View-space position of a full-resolution depth texel; the origin (which no
// visible point can be) for the sky.
vec3 view_position_at(ivec2 full_pixel) {
	full_pixel = clamp(full_pixel, ivec2(0), params.full_screen_size - 1);
	float d = texelFetch(depth_texture, full_pixel, 0).r;
	if (d == 0.0) {
		return vec3(0.0);
	}
	vec2 uv = (vec2(full_pixel) + 0.5) / vec2(params.full_screen_size);
	vec4 v = params.view_from_ndc * vec4(uv * 2.0 - 1.0, d, 1.0);
	return v.xyz / v.w;
}

// The geometric normal of the visible surface at a pixel, from the depth
// buffer: the plane through the pixel and its nearest neighbours, taking on
// each axis the neighbour closer in depth so the plane does not straddle a
// silhouette. Faces the camera by construction, which is the side the rays
// have to leave from. Falls back to p_fallback where the neighbourhood is sky.
vec3 geometric_normal(ivec2 full_pixel, vec3 view_pos, vec3 p_fallback) {
	vec3 px = view_position_at(full_pixel + ivec2(1, 0));
	vec3 mx = view_position_at(full_pixel - ivec2(1, 0));
	vec3 py = view_position_at(full_pixel + ivec2(0, 1));
	vec3 my = view_position_at(full_pixel - ivec2(0, 1));
	bool has_px = px != vec3(0.0);
	bool has_mx = mx != vec3(0.0);
	bool has_py = py != vec3(0.0);
	bool has_my = my != vec3(0.0);
	if (!(has_px || has_mx) || !(has_py || has_my)) {
		return p_fallback;
	}
	vec3 dx = (has_px && (!has_mx || abs(px.z - view_pos.z) <= abs(view_pos.z - mx.z))) ? px - view_pos : view_pos - mx;
	vec3 dy = (has_py && (!has_my || abs(py.z - view_pos.z) <= abs(view_pos.z - my.z))) ? py - view_pos : view_pos - my;
	vec3 n = cross(dx, dy);
	float len = length(n);
	if (len < 1e-12) {
		return p_fallback;
	}
	n /= len;
	// The camera is at the view-space origin.
	return dot(n, view_pos) > 0.0 ? -n : n;
}

// The surface's curvature at a pixel from the depth buffer, per metre, the
// larger of the two screen axes, convex only (concave reads as flat). For a
// plane the second difference of the neighbours' positions lies in the
// plane, so its component along the normal is zero exactly; for a convex
// surface the neighbours fall behind the tangent plane by k |dP|^2 / 2 each,
// which is what is read back. Axes that cross a silhouette (a neighbour far
// off in depth) are skipped.
float surface_curvature(ivec2 full_pixel, vec3 view_pos, vec3 geo_normal) {
	float k = 0.0;
	float tolerance = 0.05 * abs(view_pos.z) + 0.02;
	for (int axis = 0; axis < 2; axis++) {
		ivec2 o = axis == 0 ? ivec2(1, 0) : ivec2(0, 1);
		vec3 pa = view_position_at(full_pixel + o);
		vec3 pb = view_position_at(full_pixel - o);
		if (pa == vec3(0.0) || pb == vec3(0.0) || abs(pa.z - view_pos.z) > tolerance || abs(pb.z - view_pos.z) > tolerance) {
			continue;
		}
		vec3 d1 = 0.5 * (pa - pb);
		vec3 d2 = pa - 2.0 * view_pos + pb;
		k = max(k, -dot(d2, geo_normal) / max(dot(d1, d1), 1e-8));
	}
	return k;
}

// Keeps a sample direction on the surface's side of its geometric plane:
// one drawn from a lobe around a normal-mapped shading normal can point into
// the surface, and is folded across the plane rather than traced into it.
vec3 fold_above(vec3 dir, vec3 geo_normal) {
	float below = dot(dir, geo_normal);
	return below < 0.0 ? dir - 2.0 * below * geo_normal : dir;
}

// One gather ray: screen trace, then BVH, cache radiance at the hit, sky on
// miss. Positions are camera-relative world space (the cascade convention).
// r_hit_distance reports how far the ray got (HIT_DISTANCE_MISS when it
// escaped), which the denoiser uses to keep contact GI away from far-field GI.
// world_geo_normal is the geometric normal: the ray origins are pushed off
// the surface along it, and the shading normal, which may lean into the
// surface, has no say in that.
vec3 trace_radiance_chain(vec3 rel_origin, vec3 world_geo_normal, vec3 world_dir, vec3 view_origin, vec3 view_dir, float jitter, out float r_hit_distance) {
	r_hit_distance = params.ao_range; // Nothing hit within range.
	if (bool(params.flags & FLAG_SCREEN_TRACES)) {
		vec3 hit_view;
		vec3 view_normal = transpose(mat3(params.world_from_view)) * world_geo_normal;
		if (screen_trace_hit(view_origin, view_normal, view_dir, jitter, hit_view)) {
			mat3 world_basis = mat3(params.world_from_view);
			vec3 rel_hit = world_basis * hit_view;
			r_hit_distance = length(hit_view - view_origin);
			return screen_radiance_boost(hit_view, sdfgi_cache_radiance(rel_hit, world_dir));
		}
	}

	// Limit rays to the outermost cascade like the probe integrator; without
	// SDFGI (sky-visibility mode) use a fixed generous range.
	float t_max = min(params.z_far * 4.0, 4000.0);
	if (bool(params.flags & FLAG_SDFGI)) {
		uint last = sdfgi.max_cascades - 1u;
		vec3 p = vec3(rel_origin.x, rel_origin.y * sdfgi.y_mult, rel_origin.z);
		vec3 d = vec3(world_dir.x, world_dir.y * sdfgi.y_mult, world_dir.z);
		float d_len = length(d);
		d /= d_len;
		vec3 bounds_min = sdfgi.cascades[last].position;
		vec3 bounds_max = bounds_min + sdfgi.grid_size / sdfgi.cascades[last].to_cell;
		vec3 inv_d = 1.0 / d;
		vec3 bt0 = (bounds_min - p) * inv_d;
		vec3 bt1 = (bounds_max - p) * inv_d;
		vec3 btmax = max(bt0, bt1);
		float t_exit = min(btmax.x, min(btmax.y, btmax.z));
		if (t_exit > 0.0) {
			t_max = t_exit / d_len;
		}
	}

	vec3 origin = rel_origin + world_geo_normal * params.ray_bias;
	// The TLAS lives in absolute world space; positions here are
	// camera-relative, so the query origin adds the camera origin back.
	rayQueryEXT rq;
	// The opaque flag: alpha-tested casters (non-opaque instances) occlude
	// the bounce ray whole. Confirming their hits from the cards' coverage,
	// as the direct pass does, cost 5.5 ms on the game project for a diffuse
	// term that cannot show the holes.
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin + params.world_from_view[3].xyz, params.ray_bias, world_dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		float t_hit = rayQueryGetIntersectionTEXT(rq, true);
		r_hit_distance = t_hit;
		vec3 rel_hit = origin + world_dir * t_hit;
		mat3 view_basis = transpose(mat3(params.world_from_view));
		vec3 view_hit = view_basis * rel_hit;
		if (bool(params.flags & FLAG_SURFACE_CACHE)) {
			uint instance_id = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
			vec3 world_hit = rel_hit + params.world_from_view[3].xyz;
			// The ray's footprint at the hit: the diffuse cone, or the lobe's
			// for the reflection ray (a mirror's is a point).
			card_lookup_footprint = t_hit * (hit_specular ? specular_cone_tan : abs(params.card_cone_tan));
			// The hit goes to its material rather than the cards: for every
			// hit, for the mirror ray's (a card's texel cannot carry the
			// detail a mirror shows), or, the usual case, for a hit the
			// cards cannot shade.
			bool defer_first = bool(params.flags & FLAG_HIT_ALL) || (hit_mirror && bool(params.flags & FLAG_HIT_MIRROR));
			if (!defer_first) {
				vec3 card_radiance;
				uint card_set;
				if (surface_cache_lookup(instance_id, world_hit, world_dir, card_radiance, card_set)) {
					card_requests.frame[card_set] = params.surface_cache_frame;
					cache_tier = CACHE_TIER_CARD;
					return screen_radiance_boost(view_hit, card_radiance);
				}
			}
			if (bool(params.flags & FLAG_HIT_SHADING) && instance_id != SURFACE_CACHE_INVALID) {
				uint geometry_base = card_instances.data[instance_id].geometry_base;
				uint material_base = card_instances.data[instance_id].material_base;
				if (geometry_base != SURFACE_CACHE_INVALID && material_base != SURFACE_CACHE_INVALID) {
					uint geometry_index = rayQueryGetIntersectionGeometryIndexEXT(rq, true);
					uint slot = hit_materials.data[material_base + geometry_index];
					if (slot != RT_HIT_INVALID && bool(params.flags & FLAG_HIT_DEBUG_CONSTANT)) {
						return vec3(0.6);
					}
					if (slot != RT_HIT_INVALID) {
						uint idx = atomicAdd(hit_counts.data[RT_HIT_COUNT_TOTAL], 1u);
						if (idx < params.hit_capacity) {
							atomicAdd(hit_counts.data[slot], 1u);
							uint flags = (rayQueryGetIntersectionFrontFaceEXT(rq, true) ? RT_HIT_PACKET_FRONT_FACE : 0u) | (hit_specular ? RT_HIT_PACKET_MIRROR : 0u);
							uint b = idx * RT_HIT_PACKET_WORDS;
							hit_packets.data[b] = rt_hit_pack_pixel(hit_pixel, hit_slot, flags);
							hit_packets.data[b + 1u] = instance_id;
							hit_packets.data[b + 2u] = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
							hit_packets.data[b + 3u] = (slot & 0xFFFFu) | ((geometry_index & 0xFFu) << 16u);
							hit_packets.data[b + 4u] = packHalf2x16(rayQueryGetIntersectionBarycentricsEXT(rq, true));
							hit_packets.data[b + 5u] = rt_hit_pack_dir(world_dir);
							hit_packets.data[b + 6u] = floatBitsToUint(world_hit.x);
							hit_packets.data[b + 7u] = floatBitsToUint(world_hit.y);
							hit_packets.data[b + 8u] = floatBitsToUint(world_hit.z);
							hit_packets.data[b + 9u] = floatBitsToUint(t_hit);
							hit_results.data[uint(hit_pixel.y * params.screen_size.x + hit_pixel.x) * (params.ray_count + 1u) + hit_slot] = uvec4(0u, 0u, rt_hit_pack_dir(world_dir), RT_HIT_RESULT_PENDING);
							// Nothing now; the resolve adds the material's answer.
							trace_source = TIER_SRC_HIT_SHADED;
							return vec3(0.0);
						} else {
							atomicAdd(hit_counts.data[RT_HIT_COUNT_OVERFLOW], 1u);
						}
					}
				}
			}
			if (defer_first) {
				vec3 card_radiance;
				uint card_set;
				if (surface_cache_lookup(instance_id, world_hit, world_dir, card_radiance, card_set)) {
					card_requests.frame[card_set] = params.surface_cache_frame;
					cache_tier = CACHE_TIER_CARD;
					return screen_radiance_boost(view_hit, card_radiance);
				}
			}
		}
		return screen_radiance_boost(view_hit, sdfgi_cache_radiance(rel_hit, world_dir));
	}
	trace_source = TIER_SRC_SKY;
	return sky_eval(world_dir);
}

// The chain above, with the diagnostics' accounting of which tier answered
// (FLAG_TIER_STATS, off in normal use): a deferred hit or a miss names itself,
// a card, cascade or probe answer is the cache tier the chain left behind,
// and last frame's screen counts as its own source wherever the boost took
// it over. The deferred hits' light is added later by the hit shader, which
// keeps its own counts (RT_HIT_COUNT_TIERS).
vec3 trace_radiance(vec3 rel_origin, vec3 world_geo_normal, vec3 world_dir, vec3 view_origin, vec3 view_dir, float jitter, out float r_hit_distance) {
	trace_source = TIER_SRC_UNSET;
	boost_from_screen = false;
	cache_tier = CACHE_TIER_PROBE;
	vec3 radiance = trace_radiance_chain(rel_origin, world_geo_normal, world_dir, view_origin, view_dir, jitter, r_hit_distance);
	if (bool(params.flags & FLAG_TIER_STATS)) {
		uint src = trace_source;
		if (src == TIER_SRC_UNSET) {
			src = cache_tier == CACHE_TIER_CARD ? TIER_SRC_CARD : (cache_tier == CACHE_TIER_SOLID ? TIER_SRC_CASCADE : TIER_SRC_PROBE);
		}
		if (boost_from_screen && src != TIER_SRC_HIT_SHADED && src != TIER_SRC_SKY) {
			src = TIER_SRC_SCREEN;
		}
		atomicAdd(calibration.tier_count[src], 1u);
		atomicAdd(calibration.tier_lum[src], uint(min(luminance(max(radiance, vec3(0.0))), 64.0) * 16.0));
	}
	return radiance;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	ivec2 full_pixel = min(pixel * int(params.depth_scale), params.full_screen_size - 1);
	float depth = texelFetch(depth_texture, full_pixel, 0).r;
	if (depth == 0.0) {
		imageStore(out_ambient, pixel, vec4(0.0));
		imageStore(out_reflection, pixel, vec4(0.0));
		imageStore(out_spec_ray, pixel, vec4(0.0));
		imageStore(out_view_depth, pixel, vec4(0.0));
		// Sky: unoccluded, no directional bias.
		imageStore(out_directional, pixel, vec4(0.0, 0.0, 0.0, 1.0));
		imageStore(out_fallback, pixel, vec4(0.0));
		return;
	}

	vec2 uv = (vec2(full_pixel) + 0.5) / vec2(params.full_screen_size);
	vec4 view_pos4 = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 view_pos = view_pos4.xyz / view_pos4.w;

	// One pixel in sixteen feeds the calibration sums: plenty for a mean, and
	// the 32-bit fixed-point sums cannot overflow at any screen size in use.
	calibrate_pixel = bool(params.flags & FLAG_CALIBRATE_CACHE) && ((pixel.x | pixel.y) & 3) == 0;

	vec4 nr = texelFetch(normal_roughness_texture, full_pixel, 0);
	vec3 view_normal = normalize(nr.xyz * 2.0 - 1.0);
	// The surface the rays actually leave from. The buffer holds the shading
	// normal, and two things about it can put a ray behind the surface:
	//
	// - The scene shader turns a double-sided material's normal toward the
	//   viewer by winding (gl_FrontFacing), which is wrong for a mesh whose
	//   triangles wind against their vertex normals -- an authoring slip
	//   common in imported assets, and one the raster path barely shows. Here
	//   it is fatal: every ray leaves through the wall, the screen trace's
	//   first step lands on the wall itself, the on-screen radiance read there
	//   is the wall's own last-frame colour, and that loop converges on black.
	// - A normal map tilts the shading normal, and a cosine lobe around a
	//   tilted normal puts part of itself below the real surface. Those rays
	//   meet the wall two march steps in and read its own colour back, and
	//   report a hit distance of centimetres that the near-field visibility
	//   takes for contact occlusion: the wall's bumps come out as bright
	//   self-lit patches ringed by dark bands.
	//
	// So the shading normal is made to agree with the geometric one, and the
	// rays below are kept on the geometric normal's side.
	vec3 geo_view_normal = geometric_normal(full_pixel, view_pos, view_normal);
	if (dot(view_normal, geo_view_normal) < 0.0) {
		view_normal = -view_normal;
	}
	float roughness = nr.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);

	// Camera-relative world space: the cascade data is stored relative to the
	// camera origin, and staying camera-relative preserves precision far from
	// the world origin. The TLAS is absolute, so rays offset by the origin.
	mat3 world_basis = mat3(params.world_from_view);
	vec3 rel_pos = world_basis * view_pos;
	vec3 world_normal = normalize(world_basis * view_normal);
	vec3 world_geo_normal = normalize(world_basis * geo_view_normal);

	vec3 irradiance = vec3(0.0);
	// First moment of the incoming radiance and the near-field visibility,
	// both free from the rays we already trace.
	vec3 moment = vec3(0.0);
	float visibility = 0.0;
	hit_pixel = pixel;
	hit_mirror = false;
	hit_specular = false;
	for (uint r = 0u; r < params.ray_count; r++) {
		vec2 rnd = stbn_sample(pixel, r);
		vec3 dir = fold_above(cosine_hemisphere(world_normal, rnd), world_geo_normal);
		vec3 view_dir = transpose(world_basis) * dir;
		float t_hit;
		hit_slot = r;
		// Clamped non-negative: half-float caches and the screen radiance
		// boost can return a small negative, and the |moment| <= luminance
		// bound the reconstruction relies on only holds for positive radiance.
		vec3 radiance = max(trace_radiance(rel_pos, world_geo_normal, dir, view_pos, view_dir, stbn_sample(pixel, 7u).r, t_hit), vec3(0.0));
		irradiance += radiance;
		moment += luminance(radiance) * dir;
		// Only nearby geometry occludes: in an open scene nearly every ray
		// hits something eventually, and counting those would report near
		// total occlusion everywhere.
		visibility += clamp(t_hit * params.inv_ao_range, 0.0, 1.0);
	}
	float inv_rays = 1.0 / float(params.ray_count);
	irradiance *= inv_rays;
	moment *= inv_rays;
	visibility *= inv_rays;

	// How young this pixel's screen history is (last frame's frame count at
	// its reprojection, none off frame), for the fallback below.
	float prev_frames = 0.0;
	{
		vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, depth, 1.0);
		if (prev_ndc.w > 0.0) {
			vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
			if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
				prev_frames = textureLod(prev_gi_meta, prev_uv, 0.0).r * 64.0;
			}
		}
	}
	vec3 reflection = vec3(0.0);
	// Where the reflected image lives: the virtual point behind the surface,
	// at the hit distance beyond it along the view ray, expressed as a view
	// depth. The temporal filter reprojects the reflection by this rather
	// than by the surface, which is what stops a glossy floor's reflection
	// from smearing as the camera moves. Defaults to the surface itself.
	float virtual_view_depth = -view_pos.z;
	vec4 spec_ray = vec4(0.0);
	// Smooth surfaces get a mirror ray only when the surface cache is there to
	// give the hit a surface at texture resolution; otherwise the rough band
	// alone, with sharp reflections left to SSR / probes, whose sharpness the
	// blurry cache cannot match.
	bool mirror = bool(params.flags & FLAG_MIRROR) && roughness <= 0.2;
	if (bool(params.flags & FLAG_SPECULAR) && (roughness > 0.2 || mirror)) {
		// GGX half-vector sampling around the mirror direction.
		vec2 rnd = stbn_sample(pixel, 6u);
		vec3 v = normalize(-(world_basis * view_pos));
		// (Measured and not kept: narrowing a young pixel's lobe toward the
		// mirror direction by its youth, against the entering band's
		// one-sample sparkle. The sparkle went, but the sharp image it left
		// in the history read 0.034 against 0.028 at the stop of the flick
		// case and was still behind at stop + 16; a rough lobe's blur is
		// what the eye expects there.)
		float alpha = roughness * roughness;
		specular_cone_tan = mirror ? 0.0 : min(2.0 * alpha, params.card_cone_tan);
		float phi = rnd.x * 2.0 * M_PI;
		float ct = sqrt((1.0 - rnd.y) / (1.0 + (alpha * alpha - 1.0) * rnd.y));
		float st = sqrt(max(1.0 - ct * ct, 0.0));
		vec3 h = normalize(basis_around(world_normal) * vec3(st * cos(phi), st * sin(phi), ct));
		vec3 dir = mirror ? reflect(-v, world_normal) : reflect(-v, h);
		if (dot(dir, world_normal) <= 1e-4) {
			dir = reflect(-v, world_normal);
		}
		dir = fold_above(dir, world_geo_normal);
		vec3 view_dir = transpose(world_basis) * dir;
		float spec_t_hit;
		hit_slot = params.ray_count;
		hit_specular = true;
		hit_mirror = mirror;
		reflection = trace_radiance(rel_pos, world_geo_normal, dir, view_pos, view_dir, stbn_sample(pixel, 5u).r, spec_t_hit);
		if (!mirror) {
			// The density of the direction traced (after the folds), GGX
			// over the half vector turned to directions, and the hit's
			// distance, for the resolve.
			vec3 h_final = normalize(v + dir);
			float ndh = max(dot(world_normal, h_final), 0.0);
			float vdh = max(dot(v, h_final), 1e-4);
			float d = ndh * ndh * (alpha * alpha - 1.0) + 1.0;
			float pdf = alpha * alpha / (M_PI * d * d) * ndh / (4.0 * vdh);
			spec_ray = vec4(octahedron_encode(view_dir), max(pdf, 1e-6), min(spec_t_hit, 1e4));
		}
		float view_len = max(length(view_pos), 1e-4);
		// A curved mirror's image is not at the hit distance behind it: a
		// convex surface of curvature k images a point at distance t at
		// t / (1 + 2 k t) -- a pillar of 0.4 m radius images the far wall a
		// fifth of a metre behind its surface, not four metres. Reprojected
		// at the hit distance instead, the temporal filter fetched the
		// pillar's reflection history from where the wall would have
		// reprojected, and every highlight on it doubled and smeared under a
		// dolly (rt_lab temporal_test MOTION=dolly, the pillar was the whole
		// of the diff). Planes read zero curvature and keep the hit distance.
		float curvature = surface_curvature(full_pixel, view_pos, geo_view_normal);
		float t_image = min(spec_t_hit, 1e4);
		t_image /= (1.0 + 2.0 * curvature * t_image);
		virtual_view_depth = -view_pos.z * (1.0 + t_image / view_len);
	}

	// Nothing non-finite leaves the gather: the temporal filter would keep
	// it for the history's lifetime and the spatial filter spread it a
	// stride further every frame (growing black voids). A bad sample counts
	// as no light this frame; the tier print reports how many there were.
	vec4 directional_out = vec4(moment, visibility);
	bool non_finite = any(isnan(irradiance)) || any(isinf(irradiance)) || any(isnan(reflection)) || any(isinf(reflection)) || any(isnan(directional_out)) || any(isinf(directional_out));
	if (non_finite) {
		irradiance = vec3(0.0);
		reflection = vec3(0.0);
		directional_out = vec4(0.0, 0.0, 0.0, 1.0);
		if (bool(params.flags & FLAG_TIER_STATS)) {
			atomicAdd(calibration.tier_count[7], 1u); // Diagnostics (GODOT_GI_TIER_PRINT): the pixels whose gather went non-finite.
		}
	}
	imageStore(out_ambient, pixel, vec4(irradiance, clamp(pixel_change, 0.0, 1.0)));
	imageStore(out_reflection, pixel, vec4(reflection, virtual_view_depth));
	imageStore(out_spec_ray, pixel, spec_ray);
	imageStore(out_view_depth, pixel, vec4(-view_pos.z, 0.0, 0.0, 0.0));
	imageStore(out_directional, pixel, directional_out);

	// The young pixel's stand-in. A fast turn refreshes most of the screen
	// within a few frames, and the entering band is one-sample pixels among
	// one-sample pixels: no screen-space kernel averages that into a
	// picture, and it sparkles for the thirty frames the history takes to
	// converge. The cards under the surface hold the same estimate,
	// accumulated over sixty-four relights (the bounce rays read the same
	// cards these rays do, so the two agree where both are converged;
	// the SDFGI probes, tried first, ran twice as bright and blue, and the
	// screen radiance fed that back into the history). Only the surface's
	// instance is missing, which the G-buffer does not carry: one short
	// primary ray recovers it, spent only where the history is young.
	vec4 fallback = vec4(0.0);
	if (bool(params.flags & FLAG_SURFACE_CACHE) && !bool(params.flags & FLAG_FALLBACK_OFF)) {
		if (prev_frames < FALLBACK_FRAMES || bool(params.flags & (FLAG_FALLBACK_ALL | FLAG_FALLBACK_EVERY))) {
			float view_len = length(rel_pos);
			vec3 eye_dir = rel_pos / max(view_len, 1e-4);
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, params.world_from_view[3].xyz, 0.0, eye_dir, view_len * 1.02);
			while (rayQueryProceedEXT(rq)) {
			}
			if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
				uint instance_id = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
				vec3 world_hit = params.world_from_view[3].xyz + eye_dir * rayQueryGetIntersectionTEXT(rq, true);
				float change_before = pixel_change;
				vec3 card_radiance;
				uint card_set;
				// Looked up along the surface's normal, not the eye ray: the
				// card facing the surface is the one that holds it, and a
				// wall at a grazing angle picked its neighbour's otherwise.
				card_lookup_footprint = 0.0;
				if (surface_cache_lookup(instance_id, world_hit, -world_normal, card_radiance, card_set)) {
					card_requests.frame[card_set] = params.surface_cache_frame;
					// A tent over the card's texels (never across its border):
					// the card is coarse against the screen, and its texels
					// would show as blocks at the fade's full weight. The tent
					// widens the younger the texel's accumulation is (see
					// surface_cache_lookup: a few samples per texel read as a
					// mottle over the whole surface, and the fade shows this
					// read at full weight): a texel relit once is read as a
					// four-by-four grid of bilinear taps four texels apart (a
					// wider grid, eight apart, leaked light across the walls and
					// flickered against the history it fades into), the spacing
					// halving with every doubling of its relights.
					vec4 ind0 = texelFetch(card_indirect_atlas, ivec2(card_atlas_texel), 0);
					float relights = ind0.a * 64.0;
					if (dyn_lights.count > 0u) {
						// And the dynamic histories' age, by their share:
						// the stand-in is then as fresh as the pixel's own
						// samples where a moving light is the light, and
						// fades out like them.
						relights = min(relights, card_dynamic_age(ivec2(card_atlas_texel), ind0.rgb));
					}
					vec2 dims = vec2(card_atlas_dims);
					float spacing = 1.0;
					if (params.card_youth_lod > 0.0 && relights > 0.0) {
						float youth_lod = params.card_youth_lod * (1.0 - log2(max(relights, 1.0)) / 6.0);
						spacing = clamp(exp2(youth_lod - 1.0), 1.0, max(min(dims.x, dims.y) / 8.0, 1.0));
					}
					vec2 t_min = vec2(card_atlas_origin) + 0.5;
					vec2 t_max = vec2(card_atlas_origin + card_atlas_dims) - 0.5;
					vec3 ind = vec3(0.0);
					for (int dy = 0; dy < 4; dy++) {
						for (int dx = 0; dx < 4; dx++) {
							vec2 t = clamp(card_atlas_texel + (vec2(dx, dy) - 1.5) * spacing, t_min, t_max);
							vec2 uv = t / float(params.surface_cache_atlas_size);
							uint parts = params.fallback_parts == 0u ? 7u : params.fallback_parts;
							ind += ((parts & 1u) != 0u ? textureLod(card_indirect_atlas, uv, 0.0).rgb : vec3(0.0)) + ((parts & 2u) != 0u ? max(textureLod(card_indirect_dyn_atlas, uv, 0.0).rgb, vec3(0.0)) : vec3(0.0)) + ((parts & 4u) != 0u ? max(textureLod(card_indirect_dyn2_atlas, uv, 0.0).rgb, vec3(0.0)) : vec3(0.0));
						}
					}
					ind /= 16.0;
					fallback = vec4(max(ind, vec3(0.0)), min(relights, 64.0) / 64.0 * card_lookup_confidence);
				}
				pixel_change = change_before;
			}
		}
	}
	imageStore(out_fallback, pixel, fallback);
}
