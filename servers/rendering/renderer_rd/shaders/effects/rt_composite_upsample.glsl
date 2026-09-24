#[compute]

#version 450

#VERSION_DEFINES

// The opaque pass's two RT composites, upsampled at full resolution once
// (plan section 98). At half or quarter resolution the scene shader used
// to take four taps of each signal per fragment, weighted by each tap's
// depth and normal against the fragment's -- up to thirty-six texture
// instructions in a fragment shader that is latency-bound on them (about
// 0.14 ms per instruction per composite at 1080p, section 84), with a
// fast path for the fragments whose four taps sit on their surface. This
// kernel runs the same loop for every pixel of the prepass, against the
// prepass's own depth and normal (the opaque pass's fragment is that
// pixel), and hands the scene shader one texel per signal. The weights
// are the loop's, with the fast path's planar depth test (tap_weights).
//
// The opaque pass is bandwidth- as well as latency-bound, and so is this
// kernel (an M4 moves ~100 GB/s): the first form wrote 32 bytes a pixel
// (the ratios, the three GI signals and the gather's basis normal) for
// 1.3-1.7 ms. So the GI's re-basing onto the pixel's normal runs here too
// (the prepass normal is the fragment's, bar a bent normal map, whose
// materials keep the fragment's loop), and what the fragment still needs
// fits one RGBA32UI texel (stochastic_direct_lights bit 7, rt_gi bit 10)
// and a small one beside it:
// - the ambient, re-based, and the reflection, as six halves (x, y, z);
//   a shared exponent (RGB9E5, 16 bytes with everything below) was tried
//   and left the non-dominant channels a few bits: +12-15 % high
//   frequency in green and blue on the lab's red-lit mirror floor;
// - w: the diffuse and specular ratios as halves (each a luminance
//   visibility fraction the denoiser carries replicated over rgb; red is
//   the channel with the longest mantissa in the packed diffuse buffer);
// - an R16F: the near-field visibility where the specular occlusion may
//   use it, negative where it may not.
// With image lights (USE_IMAGES) their diffuse term and Schlick weight,
// and their specular lobe, in two RGBA16F textures beside those.

#include "../normal_roughness_inc.glsl"
#include "rt_sample_offset_inc.glsl"

// 16x16 pixels a group. Its taps are a block of the sampling grid at most
// 16 / scale + 2 texels wide, loaded once into shared memory (the first
// form fetched every tap per pixel, 38 fetches a pixel for 1.7 ms at
// 1080p): each grid texel serves up to 16 pixels at the quarter tiers.
#define TILE 16
#define GRID 10 // (TILE / 2 + 2): the half tier's block; the quarter's is 6.
layout(local_size_x = TILE, local_size_y = TILE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 2) uniform sampler2D stochastic_diffuse;
layout(set = 0, binding = 3) uniform sampler2D stochastic_specular;
layout(set = 0, binding = 4) uniform sampler2D stochastic_image_diffuse;
layout(set = 0, binding = 5) uniform sampler2D stochastic_image_specular;
layout(set = 0, binding = 6) uniform sampler2D stochastic_depth;
// The denoisers' guide normal at the sampling grid's size, or the
// full-resolution normal buffer read at a stride of the scale (flag).
layout(set = 0, binding = 7) uniform sampler2D stochastic_normal;
layout(set = 0, binding = 8) uniform sampler2D gi_ambient;
layout(set = 0, binding = 9) uniform sampler2D gi_reflection;
layout(set = 0, binding = 10) uniform sampler2D gi_directional;
layout(set = 0, binding = 11) uniform sampler2D gi_depth;
layout(set = 0, binding = 12) uniform sampler2D gi_normal;

layout(set = 1, binding = 0, rgba32ui) uniform restrict writeonly uimage2D out_packed;
layout(set = 1, binding = 1, r16f) uniform restrict writeonly image2D out_visibility;
#ifdef USE_IMAGES
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_image_diffuse;
layout(set = 1, binding = 3, rgba16f) uniform restrict writeonly image2D out_image_specular;
#endif

