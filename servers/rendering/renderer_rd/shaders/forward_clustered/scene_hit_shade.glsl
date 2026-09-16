#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : enable

// Deferred hit shading: a material's fragment code run in compute at the
// ray hits the GI gather deferred to it (idTech 8's "shade the hit from
// the material", done here without a hit shader stage). One dispatch per
// material, over the packets binned to it: the hit's triangle comes out of
// the geometry pool, its attributes are interpolated the way the vertex
// stage would have handed them to the fragment stage, the user's fragment
// code produces the material inputs, and the hit is lit like a surface
// cache texel (the sun and the world light grid with one shadow ray; for
// the indirect term the hit's own card bounce where its card holds one
// (card_indirect), else a bounce ray into the cards, the probes or the
// sky; emission on top). The result
// goes to the pixel's slot for the resolve pass.
//
// The fragment code sees the scene shader's environment by name (see the
// renames in scene_shader_forward_clustered.cpp): what a hit cannot provide
// (screen textures, derivatives) fails the material's compile, and such a
// material's hits keep the probe fallback.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// In dependency order (rt_hit_inc packs directions with oct_inc's
// vec3_to_oct): the includes are expanded textually, so clang-format's
// alphabetical sort would leave the packer undeclared.
// clang-format off
#include "../oct_inc.glsl"
#include "../light_data_inc.glsl"
#include "../effects/surface_cache_inc.glsl"
#include "../effects/rt_hit_inc.glsl"
// clang-format on

#define M_PI 3.14159265359
#define SDFGI_MAX_CASCADES 8
#define SDFGI_OCT_SIZE 6

/* Set 0: the scene. */

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std140) uniform Params {
	mat4 world_from_view; // The camera transform.
	mat4 view_from_world;
	mat4 ndc_from_view; // For the screen radiance boost, as the gather has them.
	mat4 view_from_ndc;
	mat4 reproject;
	vec4 camera_origin;
	vec4 sky_quat_or_color;
	ivec2 screen_size; // The gather's.
	uint ray_count; // Diffuse slots per pixel; the mirror slot follows them.
	uint flags;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float sky_energy;
	vec2 sky_border;
	float time;
	float emissive_exposure_normalization;
	float lod_bias;
	float cone_scale; // A diffuse ray's footprint, in world units per unit of distance.
	vec3 grid_origin; // The world light grid (FLAG_GRID).
	float grid_cell;
	uint grid_n;
	uint grid_cap;
	float probe_floor;
	float probe_scale; // The gather's calibration of its probe tier.
	float screen_radiance_clamp;
	float screen_radiance_border_fade;
	float card_atlas_size; // The lighting atlas edge, for the bounce's mip reads.
	float card_youth_lod; // The tent a young card texel's bounce is read through (see card_indirect); 0 reads the texel alone.
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
	uint area_light_count; // The area lights (every one in the population, each tested for range at the hit).
	uint pad_area0;
	uint pad_area1;
	uint pad_area2;
}
params;

#define FLAG_SDFGI 1u
#define FLAG_SKY_MODE_SKY 2u
#define FLAG_SKY_MODE_COLOR 4u
#define FLAG_GRID 8u
#define FLAG_DEBUG_ALBEDO 16u
#define FLAG_DEBUG_NORMAL 32u
#define FLAG_DEBUG_UV 64u
#define FLAG_NO_SHADOW_RAYS 128u
#define FLAG_NO_INDIRECT 256u
#define FLAG_NO_DIRECT 512u
#define FLAG_BOUNCE_TRACED 1024u // Ablation (hit_shading_debug 64): the indirect term traces its own ray even where the hit's card holds an accumulated one.
#define FLAG_TIER_STATS 65536u // Diagnostics (GODOT_GI_TIER_PRINT): count where the hits' bounce came from (RT_HIT_COUNT_TIERS).
#define FLAG_SCREEN_RADIANCE 16384u // Last frame's screen stands in for the bounce's hit where it is on screen.
#define FLAG_DEBUG_GEO_NORMAL 2048u // The triangle's own normal, as oriented.
#define FLAG_DEBUG_VERTEX_NORMAL 4096u // The interpolated vertex normal, before any flip.
#define FLAG_DEBUG_DIRECT 8192u // The direct term alone, unshadowed, as the colour.

layout(set = 0, binding = 2, std430) restrict readonly buffer Sorted {
	uint data[];
}
sorted;

layout(set = 0, binding = 3, std430) restrict readonly buffer Packets {
	uint data[];
}
packets;

layout(set = 0, binding = 4, std430) restrict readonly buffer Geometry {
	HitGeometry data[];
}
geometry;

layout(set = 0, binding = 5, std430) restrict readonly buffer VertexPool {
	uint data[];
}
vertex_pool;

layout(set = 0, binding = 6, std430) restrict readonly buffer IndexPool {
	uint data[];
}
index_pool;

