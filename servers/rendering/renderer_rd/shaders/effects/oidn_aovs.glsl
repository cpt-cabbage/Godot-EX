#[compute]

#version 450

#VERSION_DEFINES

// The auxiliary images a learned denoiser takes next to the noisy colour
// (Open Image Denoise's albedo and normal; the depth, specular albedo,
// roughness and motion its temporal / supersampling form is expected to
// add), unpacked from the prepass G-buffer at the internal resolution,
// together with a copy of the linear colour the upscaler is about to get.
// A harness dump only (AOV=oidn): nothing reads these on the GPU yet.
//
//   color:       the internal colour, linear, before the upscaler and the tonemapper.
//   depth:       linear view depth, clamped to [near, far]; far where no surface.
//   normal:      world-space normal (zero where no surface), a = roughness.
//   albedo:      diffuse albedo (albedo * (1 - metallic)), a = 0 no surface,
//                1 lit surface, 2 unshaded surface.
//   spec_albedo: the directional albedo of the specular lobe as the scene
//                shader folds it into its reflection (F0, f90 and the DFG
//                term, with the multiscatter compensation), a = metallic.
//   motion:      to the previous frame, in pixels, without the camera jitter
//                (the jitter goes in the dump's sidecar).

#include "../albedo_f0_inc.glsl"
#include "../normal_roughness_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 2) uniform sampler2D gbuf_albedo_texture;
layout(set = 0, binding = 3) uniform sampler2D gbuf_f0_texture;
layout(set = 0, binding = 4) uniform sampler2D velocity_texture;
layout(set = 0, binding = 5) uniform sampler2D color_texture;
layout(set = 0, binding = 6) uniform sampler2D dfg_lut;
layout(set = 0, binding = 7, std140) uniform Params {
	mat4 view_from_ndc;
	mat4 world_from_view;
	// The camera-only reprojection with both frames' jitter in it, the form
	// the RT passes reproject by; jitter_delta takes it back out.
	mat4 prev_ndc_from_ndc;
	vec2 jitter_delta; // Half the previous frame's TAA jitter minus this frame's, NDC.
	ivec2 screen_size;
	float z_near;
	float z_far;
	uint flags;
	uint pad;
}
params;

layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D out_color;
layout(r32f, set = 1, binding = 1) uniform restrict writeonly image2D out_depth;
layout(rgba16f, set = 1, binding = 2) uniform restrict writeonly image2D out_normal;
layout(rgba16f, set = 1, binding = 3) uniform restrict writeonly image2D out_albedo;
layout(rgba16f, set = 1, binding = 4) uniform restrict writeonly image2D out_spec_albedo;
layout(rg16f, set = 1, binding = 5) uniform restrict writeonly image2D out_motion;

// The frame has a velocity buffer written this frame (the motion-vector
// prepass, or the colour pass's when an upscaler or TAA asked for it):
// unjittered, uv units, previous minus current. Without one, every pixel
// takes the camera's motion alone.
#define FLAG_HAS_VELOCITY 1u

vec3 view_position(vec2 p_uv, float p_depth) {
	vec4 v = params.view_from_ndc * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return v.xyz / v.w;
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.screen_size))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.screen_size);
	float depth = texelFetch(depth_texture, pos, 0).r;
	vec4 nr = texelFetch(normal_roughness_texture, pos, 0);
	vec4 gb_a = texelFetch(gbuf_albedo_texture, pos, 0);
	vec4 gb_f = texelFetch(gbuf_f0_texture, pos, 0);
	// Reverse Z: the cleared depth is 0 where the prepass drew nothing (sky).
	bool surface = depth > 0.0 && nr_valid(nr);

	imageStore(out_color, pos, vec4(texelFetch(color_texture, pos, 0).rgb, 1.0));

	vec3 p = view_position(uv, depth);
	float linear_depth = surface ? clamp(-p.z, params.z_near, params.z_far) : params.z_far;
	imageStore(out_depth, pos, vec4(linear_depth));

	float roughness = surface ? nr_roughness(nr) : 0.0;
	vec3 n_view = surface ? nr_normal(nr) : vec3(0.0);
	vec3 n_world = surface ? normalize(mat3(params.world_from_view) * n_view) : vec3(0.0);
	imageStore(out_normal, pos, vec4(n_world, roughness));

	imageStore(out_albedo, pos, vec4(surface ? gb_albedo(gb_a) : vec3(0.0), surface ? (gb_unshaded(gb_a) ? 2.0 : 1.0) : 0.0));

	vec3 spec = vec3(0.0);
	float metallic = 0.0;
	if (surface) {
		// Toward the eye from the pixel's point: from the near plane's point on
		// the same pixel, which is the camera for a perspective projection and
		// the view axis for an orthogonal one.
		vec3 v = normalize(view_position(uv, 1.0) - p);
		float n_dot_v = clamp(dot(n_view, v), 0.0001, 1.0);
		vec2 dfg = textureLod(dfg_lut, vec2(n_dot_v, 1.0 - roughness), 0.0).rg;
		vec3 f0 = gb_f0(gb_f);
		metallic = gb_metallic(gb_f);
		// scene_forward_clustered.glsl's indirect specular, term for term.
		float f90 = clamp(50.0 * f0.g, metallic, 1.0);
		vec3 energy_compensation = 1.0 + f0 * (1.0 / max(dfg.y, 1e-4) - 1.0);
		spec = energy_compensation * ((f90 - f0) * dfg.x + f0 * dfg.y);
	}
	imageStore(out_spec_albedo, pos, vec4(spec, metallic));

	vec2 motion_uv;
	if (surface && (params.flags & FLAG_HAS_VELOCITY) != 0u) {
		motion_uv = texelFetch(velocity_texture, pos, 0).xy;
	} else {
		// The camera's motion at the pixel's depth (the far plane for the
		// sky), the jitter of both frames taken out.
		vec4 prev_ndc = params.prev_ndc_from_ndc * vec4(uv * 2.0 - 1.0, surface ? depth : 1e-7, 1.0);
		motion_uv = prev_ndc.w > 0.0 ? (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5 - uv - params.jitter_delta : vec2(0.0);
	}
	imageStore(out_motion, pos, vec4(motion_uv * vec2(params.screen_size), 0.0, 0.0));
}
