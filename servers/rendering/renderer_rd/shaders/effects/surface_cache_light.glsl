#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : enable

// Surface cache lighting: shades the card texels of this frame's active card
// sets. Per texel, the position and normal come back out of the capture
// (card depth plus the card's orthographic frame), direct light is the sun
// and the lights culled to the set's box with one shadow ray per texel per
// frame (a ratio estimator over the box's light list, like the direct
// pass), indirect light is the SDFGI lightprobes or the sky, and emission
// rides on top. The result accumulates in the radiance atlas the GI gather
// reads at ray hits.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "../oct_inc.glsl"
#include "surface_cache_inc.glsl"

#define MAX_LIGHTS_PER_SET 32u
#define M_PI 3.14159265359
#define SDFGI_MAX_CASCADES 8
#define SDFGI_OCT_SIZE 6

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict readonly buffer Sets {
	CardSet data[];
}
sets;

layout(set = 0, binding = 2, std430) restrict readonly buffer Active {
	uint count;
	uint list[];
}
active_sets;

layout(set = 0, binding = 3, std430) restrict readonly buffer SetLights {
	uint data[];
}
set_lights;

layout(set = 0, binding = 4, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 5, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

// A uniform buffer, as the scene shader binds it (8 = RendererSceneRender::MAX_DIRECTIONAL_LIGHTS).
layout(set = 0, binding = 6, std140) uniform DirectionalLights {
	DirectionalLightData data[8];
}
directional_lights;

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 world_from_view;
	vec4 camera_origin;
	vec4 sky_quat_or_color;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float sky_energy;
	vec2 sky_border;
	uint flags;
	uint temporal_frames;
	uint atlas_size;
	uint debug; // Profiling ablations: 1 no bounce ray, 2 no shadow rays, 4 no local lights, 8 no directional lights.
	vec3 grid_origin; // The world light grid, when FLAG_GRID: its corner, cell size, cells per edge, entries per cell.
	float grid_cell;
	uint grid_n;
	uint grid_cap;
	float bounce_floor; // The fewest relights a change restarts the bounce accumulation to (see accumulate).
	uint young_rays; // Extra bounce rays for a texel whose accumulation is young (see trace_bounce_young).
	uint dynamic_rays; // Light rays per dynamic light per texel per relight (see trace_dynamic).
	float dynamic_motion; // How far the dynamic lights moved this frame, over the distance that refreshes their bounce whole (0 at rest, 1 and above a full refresh): caps the dynamic histories' length at its inverse (see accumulate).
	float dynamic_window; // The most relights the dynamic histories accumulate.
	float dynamic_change; // How much the dynamic lights' intensity or colour changed this frame, relative (a hue turning at constant luminance is a change the luminance below cannot see).
	float dynamic_join; // On the frame a light joins the dynamic set (every set relit): the share of its bounce the static accumulation holds, which it sheds (see accumulate). 0 otherwise.
	uint area_light_count; // The area lights (every one in the population: a few, each tested for range per texel).
	float pad_join1;
	float pad_join2;
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
}
params;

#define FLAG_SDFGI 1u
#define FLAG_SKY_MODE_SKY 2u
#define FLAG_SKY_MODE_COLOR 4u
#define FLAG_SHARED_BOUNCE_RAY 16u
#define FLAG_DYNAMIC_YOUNG 64u // The young texels' extra cosine rays while a dynamic light moves (GODOT_CARD_DYN_YOUNG).
#define FLAG_GRID 32u // The world light grid was built this frame: a texel inside it reads its cell's lights. // One bounce ray per 2x2 quad: a thread per quad, a workgroup per 16x16 texels.

layout(set = 0, binding = 8) uniform texture2D albedo_atlas;
layout(set = 0, binding = 9) uniform texture2D normal_atlas;
layout(set = 0, binding = 10) uniform texture2D emission_atlas;
layout(set = 0, binding = 11) uniform texture2D depth_atlas;
layout(set = 0, binding = 12, rgba16f) uniform restrict image2D lighting_atlas;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell;
	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 13, std140) uniform SDFGI {
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

layout(set = 0, binding = 14) uniform texture2DArray lightprobe_texture;
layout(set = 0, binding = 15) uniform texture3D occlusion_texture;
layout(set = 0, binding = 16) uniform sampler linear_sampler_mipmaps;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 17) uniform texture2DArray sky_radiance;
#else
layout(set = 0, binding = 17) uniform texture2D sky_radiance;
#endif