layout(set = 0, binding = 7, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;

layout(set = 0, binding = 8, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

layout(set = 0, binding = 9, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 10, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 11, std140) uniform DirectionalLights {
	DirectionalLightData data[8];
}
directional_lights;

layout(set = 0, binding = 12, std430) restrict readonly buffer LightGrid {
	uint data[];
}
light_grid;
#define GRID_SPOT_BIT 0x80000000u

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

// Where this material's packets start in the sorted list, and how many
// (rt_hit_bin.glsl's scan wrote them; the dispatch only names the slot).
layout(set = 0, binding = 18, std430) restrict readonly buffer Offsets {
	uint data[];
}
offsets;

layout(set = 0, binding = 19, std430) restrict buffer Counts {
	uint data[];
}
counts;

// The cards, for the hit's own bounce ray (the indirect term): a card the
// ray lands on gives its accumulated radiance, as for the gather's hits.
layout(set = 0, binding = 20, std430) restrict readonly buffer CardSets {
	CardSet data[];
}
card_sets;
layout(set = 0, binding = 21) uniform texture2D card_depth_atlas;
layout(set = 0, binding = 22) uniform texture2D card_lighting_atlas;
// The scene's depth and last frame's rendered radiance, for the bounce's
// screen lookup (the gather's screen_radiance_boost).
layout(set = 0, binding = 23) uniform sampler2D depth_texture;
layout(set = 0, binding = 24) uniform sampler2D screen_radiance_texture;
// The cards' accumulated bounce irradiance (surface_cache_light.glsl
// indirect_atlas), for the hit's own indirect term (card_indirect).
layout(set = 0, binding = 25) uniform texture2D card_indirect_atlas;
layout(set = 0, binding = 26) uniform texture2D card_indirect_dyn_atlas; // The dynamic lights' bounce, apart.
layout(set = 0, binding = 27) uniform texture2D card_indirect_dyn2_atlas;
// The area lights and the atlas their textures live in (read through
// linear_sampler_mipmaps), as the card lighting has them.
layout(set = 0, binding = 28, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;
layout(set = 0, binding = 30) uniform texture2D decal_atlas_srgb; // The lights' projector textures.
layout(set = 0, binding = 29) uniform texture2D area_light_atlas;

/* Set 1: the material samplers, by the names the compiler emits. */

layout(set = 1, binding = 0) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 1, binding = 1) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 1, binding = 2) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 4) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 5) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 6) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 1, binding = 7) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 1, binding = 8) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 9) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 10) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 1, binding = 11) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

/* Set 2: the output. */

layout(set = 2, binding = 0, std430) restrict buffer Results {
	uvec4 data[];
}
results;

layout(push_constant, std430) uniform Dispatch {
	uint packet_base; // Unused: the material's start in the sorted list comes from offsets.data[material_slot].
	uint packet_count;
	uint material_slot;
	uint pad;
}
dispatch;

/* Set 3: the material. */

#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = MATERIAL_UNIFORM_SET, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

/* Lighting, as the surface cache lights its texels (surface_cache_light.glsl). */

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

// The world radius of the bounce ray's footprint at its hit (see card_lookup).
float card_lookup_footprint = 0.0;
// The texel card_lookup landed on (continuous atlas coordinates), its card's
// origin and size, for card_indirect's read of the same texel in another atlas.
vec2 card_hit_texel = vec2(0.0);
ivec2 card_hit_origin = ivec2(0);
ivec2 card_hit_dims = ivec2(1);

