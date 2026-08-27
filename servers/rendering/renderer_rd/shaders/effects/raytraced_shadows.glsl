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
	vec4 to_sun; // xyz: direction toward the sun (world space), w: tan of the sun's angular half-size.
	ivec2 screen_size;
	float ray_bias;
	float max_distance;
}
params;

#define SOFT_SHADOW_SAMPLES 4u

bool trace_occluded(vec3 p_origin, vec3 p_dir) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
			0xFF, p_origin, params.ray_bias, p_dir, params.max_distance);
	rayQueryProceedEXT(rq);
	return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

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

	vec3 to_sun = normalize(params.to_sun.xyz);
	float tan_half_angle = params.to_sun.w;

	float visibility;
	if (tan_half_angle > 0.0001) {
		// Sample the sun's disk: concentric-ish disk samples rotated per pixel
		// with interleaved gradient noise.
		vec3 basis_u = normalize(cross(to_sun, abs(to_sun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 basis_v = cross(to_sun, basis_u);
		float noise = fract(52.9829189 * fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y)));

		uint hits = 0u;
		for (uint s = 0u; s < SOFT_SHADOW_SAMPLES; s++) {
			float angle = (float(s) + noise) * (6.2831853 / float(SOFT_SHADOW_SAMPLES));
			float radius = sqrt((float(s) + 0.5) / float(SOFT_SHADOW_SAMPLES));
			vec2 disk = vec2(cos(angle), sin(angle)) * radius;
			vec3 dir = normalize(to_sun + (basis_u * disk.x + basis_v * disk.y) * tan_half_angle);
			if (trace_occluded(world.xyz, dir)) {
				hits++;
			}
		}
		visibility = 1.0 - float(hits) / float(SOFT_SHADOW_SAMPLES);
	} else {
		visibility = trace_occluded(world.xyz, to_sun) ? 0.0 : 1.0;
	}

	imageStore(shadow_mask, pixel, vec4(visibility));
}
