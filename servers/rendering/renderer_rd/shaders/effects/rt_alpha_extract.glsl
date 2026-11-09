#[compute]

#version 450

#VERSION_DEFINES

// One alpha-tested material's coverage into a layer of the ray tracer's alpha masks (raytracing_scene.cpp,
// _alpha_layer): the alpha of its albedo texture, read at the mip nearest the mask's size. Shadow rays test their
// candidate hits against it (rt_alpha_inc.glsl).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, r8) uniform restrict writeonly image2DArray masks;

layout(push_constant, std430) uniform Params {
	uint layer;
	uint size;
	float lod;
	uint pad;
}
params;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, ivec2(params.size)))) {
		return;
	}
	vec2 uv = (vec2(p) + 0.5) / float(params.size);
	imageStore(masks, ivec3(p, int(params.layer)), vec4(textureLod(source, uv, params.lod).a));
}