// The gather's card lookup (stochastic_indirect_gi.glsl surface_cache_lookup)
// over this pass's bindings: the nearest texel of the lit atlas.
bool card_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance) {
	r_radiance = vec3(0.0);
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	uint set = card_instances.data[p_instance_id].set;
	if (set == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardSet s = card_sets.data[set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return false;
	}
	vec3 local_pos = (card_instances.data[p_instance_id].local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(card_instances.data[p_instance_id].local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	ivec2 best_texel = ivec2(0);
	vec2 best_uv = vec2(0.0);
	uint best_packed = 0u;
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
		uint packed = card_sets.data[set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(card_depth_atlas, texel, 0).r;
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
			best_texel = texel;
			best_uv = uv01;
			best_packed = packed;
		}
	}
	if (best_w <= 0.0) {
		return false;
	}
	// Through the mip the bounce ray's cone covers at the hit (as the
	// gather reads its hits), bilinear inside the card, never across its
	// border: one cosine ray per hit lands on an average of the region it
	// stands for rather than on one bright texel.
	vec2 best_dims = vec2(card_dims_packed(best_packed));
	float best_texel_world = (longest + 2.0 * s.margin) / max(best_dims.x, best_dims.y);
	float lod = 0.0;
	if (card_lookup_footprint > 0.0) {
		float max_lod = min(5.0, log2(min(best_dims.x, best_dims.y)) - 2.0);
		lod = clamp(log2(max(card_lookup_footprint / best_texel_world, 1.0)), 0.0, max(max_lod, 0.0));
	}
	float margin = 0.5 * exp2(lod);
	vec2 atlas_texel = vec2(card_origin_packed(best_packed)) + clamp(best_uv * best_dims, vec2(margin), best_dims - margin);
	r_radiance = textureLod(sampler2D(card_lighting_atlas, linear_sampler_mipmaps), atlas_texel / params.card_atlas_size, lod).rgb;
	card_hit_texel = atlas_texel;
	card_hit_origin = card_origin_packed(best_packed);
	card_hit_dims = ivec2(best_dims);
	return true;
}

// The hit's indirect term from its own card: the bounce irradiance the card
// lighting accumulates over sixty-four relights at the texel under the hit
// (surface_cache_light.glsl indirect_atlas, the same term the card's own
// radiance is assembled from), in place of one cosine ray per hit. A single
// ray's estimate of a room's bounce is a firefly wherever it lands on a lit
// patch -- the game project's mirror floor under a flashlight: the reflected
// ceiling's bounce rays landing on the beam's spot on the floor -- and a
// mirror's reflection is never filtered spatially, so the screen history was
// all that hid it, and a lighting change restarts that history (the floor
// sparkled whenever the light moved). Read as the gather's young-pixel
// fallback reads the atlas: a tent over the card's texels, wider the fewer
// relights the texel has, never across the card's border. A hit with no card
// under it, or a texel never relit, keeps the traced ray.
bool card_indirect(uint p_instance_id, vec3 p_world_pos, vec3 p_n_world, out vec3 r_indirect) {
	r_indirect = vec3(0.0);
	vec3 unused;
	card_lookup_footprint = 0.0;
	// Along the normal: the card facing the surface is the one that holds
	// it (along the ray a grazing wall picks its neighbour's card).
	if (!card_lookup(p_instance_id, p_world_pos, -p_n_world, unused)) {
		return false;
	}
	vec4 ind0 = texelFetch(card_indirect_atlas, ivec2(card_hit_texel), 0);
	float relights = ind0.a * 64.0;
	if (relights <= 0.0) {
		return false;
	}
	// The dynamic lights' histories are the younger while a light moves
	// (surface_cache_light.glsl accumulate; their age is the second one's
	// alpha), counted by their share of the bounce here: the tent is a
	// blur, and after any move every texel's dynamic history is young.
	// Both bounces come summed and filtered in the one atlas, its alpha the
	// age to read it at (surface_cache_light.glsl filter_dynamic).
	vec4 dyn2 = texelFetch(card_indirect_dyn2_atlas, ivec2(card_hit_texel), 0);
	if (dyn2.a > 0.0) {
		vec3 dyn = max(dyn2.rgb, vec3(0.0));
		// A share of two luminances: Rec.709 weights rather than luma_weights,
		// which only reshapes the ratio a little and never its range.
		float dyn_lum = dot(dyn, vec3(0.2126, 0.7152, 0.0722));
		float share = dyn_lum / max(dyn_lum + dot(max(ind0.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), 1e-4);
		relights = min(relights, dyn2.a * 64.0 / max(share, 0.05));
	}
	vec2 dims = vec2(card_hit_dims);
	float spacing = 1.0;
	if (params.card_youth_lod > 0.0) {
		float youth_lod = params.card_youth_lod * (1.0 - log2(max(relights, 1.0)) / 6.0);
		spacing = clamp(exp2(youth_lod - 1.0), 1.0, max(min(dims.x, dims.y) / 8.0, 1.0));
	}
	vec2 t_min = vec2(card_hit_origin) + 0.5;
	vec2 t_max = vec2(card_hit_origin + card_hit_dims) - 0.5;
	vec3 ind = vec3(0.0);
	for (int dy = 0; dy < 4; dy++) {
		for (int dx = 0; dx < 4; dx++) {
			vec2 t = clamp(card_hit_texel + (vec2(dx, dy) - 1.5) * spacing, t_min, t_max);
			ind += textureLod(sampler2D(card_indirect_atlas, linear_sampler_mipmaps), t / params.card_atlas_size, 0.0).rgb + max(textureLod(sampler2D(card_indirect_dyn_atlas, linear_sampler_mipmaps), t / params.card_atlas_size, 0.0).rgb, vec3(0.0));
		}
	}
	r_indirect = max(ind / 16.0, vec3(0.0));
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

// The gather's screen radiance boost (stochastic_indirect_gi.glsl): where
// the bounce's hit is on screen, last frame's rendered radiance there,
// clamped against the cache value, fading to it at the frame border.
vec3 screen_radiance_boost(vec3 world_hit, vec3 cache_radiance) {
	if (!bool(params.flags & FLAG_SCREEN_RADIANCE)) {
		return cache_radiance;
	}
	vec3 view_hit = (params.view_from_world * vec4(world_hit, 1.0)).xyz;
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
		return cache_radiance;
	}
	vec4 prev_ndc = params.reproject * vec4(ndc.xy, scene_depth, 1.0);
	if (prev_ndc.w <= 0.0) {
		return cache_radiance;
	}
	vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
	if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
		return cache_radiance;
	}
	vec3 col = textureLod(screen_radiance_texture, prev_uv, 0.0).rgb;
	float l = luminance(col);
	float clamp_l = max(luminance(cache_radiance) * 4.0, params.screen_radiance_clamp) + 0.5;
	if (l > clamp_l) {
		col *= clamp_l / l;
	}
	vec2 border = min(min(uv, vec2(1.0) - uv), min(prev_uv, vec2(1.0) - prev_uv));
	return mix(cache_radiance, col, smoothstep(0.0, max(params.screen_radiance_border_fade, 1e-5), min(border.x, border.y)));
}

// The triangle a ray landed on, oriented toward the ray: the normal the
// probe lookup at a card-less bounce hit wants (the SDF's is not bound
// here, and the ray's own direction is a poor stand-in at grazing angles).
vec3 hit_triangle_normal(uint p_instance_id, uint p_geometry_index, uint p_primitive, vec3 p_dir, vec3 p_fallback) {
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return p_fallback;
	}
	uint geometry_base = card_instances.data[p_instance_id].geometry_base;
	if (geometry_base == SURFACE_CACHE_INVALID) {
		return p_fallback;
	}
	HitGeometry g = geometry.data[geometry_base + p_geometry_index];
	uint ib = g.index_base + p_primitive * 3u;
	uint i0 = (g.vertex_base + index_pool.data[ib]) * RT_HIT_VERTEX_WORDS;
	uint i1 = (g.vertex_base + index_pool.data[ib + 1u]) * RT_HIT_VERTEX_WORDS;
	uint i2 = (g.vertex_base + index_pool.data[ib + 2u]) * RT_HIT_VERTEX_WORDS;
	vec3 p0 = vec3(uintBitsToFloat(vertex_pool.data[i0]), uintBitsToFloat(vertex_pool.data[i0 + 1u]), uintBitsToFloat(vertex_pool.data[i0 + 2u]));
	vec3 p1 = vec3(uintBitsToFloat(vertex_pool.data[i1]), uintBitsToFloat(vertex_pool.data[i1 + 1u]), uintBitsToFloat(vertex_pool.data[i1 + 2u]));
	vec3 p2 = vec3(uintBitsToFloat(vertex_pool.data[i2]), uintBitsToFloat(vertex_pool.data[i2 + 1u]), uintBitsToFloat(vertex_pool.data[i2 + 2u]));
	mat3 world_from_local = mat3(card_instances.data[p_instance_id].world_from_local_x.xyz, card_instances.data[p_instance_id].world_from_local_y.xyz, card_instances.data[p_instance_id].world_from_local_z.xyz);
	vec3 n = cross(world_from_local * (p1 - p0), world_from_local * (p2 - p0));
	float len = length(n);
	if (len <= 1e-12) {
		return p_fallback;
	}
	n /= len;
	return dot(n, p_dir) > 0.0 ? -n : n;
}

// The indirect term: one cosine ray, as the card lighting traces for its
// texels. A card at the bounce's hit gives its radiance; otherwise the
// probes stand in the way the gather's own fallback does (the probe tier
// at the hit, calibrated against the screen); a miss reads the sky; and
// last frame's screen overrides either where the hit is in view.
// Diagnostics (FLAG_TIER_STATS): the slot's count and luminance sum, see
// RT_HIT_COUNT_TIERS for what the slots mean.
void tier_stat(uint i, vec3 radiance) {
	if ((params.flags & FLAG_TIER_STATS) != 0u) {
		atomicAdd(counts.data[RT_HIT_COUNT_TIERS + i], 1u);
		atomicAdd(counts.data[RT_HIT_COUNT_TIERS + 8u + i], uint(min(luminance(max(radiance, vec3(0.0))), 64.0) * 16.0));
	}
}

vec3 trace_bounce(vec3 origin, vec3 n_world, vec3 rel_origin, inout uint seed) {
	seed = pcg_hash(seed);
	float r0 = hash_to_float(seed);
	seed = pcg_hash(seed);
	float r1 = hash_to_float(seed);
	vec3 ray_dir = basis_around(n_world, vec2(r0, r1));
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.0, ray_dir, 1e4);
	while (rayQueryProceedEXT(rq)) {
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		float t_hit = rayQueryGetIntersectionTEXT(rq, true);
		vec3 hit = origin + ray_dir * t_hit;
		uint hit_instance = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		vec3 cache_radiance;
		uint src = 2u;
		// The bounce's cone is at least half a unit of hit distance, wider
		// than the diffuse gather's (GODOT_GI_CONE, 0.25): the pixel's own
		// cone scale is a few pixels' worth, for texture detail at the hit,
		// too narrow for a bounce read.
		card_lookup_footprint = t_hit * max(params.cone_scale, 0.5);
		if (!card_lookup(hit_instance, hit, ray_dir, cache_radiance)) {
			vec3 hit_n = hit_triangle_normal(hit_instance, rayQueryGetIntersectionGeometryIndexEXT(rq, true), rayQueryGetIntersectionPrimitiveIndexEXT(rq, true), ray_dir, -ray_dir);
			vec3 irr;
			if (sdfgi_probe_irradiance(hit - params.camera_origin.xyz, hit_n, irr)) {
				cache_radiance = irr * params.probe_floor * params.probe_scale;
				src = 3u;
			} else {
				cache_radiance = sky_eval(ray_dir);
				src = 4u;
			}
		}
		tier_stat(src, cache_radiance);
		return screen_radiance_boost(hit, cache_radiance);
	}
	vec3 sky = sky_eval(ray_dir);
	tier_stat(5u, sky);
	return sky;
}

