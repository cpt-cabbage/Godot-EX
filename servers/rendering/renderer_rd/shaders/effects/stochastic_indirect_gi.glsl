#[compute]

#version 460

#VERSION_DEFINES

// The wavefront split (plan section 94, GODOT_GI_WAVEFRONT): the one kernel
// below is also built as three, the shape the direct pass took in section
// 81. GATHER_SETUP is the kernel up to its rays -- the pixel's surface, its
// ray count and reflection decisions, every ray's direction and screen
// trace -- appending a request for each ray the screen did not answer;
// GATHER_TRACE is a linear dispatch over the requests, the query and
// nothing else; GATHER_RESOLVE is the kernel again from its rays on, the
// hit read from the record the trace kernel left instead of the query. The
// intersector's registers otherwise sit beside the card lookup's (six depth
// fetches, the boost, the change marks, the packet append) for the whole
// kernel. Setup and resolve must not mention the acceleration structure:
// declaring it puts the ray-query capability in the SPIR-V, and the Metal
// container routes on that. What only the single kernel does -- the planar
// mirrors' chains, which interleave card reads with continuation rays, and
// the GODOT_GI_MIRROR knob light's two-leg shadow rays -- keeps it: the
// host runs the split only for a frame without either.
#if defined(GATHER_SETUP) || defined(GATHER_RESOLVE)
#define GATHER_NO_RAYS
#endif
#if defined(GATHER_SETUP) || defined(GATHER_TRACE) || defined(GATHER_RESOLVE)
#define GATHER_SPLIT
#endif

#ifndef GATHER_NO_RAYS
#extension GL_EXT_ray_query : require
#endif
#extension GL_EXT_samplerless_texture_functions : enable
#extension GL_KHR_shader_subgroup_basic : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable
#extension GL_KHR_shader_subgroup_ballot : enable

// Ray-traced indirect lighting ("Lumen-lite" final gather).
// Per pixel: cosine-sampled hemisphere rays traced against the scene BVH,
// shaded at the hit from last frame's screen where the hit is on it, else
// the surface cache's cards, else the deferred material hit shading, with
// planar-mirror chains for mirror hits and the SDFGI/VoxelGI cascades only
// where none of those answer (trace_radiance_chain); the sky on a miss.
// Optionally one GGX-sampled ray feeds a rough specular term. Outputs demodulated irradiance (no albedo) and specular radiance;
// the stochastic denoiser filters both like the direct lighting pair.

#include "../albedo_f0_inc.glsl"
#include "../normal_roughness_inc.glsl"
#include "rt_sample_offset_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"
#include "surface_cache_inc.glsl"

#define SDFGI_MAX_CASCADES 8

#ifndef GATHER_NO_RAYS
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
#endif
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;

#define MAX_MIRROR_PLANES 4u
struct MirrorPlane {
	vec4 plane;
	vec4 params;
	vec4 center;
	vec4 u_axis;
	vec4 v_axis;
};

layout(set = 0, binding = 3, std140) uniform Params {
	mat4 view_from_ndc; // Inverse of the (depth-corrected) projection.
	mat4 ndc_from_view; // The (depth-corrected) projection, for screen traces.
	mat4 world_from_view; // Camera transform.
	mat4 reproject; // Current NDC -> previous frame NDC, for screen radiance.
	ivec2 screen_size;
	ivec2 full_screen_size;
	uint depth_scale; // 2 when sampling at half resolution; bit 8 the block's center sample (rt_sample_offset_inc.glsl).
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
	float card_youth_lod; // The mip a texel relit once is read through (0 disables); half a level less per doubling of its relights at the default 3.0 (the setting over six, see surface_cache_lookup).
	uint fallback_parts; // Diagnostics: which histories the fallback shows (0 all; 1 static, 2 dynamic first bounce, 4 later bounces); bit 3 weights the tent by coverage (GODOT_GI_FALLBACK_COVERAGE=0 clears it).
	float pad_memory;
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
	// y: the firefly ceiling's ratio over the cache value; z: the allowance
	// added above the ceiling (x, w unused).
	vec4 screen_radiance_extra;
	// x: diffuse rays per pixel with a history; y: rays for a pixel whose
	// history is young (under FALLBACK_FRAMES), so the entering band of a
	// turn converges in a few frames instead of thirty. ray_count above is
	// the larger of the two: the hit slots are sized by it (z, w unused).
	uvec4 ray_params;
	vec4 pad_cv;
	// The GODOT_GI_MIRROR knob's own omni light to image (plan section 41;
	// a scene without the stochastic direct pass, the box): xyz world
	// position, w energy.
	vec4 mirror_light;
	vec4 mirror_params; // y the knob light's range, z debug bits (1 the image terms alone, 2 no continuation, 8 GODOT_GI_MIRROR_SPEC=0, 16 GODOT_GI_MIRROR_SCREEN=0); x the stand-in mark (GODOT_GI_STANDIN_MARK, see standin_youth; 0 off), w the frames since a hit pixel's reveal over which it counts as young to it (GODOT_GI_STANDIN_YOUNG).
	MirrorPlane mirrors[MAX_MIRROR_PLANES]; // The scene's planar mirrors (mirror_planes_inc.glsl), world space.
	uint mirror_count;
	uint mirror_order; // The longest image chain evaluated (1: single images, 2: pairs too).
	// A card whose texel is wider than this (meters) is not read by the
	// gather: the hit goes to hit shading (0 disables; see the coarse-card
	// leak in surface_cache_lookup).
	float card_coarse_limit;
	float pad_pick;
	// The coarsest mip a read requests its tile's relight at (3; 0: every
	// request at full density, the form before section 92), and the
	// levels finer than the read's own the request is shifted (a quality
	// dial, GODOT_CARD_LOD_BIAS; negative asks coarser).
	uint card_request_lod;
	int card_request_bias;
	// One read in this many asks at its own level; the rest ask at the
	// coarsest plane, which says only that the tile was read. So a finer
	// plane bit means about this many reads at that level over the tile's
	// turn, and a few near rays among many far ones do not hold a whole
	// tile at full density (GODOT_CARD_LOD_SAMPLE; 1: every read asks at
	// its level, the finest wins).
	uint card_request_sample;
	uint pad_request1;
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
#define FLAG_SPEC_SOURCE_PAINT 524288u // Diagnostics (GODOT_GI_SPEC_SOURCE_PAINT=1): the reflection painted by what answered its ray (see the end of main).
#define FLAG_SRAD_PREV_DEPTH 8388608u // GODOT_GI_SRAD_PREV_DEPTH=0 reverts: a screen read is validated against last frame's depth where it reads last frame's colour.
#define FLAG_SRAD_FOLD 2097152u // The screen texture is the diffuse target: a card hit's read adds the surface's specular energy from the G-buffer (see screen_radiance_boost).
#define FLAG_TIER_STATS 262144u // Diagnostics (GODOT_GI_TIER_PRINT): count which tier answered each ray, and with how much light.
#define FLAG_SPEC_VNDF 1048576u // The reflection ray samples GGX's visible normals (default; GODOT_GI_VNDF=0 reverts to the plain NDF).
#define FLAG_REUSE_RECORD 131072u // Record every ray for the neighbourhood reuse (stochastic_gi_reuse.glsl, GODOT_GI_REUSE).
#define FLAG_NO_REQUESTS 16777216u // Diagnostics (GODOT_GI_REQUESTS=0): the card reads ask for no relight.
#define FLAG_ABLATE_RAYS 33554432u // Diagnostics (GODOT_GI_ABLATE=rays): the bounce rays are not traced (every one misses to the sky).
#define FLAG_ABLATE_SPEC 67108864u // Diagnostics (GODOT_GI_ABLATE=spec): no reflection ray; the diffuse mean stands in for it.
#define FLAG_ABLATE_CARDS 134217728u // Diagnostics (GODOT_GI_ABLATE=cards): no hit reads a card (the probes, or the hit packets).
#define FLAG_DYN_SPLIT 536870912u // The moving lights' term apart (GODOT_GI_DYN_SPLIT, section 88): out_ambient_dyn carries it with its own change mark, the temporal pass accumulates it as a history of its own, and out_ambient's mark is the static lights' alone.
#define FLAG_SPEC_HALF_RATE 268435456u // The rough reflection ray on a checkerboard that alternates each frame; the resolve fills the rest from the traced neighbors (raytraced_gi/quality/half_rate_reflections).
#define FLAG_CARD_MIRROR_FOLD 4194304u // The cards light a planar mirror's texels with their F0 folded back out of the albedo (surface_cache_light.glsl card_diffuse_albedo); a hit's dynamic direct term does the same.

layout(set = 0, binding = 4) uniform sampler2DArray stbn_texture;

// The SDFGI radiance cache: distance fields for hit normals, direct light
// volumes (with anisotropy) for the hits nothing else shades. Bound to
// defaults when inactive.
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
	// Diagnostics (FLAG_TIER_STATS): the reflection rays alone, by what
	// answered them (SPEC_SRC_*).
	uint spec_count[8];
	uint spec_lum[8];
	// Diagnostics (FLAG_TIER_STATS, the mirror path): the continuations'
	// luminance in 1/1024 units, the ones whose lookup found a card (x1024),
	// and their count ([2] unused since the control variate went).
	uint cv_sums[4];
	// Diagnostics (FLAG_TIER_STATS): the screen reads' fold (FLAG_SRAD_FOLD)
	// by the hit pixel's G-buffer metallic (four bins): the reads, and the
	// fold's share of the card summed in 1/1024 units.
	uint fold_count[4];
	uint fold_sum[4];
	uint fold_screen[4]; // The diffuse target's luminance before the fold, 1/1024 units.
	uint fold_card[4]; // The card's, the same.
	uint fold_near[4]; // Reads within a quarter meter of a mirror's rectangle (a hit the mirror path should have taken?).
	uint fold_near_screen[4]; // Their diffuse target and card luminance sums, as above.
	uint fold_near_card[4];
	uint fold_near_spec[4]; // Of the near reads, the reflection rays'.
	// Diagnostics (FLAG_TIER_STATS): why a card lookup failed, by the furthest
	// test any of the set's cards passed (LOOKUP_FAIL_*), and in [7] the lookups.
	uint lookup_fail[8];
	// Diagnostics (FLAG_TIER_STATS): the pixels whose history is under
	// FALLBACK_FRAMES (they trace the stand-in's primary ray), and the pixels.
	uint young_pixels;
	uint pixels;
	uint young_static; // Of them, the pixels whose static history is young (FLAG_DYN_SPLIT; the same count otherwise).
	uint pad_young;
	uint request_lod[4]; // Diagnostics (FLAG_TIER_STATS): the card reads by the level they requested their relight at.
	uint young_cause[16]; // Diagnostics: written by the GI temporal pass (stochastic_denoise.glsl FLAG_CAUSE_STATS, word 90), why its young pixels are young.
}
calibration;

#define LOOKUP_FAIL_NO_RECORD 0u // The hit's instance names no record, or the ablation.
#define LOOKUP_FAIL_NO_SET 1u // The record has no card set.
#define LOOKUP_FAIL_NOT_CAPTURED 2u // The set has no capture yet, or cards under 8 texels.
#define LOOKUP_FAIL_OUTSIDE 3u // No facing card projects the hit inside its box.
#define LOOKUP_FAIL_NO_DEPTH 4u // The card's texel holds no surface.
#define LOOKUP_FAIL_COARSE 5u // The card is coarser than the limit.
#define LOOKUP_FAIL_TOLERANCE 6u // The stored depth is off by more than the tolerance.

