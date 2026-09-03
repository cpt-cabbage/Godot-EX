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
	uint pad_grid0;
	uint pad_grid1;
}
params;

#define FLAG_SDFGI 1u
#define FLAG_SKY_MODE_SKY 2u
#define FLAG_SKY_MODE_COLOR 4u
#define FLAG_SHARED_BOUNCE_RAY 16u
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

// r: the unshadowed direct luminance (plus emission) at the last relight,
// g: its relative change since the relight before. The direct term is
// deterministic, so a change in it is a change in the lights, not noise:
// A-SVGF's temporal gradient with the relight as the re-shade. It scales the
// restart of the accumulations below, and the GI gather reads it at hits.
layout(set = 0, binding = 21, rg16f) uniform restrict image2D change_atlas;

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
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

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
bool card_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance, out uint r_set, out float r_change) {
	r_radiance = vec3(0.0);
	r_set = SURFACE_CACHE_INVALID;
	r_change = 0.0;
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	r_set = inst.set;
	CardSet s = sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return false;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	ivec2 best_texel = ivec2(0);
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
			best_texel = texel;
		}
	}
	if (best_w <= 0.0) {
		return false;
	}
	r_radiance = imageLoad(lighting_atlas, best_texel).rgb;
	r_change = imageLoad(change_atlas, best_texel).g;
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
void shade_direct(uint entry, Texel t, inout uint seed, out vec3 direct, out vec3 direct_unshadowed) {
	direct = vec3(0.0);
	direct_unshadowed = vec3(0.0);

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

	// The lights: the texel's cell of the world light grid (every light
	// that reaches the texel), or the set's list (the first 32 overlapping
	// its box) where the grid is off or the texel lies outside it.
	uint base = entry * (1u + MAX_LIGHTS_PER_SET);
	uint light_count = min(set_lights.data[base], MAX_LIGHTS_PER_SET);
	bool from_grid = false;
	if (bool(params.flags & FLAG_GRID)) {
		vec3 rel = (t.world_pos - params.grid_origin) / params.grid_cell;
		if (all(greaterThanEqual(rel, vec3(0.0))) && all(lessThan(rel, vec3(float(params.grid_n))))) {
			uvec3 c = uvec3(rel);
			base = (c.x + params.grid_n * (c.y + params.grid_n * c.z)) * (1u + params.grid_cap);
			light_count = min(light_grid.data[base], params.grid_cap);
			from_grid = true;
		}
	}
	if ((params.debug & 4u) != 0u) {
		light_count = 0u;
	}
	for (uint j = 0u; j < light_count; j++) {
		uint idx = from_grid ? light_grid.data[base + 1u + j] : set_lights.data[base + 1u + j];
		bool is_spot = from_grid ? (idx & GRID_SPOT_BIT) != 0u : idx >= params.omni_light_count;
		if (from_grid) {
			idx &= ~GRID_SPOT_BIT;
		} else if (is_spot) {
			idx -= params.omni_light_count;
		}
		LightData ld = is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		vec3 rel = pos - t.world_pos;
		float len = length(rel);
		float attenuation = get_omni_attenuation(len, ld.inv_radius, ld.attenuation);
		vec3 l = rel / max(len, 1e-5);
		if (is_spot) {
			vec3 spot_dir = normalize(mat3(params.world_from_view) * ld.direction);
			float scos = max(dot(-l, spot_dir), ld.cone_angle);
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
			attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
		}
		float ndotl = max(dot(t.n_world, l), 0.0);
		vec3 c = ld.color * (ndotl * attenuation * (1.0 / M_PI));
		float w = luminance(abs(c));
		if (w <= 0.0) {
			continue;
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
	direct_unshadowed += sum;
	if (selected) {
		float vis = 1.0;
		if (sel_opacity > 0.001 && sel_mask != 0u && (params.debug & 2u) == 0u) {
			vec3 to_light = sel_pos - t.origin;
			float dist = length(to_light);
			vis = mix(1.0, occluded(t.origin, to_light / max(dist, 1e-5), max(dist - params.ray_bias, 0.0), sel_mask) ? 0.0 : 1.0, sel_opacity);
		}
		direct += sum * vis;
	}

}

// The indirect term: one cosine ray into the scene (see main).
void trace_bounce(Texel t, inout uint seed, out vec3 indirect_sample, out float bounce_change) {
	indirect_sample = vec3(0.0);
	bounce_change = 0.0;
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
			if (card_lookup(hit_instance, t.origin + ray_dir * t_hit, ray_dir, card_radiance, hit_set, hit_change)) {
				indirect_sample = card_radiance;
				card_requests.frame[hit_set] = params.frame;
				// The bounce carries the change of the card it came from,
				// weaker by a quarter per bounce, so lighting that reaches
				// this texel only indirectly restarts it too.
				bounce_change = hit_change - 0.25;
			} else if (!sdfgi_probe_irradiance(t.world_pos - params.camera_origin.xyz, t.n_world, indirect_sample)) {
				indirect_sample = sky_eval(t.n_world);
			}
		} else {
			indirect_sample = sky_eval(ray_dir);
		}
	}
}

