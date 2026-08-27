#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D shadow_mask;

layout(push_constant, std430) uniform Params {
	mat4 inv_view_proj; // NDC -> world.
	vec4 to_sun; // xyz: direction toward the sun, world space.
	ivec2 screen_size;
	float ray_bias;
	float max_distance;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float depth = texelFetch(depth_texture, pixel, 0).r;
	if (depth == 0.0) {
		// Reverse-Z far plane (sky): fully lit.
		imageStore(shadow_mask, pixel, vec4(1.0));
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 world = params.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	world.xyz /= world.w;

	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
			0xFF, world.xyz, params.ray_bias, normalize(params.to_sun.xyz), params.max_distance);
	rayQueryProceedEXT(rq);

	bool occluded = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
	imageStore(shadow_mask, pixel, vec4(occluded ? 0.0 : 1.0));
}
