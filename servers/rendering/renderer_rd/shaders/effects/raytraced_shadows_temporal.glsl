#[compute]

#version 450

#VERSION_DEFINES

// Temporal accumulation for the ray-traced shadow mask: reprojects last
// frame's accumulated mask and blends it with the current frame, using a
// 3x3 neighborhood clamp to reject stale history.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D current_mask;
layout(set = 0, binding = 1) uniform sampler2D history_mask;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
// Motion vectors from the previous frame's color pass (uv_prev = uv + velocity).
// Bound to a default black texture when motion vectors are not rendered.
layout(set = 0, binding = 3) uniform sampler2D velocity_texture;
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D dest_mask;
layout(set = 1, binding = 1, r8) uniform restrict writeonly image2D dest_history;

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC.
	ivec2 screen_size;
	float blend_alpha;
	float pad;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float depth = texelFetch(depth_texture, pixel, 0).r;
	float current = texelFetch(current_mask, pixel, 0).r;

	if (depth == 0.0) {
		imageStore(dest_mask, pixel, vec4(1.0));
		imageStore(dest_history, pixel, vec4(1.0));
		return;
	}

	// Neighborhood bounds of the current frame, used to clamp history.
	float mn = current;
	float mx = current;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 sample_pixel = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float v = texelFetch(current_mask, sample_pixel, 0).r;
			mn = min(mn, v);
			mx = max(mx, v);
		}
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, depth, 1.0);
	float result = current;
	if (prev_ndc.w > 0.0) {
		vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
		// The camera reprojection is exact for static geometry; where the
		// velocity buffer (one frame stale, written by the previous color pass)
		// disagrees by more than a pixel, the pixel belongs to a moving object
		// and its velocity is the better predictor of where the history lives.
		vec2 velocity = texelFetch(velocity_texture, pixel, 0).xy;
		if (velocity != vec2(0.0)) {
			vec2 residual = (uv + velocity) - prev_uv;
			if (any(greaterThan(abs(residual) * vec2(params.screen_size), vec2(1.0)))) {
				prev_uv = uv + velocity;
			}
		}
		if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			float history = textureLod(history_mask, prev_uv, 0.0).r;
			// Widen the clamp window slightly to reduce flicker in stable regions.
			history = clamp(history, mn - 0.05, mx + 0.05);
			result = mix(history, current, params.blend_alpha);
		}
	}

	imageStore(dest_mask, pixel, vec4(result));
	imageStore(dest_history, pixel, vec4(result));
}