// The cache reads itself for its indirect term: each texel traces one
// cosine ray per frame and, where it lands on another card, takes that
// card's outgoing radiance from the previous update. Emission and direct
// light so propagate bounce by bounce through the cards, with nothing
// depending on the probes for geometry that moves.
layout(set = 0, binding = 18, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;

layout(set = 0, binding = 19, rgba16f) uniform restrict image2D indirect_atlas;

// Sets the card rays land on are asked for like the gather's hits are:
// otherwise a wall only ever seen through a bounce is relit on the
// round-robin alone, and its stale lighting feeds every card that reads it.
layout(set = 0, binding = 20, std430) restrict writeonly buffer CardRequests {
	uint frame[];
}
card_requests;

// The direct term's temporal gradients and its accumulated visibility, six
// halves packed in four uints (see change_load / change_store):
// - the unshadowed direct radiance (times albedo, plus emission) at the
//   last relight, in colour: a hue change at constant brightness is a
//   change too;
// - its relative change since the relight before, the radiance gradient.
//   The unshadowed term is deterministic, so a change in it is a change in
//   the lights, not noise: A-SVGF's temporal gradient with the relight as
//   the re-shade. It restarts the bounce accumulation and the GI gather
//   reads it at hits to restart the pixel's history;
// - the local lights' geometric sum (attenuation times cosine, unit
//   colour) at the last relight. Its change is the geometric gradient: a
//   light that moved, appeared or vanished, which is what can change a
//   texel's visibility; a light's colour or intensity changing does not,
//   so it restarts the visibility ratio and nothing else;
// - the accumulated visibility ratio of the local lights (the ratio
//   estimator: the unshadowed sum is exact every relight, the shadow ray
//   only measures what fraction of it arrives);
// - what the last relight's bounce ray hit: the set (16 bits, 0xFFFF for
//   none) and the distance (a half). The next relight traces that same ray
//   again (its seed is the relight's frame, see Relit) and compares:
//   A-SVGF's gradient sample for the bounce, on the hit's identity rather
//   than its radiance. It is what catches a surface moving out of the ray's
//   way -- an emissive box that swept past a floor and left, whose glow the
//   accumulated bounce otherwise kept for the whole window (rt_lab
//   temporal_test descend: 0.078 against the converged reference at the
//   stop, still 0.035 thirty frames on); the radiance gradient above sees
//   only a light changing. Comparing the radiance instead was measured and
//   is a chain reaction: a restarted texel's radiance is one sample's, every
//   texel whose re-traced ray lands on it reads that as a change and
//   restarts too, and in the game project's dense room the cards never
//   settled (the glossy floor flickered at 46 hot pixels per thousand at
//   rest, 0.2 with the gradient off). What a ray hits is deterministic on a
//   still scene, and a light's change already reaches the bounce through
//   the hit card's own gradient.
layout(set = 0, binding = 21, rgba32ui) uniform restrict uimage2D change_atlas;

// Per set, two frames: the relight before the last and the last (the prepare
// pass promotes the last to the previous when it selects the set).
layout(set = 0, binding = 23, std430) restrict buffer Relit {
	uint frame[];
}
relit;

// The dynamic lights' bounce (see trace_dynamic), its own history: rgb the
// accumulated term, a the luminance of the dynamic lights' direct term at
// the last relight (for the total change, see accumulate).
layout(set = 0, binding = 24, rgba16f) uniform restrict image2D indirect_dyn_atlas;

// The dynamic lights (see trace_dynamic): the lights that moved or changed
// lately, in world space, with their weights (1 while a light changes,
// fading to 0 once it has rested; LightStorage tracks them, surface_cache.cpp
// uploads them). pad is 1 for a spot. The GI gather reads the same buffer:
// their direct term is not in the lighting atlas (see accumulate), it is
// added at the hits from the lights' current state.
layout(set = 0, binding = 27, std430) restrict readonly buffer DynamicLights {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
	vec4 weights[2];
	LightData data[8];
}
dyn_lights;
// Their bounces after the first, apart: the cosine rays' reading of both
// histories at their hits (see trace_bounce); alpha its relights over 64.
layout(set = 0, binding = 26, rgba16f) uniform restrict image2D indirect_dyn2_atlas;
// The static lights' radiance alone (the lighting atlas less the dynamic
// bounces), what the static cosine rays read at their hits, and in alpha
// the visibility ratio for the dynamic lights' direct term at the gather's
// hits. Subtracting the dynamic histories from the lighting atlas instead
// read the two from different moments of the same dispatch.
layout(set = 0, binding = 28, rgba16f) uniform restrict image2D static_atlas;

// The area lights (the population's, like the omni and spot buffers) and
// the atlas their textures live in, read through linear_sampler_mipmaps.
layout(set = 0, binding = 29, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;
layout(set = 0, binding = 30) uniform texture2D area_light_atlas;
// The projector textures (a spot's cookie, an omni's dual paraboloid map),
// sampled where a light's projector_rect is set (see projector_factor).
layout(set = 0, binding = 31) uniform texture2D decal_atlas_srgb;

// Diagnostics (GODOT_CARD_ABLATE=stats): the dynamic rays' fate, counted
// (see trace_dynamic; surface_cache.cpp prints and clears it).
layout(set = 0, binding = 25, std430) restrict buffer DynStats {
	// [0..15] the dynamic rays' fate (dyn_stat); [16..19] and [20..23] the
	// static bounce ray's source, counts and luminance sums (tier_stat);
	// [24..28] why a card lookup failed (card_reject); [29..30] the count and
	// luminance of the bounces read through a failed depth test.
	uint count[32];
}
dyn_stats;

// Diagnostics (GODOT_GI_TIER_PRINT, debug bit 8192): where a texel's static
// bounce ray got its radiance. i: 0 a card, 1 the SDFGI probes at a hit
// without a card, 2 the sky at such a hit without probes, 3 a miss (sky).
// Luminance sums are in 1/16 units.
float luminance(vec3 c);
void tier_stat(uint i, vec3 radiance) {
	if ((params.debug & 8192u) != 0u) {
		atomicAdd(dyn_stats.count[16u + i], 1u);
		atomicAdd(dyn_stats.count[20u + i], uint(min(luminance(max(radiance, vec3(0.0))), 64.0) * 16.0));
	}
}

struct Change {
	vec3 unshadowed; // The static lights' unshadowed term (the dynamic lights' is in indirect_dyn_atlas.a).
	float change; // The whole lighting's change, for the GI gather's screen history.
	float change_static; // The static lights' change, for the bounce accumulation's restart.
	float geom;
	float vis;
	uint bounce_set; // The last bounce ray's hit set, 0xFFFFu for none.
	float bounce_t; // Its hit distance.
};

Change change_load(ivec2 texel) {
	uvec4 p = imageLoad(change_atlas, texel);
	Change c;
	c.unshadowed.rg = unpackHalf2x16(p.x);
	c.unshadowed.b = unpackHalf2x16(p.y & 0xFFFFu).x;
	c.change = float((p.y >> 16u) & 0xFFu) / 255.0;
	c.change_static = float((p.y >> 24u) & 0xFFu) / 255.0;
	vec2 gv = unpackHalf2x16(p.z);
	c.geom = gv.x;
	c.vis = gv.y;
	c.bounce_set = p.w >> 16u;
	c.bounce_t = unpackHalf2x16(p.w).x;
	return c;
}

void change_store(ivec2 texel, Change c) {
	uint y = (packHalf2x16(vec2(c.unshadowed.b, 0.0)) & 0xFFFFu) | (uint(clamp(c.change, 0.0, 1.0) * 255.0 + 0.5) << 16u) | (uint(clamp(c.change_static, 0.0, 1.0) * 255.0 + 0.5) << 24u);
	imageStore(change_atlas, texel, uvec4(packHalf2x16(c.unshadowed.rg), y, packHalf2x16(vec2(c.geom, c.vis)), (packHalf2x16(vec2(c.bounce_t, 0.0)) & 0xFFFFu) | (c.bounce_set << 16u)));
}

// The static lights' change (the bounce accumulation's restart), and the
// whole lighting's (the GI gather's, which reads it the same way): the two
// bytes above the second uint's half.
float change_load_gradient(ivec2 texel) {
	return float((imageLoad(change_atlas, texel).y >> 24u) & 0xFFu) / 255.0;
}

float change_load_total(ivec2 texel) {
	return float((imageLoad(change_atlas, texel).y >> 16u) & 0xFFu) / 255.0;
}

// The world light grid: per cell, a count then grid_cap entries (a light
// index, bit 31 set for a spot). See surface_cache_grid.glsl.
layout(set = 0, binding = 22, std430) restrict readonly buffer LightGrid {
	uint data[];
}
light_grid;
#define GRID_SPOT_BIT 0x80000000u

uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
	return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

float luminance(vec3 c) {
	return dot(c, params.luma_weights.rgb);
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

#include "../area_light_diffuse_inc.glsl"

vec3 sky_eval(vec3 world_dir) {
	if (bool(params.flags & FLAG_SKY_MODE_SKY)) {
		vec4 q = params.sky_quat_or_color;
		vec3 t = cross(q.xyz, world_dir);
		vec3 dir = world_dir + ((t * q.w) + cross(q.xyz, t)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sampler2DArray(sky_radiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(dir, params.sky_border), 0.0), 3.0).rgb * params.sky_energy;
#else
		return textureLod(sampler2D(sky_radiance, linear_sampler_mipmaps), vec3_to_oct_with_border(dir, params.sky_border), 3.0).rgb * params.sky_energy;
#endif
	} else if (bool(params.flags & FLAG_SKY_MODE_COLOR)) {
		return params.sky_quat_or_color.rgb * params.sky_energy;
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

// The SDFGI lightprobe irradiance at a camera-relative position (the same
// evaluation the gather and the deferred resolve use). False outside every
// cascade.
bool sdfgi_probe_irradiance(vec3 rel_pos, vec3 normal, out vec3 r_irradiance) {
	r_irradiance = vec3(0.0);
	if (!bool(params.flags & FLAG_SDFGI)) {
		return false;
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
		r_irradiance = accum.rgb * sdfgi.cascades[c].exposure_normalization * sdfgi.energy;
		return true;
	}
	return false;
}

// The gather's card lookup (stochastic_indirect_gi.glsl surface_cache_lookup),
// over this pass's own bindings: nearest texel of the lit atlas.
// Diagnostics (tier_stat): why the last card_lookup() failed. 0 the hit's
// instance is not in the cache, 1 it has no card set, 2 the set is not
// captured yet (or too small), 3 no card faces the ray with a filled texel
// under the hit, 4 a card does but its stored depth disagrees with the hit.
uint card_reject = 0u;

bool card_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance, out uint r_set, out float r_change, out float r_change_total, out vec3 r_n_world, out vec3 r_albedo, out ivec2 r_texel) {
	r_radiance = vec3(0.0);
	r_set = SURFACE_CACHE_INVALID;
	r_change = 0.0;
	r_change_total = 0.0;
	r_n_world = vec3(0.0);
	r_albedo = vec3(0.0);
	r_texel = ivec2(0);
	card_reject = 0u;
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	card_reject = 1u;
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	r_set = inst.set;
	CardSet s = sets.data[inst.set];
	card_reject = 2u;
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return false;
	}
	card_reject = 3u;
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	ivec2 best_texel = ivec2(0);
	uint best_k = 0u;
	// The best-facing card with a filled texel under the hit whatever its
	// stored depth says: the stand-in when every card fails the depth test.
	float alt_w = 0.0;
	ivec2 alt_texel = ivec2(0);
	uint alt_k = 0u;
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
		uint packed = sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		if (facing > alt_w) {
			alt_w = facing;
			alt_texel = texel;
			alt_k = k;
		}
		// The box's longest extent over the card's longer edge, as when the
		// cards were square: a card's own (shorter) texel made the tolerance
		// reject grazing hits that then paid for the probe fallback.
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			card_reject = 4u;
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_texel = texel;
			best_k = k;
		}
	}
	if (best_w <= 0.0) {
		// Every facing card disagrees on depth: the hit is on another layer
		// of the same instance than the card captured (a back face reached
		// from inside a wall, a face behind the captured one, a corner
		// within a texel). A tenth of the bounce rays end here on the
		// closed box and the game room alike, and the SDFGI probe that
		// used to stand in read 130x darker than the card rays in the box
		// and 4.5x brighter in the game. The card's own texel -- the same
		// instance and material, lit by the same lights -- is the nearer
		// answer, and the one that stays when SDFGI is off.
		// GODOT_CARD_ABLATE=strict (debug bit 16384) keeps the rejection.
		if (alt_w <= 0.0 || (params.debug & 16384u) != 0u) {
			return false;
		}
		best_w = alt_w;
		best_texel = alt_texel;
		best_k = alt_k;
		card_reject = 5u;
	}
	r_radiance = imageLoad(lighting_atlas, best_texel).rgb;
	r_change = change_load_gradient(best_texel);
	r_change_total = change_load_total(best_texel);
	r_albedo = texelFetch(albedo_atlas, best_texel, 0).rgb;
	r_texel = best_texel;
	// The captured normal, as read_texel decodes it: the dynamic bounce needs
	// the surface's orientation at the hit for its geometry terms.
	vec3 n_cam = normalize(texelFetch(normal_atlas, best_texel, 0).rgb * 2.0 - 1.0);
	vec3 axis, u, v;
	card_basis(best_k, axis, u, v);
	r_n_world = normalize(mat3(s.world_from_local) * (u * n_cam.x + v * n_cam.y + axis * n_cam.z));
	return true;
}

vec3 basis_around(vec3 n, vec2 rnd) {
	vec3 t = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 b1 = normalize(cross(n, t));
	vec3 b2 = cross(n, b1);
	float phi = rnd.x * 2.0 * M_PI;
	float r = sqrt(rnd.y);
	return normalize(b1 * (r * cos(phi)) + b2 * (r * sin(phi)) + n * sqrt(max(1.0 - rnd.y, 0.0)));
}


// Coverage of a non-opaque candidate hit (an alpha-tested caster), from its
// instance's cards: the card facing the ray most squarely whose stored depth
// agrees with the hit, and its captured albedo alpha at that texel. A hit
// with no card, or none agreeing, counts as covered, so a thick object never
// leaks; only a texel the material left below half alpha lets the ray on.
bool card_covers(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir) {
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardSet s = sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return true;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	float best_alpha = 1.0;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = abs(dot(axis, local_dir)); // Either side: the ray may come from behind the leaf.
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_alpha = texelFetch(albedo_atlas, texel, 0).a;
		}
	}
	if (best_w <= 0.0) {
		return true;
	}
	return best_alpha >= 0.5;
}

