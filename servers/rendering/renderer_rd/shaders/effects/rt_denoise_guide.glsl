#[compute]

#version 460

#VERSION_DEFINES

#include "rt_sample_offset_inc.glsl"
// The denoisers' guide at the signal's resolution (plan section 83): the
// depth and the normal / roughness the spatial passes' edge stops read,
// point-sampled from the full-resolution G-buffer once, so that the
// twenty-five taps of every pixel read texels packed like the signal
// instead of a stride of the full-resolution textures. What the
// denoiser sees is exactly what it read before at pixel * depth_scale
// (at the block's center pixel from the quarter tier up, rt_sample_offset_inc.glsl).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_texture;

layout(set = 1, binding = 0, r32f) uniform restrict writeonly image2D out_depth;
layout(set = 1, binding = 1, rgb10_a2) uniform restrict writeonly image2D out_normal_roughness;

layout(push_constant, std430) uniform Params {
	ivec2 size; // The guide's.
	ivec2 full_size;
	int scale;
	int pad[3];
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.size.x || pixel.y >= params.size.y) {
		return;
	}
	ivec2 full_pixel = rt_full_pixel(pixel, params.scale, params.full_size);
	imageStore(out_depth, pixel, vec4(texelFetch(depth_texture, full_pixel, 0).r));
	imageStore(out_normal_roughness, pixel, texelFetch(normal_roughness_texture, full_pixel, 0));
}
