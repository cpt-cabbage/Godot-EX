#[compute]

#version 450

#VERSION_DEFINES

// Denoiser for the stochastic direct lighting buffers: a depth-aware 5x5
// spatial filter of the current frame fused with reprojected temporal
// accumulation, clamped to the local neighborhood to reject stale history.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D raw_diffuse;
layout(set = 0, binding = 1) uniform sampler2D raw_specular;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(set = 0, binding = 3) uniform sampler2D history_diffuse;
layout(set = 0, binding = 4) uniform sampler2D history_specular;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image2D out_specular;
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_history_diffuse;
layout(set = 1, binding = 3, rgba16f) uniform restrict writeonly image2D out_history_specular;

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC.
	ivec2 screen_size;
	float blend_alpha; // Weight of the current frame.
	float depth_tolerance;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel, 0).r;
	if (center_depth == 0.0) {
		// Reverse-Z far plane: nothing was shaded here.
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_history_diffuse, pixel, vec4(0.0));
		imageStore(out_history_specular, pixel, vec4(0.0));
		return;
	}

	// Depth-aware spatial filter of the current frame, tracking the
	// neighborhood bounds used to rectify history.
	vec3 sum_diffuse = vec3(0.0);
	vec3 sum_specular = vec3(0.0);
	float total_weight = 0.0;
	vec3 min_diffuse = vec3(1e30);
	vec3 max_diffuse = vec3(-1e30);
	vec3 min_specular = vec3(1e30);
	vec3 max_specular = vec3(-1e30);

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 sp = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float sd = texelFetch(depth_texture, sp, 0).r;
			if (sd == 0.0) {
				continue;
			}
			vec3 d = texelFetch(raw_diffuse, sp, 0).rgb;
			vec3 s = texelFetch(raw_specular, sp, 0).rgb;

			float depth_diff = abs(sd - center_depth) / max(center_depth, 1e-6);
			if (depth_diff >= params.depth_tolerance) {
				continue;
			}
			float weight = exp(-0.3 * float(x * x + y * y));
			sum_diffuse += d * weight;
			sum_specular += s * weight;
			total_weight += weight;

			if (abs(x) <= 1 && abs(y) <= 1) {
				min_diffuse = min(min_diffuse, d);
				max_diffuse = max(max_diffuse, d);
				min_specular = min(min_specular, s);
				max_specular = max(max_specular, s);
			}
		}
	}

	vec3 current_diffuse = sum_diffuse / max(total_weight, 1e-6);
	vec3 current_specular = sum_specular / max(total_weight, 1e-6);
	if (total_weight <= 0.0) {
		current_diffuse = texelFetch(raw_diffuse, pixel, 0).rgb;
		current_specular = texelFetch(raw_specular, pixel, 0).rgb;
		min_diffuse = current_diffuse;
		max_diffuse = current_diffuse;
		min_specular = current_specular;
		max_specular = current_specular;
	}

	vec3 result_diffuse = current_diffuse;
	vec3 result_specular = current_specular;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, center_depth, 1.0);
	if (prev_ndc.w > 0.0) {
		vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
		if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			vec3 hist_d = textureLod(history_diffuse, prev_uv, 0.0).rgb;
			vec3 hist_s = textureLod(history_specular, prev_uv, 0.0).rgb;
			// Widen the bounds slightly so stable regions keep accumulating.
			vec3 slack_d = (max_diffuse - min_diffuse) * 0.25 + vec3(1e-4);
			vec3 slack_s = (max_specular - min_specular) * 0.25 + vec3(1e-4);
			hist_d = clamp(hist_d, min_diffuse - slack_d, max_diffuse + slack_d);
			hist_s = clamp(hist_s, min_specular - slack_s, max_specular + slack_s);
			result_diffuse = mix(hist_d, current_diffuse, params.blend_alpha);
			result_specular = mix(hist_s, current_specular, params.blend_alpha);
		}
	}

	imageStore(out_diffuse, pixel, vec4(result_diffuse, 1.0));
	imageStore(out_specular, pixel, vec4(result_specular, 1.0));
	imageStore(out_history_diffuse, pixel, vec4(result_diffuse, 1.0));
	imageStore(out_history_specular, pixel, vec4(result_specular, 1.0));
}