bool occluded(vec3 origin, vec3 dir, float t_max, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			if (card_covers(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), origin + dir * rayQueryGetIntersectionTEXT(rq, false), dir)) {
				rayQueryConfirmIntersectionEXT(rq);
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Occlusion as the bounce rays see it: everything opaque, the first hit ends
// the query (the dynamic bounce's connection, see trace_dynamic).
bool occluded_opaque(vec3 origin, vec3 dir, float t_max) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// The local lights that reach a point: its cell of the world light grid
// (every light that reaches it), or the set's list (the first 32
// overlapping its box) where the grid is off or the point lies outside it.
void light_list(uint entry, vec3 world_pos, out uint r_base, out uint r_count, out bool r_from_grid) {
	r_base = entry * (1u + MAX_LIGHTS_PER_SET);
	r_count = min(set_lights.data[r_base], MAX_LIGHTS_PER_SET);
	r_from_grid = false;
	if (bool(params.flags & FLAG_GRID)) {
		vec3 rel = (world_pos - params.grid_origin) / params.grid_cell;
		if (all(greaterThanEqual(rel, vec3(0.0))) && all(lessThan(rel, vec3(float(params.grid_n))))) {
			uvec3 c = uvec3(rel);
			r_base = (c.x + params.grid_n * (c.y + params.grid_n * c.z)) * (1u + params.grid_cap);
			r_count = min(light_grid.data[r_base], params.grid_cap);
			r_from_grid = true;
		}
	}
	if ((params.debug & 4u) != 0u) {
		r_count = 0u;
	}
}

LightData light_at(uint base, uint j, bool from_grid, out bool r_is_spot) {
	uint idx = from_grid ? light_grid.data[base + 1u + j] : set_lights.data[base + 1u + j];
	r_is_spot = from_grid ? (idx & GRID_SPOT_BIT) != 0u : idx >= params.omni_light_count;
	if (from_grid) {
		idx &= ~GRID_SPOT_BIT;
	} else if (r_is_spot) {
		idx -= params.omni_light_count;
	}
	return r_is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
}

// A local light's unshadowed contribution at a point: its colour times the
// cosine, the attenuation and the cone, over pi (the direct pass's terms).
// r_pos is the light's world position, r_geom the geometric factor
// (attenuation times cosine) the geometric gradient sums.
// A light's projector texture at a world point (the direct pass's mapping:
// a spot's cookie through a perspective projection, an omni's map through
// the dual paraboloid), read through the cards' own world-space projector
// matrix (light_storage.cpp _fill_card_light_data). One where the light has
// none.
vec3 projector_factor(LightData ld, bool is_spot, vec3 world_pos) {
	if (ld.projector_rect == vec4(0.0)) {
		return vec3(1.0);
	}
	vec4 proj;
	if (is_spot) {
		vec4 splane = ld.shadow_matrix * vec4(world_pos, 1.0);
		splane /= splane.w;
		vec2 proj_uv = splane.xy * ld.projector_rect.zw;
		proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), proj_uv + ld.projector_rect.xy, 0.0);
	} else {
		vec3 local_v = normalize((ld.shadow_matrix * vec4(world_pos, 1.0)).xyz);
		vec4 atlas_rect = ld.projector_rect;
		if (local_v.z >= 0.0) {
			atlas_rect.y += atlas_rect.w;
		}
		local_v.z = 1.0 + abs(local_v.z);
		local_v.xy /= local_v.z;
		local_v.xy = local_v.xy * 0.5 + 0.5;
		vec2 proj_uv = local_v.xy * atlas_rect.zw;
		proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), proj_uv + atlas_rect.xy, 0.0);
	}
	return proj.rgb * proj.a;
}

vec3 light_contribution_world(LightData ld, bool is_spot, vec3 light_pos, vec3 spot_dir, vec3 world_pos, vec3 n, out float r_geom) {
	vec3 rel = light_pos - world_pos;
	float len = length(rel);
	float attenuation = get_omni_attenuation(len, ld.inv_radius, ld.attenuation);
	vec3 l = rel / max(len, 1e-5);
	if (is_spot) {
		float scos = max(dot(-l, spot_dir), ld.cone_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
		attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
	}
	float ndotl = max(dot(n, l), 0.0);
	r_geom = ndotl * attenuation;
	if (r_geom <= 0.0) {
		return vec3(0.0);
	}
	return ld.color * projector_factor(ld, is_spot, world_pos) * (r_geom * (1.0 / M_PI));
}

// The scene's light buffers hold view-space positions and directions.
vec3 light_contribution(LightData ld, bool is_spot, vec3 world_pos, vec3 n, out vec3 r_pos, out float r_geom) {
	r_pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
	vec3 spot_dir = is_spot ? normalize(mat3(params.world_from_view) * ld.direction) : vec3(0.0, -1.0, 0.0);
	return light_contribution_world(ld, is_spot, r_pos, spot_dir, world_pos, n, r_geom);
}

// The dynamic lights' unshadowed direct term at a point, each by its weight.
vec3 dynamic_direct(vec3 world_pos, vec3 n) {
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		float geom;
		sum += light_contribution_world(ld, ld.pad > 0.5, ld.position, normalize(ld.direction), world_pos, n, geom) * dyn_lights.weights[i >> 2u][i & 3u];
	}
	return sum;
}

struct Texel {
	vec3 world_pos;
	vec3 n_world;
	vec3 origin;
	vec3 albedo;
	vec3 emission;
};

// The texel's position, normal and material back out of the capture. False
// where nothing was captured.
bool read_texel(CardSet s, uint card, ivec2 dims, ivec2 texel_in_card, ivec2 texel, out Texel t) {
	float depth = texelFetch(depth_atlas, texel, 0).r;
	if (depth <= 0.0) {
		return false;
	}
	t.albedo = texelFetch(albedo_atlas, texel, 0).rgb;
	vec3 n_cam = normalize(texelFetch(normal_atlas, texel, 0).rgb * 2.0 - 1.0);
	t.emission = texelFetch(emission_atlas, texel, 0).rgb;
	vec2 uv01 = (vec2(texel_in_card) + 0.5) / vec2(dims);
	vec3 local_pos = card_local_point(s, card, uv01, depth);
	vec3 axis, u, v;
	card_basis(card, axis, u, v);
	vec3 n_local = u * n_cam.x + v * n_cam.y + axis * n_cam.z;
	t.world_pos = (s.world_from_local * vec4(local_pos, 1.0)).xyz;
	t.n_world = normalize(mat3(s.world_from_local) * n_local);
	t.origin = t.world_pos + t.n_world * params.ray_bias;
	return true;
}

// Direct light at the texel: the sun, and the lights culled to the set's box.
// The direct term in its parts: the directional lights with their one hard
// shadow ray each (deterministic: the same ray every relight, so exact, no
// accumulation), the local lights' unshadowed analytic sum (exact) with its
// geometric sum, and one visibility sample of a light drawn from them (the
// only stochastic part, accumulated as a ratio by accumulate()). The whole
// unshadowed term is returned for the radiance gradient.
struct Direct {
	vec3 exact; // The shadowed directional term.
	vec3 unshadowed; // Every light, unshadowed.
	vec3 local_sum; // The local lights, unshadowed.
	vec3 dyn_sum; // The dynamic lights among them, unshadowed (a part of local_sum).
	float local_geom; // Their geometric sum.
	float vis; // The drawn light's visibility, when one was drawn.
	bool sampled;
};