// Shadow rays take alpha-tested casters whole, like the gather's bounce ray
// (the card albedo atlas that would confirm their coverage is not bound here).
bool occluded(vec3 origin, vec3 dir, float t_max, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, mask, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Direct light at the hit: the sun with a shadow ray, and the local lights
// as a ratio estimate over the hit's grid cell (every light in the
// population where the hit lies outside the grid), one shadow ray.
vec3 shade_direct(vec3 world_pos, vec3 n_world, vec3 origin, inout uint seed) {
	vec3 direct = vec3(0.0);
	bool shadow_rays = (params.flags & FLAG_NO_SHADOW_RAYS) == 0u;

	for (uint i = 0u; i < params.directional_light_count; i++) {
		DirectionalLightData dl = directional_lights.data[i];
		vec3 l = normalize(mat3(params.world_from_view) * dl.direction);
		float ndotl = dot(n_world, l);
		if (ndotl <= 0.0) {
			continue;
		}
		vec3 c = dl.color * dl.energy * (ndotl * (1.0 / M_PI));
		if (dl.shadow_opacity <= 0.001 || !shadow_rays) {
			direct += c;
			continue;
		}
		float vis = mix(1.0, occluded(origin, l, 1e4, 0xFFu) ? 0.0 : 1.0, dl.shadow_opacity);
		direct += c * vis;
	}

	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	vec3 sel_pos = vec3(0.0);
	float sel_opacity = 0.0;
	uint sel_mask = 0u;
	bool selected = false;

	uint base = 0u;
	uint light_count = min(params.omni_light_count + params.spot_light_count, 256u);
	bool from_grid = false;
	if (bool(params.flags & FLAG_GRID)) {
		vec3 rel = (world_pos - params.grid_origin) / params.grid_cell;
		if (all(greaterThanEqual(rel, vec3(0.0))) && all(lessThan(rel, vec3(float(params.grid_n))))) {
			uvec3 c = uvec3(rel);
			base = (c.x + params.grid_n * (c.y + params.grid_n * c.z)) * (1u + params.grid_cap);
			light_count = min(light_grid.data[base], params.grid_cap);
			from_grid = true;
		}
	}
	for (uint j = 0u; j < light_count; j++) {
		uint idx = from_grid ? light_grid.data[base + 1u + j] : j;
		bool is_spot = from_grid ? (idx & GRID_SPOT_BIT) != 0u : idx >= params.omni_light_count;
		if (from_grid) {
			idx &= ~GRID_SPOT_BIT;
		} else if (is_spot) {
			idx -= params.omni_light_count;
		}
		LightData ld = is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		vec3 rel = pos - world_pos;
		float len = length(rel);
		float attenuation = get_omni_attenuation(len, ld.inv_radius, ld.attenuation);
		vec3 l = rel / max(len, 1e-5);
		if (is_spot) {
			vec3 spot_dir = normalize(mat3(params.world_from_view) * ld.direction);
			float scos = max(dot(-l, spot_dir), ld.cone_angle);
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
			attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
		}
		float ndotl = max(dot(n_world, l), 0.0);
		vec3 c = ld.color * (ndotl * attenuation * (1.0 / M_PI));
		float w = luminance(abs(c));
		if (w <= 0.0) {
			continue;
		}
		// The projector texture, as the card lighting reads it
		// (surface_cache_light.glsl projector_factor): the cards' light
		// buffers carry a world-space projector matrix.
		if (ld.projector_rect != vec4(0.0)) {
			vec4 proj;
			if (is_spot) {
				vec4 splane = ld.shadow_matrix * vec4(world_pos, 1.0);
				splane /= splane.w;
				proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), splane.xy * ld.projector_rect.zw + ld.projector_rect.xy, 0.0);
			} else {
				vec3 local_v = normalize((ld.shadow_matrix * vec4(world_pos, 1.0)).xyz);
				vec4 atlas_rect = ld.projector_rect;
				if (local_v.z >= 0.0) {
					atlas_rect.y += atlas_rect.w;
				}
				local_v.z = 1.0 + abs(local_v.z);
				local_v.xy /= local_v.z;
				local_v.xy = local_v.xy * 0.5 + 0.5;
				proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), local_v.xy * atlas_rect.zw + atlas_rect.xy, 0.0);
			}
			c *= proj.rgb * proj.a;
			w = luminance(abs(c));
			if (w <= 0.0) {
				continue;
			}
		}
		sum += c;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = pos;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	// Area lights, in the same estimator (as the card lighting): the LTC
	// diffuse term, the shadow ray to a point drawn uniformly on the rect.
	for (uint j = 0u; j < params.area_light_count; j++) {
		LightData ld = area_lights.data[j];
		seed = pcg_hash(seed);
		float xi0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float xi1 = hash_to_float(seed);
		float geom;
		vec3 point;
		vec3 c = area_light_contribution(ld, params.world_from_view, world_pos, n_world, vec2(xi0, xi1), area_light_atlas, linear_sampler_mipmaps, geom, point);
		float w = luminance(abs(c));
		// The negation rejects a NaN term as well (see the card lighting).
		if (!(w > 0.0)) {
			continue;
		}
		sum += c;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = point;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	if (selected) {
		float vis = 1.0;
		if (sel_opacity > 0.001 && sel_mask != 0u && shadow_rays) {
			vec3 to_light = sel_pos - origin;
			float dist = length(to_light);
			vis = mix(1.0, occluded(origin, to_light / max(dist, 1e-5), max(dist - params.ray_bias, 0.0), sel_mask) ? 0.0 : 1.0, sel_opacity);
		}
		direct += sum * vis;
	}
	return direct;
}

