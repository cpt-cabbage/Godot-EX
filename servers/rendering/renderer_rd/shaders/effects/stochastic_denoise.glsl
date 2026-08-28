#[compute]

#version 450

#VERSION_DEFINES

// Denoiser for the stochastic direct lighting buffers, following the
// MegaLights / SVGF structure: temporal accumulation of lighting and its
// luminance moments (giving a per-pixel variance estimate), then a single
// variance-driven spatial pass using depth, normal and variance edge-stopping.
// Diffuse and specular are filtered as separate demodulated signals.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D in_diffuse;
layout(set = 0, binding = 1) uniform sampler2D in_specular;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;

#ifdef MODE_TEMPORAL
layout(set = 0, binding = 3) uniform sampler2D history_diffuse;
layout(set = 0, binding = 4) uniform sampler2D history_specular;
layout(set = 0, binding = 5) uniform sampler2D history_moments;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;
// xy: diffuse luminance 1st/2nd moment, zw: specular, plus accumulated frames
// encoded in the alpha of the lighting targets.
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_moments;
#else // MODE_SPATIAL
layout(set = 0, binding = 3) uniform sampler2D moments_texture;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_texture;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;
#endif

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC (temporal only).
	ivec2 screen_size;
	float blend_alpha; // Minimum weight of the current frame.
	float depth_tolerance;
	float variance_threshold; // Relative variance below which filtering is skipped.
	int stride; // Spatial kernel stride.
	float pad0;
	float pad1;
}
params;

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 rgb_to_ycocg(vec3 c) {
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
			0.5 * c.r - 0.5 * c.b,
			-0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 ycocg_to_rgb(vec3 c) {
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

#ifdef MODE_TEMPORAL

// Variance clipping in YCoCg (Salvi 2016, Karis 2014): clips history to an
// ellipsoid around the local mean, which avoids the color shifts that a
// per-channel RGB min/max clamp introduces.
vec3 clip_to_aabb(vec3 history, vec3 mean, vec3 extent, out float distance_outside) {
	vec3 h = rgb_to_ycocg(history);
	vec3 m = rgb_to_ycocg(mean);
	vec3 e = max(abs(rgb_to_ycocg(mean + extent) - m), vec3(1e-5));
	vec3 clipped = clamp(h, m - e, m + e);
	distance_outside = length((clipped - h) / e);
	return ycocg_to_rgb(clipped);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel, 0).r;
	if (center_depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_moments, pixel, vec4(0.0));
		return;
	}

	vec3 current_diffuse = texelFetch(in_diffuse, pixel, 0).rgb;
	vec3 current_specular = texelFetch(in_specular, pixel, 0).rgb;

	// 5x5 neighborhood statistics for history rectification.
	vec3 mean_d = vec3(0.0);
	vec3 mean_s = vec3(0.0);
	vec3 m2_d = vec3(0.0);
	vec3 m2_s = vec3(0.0);
	float count = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 sp = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			vec3 d = texelFetch(in_diffuse, sp, 0).rgb;
			vec3 s = texelFetch(in_specular, sp, 0).rgb;
			mean_d += d;
			mean_s += s;
			m2_d += d * d;
			m2_s += s * s;
			count += 1.0;
		}
	}
	mean_d /= count;
	mean_s /= count;
	vec3 stddev_d = sqrt(max(m2_d / count - mean_d * mean_d, vec3(0.0)));
	vec3 stddev_s = sqrt(max(m2_s / count - mean_s * mean_s, vec3(0.0)));

	float lum_d = luminance(current_diffuse);
	float lum_s = luminance(current_specular);

	vec3 result_diffuse = current_diffuse;
	vec3 result_specular = current_specular;
	vec4 moments = vec4(lum_d, lum_d * lum_d, lum_s, lum_s * lum_s);
	float frames = 1.0;
	// Shading confidence from the sampling pass (share of energy carried by
	// the strongest single light), carried in the specular history alpha.
	float dominance = texelFetch(in_diffuse, pixel, 0).a;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, center_depth, 1.0);
	if (prev_ndc.w > 0.0) {
		vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
		if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			vec4 hist_d4 = textureLod(history_diffuse, prev_uv, 0.0);
			vec4 hist_s4 = textureLod(history_specular, prev_uv, 0.0);
			vec4 hist_moments = textureLod(history_moments, prev_uv, 0.0);

			const float gamma = 1.5;
			float outside_d;
			float outside_s;
			vec3 hist_d = clip_to_aabb(hist_d4.rgb, mean_d, stddev_d * gamma, outside_d);
			vec3 hist_s = clip_to_aabb(hist_s4.rgb, mean_s, stddev_s * gamma, outside_s);

			// History confidence: the further the history was from the current
			// neighborhood, the faster it is discarded (reduces ghosting).
			float confidence = clamp(1.0 - max(outside_d, outside_s), 0.0, 1.0);
			frames = min(hist_d4.a * confidence + 1.0, 1.0 / max(params.blend_alpha, 1e-3));
			float alpha = max(1.0 / frames, params.blend_alpha);

			result_diffuse = mix(hist_d, current_diffuse, alpha);
			result_specular = mix(hist_s, current_specular, alpha);
			moments = mix(hist_moments, moments, alpha);
			dominance = mix(hist_s4.a, dominance, alpha);
		}
	}

	imageStore(out_diffuse, pixel, vec4(result_diffuse, frames));
	imageStore(out_specular, pixel, vec4(result_specular, dominance));
	imageStore(out_moments, pixel, moments);
}

