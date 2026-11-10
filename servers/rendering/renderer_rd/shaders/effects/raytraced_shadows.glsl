#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
#ifndef MODE_AREA
// The sun's light through something the TLAS does not hold, set by a module (Raytracing::set_sun_shadow_modulator):
// the weather module's clouds, their transmittance on a plane (`frame.w`, world y) that a point reaches along the
// light, over a square (`frame.xy` its corner in world x, z, `frame.z` its side); sun_modulator_inc.glsl reads it.
layout(set = 0, binding = 2) uniform sampler2D sun_modulator;
layout(set = 0, binding = 3, std140) uniform SunModulator {
	vec4 frame;
	vec4 flags; // x 1 when set, y 1 with a canopy map
	vec4 canopy_frame; // see canopy_visibility
	vec4 canopy_params;
	vec4 ground_frame;
	vec4 ground_params;
}
sun_mod;
layout(set = 0, binding = 4) uniform sampler2D sun_canopy;
layout(set = 0, binding = 5) uniform sampler2D sun_canopy_ground;

#include "sun_modulator_inc.glsl"

// The sun's light under the crowns of trees that cast no shadow of their own (far ones, drawn as impostors; Godot-EX,
// 2026-11-10): a height field's shadow from a map of the crowns (`canopy`: R their tops above the ground, times cp.x m;
// G how far from the camera that tree's last shadow-casting level ends, times cp.y m; frames are a corner in world x, z
// and one over the sides) and the ground's heights (gp.x + gp.y * R). A point `p` is shaded where the ray to the sun
// passes under a crown before it clears the tallest (gp.z m), and a crown counts only once its own tree is past that
// distance from the camera: nearer, the tree casts its real shadow, and a second one from the map put every tree still
// drawn as a mesh in deeper shade the further off it stood (a Protea's crown 1.3 times brighter 15 m nearer). No
// pixel nearer than cp.z less the ray's reach is touched; gp.w is how much of the sun a crown takes. Eight taps
// along the ray, the ground between its ends taken as a plane; nothing where a coarse read finds no crown along it.
float canopy_visibility(sampler2D canopy, sampler2D ground, vec3 p, vec3 to_sun, vec3 cam, vec4 cf, vec4 cp, vec4 gf, vec4 gp) {
	float dist = distance(p, cam);
	if (to_sun.y < 0.03) {
		return 1.0;
	}
	float g0 = gp.x + gp.y * textureLod(ground, (p.xz - gf.xy) * gf.zw, 0.0).r;
	float above = max(p.y - g0, 0.0);
	if (above >= gp.z) {
		return 1.0;
	}
	float reach = min((gp.z - above) / to_sun.y, 160.0);
	float across = reach * length(to_sun.xz);
	if (dist + across < cp.z) {
		return 1.0;
	}
	vec2 mid = (p.xz + to_sun.xz * reach * 0.5 - cf.xy) * cf.zw;
	if (any(lessThan(mid, vec2(0.0))) || any(greaterThan(mid, vec2(1.0)))) {
		return 1.0;
	}
	if (textureLod(canopy, mid, log2(max(across, cp.w) / cp.w) + 2.0).r <= 0.0) {
		return 1.0;
	}
	vec3 end = p + to_sun * reach;
	float g1 = gp.x + gp.y * textureLod(ground, (end.xz - gf.xy) * gf.zw, 0.0).r;
	// The taps crowd toward the point (the square of an even spacing), each read as coarsely as the gap to the next: a
	// small tree's shadow is short and needs the map's own texels, a tall one's long and its crown wide. Evenly spaced
	// over the tallest tree's reach, a 6 m Protea alone cast a third of the shadow its meshes did.
	// A point up in a crown starts beyond it (0.7 of its height above the ground along the ray): the map's dome is
	// no measure of which leaves of its own crown shade it, and the taps at its foot put every far crown in shade
	// (leaves at 0.16 of the grass's luminance against 0.34 for the same tree's meshes).
	float near = min(0.7 * above, reach);
	float occ = 0.0;
	for (int i = 0; i < 8; i++) {
		float e = (float(i) + 0.5) / 8.0;
		float f = e * e;
		vec3 q = p + to_sun * mix(near, reach, f);
		float lod = log2(max(max(dist * 0.0015, across * e / 6.0) / cp.w, 1.0));
		vec2 c = textureLod(canopy, (q.xz - cf.xy) * cf.zw, lod).rg;
		float h = c.r * cp.x;
		// Past its last shadow-casting level, over the stretch that level's crossfade and the engine's slack take.
		float own = c.g * cp.y;
		float far = smoothstep(own, own * 1.15, distance(q, cam));
		occ = max(occ, far * smoothstep(0.5, 2.5, h) * smoothstep(0.0, 2.0, mix(g0, g1, f) + h - q.y));
	}
	return 1.0 - occ * gp.w;
}
#endif
layout(set = 1, binding = 0, r8) uniform restrict writeonly image2D shadow_mask;