// What the fragment code cannot have in compute, redirected. A texture
// lookup without an explicit level takes the ray cone's: the hit's uv
// footprint per texel, from the triangle's uv density and the ray's spread
// at its distance. Derivatives have no substitute and fail the compile.
// A discard leaves the hit to the probes (what the ray would have found
// past the hole is not traced again), written here since the code's
// return leaves main.
float hit_uv_footprint = 0.0;
bool hit_discarded = false;
uint hit_result_index = 0u;
uint hit_result_flags = 0u;
vec3 hit_dir = vec3(0.0, 0.0, 1.0);
vec3 hit_rel_pos = vec3(0.0);
float hit_lod(vec2 size) {
	float texels = hit_uv_footprint * max(size.x, size.y);
	return max(log2(max(texels, 1e-6)), 0.0) + params.lod_bias;
}
void hit_write_discard() {
	vec3 irr;
	vec3 radiance;
	if (sdfgi_probe_irradiance(hit_rel_pos, -hit_dir, irr)) {
		radiance = irr * params.probe_floor * params.probe_scale;
		tier_stat(6u, radiance);
	} else {
		radiance = sky_eval(hit_dir);
		tier_stat(7u, radiance);
	}
	results.data[hit_result_index] = uvec4(rt_hit_pack_radiance(radiance), rt_hit_pack_dir(hit_dir), hit_result_flags);
}
#define texture(s, c) textureLod(s, c, hit_lod(vec2(textureSize(s, 0))))
#define discard \
	{ \
		hit_discarded = true; \
		hit_write_discard(); \
		return; \
	}
