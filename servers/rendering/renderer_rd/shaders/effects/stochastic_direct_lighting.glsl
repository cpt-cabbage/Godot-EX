#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// Stochastic direct lighting (mini-MegaLights).
// Per pixel: weighted reservoir sampling over a candidate set built from the
// previous frame's visible light list (guided) and a strided subset of the
// clustered light grid cell (discovery), one ray-query visibility ray per
// unique selected sample, shading of visible samples into demodulated diffuse
// (irradiance, no albedo) and specular buffers.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 3, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;
layout(set = 0, binding = 4, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

// Visible light list from the previous frame, one fixed-size list per 8x8 tile.
layout(set = 0, binding = 5, std430) restrict readonly buffer LightList {
	uint data[];
}
prev_light_list;

layout(set = 0, binding = 6, std140) uniform Params {
	mat4 view_from_ndc; // Inverse of the (depth-corrected) projection.
	mat4 world_from_view; // Camera transform.
	mat4 reproject; // Current NDC -> previous frame NDC, for the tile lookup.
	ivec2 screen_size;
	uint omni_light_count;
	uint spot_light_count;
	uint frame_index;
	float ray_bias;
	int tiles_x;
	int tiles_y;
	uint cluster_shift;
	uint cluster_width;
	uint max_cluster_element_count_div_32;
	uint cluster_type_size;
	float z_far;
	float pad0;
	float pad1;
	float pad2;
}
params;

// The froxel light grid built by clustered forward culling. Same layout as the
// scene shader: per cell, per light type, max_cluster_element_count_div_32
// bitmask words followed by 32 packed z-slice min/max element ranges.
layout(set = 0, binding = 7, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;
// One light this pixel found visible, gathered next frame into the tile lists.
layout(set = 1, binding = 2, r32ui) uniform restrict writeonly uimage2D out_visible_light;

#define M_PI 3.14159265359
#define RESERVOIR_COUNT 4u
#define TILE_SIZE 8
#define LIST_SIZE 8
#define INVALID_LIGHT 0xFFFFFFFFu
#define SPOT_BIT 0x80000000u
// Bounds the RIS estimator. Rarely selected lights produce a huge
// weight_sum/selected_weight ratio, which shows up as fireflies and, once
// filtered, as blotches. Slightly biased, but far lower variance.
#define ESTIMATOR_CLAMP 48.0

// Candidate set: the guided (visible list) candidates plus a strided subset of
// the cluster cell. Keeping the per-pixel candidate count fixed is what makes
// the cost independent of the total light count (MegaLights' central promise);
// the stride multiplier keeps the subset an unbiased representation.
#define MAX_GUIDED_CANDIDATES 8u
#define MAX_DISCOVERY_CANDIDATES 12u
#define MAX_CANDIDATES 20u

// Hidden lights (not on the visible list) are clamped to this share of the
// total sampling weight, relaxed when the visible lights are dim (paper's
// hidden light budget). Weight clamping keeps the RIS estimator unbiased.
#define HIDDEN_WEIGHT_BUDGET 0.2
#define DIM_VISIBLE_WEIGHT 0.25

// Samples whose unshadowed contribution is below this fraction of the pixel's
// total unshadowed luminance are dropped without tracing a ray (the paper's
// exposure-relative sample culling; we express the threshold relative to the
// pixel's own lighting since the pass runs before exposure).
#define CULL_CONTRIBUTION_FRACTION 0.002

// PCG hash: decorrelates the per-pixel random variables. A screen-space
// gradient pattern makes neighboring pixels select the same lights, which
// spatial filtering turns into blotches rather than smooth gradients.
uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
	return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return nd * pow(max(dist, 0.0001), -decay);
}

struct Reservoir {
	uint candidate; // Index into the candidate arrays; 0xFFFFFFFF = none.
	float weight_sum;
	float selected_weight;
};

// Streaming weighted reservoir sampling, warping the random variable back to
// [0;1) after each decision so one variable drives the whole loop.
void reservoir_update(inout Reservoir r, uint index, float w, inout float rng) {
	if (w <= 0.0) {
		return;
	}
	r.weight_sum += w;
	float p = w / r.weight_sum;
	if (rng < p) {
		r.candidate = index;
		r.selected_weight = w;
		rng = rng / p;
	} else {
		rng = (rng - p) / (1.0 - p);
	}
	rng = clamp(rng, 0.0, 0.9999999);
}

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Perceptual weight (MegaLights): tames very strong lights.
float light_weight(float lum) {
	return log2(lum + 1.0);
}

// Unshadowed diffuse (radiance, no albedo) and specular contribution of a
// light at a view-space point. Zero when out of range or facing away.
void light_eval(bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec3 light_rel_vec) {
	LightData ld = is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
	light_rel_vec = ld.position - view_pos;
	float light_length = length(light_rel_vec);
	float attenuation = get_omni_attenuation(light_length, ld.inv_radius, ld.attenuation);
	if (is_spot) {
		float scos = max(dot(-normalize(light_rel_vec), ld.direction), ld.cone_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
		attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
	}
	vec3 l = normalize(light_rel_vec);
	float ndotl = max(dot(view_normal, l), 0.0);
	diffuse = ld.color * (ndotl * (1.0 / M_PI) * attenuation);

	// Schlick-GGX, dielectric F0. The prepass has no albedo/metallic, so the
	// specular is an approximation the composite cannot recover exactly.
	vec3 v = normalize(-view_pos);
	vec3 h = normalize(v + l);
	float ndotv = max(dot(view_normal, v), 1e-4);
	float ndoth = max(dot(view_normal, h), 0.0);
	float ldoth = max(dot(l, h), 0.0);

	float alpha = max(roughness * roughness, 1e-3);
	float alpha2 = alpha * alpha;
	float d = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
	float D = alpha2 / (M_PI * d * d);
	float k = alpha * 0.5;
	float G = (ndotl / (ndotl * (1.0 - k) + k)) * (ndotv / (ndotv * (1.0 - k) + k));
	const float f0 = 0.04;
	float F = f0 + (1.0 - f0) * pow(1.0 - ldoth, 5.0);
	specular = ld.color * attenuation * ndotl * (D * G * F / max(4.0 * ndotv, 1e-4)) * ld.specular_amount;
}

bool trace_visible(vec3 world_origin, vec3 world_target) {
	vec3 delta = world_target - world_origin;
	float dist = length(delta);
	if (dist < 1e-4) {
		return true;
	}
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
			0xFF, world_origin, params.ray_bias, delta / dist, dist - params.ray_bias);
	rayQueryProceedEXT(rq);
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT;
}

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;
	item_from = item_min >> 5;
	item_to = (item_max == 0u) ? 0u : ((item_max - 1u) >> 5) + 1u;
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

// Candidate set for this pixel's reservoir sampling.
uint candidate_entries[MAX_CANDIDATES];
float candidate_weights[MAX_CANDIDATES];
float candidate_lum[MAX_CANDIDATES]; // Unshadowed luminance, for culling.
uint candidate_count = 0u;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float depth = texelFetch(depth_texture, pixel, 0).r;
	if (depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_visible_light, pixel, uvec4(INVALID_LIGHT));
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 view_pos4 = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 view_pos = view_pos4.xyz / view_pos4.w;

	vec4 nr = texelFetch(normal_roughness_texture, pixel, 0);
	vec3 view_normal = normalize(nr.xyz * 2.0 - 1.0);
	float roughness = nr.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);

	uint pixel_seed = pcg_hash(uint(pixel.x) + pcg_hash(uint(pixel.y) + pcg_hash(params.frame_index)));

	// Look up the visible light list built last frame, reprojecting into the
	// previous frame's tile grid. The tile is jittered stochastically so the
	// transition between neighboring tile lists is dithered instead of showing
	// up as an 8 pixel grid.
	uint visible_list[LIST_SIZE];
	uint visible_count = 0u;
	{
		vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, depth, 1.0);
		if (prev_ndc.w > 0.0) {
			vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
			vec2 jitter = vec2(hash_to_float(pcg_hash(pixel_seed + 0x51u)), hash_to_float(pcg_hash(pixel_seed + 0x97u))) - 0.5;
			vec2 tile_coord = (prev_uv * vec2(params.screen_size)) / float(TILE_SIZE) + jitter;
			ivec2 tile = ivec2(floor(tile_coord));
			if (all(greaterThanEqual(tile, ivec2(0))) && tile.x < params.tiles_x && tile.y < params.tiles_y) {
				uint base = uint(tile.y * params.tiles_x + tile.x) * uint(LIST_SIZE);
				for (uint i = 0u; i < uint(LIST_SIZE); i++) {
					uint entry = prev_light_list.data[base + i];
					if (entry == INVALID_LIGHT) {
						break;
					}
					// Drop stale entries from lights that no longer exist.
					uint idx = entry & ~SPOT_BIT;
					uint count = (entry & SPOT_BIT) != 0u ? params.spot_light_count : params.omni_light_count;
					if (idx < count) {
						visible_list[visible_count++] = entry;
					}
				}
			}
		}
	}

	// Guided candidates: the lights the tile saw last frame.
	float guided_weight_sum = 0.0;
	// Total unshadowed luminance estimate, for the sample culling threshold.
	float total_lum = 0.0;
	vec3 unused;
	for (uint i = 0u; i < visible_count && candidate_count < MAX_GUIDED_CANDIDATES; i++) {
		uint entry = visible_list[i];
		vec3 f, s;
		light_eval((entry & SPOT_BIT) != 0u, entry & ~SPOT_BIT, view_pos, view_normal, roughness, f, s, unused);
		float lum = luminance(f + s);
		float w = light_weight(lum);
		if (w <= 0.0) {
			continue;
		}
		candidate_entries[candidate_count] = entry;
		candidate_weights[candidate_count] = w;
		candidate_lum[candidate_count] = lum;
		guided_weight_sum += w;
		total_lum += lum;
		candidate_count++;
	}
	uint guided_count = candidate_count;

	// Discovery candidates: a strided subset of this pixel's cluster cell, so
	// newly visible lights are still found. The stride multiplier on the weight
	// keeps the subset an unbiased stand-in for the cell's full light list, and
	// bounds the per-pixel cost regardless of how many lights the scene has.
	{
		uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
		uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
		uint cluster_z = uint(clamp((-view_pos.z / params.z_far) * 32.0, 0.0, 31.0));

		// First pass: count the candidates in the cell (omni then spot).
		uint cell_count = 0u;
		for (uint type = 0u; type < 2u; type++) {
			uint type_offset = cluster_offset + type * params.cluster_type_size;
			uint item_min, item_max, item_from, item_to;
			cluster_get_item_range(type_offset + params.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);
			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[type_offset + i] & cluster_get_range_clip_mask(i, item_min, item_max);
				cell_count += uint(bitCount(mask));
			}
		}

		uint stride = max(1u, (cell_count + MAX_DISCOVERY_CANDIDATES - 1u) / MAX_DISCOVERY_CANDIDATES);
		uint start = uint(hash_to_float(pcg_hash(pixel_seed + 0x3Du)) * float(stride));
		float stride_mult = float(stride);

		uint cell_index = 0u;
		for (uint type = 0u; type < 2u; type++) {
			uint type_offset = cluster_offset + type * params.cluster_type_size;
			uint item_min, item_max, item_from, item_to;
			cluster_get_item_range(type_offset + params.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);
			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[type_offset + i] & cluster_get_range_clip_mask(i, item_min, item_max);
				while (mask != 0u) {
					uint bit = findLSB(mask);
					mask &= ~(1u << bit);
					uint take = cell_index++;
					if (take % stride != start) {
						continue;
					}
					uint entry = (32u * i + bit) | (type == 1u ? SPOT_BIT : 0u);
					// Skip lights already on the guided list.
					bool listed = false;
					for (uint j = 0u; j < guided_count; j++) {
						if (candidate_entries[j] == entry) {
							listed = true;
							break;
						}
					}
					if (listed || candidate_count >= MAX_CANDIDATES) {
						continue;
					}
					vec3 f, s;
					light_eval((entry & SPOT_BIT) != 0u, entry & ~SPOT_BIT, view_pos, view_normal, roughness, f, s, unused);
					float lum = luminance(f + s);
					float w = light_weight(lum);
					if (w <= 0.0) {
						continue;
					}
					candidate_entries[candidate_count] = entry;
					candidate_weights[candidate_count] = w * stride_mult;
					candidate_lum[candidate_count] = lum;
					total_lum += lum * stride_mult;
					candidate_count++;
				}
			}
		}
	}

	// Hidden light budget: clamp discovery weights to a fixed share of the
	// total, relaxed when the guided lights are dim so a brighter light that
	// just became visible can still win quickly.
	if (guided_count > 0u && candidate_count > guided_count) {
		float hidden_weight_sum = 0.0;
		for (uint i = guided_count; i < candidate_count; i++) {
			hidden_weight_sum += candidate_weights[i];
		}
		if (hidden_weight_sum > 0.0) {
			float budget = (HIDDEN_WEIGHT_BUDGET / (1.0 - HIDDEN_WEIGHT_BUDGET)) * guided_weight_sum;
			float scale = min(1.0, budget / hidden_weight_sum);
			float relax = clamp(1.0 - guided_weight_sum / DIM_VISIBLE_WEIGHT, 0.0, 1.0);
			scale = mix(scale, 1.0, relax);
			for (uint i = guided_count; i < candidate_count; i++) {
				candidate_weights[i] *= scale;
			}
		}
	}

	Reservoir reservoirs[RESERVOIR_COUNT];
	float rngs[RESERVOIR_COUNT];
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		reservoirs[r].candidate = INVALID_LIGHT;
		reservoirs[r].weight_sum = 0.0;
		reservoirs[r].selected_weight = 0.0;
		rngs[r] = hash_to_float(pcg_hash(pixel_seed + r * 0x9E3779B9u));
	}
	for (uint i = 0u; i < candidate_count; i++) {
		for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
			reservoir_update(reservoirs[r], i, candidate_weights[i], rngs[r]);
		}
	}

	vec3 world_pos = (params.world_from_view * vec4(view_pos, 1.0)).xyz;
	mat3 world_basis = mat3(params.world_from_view);

	// Trace each unique selected light once (reservoirs frequently agree when
	// few lights dominate; duplicate rays would hit the same target).
	uint traced_candidates[RESERVOIR_COUNT];
	bool traced_visible[RESERVOIR_COUNT];
	uint traced_count = 0u;

	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);
	uint chosen_visible_light = INVALID_LIGHT;
	uint visible_found = 0u;
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		uint c = reservoirs[r].candidate;
		if (c == INVALID_LIGHT) {
			continue;
		}
		uint entry = candidate_entries[c];

		// Exposure-relative culling: skip rays for samples too dim to matter.
		if (candidate_lum[c] < CULL_CONTRIBUTION_FRACTION * total_lum) {
			continue;
		}

		bool is_spot = (entry & SPOT_BIT) != 0u;
		uint idx = entry & ~SPOT_BIT;

		vec3 f, s, light_rel_vec;
		light_eval(is_spot, idx, view_pos, view_normal, roughness, f, s, light_rel_vec);

		bool visible = false;
		bool found = false;
		for (uint t = 0u; t < traced_count; t++) {
			if (traced_candidates[t] == c) {
				visible = traced_visible[t];
				found = true;
				break;
			}
		}
		if (!found) {
			vec3 world_light = world_pos + world_basis * light_rel_vec;
			visible = trace_visible(world_pos, world_light);
			traced_candidates[traced_count] = c;
			traced_visible[traced_count] = visible;
			traced_count++;
		}
		if (!visible) {
			continue;
		}

		// Pick one visible light uniformly to seed next frame's tile list.
		visible_found++;
		if (hash_to_float(pcg_hash(pixel_seed + 0xB5u + visible_found)) < 1.0 / float(visible_found)) {
			chosen_visible_light = entry;
		}

		// RIS estimator f * (weight_sum / selected_weight), averaged over the
		// reservoirs and clamped to bound variance.
		float estimator = min(reservoirs[r].weight_sum / max(reservoirs[r].selected_weight, 1e-6), ESTIMATOR_CLAMP) / float(RESERVOIR_COUNT);
		diffuse += f * estimator;
		specular += s * estimator;
	}

	imageStore(out_diffuse, pixel, vec4(diffuse, 1.0));
	imageStore(out_specular, pixel, vec4(specular, 1.0));
	imageStore(out_visible_light, pixel, uvec4(chosen_visible_light));
}