#define SPEC_SRC_SCREEN 0u // The screen, whole.
#define SPEC_SRC_PARTIAL 1u // The screen at the border fade, the rest the card.
#define SPEC_SRC_CARD 3u // The card alone (2 was the cards' screen memory).
#define SPEC_SRC_HIT_SHADED 4u
#define SPEC_SRC_SKY 5u
#define SPEC_SRC_OTHER 6u // The cascades or probes, screen-boosted or not.
uint boost_source = SPEC_SRC_OTHER; // What the last screen_radiance_boost answered with.
bool srad_prev_rejected = false; // Diagnostics: the last screen read failed FLAG_SRAD_PREV_DEPTH's test.
uint spec_paint_src = SPEC_SRC_OTHER; // Diagnostics (FLAG_SPEC_SOURCE_PAINT): what answered the reflection ray.
bool boost_fold = true; // screen_radiance_boost adds the fold (FLAG_SRAD_FOLD); the mirror path reads a plane's diffuse without it, its specular being the continuation.

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

layout(set = 0, binding = 20, std430) restrict buffer CardRequests {
	uint frame[SURFACE_CACHE_MAX_SETS]; // Per set: the frame of the last read.
	uint tiles[]; // Per set, per card: the 16x16 tiles read (see surface_cache_inc.glsl).
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
layout(set = 0, binding = 44) uniform sampler2D prev_signal_view_depth; // Last frame's view depth at the signal's pixels (FLAG_SRAD_PREV_DEPTH).
layout(set = 0, binding = 43) uniform sampler2D prev_gi_history_dyn; // The moving lights' history (FLAG_DYN_SPLIT), for the mark its alpha carries.

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
uint spec_hit_id = 0u; // The reflection ray's hit (out_spec_hit), set by trace_radiance_chain.
uint pixel_rays = 1u; // The diffuse rays this pixel traces (ray_params), for the deferred hits' packets.

// Set per pixel in main(): the largest lighting change a ray of this pixel
// landed on. The temporal pass restarts the history in proportion.
float pixel_change = 0.0;
// The moving lights' part of the current ray's radiance (FLAG_DYN_SPLIT):
// their direct term at a card hit and the cards' bounce of it (the two
// things a card cannot hold still while a light moves), which the temporal
// pass accumulates apart from the rest so that a beam sweeping the level
// restarts only its own history. Reset per ray; scaled by what the screen
// took over (screen_radiance_boost); the mark it raises is the cards'
// whole change, where the static history's is the static lights' alone.
vec3 ray_dyn = vec3(0.0);
float pixel_change_dyn = 0.0;

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
layout(set = 1, binding = 6, rgba16f) uniform restrict writeonly image2D out_ambient_dyn; // The moving lights' irradiance and its change mark (FLAG_DYN_SPLIT).
layout(set = 1, binding = 7, rgba16f) uniform restrict writeonly image2D out_fallback_dyn; // The moving lights' share of the young pixel's stand-in (FLAG_DYN_SPLIT; see out_fallback).
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
layout(set = 0, binding = 37) uniform sampler2D decal_atlas_srgb; // The dynamic lights' projector textures (see card_dynamic_direct).
layout(set = 0, binding = 42) uniform sampler2D card_specular_atlas; // The capture's F0 (rgb): what a mirror texel's albedo folded in (FLAG_CARD_MIRROR_FOLD).
// The prepass G-buffer's F0 and diffuse albedo (albedo_f0_inc.glsl), for the
// screen read's fold (FLAG_SRAD_FOLD) and the moving lights' screen term.
layout(set = 0, binding = 40) uniform sampler2D gbuf_f0_texture;
layout(set = 0, binding = 41) uniform sampler2D gbuf_albedo_texture;

layout(set = 0, binding = 35, std430) restrict readonly buffer DynamicLights {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
	vec4 weights[2];
	vec4 prev_color[8]; // Each light's colour (energy in) last frame: what the screen's colour, a frame old, was lit with (see screen_radiance_boost).
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
	// Both bounces come summed in the one (filtered) atlas; its alpha is the
	// age to read it at.
	vec4 dyn2 = texelFetch(card_indirect_dyn2_atlas, tex0, 0);
	vec3 dyn = max(dyn2.rgb, vec3(0.0));
	// A share of two luminances: Rec.709 weights rather than luma_weights,
	// which only reshapes the ratio a little and never its range.
	float dyn_lum = dot(dyn, vec3(0.2126, 0.7152, 0.0722));
	float share = dyn_lum / max(dyn_lum + dot(max(static_bounce, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), 1e-4);
	return dyn2.a * 64.0 / max(share, 0.05);
}

// The dynamic lights' unshadowed direct term at a point (the card lighting's
// light_contribution_world, over pi).
vec3 dyn_light_direct(uint i, vec3 world_pos, vec3 n);

vec3 card_dynamic_direct(vec3 world_pos, vec3 n) {
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		sum += dyn_light_direct(i, world_pos, n);
	}
	return sum;
}

// The same with each light as it was last frame (its colour and energy;
// the position is this frame's), for what a frame-old screen colour holds.
vec3 dyn_light_direct_color(uint i, vec3 world_pos, vec3 n, vec3 color);
vec3 card_dynamic_direct_prev(vec3 world_pos, vec3 n) {
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		sum += dyn_light_direct_color(i, world_pos, n, dyn_lights.prev_color[i].rgb);
	}
	return sum;
}

// One dynamic light's unshadowed direct term at a point, times its weight.
vec3 dyn_light_direct(uint i, vec3 world_pos, vec3 n) {
	return dyn_light_direct_color(i, world_pos, n, dyn_lights.data[i].color);
}

vec3 dyn_light_direct_color(uint i, vec3 world_pos, vec3 n, vec3 p_color) {
	{
		LightData ld = dyn_lights.data[i];
		vec3 rel = ld.position - world_pos;
		float len = length(rel);
		float attenuation = card_omni_attenuation(len, ld.inv_radius, ld.attenuation);
		vec3 l = rel / max(len, 1e-5);
		bool is_spot = ld.pad > 0.5;
		if (is_spot) {
			float scos = max(dot(-l, normalize(ld.direction)), ld.cone_angle);
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
			attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
		}
		float geom = max(dot(n, l), 0.0) * attenuation;
		if (geom <= 0.0) {
			return vec3(0.0);
		}
		vec3 color = p_color;
		// The projector texture, as the card lighting reads it
		// (surface_cache_light.glsl projector_factor): a spot's cookie
		// through the cards' world-space projector matrix, an omni's map
		// through the dual paraboloid.
		if (ld.projector_rect != vec4(0.0)) {
			vec4 proj;
			if (is_spot) {
				vec4 splane = ld.shadow_matrix * vec4(world_pos, 1.0);
				splane /= splane.w;
				proj = textureLod(decal_atlas_srgb, splane.xy * ld.projector_rect.zw + ld.projector_rect.xy, 0.0);
			} else {
				vec3 local_v = normalize((ld.shadow_matrix * vec4(world_pos, 1.0)).xyz);
				vec4 atlas_rect = ld.projector_rect;
				if (local_v.z >= 0.0) {
					atlas_rect.y += atlas_rect.w;
				}
				local_v.z = 1.0 + abs(local_v.z);
				local_v.xy /= local_v.z;
				local_v.xy = local_v.xy * 0.5 + 0.5;
				proj = textureLod(decal_atlas_srgb, local_v.xy * atlas_rect.zw + atlas_rect.xy, 0.0);
			}
			color *= proj.rgb * proj.a;
		}
		return color * (geom * (1.0 / 3.14159265359)) * dyn_lights.weights[i >> 2u][i & 3u];
	}
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
// What the reflection ray hit, for the temporal pass to follow a moving
// object's image in a mirror (its FLAG_SPEC_OBJECTS): a card set, by the TLAS
// instance the ray query committed; a screen position, where the screen trace
// answered (the prepass motion vector there says whether that point moved);
// or nothing (the sky, a probe, a chain through a planar mirror, whose image
// is of whatever the continuation met).
layout(set = 1, binding = 12, r32ui) uniform restrict writeonly uimage2D out_spec_hit;
#define SPEC_HIT_SET (1u << 30u) // | the card set.
#define SPEC_HIT_SCREEN (2u << 30u) // | the hit's screen uv, 15 bits each (y << 15 | x) over 32767.
// History frames under which the gather spends the primary ray on it.
#define FALLBACK_FRAMES 8.0

#if !defined(GATHER_SETUP) && !defined(GATHER_TRACE)
// Every ray this pixel traced, for the neighbourhood reuse
// (FLAG_REUSE_RECORD; stochastic_gi_reuse.glsl reads them, the hit shading's
// resolve fills in the deferred hits' radiance). One per ray slot, pixel
// index * (ray_count + 1) + slot as the hit results number them: the
// radiance as halves (x: r g, y: b and the hit distance), z the view-space
// direction, w the flags and the moving lights' share (unorm 8 bits << 8).
layout(set = 1, binding = 13, std430) restrict writeonly buffer GiReuseRays {
	uvec4 data[];
}
reuse_rays;
#define GI_REUSE_RAY_VALID 1u
#define GI_REUSE_RAY_SPEC 2u // The reflection ray, traced (a valid reflection slot without it: skipped by the half-rate checkerboard).
#define GI_REUSE_RAY_MIRROR 4u
#define GI_REUSE_RAY_BELOW 8u // The GGX draw fell below the horizon; the mirror direction was traced in its place.
#define GI_REUSE_RAY_COUNT_SHIFT 4u // The pixel's diffuse rays - 1.
#define GI_REUSE_RAY_DYN_SHIFT 8u

uvec4 gi_reuse_record(vec3 radiance, vec3 view_dir, float t_hit, uint flags) {
	return uvec4(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, min(t_hit, 65000.0))), rt_hit_pack_dir(view_dir), flags);
}
#endif

#ifdef GATHER_SPLIT
// The split's hand-over. Every pixel has GATHER_SLOTS ray records at fixed
// places (pixel index * slots + slot): the diffuse rays at 0 .. ray_count -
// 1, the reflection ray at ray_count (as hit_slot numbers them), the
// fallback's eye ray at ray_count + 1. A record is two uvec4: the first the
// answer -- flags and geometry index << 8, then t, the instance's custom
// index and the primitive index for a traced ray, or the view-space point
// for a screen-trace hit -- the second the direction traced and, in w, the
// barycentrics as halves. The setup kernel writes the direction (and the
// whole of a screen hit's record) and appends a request for each ray the
// BVH must answer: the absolute origin and t_max, the direction and the
// record's index, so the trace kernel reads nothing but its request. The
// count and the dispatch's group count are two buffers: the trace kernel is
// dispatched indirectly from the second, which a compute list cannot also
// bind as storage (it gets a dummy there).
layout(set = 1, binding = 8, std430) restrict buffer GatherCount {
	uint count;
}
gather_count;
layout(set = 1, binding = 9, std430) restrict buffer GatherArgs {
	uvec4 groups; // x: (count + 63) / 64, y and z 1.
}
gather_args;
layout(set = 1, binding = 10, std430) restrict buffer GatherRequests {
	uvec4 data[]; // Two per request: origin xyz and t_max; direction xyz and the record index.
}
gather_requests;
layout(set = 1, binding = 11, std430) restrict buffer GatherRecords {
	uvec4 data[]; // Two per ray slot (see above).
}
gather_records;

#define GATHER_SLOTS (params.ray_count + 2u)
#define GATHER_RECORD_HIT 1u
#define GATHER_RECORD_FRONT_FACE 2u
#define GATHER_RECORD_SCREEN_HIT 4u
#endif

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

#include "mirror_planes_inc.glsl"