// A hit has no neighboring fragments, and compute has no screen-space
// derivatives (the material code asks for them all the same:
// StandardMaterial3D's MSDF text path divides by fwidth(uv), alpha
// antialiasing and grid shaders too, and the variant failed to compile).
// What they stand for at a hit is the ray's footprint, so that is what
// they return: the same type as their argument, the footprint in every
// component, never zero.
#define dFdx(x) ((x) * 0.0 + hit_uv_footprint)
#define dFdy(x) ((x) * 0.0 + hit_uv_footprint)
#define dFdxCoarse(x) ((x) * 0.0 + hit_uv_footprint)
#define dFdyCoarse(x) ((x) * 0.0 + hit_uv_footprint)
#define dFdxFine(x) ((x) * 0.0 + hit_uv_footprint)
#define dFdyFine(x) ((x) * 0.0 + hit_uv_footprint)
#define fwidth(x) ((x) * 0.0 + 2.0 * hit_uv_footprint)
#define fwidthCoarse(x) ((x) * 0.0 + 2.0 * hit_uv_footprint)
#define fwidthFine(x) ((x) * 0.0 + 2.0 * hit_uv_footprint)

// The scene shader's built-ins the compiler names, as much of them as a
// hit has. FRAGCOORD, FRONT_FACING and DEPTH are rewritten to these by the
// scene shader before the code reaches this template.
struct HitSceneData {
	mat4 main_cam_inv_view_matrix;
	uint camera_visible_layers;
	float emissive_exposure_normalization;
	uint flags;
	vec2 screen_pixel_size;
};
struct HitSceneDataBlock {
	HitSceneData data;
};
HitSceneDataBlock scene_data_block;
#define SCENE_DATA_FLAGS_IN_SHADOW_PASS 0u
#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0
#define OUTPUT_IS_MULTIVIEW false
#define ViewIndex 0
struct HitInstanceStub {
	int instance_uniforms_ofs;
};
struct HitInstances {
	HitInstanceStub data[1];
};
HitInstances instances;
uint instance_index_interp = 0u;
#define INSTANCE_INDEX 0u
#define VERTEX_INDEX 0u
vec4 hit_fragcoord = vec4(0.5, 0.5, 0.0, 1.0);
bool hit_front_facing = true;
float hit_fragdepth = 0.0;

#GLOBALS

/* The pool. */