void shade_direct(uint entry, Texel t, inout uint seed, out Direct d) {
	vec3 direct = vec3(0.0);
	vec3 direct_unshadowed = vec3(0.0);
	d.local_geom = 0.0;
	d.vis = 1.0;
	d.sampled = false;

	// Directional lights: a shadow ray each. (Folding the sun into the
	// estimator below was measured to save nothing: its ray is cheap.)
	uint directional_count = (params.debug & 8u) != 0u ? 0u : params.directional_light_count;
	for (uint i = 0u; i < directional_count; i++) {
		DirectionalLightData dl = directional_lights.data[i];
		vec3 l = normalize(mat3(params.world_from_view) * dl.direction);
		float ndotl = dot(t.n_world, l);
		if (ndotl <= 0.0) {
			continue;
		}
		vec3 c = dl.color * dl.energy * (ndotl * (1.0 / M_PI));
		direct_unshadowed += c;
		if (dl.shadow_opacity <= 0.001 || (params.debug & 2u) != 0u) {
			direct += c;
			continue;
		}
		float vis = mix(1.0, occluded(t.origin, l, 1e4, 0xFFu) ? 0.0 : 1.0, dl.shadow_opacity);
		direct += c * vis;
	}

	// Omni and spot lights overlapping the set's box: the full analytic sum,
	// times the visibility of one light drawn in proportion to its
	// contribution. With the weights equal to the contributions the ratio is
	// the drawn light's visibility, as in the direct pass.
	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	vec3 sel_pos = vec3(0.0);
	float sel_opacity = 0.0;
	uint sel_mask = 0u;
	bool selected = false;

	uint base;
	uint light_count;
	bool from_grid;
	light_list(entry, t.world_pos, base, light_count, from_grid);
	for (uint j = 0u; j < light_count; j++) {
		bool is_spot;
		LightData ld = light_at(base, j, from_grid, is_spot);
		vec3 pos;
		float geom;
		vec3 c = light_contribution(ld, is_spot, t.world_pos, t.n_world, pos, geom);
		float w = luminance(abs(c));
		if (w <= 0.0) {
			continue;
		}
		sum += c;
		d.local_geom += geom;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = pos;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	// Area lights, in the same estimator: their LTC diffuse term, with the
	// shadow ray to a point drawn uniformly on the rect (a soft shadow over
	// the relights). Not culled: a scene holds a few, and the range test in
	// the term is the first thing it does.
	uint area_count = (params.debug & 4u) != 0u ? 0u : params.area_light_count;
	for (uint j = 0u; j < area_count; j++) {
		LightData ld = area_lights.data[j];
		seed = pcg_hash(seed);
		float xi0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float xi1 = hash_to_float(seed);
		float geom;
		vec3 point;
		vec3 c = area_light_contribution(ld, params.world_from_view, t.world_pos, t.n_world, vec2(xi0, xi1), area_light_atlas, linear_sampler_mipmaps, geom, point);
		float w = luminance(abs(c));
		// Written as the negation so a NaN term is rejected too (every
		// comparison with a NaN is false): the accumulation below keeps
		// whatever enters it for the texel's lifetime, and a single NaN
		// texel poisons every ray that reads it, then the gather's history
		// and its spatial filter, which spreads it a stride further every
		// frame (growing black voids over the whole frame).
		if (!(w > 0.0)) {
			if ((params.debug & 8192u) != 0u && (isnan(w) || isinf(w))) {
				atomicAdd(dyn_stats.count[31], 1u); // Diagnostics (GODOT_GI_TIER_PRINT): the area terms rejected as NaN.
			}
			continue;
		}
		sum += c;
		d.local_geom += geom;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = point;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	direct_unshadowed += sum;
	if (selected) {
		float vis = 1.0;
		if (sel_opacity > 0.001 && sel_mask != 0u && (params.debug & 2u) == 0u) {
			vec3 to_light = sel_pos - t.origin;
			float dist = length(to_light);
			vis = mix(1.0, occluded(t.origin, to_light / max(dist, 1e-5), max(dist - params.ray_bias, 0.0), sel_mask) ? 0.0 : 1.0, sel_opacity);
		}
		d.vis = vis;
		d.sampled = true;
	}
	d.exact = direct;
	d.unshadowed = direct_unshadowed;
	d.local_sum = sum;
	d.dyn_sum = dynamic_direct(t.world_pos, t.n_world);
}

// The dynamic lights' bounce. A flashlight's spot on the floor is small
// and bright, and its bounce on the ceiling is what a cosine ray per texel
// cannot estimate: one texel in a few lands a ray on the spot per relight,
// and the field over the ceiling is a mottle of texels that saw it and
// texels that did not -- coherent across every mip the hits read it
// through, so no screen-space filter averages it -- that sixty-four
// relights average out, which a light that keeps moving never gives them,
// and that every move restarts (the change of a lit texel is carried by
// the rays that hit it and restarts the readers to one sample, at random
// relights over the eight the mark lasts; the static lamps' converged
// bounce goes with it). So a light that moves or changes is estimated
// apart, with a history of its own that follows the light's motion (see
// accumulate) and needs no restart. Two strategies estimate its first
// bounce, combined by the balance heuristic: rays from the light, uniform
// over its cone (or the sphere), landing on the lit surface and connected
// to the texel by a shadow ray (every texel sees the spot every relight,
// the density known exactly); and the static cosine rays, whose hit
// carries the light's direct term analytically (see trace_bounce) -- the
// better strategy for a lamp lighting a whole wall, where a light ray is
// one sample of a wide area. The lighting atlas holds no dynamic light's
// direct term (see accumulate), so nothing is subtracted against a stale
// state; the cosine rays read the static lights' radiance plus the
// dynamic lights' bounces, and hand the first of those to the dynamic
// term's second-bounce history, so it too follows the light closely; the
// third and after stay with the static accumulation. A light is dynamic
// while LightStorage has seen it change lately (GODOT_CARD_DYN_HOLD
// frames), then its weight fades (GODOT_CARD_DYN_FADE) and the static
// accumulation absorbs its bounce as slowly as the weight leaves.
//
// Directions come from the R2 sequence per texel, advanced per relight: the
// samples of one texel's window tile the cone rather than clump. The
// squared distance of the connection is softened by a centimetre.
void dyn_stat(uint i, bool ceiling) {
	if ((params.debug & 4096u) != 0u) {
		atomicAdd(dyn_stats.count[i], 1u);
		if (ceiling) {
			atomicAdd(dyn_stats.count[i + 8u], 1u);
		}
	}
}

void trace_dynamic(ivec2 texel, Texel t, float n_cosine, float n_light, out vec3 dyn_sample, out float landed, out float change_total) {
	dyn_sample = vec3(0.0);
	landed = 0.0;
	change_total = 0.0;
	bool ceiling = t.n_world.y < -0.7;
	uint n_rays = uint(n_light);
	if (dyn_lights.count == 0u || n_rays == 0u || (params.debug & 1u) != 0u) {
		return;
	}
	uint h = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) ^ 0x2545F491u));
	float o0 = hash_to_float(h);
	float o1 = hash_to_float(pcg_hash(h));
	float inv_rays = 1.0 / float(n_rays);
	float share = inv_rays / float(dyn_lights.count);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		bool is_spot = ld.pad > 0.5;
		float weight = dyn_lights.weights[i >> 2u][i & 3u];
		if (ld.inv_radius <= 0.0 || weight <= 0.0) {
			continue;
		}
		vec3 pos = ld.position;
		float range = 1.0 / ld.inv_radius;
		vec3 axis = is_spot ? normalize(ld.direction) : vec3(0.0, 0.0, 1.0);
		float cone_cos = is_spot ? clamp(ld.cone_angle, -1.0, 0.9999) : -1.0;
		float pdf_dir = 1.0 / (2.0 * M_PI * (1.0 - cone_cos));
		vec3 tng = abs(axis.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
		vec3 b1 = normalize(cross(axis, tng));
		vec3 b2 = cross(axis, b1);
		for (uint sidx = 0u; sidx < n_rays; sidx++) {
			float k = float(((params.frame & 63u) * n_rays + sidx) * (i + 1u));
			float r0 = fract(o0 + k * 0.7548776662);
			float r1 = fract(o1 + k * 0.5698402910);
			float phi = r1 * 2.0 * M_PI;
			float ct = 1.0 - r0 * (1.0 - cone_cos);
			float st = sqrt(max(1.0 - ct * ct, 0.0));
			vec3 dir = normalize(b1 * (st * cos(phi)) + b2 * (st * sin(phi)) + axis * ct);
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, pos, 0.0, dir, range);
			while (rayQueryProceedEXT(rq)) {
			}
			dyn_stat(0u, ceiling);
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
				continue;
			}
			dyn_stat(1u, ceiling);
			float d_lp = rayQueryGetIntersectionTEXT(rq, true);
			vec3 p = pos + dir * d_lp;
			vec3 unused_radiance;
			uint hit_set;
			float unused_change;
			float hit_change_total;
			vec3 n_p;
			vec3 albedo_p;
			ivec2 texel_p;
			if (!card_lookup(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true), p, dir, unused_radiance, hit_set, unused_change, hit_change_total, n_p, albedo_p, texel_p)) {
				continue;
			}
			dyn_stat(2u, ceiling);
			float cos_pl = dot(n_p, -dir);
			if (cos_pl <= 0.0 || d_lp < 1e-4) {
				continue;
			}
			dyn_stat(3u, ceiling);
			float unused_geom;
			vec3 c_p = light_contribution_world(ld, is_spot, pos, axis, p, n_p, unused_geom);
			// The landing's radiance from this light's direct term alone.
			vec3 l_dyn = albedo_p * c_p * weight;
			float pdf_area = pdf_dir * cos_pl / (d_lp * d_lp);
			card_requests.frame[hit_set] = params.frame;
			change_total = max(change_total, hit_change_total - 0.25);
			// The texel connected to the landing. The estimator of the
			// irradiance over pi is radiance * geom / (pi * pdf_area); its
			// balance-heuristic weight against the cosine rays (n_cosine of
			// them, density geom / pi per unit area at the landing) folds in
			// as radiance * geom / (pi * rays * pdf_area + n_cosine * geom),
			// which a landing beside the texel cannot blow up.
			{
				vec3 rel = p - t.origin;
				float d = length(rel);
				vec3 l = rel / max(d, 1e-4);
				float cos_t = dot(t.n_world, l);
				float cos_pt = dot(n_p, -l);
				dyn_stat(4u, ceiling && cos_t > 0.0);
				dyn_stat(5u, ceiling && cos_t > 0.0 && cos_pt > 0.0);
				if (d >= 1e-4 && cos_t > 0.0 && cos_pt > 0.0 && !occluded_opaque(t.origin, l, max(d - params.ray_bias, 0.0))) {
					dyn_stat(6u, ceiling);
					float geom = cos_t * cos_pt / (d * d + 0.01);
					dyn_sample += l_dyn * (geom / (M_PI * n_light * pdf_area + n_cosine * geom));
					landed += share;
				}
			}
		}
	}
}