#ifndef GATHER_NO_RAYS
// A shadow segment: anything opaque between the two points.
bool mirror_occluded(vec3 from_world, vec3 to_world) {
	vec3 d = to_world - from_world;
	float len = length(d);
	if (len < 1e-4) {
		return false;
	}
	rayQueryEXT sq;
	rayQueryInitializeEXT(sq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, from_world, 0.0, d / len, len);
	while (rayQueryProceedEXT(sq)) {
	}
	return rayQueryGetIntersectionTypeEXT(sq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}
#endif

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
		// nothing: filtered against empty neighbors every tap came back
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

// The cards' screen memory -- a texel remembering what the settled screen
// showed over the card, read where a hit cannot read the screen (section
// 34) -- was measured as a fix for the reflection's fade-in after a camera
// move and a wash on the flashlight cases; off since, removed 2026-09-22.
// The share of this pixel's card reads that landed on a screen pixel
// revealed under mirror_params.w frames ago (the edge's stand-in is
// permanent and not counted), summed over the reads, and their count. After
// a camera flick every history restarts from dark samples -- the screen
// the rays read is itself a running mean of such -- and the right-hand
// furniture of pose E, a quarter of whose light is the screen term, read
// 18% dark at the stop and climbed back over fifty frames, the early
// samples staying in the mean (section 56 B). With mirror_params.x > 0
// the pixel raises a change mark of that times the share (out_ambient's
// alpha, the mark a lighting change raises), so its history stays short
// while the screen it reads is young and grows once that has settled
// (section 60). The mark keyed on the hit pixel's frame count instead
// fed itself: the count is what marks shorten.
float standin_youth = 0.0;
float standin_reads = 0.0;

// The atlas position the last successful card lookup read (bilinear, in
// texels), and the card it lies in (surface_cache_lookup sets them).
vec2 card_atlas_texel = vec2(0.0);
ivec2 card_atlas_origin = ivec2(0);
ivec2 card_atlas_dims = ivec2(1);

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
	boost_source = tier == CACHE_TIER_CARD ? SPEC_SRC_CARD : SPEC_SRC_OTHER; // Diagnostics; the reads below refine it.
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
	// FLAG_SRAD_PREV_DEPTH: the colour is last frame's, so the
	// surface it shows is last frame's too. The test above is this frame's
	// depth: where a moving object stood last frame and has since left, the
	// hit's surface is visible now and passes, and the read returns the
	// object. Behind the lab's gray box sweeping along the wall the floor's
	// reflection of the wall's foot read the box's gray where it had been, a
	// comb of streaks along the floor's edge (0.026 against the converged
	// image there; the cards alone, screen reads off, 0.003). So the read is
	// held to last frame's depth at the pixel it reads, the signal's (one
	// sample of its block: an edge falls back to the card).
	if (bool(params.flags & FLAG_SRAD_PREV_DEPTH)) {
		ivec2 prev_size = textureSize(prev_signal_view_depth, 0);
		float prev_depth = texelFetch(prev_signal_view_depth, clamp(ivec2(prev_uv * vec2(prev_size)), ivec2(0), prev_size - 1), 0).r;
		vec4 pv = params.view_from_ndc * vec4(0.0, 0.0, prev_ndc.z / prev_ndc.w, 1.0);
		float expected = -pv.z / pv.w;
		if (prev_depth <= 0.0 || abs(prev_depth - expected) > max(expected * 0.05, 0.05)) {
			srad_prev_rejected = true;
			return cache_radiance;
		}
	}
	vec3 col = textureLod(screen_radiance_texture, prev_uv, 0.0).rgb;
	if (bool(params.flags & FLAG_SRAD_FOLD) && boost_fold && tier == CACHE_TIER_CARD) {
		// The diffuse target has no specular at all, and a rendered pixel's
		// specular is the camera's anyway (Fresnel-boosted at the grazing
		// angles the camera sees a ceiling at, and read here by rays from
		// every other direction: the screen read 1.45-1.57x the card at the
		// same points in the game, 1.05x in the radiosity box, and 0.98-1.03x
		// as the diffuse target). What a ray from elsewhere wants of the
		// surface's specular is, over the rays' directions, its energy: the
		// hemispherical specular albedo (Schlick's average, F0 + (f90 - F0)
		// / 21, f90 the scene shader's clamp(50 F0.g, metallic, 1): a
		// material without a specular lobe folds nothing, the Lambert box
		// read 1.02-1.05 of its truth with the plain 1 / 21) over the
		// surface's total albedo, taken of the card's radiance
		// (the card holds the F0 fold as Lambert, which is that energy) --
		// from the card rather than the diffuse colour so a metal, whose
		// diffuse is nothing, reads its card whole.
		ivec2 gb_pixel = ivec2(uv * vec2(params.full_screen_size));
		vec4 gb_a = texelFetch(gbuf_albedo_texture, gb_pixel, 0);
		if (!gb_unshaded(gb_a)) {
			vec4 gb_f = texelFetch(gbuf_f0_texture, gb_pixel, 0);
			vec3 f0 = gb_f0(gb_f);
			float f90 = clamp(50.0 * f0.g, gb_metallic(gb_f), 1.0);
			float f_avg = luminance(f0 + (vec3(f90) - f0) / 21.0);
			float a_lum = luminance(gb_albedo(gb_a));
			float fold = f_avg / max(a_lum + f_avg, 1e-4);
			if (bool(params.flags & FLAG_TIER_STATS)) {
				uint bin = uint(gb_metallic(gb_f) * 3.0 + 0.5);
				atomicAdd(calibration.fold_count[bin], 1u);
				atomicAdd(calibration.fold_sum[bin], uint(fold * 1024.0));
				atomicAdd(calibration.fold_screen[bin], uint(min(luminance(col), 64.0) * 1024.0));
				atomicAdd(calibration.fold_card[bin], uint(min(luminance(cache_radiance), 64.0) * 1024.0));
				vec3 world_hit = params.world_from_view[3].xyz + mat3(params.world_from_view) * view_hit;
				for (uint mi = 0u; mi < mirror_count(); mi++) {
					if (abs(mirror_height(mi, world_hit)) < 0.25 && mirror_in_rect(mi, world_hit)) {
						atomicAdd(calibration.fold_near[bin], 1u);
						atomicAdd(calibration.fold_near_screen[bin], uint(min(luminance(col), 64.0) * 1024.0));
						atomicAdd(calibration.fold_near_card[bin], uint(min(luminance(cache_radiance), 64.0) * 1024.0));
						if (hit_specular) {
							atomicAdd(calibration.fold_near_spec[bin], 1u);
						}
						break;
					}
				}
			}
			col += cache_radiance * fold;
		}
	}
	// Weaker by a quarter per bounce: two pixels whose rays keep landing on
	// each other would otherwise hand the mark back and forth forever.
	float hit_mark = textureLod(prev_gi_history, prev_uv, 0.0).a;
	pixel_change = max(pixel_change, hit_mark - 0.25);
	// The moving lights' part of the screen's colour (FLAG_DYN_SPLIT): their
	// direct term on the hit pixel, estimated from the G-buffer's albedo and
	// normal and the lights' current state (unshadowed: in their shadow it
	// hands the moving history more than its due, which is bounded by the
	// colour itself), and the hit pixel's own history of them. Landed whole
	// in the static history, the lamp of rt_lab's colour case turned and
	// the ceiling, lit by the floor through these reads, held the old
	// colour for the static window (section 88). Each history's mark
	// reaches this pixel through its own reads, weakened by the quarter.
	vec3 dyn_screen = vec3(0.0);
	if (bool(params.flags & FLAG_DYN_SPLIT)) {
		vec4 hit_dyn = textureLod(prev_gi_history_dyn, prev_uv, 0.0);
		pixel_change_dyn = max(pixel_change_dyn, hit_dyn.a - 0.25);
		ivec2 gb_pixel = ivec2(uv * vec2(params.full_screen_size));
		vec3 albedo_hit = gb_albedo(texelFetch(gbuf_albedo_texture, gb_pixel, 0));
		dyn_screen = albedo_hit * max(hit_dyn.rgb, vec3(0.0)); // The history is irradiance; the screen has the albedo in.
		if (dyn_lights.count > 0u) {
			vec3 n_hit = normalize(mat3(params.world_from_view) * nr_normal(texelFetch(normal_roughness_texture, gb_pixel, 0)));
			vec3 world_hit = params.world_from_view[3].xyz + mat3(params.world_from_view) * view_hit;
			// The lights as they were last frame: the screen is a frame old
			// (a lamp switched off this frame still lights it).
			dyn_screen += albedo_hit * card_dynamic_direct_prev(world_hit, n_hit);
		}
		dyn_screen = min(dyn_screen, max(col, vec3(0.0)));
	}
	// Measured and not kept (section 33): the screen term faded in with the
	// hit pixel's own history, the card standing in until then (a quarter
	// off a flick's flash, nothing off the settle).
	float l = luminance(col);
	// Both tiers for the same point: what the calibration is made of. The raw
	// cache value, not the scaled one, or the estimate would chase itself.
	// Sampled before the firefly ceiling, which is keyed to the very scale
	// being estimated. Hits at the frame border are left out along with the
	// rest of what the hand-back below distrusts.
	if (calibrate_pixel && tier < 2u) {
		uint slot = tier;
		vec2 border_c = min(min(uv, vec2(1.0) - uv), min(prev_uv, vec2(1.0) - prev_uv));
		if (min(border_c.x, border_c.y) >= params.screen_radiance_border_fade) {
			atomicAdd(calibration.sum_screen[slot], uint(min(l, 64.0) * 1024.0));
			atomicAdd(calibration.sum_cache[slot], uint(min(luminance(raw_cache_radiance), 64.0) * 1024.0));
			atomicAdd(calibration.samples[slot], 1u);
		}
	}
	// Firefly ceiling. Keying this to the cache alone closes a loop: the gather
	// writes the buffer this reads, so where the cache is dim the ceiling caps
	// the screen term below the light actually in the room, the image dims, and
	// the next frame reads the dimmer image -- a wall lit only by bounce
	// ratchets down to the cache value over the accumulation window. The
	// absolute term is what the cache cannot drag down.
	float clamp_l = max(luminance(cache_radiance) * params.screen_radiance_extra.y, params.screen_radiance_clamp) + params.screen_radiance_extra.z;
	if (l > clamp_l) {
		col *= clamp_l / l;
	}
	// Both lookups have to be well inside for the sample to be trustworthy:
	// uv carries the depth test, prev_uv the colour fetch. Whichever is nearer
	// an edge decides how much of the boost survives.
	vec2 border = min(min(uv, vec2(1.0) - uv), min(prev_uv, vec2(1.0) - prev_uv));
	// A fade of 0 collapses the smoothstep back to the hard switch at the edge.
	float border_share = smoothstep(0.0, max(params.screen_radiance_border_fade, 1e-5), min(border.x, border.y));
	float screen_share = border_share;
	boost_from_screen = screen_share > 0.5;
	if (tier == CACHE_TIER_CARD && params.mirror_params.x > 0.0) {
		// The hit pixel revealed under mirror_params.w frames ago (the meta's
		// alpha, stochastic_denoise.glsl REVEAL_STEP): the colour read is
		// a running mean still climbing from its restart. The age, not the
		// frame count: the count is what the mark shortens, and keyed on
		// it the pixels kept each other young for ever (measured: the
		// furniture 12% dark at rest).
		vec4 hit_meta_age = textureLod(prev_gi_meta, prev_uv, 0.0);
		float age = (1.0 - hit_meta_age.a) * (255.0 / 4.0);
		standin_youth += border_share * (1.0 - min(age / params.mirror_params.w, 1.0));
		standin_reads += 1.0;
	}
	if (screen_share > 0.0 && tier == CACHE_TIER_CARD) {
		boost_source = screen_share >= 0.999 ? SPEC_SRC_SCREEN : SPEC_SRC_PARTIAL;
	}
	ray_dyn = mix(ray_dyn, dyn_screen, screen_share);
	return mix(cache_radiance, col, screen_share);
}

