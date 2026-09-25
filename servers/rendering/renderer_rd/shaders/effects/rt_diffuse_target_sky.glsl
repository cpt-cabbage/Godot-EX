#[compute]

#version 460

#VERSION_DEFINES

// The ray-traced GI's diffuse target where the opaque pass wrote it itself
// (render_forward_clustered.cpp rt_diffuse_target_mrt): the sky is drawn
// after that pass, into the colour alone, so its pixels -- no depth --
// take the colour here. The gather's screen reads never land on the sky
// (they need a surface), but their bilinear taps at a silhouette do.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D color_texture;
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D diffuse_target;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.size.x || pixel.y >= params.size.y) {
		return;
	}
	if (texelFetch(depth_texture, pixel, 0).r != 0.0) {
		return;
	}
	imageStore(diffuse_target, pixel, texelFetch(color_texture, pixel, 0));
}