// Alpha-tested casters (rt_alpha_inc.glsl): the records, the hit shading's pools and the alpha masks, in set 2.
#define RT_ALPHA_SET 2
#include "rt_alpha_inc.glsl"

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
	uint caster_mask_and_rays; // Bits 0..7 caster mask, 8..15 soft shadow rays, 16 alpha-tested casters in the TLAS.
}
params;

#define SOFT_SHADOW_SAMPLES max((params.caster_mask_and_rays >> 8) & 0xFFu, 1u)
#define ALPHA_CASTERS ((params.caster_mask_and_rays & (1u << 16u)) != 0u)
// Past this many leaves let through, a ray is deep in a crown: the next one stops it.
#define ALPHA_MAX_PASSES 16u

bool trace_occluded(vec3 p_origin, vec3 p_dir, float p_max_dist) {
	uint caster_mask = params.caster_mask_and_rays & 0xFFu;
	if (caster_mask == 0u) {
		return false; // The light casts no shadows from any object.
	}
	// Back faces culled: the shadow-map convention, where only the faces a
	// material draws occlude (see trace_visible in the stochastic pass).
	if (!ALPHA_CASTERS) {
		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, tlas,
				gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
				caster_mask, p_origin, params.axis_u.w, p_dir, p_max_dist);
		rayQueryProceedEXT(rq);
		return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
	}
	// Alpha-tested casters (non-opaque TLAS instances) hand back their hits as candidates: confirmed where the
	// material's alpha mask covers the hit's UV, or where no mask is known (seen whole, as before).
	rayQueryEXT rqa;
	rayQueryInitializeEXT(rqa, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
			caster_mask, p_origin, params.axis_u.w, p_dir, p_max_dist);
	uint passed = 0u;
	while (rayQueryProceedEXT(rqa)) {
		if (rayQueryGetIntersectionTypeEXT(rqa, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			int covers = passed >= ALPHA_MAX_PASSES ? 1 : RT_ALPHA_CANDIDATE(rqa);
			if (covers != 0) {
				rayQueryConfirmIntersectionEXT(rqa);
			} else {
				passed++;
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(rqa, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
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
	// Sample the light's rectangle along the R2 sequence (Roberts' additive
	// recurrence, the 2D golden ratio), rotated per pixel and frame by the
	// noise: well spread for any sample count, where the 2x2 grid it
	// replaced wrapped samples past the fourth onto the same four cells, so
	// rays_per_pixel above 4 added samples but no finer stratification.
	uint hits = 0u;
	for (uint s = 0u; s < SOFT_SHADOW_SAMPLES; s++) {
		vec2 jitter = fract(vec2(noise, noise * 1.6180339887) + float(s) * vec2(0.7548776662, 0.5698402910));
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
	if (sun_mod.flags.x > 0.5) {
		visibility *= sun_modulator_visibility(sun_modulator, world.xyz, to_sun, sun_mod.frame, 1e9);
	}
	if (sun_mod.flags.y > 0.5 && visibility > 0.0) {
		vec4 cam = params.inv_view_proj * vec4(0.0, 0.0, 1.0, 1.0); // the near plane's middle: the camera, within centimetres
		visibility *= canopy_visibility(sun_canopy, sun_canopy_ground, world.xyz, to_sun, cam.xyz / cam.w, sun_mod.canopy_frame, sun_mod.canopy_params, sun_mod.ground_frame, sun_mod.ground_params);
	}
#endif

	imageStore(shadow_mask, pixel, vec4(visibility));
}