// Radiance from the surface cache at a hit, if a captured card covers it.
// The hit goes into the instance's local space and onto each of the six
// cards; a card counts where the hit lies inside its frame, faces the ray
// (a card only saw surfaces facing its own axis) and stored a depth within
// a texel or two of the hit's, which is what keeps a hit on one wall from
// reading the card of the wall behind it. Among the valid cards the one
// facing the ray most squarely wins.

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
	const bool fail_stats = bool(params.flags & FLAG_TIER_STATS);
	if (fail_stats) {
		// Every ray counts here: the group's lanes add up before one atomic
		// (a million atomics on one word a frame at half resolution otherwise).
		uint n = subgroupAdd(1u);
		if (subgroupElect()) {
			atomicAdd(calibration.lookup_fail[7], n);
		}
	}
	if (p_instance_id == SURFACE_CACHE_INVALID || bool(params.flags & FLAG_ABLATE_CARDS)) {
		if (fail_stats) {
			atomicAdd(calibration.lookup_fail[LOOKUP_FAIL_NO_RECORD], 1u);
		}
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		if (fail_stats) {
			atomicAdd(calibration.lookup_fail[LOOKUP_FAIL_NO_SET], 1u);
		}
		return false;
	}
	CardSet s = card_sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 8.0) {
		if (fail_stats) {
			atomicAdd(calibration.lookup_fail[LOOKUP_FAIL_NOT_CAPTURED], 1u);
		}
		return false;
	}
	uint fail_stage = LOOKUP_FAIL_OUTSIDE;
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
		fail_stage = max(fail_stage, LOOKUP_FAIL_NO_DEPTH);
		if (stored <= 0.0) {
			continue;
		}
		fail_stage = max(fail_stage, LOOKUP_FAIL_COARSE);
		// Two texels of the card's own footprint, or a slice of the box.
		// The box's longest extent over the card's longer edge, as when the
		// cards were square: a card's own (shorter) texel made the tolerance
		// reject grazing hits that then paid for the probe fallback.
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		// A coarse card of an open module (a lattice ceiling at a 25 cm
		// texel, plan section 79) stores whatever won the depth test
		// through its bars, and the tolerance below, sized to the module,
		// cannot tell a hit on a bar from the surface behind it: the hit
		// reads the lit surface through the bar. Past the limit the hit is
		// shaded exactly instead (the gather's hit packets; the cards' own
		// bounce rays have no such path and keep reading).
		// The limit in meters, or negative: that many times the ray's own
		// footprint at the hit (a near hit resolves finer than a coarse
		// card, a far one no better than it).
		if (params.card_coarse_limit > 0.0 && texel_world > params.card_coarse_limit) {
			continue;
		}
		if (params.card_coarse_limit < 0.0 && card_lookup_footprint > 0.0 && texel_world > -params.card_coarse_limit * card_lookup_footprint) {
			continue;
		}
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		fail_stage = max(fail_stage, LOOKUP_FAIL_TOLERANCE);
		if (abs(stored - depth) > tolerance) {
			continue;
		}
		// Among the valid cards the one facing the ray most squarely wins.
		// A pick weighing the depth mismatch against the facing moved
		// nothing on the coarse-card leak it was built for (section 82).
		if (facing > best_w) {
			best_w = facing;
			best_uv = uv01;
			best_packed = packed;
			best_k = k;
			best_mismatch = abs(stored - depth) / tolerance;
		}
	}
	if (best_w <= 0.0) {
		if (fail_stats) {
			atomicAdd(calibration.lookup_fail[fail_stage], 1u);
		}
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
	// The read is the request: the texel's tile is relit next frame, at the
	// level the footprint read it through (the youth level below is the
	// read's own smoothing, not the resolution the ray wants). A far read
	// then costs the cards a sixteenth of the tile, or less.
	if (!bool(params.flags & FLAG_NO_REQUESTS)) {
		ivec2 dims = ivec2(best_dims);
		ivec2 texel = card_origin_packed(best_packed) + clamp(ivec2(best_uv * best_dims), ivec2(0), dims - ivec2(1));
		uint bit;
		uint word = card_tile_word(inst.set, best_k, best_packed, texel, bit);
		uint max_plane = min(params.card_request_lod, SURFACE_CACHE_LOD_PLANES - 1u);
		uint plane = uint(clamp(int(lod) - params.card_request_bias, 0, int(max_plane)));
		if (plane < max_plane && params.card_request_sample > 1u && (pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame_index + gl_GlobalInvocationID.x * 7919u + gl_GlobalInvocationID.y * 104729u))) % params.card_request_sample) != 0u) {
			plane = max_plane;
		}
		atomicOr(card_requests.tiles[plane * SURFACE_CACHE_PLANE_WORDS + word], bit);
		card_requests.frame[inst.set] = params.surface_cache_frame;
		if (fail_stats) {
			atomicAdd(calibration.request_lod[min(uint(lod), 3u)], 1u);
		}
	}
	// A young texel is read through a coarser level. The bounce a texel
	// accumulates restarts when the light on it changes (the temporal
	// gradient), and for the relights after that it is one or a few samples
	// -- every ray landing near it reads the same sample, so the noise is
	// not per pixel but a mottle over the whole surface that no screen-space
	// filter averages (a flashlight sweeping the room left the ceiling and
	// walls blotched for the cards' whole window). The level halves the
	// noise per step: a texel relit once reads eight by eight of its
	// neighbors, and the level falls half a step per doubling of its
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
		if ((params.flags & FLAG_CARD_MIRROR_FOLD) != 0u && mirror_at(p_world_hit) < MAX_MIRROR_PLANES) {
			// The same diffuse albedo the cards lit the texel with (the
			// capture folded F0 in as Lambertian; the mirror's reflection
			// is the image lights' and the mirror path's): with the folded
			// one here, a lamp's bounce off a mirror floor read diffuse
			// while the lamp was dynamic and darkened over the fade.
			albedo = max(albedo - texelFetch(card_specular_atlas, tex0, 0).rgb, vec3(0.0));
		}
		vec3 n_cam = normalize(texelFetch(card_normal_atlas, tex0, 0).rgb * 2.0 - 1.0);
		vec3 axis, u, v;
		card_basis(best_k, axis, u, v);
		vec3 n_world = normalize(mat3(s.world_from_local) * (u * n_cam.x + v * n_cam.y + axis * n_cam.z));
		float vis = texelFetch(card_static_atlas, tex0, 0).a;
		vec3 dyn_direct = albedo * card_dynamic_direct(p_world_hit, n_world) * vis;
		r_radiance += dyn_direct;
		if (bool(params.flags & FLAG_DYN_SPLIT)) {
			// The moving lights' part of what this read returns: their
			// direct term above and the cards' bounce of them, which the
			// lighting atlas holds summed with the rest (read here at the
			// texel, the atlas at its level: the bounce is filtered over
			// the card and reads nearly the same either way).
			ray_dyn = min(dyn_direct + albedo * max(texelFetch(card_indirect_dyn_atlas, tex0, 0).rgb, vec3(0.0)), max(r_radiance, vec3(0.0)));
		}
	}
	if (params.card_cone_tan < 0.0) {
		// Diagnostics (GODOT_GI_CONE < 0): the level picked, as the radiance.
		r_radiance = vec3(lod / 5.0, card_lookup_footprint, best_texel_world * 10.0);
	}
	uint change_packed = texelFetch(card_change_atlas, ivec2(atlas_texel), 0).y;
	float texel_change = float((change_packed >> 16u) & 0xFFu) / 255.0;
	if (bool(params.flags & FLAG_DYN_SPLIT)) {
		// The static history takes the static lights' change (the byte the
		// cards' own bounce accumulation restarts by), the moving lights'
		// history the whole of it.
		pixel_change = max(pixel_change, float((change_packed >> 24u) & 0xFFu) / 255.0);
		pixel_change_dyn = max(pixel_change_dyn, texel_change);
	} else {
		pixel_change = max(pixel_change, texel_change);
	}
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
	// r_hit_distance as a contact at a few centimeters and drives this pixel's
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
// buffer: the plane through the pixel and its nearest neighbors, taking on
// each axis the neighbor closer in depth so the plane does not straddle a
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

// The surface's curvature at a pixel from the depth buffer, per meter, the
// larger of the two screen axes, convex only (concave reads as flat). For a
// plane the second difference of the neighbours' positions lies in the
// plane, so its component along the normal is zero exactly; for a convex
// surface the neighbors fall behind the tangent plane by k |dP|^2 / 2 each,
// which is what is read back. Axes that cross a silhouette (a neighbor far
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

// The BVH ray's range: the outermost cascade like the probe integrator;
// without SDFGI (sky-visibility mode) a fixed generous range.
float gather_t_max(vec3 rel_origin, vec3 world_dir) {
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
	return t_max;
}

// The ray's hit, from the query in the single kernel or from the record the
// trace kernel left in the resolve (rec, the record's first uvec4, loaded
// by trace_radiance_chain). Read where they are used, as the query's
// accessors were: the single kernel keeps its form.
#ifdef GATHER_RESOLVE
#define GATHER_HIT_COMMITTED ((rec.x & GATHER_RECORD_HIT) != 0u)
#define GATHER_HIT_T uintBitsToFloat(rec.y)
#define GATHER_HIT_INSTANCE rec.z
#define GATHER_HIT_PRIMITIVE rec.w
#define GATHER_HIT_GEOMETRY (rec.x >> 8u)
#define GATHER_HIT_FRONT_FACE ((rec.x & GATHER_RECORD_FRONT_FACE) != 0u)
#define GATHER_HIT_BARYCENTRICS_PACKED gather_records.data[gather_record * 2u + 1u].w
#else
#define GATHER_HIT_COMMITTED (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
#define GATHER_HIT_T rayQueryGetIntersectionTEXT(rq, true)
#define GATHER_HIT_INSTANCE rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)
#define GATHER_HIT_PRIMITIVE rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)
#define GATHER_HIT_GEOMETRY rayQueryGetIntersectionGeometryIndexEXT(rq, true)
#define GATHER_HIT_FRONT_FACE rayQueryGetIntersectionFrontFaceEXT(rq, true)
#define GATHER_HIT_BARYCENTRICS_PACKED packHalf2x16(rayQueryGetIntersectionBarycentricsEXT(rq, true))
#endif

#ifdef GATHER_RESOLVE
uint gather_record = 0u; // The current ray's record (set per ray in main).
#endif