// Diagnostics (paint3): the dynamic term's share of the sample, see accumulate.

// The indirect term of the static lights: one cosine ray into the scene
// (see main), the card's radiance at its hit less the dynamic lights' part.
// r_dyn1 / r_dyn2: the cosine ray's share of the dynamic lights' first and
// second bounce (see trace_dynamic), n_cosine rays of it this relight.
void trace_bounce(Texel t, inout uint seed, float n_cosine, float n_light, out vec3 indirect_sample, out vec3 r_dyn1, out vec3 r_dyn2, out float bounce_change, out float bounce_change_total, out uint hit_set_id, out float hit_t) {
	indirect_sample = vec3(0.0);
	r_dyn1 = vec3(0.0);
	r_dyn2 = vec3(0.0);
	bounce_change = 0.0;
	bounce_change_total = 0.0;
	hit_set_id = 0xFFFFu;
	hit_t = 0.0;
	if ((params.debug & 1u) != 0u) {
		// Ablated: the probes or the sky stand in for the ray.
		if (!sdfgi_probe_irradiance(t.world_pos - params.camera_origin.xyz, t.n_world, indirect_sample)) {
			indirect_sample = sky_eval(t.n_world);
		}
	} else {
		seed = pcg_hash(seed);
		float r0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float r1 = hash_to_float(seed);
		vec3 ray_dir = basis_around(t.n_world, vec2(r0, r1));
		rayQueryEXT rq;
		// Opaque: alpha-tested casters occlude the bounce ray whole (the
		// shadow rays above consult the cards' coverage; the bounce is a
		// diffuse term and the lookups cost a millisecond on the game project).
		rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, t.origin, 0.0, ray_dir, 1e4);
		while (rayQueryProceedEXT(rq)) {
		}
		if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
			float t_hit = rayQueryGetIntersectionTEXT(rq, true);
			uint hit_instance = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
			vec3 card_radiance;
			uint hit_set;
			float hit_change;
			float hit_change_total;
			vec3 n_hit;
			vec3 albedo_hit;
			ivec2 texel_hit;
			hit_t = t_hit;
			vec3 hit_pos = t.origin + ray_dir * t_hit;
			if (card_lookup(hit_instance, hit_pos, ray_dir, card_radiance, hit_set, hit_change, hit_change_total, n_hit, albedo_hit, texel_hit)) {
				hit_set_id = hit_set & 0xFFFFu;
				if (dyn_lights.count > 0u) {
					// The atlas holds no dynamic light's direct term (see
					// accumulate): the ray reads the static lights' radiance
					// and the dynamic lights' bounces. The first of those is
					// the second-bounce history's (r_dyn2); it comes out of
					// the static sample, from the same relight as the
					// radiance. The direct term at the hit, analytic, with
					// the card's visibility ratio for its shadow, is this
					// ray's estimate of the first bounce, weighted against
					// the light rays by the balance heuristic (each light's
					// density at the hit: its cone's, times the cosine over
					// the distance squared, where the hit is in its cone and
					// range and faces it).
					// Both histories at the hit: the first bounce's and the
					// later bounces', so this reading is the second bounce
					// and, through the later history's own readings, every
					// one after (a bounce of lag apiece); the static
					// accumulation holds none of the dynamic lights' light.
					vec3 dyn_hit = max(imageLoad(indirect_dyn_atlas, texel_hit).rgb, vec3(0.0)) + max(imageLoad(indirect_dyn2_atlas, texel_hit).rgb, vec3(0.0));
					card_radiance = max(imageLoad(static_atlas, texel_hit).rgb, vec3(0.0));
					r_dyn2 = albedo_hit * dyn_hit;
					float vis_hit = imageLoad(static_atlas, texel_hit).a;
					float cos_ht = max(dot(n_hit, -ray_dir), 0.0);
					float p_cos = n_cosine * max(dot(t.n_world, ray_dir), 0.0) * cos_ht / (M_PI * max(t_hit * t_hit, 1e-4));
					for (uint i = 0u; i < dyn_lights.count; i++) {
						LightData ld = dyn_lights.data[i];
						bool is_spot = ld.pad > 0.5;
						vec3 axis = is_spot ? normalize(ld.direction) : vec3(0.0, 0.0, 1.0);
						float geom_unused;
						vec3 c = light_contribution_world(ld, is_spot, ld.position, axis, hit_pos, n_hit, geom_unused) * dyn_lights.weights[i >> 2u][i & 3u];
						if (luminance(c) <= 0.0) {
							continue;
						}
						float pdf_light = 0.0;
						if (n_light > 0.0 && ld.inv_radius > 0.0) {
							vec3 rel = ld.position - hit_pos;
							float d = length(rel);
							vec3 l = rel / max(d, 1e-4);
							float cos_hl = dot(n_hit, l);
							float cone_cos = is_spot ? clamp(ld.cone_angle, -1.0, 0.9999) : -1.0;
							if (d < 1.0 / ld.inv_radius && cos_hl > 0.0 && (!is_spot || dot(-l, axis) > cone_cos)) {
								pdf_light = cos_hl / (2.0 * M_PI * (1.0 - cone_cos) * d * d);
							}
						}
						r_dyn1 += albedo_hit * c * vis_hit * (p_cos / (p_cos + n_light * pdf_light));
					}
				}
				indirect_sample = card_radiance;
				tier_stat(0u, card_radiance);
				if (card_reject == 5u && (params.debug & 8192u) != 0u) {
					atomicAdd(dyn_stats.count[29u], 1u); // Read through a depth mismatch (see card_lookup).
					atomicAdd(dyn_stats.count[30u], uint(min(luminance(max(card_radiance, vec3(0.0))), 64.0) * 16.0));
				}
				card_requests.frame[hit_set] = params.frame;
				// The bounce carries the change of the card it came from,
				// weaker by a quarter per bounce, so lighting that reaches
				// this texel only indirectly restarts it too.
				bounce_change = hit_change - 0.25;
				bounce_change_total = hit_change_total - 0.25;
			} else {
				if ((params.debug & 8192u) != 0u) {
					atomicAdd(dyn_stats.count[24u + min(card_reject, 4u)], 1u); // Why the lookup failed (card_reject), 5 slots at 24..28.
				}
				if (sdfgi_probe_irradiance(t.world_pos - params.camera_origin.xyz, t.n_world, indirect_sample)) {
					tier_stat(1u, indirect_sample);
				} else {
					indirect_sample = sky_eval(t.n_world);
					tier_stat(2u, indirect_sample);
				}
			}
		} else {
			indirect_sample = sky_eval(ray_dir);
			tier_stat(3u, indirect_sample);
		}
	}
}

// The young texel's extra rays. A lighting change restarts the bounce
// accumulation (see accumulate), and for the relights after it the texel is
// one sample, then a few: every gather ray landing near it reads that same
// sample, so its noise is not per pixel but a mottle over the whole surface,
// which no screen-space filter averages and the cards' mip levels only
// spread. More rays where the history is young buy the samples back at the
// restart, and cost nothing where it is not. (A moving light's bounce is
// the dynamic term's now, see trace_dynamic; this is for the rest: a fresh
// capture, a lamp switched, a surface moved.)
#define YOUNG_RELIGHTS 8.0

// The bounce atlas's alpha: the relights (whole, up to 64) over 64, and in
// the half-relight under them the hand-over fraction of a light joining
// the dynamic set (see accumulate), about five bits of it. Readers that
// want a relight count take the alpha times 64 as it is: the fraction is
// under half a relight.
float ind_pack(float relights, float join) {
	return (relights + 0.5 * clamp(join, 0.0, 1.0)) / 64.0;
}