#else // MODE_SPATIAL

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel, 0).r;
	vec4 center_d4 = texelFetch(in_diffuse, pixel, 0);
	vec4 center_s4 = texelFetch(in_specular, pixel, 0);
	if (center_depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		return;
	}

	vec4 moments = texelFetch(moments_texture, pixel, 0);
	float var_d = max(moments.y - moments.x * moments.x, 0.0);
	float var_s = max(moments.w - moments.z * moments.z, 0.0);

	// Only filter where the signal is actually noisy relative to its
	// magnitude; elsewhere the temporal result is already converged and
	// filtering would only cost sharpness. Also skip where a single light
	// carried ~80%+ of the energy (shading confidence): its shadow signal is
	// nearly binary, converges fast temporally, and spatial filtering would
	// only soften the edge.
	float rel_d = var_d / max(moments.x * moments.x, 1e-6);
	float rel_s = var_s / max(moments.z * moments.z, 1e-6);
	float frames = center_d4.a;
	float dominance = center_s4.a;
	bool newly_revealed = frames < 4.0;
	if (!newly_revealed && (max(rel_d, rel_s) < params.variance_threshold || (dominance > 0.8 && frames >= 8.0))) {
		imageStore(out_diffuse, pixel, center_d4);
		imageStore(out_specular, pixel, center_s4);
		return;
	}

	vec3 center_normal = normalize(texelFetch(normal_roughness_texture, pixel, 0).xyz * 2.0 - 1.0);
	float sigma_d = 4.0 * sqrt(var_d) + 1e-4;
	float sigma_s = 4.0 * sqrt(var_s) + 1e-4;

	// Newly revealed pixels have no usable variance estimate yet, so widen the
	// kernel and ignore the luminance stopping function for a few frames.
	int stride = newly_revealed ? params.stride * 2 : params.stride;

	// Rotate the sparse kernel per pixel so its footprint does not imprint a
	// grid pattern on the result; the rotation is static (not per frame) to
	// avoid shimmer after temporal accumulation.
	float angle = fract(dot(vec2(pixel), vec2(0.7548776662, 0.5698402909))) * 6.2831853;
	mat2 rot = mat2(vec2(cos(angle), -sin(angle)), vec2(sin(angle), cos(angle)));

	vec3 sum_d = center_d4.rgb;
	vec3 sum_s = center_s4.rgb;
	float weight_d = 1.0;
	float weight_s = 1.0;

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 sp = clamp(pixel + ivec2(round(rot * (vec2(x, y) * float(stride)))), ivec2(0), params.screen_size - 1);
			float sd = texelFetch(depth_texture, sp, 0).r;
			if (sd == 0.0) {
				continue;
			}

			// SVGF edge-stopping functions: depth, normal and luminance.
			float depth_diff = abs(sd - center_depth) / max(center_depth, 1e-6);
			if (depth_diff >= params.depth_tolerance) {
				continue;
			}
			vec3 n = normalize(texelFetch(normal_roughness_texture, sp, 0).xyz * 2.0 - 1.0);
			float w_normal = pow(max(dot(center_normal, n), 0.0), 32.0);
			if (w_normal <= 0.0) {
				continue;
			}
			float w_spatial = exp(-0.3 * float(x * x + y * y)) * w_normal;

			vec3 d = texelFetch(in_diffuse, sp, 0).rgb;
			vec3 s = texelFetch(in_specular, sp, 0).rgb;

			float wd = w_spatial;
			float ws = w_spatial;
			if (!newly_revealed) {
				wd *= exp(-abs(luminance(d) - moments.x) / sigma_d);
				ws *= exp(-abs(luminance(s) - moments.z) / sigma_s);
			}

			sum_d += d * wd;
			sum_s += s * ws;
			weight_d += wd;
			weight_s += ws;
		}
	}

	imageStore(out_diffuse, pixel, vec4(sum_d / weight_d, frames));
	imageStore(out_specular, pixel, vec4(sum_s / weight_s, dominance));
}

#endif