// The setup kernel shades nothing (and has no query to fall back on).
#ifndef GATHER_SETUP
// One gather ray: screen trace, then BVH, cache radiance at the hit, sky on
// miss. Positions are camera-relative world space (the cascade convention).
// r_hit_distance reports how far the ray got (HIT_DISTANCE_MISS when it
// escaped), which the denoiser uses to keep contact GI away from far-field GI.
// world_geo_normal is the geometric normal: the ray origins are pushed off
// the surface along it, and the shading normal, which may lean into the
// surface, has no say in that.
vec3 trace_radiance_chain(vec3 rel_origin, vec3 world_geo_normal, vec3 world_dir, vec3 view_origin, vec3 view_dir, float jitter, out float r_hit_distance) {
	r_hit_distance = params.ao_range; // Nothing hit within range.
	bool screen_hit = false;
	vec3 hit_view;
#ifdef GATHER_RESOLVE
	uvec4 rec = gather_records.data[gather_record * 2u];
	screen_hit = (rec.x & GATHER_RECORD_SCREEN_HIT) != 0u;
	hit_view = uintBitsToFloat(rec.yzw);
#else
	if (bool(params.flags & FLAG_SCREEN_TRACES)) {
		vec3 view_normal = transpose(mat3(params.world_from_view)) * world_geo_normal;
		screen_hit = screen_trace_hit(view_origin, view_normal, view_dir, jitter, hit_view);
	}
#endif
	if (screen_hit) {
		mat3 world_basis = mat3(params.world_from_view);
		vec3 rel_hit = world_basis * hit_view;
		r_hit_distance = length(hit_view - view_origin);
		if (hit_specular) {
			vec4 hit_ndc = params.ndc_from_view * vec4(hit_view, 1.0);
			uvec2 q = uvec2(clamp((hit_ndc.xy / hit_ndc.w) * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * 32767.0 + 0.5);
			spec_hit_id = SPEC_HIT_SCREEN | (q.y << 15u) | q.x;
		}
		return screen_radiance_boost(hit_view, sdfgi_cache_radiance(rel_hit, world_dir));
	}

	vec3 origin = rel_origin + world_geo_normal * params.ray_bias;
#ifndef GATHER_RESOLVE
	float t_max = gather_t_max(rel_origin, world_dir);
	// The TLAS lives in absolute world space; positions here are
	// camera-relative, so the query origin adds the camera origin back.
	rayQueryEXT rq;
	// The opaque flag: alpha-tested casters (non-opaque instances) occlude
	// the bounce ray whole. Confirming their hits from the cards' coverage,
	// as the direct pass does, cost 5.5 ms on the game project for a diffuse
	// term that cannot show the holes. A hit in a hole -- the capture leaves
	// an alpha-tested material's texels under half alpha unfilled: a gap in
	// a leaf, a mesh whose material draws nothing (a room's invisible dome
	// carried a flashlight's lit cap from inside to the walls outside while
	// its texels were captured and lit) -- finds no card and takes the
	// cache tier like any other hit without one. Going on through the hole
	// was measured: one ray in a hundred, and 0.5-0.8 ms on this
	// register-bound pass for the code alone, as a loop or as a second
	// trace.
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin + params.world_from_view[3].xyz, params.ray_bias, world_dir, bool(params.flags & FLAG_ABLATE_RAYS) ? params.ray_bias : t_max);
	while (rayQueryProceedEXT(rq)) {
	}
#endif
	if (GATHER_HIT_COMMITTED) {
		float t_hit = GATHER_HIT_T;
		r_hit_distance = t_hit;
		vec3 rel_hit = origin + world_dir * t_hit;
		mat3 view_basis = transpose(mat3(params.world_from_view));
		vec3 view_hit = view_basis * rel_hit;
		if (bool(params.flags & FLAG_SURFACE_CACHE)) {
			uint instance_id = GATHER_HIT_INSTANCE;
			vec3 world_hit = rel_hit + params.world_from_view[3].xyz;
			if (hit_specular) {
				uint hit_set = card_instances.data[instance_id].set;
				spec_hit_id = hit_set == SURFACE_CACHE_INVALID ? 0u : (SPEC_HIT_SET | hit_set);
			}
			// The glossy reflection rays take the mirror path too
			// (GODOT_GI_MIRROR_SPEC=0 keeps them on the screen read): the
			// ceiling's lobe lands on the mirror floor and wants the floor's
			// image of the room in its own direction, which the continuation
			// is. The screen read gave the floor's colour toward the camera
			// (its image of another part of the ceiling, and the beam's
			// highlight), and as the diffuse target it gives the floor's 20%
			// diffuse alone: at pose E every reflection ray on the floor read
			// the screen (RT_GI_FOLD's near-a-mirror count, 99.8% reflection
			// rays), and the box's glossy ceiling pixel over its mirror floor
			// reads 0.92 of its reference with the path against 0.725 on the
			// screen. The delta mirror rays (a roughness-0 floor's) stay on
			// the screen read: chained through the ceiling they read the
			// cards' dynamic term at off-screen crossings, which lags a moving
			// light's stop (the floor flash at stop + 32: err 0.026 and 18
			// hot pixels per thousand with them on the path, 0.020 and 3.5
			// without), while the ceiling's screen pixel is exact each frame.
#ifndef GATHER_NO_RAYS
			bool spec_path = hit_specular && !hit_mirror && (uint(params.mirror_params.z) & 8u) == 0u;
			bool mirror_path = mirror_on() && (uint(params.mirror_params.z) & 2u) == 0u && (!hit_specular || spec_path);
			uint hit_plane = mirror_path ? mirror_at(world_hit) : MAX_MIRROR_PLANES;
			if (hit_plane < MAX_MIRROR_PLANES) {
				spec_hit_id = 0u; // The image is the continuation's.
				// A planar mirror: the ray reads the mirror's diffuse card,
				// reflects and goes on, weighted by the Fresnel at the
				// bounce, through up to MIRROR_BOUNCES_MAX mirrors (a ray off
				// the ceiling that lands on a mirror floor would otherwise
				// stop at the floor's dim diffuse card and lose what the
				// floor reflects), and reads the card where it lands (the
				// probes without one). A mirror's own card is diffuse-only
				// (the light pass takes the F0 fold back out), so nothing is
				// counted twice.
				vec3 plane_diffuse = vec3(0.0);
				vec3 bounced = vec3(0.0);
				bool card_hit = false;
				float f = 1.0;
				float t_total = t_hit;
				vec3 from = world_hit;
				vec3 dir = world_dir;
				uint inst = instance_id;
				uint plane = hit_plane;
				// The chain's card reads through the ray's own footprint: the
				// diffuse cone's, or the reflection lobe's.
				float chain_cone_tan = hit_specular ? specular_cone_tan : abs(params.card_cone_tan);
				for (uint bounce = 0u; bounce < MIRROR_BOUNCES_MAX; bounce++) {
					vec3 n = params.mirrors[plane].plane.xyz;
					card_lookup_footprint = t_total * chain_cone_tan;
					vec3 plane_radiance;
					uint plane_set;
					if (surface_cache_lookup(inst, from, dir, plane_radiance, plane_set)) {
						card_requests.frame[plane_set] = params.surface_cache_frame;
						plane_radiance = max(plane_radiance, vec3(0.0));
						// The plane's diffuse from the screen where the
						// crossing is on it (the diffuse target: exact direct
						// light, denoised; the card's dynamic term lags a
						// moving light's stop by tens of frames, and the
						// floor's mirror rays chained through the ceiling read
						// that lag as a wash 1.6x the converged floor at stop
						// + 32), the card otherwise. No fold: the plane's
						// specular is the continuation below.
						if ((uint(params.mirror_params.z) & 16u) == 0u) {
							cache_tier = CACHE_TIER_CARD;
							boost_fold = false;
							plane_radiance = screen_radiance_boost(bounce == 0u ? view_hit : view_basis * (from - params.world_from_view[3].xyz), plane_radiance);
							boost_fold = true;
						}
						plane_diffuse += f * plane_radiance;
					}
					f *= mirror_fresnel(plane, abs(dot(n, dir)));
					if (bounce > 0u && !mirror_roulette(f, stbn_sample(hit_pixel, 12u + bounce).x)) {
						break; // The chain ends here: what it held is in plane_diffuse.
					}
					dir = mirror_reflect(plane, dir, stbn_sample(hit_pixel, 9u + bounce));
					// A glossy plane spreads the chain by its lobe: the card at
					// the next hit is read through that width as well, so one
					// sampled continuation reads the lobe's average rather than
					// one bright texel of it (the floor's mirror rays off the
					// glossy ceiling slab read the flashlight's spot on the
					// ceiling card one texel at a time: 18 hot pixels per
					// thousand at stop + 32 on the floor flash, 2.7 without
					// the path).
					float plane_alpha = params.mirrors[plane].params.y * params.mirrors[plane].params.y;
					chain_cone_tan = max(chain_cone_tan, 2.0 * plane_alpha);
					rayQueryEXT rq2;
					rayQueryInitializeEXT(rq2, tlas, gl_RayFlagsOpaqueEXT, 0xFF, from + n * params.ray_bias, params.ray_bias, dir, t_max);
					while (rayQueryProceedEXT(rq2)) {
					}
					if (rayQueryGetIntersectionTypeEXT(rq2, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
						break; // The sky: nothing (the box has none; a real scene's sky is the cards' job).
					}
					inst = rayQueryGetIntersectionInstanceCustomIndexEXT(rq2, true);
					float t2 = rayQueryGetIntersectionTEXT(rq2, true);
					from = from + n * params.ray_bias + dir * t2;
					t_total += t2;
					plane = bounce + 1u < MIRROR_BOUNCES_MAX ? mirror_at(from) : MAX_MIRROR_PLANES;
					if (plane < MAX_MIRROR_PLANES) {
						continue;
					}
					card_lookup_footprint = t_total * chain_cone_tan;
					vec3 card_radiance;
					uint card_set;
					if (surface_cache_lookup(inst, from, dir, card_radiance, card_set)) {
						card_requests.frame[card_set] = params.surface_cache_frame;
						// The chain's end reads the screen where it is on it,
						// as any card hit does (the fold included): the
						// floor's mirror ray off the ceiling lands on the
						// flashlit sofa, whose card's dynamic term settles
						// thirty frames after the light stops while the
						// screen's direct light is exact every frame.
						card_radiance = max(card_radiance, vec3(0.0));
						if ((uint(params.mirror_params.z) & 16u) == 0u) {
							cache_tier = CACHE_TIER_CARD;
							card_radiance = max(screen_radiance_boost(view_basis * (from - params.world_from_view[3].xyz), card_radiance), vec3(0.0));
						}
						bounced = f * card_radiance;
						card_hit = true;
					} else {
						bounced = f * max(sdfgi_cache_radiance(from - params.world_from_view[3].xyz, dir), vec3(0.0));
					}
					break;
				}
				cache_tier = CACHE_TIER_CARD;
				vec3 answer = plane_diffuse + bounced;
				if (bool(params.flags & FLAG_TIER_STATS)) {
					// Diagnostics (the RT_GI_CV line while the mirror is on): continuations, their lookups that found a card, their luminance.
					atomicAdd(calibration.cv_sums[3], 1u);
					atomicAdd(calibration.cv_sums[0], uint(min(luminance(answer), 64.0) * 1024.0));
					atomicAdd(calibration.cv_sums[1], card_hit ? 1024u : 0u);
				}
				return answer;
			}
#endif
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
					uint geometry_index = GATHER_HIT_GEOMETRY;
					uint slot = hit_materials.data[material_base + geometry_index];
					if (slot != RT_HIT_INVALID && bool(params.flags & FLAG_HIT_DEBUG_CONSTANT)) {
						return vec3(0.6);
					}
					if (slot != RT_HIT_INVALID) {
						uint idx = atomicAdd(hit_counts.data[RT_HIT_COUNT_TOTAL], 1u);
						if (idx < params.hit_capacity) {
							atomicAdd(hit_counts.data[slot], 1u);
							uint flags = (GATHER_HIT_FRONT_FACE ? RT_HIT_PACKET_FRONT_FACE : 0u) | (hit_specular ? RT_HIT_PACKET_MIRROR : 0u);
							uint b = idx * RT_HIT_PACKET_WORDS;
							hit_packets.data[b] = rt_hit_pack_pixel(hit_pixel, hit_slot, flags);
							hit_packets.data[b + 1u] = instance_id;
							hit_packets.data[b + 2u] = GATHER_HIT_PRIMITIVE;
							hit_packets.data[b + 3u] = (slot & 0xFFFFu) | ((geometry_index & 0xFFu) << 16u) | ((pixel_rays - 1u) << 24u);
							hit_packets.data[b + 4u] = GATHER_HIT_BARYCENTRICS_PACKED;
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
	boost_source = SPEC_SRC_OTHER;
	cache_tier = CACHE_TIER_PROBE;
	srad_prev_rejected = false;
	vec3 radiance = trace_radiance_chain(rel_origin, world_geo_normal, world_dir, view_origin, view_dir, jitter, r_hit_distance);
	if (hit_specular && bool(params.flags & FLAG_SPEC_SOURCE_PAINT)) {
		uint s = boost_source;
		if (trace_source == TIER_SRC_HIT_SHADED) {
			s = SPEC_SRC_HIT_SHADED;
		} else if (trace_source == TIER_SRC_SKY) {
			s = SPEC_SRC_SKY;
		} else if (trace_source == TIER_SRC_NONE) {
			s = SPEC_SRC_OTHER;
		}
		spec_paint_src = srad_prev_rejected ? 7u : s;
	}
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
		if (hit_specular) {
			// The reflection ray alone, by what answered it (SPEC_SRC_*).
			uint s = boost_source;
			if (trace_source == TIER_SRC_HIT_SHADED) {
				s = SPEC_SRC_HIT_SHADED;
			} else if (trace_source == TIER_SRC_SKY) {
				s = SPEC_SRC_SKY;
			} else if (trace_source == TIER_SRC_NONE) {
				s = SPEC_SRC_OTHER;
			}
			atomicAdd(calibration.spec_count[s], 1u);
			atomicAdd(calibration.spec_lum[s], uint(min(luminance(max(radiance, vec3(0.0))), 64.0) * 16.0));
		}
	}
	return radiance;
}
#endif

// The pixel a thread of the per-pixel kernels takes.
ivec2 gather_pixel_coord() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (bool(params.flags & FLAG_SPEC_HALF_RATE)) {
		// The half-rate reflection's threads, remapped: a ray costs its
		// SIMD group the traversal whether one lane traces or all of them,
		// so a checkerboard on the natural mapping (an 8x4 group holding
		// half of each) saved nothing (measured: 8.2 -> 7.7 ms on the TPS
		// bridge, inside the spread, against 3.5 for no reflection ray at
		// all). The block's 32 traced pixels go to the first SIMD group of
		// the 8x8 workgroup and the 32 skipped to the second, and the
		// group that skips is idle for the whole ray. (The split's trace
		// kernel is compacted and does not need it; its setup and resolve
		// keep the mapping so a pixel's thread ids, which seed the card
		// requests' sampling, are the single kernel's.)
		uint l = gl_LocalInvocationIndex;
		uint k = l & 31u;
		ivec2 lp = ivec2(int((k & 3u) * 2u), int(k >> 2u));
		lp.x += int((uint(lp.y) + (l >> 5u)) & 1u);
		pixel = ivec2(gl_WorkGroupID.xy) * 8 + lp;
	}
	return pixel;
}

// What a pixel's rays leave from, and which rays it traces: the same inputs
// and the same code in the single kernel, the setup and the resolve, so the
// three agree on every decision.
struct GatherPixel {
	ivec2 full_pixel;
	vec2 uv;
	float depth;
	vec3 view_pos;
	vec3 view_normal;
	vec3 geo_view_normal;
	float roughness;
	vec3 rel_pos;
	vec3 world_normal;
	vec3 world_geo_normal;
	float prev_frames;
	float prev_frames_static;
	uint rays; // The diffuse rays traced.
	bool mirror; // The reflection ray is a mirror ray, not a GGX sample.
	bool spec_stand_in;
	bool spec_trace; // The reflection ray is traced.
	bool fallback; // The eye ray for the cards' stand-in is traced.
};

// False for the sky.
bool gather_pixel_setup(ivec2 pixel, out GatherPixel g) {
	g.full_pixel = rt_full_pixel(pixel, int(params.depth_scale), params.full_screen_size);
	g.depth = texelFetch(depth_texture, g.full_pixel, 0).r;
	if (g.depth == 0.0) {
		return false;
	}

	g.uv = (vec2(g.full_pixel) + 0.5) / vec2(params.full_screen_size);
	vec4 view_pos4 = params.view_from_ndc * vec4(g.uv * 2.0 - 1.0, g.depth, 1.0);
	g.view_pos = view_pos4.xyz / view_pos4.w;

	vec4 nr = texelFetch(normal_roughness_texture, g.full_pixel, 0);
	g.view_normal = nr_normal(nr);
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
	//   report a hit distance of centimeters that the near-field visibility
	//   takes for contact occlusion: the wall's bumps come out as bright
	//   self-lit patches ringed by dark bands.
	//
	// So the shading normal is made to agree with the geometric one, and the
	// rays below are kept on the geometric normal's side.
	g.geo_view_normal = geometric_normal(g.full_pixel, g.view_pos, g.view_normal);
	if (dot(g.view_normal, g.geo_view_normal) < 0.0) {
		g.view_normal = -g.view_normal;
	}
	g.roughness = nr_roughness(nr);

	// Camera-relative world space: the cascade data is stored relative to the
	// camera origin, and staying camera-relative preserves precision far from
	// the world origin. The TLAS is absolute, so rays offset by the origin.
	mat3 world_basis = mat3(params.world_from_view);
	g.rel_pos = world_basis * g.view_pos;
	g.world_normal = normalize(world_basis * g.view_normal);
	g.world_geo_normal = normalize(world_basis * g.geo_view_normal);

	// How young this pixel's screen history is (last frame's frame count at
	// its reprojection, none off frame): a young pixel traces more rays
	// (ray_params.y), and reads the cards' stand-in below.
	g.prev_frames = 0.0;
	g.prev_frames_static = 0.0;
	{
		vec4 prev_ndc = params.reproject * vec4(g.uv * 2.0 - 1.0, g.depth, 1.0);
		if (prev_ndc.w > 0.0) {
			vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
			if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
				vec4 prev_meta = textureLod(prev_gi_meta, prev_uv, 0.0);
				g.prev_frames = prev_meta.r * 64.0;
				if (bool(params.flags & FLAG_DYN_SPLIT)) {
					// The moving lights' history restarts under every sweep
					// and its stand-in is the cards' own estimate of the
					// same term: it wants the stand-in, not the extra rays
					// (which cost the TPS demo's frame under its beams).
					g.prev_frames_static = g.prev_frames;
					g.prev_frames = min(g.prev_frames, prev_meta.b * 64.0);
				}
			}
		}
	}
	if (!bool(params.flags & FLAG_DYN_SPLIT)) {
		g.prev_frames_static = g.prev_frames;
	}
	g.rays = clamp(g.prev_frames_static < FALLBACK_FRAMES ? params.ray_params.y : params.ray_params.x, 1u, params.ray_count);

	// Smooth surfaces get a mirror ray only when the surface cache is there to
	// give the hit a surface at texture resolution; otherwise the rough band
	// alone, with sharp reflections left to SSR / probes, whose sharpness the
	// blurry cache cannot match.
	g.mirror = bool(params.flags & FLAG_MIRROR) && g.roughness <= 0.2;
	// Diagnostics (FLAG_ABLATE_SPEC): no reflection ray, the diffuse rays'
	// mean radiance standing in (what a lobe as wide as the hemisphere
	// would return; as a budget for rough dielectrics it read 12% bright on
	// the TPS demo's floor, section 84).
	g.spec_stand_in = bool(params.flags & FLAG_ABLATE_SPEC);
	// The half-rate form (FLAG_SPEC_HALF_RATE): a rough pixel traces its
	// reflection ray on alternate frames, its four neighbors on the
	// others, and the resolve pass reweights the neighbours' hits into
	// this pixel's lobe (the same reuse the full resolve does, here only
	// for the pixels without a ray: spec_ray stays zero to mark them). A
	// mirror keeps its ray: no neighbour's sample is its image. The stand-in
	// (the diffuse mean) was measured as the cheaper form first and is a
	// bias, not a budget: a grazing lobe on the TPS demo's floor sees the
	// dark far end of the ring, the hemisphere mean sees the lit ceiling,
	// and the frame read 12% brighter.
	// The checkerboard does not alternate by frame: it did first, and a
	// pixel then toggled between its own sample and its neighbours' mean,
	// two estimators that agree on a flat wall and not on a normal-mapped
	// pipe or a highlight a pixel wide -- the machines' flicker at rest
	// doubled and a strafe past the hall's pipes left a mottle where the
	// two patterns met in the history. Fixed, a skipped pixel is always
	// the mean of its four traced neighbors: a blurrier reflection on
	// half the pixels, and a still one.
	bool spec_skip = bool(params.flags & FLAG_SPEC_HALF_RATE) && !g.mirror && g.roughness > 0.2 && ((pixel.x + pixel.y) & 1) != 0;
	if (spec_skip) {
		// Only where a 4-neighbour is on this surface by the fill's own
		// stops (a twentieth of the depth, the normal to a few degrees):
		// a railing or a pipe a pixel wide at the quarter tier has none,
		// and a fill with nothing to read is a black sample. Those trace.
		bool has_neighbour = false;
		for (int k = 0; k < 4 && !has_neighbour; k++) {
			ivec2 sp = pixel + ivec2(k == 0 ? -1 : (k == 1 ? 1 : 0), k == 2 ? -1 : (k == 3 ? 1 : 0));
			if (any(lessThan(sp, ivec2(0))) || any(greaterThanEqual(sp, params.screen_size))) {
				continue;
			}
			ivec2 sfp = rt_full_pixel(sp, int(params.depth_scale), params.full_screen_size);
			float sd = texelFetch(depth_texture, sfp, 0).r;
			if (sd == 0.0) {
				continue;
			}
			vec2 suv = (vec2(sfp) + 0.5) / vec2(params.full_screen_size);
			vec4 sp4 = params.view_from_ndc * vec4(suv * 2.0 - 1.0, sd, 1.0);
			float s_depth = -sp4.z / sp4.w;
			if (abs(s_depth + g.view_pos.z) > 0.05 * max(-g.view_pos.z, 1.0)) {
				continue;
			}
			vec3 sn = nr_normal(texelFetch(normal_roughness_texture, sfp, 0));
			has_neighbour = pow(max(dot(g.view_normal, sn), 0.0), 32.0) > 1e-3;
		}
		spec_skip = has_neighbour;
	}
	// Measured and not kept (section 90): the diffuse ray standing in as a
	// rough pixel's reflection sample, reweighted into the lobe by the
	// resolve -- +8% brighter for -1.8 ms, +2.7% gated to steep views.
	g.spec_trace = !g.spec_stand_in && bool(params.flags & FLAG_SPECULAR) && (g.roughness > 0.2 || g.mirror) && !spec_skip;
	g.fallback = bool(params.flags & FLAG_SURFACE_CACHE) && !bool(params.flags & FLAG_FALLBACK_OFF) && (g.prev_frames < FALLBACK_FRAMES || bool(params.flags & FLAG_FALLBACK_ALL));
	return true;
}

// Diffuse ray r's direction: cosine-weighted about the shading normal, kept
// on the geometric normal's side.
vec3 gather_diffuse_dir(GatherPixel g, ivec2 pixel, uint r) {
	return fold_above(cosine_hemisphere(g.world_normal, stbn_sample(pixel, r)), g.world_geo_normal);
}

// The reflection ray's direction: GGX half-vector sampling around the
// mirror direction (the mirror direction itself for a mirror). below: the
// draw fell under the horizon and the mirror direction stands in.
vec3 gather_reflection_dir(GatherPixel g, ivec2 pixel, out bool below) {
	vec2 rnd = stbn_sample(pixel, 6u);
	vec3 v = normalize(-g.rel_pos);
	// (Measured and not kept: narrowing a young pixel's lobe toward the
	// mirror direction by its youth, against the entering band's
	// one-sample sparkle. The sparkle went, but the sharp image it left
	// in the history read 0.034 against 0.028 at the stop of the flick
	// case and was still behind at stop + 16; a rough lobe's blur is
	// what the eye expects there.)
	float alpha = g.roughness * g.roughness;
	vec3 dir = reflect(-v, g.world_normal);
	below = false;
	if (!g.mirror) {
		// The half vector's density: the plain NDF draws half vectors facing
		// away from the view as often as toward it, so a grazing view's
		// samples fall below the horizon or carry a weight the 1 / (4 v.h)
		// turns unbounded; the visible normals (FLAG_SPEC_VNDF; Heitz 2018,
		// drawn as Dupuy and Benyoub 2023's spherical cap) are the NDF seen
		// from the view, so every half vector faces it and the BRDF weight
		// the resolve applies is G / G1(v), at most 1 / G1(v) (section 105).
		//
		// A draw that still falls below the horizon has no light to return:
		// the mirror direction is traced in its place (the ray is spent
		// anyway) and below marks it, a zero in the weighted estimate.
		// (Measured and not kept: redrawing it, up to four times. The
		// estimate is then the mean over the lobe above the surface under
		// the sampling density, one convention among three that each
		// converged to a different level, none of them the BRDF-weighted
		// mean the DFG multiply wants; section 105.)
		float phi = rnd.x * 2.0 * M_PI;
		vec3 h_local;
		if (bool(params.flags & FLAG_SPEC_VNDF)) {
			mat3 basis = basis_around(g.world_normal);
			vec3 v_local = transpose(basis) * v;
			v_local.z = max(v_local.z, 1e-4);
			vec3 v_std = normalize(vec3(v_local.xy * alpha, v_local.z));
			float z = (1.0 - rnd.y) * (1.0 + v_std.z) - v_std.z;
			float sz = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
			vec3 h_std = vec3(sz * cos(phi), sz * sin(phi), z) + v_std;
			h_local = vec3(h_std.xy * alpha, max(h_std.z, 0.0));
		} else {
			float ct = sqrt((1.0 - rnd.y) / (1.0 + (alpha * alpha - 1.0) * rnd.y));
			float st = sqrt(max(1.0 - ct * ct, 0.0));
			h_local = vec3(st * cos(phi), st * sin(phi), ct);
		}
		vec3 d = reflect(-v, normalize(basis_around(g.world_normal) * h_local));
		if (dot(d, g.world_normal) > 1e-4) {
			dir = d;
		} else {
			below = true;
		}
	}
	return fold_above(dir, g.world_geo_normal);
}

#ifdef GATHER_SETUP
// One request per lane that wants a ray, appended in uniform control flow:
// one atomic per SIMD group for the count and one for the dispatch's group
// count, instead of two per ray. Nothing bounds the index: the buffer holds
// a request per slot per pixel, the most the kernel can append.
void gather_append(bool want, uvec4 a, uvec4 b) {
	uint base = subgroupExclusiveAdd(want ? 1u : 0u);
	uint total = subgroupAdd(want ? 1u : 0u);
	if (total == 0u) {
		return;
	}
	uint first = 0u;
	if (subgroupElect()) {
		first = atomicAdd(gather_count.count, total);
		atomicMax(gather_args.groups.x, (first + total + 63u) / 64u);
	}
	first = subgroupBroadcastFirst(first);
	if (want) {
		gather_requests.data[(first + base) * 2u] = a;
		gather_requests.data[(first + base) * 2u + 1u] = b;
	}
}

// The setup kernel: every ray's direction into its record, the screen
// trace where it answers (the record then holds the hit), a request where
// it does not. The loop over the slots is uniform (the lanes past the
// screen and on the sky take part in the appends with nothing to add).
void setup_main() {
	ivec2 pixel = gather_pixel_coord();
	GatherPixel g;
	bool on_surface = pixel.x < params.screen_size.x && pixel.y < params.screen_size.y && gather_pixel_setup(pixel, g);
	uint record_base = on_surface ? uint(pixel.y * params.screen_size.x + pixel.x) * GATHER_SLOTS : 0u;
	vec3 camera = params.world_from_view[3].xyz;
	for (uint s = 0u; s < GATHER_SLOTS; s++) {
		bool want = false;
		uvec4 a = uvec4(0u);
		uvec4 b = uvec4(0u);
		if (on_surface) {
			uint record = record_base + s;
			if (s == params.ray_count + 1u) {
				if (g.fallback) {
					// The eye ray: from the camera to just past the surface.
					float view_len = length(g.rel_pos);
					vec3 eye_dir = g.rel_pos / max(view_len, 1e-4);
					a = uvec4(floatBitsToUint(camera), floatBitsToUint(view_len * 1.02));
					b = uvec4(floatBitsToUint(eye_dir), record);
					want = true;
				}
			} else if (s < g.rays || (s == params.ray_count && g.spec_trace)) {
				bool spec = s == params.ray_count;
				bool below;
				vec3 dir = spec ? gather_reflection_dir(g, pixel, below) : gather_diffuse_dir(g, pixel, s);
				gather_records.data[record * 2u + 1u] = uvec4(floatBitsToUint(dir), 0u);
				bool screen_hit = false;
				vec3 hit_view;
				if (bool(params.flags & FLAG_SCREEN_TRACES)) {
					mat3 view_basis = transpose(mat3(params.world_from_view));
					screen_hit = screen_trace_hit(g.view_pos, view_basis * g.world_geo_normal, view_basis * dir, stbn_sample(pixel, spec ? 5u : 7u).r, hit_view);
				}
				if (screen_hit) {
					gather_records.data[record * 2u] = uvec4(GATHER_RECORD_SCREEN_HIT, floatBitsToUint(hit_view));
				} else {
					vec3 origin = g.rel_pos + g.world_geo_normal * params.ray_bias + camera;
					float t_max = bool(params.flags & FLAG_ABLATE_RAYS) ? params.ray_bias : gather_t_max(g.rel_pos, dir);
					a = uvec4(floatBitsToUint(origin), floatBitsToUint(t_max));
					b = uvec4(floatBitsToUint(dir), record);
					want = true;
				}
			}
		}
		gather_append(want, a, b);
	}
}
#endif

#ifdef GATHER_TRACE
// The trace kernel: one request, one ray, the answer into its record.
void trace_main() {
	uint i = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
	if (i >= gather_count.count) {
		return;
	}
	uvec4 a = gather_requests.data[i * 2u];
	uvec4 b = gather_requests.data[i * 2u + 1u];
	uint record = b.w;
	// The eye ray starts at the camera; the others a bias off their surface,
	// as in the single kernel.
	float t_min = (record % GATHER_SLOTS) == params.ray_count + 1u ? 0.0 : params.ray_bias;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, uintBitsToFloat(a.xyz), t_min, uintBitsToFloat(b.xyz), uintBitsToFloat(a.w));
	while (rayQueryProceedEXT(rq)) {
	}
	uvec4 answer = uvec4(0u);
	if (GATHER_HIT_COMMITTED) {
		answer = uvec4(GATHER_RECORD_HIT | (GATHER_HIT_FRONT_FACE ? GATHER_RECORD_FRONT_FACE : 0u) | (GATHER_HIT_GEOMETRY << 8u), floatBitsToUint(GATHER_HIT_T), GATHER_HIT_INSTANCE, GATHER_HIT_PRIMITIVE);
		gather_records.data[record * 2u + 1u].w = GATHER_HIT_BARYCENTRICS_PACKED;
	}
	gather_records.data[record * 2u] = answer;
}
#endif

#if !defined(GATHER_SETUP) && !defined(GATHER_TRACE)
// The single kernel, and the resolve: the same code, the rays' answers
// from the query or from the records.
void gather_main() {
	ivec2 pixel = gather_pixel_coord();
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	GatherPixel g;
	if (!gather_pixel_setup(pixel, g)) {
		imageStore(out_ambient, pixel, vec4(0.0));
		if (bool(params.flags & FLAG_DYN_SPLIT)) {
			imageStore(out_ambient_dyn, pixel, vec4(0.0));
			imageStore(out_fallback_dyn, pixel, vec4(0.0));
		}
		imageStore(out_reflection, pixel, vec4(0.0));
		imageStore(out_spec_ray, pixel, vec4(0.0));
		imageStore(out_spec_hit, pixel, uvec4(0u));
		imageStore(out_view_depth, pixel, vec4(0.0));
		// Sky: unoccluded, no directional bias.
		imageStore(out_directional, pixel, vec4(0.0, 0.0, 0.0, 1.0));
		imageStore(out_fallback, pixel, vec4(0.0));
		if (bool(params.flags & FLAG_REUSE_RECORD)) {
			uint reuse_base = uint(pixel.y * params.screen_size.x + pixel.x) * (params.ray_count + 1u);
			for (uint s = 0u; s <= params.ray_count; s++) {
				reuse_rays.data[reuse_base + s] = uvec4(0u);
			}
		}
		return;
	}
	ivec2 full_pixel = g.full_pixel;
	vec3 view_pos = g.view_pos;
	vec3 world_normal = g.world_normal;
	vec3 world_geo_normal = g.world_geo_normal;
	vec3 rel_pos = g.rel_pos;
	float roughness = g.roughness;
	bool mirror = g.mirror;
	mat3 world_basis = mat3(params.world_from_view);
#ifdef GATHER_RESOLVE
	uint record_base = uint(pixel.y * params.screen_size.x + pixel.x) * GATHER_SLOTS;
#endif

	// One pixel in sixteen feeds the calibration sums: plenty for a mean, and
	// the 32-bit fixed-point sums cannot overflow at any screen size in use.
	calibrate_pixel = bool(params.flags & FLAG_CALIBRATE_CACHE) && ((pixel.x | pixel.y) & 3) == 0;

	uint rays = g.rays;
	pixel_rays = rays;
	if (bool(params.flags & FLAG_TIER_STATS)) {
		uint n = subgroupAdd(1u);
		uint young = subgroupAdd(g.prev_frames < FALLBACK_FRAMES ? 1u : 0u);
		uint young_static = subgroupAdd(g.prev_frames_static < FALLBACK_FRAMES ? 1u : 0u);
		if (subgroupElect()) {
			atomicAdd(calibration.pixels, n);
			if (young > 0u) {
				atomicAdd(calibration.young_pixels, young);
			}
			if (young_static > 0u) {
				atomicAdd(calibration.young_static, young_static);
			}
		}
	}

	vec3 irradiance = vec3(0.0);
	vec3 irradiance_dyn = vec3(0.0); // The moving lights' part of it (FLAG_DYN_SPLIT).
	// First moment of the incoming radiance and the near-field visibility,
	// both free from the rays we already trace.
	vec3 moment = vec3(0.0);
	float visibility = 0.0;
	hit_pixel = pixel;
	hit_mirror = false;
	hit_specular = false;
	for (uint r = 0u; r < rays; r++) {
#ifdef GATHER_RESOLVE
		gather_record = record_base + r;
		vec3 dir = uintBitsToFloat(gather_records.data[gather_record * 2u + 1u].xyz);
#else
		vec3 dir = gather_diffuse_dir(g, pixel, r);
#endif
		vec3 view_dir = transpose(world_basis) * dir;
		float t_hit;
		hit_slot = r;
		ray_dyn = vec3(0.0);
		// Clamped non-negative: half-float caches and the screen radiance
		// boost can return a small negative, and the |moment| <= luminance
		// bound the reconstruction relies on only holds for positive radiance.
		vec3 radiance = max(trace_radiance(rel_pos, world_geo_normal, dir, view_pos, view_dir, stbn_sample(pixel, 7u).r, t_hit), vec3(0.0));
		irradiance += radiance;
		irradiance_dyn += clamp(ray_dyn, vec3(0.0), radiance);
		if (bool(params.flags & FLAG_REUSE_RECORD)) {
			float dyn_share = clamp(luminance(clamp(ray_dyn, vec3(0.0), radiance)) / max(luminance(radiance), 1e-6), 0.0, 1.0);
			uint flags = GI_REUSE_RAY_VALID | ((rays - 1u) << GI_REUSE_RAY_COUNT_SHIFT) | (uint(round(dyn_share * 255.0)) << GI_REUSE_RAY_DYN_SHIFT);
			reuse_rays.data[uint(pixel.y * params.screen_size.x + pixel.x) * (params.ray_count + 1u) + r] = gi_reuse_record(radiance, view_dir, t_hit, flags);
		}
		moment += luminance(radiance) * dir;
		// Only nearby geometry occludes: in an open scene nearly every ray
		// hits something eventually, and counting those would report near
		// total occlusion everywhere.
		visibility += clamp(t_hit * params.inv_ao_range, 0.0, 1.0);
	}
	float inv_rays = 1.0 / float(rays);
	irradiance *= inv_rays;
	irradiance_dyn *= inv_rays;
	moment *= inv_rays;
	visibility *= inv_rays;
#ifndef GATHER_NO_RAYS
	if (mirror_on() && params.mirror_light.w > 0.0) {
		// The knob light's images through the planar mirrors: direct light
		// a mirror throws onto this surface, which no ray can find (a point
		// seen through a delta), so it is evaluated here, the way the box's
		// image solve does. A light's image at a point is the light itself
		// at the mirrored point with the mirrored normal, times the Fresnel
		// at the crossing, seen through a shadow ray in two legs (to a
		// centimeter above the mirror measured along its normal, then from
		// the mirror to the light). The knob's own light only (a scene
		// without the stochastic direct pass, the box): the scene's lights,
		// the dynamic ones included, are imaged by that pass (their caustic
		// is direct light), so they are not imaged twice here.
		vec3 world_pos = rel_pos + params.world_from_view[3].xyz;
		if ((uint(params.mirror_params.z) & 1u) != 0u) {
			irradiance = vec3(0.0); // Diagnostics: the image terms alone.
			moment = vec3(0.0);
		}
		vec3 start = world_pos + world_geo_normal * params.ray_bias;
		vec3 light = params.mirror_light.xyz;
		for (uint mi = 0u; mi < mirror_count(); mi++) {
			vec3 n = params.mirrors[mi].plane.xyz;
			float hp = mirror_height(mi, world_pos);
			if (hp <= 0.005 || mirror_height(mi, light) <= 0.0) {
				continue;
			}
			vec3 img = mirror_point(mi, light);
			vec3 m;
			float cos_p;
			float cover = mirror_crossing(mi, world_pos, img, m, cos_p);
			if (cover <= 0.0) {
				continue;
			}
			vec3 rel = img - world_pos;
			float d = length(rel);
			vec3 dir = rel / max(d, 1e-4);
			float cos_n = dot(world_normal, dir);
			if (cos_n <= 0.0 || d >= params.mirror_params.y) {
				continue;
			}
			float nd = d / params.mirror_params.y;
			nd *= nd;
			nd *= nd;
			nd = max(1.0 - nd, 0.0);
			nd *= nd;
			vec3 c = vec3(params.mirror_light.w * nd / max(d, 1e-4) * cos_n) * (cover * mirror_fresnel(mi, cos_p));
			if (luminance(c) <= 0.0) {
				continue;
			}
			vec3 leg_end = world_pos + dir * (max(hp - 0.01, 0.0) / cos_p);
			if (mirror_occluded(start, leg_end) || mirror_occluded(m + n * 0.01, light)) {
				continue;
			}
			irradiance += c;
			moment += luminance(c) * dir;
		}
	}
#endif
	vec3 reflection = vec3(0.0);
	// Where the reflected image lives: the virtual point behind the surface,
	// at the hit distance beyond it along the view ray, expressed as a view
	// depth. The temporal filter reprojects the reflection by this rather
	// than by the surface, which is what stops a glossy floor's reflection
	// from smearing as the camera moves. Defaults to the surface itself.
	float virtual_view_depth = -view_pos.z;
	vec4 spec_ray = vec4(0.0);
	if (g.spec_stand_in) {
		reflection = irradiance;
	} else if (g.spec_trace) {
		vec3 v = normalize(-rel_pos);
		float alpha = roughness * roughness;
		specular_cone_tan = mirror ? 0.0 : min(2.0 * alpha, params.card_cone_tan);
		bool below;
#ifdef GATHER_RESOLVE
		gather_record = record_base + params.ray_count;
		vec3 dir = uintBitsToFloat(gather_records.data[gather_record * 2u + 1u].xyz);
		gather_reflection_dir(g, pixel, below);
#else
		vec3 dir = gather_reflection_dir(g, pixel, below);
#endif
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
			float ndf = alpha * alpha / (M_PI * d * d);
			float pdf = ndf * ndh / (4.0 * vdh);
			if (bool(params.flags & FLAG_SPEC_VNDF)) {
				// D(h) G1(v) / (4 n.v), G1 Smith's.
				float ndv = max(dot(world_normal, v), 1e-4);
				pdf = ndf / (2.0 * (ndv + sqrt(alpha * alpha + (1.0 - alpha * alpha) * ndv * ndv)));
			}
			// Negative where the draw fell below the horizon and the mirror
			// direction stood in: a zero in the BRDF-weighted estimate
			// (stochastic_reflection_resolve.glsl), nonzero for every test
			// of whether the pixel traced.
			spec_ray = vec4(octahedron_encode(view_dir), below ? -1.0 : max(pdf, 1e-6), min(spec_t_hit, 1e4));
		}
		float view_len = max(length(view_pos), 1e-4);
		// A curved mirror's image is not at the hit distance behind it: a
		// convex surface of curvature k images a point at distance t at
		// t / (1 + 2 k t) -- a pillar of 0.4 m radius images the far wall a
		// fifth of a meter behind its surface, not four meters. Reprojected
		// at the hit distance instead, the temporal filter fetched the
		// pillar's reflection history from where the wall would have
		// reprojected, and every highlight on it doubled and smeared under a
		// dolly (rt_lab temporal_test MOTION=dolly, the pillar was the whole
		// of the diff). Planes read zero curvature and keep the hit distance.
		float curvature = surface_curvature(full_pixel, view_pos, g.geo_view_normal);
		float t_image = min(spec_t_hit, 1e4);
		t_image /= (1.0 + 2.0 * curvature * t_image);
		virtual_view_depth = -view_pos.z * (1.0 + t_image / view_len);
		if (bool(params.flags & FLAG_REUSE_RECORD)) {
			uint flags = GI_REUSE_RAY_VALID | GI_REUSE_RAY_SPEC | (mirror ? GI_REUSE_RAY_MIRROR : 0u) | (below ? GI_REUSE_RAY_BELOW : 0u);
			reuse_rays.data[uint(pixel.y * params.screen_size.x + pixel.x) * (params.ray_count + 1u) + params.ray_count] = gi_reuse_record(max(reflection, vec3(0.0)), view_dir, spec_t_hit, flags);
		}
	} else if (bool(params.flags & FLAG_REUSE_RECORD)) {
		// No reflection ray: skipped by the half-rate checkerboard (a rough
		// pixel that reflects, valid without GI_REUSE_RAY_SPEC), or none.
		bool skipped = !g.spec_stand_in && bool(params.flags & FLAG_SPECULAR) && roughness > 0.2 && !mirror;
		reuse_rays.data[uint(pixel.y * params.screen_size.x + pixel.x) * (params.ray_count + 1u) + params.ray_count] = uvec4(0u, 0u, 0u, skipped ? GI_REUSE_RAY_VALID : 0u);
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
	if (bool(params.flags & FLAG_SPEC_SOURCE_PAINT) && g.spec_trace) {
		// Diagnostics: screen green, partial (the border fade) dark green,
		// card blue, hit-shaded magenta, sky cyan, cascades/probes red, and
		// white a card standing in for a screen read FLAG_SRAD_PREV_DEPTH
		// rejected.
		const vec3 src_colors[8] = vec3[8](vec3(0.0, 1.0, 0.0), vec3(0.0, 0.4, 0.0), vec3(0.5), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(1.0));
		reflection = src_colors[min(spec_paint_src, 7u)];
	}
	imageStore(out_reflection, pixel, vec4(reflection, virtual_view_depth));
	imageStore(out_spec_ray, pixel, spec_ray);
	imageStore(out_spec_hit, pixel, uvec4(g.spec_trace && !g.spec_stand_in ? spec_hit_id : 0u));
	imageStore(out_view_depth, pixel, vec4(-view_pos.z, 0.0, 0.0, 0.0));

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
	vec3 fallback_dyn = vec3(0.0);
	float fallback_coverage = 0.0;
	if (g.fallback) {
		float view_len = length(rel_pos);
		vec3 eye_dir = rel_pos / max(view_len, 1e-4);
#ifdef GATHER_RESOLVE
		uvec4 rec = gather_records.data[(record_base + params.ray_count + 1u) * 2u];
#else
		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, params.world_from_view[3].xyz, 0.0, eye_dir, view_len * 1.02);
		while (rayQueryProceedEXT(rq)) {
		}
#endif
		if (GATHER_HIT_COMMITTED) {
			uint instance_id = GATHER_HIT_INSTANCE;
			vec3 world_hit = params.world_from_view[3].xyz + eye_dir * GATHER_HIT_T;
			float change_before = pixel_change;
			float change_dyn_before = pixel_change_dyn;
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
				// shrinking by root two with every doubling of its relights
				// (exp2 of the youth level, which falls half a step per
				// doubling).
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
				vec3 ind_dyn = vec3(0.0);
				uint parts = (params.fallback_parts & 7u) == 0u ? 7u : (params.fallback_parts & 7u);
				// The tent weighted by the texels under it that a card filled
				// and the lighting has reached (the mip chain's rule; plan
				// section 106): a card is a rectangle over its instance, and
				// where the surface has a hole or an edge -- a wall's
				// window, the reveal's concave corner -- the taps that land
				// beyond it read the atlas's black. Unweighted, the stand-in
				// fell linearly toward the edge, 40% low at the texel beside
				// it: a dark strip along the lab's window reveal for the
				// frames after a strafe, while the wall was young.
				bool coverage = (params.fallback_parts & 8u) != 0u;
				float covered = 0.0;
				for (int dy = 0; dy < 4; dy++) {
					for (int dx = 0; dx < 4; dx++) {
						vec2 t = clamp(card_atlas_texel + (vec2(dx, dy) - 1.5) * spacing, t_min, t_max);
						vec2 uv = t / float(params.surface_cache_atlas_size);
						// The dynamic bounces come summed and filtered in the one atlas (parts 2 and 4 both select it).
						ind += ((parts & 1u) != 0u ? textureLod(card_indirect_atlas, uv, 0.0).rgb : vec3(0.0));
						ind_dyn += ((parts & 6u) != 0u ? max(textureLod(card_indirect_dyn_atlas, uv, 0.0).rgb, vec3(0.0)) : vec3(0.0));
						if (coverage) {
							// The bilinear tap's own weights on its four
							// texels, in textureGather's order (i0 j1, i1 j1,
							// i1 j0, i0 j0).
							vec2 f = fract(t - 0.5);
							vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
							vec4 depth4 = textureGather(card_depth_atlas, uv, 0);
							vec4 relit4 = textureGather(card_indirect_atlas, uv, 3);
							covered += dot(w, vec4(greaterThan(depth4, vec4(0.0))) * vec4(greaterThan(relit4, vec4(0.0))));
						}
					}
				}
				float norm = coverage ? 1.0 / max(covered, 1e-3) : 1.0 / 16.0;
				ind *= norm;
				ind_dyn *= norm;
				// The moving lights' share apart (FLAG_DYN_SPLIT: the
				// spatial pass fades each history's own share in);
				// the stand-in itself is the sum, as before.
				ind += ind_dyn;
				fallback = vec4(max(ind, vec3(0.0)), min(relights, 64.0) / 64.0 * card_lookup_confidence);
				fallback_dyn = max(ind_dyn, vec3(0.0));
				// The tent's covered share rides in the moving lights'
				// stand-in's spare alpha: beside a hole the stand-in is the
				// mean of a few texels, not sixteen, and the spatial pass
				// trusts it that much less (renormalised at full trust, the
				// junctions' hot pixels doubled).
				fallback_coverage = coverage ? min(covered / 16.0, 1.0) : 1.0;
			}
			pixel_change = change_before;
			pixel_change_dyn = change_dyn_before;
		}
	}
	imageStore(out_fallback, pixel, fallback);
	if (bool(params.flags & FLAG_DYN_SPLIT)) {
		imageStore(out_fallback_dyn, pixel, vec4(fallback_dyn, fallback_coverage));
	}

	// Measured and not kept (section 39): the cards' field under the surface
	// as a control variate for the rays -- unbiased, and no gain on the
	// flicks.
	if (params.mirror_params.x > 0.0 && standin_reads > 0.0) {
		pixel_change = max(pixel_change, params.mirror_params.x * standin_youth / standin_reads);
	}
	if (bool(params.flags & FLAG_DYN_SPLIT)) {
		// The two histories' samples: the moving lights' part, and the rest
		// (never negative: the image lights above move the whole, the part is
		// capped by it).
		irradiance_dyn = clamp(irradiance_dyn, vec3(0.0), irradiance);
		imageStore(out_ambient, pixel, vec4(irradiance - irradiance_dyn, clamp(pixel_change, 0.0, 1.0)));
		imageStore(out_ambient_dyn, pixel, vec4(irradiance_dyn, clamp(pixel_change_dyn, 0.0, 1.0)));
	} else {
		imageStore(out_ambient, pixel, vec4(irradiance, clamp(pixel_change, 0.0, 1.0)));
	}
	imageStore(out_directional, pixel, directional_out);
}
#endif

void main() {
#if defined(GATHER_SETUP)
	setup_main();
#elif defined(GATHER_TRACE)
	trace_main();
#else
	gather_main();
#endif
}