// The temporal gradient and the two accumulations, into the atlases.
void accumulate(ivec2 texel, Texel t, bool reset, vec3 direct, vec3 direct_unshadowed, vec3 indirect_sample, float bounce_change) {

	// The temporal gradient: how much the deterministic part of this texel's
	// lighting moved since the last relight, relative to itself. Static
	// lights give exactly zero (the numbers are recomputed from the same
	// inputs); a fresh capture has nothing to compare with.
	// A texel never lit since its capture (no frames accumulated) has nothing
	// to compare with either: the capture's reset flag is raised on the frame
	// its record is built, which is not always the frame it is first lit.
	vec4 old = imageLoad(lighting_atlas, texel);
	float lum_unshadowed = luminance(t.albedo * direct_unshadowed + t.emission);
	vec2 prev_change = imageLoad(change_atlas, texel).rg;
	float change = (reset || old.a <= 0.0) ? 0.0 : abs(lum_unshadowed - prev_change.x) / max(max(lum_unshadowed, prev_change.x), 1e-4);
	// The change outlives the relight that found it, fading over eight: the
	// gather's one ray per pixel lands on a given card only now and then,
	// and a change seen for one frame would restart almost no pixel.
	change = max(change, max(prev_change.y - 0.125, bounce_change));
	imageStore(change_atlas, texel, vec4(lum_unshadowed, change, 0.0, 0.0));
	// The accumulations restart to the frame count the change leaves credible
	// (A-SVGF: alpha = max(alpha, gradient)); a small change barely touches
	// them, a light switching costs them all their frames but one.
	float keep_frames = change > 0.02 ? max(1.0, 1.0 / change) : 64.0;

	// The indirect estimate accumulates on its own: one ray per frame is far
	// noisier than the direct term's one shadow ray.
	vec4 old_indirect = imageLoad(indirect_atlas, texel);
	float ind_frames = reset ? 0.0 : min(old_indirect.a * 64.0, keep_frames);
	float ind_alpha = max(1.0 / (ind_frames + 1.0), 1.0 / float(params.temporal_frames));
	vec3 indirect = ind_frames <= 0.0 ? indirect_sample : mix(old_indirect.rgb, indirect_sample, ind_alpha);
	imageStore(indirect_atlas, texel, vec4(indirect, min(ind_frames + 1.0, 64.0) / 64.0));

	vec3 radiance = max(t.albedo * (direct + indirect) + t.emission, vec3(0.0));

	float frames = reset ? 0.0 : min(old.a * 64.0, keep_frames);
	float alpha = max(1.0 / (frames + 1.0), 1.0 / float(params.temporal_frames));
	vec3 accum = frames <= 0.0 ? radiance : mix(old.rgb, radiance, alpha);
	imageStore(lighting_atlas, texel, vec4(accum, min(frames + 1.0, 64.0) / 64.0));
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

	if (!quad_mode) {
		ivec2 texel_in_card = block_origin + ivec2(gl_LocalInvocationID.xy);
		ivec2 texel = origin_texel + texel_in_card;
		Texel t;
		if (!read_texel(s, card, dims, texel_in_card, texel, t)) {
			return; // Nothing captured here.
		}
		uint seed = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		vec3 direct, direct_unshadowed;
		shade_direct(entry, t, seed, direct, direct_unshadowed);
		vec3 indirect_sample;
		float bounce_change;
		trace_bounce(t, seed, indirect_sample, bounce_change);
		accumulate(texel, t, reset, direct, direct_unshadowed, indirect_sample, bounce_change);
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
	vec3 indirect_sample;
	float bounce_change;
	trace_bounce(t[tracer], seed, indirect_sample, bounce_change);
	for (uint k = 0u; k < 4u; k++) {
		if (!valid[k]) {
			continue;
		}
		ivec2 texel = origin_texel + quad_in_card + ivec2(int(k & 1u), int(k >> 1u));
		uint seed_k = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		vec3 direct, direct_unshadowed;
		shade_direct(entry, t[k], seed_k, direct, direct_unshadowed);
		accumulate(texel, t[k], reset, direct, direct_unshadowed, indirect_sample, bounce_change);
	}
}