void read_vertex(uint v, out vec3 r_pos, out vec3 r_normal, out vec3 r_tangent, out float r_binormal_sign, out vec2 r_uv, out vec2 r_uv2, out vec4 r_color) {
	uint b = v * RT_HIT_VERTEX_WORDS;
	r_pos = vec3(uintBitsToFloat(vertex_pool.data[b]), uintBitsToFloat(vertex_pool.data[b + 1u]), uintBitsToFloat(vertex_pool.data[b + 2u]));
	r_normal = oct_to_vec3(unpackUnorm2x16(vertex_pool.data[b + 3u]) * 2.0 - 1.0);
	uint t = vertex_pool.data[b + 4u];
	r_tangent = oct_to_vec3(vec2(float(t & 0xFFFFu) / 65535.0, float((t >> 16u) & 0x7FFFu) / 32767.0) * 2.0 - 1.0);
	r_binormal_sign = (t & 0x80000000u) != 0u ? -1.0 : 1.0;
	r_uv = unpackHalf2x16(vertex_pool.data[b + 5u]);
	r_uv2 = unpackHalf2x16(vertex_pool.data[b + 6u]);
	r_color = unpackUnorm4x8(vertex_pool.data[b + 7u]);
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= counts.data[dispatch.material_slot]) {
		return;
	}
	uint p = sorted.data[offsets.data[dispatch.material_slot] + i] * RT_HIT_PACKET_WORDS;
	uint w0 = packets.data[p];
	ivec2 pixel = rt_hit_unpack_pixel(w0);
	uint slot = rt_hit_unpack_slot(w0);
	uint pflags = rt_hit_unpack_flags(w0);
	uint record = packets.data[p + 1u];
	uint primitive = packets.data[p + 2u];
	uint geometry_index = (packets.data[p + 3u] >> 16u) & 0xFFu;
	uint pixel_rays_minus_one = (packets.data[p + 3u] >> 24u) & 3u; // The pixel's diffuse rays, for the resolve's average.
	vec2 bary = unpackHalf2x16(packets.data[p + 4u]);
	vec3 dir = rt_hit_unpack_dir(packets.data[p + 5u]);
	vec3 world_pos = vec3(uintBitsToFloat(packets.data[p + 6u]), uintBitsToFloat(packets.data[p + 7u]), uintBitsToFloat(packets.data[p + 8u]));
	float t_hit = uintBitsToFloat(packets.data[p + 9u]);
	uint result_index = uint(pixel.y * params.screen_size.x + pixel.x) * (params.ray_count + 1u) + slot;
	uint result_flags = RT_HIT_RESULT_DONE | ((pflags & RT_HIT_PACKET_MIRROR) != 0u ? RT_HIT_RESULT_MIRROR : 0u) | (pixel_rays_minus_one << RT_HIT_RESULT_RAYS_SHIFT);
	hit_result_index = result_index;
	hit_result_flags = result_flags;
	hit_dir = dir;
	hit_rel_pos = world_pos - params.camera_origin.xyz;

	CardInstance rec = card_instances.data[record];
	HitGeometry g = geometry.data[rec.geometry_base + geometry_index];
	uint ib = g.index_base + primitive * 3u;
	uint i0 = g.vertex_base + index_pool.data[ib];
	uint i1 = g.vertex_base + index_pool.data[ib + 1u];
	uint i2 = g.vertex_base + index_pool.data[ib + 2u];
	vec3 p0, p1, p2, n0, n1, n2, t0, t1, t2;
	float s0, s1, s2;
	vec2 uv0, uv1, uv2, uv20, uv21, uv22;
	vec4 c0, c1, c2;
	read_vertex(i0, p0, n0, t0, s0, uv0, uv20, c0);
	read_vertex(i1, p1, n1, t1, s1, uv1, uv21, c1);
	read_vertex(i2, p2, n2, t2, s2, uv2, uv22, c2);
	float b0 = 1.0 - bary.x - bary.y;

	mat3 world_from_local = mat3(rec.world_from_local_x.xyz, rec.world_from_local_y.xyz, rec.world_from_local_z.xyz);
	mat3 normal_from_local = transpose(mat3(rec.local_from_world)); // The inverse transpose.
	vec3 e1 = world_from_local * (p1 - p0);
	vec3 e2 = world_from_local * (p2 - p0);
	vec3 geo_normal = normalize(cross(e1, e2));
	if (dot(geo_normal, dir) > 0.0) {
		geo_normal = -geo_normal; // Toward the ray, whichever side the winding puts first.
	}
	vec3 n_local = b0 * n0 + bary.x * n1 + bary.y * n2;
	vec3 n_vertex = normalize(normal_from_local * n_local);
	vec3 n_world = (g.flags & RT_HIT_GEOMETRY_NORMAL) != 0u ? n_vertex : geo_normal;
	if (dot(n_world, geo_normal) < 0.0) {
		n_world = -n_world; // The face's own plane settles a normal wound against it, as the scene shader does.
	}
	vec3 t_world = vec3(0.0);
	vec3 b_world = vec3(0.0);
	if ((g.flags & RT_HIT_GEOMETRY_TANGENT) != 0u) {
		t_world = normalize(world_from_local * (b0 * t0 + bary.x * t1 + bary.y * t2));
		b_world = normalize(cross(n_world, t_world)) * s0;
	}
	vec2 uv = b0 * uv0 + bary.x * uv1 + bary.y * uv2;
	vec2 uv2_i = b0 * uv20 + bary.x * uv21 + bary.y * uv22;
	vec4 color = b0 * c0 + bary.x * c1 + bary.y * c2;

	// The ray cone at the hit: its width over the triangle's uv density,
	// stretched by the grazing angle.
	{
		float world_area = 0.5 * length(cross(e1, e2));
		vec2 duv1 = uv1 - uv0;
		vec2 duv2 = uv2 - uv0;
		float uv_area = 0.5 * abs(duv1.x * duv2.y - duv1.y * duv2.x);
		float uv_per_world = sqrt(uv_area / max(world_area, 1e-12));
		float cone_width = t_hit * params.cone_scale;
		hit_uv_footprint = cone_width * uv_per_world / max(abs(dot(geo_normal, dir)), 0.2);
	}

	// The fragment stage's inputs, in view space as the scene shader has them.
	mat3 view_from_world3 = mat3(params.view_from_world);
	vec3 vertex = (params.view_from_world * vec4(world_pos, 1.0)).xyz;
	vec3 view_highp = normalize(view_from_world3 * -dir); // Toward whoever is looking: the ray's origin.
	vec3 eye_offset = vec3(0.0);
	vec3 normal_highp = view_from_world3 * n_world;
	vec3 tangent = view_from_world3 * t_world;
	vec3 binormal = view_from_world3 * b_world;
	vec2 uv_interp = uv;
	vec2 uv2_interp = uv2_i;
	vec2 streaming_uv = uv;
	vec4 color_interp = color;
	hit_front_facing = (pflags & RT_HIT_PACKET_FRONT_FACE) != 0u;

	vec3 albedo_highp = vec3(1.0);
	vec3 backlight = vec3(0.0);
	vec4 transmittance_color = vec4(0.0, 0.0, 0.0, 1.0);
	float transmittance_depth = 0.0;
	float transmittance_boost = 0.0;
	float metallic_highp = 0.0;
	float specular = 0.5;
	vec3 emission = vec3(0.0);
	float roughness_highp = 1.0;
	float rim = 0.0;
	float rim_tint = 0.0;
	float clearcoat = 0.0;
	float clearcoat_roughness = 0.0;
	float anisotropy = 0.0;
	vec2 anisotropy_flow = vec2(1.0, 0.0);
	vec3 energy_compensation = vec3(1.0);
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 custom_radiance = vec4(0.0);
	vec4 custom_irradiance = vec4(0.0);
	float ao = 1.0;
	float ao_light_affect = 0.0;
	float alpha_highp = 1.0;
	float premul_alpha = 1.0;
	vec3 normal_map = vec3(0.5);
	vec3 bent_normal_vector = vec3(0.5);
	vec3 bent_normal_map = vec3(0.5);
	float normal_map_depth = 1.0;
	vec2 screen_uv = vec2(0.5);
	float sss_strength = 0.0;
	float alpha_scissor_threshold = 1.0;
	float alpha_hash_scale = 1.0;
	float alpha_antialiasing_edge = 0.0;
	vec2 alpha_texture_coordinate = vec2(0.0);
	vec2 point_coord = vec2(0.5);
	vec4 instance_custom = vec4(0.0);
	vec3 light_vertex = vertex;
	float global_time = params.time;

	mat4 inv_view_matrix = params.world_from_view;
	mat4 read_view_matrix = params.view_from_world;
	mat4 projection_matrix = mat4(1.0);
	mat4 inv_projection_matrix = mat4(1.0);
	mat4 read_model_matrix = inverse(rec.local_from_world);
	mat3 model_normal_matrix = normal_from_local;
	mat4 modelview = read_view_matrix * read_model_matrix;
	mat3 modelview_normal = view_from_world3 * model_normal_matrix;
	vec2 read_viewport_size = vec2(params.screen_size);
	HitSceneData scene_data;
	scene_data.main_cam_inv_view_matrix = params.world_from_view;
	scene_data.camera_visible_layers = 0xFFFFFFFFu;
	scene_data.emissive_exposure_normalization = params.emissive_exposure_normalization;
	scene_data.flags = 0u;
	scene_data.screen_pixel_size = 1.0 / vec2(params.screen_size);
	scene_data_block.data = scene_data;
	instances.data[0].instance_uniforms_ofs = rec.instance_uniforms_ofs;

	{
#CODE : FRAGMENT
	}

	float roughness = roughness_highp;
	float metallic = metallic_highp;
	vec3 albedo = albedo_highp;
	float alpha = alpha_highp;
	vec3 normal = normal_highp;

