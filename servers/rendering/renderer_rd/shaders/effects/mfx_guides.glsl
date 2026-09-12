#[compute]

#version 450

#VERSION_DEFINES

// The MetalFX temporal denoised scaler's guide textures, unpacked from the
// prepass buffers: the normal as a plain vector (world space unless
// FLAG_VIEW_NORMAL), the roughness alone, the specular ray's hit distance
// from the GI gather's spec_ray (w; a miss stays far), and the strength
// mask marking the pixels the denoiser is to leave alone (sky, unshaded).

#include "../normal_roughness_inc.glsl"
#include "../albedo_f0_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 2) uniform sampler2D gbuf_albedo_texture;
layout(set = 0, binding = 3) uniform sampler2D spec_ray_texture;
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D out_normal;
layout(r16f, set = 1, binding = 1) uniform restrict writeonly image2D out_roughness;
layout(r16f, set = 1, binding = 2) uniform restrict writeonly image2D out_hit_distance;
layout(r8, set = 1, binding = 3) uniform restrict writeonly image2D out_strength_mask;

#define FLAG_VIEW_NORMAL 1u
#define FLAG_HAS_SPEC_RAY 2u

layout(push_constant, std430) uniform Params {
	mat4 world_from_view;
	ivec2 screen_size;
	uint flags;
	float miss_distance;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.screen_size))) {
		return;
	}
	float depth = texelFetch(depth_texture, pos, 0).r;
	vec4 nr = texelFetch(normal_roughness_texture, pos, 0);
	vec4 gb_a = texelFetch(gbuf_albedo_texture, pos, 0);
	bool sky = depth == 0.0 || !nr_valid(nr);
	vec3 n = sky ? vec3(0.0, 0.0, 1.0) : nr_normal(nr);
	if ((params.flags & FLAG_VIEW_NORMAL) == 0u) {
		n = normalize(mat3(params.world_from_view) * n);
	}
	float hit = params.miss_distance;
	if ((params.flags & FLAG_HAS_SPEC_RAY) != 0u) {
		float t = texelFetch(spec_ray_texture, pos, 0).w;
		hit = t > 0.0 ? min(t, params.miss_distance) : params.miss_distance;
	}
	imageStore(out_normal, pos, vec4(n, 0.0));
	imageStore(out_roughness, pos, vec4(sky ? 1.0 : nr_roughness(nr)));
	imageStore(out_hit_distance, pos, vec4(hit));
	imageStore(out_strength_mask, pos, vec4((sky || gb_unshaded(gb_a)) ? 1.0 : 0.0));
}