#define FLAG_STOCHASTIC 1u
#define FLAG_STOCHASTIC_IMAGES 2u
#define FLAG_STOCHASTIC_GUIDE 4u
#define FLAG_GI 8u
#define FLAG_GI_GUIDE 16u
#define FLAG_GI_DIRECTIONAL 32u // rt_gi bit 3: re-base onto the pixel's normal.
#define FLAG_GI_OCCLUSION 64u // rt_gi bit 4: the specular occlusion takes the visibility.

layout(push_constant, std430) uniform Params {
	mat4 view_from_ndc;
	ivec2 full_size;
	int stochastic_scale;
	int gi_scale;
	vec4 view_from_world; // A quaternion: the moment is stored in world space.
	vec3 luminance_weights; // The working space's (the scene shader's scene_data.luminance_weights).
	float directionality;
	uint flags;
	uint pad[3];
}
params;

shared float s_depth[GRID * GRID];
shared vec3 s_normal[GRID * GRID];
shared vec2 s_ratio[GRID * GRID];
#ifdef USE_IMAGES
shared vec4 s_image_diffuse[GRID * GRID]; // rgb, the specular's Schlick weight in a.
shared vec3 s_image_specular[GRID * GRID];
#endif
shared float g_depth[GRID * GRID];
shared vec3 g_normal[GRID * GRID];
shared vec3 g_ambient[GRID * GRID];
shared vec3 g_reflection[GRID * GRID];
shared vec4 g_directional[GRID * GRID];