#ifdef ALPHA_SCISSOR_USED
	if (alpha < alpha_scissor_threshold) {
		hit_discarded = true;
	}
#endif
#ifdef ALPHA_HASH_USED
	if (alpha < 0.5) {
		hit_discarded = true;
	}
#endif

#if defined(NORMAL_MAP_USED)
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));
	normal = normalize(mix(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#endif

	// Back to the world for the lighting.
	mat3 world_from_view3 = mat3(params.world_from_view);
	vec3 n_shade = normalize(world_from_view3 * normal);
	if (dot(n_shade, geo_normal) < 0.0) {
		n_shade = geo_normal;
	}
	vec3 rel_pos = hit_rel_pos;
	vec3 radiance;

	if (hit_discarded) {
		// The material left a hole here (a leaf's alpha): the probes stand
		// in, as for a hit with no card.
		hit_write_discard();
		return;
	} else if (bool(params.flags & FLAG_DEBUG_ALBEDO)) {
		radiance = albedo;
	} else if (bool(params.flags & FLAG_DEBUG_NORMAL)) {
		radiance = n_shade * 0.5 + 0.5;
	} else if (bool(params.flags & FLAG_DEBUG_GEO_NORMAL)) {
		radiance = geo_normal * 0.5 + 0.5;
	} else if (bool(params.flags & FLAG_DEBUG_VERTEX_NORMAL)) {
		radiance = n_vertex * 0.5 + 0.5;
	} else if (bool(params.flags & FLAG_DEBUG_DIRECT)) {
		uint seed = 0u;
		radiance = shade_direct(world_pos, n_shade, world_pos + geo_normal * params.ray_bias, seed);
	} else if (bool(params.flags & FLAG_DEBUG_UV)) {
		radiance = vec3(fract(uv), 0.0);
	} else {
		vec3 origin = world_pos + geo_normal * params.ray_bias;
		uint seed = pcg_hash(uint(pixel.x) + pcg_hash(uint(pixel.y) + pcg_hash(params.frame + slot * 977u)));
		vec3 direct = (params.flags & FLAG_NO_DIRECT) != 0u ? vec3(0.0) : shade_direct(world_pos, n_shade, origin, seed);
		vec3 indirect = vec3(0.0);
		if ((params.flags & FLAG_NO_INDIRECT) == 0u) {
			// The card's accumulated bounce where the hit has one, a traced
			// ray otherwise (see card_indirect).
			if ((params.flags & FLAG_BOUNCE_TRACED) != 0u || !card_indirect(record, world_pos, n_shade, indirect)) {
				indirect = trace_bounce(origin, n_shade, rel_pos, seed);
			} else {
				tier_stat(1u, indirect);
			}
		}
		emission *= params.emissive_exposure_normalization;
		radiance = max(albedo * (direct + indirect * ao) + emission, vec3(0.0));
	}

	tier_stat(0u, radiance);
	results.data[result_index] = uvec4(rt_hit_pack_radiance(radiance), rt_hit_pack_dir(dir), result_flags);
}
