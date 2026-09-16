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
#ifdef MODE_AREA
	vec4 light_pos; // xyz: area light center (world space).
	vec4 axis_u; // xyz: full extent along the rect's U axis, w: ray bias.
	vec4 axis_v; // xyz: full extent along the rect's V axis, w: max distance.
#else
	vec4 light_pos; // xyz: direction toward the sun (world space), w: tan of the sun's angular half-size.
	vec4 axis_u; // w: ray bias.
	vec4 axis_v; // w: max distance.
#endif
	ivec2 screen_size;
	uint frame_index; // Varies the sampling pattern for temporal accumulation.
	uint caster_mask_and_rays; // Bits 0..7 caster mask, 8..15 soft shadow rays.
}
params;

#define SOFT_SHADOW_SAMPLES max((params.caster_mask_and_rays >> 8) & 0xFFu, 1u)

bool trace_occluded(vec3 p_origin, vec3 p_dir, float p_max_dist) {
	uint caster_mask = params.caster_mask_and_rays & 0xFFu;
	if (caster_mask == 0u) {
		return false; // The light casts no shadows from any object.
	}
	// Back faces culled: the shadow-map convention, where only the faces a
	// material draws occlude (see trace_visible in the stochastic pass).
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
			caster_mask, p_origin, params.axis_u.w, p_dir, p_max_dist);
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

	float noise = fract(52.9829189 * fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y)));
	// Advance the pattern each frame with the golden ratio for temporal accumulation.
	noise = fract(noise + float(params.frame_index % 64u) * 0.61803398875);

	float visibility;

#ifdef MODE_AREA
	// Sample the light's rectangle with jittered points, stratified over a
	// 2x2 grid: samples past the fourth wrap onto the same four cells (fract),
	// so rays_per_pixel above 4 adds samples but no finer stratification.
	uint hits = 0u;
	for (uint s = 0u; s < SOFT_SHADOW_SAMPLES; s++) {
		vec2 strat = vec2(float(s % 2u), float(s / 2u)) * 0.5;
		vec2 jitter = fract(vec2(noise, noise * 1.6180339887) + strat + vec2(0.25));
		vec3 target = params.light_pos.xyz + params.axis_u.xyz * (jitter.x - 0.5) + params.axis_v.xyz * (jitter.y - 0.5);
		vec3 delta = target - world.xyz;
		float dist = length(delta);
		if (dist < 1e-4) {
			continue;
		}
		if (trace_occluded(world.xyz, delta / dist, min(dist - params.axis_u.w, params.axis_v.w))) {
			hits++;
		}
	}
	visibility = 1.0 - float(hits) / float(SOFT_SHADOW_SAMPLES);
#else
	vec3 to_sun = normalize(params.light_pos.xyz);
	float tan_half_angle = params.light_pos.w;

	if (tan_half_angle > 0.0001) {
		// Sample the sun's disk: concentric-ish disk samples rotated per pixel
		// with interleaved gradient noise.
		vec3 basis_u = normalize(cross(to_sun, abs(to_sun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 basis_v = cross(to_sun, basis_u);

		uint hits = 0u;
		for (uint s = 0u; s < SOFT_SHADOW_SAMPLES; s++) {
			float angle = (float(s) + noise) * (6.2831853 / float(SOFT_SHADOW_SAMPLES));
			float radius = sqrt((float(s) + 0.5) / float(SOFT_SHADOW_SAMPLES));
			vec2 disk = vec2(cos(angle), sin(angle)) * radius;
			vec3 dir = normalize(to_sun + (basis_u * disk.x + basis_v * disk.y) * tan_half_angle);
			if (trace_occluded(world.xyz, dir, params.axis_v.w)) {
				hits++;
			}
		}
		visibility = 1.0 - float(hits) / float(SOFT_SHADOW_SAMPLES);
	} else {
		visibility = trace_occluded(world.xyz, to_sun, params.axis_v.w) ? 0.0 : 1.0;
	}
#endif

	imageStore(shadow_mask, pixel, vec4(visibility));
}