vec2 ind_unpack(float a) {
	float x = a * 64.0;
	float relights = floor(x + 1e-3);
	return vec2(relights, clamp((x - relights) * 2.0, 0.0, 1.0));
}

bool texel_young(ivec2 texel) {
	return imageLoad(indirect_atlas, texel).a * 64.0 < YOUNG_RELIGHTS;
}

// The dynamic histories under YOUNG_RELIGHTS (their age is the second one's
// alpha, see accumulate): a light that moved, and the relights after it.
bool texel_young_dynamic(ivec2 texel) {
	return dyn_lights.count > 0u && imageLoad(indirect_dyn2_atlas, texel).a * 64.0 < YOUNG_RELIGHTS;
}

// The light rays this relight: two at least while the dynamic histories are young.
float light_rays(bool young_dynamic) {
	return float(params.dynamic_rays == 0u ? 0u : (young_dynamic ? max(params.dynamic_rays, 2u) : params.dynamic_rays));
}

// How many cosine rays the texel traces this relight (see trace_bounce_young):
// the extra ones for a young accumulation, and while a dynamic light moves
// (its histories are then a relight or two long, and the cosine rays are
// the better half of its estimate wherever it lights a wide area).
uint cosine_rays(bool young, bool young_dynamic) {
	bool dynamic_young = (params.flags & FLAG_DYNAMIC_YOUNG) != 0u && young_dynamic;
	return ((young || dynamic_young) && (params.debug & 1u) == 0u) ? params.young_rays + 1u : 1u;
}

void trace_bounce_young(uint n_cosine, float n_light, Texel t, inout uint seed, inout vec3 indirect_sample, inout vec3 dyn1, inout vec3 dyn2, inout float bounce_change, inout float bounce_change_total) {
	if (n_cosine <= 1u) {
		return;
	}
	for (uint r = 1u; r < n_cosine; r++) {
		vec3 extra;
		vec3 extra_dyn1;
		vec3 extra_dyn2;
		float extra_change;
		float extra_total;
		uint extra_set;
		float extra_t;
		trace_bounce(t, seed, float(n_cosine), n_light, extra, extra_dyn1, extra_dyn2, extra_change, extra_total, extra_set, extra_t);
		indirect_sample += extra;
		dyn1 += extra_dyn1;
		dyn2 += extra_dyn2;
		bounce_change = max(bounce_change, extra_change);
		bounce_change_total = max(bounce_change_total, extra_total);
	}
	indirect_sample /= float(n_cosine);
	dyn1 /= float(n_cosine);
	dyn2 /= float(n_cosine);
}

// The bounce gradient: the previous relight's ray traced again from the same
// texel with the same seed, what it hits now against what it hit then
// (stored per texel): another set, or the same one at a distance changed by
// more than a tenth, is a surface that moved into or out of the ray's way.
// A ray that finds the same surface at the same distance gives nothing,
// whatever its radiance did. Off for a fresh texel and when the set has no
// previous relight.
float bounce_gradient(ivec2 texel, Texel t, uint prev_seed, bool have_prev) {
	if (!have_prev || (params.debug & 16u) != 0u) {
		return -1.0;
	}
	Change prev = change_load(texel);
	vec4 old_indirect = imageLoad(indirect_atlas, texel);
	if (old_indirect.a <= 0.0) {
		return -1.0;
	}
	vec3 again;
	float unused_change;
	uint set_now;
	float t_now;
	uint seed = prev_seed;
	float unused_total;
	vec3 unused_dyn1;
	vec3 unused_dyn2;
	trace_bounce(t, seed, 1.0, 0.0, again, unused_dyn1, unused_dyn2, unused_change, unused_total, set_now, t_now);
	if (set_now != prev.bounce_set) {
		return 1.0;
	}
	if (set_now == 0xFFFFu && (t_now <= 0.0 || prev.bounce_t <= 0.0)) {
		return 0.0; // The sky both times.
	}
	float rel = abs(t_now - prev.bounce_t) / max(max(t_now, prev.bounce_t), 0.05);
	return smoothstep(0.05, 0.3, rel);
}

