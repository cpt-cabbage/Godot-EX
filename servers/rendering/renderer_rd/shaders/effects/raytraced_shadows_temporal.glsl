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
// A dummy binding when motion vectors are not rendered (FLAG_HAS_VELOCITY unset).
layout(set = 0, binding = 3) uniform sampler2D velocity_texture;
// The previous frame pair's reprojection, the camera-only motion the (one
// frame stale) velocity buffer was rendered with.
layout(set = 0, binding = 4, std140) uniform ReprojectUBO {
	mat4 prev_reproject;
}
reprojection;
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D dest_mask;
layout(set = 1, binding = 1, r8) uniform restrict writeonly image2D dest_history;

#define FLAG_HAS_VELOCITY 1u

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC.
	ivec2 screen_size;
	float blend_alpha;
	uint flags;
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
		// The camera reprojection is exact for static geometry, but a nonzero
		// velocity does not mean a moving object: Godot velocity buffers carry
		// camera motion for static geometry too, and this buffer is one frame
		// stale. Classify by comparing against the camera-only motion of the
		// frame pair the buffer was rendered with; only where the two disagree
		// is the pixel a moving object, and the stale velocity is then the
		// best predictor available of where its history lives.
		if ((params.flags & FLAG_HAS_VELOCITY) != 0u) {
			vec2 velocity = texelFetch(velocity_texture, pixel, 0).xy;
			vec4 prevprev_ndc = reprojection.prev_reproject * vec4(prev_ndc.xyz / prev_ndc.w, 1.0);
			if (velocity != vec2(0.0) && prevprev_ndc.w > 0.0) {
				vec2 static_motion = (prevprev_ndc.xy / prevprev_ndc.w) * 0.5 + 0.5 - prev_uv;
				// 2px: the velocity buffer is unjittered while the matrices
				// carry the TAA jitter of both frames.
				vec2 object_pixels = (velocity - static_motion) * vec2(params.screen_size);
				if (any(greaterThan(abs(object_pixels), vec2(2.0)))) {
					prev_uv = uv + velocity;
				}
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
