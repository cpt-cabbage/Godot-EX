#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// Stochastic direct lighting (mini-MegaLights, phase A).
// Per pixel: weighted reservoir sampling of omni/spot lights, one ray-query
// visibility ray per selected sample, shading of visible samples into
// demodulated diffuse (irradiance, no albedo) and specular buffers.

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
}
params;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;
// One light this pixel found visible, gathered next frame into the tile lists.
layout(set = 1, binding = 2, r32ui) uniform restrict writeonly uimage2D out_visible_light;

#define M_PI 3.14159265359
#define RESERVOIR_COUNT 4u
// Reservoirs drawn from the guided (visible) list; the rest go to lights not
// on the list, so newly visible lights are still discovered. Mirrors the
// paper's hidden light sample budget.
#define VISIBLE_RESERVOIR_COUNT 3u
#define TILE_SIZE 8
#define LIST_SIZE 8
#define INVALID_LIGHT 0xFFFFFFFFu
#define SPOT_BIT 0x80000000u
// Bounds the RIS estimator. Rarely selected lights produce a huge
// weight_sum/selected_weight ratio, which shows up as fireflies and, once
// filtered, as blotches. Slightly biased, but far lower variance.
#define ESTIMATOR_CLAMP 48.0

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
	uint light_index; // SPOT_BIT marks spot lights; 0xFFFFFFFF = none.
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
		r.light_index = index;
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
float light_weight(vec3 contribution) {
	return log2(luminance(contribution) + 1.0);
}

// Unshadowed diffuse contribution (radiance, no albedo) of a light at a
// view-space point. Returns vec3(0) when out of range or facing away.
vec3 light_diffuse_contribution(bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, out vec3 light_rel_vec) {
	if (is_spot) {
		LightData ld = spot_lights.data[idx];
		light_rel_vec = ld.position - view_pos;
		float light_length = length(light_rel_vec);
		float attenuation = get_omni_attenuation(light_length, ld.inv_radius, ld.attenuation);
		float scos = max(dot(-normalize(light_rel_vec), ld.direction), ld.cone_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
		attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
		float ndotl = max(dot(view_normal, normalize(light_rel_vec)), 0.0);
		return ld.color * (ndotl * (1.0 / M_PI) * attenuation);
	} else {
		LightData ld = omni_lights.data[idx];
		light_rel_vec = ld.position - view_pos;
		float light_length = length(light_rel_vec);
		float attenuation = get_omni_attenuation(light_length, ld.inv_radius, ld.attenuation);
		float ndotl = max(dot(view_normal, normalize(light_rel_vec)), 0.0);
		return ld.color * (ndotl * (1.0 / M_PI) * attenuation);
	}
}

// Schlick-GGX specular for a visible sample.
vec3 light_specular_contribution(bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, float roughness, vec3 diffuse_radiance) {
	LightData ld = is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
	vec3 l = normalize(ld.position - view_pos);
	vec3 v = normalize(-view_pos);
	vec3 h = normalize(v + l);
	float ndotl = max(dot(view_normal, l), 0.0);
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

	// diffuse_radiance already contains color * ndotl/PI * attenuation; recover
	// color * attenuation by dividing the lambert term out.
	vec3 light_radiance = diffuse_radiance * (M_PI / max(ndotl, 1e-4));
	return light_radiance * (D * G * F / max(4.0 * ndotv, 1e-4)) * ld.specular_amount;
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

	// Split the sample budget between the guided list and everything else. The
	// stream probabilities are folded into the estimator, so the result stays
	// an unbiased estimate of the full light sum either way.
	uint visible_reservoirs = visible_count > 0u ? VISIBLE_RESERVOIR_COUNT : 0u;
	float q_visible = float(visible_reservoirs) / float(RESERVOIR_COUNT);
	float q_hidden = 1.0 - q_visible;

	Reservoir reservoirs[RESERVOIR_COUNT];
	float rngs[RESERVOIR_COUNT];
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		reservoirs[r].light_index = INVALID_LIGHT;
		reservoirs[r].weight_sum = 0.0;
		reservoirs[r].selected_weight = 0.0;
		rngs[r] = hash_to_float(pcg_hash(pixel_seed + r * 0x9E3779B9u));
	}

	vec3 unused;
	// Guided stream: only the lights the tile saw last frame.
	for (uint i = 0u; i < visible_count; i++) {
		uint entry = visible_list[i];
		bool is_spot = (entry & SPOT_BIT) != 0u;
		float w = light_weight(light_diffuse_contribution(is_spot, entry & ~SPOT_BIT, view_pos, view_normal, unused));
		for (uint r = 0u; r < visible_reservoirs; r++) {
			reservoir_update(reservoirs[r], entry, w, rngs[r]);
		}
	}

	// Discovery stream: every light that is not already on the list.
	for (uint i = 0u; i < params.omni_light_count + params.spot_light_count; i++) {
		bool is_spot = i >= params.omni_light_count;
		uint entry = is_spot ? ((i - params.omni_light_count) | SPOT_BIT) : i;
		bool listed = false;
		for (uint j = 0u; j < visible_count; j++) {
			if (visible_list[j] == entry) {
				listed = true;
				break;
			}
		}
		if (listed) {
			continue;
		}
		float w = light_weight(light_diffuse_contribution(is_spot, entry & ~SPOT_BIT, view_pos, view_normal, unused));
		for (uint r = visible_reservoirs; r < RESERVOIR_COUNT; r++) {
			reservoir_update(reservoirs[r], entry, w, rngs[r]);
		}
	}

	vec3 world_pos = (params.world_from_view * vec4(view_pos, 1.0)).xyz;
	mat3 world_basis = mat3(params.world_from_view);

	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);
	uint chosen_visible_light = INVALID_LIGHT;
	uint visible_found = 0u;
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		if (reservoirs[r].light_index == INVALID_LIGHT) {
			continue;
		}
		float q_stream = r < visible_reservoirs ? q_visible : q_hidden;
		if (q_stream <= 0.0) {
			continue;
		}
		bool is_spot = (reservoirs[r].light_index & SPOT_BIT) != 0u;
		uint idx = reservoirs[r].light_index & ~SPOT_BIT;

		vec3 light_rel_vec;
		vec3 f = light_diffuse_contribution(is_spot, idx, view_pos, view_normal, light_rel_vec);
		float w = light_weight(f);
		if (w <= 0.0) {
			continue;
		}

		vec3 world_light = world_pos + world_basis * light_rel_vec;
		if (!trace_visible(world_pos, world_light)) {
			continue;
		}

		// Pick one visible light uniformly to seed next frame's tile list.
		visible_found++;
		if (hash_to_float(pcg_hash(pixel_seed + 0xB5u + visible_found)) < 1.0 / float(visible_found)) {
			chosen_visible_light = reservoirs[r].light_index;
		}

		// RIS estimator f * (weight_sum / selected_weight), divided by the
		// probability of having drawn from this stream, averaged over the
		// reservoirs and clamped to bound variance.
		float estimator = min(reservoirs[r].weight_sum / max(reservoirs[r].selected_weight * q_stream, 1e-6), ESTIMATOR_CLAMP) / float(RESERVOIR_COUNT);
		diffuse += f * estimator;
		specular += light_specular_contribution(is_spot, idx, view_pos, view_normal, roughness, f) * estimator;
	}

	imageStore(out_diffuse, pixel, vec4(diffuse, 1.0));
	imageStore(out_specular, pixel, vec4(specular, 1.0));
	imageStore(out_visible_light, pixel, uvec4(chosen_visible_light));
}