// The temporal gradients and the accumulations, into the atlases. The card's
// radiance is assembled here every relight from an exact direct term and two
// accumulated factors, never accumulated itself: a light's colour or
// intensity changing, or a light moving, shows in the cards the frame the
// set is relit, and the histories hold only what is stochastic -- the local
// lights' visibility ratio and the bounce.
// bounce_gradient is the re-traced previous ray's relative change (or a
// negative value when there was no previous ray to re-trace); card_min /
// card_max bound the card, so the change read from the neighbours never
// crosses into another card packed beside it in the atlas.
void accumulate(ivec2 texel, Texel t, bool reset, Direct d, vec3 indirect_sample, float bounce_change, float bounce_change_total, vec3 dyn_sample, vec3 dyn2_sample, float dyn_landed, float bounce_gradient, uint bounce_set, float bounce_t, ivec2 card_min, ivec2 card_max) {
	vec4 old = imageLoad(lighting_atlas, texel);
	Change prev = change_load(texel);
	// A fresh capture has nothing to compare with, and neither has a texel
	// never lit since its capture (no frames accumulated): the capture's
	// reset flag is raised on the frame its record is built, which is not
	// always the frame it is first lit.
	bool fresh = reset || old.a <= 0.0;

	// The radiance gradient: how much the deterministic part of this texel's
	// lighting moved since the last relight, relative to itself, per channel
	// and the largest taken (a hue turning at constant luminance is a change
	// the eye sees, and a luminance gradient read it as nothing: the cards
	// and the GI's history held the old hue for their whole window). Each
	// channel is relative to its own size, floored at a quarter of the
	// luminance so a channel that is nearly nothing cannot restart by
	// doubling (and the denominator is not the luminance: on a red wall
	// that read the red channel's move at five times its size, restarted
	// every history to one frame at every relight, and the glossy floor's
	// sparse reflections came out bright). Static lights give exactly zero
	// (the numbers are recomputed from the same inputs).
	// The static lights' term: the dynamic lights (see trace_dynamic) are
	// gradients of their own.
	vec3 unshadowed = t.albedo * (d.unshadowed - d.dyn_sum) + t.emission;
	float dyn_lum = luminance(t.albedo * d.dyn_sum);
	vec4 dyn_old = imageLoad(indirect_dyn_atlas, texel);
	vec3 delta = abs(unshadowed - prev.unshadowed);
	float lum_floor = 0.25 * max(luminance(unshadowed), luminance(prev.unshadowed));
	vec3 rel = delta / max(max(unshadowed, prev.unshadowed), vec3(max(lum_floor, 1e-4)));
	float change = fresh ? 0.0 : max(max(rel.r, rel.g), rel.b);
	// The whole lighting's change, for the GI gather's screen history: the
	// static change, and the dynamic lights' direct term here against the
	// last relight's (by luminance, floored at a quarter of the whole), and
	// what the rays carried from their hits.
	float total_lum = luminance(unshadowed) + dyn_lum;
	float prev_total_lum = luminance(prev.unshadowed) + dyn_old.a;
	float dyn_change = fresh ? 0.0 : abs(dyn_lum - dyn_old.a) / max(max(dyn_lum, dyn_old.a), max(0.25 * max(total_lum, prev_total_lum), 1e-4));
	if (!fresh && dyn_lum > 0.05 * total_lum) {
		dyn_change = max(dyn_change, params.dynamic_change);
	}
	float change_total = max(max(change, dyn_change), max(prev.change - 0.125, bounce_change_total));
	// The change outlives the relight that found it, fading over eight: the
	// gather's one ray per pixel lands on a given card only now and then,
	// and a change seen for one frame would restart almost no pixel.
	change = max(change, max(prev.change_static - 0.125, bounce_change));
	// The bounce gradient is one ray's verdict, so only the texels whose
	// last ray happened to see what moved would restart on their own and
	// the rest would hold the stale bounce beside them: the change spreads
	// to the eight neighbours, a texel per relight, fading as it goes.
	if (!fresh) {
		change = max(change, bounce_gradient);
		float spread = 0.0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (dx == 0 && dy == 0) {
					continue;
				}
				ivec2 n = texel + ivec2(dx, dy);
				if (any(lessThan(n, card_min)) || any(greaterThan(n, card_max))) {
					continue;
				}
				spread = max(spread, change_load_gradient(n));
			}
		}
		change = max(change, spread - 0.125);
	}
	change_total = max(change_total, change);

	// The geometric gradient: the local lights' geometric sum against the
	// last relight's. Only movement (or a light appearing or vanishing) can
	// change a texel's visibility; a colour or intensity change leaves the
	// ratio exactly right and is not a reason to lose its history.
	float geom_change = fresh ? 0.0 : abs(d.local_geom - prev.geom) / max(max(d.local_geom, prev.geom), 1e-6);

	// Two histories, each restarted to the frame count its gradient leaves
	// credible (A-SVGF: alpha = max(alpha, gradient)); a small change barely
	// touches them, a light switching costs them all their frames but one.
	// The window is twice the setting: before the ratio, the composite was
	// accumulated over the accumulated bounce, and the two cascaded windows
	// smoothed as much as one of double the length.
	float window = float(min(2u * params.temporal_frames, 64u));

	// The visibility ratio (one for every local light, dynamic or not; a
	// ratio of the dynamic lights' own, drawn on alternate relights, was
	// measured and changed nothing).
	float keep_vis = (geom_change > 0.02 && (params.debug & 64u) == 0u) ? max(1.0, 1.0 / geom_change) : 64.0;
	float frames = reset ? 0.0 : min(old.a * 64.0, keep_vis);
	float vis = prev.vis;
	if (d.sampled) {
		float alpha = max(1.0 / (frames + 1.0), 1.0 / window);
		vis = frames <= 0.0 ? d.vis : mix(prev.vis, d.vis, alpha);
	} else if (frames <= 0.0) {
		vis = 1.0; // No local light reaches this texel; the sum is zero anyway.
	}
	float vis_dyn = vis;
	frames = min(frames + 1.0, 64.0);
	Change now;
	now.unshadowed = unshadowed;
	now.change = change_total;
	now.change_static = change;
	now.geom = d.local_geom;
	now.vis = vis;
	now.bounce_set = bounce_set;
	now.bounce_t = bounce_t;
	change_store(texel, now);

	// The bounce, one ray per frame: restarted by the radiance gradient (a
	// light that changed here changed at the texels this one bounces off,
	// near enough) and by the change the ray's own hit carried.
	// The restart is A-SVGF's, to 1 / change relights, for the gradient as
	// for a light. Two softer forms were measured against rt_lab's moving
	// box and neither kept: a quarter-strength change (a restart to four
	// relights) left the descend at 0.058 eight frames after the stop, the
	// same as no gradient, because the spread to the neighbours dies a
	// texel out and the glow stays wherever no ray saw the box leave; and a
	// floor of four relights on the bounce alone, with the change carried
	// whole, cleared the glow as the full restart does and left the flicker
	// at rest exactly where it was (0.0050 against 0.0051 under the box
	// still sweeping) -- that flicker is the GI screen history restarting
	// at the pixels whose rays hit the tracked texels, not the bounce's
	// own sample count.
	// The dynamic bounces: histories of their own, accumulated like the
	// static one but never restarted by a change; instead the lights'
	// motion caps their length (A-SVGF's alpha = max(alpha, gradient), the
	// gradient being how far the lights moved this frame): a sweep keeps
	// them at a frame or two, at rest they grow to the window.
	vec3 dyn = vec3(0.0);
	vec3 dyn2 = vec3(0.0);
	float dyn_frames = 0.0;
	vec4 dyn2_old = imageLoad(indirect_dyn2_atlas, texel);
	if (dyn_lights.count > 0u) {
		// Floored at one old relight on purpose: a full-refresh motion keeps
		// half of the last relight. Measured without the floor (the history
		// worth 1 / motion relights including this one, so nothing kept at a
		// full refresh) on the game flick: the cards' field 0.057 -> 0.062 at
		// the stop, 0.022 -> 0.028 four frames on, the screen 0.069 -> 0.071;
		// one relight's two light rays are noisier than the half-relight of
		// lag they replace (MEGALIGHTS_PLAN.md section 27).
		float keep_dyn = params.dynamic_motion > 0.0 ? max(1.0, 1.0 / params.dynamic_motion) : params.dynamic_window;
		// A join (see below) starts them afresh: a light changing again while
		// its weight fades had its histories at that weight.
		dyn_frames = (fresh || params.dynamic_join > 0.0) ? 0.0 : min(min(dyn2_old.a * 64.0, keep_dyn), params.dynamic_window);
		float dyn_alpha = 1.0 / (dyn_frames + 1.0);
		dyn = mix(max(dyn_old.rgb, vec3(0.0)), dyn_sample, dyn_alpha);
		dyn2 = mix(max(dyn2_old.rgb, vec3(0.0)), dyn2_sample, dyn_alpha);
		dyn_frames = min(dyn_frames + 1.0, 64.0);
	}
	float keep_ind = (change > 0.02 && (params.debug & 32u) == 0u) ? max(params.bounce_floor, 1.0 / change) : 64.0;
	vec4 old_indirect = imageLoad(indirect_atlas, texel);
	// The hand-over of a light joining the dynamic set (params.dynamic_join,
	// the frame every set is relit). Until it moved, the light was one of
	// the static lights: the accumulation holds its bounce, and from this
	// relight the dynamic histories estimate the whole of it, so the room
	// would read the beam's bounce twice, fading over the static window (a
	// flashlight's first move brightened the ceiling by a tenth for half a
	// second). Neither a restart nor a subtraction of the first relight's
	// estimate answers it: one relight's light rays are a fraction of the
	// bounce, and its second bounce is not in them at all. Instead the
	// texel keeps the fraction of its pre-join content the accumulation
	// still holds -- one at the join, times one less the blend weight every
	// relight, so a restart clears it -- and stores the accumulation less
	// that fraction of the dynamic histories: what the readers sum (this
	// and the dynamic histories) is then the pre-join value exactly at the
	// join, and moves to the static accumulation plus the dynamic estimate
	// at the accumulation's own pace, unbiased at every relight, the
	// estimate's noise entering only as fast as the accumulation forgets.
	// The fraction rides in the alpha's half-relight (the relight count is
	// whole; see ind_unpack).
	vec2 ind_old = ind_unpack(old_indirect.a);
	float join = 0.0;
	if (dyn_lights.count > 0u && !fresh) {
		// The stored value plus the fraction of the histories it was stored
		// less: the accumulation itself.
		old_indirect.rgb += ind_old.y * (max(dyn_old.rgb, vec3(0.0)) + max(dyn2_old.rgb, vec3(0.0)));
		join = max(ind_old.y, params.dynamic_join);
	}
	float ind_frames = reset ? 0.0 : min(ind_old.x, keep_ind);
	float ind_alpha = max(1.0 / (ind_frames + 1.0), 1.0 / window);
	vec3 indirect = ind_frames <= 0.0 ? indirect_sample : mix(old_indirect.rgb, indirect_sample, ind_alpha);
	join *= ind_frames <= 0.0 ? 0.0 : 1.0 - ind_alpha;
	if ((params.debug & 128u) != 0u) {
		// Diagnostics (GODOT_CARD_ABLATE=paint, seen through GODOT_GI_FALLBACK=all):
		// the texel's own radiance gradient, the re-traced bounce gradient,
		// and the capture reset, as colour. The paint replaces the
		// accumulation, and the rays of other texels read it through their
		// hits, so the whole scene tints within a few relights: read the
		// first frames.
		indirect = vec3(fresh ? 0.0 : max(max(rel.r, rel.g), rel.b), max(bounce_gradient, 0.0), reset ? 1.0 : 0.0);
	} else if ((params.debug & 256u) != 0u) {
		// (paint2) The change the bounce ray carried from its hit, the
		// neighbours' spread, and the fading change from the last relight.
		indirect = vec3(max(bounce_change, 0.0), 0.0, max(prev.change - 0.125, 0.0));
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				ivec2 n = texel + ivec2(dx, dy);
				if ((dx != 0 || dy != 0) && all(greaterThanEqual(n, card_min)) && all(lessThanEqual(n, card_max))) {
					indirect.g = max(indirect.g, change_load_gradient(n));
				}
			}
		}
	}
	if ((params.debug & 512u) != 0u) {
		// (paint3) Whether any light is dynamic, the share of the dynamic
		// rays that landed on a card and connected, and the dynamic term's
		// share of the bounce.
		indirect = vec3(dyn_lights.count > 0u ? 1.0 : 0.0, dyn_landed, luminance(dyn + dyn2) / max(luminance(indirect + dyn + dyn2), 1e-4));
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	} else if ((params.debug & 131072u) != 0u) {
		// (paint8) The static lights' visibility ratio (r) and the dynamic lights' (g).
		indirect = vec3(vis, vis_dyn, 0.0);
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	} else if ((params.debug & 2048u) != 0u) {
		// (paint5) The share of the dynamic rays that landed and connected,
		// as grey: a luminance readout (rt_lab/radiosity_box.gd prints it
		// at its sample points).
		indirect = vec3(dyn_landed);
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	}
	// Nothing non-finite reaches the atlases: they persist, and every reader
	// (the gather's rays, the other texels' bounce rays, the hit shader)
	// would carry it on. A poisoned accumulation restarts instead.
	if (any(isnan(indirect)) || any(isinf(indirect))) {
		indirect = vec3(0.0);
		ind_frames = 0.0;
		join = 0.0;
		if ((params.debug & 8192u) != 0u) {
			atomicAdd(dyn_stats.count[31], 1u);
		}
	}
	if (any(isnan(dyn)) || any(isinf(dyn)) || any(isnan(dyn2)) || any(isinf(dyn2))) {
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
		dyn_frames = 0.0;
	}
	imageStore(indirect_dyn_atlas, texel, vec4(dyn, dyn_lum));
	imageStore(indirect_dyn2_atlas, texel, vec4(dyn2, dyn_frames / 64.0));
	// The accumulation less the joining light's share of the dynamic
	// histories (see the hand-over above); the readers add the histories.
	vec3 indirect_stored = max(indirect - join * (dyn + dyn2), vec3(0.0));
	imageStore(indirect_atlas, texel, vec4(indirect_stored, ind_pack(min(ind_frames + 1.0, 64.0), join)));

	// The radiance the rays read: without the dynamic lights' direct term,
	// which every reader adds from the lights' current state (the gather at
	// its hits, the light rays at their landings): a card relit with the
	// beam where it was a frame ago never hands that beam to a reader
	// subtracting it where it is now.
	vec3 direct = d.exact + max(d.local_sum - d.dyn_sum, vec3(0.0)) * vis;
	vec3 radiance = max(t.albedo * (direct + indirect_stored + dyn + dyn2) + t.emission, vec3(0.0));
	vec3 static_radiance = max(t.albedo * (direct + indirect_stored) + t.emission, vec3(0.0));
	if (any(isnan(radiance)) || any(isinf(radiance)) || any(isnan(static_radiance)) || any(isinf(static_radiance))) {
		radiance = vec3(0.0);
		static_radiance = vec3(0.0);
		if ((params.debug & 8192u) != 0u) {
			atomicAdd(dyn_stats.count[31], 1u);
		}
	}
	imageStore(static_atlas, texel, vec4(static_radiance, vis_dyn));
	imageStore(lighting_atlas, texel, vec4(radiance, frames / 64.0));
}