vec3 quat_rotate(vec4 q, vec3 v) {
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// A grid texel's normal from the stochastic block or the GI's, faced
// toward the viewer.
vec3 tap_normal(bool p_gi, int p_k, vec3 p_view) {
	vec3 n = p_gi ? g_normal[p_k] : s_normal[p_k];
	return dot(n, p_view) < 0.0 ? -n : n;
}

// The depth this pixel's plane has along the view ray of full-resolution
// pixel p_full_pixel (the scene shader's plane_depth_at_tap).
float plane_depth_at_tap(ivec2 p_full_pixel, vec3 p_vertex, vec3 p_normal) {
	vec2 ndc = (vec2(p_full_pixel) + 0.5) / vec2(params.full_size) * 2.0 - 1.0;
	vec4 r4 = params.view_from_ndc * vec4(ndc, 1.0, 1.0);
	vec3 r = r4.xyz / r4.w;
	float denom = dot(p_normal, r);
	if (abs(denom) < 1e-6) {
		return -p_vertex.z;
	}
	return -dot(p_normal, p_vertex) / denom * r.z;
}

// The scene shader's tap weights for the four grid texels around a pixel
// (its comments explain each factor): the bilinear weight, times the
// depth weight (r_depth_w, which the fallback ranks on alone), times the
// normal weight. One change: each tap's depth is judged against this
// pixel's plane along the tap's own view ray, as the fragment's fast path
// judged it, not against the pixel's own depth. On a floor at a grazing
// angle a tap a pixel or two away sits at a visibly different depth, and
// the own-depth weight leaned every other row toward the nearer taps: the
// fast path served those fragments, so the loop's lean never showed until
// this kernel ran it everywhere (2 px row seams on the lab's mirror floor,
// blocks_y x8). On a plane the weights are now the bilinear ones; across
// a silhouette the tap is as far off the plane as it was off the depth.
// pow(x, 8) as three squarings: the loops' arithmetic, not their reads,
// was half this kernel's time.
void tap_weights(bool p_gi, ivec2 p_pixel, int p_packed_scale, ivec2 p_origin, int p_width, vec3 p_vertex, vec3 p_view, vec3 p_face, out ivec4 r_k, out vec4 r_w, out vec4 r_depth_w) {
	// Grid texel t was lit at full-res pixel scale * t + the sample offset
	// (rt_sample_offset_inc.glsl; zero below the quarter tier).
	int p_scale = rt_scale(p_packed_scale);
	ivec2 sample_offset = rt_sample_offset(p_packed_scale);
	vec2 pos = vec2(p_pixel - sample_offset) / float(p_scale);
	ivec2 base = ivec2(floor(pos));
	vec2 fr = pos - vec2(base);
	ivec2 half_size = (params.full_size + ivec2(p_scale - 1)) / p_scale;
	float inv_tolerance = 1.0 / max(-p_vertex.z * 0.1, 1e-4);
	for (int i = 0; i < 4; i++) {
		ivec2 off = ivec2(i & 1, i >> 1);
		ivec2 t = base + off - p_origin;
		int k = t.x + t.y * p_width;
		// The full-res pixel the pass lit the (clamped) texel at.
		ivec2 fp = min(clamp(base + off, ivec2(0), half_size - 1) * p_scale + sample_offset, params.full_size - ivec2(1));
		float depth = p_gi ? g_depth[k] : s_depth[k];
		float depth_w = exp(-abs(depth - plane_depth_at_tap(fp, p_vertex, p_face)) * inv_tolerance);
		float c = max(dot(p_face, tap_normal(p_gi, k, p_view)), 0.0);
		c *= c;
		c *= c;
		c *= c;
		r_k[i] = k;
		r_depth_w[i] = depth_w;
		r_w[i] = (1.0 - abs(float(off.x) - fr.x)) * (1.0 - abs(float(off.y) - fr.y)) * depth_w * c;
	}
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * TILE;
	uint local = gl_LocalInvocationIndex;

	// The group's grid blocks: from the tap base of its first pixel to one
	// past its last pixel's, clamped at the grid's edge as the loop clamps.
	// The sample offset moves each block's texel right and down by half a
	// block, so the group's first tap base can be one texel before the
	// tile's (floored, -1 at the frame's edge; the loads clamp).
	int s_scale = max(rt_scale(params.stochastic_scale), 1);
	int g_scale = max(rt_scale(params.gi_scale), 1);
	ivec2 s_origin = ivec2(floor(vec2(tile_origin - rt_sample_offset(params.stochastic_scale)) / float(s_scale)));
	ivec2 g_origin = ivec2(floor(vec2(tile_origin - rt_sample_offset(params.gi_scale)) / float(g_scale)));
	int s_width = TILE / s_scale + 2;
	int g_width = TILE / g_scale + 2;
	// Both on one grid with one guide (the played configuration: both at
	// the quarter tier), the two passes' view depths and tap normals are
	// the same texels, so are the weights: computed once, from the
	// stochastic block.
	const uint guides = FLAG_STOCHASTIC_GUIDE | FLAG_GI_GUIDE;
	bool shared_taps = (params.flags & (FLAG_STOCHASTIC | FLAG_GI)) == (FLAG_STOCHASTIC | FLAG_GI) && params.stochastic_scale == params.gi_scale && ((params.flags & guides) == 0u || (params.flags & guides) == guides);
	if ((params.flags & FLAG_STOCHASTIC) != 0u && local < uint(s_width * s_width)) {
		int scale = s_scale;
		ivec2 half_size = (params.full_size + ivec2(scale - 1)) / scale;
		ivec2 hp = clamp(s_origin + ivec2(int(local) % s_width, int(local) / s_width), ivec2(0), half_size - 1);
		ivec2 fp = min(hp * scale + rt_sample_offset(params.stochastic_scale), params.full_size - ivec2(1));
		s_depth[local] = texelFetch(stochastic_depth, hp, 0).r;
		s_normal[local] = nr_normal(texelFetch(stochastic_normal, (params.flags & FLAG_STOCHASTIC_GUIDE) != 0u ? hp : fp, 0));
		s_ratio[local] = vec2(texelFetch(stochastic_diffuse, hp, 0).r, texelFetch(stochastic_specular, hp, 0).r);
#ifdef USE_IMAGES
		vec4 image_specular = texelFetch(stochastic_image_specular, hp, 0);
		s_image_diffuse[local] = vec4(texelFetch(stochastic_image_diffuse, hp, 0).rgb, image_specular.a);
		s_image_specular[local] = image_specular.rgb;
#endif
	}
	if ((params.flags & FLAG_GI) != 0u && local < uint(g_width * g_width)) {
		int scale = g_scale;
		ivec2 half_size = (params.full_size + ivec2(scale - 1)) / scale;
		ivec2 hp = clamp(g_origin + ivec2(int(local) % g_width, int(local) / g_width), ivec2(0), half_size - 1);
		ivec2 fp = min(hp * scale + rt_sample_offset(params.gi_scale), params.full_size - ivec2(1));
		if (!shared_taps) {
			g_depth[local] = texelFetch(gi_depth, hp, 0).r;
			g_normal[local] = nr_normal(texelFetch(gi_normal, (params.flags & FLAG_GI_GUIDE) != 0u ? hp : fp, 0));
		}
		g_ambient[local] = texelFetch(gi_ambient, hp, 0).rgb;
		g_reflection[local] = texelFetch(gi_reflection, hp, 0).rgb;
		g_directional[local] = texelFetch(gi_directional, hp, 0);
	}
	memoryBarrierShared();
	barrier();

	if (pixel.x >= params.full_size.x || pixel.y >= params.full_size.y) {
		return;
	}
	vec4 nr = texelFetch(normal_roughness_texture, pixel, 0);
	if (!nr_valid(nr)) {
		return; // Sky: no opaque fragment reads it.
	}
	float ndc_depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.full_size);
	vec4 vertex4 = params.view_from_ndc * vec4(uv * 2.0 - 1.0, ndc_depth, 1.0);
	vec3 vertex = vertex4.xyz / vertex4.w;
	vec3 view = -normalize(vertex);
	vec3 normal = nr_normal(nr);
	// Faced toward the viewer, as the passes face the normals they lit
	// each texel with.
	vec3 face = dot(normal, view) < 0.0 ? -normal : normal;
	uvec4 packed_out = uvec4(0u);

	ivec4 s_k = ivec4(0);
	vec4 s_w = vec4(0.0);
	vec4 s_depth_w = vec4(0.0);
	if ((params.flags & FLAG_STOCHASTIC) != 0u) {
		// The scene shader's loop: the sampling pass lit full-res pixel
		// scale * p (+ the sample offset) for grid texel p.
		tap_weights(false, pixel, params.stochastic_scale, s_origin, s_width, vertex, view, face, s_k, s_w, s_depth_w);
		vec2 up_ratio = vec2(0.0);
		vec4 up_image_diffuse = vec4(0.0);
		vec3 up_image_specular = vec3(0.0);
		float up_weight = 0.0;
		int near = 0;
		for (int i = 0; i < 4; i++) {
			int k = s_k[i];
			float w = s_w[i];
			up_ratio += s_ratio[k] * w;
#ifdef USE_IMAGES
			up_image_diffuse += s_image_diffuse[k] * w;
			up_image_specular += s_image_specular[k] * w;
#endif
			up_weight += w;
			// Ranked on depth alone, the first of equals as the loop's strict test.
			near = s_depth_w[i] > s_depth_w[near] ? i : near;
		}
		if (up_weight > 1e-6) {
			up_ratio /= up_weight;
			up_image_diffuse /= up_weight;
			up_image_specular /= up_weight;
		} else {
			up_ratio = s_ratio[s_k[near]];
#ifdef USE_IMAGES
			up_image_diffuse = s_image_diffuse[s_k[near]];
			up_image_specular = s_image_specular[s_k[near]];
#endif
		}
		packed_out.w = packHalf2x16(up_ratio);
#ifdef USE_IMAGES
		imageStore(out_image_diffuse, pixel, up_image_diffuse);
		imageStore(out_image_specular, pixel, vec4(up_image_specular, 0.0));
#endif
	}

	if ((params.flags & FLAG_GI) != 0u) {
		ivec4 g_k = s_k;
		vec4 g_w = s_w;
		vec4 g_depth_w = s_depth_w;
		if (!shared_taps) {
			tap_weights(true, pixel, params.gi_scale, g_origin, g_width, vertex, view, face, g_k, g_w, g_depth_w);
		}
		// The GI's taps' normals are in the block their weights came from.
		bool normals_gi = !shared_taps;
		vec2 fr = fract(vec2(pixel - rt_sample_offset(params.gi_scale)) / float(g_scale));
		ivec2 near_tap = ivec2(greaterThanEqual(fr, vec2(0.5)));
		int near_i = near_tap.x + near_tap.y * 2;
		vec3 ambient = vec3(0.0);
		vec3 reflection = vec3(0.0);
		vec4 directional = vec4(0.0);
		float weight = 0.0;
		int near_depth = 0;
		int best = 0;
		for (int i = 0; i < 4; i++) {
			int k = g_k[i];
			float w = g_w[i];
			ambient += g_ambient[k] * w;
			reflection += g_reflection[k] * w;
			directional += g_directional[k] * w;
			weight += w;
			near_depth = g_depth_w[i] > g_depth_w[near_depth] ? i : near_depth;
			best = w > g_w[best] ? i : best;
		}
		vec3 gather_normal;
		if (weight > 1e-6) {
			ambient /= weight;
			reflection /= weight;
			directional /= weight;
			gather_normal = tap_normal(normals_gi, g_k[g_w[near_i] > 0.05 * weight ? near_i : best], view);
		} else {
			ambient = g_ambient[g_k[near_depth]];
			reflection = g_reflection[g_k[near_depth]];
			directional = g_directional[g_k[near_depth]];
			gather_normal = face;
		}
		// The scene shader's re-basing (its comments explain the numbers),
		// on the normal the fragment would use.
		float visibility = 1.0;
		bool occlusion_valid = false;
		if ((params.flags & FLAG_GI_DIRECTIONAL) != 0u) {
			vec3 gi_l1 = quat_rotate(params.view_from_world, directional.xyz);
			float gi_l0 = max(dot(ambient, params.luminance_weights), 1e-6);
			float gi_len = length(gi_l1);
			if (gi_len > 1e-6 * gi_l0) {
				vec3 gi_dir = gi_l1 / gi_len;
				float gi_q = clamp(gi_len / gi_l0, 0.0, 1.0);
				visibility = clamp(directional.w, 0.0, 1.0);
				occlusion_valid = (params.flags & FLAG_GI_OCCLUSION) != 0u;
				float gi_s = clamp((gi_q - 2.0 / 3.0) * 12.0, 0.0, 1.0) * params.directionality;
				gi_s = min(gi_s, 0.75);
				float gi_num = (1.0 - gi_s) + gi_s * max(dot(gi_dir, face), 0.0);
				float gi_den = (1.0 - gi_s) + gi_s * max(dot(gi_dir, gather_normal), 0.0);
				ambient *= clamp(gi_num / max(gi_den, 0.25), 0.5, 2.0);
			}
		}
		ambient = min(ambient, vec3(65504.0));
		reflection = min(reflection, vec3(65504.0));
		packed_out.x = packHalf2x16(ambient.rg);
		packed_out.y = packHalf2x16(vec2(ambient.b, reflection.r));
		packed_out.z = packHalf2x16(reflection.gb);
		imageStore(out_visibility, pixel, vec4(occlusion_valid ? visibility : -1.0));
	}
	imageStore(out_packed, pixel, packed_out);
}
