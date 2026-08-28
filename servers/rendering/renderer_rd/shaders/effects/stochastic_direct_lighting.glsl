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

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 view_from_ndc; // Inverse of the (depth-corrected) projection.
	mat4 world_from_view; // Camera transform.
	ivec2 screen_size;
	uint omni_light_count;
	uint spot_light_count;
	uint frame_index;
	float ray_bias;
	uint pad0;
	uint pad1;
}
params;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;

#define M_PI 3.14159265359
#define RESERVOIR_COUNT 2u
#define SPOT_BIT 0x80000000u

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

	// Interleaved gradient noise advanced per frame with the golden ratio.
	float ign = fract(52.9829189 * fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y)));
	float golden = float(params.frame_index % 64u) * 0.61803398875;

	Reservoir reservoirs[RESERVOIR_COUNT];
	float rngs[RESERVOIR_COUNT];
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		reservoirs[r].light_index = 0xFFFFFFFFu;
		reservoirs[r].weight_sum = 0.0;
		reservoirs[r].selected_weight = 0.0;
		rngs[r] = fract(ign + golden + float(r) * 0.38196601125);
	}

	vec3 unused;
	for (uint i = 0u; i < params.omni_light_count; i++) {
		float w = light_weight(light_diffuse_contribution(false, i, view_pos, view_normal, unused));
		for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
			reservoir_update(reservoirs[r], i, w, rngs[r]);
		}
	}
	for (uint i = 0u; i < params.spot_light_count; i++) {
		float w = light_weight(light_diffuse_contribution(true, i, view_pos, view_normal, unused));
		for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
			reservoir_update(reservoirs[r], i | SPOT_BIT, w, rngs[r]);
		}
	}

	vec3 world_pos = (params.world_from_view * vec4(view_pos, 1.0)).xyz;
	mat3 world_basis = mat3(params.world_from_view);

	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);
	for (uint r = 0u; r < RESERVOIR_COUNT; r++) {
		if (reservoirs[r].light_index == 0xFFFFFFFFu) {
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

		// Unbiased RIS estimator: f * (weight_sum / selected_weight), averaged
		// over the independent reservoirs.
		float estimator = (reservoirs[r].weight_sum / max(reservoirs[r].selected_weight, 1e-6)) / float(RESERVOIR_COUNT);
		diffuse += f * estimator;
		specular += light_specular_contribution(is_spot, idx, view_pos, view_normal, roughness, f) * estimator;
	}

	imageStore(out_diffuse, pixel, vec4(diffuse, 1.0));
	imageStore(out_specular, pixel, vec4(specular, 1.0));
}
