#[compute]

#version 460

#VERSION_DEFINES

#include "../normal_roughness_inc.glsl"
#include "rt_sample_offset_inc.glsl"
// The denoisers' guide at the signal's resolution (plan section 83): the
// depth and the normal / roughness the spatial passes' edge stops read,
// point-sampled from the full-resolution G-buffer once, so that the
// twenty-five taps of every pixel read texels packed like the signal
// instead of a stride of the full-resolution textures. What the
// denoiser sees is exactly what it read before at pixel * depth_scale
// (at the block's center pixel from the quarter tier up, rt_sample_offset_inc.glsl).
//
// SPEC_AA: the GI reflection's lobe per texel (GODOT_GI_SPEC_AA). A
// low-resolution texel's reflection ray left along the normal of one pixel
// of its block, and the upsample spreads the texel over every pixel within
// a texel of that one (a tent, 3x3 pixels at the half tier, 7x7 at the
// quarter): a reflected highlight narrower than a texel was in that pixel or
// missed it, and drew a texel-sized square. On the TPS demo's chevron panels
// these were the orange lamps in the plates, which are roughness 0 with a
// gentle normal map (the bumps turn the normal 1 degree across a texel's
// tent, median): a mirror ray each, a lamp or not, the same in every run
// and under every filter (rt_lab r5, r6) -- a stable 3.4 px block at 4K
// under FSR2, gone only with the GI at full resolution (+11.6 ms).
//
// So the lobe is the tent's: the mean of its normals on the sample pixel's
// surface (the depth within depth_tolerance, the normal within 45 degrees,
// so a crease or the object behind does not join, weighted as the bilinear
// weighs the texel), and the roughness widened by their spread, Toksvig's
// variance from the mean normal's length L: normals of per-axis slope
// variance s have L = 1 - s, GGX's alpha^2 is twice the slope variance, so
// the footprint's lobe has alpha^2 + 2 (1 - L) / L (Tokuyoshi and Kaplanyan
// 2019's addition, capped like theirs at 0.18), the variance scaled by
// spec_variance (2). A mirror is widened the same way: a bumpy one becomes
// a narrow lobe the gather samples (its ray is otherwise a mirror ray), a
// flat footprint (L above 0.99999, a lobe under half a degree) keeps the
// pixel's own texel and its exact mirror. The diffuse rays, the spatial
// passes' stops and the upsample keep the sample pixel's normal.
//
// Measured on the bridge walk (4K, FSR2 0.588, GI high; the panel's pixels
// standing 0.08 over their 9x9 mean, orange, at frames 106-110): their
// energy 202 -> 39 at spec_variance 2 (82 at 1, 28 at 4), the panel's mean
// level and the road's fine detail unchanged. Not kept: the 2x2 block as
// the footprint (112 at 1: half its pixels weigh half in the tent, and the
// pixels before the sample pixel were missing), and the mirrors kept on
// their own texel (-3%: the glints are mirrors).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_texture;

#ifdef SPEC_AA
// Encoded like the normal-roughness buffer but for the alpha: 1 where the
// texel carries its footprint's lobe, 0 where it is the sample pixel's own
// texel (a flat footprint).
layout(set = 1, binding = 0, rgb10_a2) uniform restrict writeonly image2D out_spec_normal_roughness;
#else
layout(set = 1, binding = 0, r32f) uniform restrict writeonly image2D out_depth;
layout(set = 1, binding = 1, rgb10_a2) uniform restrict writeonly image2D out_normal_roughness;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 size; // The guide's.
	ivec2 full_size;
	int scale;
	float spec_variance; // SPEC_AA: the scale on the normals' variance (GODOT_GI_SPEC_AA).
	float depth_tolerance; // SPEC_AA: the tent's pixels on the sample pixel's surface, as a share of its depth.
	int pad;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.size.x || pixel.y >= params.size.y) {
		return;
	}
	ivec2 full_pixel = rt_full_pixel(pixel, params.scale, params.full_size);
#ifdef SPEC_AA
	vec4 nr = texelFetch(normal_roughness_texture, full_pixel, 0);
	float depth = texelFetch(depth_texture, full_pixel, 0).r;
	if (depth == 0.0) {
		imageStore(out_spec_normal_roughness, pixel, vec4(nr.xyz, 0.0));
		return;
	}
	vec3 n = nr_normal(nr);
	int scale = rt_scale(params.scale);
	vec3 sum = vec3(0.0);
	float weight = 0.0;
	for (int y = 1 - scale; y < scale; y++) {
		for (int x = 1 - scale; x < scale; x++) {
			ivec2 p = clamp(full_pixel + ivec2(x, y), ivec2(0), params.full_size - 1);
			// The raw depth's relative difference: reversed Z goes as the
			// inverse of the view depth, so this is the view depth's.
			float d = texelFetch(depth_texture, p, 0).r;
			if (d == 0.0 || abs(d - depth) > params.depth_tolerance * depth) {
				continue;
			}
			vec3 n_i = nr_normal(texelFetch(normal_roughness_texture, p, 0));
			if (dot(n_i, n) < 0.7) {
				continue;
			}
			vec2 t = 1.0 - abs(vec2(x, y)) / float(scale);
			sum += n_i * (t.x * t.y);
			weight += t.x * t.y;
		}
	}
	float len = length(sum) / weight; // The sample pixel itself is in, at weight 1.
	if (weight <= 1.0 || len >= 0.99999) {
		imageStore(out_spec_normal_roughness, pixel, vec4(nr.xyz, 0.0));
		return;
	}
	float alpha = nr_roughness(nr) * nr_roughness(nr);
	float a2 = min(alpha * alpha + min(2.0 * params.spec_variance * (1.0 - len) / len, 0.18), 1.0);
	imageStore(out_spec_normal_roughness, pixel, vec4(nr_encode(sum / (len * weight), sqrt(sqrt(a2)), false).xyz, 1.0));
#else
	imageStore(out_depth, pixel, vec4(texelFetch(depth_texture, full_pixel, 0).r));
	imageStore(out_normal_roughness, pixel, texelFetch(normal_roughness_texture, full_pixel, 0));
#endif
}
