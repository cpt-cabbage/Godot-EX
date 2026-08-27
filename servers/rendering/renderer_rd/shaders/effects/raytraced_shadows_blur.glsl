#[compute]

#version 450

#VERSION_DEFINES

// Depth-aware 5x5 blur for the ray-traced shadow visibility mask, to smooth
// the dither noise from stochastic soft-shadow sampling.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_mask;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D dest_mask;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	float depth_tolerance; // Relative depth difference where a sample stops contributing.
	float pad;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel, 0).r;
	if (center_depth == 0.0) {
		imageStore(dest_mask, pixel, vec4(1.0));
		return;
	}

	float total = 0.0;
	float total_weight = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 sample_pixel = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			float sample_depth = texelFetch(depth_texture, sample_pixel, 0).r;
			// Gaussian-ish spatial falloff times a relative depth similarity weight.
			float spatial = exp(-0.3 * float(x * x + y * y));
			float depth_diff = abs(sample_depth - center_depth) / max(center_depth, 1e-6);
			float similarity = depth_diff < params.depth_tolerance ? 1.0 : 0.0;
			float weight = spatial * similarity;
			total += texelFetch(source_mask, sample_pixel, 0).r * weight;
			total_weight += weight;
		}
	}

	imageStore(dest_mask, pixel, vec4(total / max(total_weight, 1e-6)));
}