void main() {
	uint entry = gl_WorkGroupID.y;
	if (entry >= active_sets.count) {
		return;
	}
	uint set = active_sets.list[entry];
	CardSet s = sets.data[set];
	if (s.card_size < 8.0) {
		return;
	}
	// A workgroup covers 8x8 texels, one per thread, or with the bounce ray
	// shared 16x16, a 2x2 quad per thread: the quad's one ray keeps every
	// lane of the group tracing, which is what makes the fourfold fewer rays
	// cost a quarter of the time (rays idle in a lane still cost its group).
	// The cards differ in size, so the group finds its card by walking the
	// cards' block counts.
	bool quad_mode = bool(params.flags & FLAG_SHARED_BOUNCE_RAY);
	uint tile = quad_mode ? 16u : 8u;
	uint card = SURFACE_CACHE_CARDS;
	uint block = gl_WorkGroupID.x;
	ivec2 dims = ivec2(0);
	uvec2 n = uvec2(1u);
	uint card_packed = 0u;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		uint packed = sets.data[set].cards[k];
		ivec2 kd = card_dims_packed(packed);
		uvec2 kn = max(uvec2(kd) / tile, uvec2(1u));
		uint blocks = kn.x * kn.y;
		if (block < blocks) {
			card = k;
			dims = kd;
			n = kn;
			card_packed = packed;
			break;
		}
		block -= blocks;
	}
	if (card >= SURFACE_CACHE_CARDS) {
		return;
	}
	ivec2 block_origin = ivec2(int(block % n.x), int(block / n.x)) * int(tile);
	ivec2 origin_texel = card_origin_packed(card_packed);
	bool reset = (s.flags & SURFACE_CACHE_SET_FLAG_RESET) != 0u;
	ivec2 card_min = origin_texel;
	ivec2 card_max = origin_texel + dims - ivec2(1);
	// The relight before this one, whose bounce ray the gradient re-traces.
	uint prev_frame = relit.frame[set * 2u];
	bool have_prev = !reset && prev_frame != 0u && prev_frame < params.frame;
	relit.frame[set * 2u + 1u] = params.frame;

	if (!quad_mode) {
		ivec2 texel_in_card = block_origin + ivec2(gl_LocalInvocationID.xy);
		ivec2 texel = origin_texel + texel_in_card;
		Texel t;
		if (!read_texel(s, card, dims, texel_in_card, texel, t)) {
			return; // Nothing captured here.
		}
		uint seed = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		// The bounce ray's seed is its own, not the direct term's advanced
		// one: the previous relight's ray must be reproducible from its
		// frame alone, without replaying that relight's light sampling.
		uint bounce_seed = pcg_hash(seed ^ 0x9E3779B9u);
		uint prev_seed = pcg_hash(pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(prev_frame))) ^ 0x9E3779B9u);
		Direct d;
		shade_direct(entry, t, seed, d);
		float gradient = bounce_gradient(texel, t, prev_seed, have_prev);
		vec3 indirect_sample;
		float bounce_change;
		float bounce_change_total;
		uint bounce_set;
		float bounce_t;
		bool young_dynamic = texel_young_dynamic(texel);
		uint n_cosine = cosine_rays(texel_young(texel), young_dynamic);
		float n_light = light_rays(young_dynamic);
		vec3 dyn_sample;
		vec3 dyn2_sample;
		trace_bounce(t, bounce_seed, float(n_cosine), n_light, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total, bounce_set, bounce_t);
		trace_bounce_young(n_cosine, n_light, t, bounce_seed, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total);
		vec3 dyn_light;
		float dyn_landed;
		float dyn_change_total;
		trace_dynamic(texel, t, float(n_cosine), n_light, dyn_light, dyn_landed, dyn_change_total);
		dyn_sample += dyn_light;
		bounce_change_total = max(bounce_change_total, dyn_change_total);
		accumulate(texel, t, reset, d, indirect_sample, bounce_change, bounce_change_total, dyn_sample, dyn2_sample, dyn_landed, gradient, bounce_set, bounce_t, card_min, card_max);
		return;
	}

	ivec2 quad_in_card = block_origin + ivec2(gl_LocalInvocationID.xy) * 2;
	if (any(greaterThanEqual(quad_in_card, dims))) {
		return; // An 8-texel edge fills half of the tile.
	}
	Texel t[4];
	bool valid[4];
	uint valid_count = 0u;
	for (uint k = 0u; k < 4u; k++) {
		ivec2 texel_in_card = quad_in_card + ivec2(int(k & 1u), int(k >> 1u));
		valid[k] = read_texel(s, card, dims, texel_in_card, origin_texel + texel_in_card, t[k]);
		valid_count += valid[k] ? 1u : 0u;
	}
	if (valid_count == 0u) {
		return;
	}
	// The ray leaves a different texel of the quad each frame, skipping the
	// uncaptured ones.
	uint tracer = 0u;
	for (uint j = 0u; j < 4u; j++) {
		uint k = (params.frame + j) & 3u;
		if (valid[k]) {
			tracer = k;
			break;
		}
	}
	ivec2 tracer_texel = origin_texel + quad_in_card + ivec2(int(tracer & 1u), int(tracer >> 1u));
	uint seed = pcg_hash(uint(tracer_texel.x) + pcg_hash(uint(tracer_texel.y) + pcg_hash(params.frame)));
	// The previous relight's tracer and seed: the quad's ray left the texel
	// that frame chose, with the seed that frame gave it.
	float gradient = -1.0;
	if (have_prev) {
		uint prev_tracer = 0u;
		for (uint j = 0u; j < 4u; j++) {
			uint k = (prev_frame + j) & 3u;
			if (valid[k]) {
				prev_tracer = k;
				break;
			}
		}
		ivec2 prev_tracer_texel = origin_texel + quad_in_card + ivec2(int(prev_tracer & 1u), int(prev_tracer >> 1u));
		uint prev_seed = pcg_hash(uint(prev_tracer_texel.x) + pcg_hash(uint(prev_tracer_texel.y) + pcg_hash(prev_frame)));
		gradient = bounce_gradient(prev_tracer_texel, t[prev_tracer], prev_seed, true);
	}
	vec3 indirect_sample;
	float bounce_change;
	float bounce_change_total;
	uint bounce_set;
	float bounce_t;
	bool young_dynamic = texel_young_dynamic(tracer_texel);
	uint n_cosine = cosine_rays(texel_young(tracer_texel), young_dynamic);
	float n_light = light_rays(young_dynamic);
	vec3 dyn_sample;
	vec3 dyn2_sample;
	trace_bounce(t[tracer], seed, float(n_cosine), n_light, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total, bounce_set, bounce_t);
	trace_bounce_young(n_cosine, n_light, t[tracer], seed, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total);
	vec3 dyn_light;
	float dyn_landed;
	float dyn_change_total;
	trace_dynamic(tracer_texel, t[tracer], float(n_cosine), n_light, dyn_light, dyn_landed, dyn_change_total);
	dyn_sample += dyn_light;
	bounce_change_total = max(bounce_change_total, dyn_change_total);
	for (uint k = 0u; k < 4u; k++) {
		if (!valid[k]) {
			continue;
		}
		ivec2 texel = origin_texel + quad_in_card + ivec2(int(k & 1u), int(k >> 1u));
		uint seed_k = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		Direct d;
		shade_direct(entry, t[k], seed_k, d);
		accumulate(texel, t[k], reset, d, indirect_sample, bounce_change, bounce_change_total, dyn_sample, dyn2_sample, dyn_landed, gradient, bounce_set, bounce_t, card_min, card_max);
	}
}
