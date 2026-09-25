#[compute]

#version 460

#VERSION_DEFINES

// ReSTIR GI on the gather's diffuse rays (plan R2, sections 114-115;
// GODOT_GI_RESTIR, off by default), after Ouyang et al. 2021 and ReSTIR PT
// Enhanced (Lin, Kettunen, Wyman 2026) for a one-bounce path: the camera
// x0, this pixel's surface x1, the ray's hit x2 -- the reconnection vertex.
// Between the gather (and its deferred hit shading) and the temporal
// denoiser, each pixel keeps a reservoir: one hit point and the radiance
// read there, chosen in proportion to its contribution here (the target
// p = luminance(L) cos1 / pi), with the weight W that makes f(y) W an
// estimate of the cosine lobe's mean radiance, which is what the gather's
// own rays average to.
//
// RESTIR_TEMPORAL: the pixel's own diffuse rays streamed into a reservoir
// (each ray's weight p / (n pdf), the cosine pdf), then last frame's
// reservoir at the reprojection: on this pixel's plane and facing, its
// confidence M capped (GODOT_GI_RESTIR_MCAP, 20 as in both papers) and
// lowered by the gather's change mark (the radiance a reservoir carries is
// the one read when it was traced: a light that changed makes it stale).
// With the duplication map (the paper's section 5, GODOT_GI_RESTIR_DUP) the
// cap falls to 1 where last frame's neighbourhood shares the sample:
// lerp(cap, 1, D^0.1).
//
// RESTIR_SPATIAL: the temporal reservoir and K neighbours' (3, drawn from a
// Gaussian of sigma 16 full-resolution pixels: the paper's R = 30 disk,
// GODOT_GI_RESTIR_K, _RADIUS), each on this pixel's plane, their samples
// reconnected to this receiver. A neighbour's sample is tested for
// visibility with a ray (GODOT_GI_RESTIR_VIS=0 skips it): occluded, the path
// through it does not exist from here.
//
// RESTIR_DUP: the duplication map, after the frame's reservoirs are final:
// the share of a (2r+1)^2 window (17 x 17 full-resolution pixels) holding
// the same sample, by the id each sample is born with.
//
// Reconnection. A sample moved to another receiver keeps its hit; its
// solid angle changes by the Jacobian (cos2' / cos2) (d^2 / d'^2), with
// cos2 the cosine at the hit toward each receiver: the gather records the
// hit's normal (the G-buffer's for a screen hit, the card texel's, the hit
// shader's geometric one). A sample may be reconnected only if its hit
// passes the paper's footprint test (Eq. 5; diffuse hits skip the inverse
// footprint): pi d^2 / (cos1 cos2) >= (c / 100) 4 pi |x0 - x1|^2 / cos0,
// c = 0.02 (GODOT_GI_RESTIR_C). A hit nearer than that -- a corner, the
// foot of a step -- is where the shift's density changes fastest, and it is
// kept by its own pixel only (by its history too while the receiver has
// not moved by more than half a pixel). Without the normal and the test
// (the first build, section 114) a hit grazing from one receiver and
// head-on from another took the ratio of the two cosines as its weight:
// the game's flashlight at the foot of a step read 5x the reference and the
// history spread it into disks.
//
// Combination: the generalized balance heuristic (Talbot; Lin 2022's
// unbiased form without the visibility term) in the area measure, each
// candidate's MIS weight M_i p_i(y) / sum_j M_j p_j(y) over the receivers
// its sample can be shifted to. Output: the candidates' vector-weighted sum
// (the paper's section 6.3; GODOT_GI_RESTIR_OUT=pick the picked sample's
// f(y) W), written over the pixel's own rays' mean in the raw buffers as a
// delta (the planar mirrors' image lights the gather adds stay), the
// directional moment rescaled with it.
//
// The moving lights' term (FLAG_DYN_SPLIT) stays with the pixel's own rays:
// its history restarts under every sweep, and a reservoir's radiance is the
// most stale where a light moves. Candidates carry the static share only.
//
// Not the paper's: no hybrid shift (a sample that fails the test is not
// replayed from another pixel -- one bounce has nothing to replay to), no
// dual motion vectors (disocclusion is 0.1 % a frame here, section 99), no
// paired spatial reuse (a cost, not a quality measure).

#if defined(RESTIR_SPATIAL)
#extension GL_EXT_ray_query : require
#endif

#include "../normal_roughness_inc.glsl"
#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"
#include "rt_sample_offset_inc.glsl"

#define M_PI 3.14159265359

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// The gather's parameters (stochastic_indirect_gi.glsl's Params), the
// prefix this pass reads; the buffer is the gather's own UBO.
layout(set = 0, binding = 0, std140) uniform GiParams {
	mat4 view_from_ndc;
	mat4 ndc_from_view;
	mat4 world_from_view;
	mat4 reproject; // Current NDC -> previous frame NDC.
	ivec2 screen_size;
	ivec2 full_screen_size;
	uint depth_scale;
	uint frame_index;
	uint ray_count;
	uint flags;
	vec4 sky_quat_or_color;
	float sky_energy;
	float ray_bias;
}
gi;

layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;

// The gather's ray records (stochastic_gi_reuse.glsl describes them; the
// high half of w the hit's normal under RAY_NORMAL).
layout(set = 0, binding = 3, std430) restrict readonly buffer Rays {
	uvec4 data[];
}
rays;

// A reservoir, four uvec4 a pixel:
// 0: the sample's hit point (world, floats), W
// 1: the receiver it is anchored at (world, floats), M and the estimate's
//    luminance p(y) W (halves)
// 2: x the radiance's r g (halves); y its b (half) | the hit's normal << 16
//    (rt_hit_pack_normal16); z the receiver's normal (octahedral); w the
//    flags (8 bits) | the sample's id << 8
// 3: xy the estimate's colour (halves: r g, b), zw unused
struct Reservoir {
	vec3 hit;
	float w_sum; // While streaming; W once finished.
	vec3 recv;
	float m;
	// The estimate's luminance, p(y) W at the receiver it was finished at:
	// the temporal chain averages it as stored. Re-evaluated from the stored
	// (quantized) normal and radiance instead, a grazing sample's target --
	// tiny, and W its inverse -- came back a thousand times larger and the
	// chain ran to the half-float limit (section 115).
	float e;
	// The estimate itself, its colour (the paper's vector weights, section
	// 6.3, carried through the temporal chain too): the sum of each
	// candidate's weight times f / p of its sample, averaged with the same
	// weights as e. One sample's colour at the chain's luminance (the first
	// builds) was a speckle of hues on the lab's red and green walls, the
	// error at rest 2-3x the default's (section 115).
	vec3 e_rgb;
	vec3 radiance;
	uint hit_normal; // Packed (rt_hit_pack_normal16), valid under RES_NORMAL.
	vec3 recv_normal;
	uint flags;
	uint id;
};
#define RES_VALID 1u
#define RES_DISTANT 2u // The ray missed: the sample is a direction, hit = recv + dir.
#define RES_NEIGHBOUR 4u // Chosen from a spatial neighbor this frame (the visibility ray's test).
#define RES_NORMAL 8u // The hit's normal is known.
#define RES_RECONNECT 16u // The sample passes the footprint test: other receivers may take it.
#define RES_SAMPLE_FLAGS (RES_DISTANT | RES_NORMAL | RES_RECONNECT)

layout(set = 0, binding = 4, std430) restrict readonly buffer ReservoirsIn {
	uvec4 data[];
}
res_in;
#if !defined(RESTIR_DUP)
layout(set = 0, binding = 5, std430) restrict writeonly buffer ReservoirsOut {
	uvec4 data[];
}
res_out;
#endif

#if defined(RESTIR_TEMPORAL) || defined(RESTIR_DUP)
// The duplication map (a float a pixel): written by RESTIR_DUP at the end of
// a frame, read by the next frame's temporal pass at the reprojection.
layout(set = 0, binding = 7, std430) restrict buffer Duplication {
	float data[];
}
dup;
#endif

#ifdef RESTIR_SPATIAL
layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;
layout(set = 1, binding = 0, rgba16f) uniform restrict image2D raw_ambient;
layout(set = 1, binding = 1, rgba16f) uniform restrict image2D raw_directional;
layout(set = 1, binding = 2, rgba16f) uniform restrict readonly image2D raw_ambient_dyn;
#endif
#ifdef RESTIR_TEMPORAL
layout(set = 1, binding = 0, rgba16f) uniform restrict image2D raw_ambient;
#endif

// Must match the gather's GI_REUSE_RAY_* (stochastic_indirect_gi.glsl).
#define RAY_VALID 1u
#define RAY_MISS 64u
#define RAY_NORMAL 128u
#define RAY_COUNT_SHIFT 4u
#define RAY_DYN_SHIFT 8u

#define FLAG_HISTORY 1u // Last frame's reservoirs are this view's, at this size.
#define FLAG_DYN_SPLIT 2u
#define FLAG_VISIBILITY 4u // Neighbours' samples are tested with a ray.
#define FLAG_OUTPUT_RAW 8u // Diagnostics: the pixel's own rays' mean written unchanged (the plumbing's null test).
#define FLAG_OUTPUT_MEAN 16u // The vector-weighted sum (default) rather than the picked sample.
#define FLAG_DUP 32u // The duplication map lowers the temporal cap.
#define FLAG_GAUSS 64u // Neighbors from a Gaussian (default) rather than a disk.
#define FLAG_CRITERION 128u // The footprint test (default); off, every sample with a normal reconnects.
#define FLAG_HIT_COS 256u // The hit's cosine in the Jacobian and the area target (default).
#define FLAG_DUP_HISTORY 512u // The duplication map holds last frame's (else D = 0).
// The target is luminance(L) cos1 / pi (default) or, measured and worse,
// the sample's radiance luminance over the hemisphere (Ouyang et al.'s
// p = L; GODOT_GI_RESTIR_TARGET=radiance): our candidates are cosine-drawn,
// so a grazing one's weight p / pdf goes as 1 / cos1 and the game project's
// fireflies reached 20x the reference (section 115).
#define FLAG_TARGET_RADIANCE 2048u
// Diagnostics (GODOT_GI_RESTIR_PAINT=1): the temporal pass paints the raw
// ambient and the spatial pass leaves it: r this frame's own estimate, g the
// history's (both luminance), b the result's, a 0.5 where the history was
// the same sample (within a pixel), 1 reconnected. With
// GODOT_GI_SPATIAL=0 TFRAMES=1 the gi AOV is the paint.
#define FLAG_PAINT 4096u
#define FLAG_RECONNECT_HISTORY 8192u // Diagnostics (GODOT_GI_RESTIR_IDENTITY=0): the history within a pixel reconnected too.
// The fresh reservoir from the 3x3 neighbourhood's rays (default;
// GODOT_GI_RESTIR_INIT=own the pixel's own rays only): the neighbourhood
// reuse's pool (stochastic_gi_reuse.glsl, section 102) as the initial
// candidates, each weighted by the balance heuristic over the nine pixels'
// cosine lobes that could have drawn it (p / sum_q n_q pdf_q, in this
// pixel's solid angle). ReSTIR in place of that pool (the first builds) lost
// its averaging: the default's error at rest was 20-60 % lower than ReSTIR
// on the own rays (section 115). A neighbour's ray joins only if its hit
// passes the footprint test from that neighbor (a near hit is its pixel's).
#define FLAG_POOL 16384u

layout(push_constant, std430) uniform Params {
	uint flags;
	uint frame;
	float m_cap; // A reservoir's confidence carried to the next frame.
	float radius; // The spatial disk's radius, or the Gaussian's sigma, in the signal's pixels.
	uint neighbors; // K.
	float jacobian_max; // Diagnostics: the Jacobian clamped to 1 / max .. max (0: not).
	float depth_tolerance; // Of the view depth: a neighbor off this plane by more is another surface.
	float normal_min; // The normals' dot.
	float luma_r;
	float luma_g;
	float luma_b;
	float distant_t; // A miss's visibility ray length.
	float footprint_c; // The footprint test's c (0.02).
	float pixel_angle; // A signal pixel's angular size (radians) at the screen's center.
	int dup_radius; // The duplication window's half-size, in signal pixels.
	float dup_alpha; // D's exponent in the cap's lerp (0.1).
}
params;

float luminance(vec3 c) {
	return dot(c, vec3(params.luma_r, params.luma_g, params.luma_b));
}

uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint rng_state;
float rand() {
	rng_state = pcg_hash(rng_state);
	return float(rng_state >> 8u) * (1.0 / 16777216.0);
}

uint pixel_index(ivec2 p) {
	return uint(p.y * gi.screen_size.x + p.x);
}

Reservoir reservoir_load_in(uint i) {
	uvec4 a = res_in.data[i * 4u];
	uvec4 b = res_in.data[i * 4u + 1u];
	uvec4 c = res_in.data[i * 4u + 2u];
	uvec4 d = res_in.data[i * 4u + 3u];
	Reservoir r;
	r.hit = uintBitsToFloat(a.xyz);
	r.w_sum = uintBitsToFloat(a.w);
	r.recv = uintBitsToFloat(b.xyz);
	vec2 me = unpackHalf2x16(b.w);
	r.m = me.x;
	r.e = me.y;
	r.radiance = vec3(unpackHalf2x16(c.x), unpackHalf2x16(c.y & 0xFFFFu).x);
	r.hit_normal = c.y >> 16u;
	r.recv_normal = rt_hit_unpack_dir(c.z);
	r.flags = c.w & 0xFFu;
	r.id = c.w >> 8u;
	r.e_rgb = vec3(unpackHalf2x16(d.x), unpackHalf2x16(d.y).x);
	return r;
}

Reservoir reservoir_empty() {
	Reservoir r;
	r.hit = vec3(0.0);
	r.w_sum = 0.0;
	r.recv = vec3(0.0);
	r.m = 0.0;
	r.e = 0.0;
	r.e_rgb = vec3(0.0);
	r.radiance = vec3(0.0);
	r.hit_normal = 0u;
	r.recv_normal = vec3(0.0, 0.0, 1.0);
	r.flags = 0u;
	r.id = 0u;
	return r;
}

#if !defined(RESTIR_DUP)
void reservoir_store(uint i, Reservoir r) {
	res_out.data[i * 4u] = uvec4(floatBitsToUint(r.hit), floatBitsToUint(r.w_sum));
	res_out.data[i * 4u + 1u] = uvec4(floatBitsToUint(r.recv), packHalf2x16(vec2(r.m, r.e)));
	res_out.data[i * 4u + 3u] = uvec4(packHalf2x16(r.e_rgb.rg), packHalf2x16(vec2(r.e_rgb.b, 0.0)), 0u, 0u);
	res_out.data[i * 4u + 2u] = uvec4(packHalf2x16(r.radiance.rg), (packHalf2x16(vec2(r.radiance.b, 0.0)) & 0xFFFFu) | (r.hit_normal << 16u), rt_hit_pack_dir(r.recv_normal), (r.flags & 0xFFu) | (r.id << 8u));
}
#endif

struct Surface {
	vec3 pos; // World.
	vec3 normal; // World, the shading normal (the lobe the gather sampled).
	vec3 shade_normal; // World.
	float view_depth;
	vec3 ndc;
	float footprint; // Half a signal pixel's width at this depth (world units).
};

// The gather's sampling point (gather_pixel_setup): the full-resolution
// pixel of the block it sampled.
bool surface_at(ivec2 pixel, out Surface s) {
	ivec2 full_pixel = rt_full_pixel(pixel, int(gi.depth_scale), gi.full_screen_size);
	float depth = texelFetch(depth_texture, full_pixel, 0).r;
	if (depth == 0.0) {
		return false;
	}
	vec2 uv = (vec2(full_pixel) + 0.5) / vec2(gi.full_screen_size);
	s.ndc = vec3(uv * 2.0 - 1.0, depth);
	vec4 p = gi.view_from_ndc * vec4(s.ndc, 1.0);
	vec3 view_pos = p.xyz / p.w;
	s.view_depth = -view_pos.z;
	s.pos = (gi.world_from_view * vec4(view_pos, 1.0)).xyz;
	vec3 n = nr_normal(texelFetch(normal_roughness_texture, full_pixel, 0));
	// Toward the viewer, as the gather turns it (a double-sided material's
	// normal wound against the triangle's).
	if (dot(n, -view_pos) < 0.0) {
		n = -n;
	}
	s.normal = normalize(mat3(gi.world_from_view) * n);
	s.shade_normal = s.normal;
	s.footprint = 0.5 * length(view_pos) * params.pixel_angle;
	return true;
}

// The cosine at the sample's hit toward a receiver (1 without a normal, or
// with FLAG_HIT_COS off: the first build's assumption).
float hit_cos(Reservoir s, vec3 x) {
	if ((s.flags & RES_NORMAL) == 0u || (params.flags & FLAG_HIT_COS) == 0u) {
		return 1.0;
	}
	return dot(rt_hit_unpack_normal16(s.hit_normal), normalize(x - s.hit));
}

// May the sample be taken at receiver x? Its own anchor always (a receiver
// within half a pixel of it is the same surface point); elsewhere only a
// reconnectable sample, whose hit faces x.
bool shift_ok(Reservoir s, vec3 x, float footprint) {
	if (distance(x, s.recv) <= footprint) {
		return true;
	}
	if ((s.flags & RES_RECONNECT) == 0u) {
		return false;
	}
	return (s.flags & RES_DISTANT) != 0u || hit_cos(s, x) > 1e-3;
}

// The sample seen from a receiver: its direction, and the solid angle's
// change against the receiver it is anchored at. False where it is not
// usable.
bool reconnect(Reservoir s, vec3 recv, out vec3 dir, out float jacobian, out float dist) {
	if ((s.flags & RES_DISTANT) != 0u) {
		dir = normalize(s.hit - s.recv);
		jacobian = 1.0;
		dist = params.distant_t;
		return true;
	}
	vec3 to_hit = s.hit - recv;
	float d2 = dot(to_hit, to_hit);
	jacobian = 0.0;
	dist = 0.0;
	dir = vec3(0.0, 0.0, 1.0);
	if (d2 <= 1e-8) {
		return false;
	}
	dist = sqrt(d2);
	dir = to_hit / dist;
	if (distance(recv, s.recv) <= 1e-4 * dist) {
		// The identity shift: the sample at the receiver it was drawn at
		// (a recorded normal that faces away does not make it unusable).
		jacobian = 1.0;
		return true;
	}
	vec3 from = s.hit - s.recv;
	float cos_here = hit_cos(s, recv);
	float cos_anchor = hit_cos(s, s.recv);
	if (cos_here <= 1e-3 || cos_anchor <= 1e-3) {
		return false;
	}
	jacobian = (cos_here / cos_anchor) * dot(from, from) / d2;
	if (params.jacobian_max > 0.0) {
		jacobian = clamp(jacobian, 1.0 / params.jacobian_max, params.jacobian_max);
	}
	return true;
}

// p: the sample's radiance luminance over the receiver's hemisphere (or
// with the cosine, FLAG_TARGET_COS).
float target(vec3 radiance, vec3 normal, vec3 dir) {
	float c = dot(normal, dir);
	if ((params.flags & FLAG_TARGET_RADIANCE) != 0u) {
		return c > 0.0 ? luminance(radiance) : 0.0;
	}
	return luminance(radiance) * max(c, 0.0) / M_PI;
}

// f / p at the receiver: what a unit of resampling weight is worth (the
// integrand is L cos1 / pi over the shading normal's lobe, the target's
// cosine the geometric normal's).
vec3 contribution_per_weight(vec3 radiance, Surface p, vec3 dir) {
	float c = max(dot(p.shade_normal, dir), 0.0);
	float t = target(radiance, p.normal, dir);
	return t > 0.0 ? radiance * (c / M_PI) / t : vec3(0.0);
}

// The sample's target at a receiver (x, n) in the area measure: p cos2 /
// d^2. A distant sample is a direction, the same from every receiver.
float area_target(Reservoir s, vec3 x, vec3 n) {
	if ((s.flags & RES_DISTANT) != 0u) {
		return target(s.radiance, n, normalize(s.hit - s.recv));
	}
	vec3 to_hit = s.hit - x;
	float d2 = dot(to_hit, to_hit);
	if (d2 <= 1e-8) {
		return 0.0;
	}
	return target(s.radiance, n, to_hit * inversesqrt(d2)) * max(hit_cos(s, x), 0.0) / d2;
}

// Weighted reservoir sampling: s is taken with probability w / sum.
bool reservoir_pick(inout Reservoir r, Reservoir s, float w) {
	if (!(w > 0.0)) {
		return false;
	}
	r.w_sum += w;
	if (rand() * r.w_sum < w) {
		r.hit = s.hit;
		r.radiance = s.radiance;
		r.hit_normal = s.hit_normal;
		r.id = s.id;
		r.flags = (r.flags & RES_VALID) | (s.flags & RES_SAMPLE_FLAGS);
		// The receiver the sample's W was in the measure of (a distant
		// sample's direction is taken from it); the finished reservoir is
		// re-anchored at the pixel that holds it.
		r.recv = s.recv;
		return true;
	}
	return false;
}

// Candidate i's resampling weight at the canonical receiver p: its MIS
// weight (m_i = M_i p_i(y) over mis_sum = sum_j M_j p_j(y), both in the area
// measure) times p_c(y) W_i J_i.
float candidate_weight(Reservoir s, float m_i, float mis_sum, Surface p) {
	if (!(s.w_sum > 0.0) || !(mis_sum > 0.0)) {
		return 0.0;
	}
	float own = m_i * area_target(s, s.recv, s.recv_normal);
	vec3 dir;
	float jacobian, dist;
	if (!reconnect(s, p.pos, dir, jacobian, dist)) {
		return 0.0;
	}
	return own / mis_sum * target(s.radiance, p.normal, dir) * s.w_sum * jacobian;
}

// W from the streamed sum: sum / p(y) at this receiver.
void reservoir_finish(inout Reservoir r, Surface p) {
	float p_hat = 0.0;
	vec3 dir;
	float jacobian, dist;
	if (r.w_sum > 0.0 && reconnect(r, p.pos, dir, jacobian, dist)) {
		p_hat = target(r.radiance, p.normal, dir);
	}
	r.e = p_hat > 0.0 ? r.w_sum : 0.0;
	r.w_sum = p_hat > 0.0 ? r.w_sum / p_hat : 0.0;
}

// Re-anchored at the pixel that holds it: W is in its measure now (a
// distant sample keeps its direction).
void reservoir_anchor(inout Reservoir r, Surface p) {
	if ((r.flags & RES_DISTANT) != 0u) {
		r.hit = p.pos + normalize(r.hit - r.recv);
	}
	r.recv = p.pos;
	r.recv_normal = p.normal;
}

#ifdef RESTIR_TEMPORAL

// The workgroup's 8x8 pixels and a border of one: each pixel's surface and
// its ray count, for the pooled candidates.
shared vec3 tile_pos[100];
shared vec3 tile_nrm[100];
shared uint tile_info[100]; // Bit 0 a surface with rays, bits 1-2 its rays - 1.

void load_tile(ivec2 origin) {
	for (uint i = gl_LocalInvocationIndex; i < 100u; i += 64u) {
		ivec2 q = origin + ivec2(int(i % 10u), int(i / 10u));
		Surface qs;
		uint info = 0u;
		tile_pos[i] = vec3(0.0);
		tile_nrm[i] = vec3(0.0, 0.0, 1.0);
		if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, gi.screen_size)) && surface_at(q, qs)) {
			uint w = rays.data[pixel_index(q) * (gi.ray_count + 1u)].w;
			if ((w & RAY_VALID) != 0u) {
				info = 1u | (((w >> RAY_COUNT_SHIFT) & 3u) << 1u);
				tile_pos[i] = qs.pos;
				tile_nrm[i] = qs.normal;
			}
		}
		tile_info[i] = info;
	}
}

// The footprint test's right-hand side at a receiver: (c / 100) 4 pi
// |x0 - x1|^2 / cos0.
float footprint_min_at(vec3 pos, vec3 normal) {
	vec3 to_eye = gi.world_from_view[3].xyz - pos;
	float cos0 = max(dot(normal, normalize(to_eye)), 0.05);
	return params.footprint_c * 0.01 * 4.0 * M_PI * dot(to_eye, to_eye) / cos0;
}

// Would a ray from receiver (pos, normal) to the sample's hit pass the
// footprint test there? (Always, for a direction.)
bool reconnectable_from(Reservoir s, vec3 pos, vec3 normal) {
	if ((s.flags & RES_DISTANT) != 0u) {
		return true;
	}
	if ((s.flags & RES_NORMAL) == 0u) {
		return false;
	}
	if ((params.flags & FLAG_CRITERION) == 0u) {
		return true;
	}
	vec3 to_hit = s.hit - pos;
	float d2 = dot(to_hit, to_hit);
	vec3 dir = to_hit * inversesqrt(max(d2, 1e-12));
	float cos1 = dot(normal, dir);
	float cos2 = dot(rt_hit_unpack_normal16(s.hit_normal), -dir);
	return cos1 > 1e-3 && cos2 > 1e-3 && M_PI * d2 / (cos1 * cos2) >= footprint_min_at(pos, normal);
}

// The density receiver (pos, normal)'s cosine lobe draws the sample with,
// in the solid angle of receiver p (the area measure's conversion by the
// hit's cosine and distance).
float cosine_pdf_at(Reservoir s, vec3 pos, vec3 normal, Surface p) {
	if ((s.flags & RES_DISTANT) != 0u) {
		return max(dot(normal, normalize(s.hit - s.recv)), 0.0) / M_PI;
	}
	vec3 to_q = s.hit - pos;
	vec3 to_p = s.hit - p.pos;
	float dq2 = max(dot(to_q, to_q), 1e-12);
	float dp2 = max(dot(to_p, to_p), 1e-12);
	float pdf = max(dot(normal, to_q * inversesqrt(dq2)), 0.0) / M_PI;
	float cq = max(hit_cos(s, pos), 0.0);
	float cp = max(hit_cos(s, p.pos), 1e-4);
	return pdf * (cq / dq2) / (cp / dp2);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	bool pool = (params.flags & FLAG_POOL) != 0u;
	if (pool) {
		load_tile(ivec2(gl_WorkGroupID.xy) * 8 - 1);
		barrier();
	}
	if (pixel.x >= gi.screen_size.x || pixel.y >= gi.screen_size.y) {
		return;
	}
	uint index = pixel_index(pixel);
	Surface p;
	uint slots = gi.ray_count + 1u;
	uint own_base = index * slots;
	uvec4 own0 = rays.data[own_base];
	if (!surface_at(pixel, p) || (own0.w & RAY_VALID) == 0u) {
		reservoir_store(index, reservoir_empty());
		return;
	}
	rng_state = pcg_hash(index ^ pcg_hash(params.frame * 3u + 1u));
	float tol = params.depth_tolerance * max(p.view_depth, 1.0);
	mat3 world_basis = mat3(gi.world_from_view);

	// The canonical reservoir: this frame's rays -- the pixel's own, or the
	// 3x3 neighbourhood's (FLAG_POOL) -- each with the weight p / (sum_q n_q
	// pdf_q) over the lobes that could have drawn it, so W = sum / p(y).
	Reservoir c = reservoir_empty();
	c.flags = RES_VALID;
	c.recv = p.pos;
	c.recv_normal = p.normal;
	ivec2 lp = ivec2(gl_LocalInvocationID.xy) + 1;
	int reach = pool ? 1 : 0;
	for (int dy = -reach; dy <= reach; dy++) {
		for (int dx = -reach; dx <= reach; dx++) {
			bool self = dx == 0 && dy == 0;
			vec3 qpos = p.pos;
			vec3 qn = p.normal;
			uint q_rays = ((own0.w >> RAY_COUNT_SHIFT) & 3u) + 1u;
			if (!self) {
				uint ti = uint((lp.y + dy) * 10 + lp.x + dx);
				uint info = tile_info[ti];
				qpos = tile_pos[ti];
				qn = tile_nrm[ti];
				if ((info & 1u) == 0u || abs(dot(p.normal, qpos - p.pos)) > tol || dot(p.normal, qn) < params.normal_min) {
					continue;
				}
				q_rays = ((info >> 1u) & 3u) + 1u;
			}
			c.m += float(q_rays);
			uint q_base = pixel_index(pixel + ivec2(dx, dy)) * slots;
			for (uint k = 0u; k < q_rays; k++) {
				uvec4 rec = rays.data[q_base + k];
				vec2 by = unpackHalf2x16(rec.y);
				vec3 radiance = max(vec3(unpackHalf2x16(rec.x), by.x), vec3(0.0));
				if (any(isnan(radiance)) || any(isinf(radiance))) {
					radiance = vec3(0.0);
				}
				if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
					radiance *= 1.0 - float((rec.w >> RAY_DYN_SHIFT) & 255u) / 255.0;
				}
				vec3 dir = normalize(world_basis * rt_hit_unpack_dir(rec.z));
				Reservoir s = reservoir_empty();
				s.recv = qpos;
				s.radiance = radiance;
				bool miss = (rec.w & RAY_MISS) != 0u;
				s.flags = miss ? RES_DISTANT : 0u;
				s.hit = qpos + dir * (miss ? 1.0 : by.y);
				s.id = (pcg_hash(pixel_index(pixel + ivec2(dx, dy)) * 4u + k + params.frame * 0x9E3779B9u) & 0xFFFFFEu) | 1u;
				if ((rec.w & RAY_NORMAL) != 0u && !miss) {
					s.flags |= RES_NORMAL;
					s.hit_normal = rec.w >> 16u;
				}
				if (reconnectable_from(s, qpos, qn)) {
					s.flags |= RES_RECONNECT;
				}
				if (!self && (s.flags & RES_RECONNECT) == 0u) {
					continue; // A near hit is its own pixel's.
				}
				// The sample seen from p (a direction stays one).
				if (miss) {
					s.hit = p.pos + dir;
					s.recv = p.pos;
				}
				vec3 dir_p = miss ? dir : normalize(s.hit - p.pos);
				float p_hat = target(radiance, p.normal, dir_p);
				// The mixture of the lobes that could have drawn it: every
				// pooled pixel for a reconnectable hit, this one alone for
				// a near hit.
				float mixture = 0.0;
				for (int ey = -reach; ey <= reach; ey++) {
					for (int ex = -reach; ex <= reach; ex++) {
						bool eself = ex == 0 && ey == 0;
						vec3 epos = p.pos;
						vec3 en = p.normal;
						uint e_rays = ((own0.w >> RAY_COUNT_SHIFT) & 3u) + 1u;
						if (!eself) {
							if ((s.flags & RES_RECONNECT) == 0u) {
								continue;
							}
							uint ei = uint((lp.y + ey) * 10 + lp.x + ex);
							uint einfo = tile_info[ei];
							epos = tile_pos[ei];
							en = tile_nrm[ei];
							if ((einfo & 1u) == 0u || abs(dot(p.normal, epos - p.pos)) > tol || dot(p.normal, en) < params.normal_min || !reconnectable_from(s, epos, en)) {
								continue;
							}
							e_rays = ((einfo >> 1u) & 3u) + 1u;
						}
						mixture += float(e_rays) * cosine_pdf_at(s, epos, en, p);
					}
				}
				s.recv = p.pos; // Anchored here: the mixture is in p's measure.
				float w = mixture > 1e-8 ? p_hat / mixture : 0.0;
				if (w > 0.0) {
					c.e_rgb += w * contribution_per_weight(radiance, p, dir_p);
				}
				reservoir_pick(c, s, w);
			}
		}
	}
	// A pixel whose rays all brought no light keeps its first ray as the
	// sample, so the reservoir still anchors the history.
	if (c.w_sum <= 0.0) {
		vec3 dir = normalize(world_basis * rt_hit_unpack_dir(own0.z));
		bool miss = (own0.w & RAY_MISS) != 0u;
		c.hit = p.pos + dir * (miss ? 1.0 : unpackHalf2x16(own0.y).y);
		c.flags = RES_VALID | (miss ? RES_DISTANT : 0u);
		c.radiance = vec3(0.0);
	}
	reservoir_finish(c, p);
	c.recv = p.pos;

	// Last frame's reservoir at the reprojection.
	Reservoir h = reservoir_empty();
	float h_m = 0.0;
	bool identity = false;
	if ((params.flags & FLAG_HISTORY) != 0u) {
		vec4 prev = gi.reproject * vec4(p.ndc, 1.0);
		if (prev.w > 0.0) {
			vec2 prev_uv = (prev.xy / prev.w) * 0.5 + 0.5;
			ivec2 prev_pixel = ivec2(floor(prev_uv * vec2(gi.screen_size)));
			if (all(greaterThanEqual(prev_pixel, ivec2(0))) && all(lessThan(prev_pixel, gi.screen_size))) {
				uint prev_index = pixel_index(prev_pixel);
				h = reservoir_load_in(prev_index);
				float tol = params.depth_tolerance * max(p.view_depth, 1.0);
				bool same_surface = (h.flags & RES_VALID) != 0u && abs(dot(p.normal, h.recv - p.pos)) <= tol && dot(p.normal, h.recv_normal) >= params.normal_min && distance(h.recv, p.pos) <= 4.0 * tol;
				// The reprojection takes the nearest pixel, so the history's
				// receiver is within a pixel on screen by construction -- the
				// upscaler's jitter at rest, the rounding in motion (a world
				// distance test missed grazing floors, where a sub-pixel step
				// slides the point far along the surface) -- and it is taken
				// as the same sample of the same lobe: its direction and
				// distance kept from the moved receiver (the random-replay
				// shift), its weight its own estimate p_h(y) W_h, not
				// re-evaluated here. The temporal chain is then an exact
				// running average of the pixel's own estimates. Re-evaluated
				// (the MIS over two receivers, J), each frame's gain averaged
				// 1.004 with tails to 2300x in the target ratio -- a normal
				// map's normal under the jitter, a near hit's geometry --
				// and the chain, M_h / M_c = 20 deep, compounded it: the
				// game project read +28 % at rest under MetalFX, the lab
				// +19 %, neither without the jitter (section 115).
				identity = same_surface && (params.flags & FLAG_RECONNECT_HISTORY) == 0u;
				if (same_surface && (identity || shift_ok(h, p.pos, p.footprint))) {
					// The change mark: the share of this pixel's light the
					// gather saw change since the reservoirs were drawn.
					float change = clamp(imageLoad(raw_ambient, pixel).a, 0.0, 1.0);
					float cap = params.m_cap;
					if ((params.flags & (FLAG_DUP | FLAG_DUP_HISTORY)) == (FLAG_DUP | FLAG_DUP_HISTORY)) {
						cap = mix(params.m_cap, 1.0, pow(clamp(dup.data[prev_index], 0.0, 1.0), params.dup_alpha));
					}
					// In frames: the cap is c_cap frames of this frame's
					// confidence (the pool's candidates count one frame).
					cap *= max(c.m, 1.0);
					h_m = min(h.m, cap) * (1.0 - change);
				}
			}
		}
	}

	Reservoir r = reservoir_empty();
	r.flags = RES_VALID | (c.flags & RES_SAMPLE_FLAGS);
	r.hit = c.hit;
	r.recv = c.recv;
	r.radiance = c.radiance;
	r.hit_normal = c.hit_normal;
	r.id = c.id;
	r.m = c.m + h_m;
	vec4 paint = vec4(0.0);
	paint.r = c.e;
	if (identity && h_m > 0.0) {
		// The running average: each candidate by its confidence, the
		// history at its own estimate (the target at its own receiver).
		float e_c = c.e;
		float e_h = h.e;
		r.e_rgb = (c.m * c.e_rgb + h_m * h.e_rgb) / (c.m + h_m);
		reservoir_pick(r, c, c.m / (c.m + h_m) * e_c);
		h.hit += p.pos - h.recv; // The same ray from the moved receiver.
		h.recv = p.pos;
		reservoir_pick(r, h, h_m / (c.m + h_m) * e_h);
		paint.g = e_h;
		paint.a = 0.5;
	} else {
		// Beyond a pixel (the camera moved): the history reconnected at its
		// hit, each candidate's MIS denominator over the receivers its
		// sample can be shifted to.
		float c_sum = c.m * area_target(c, c.recv, c.recv_normal) + (h_m > 0.0 && shift_ok(c, h.recv, p.footprint) ? h_m * area_target(c, h.recv, h.recv_normal) : 0.0);
		float w_c = candidate_weight(c, c.m, c_sum, p);
		reservoir_pick(r, c, w_c);
		r.e_rgb = w_c * c.e_rgb / max(c.e, 1e-6);
		if (h_m > 0.0) {
			float h_sum = c.m * area_target(h, c.recv, c.recv_normal) + h_m * area_target(h, h.recv, h.recv_normal);
			float w_h = candidate_weight(h, h_m, h_sum, p);
			reservoir_pick(r, h, w_h);
			r.e_rgb += w_h * h.e_rgb / max(h.e, 1e-6);
			paint.g = h.w_sum > 0.0 ? target(h.radiance, h.recv_normal, (h.flags & RES_DISTANT) != 0u ? normalize(h.hit - h.recv) : normalize(h.hit - h.recv)) * h.w_sum : 0.0;
			paint.a = 1.0;
		}
	}

	if (identity && h_m > 0.0 && dot(p.normal, normalize(r.hit - r.recv)) <= 0.0 && c.w_sum > 0.0) {
		// The history's sample fell below this frame's hemisphere (its
		// normal moved under the jitter): W here would be zero and the
		// running average lost, so the reservoir carries the sum on the
		// pixel's own sample.
		r.hit = c.hit;
		r.recv = c.recv;
		r.radiance = c.radiance;
		r.hit_normal = c.hit_normal;
		r.id = c.id;
		r.flags = RES_VALID | (c.flags & RES_SAMPLE_FLAGS);
	}
	reservoir_finish(r, p);
	if ((params.flags & FLAG_PAINT) != 0u) {
		paint.b = r.e;
		imageStore(raw_ambient, pixel, paint);
	}
	reservoir_anchor(r, p);
	reservoir_store(index, r);
}

#endif

#ifdef RESTIR_SPATIAL

bool visible(Surface p, vec3 dir, float dist) {
	rayQueryEXT rq;
	vec3 origin = p.pos + (p.normal + dir) * gi.ray_bias;
	float t_max = dist - max(gi.ray_bias * 4.0, 0.02 * dist);
	if (t_max <= gi.ray_bias) {
		return true;
	}
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, gi.ray_bias, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// Candidate k of this pixel's spatial set: 0 its own temporal reservoir,
// then the neighbors, a pure function of k (so the MIS sums walk the set
// again rather than hold it): from a Gaussian of sigma = radius (the
// paper's, FLAG_GAUSS) or on a disk by the golden angle. False where the
// candidate is no sample of this surface's lobe.
uint spatial_seed;
bool spatial_candidate(uint k, ivec2 pixel, Surface p, out Reservoir r) {
	r = reservoir_empty();
	ivec2 q = pixel;
	if (k > 0u) {
		float u1 = (float(pcg_hash(spatial_seed + k * 2u) >> 8u) + 0.5) * (1.0 / 16777216.0);
		float u2 = float(pcg_hash(spatial_seed + k * 2u + 1u) >> 8u) * (1.0 / 16777216.0);
		vec2 offset;
		if ((params.flags & FLAG_GAUSS) != 0u) {
			float rr = params.radius * min(sqrt(-2.0 * log(u1)), 3.0);
			offset = vec2(cos(2.0 * M_PI * u2), sin(2.0 * M_PI * u2)) * rr;
		} else {
			float angle0 = float(pcg_hash(spatial_seed) >> 8u) * (2.0 * M_PI / 16777216.0);
			float a = angle0 + float(k) * 2.39996323;
			offset = vec2(cos(a), sin(a)) * params.radius * sqrt((float(k - 1u) + u1) / float(params.neighbors));
		}
		q = pixel + ivec2(round(offset));
		if (q == pixel || any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, gi.screen_size))) {
			return false;
		}
	}
	r = reservoir_load_in(pixel_index(q));
	if ((r.flags & RES_VALID) == 0u) {
		return false;
	}
	if (k > 0u) {
		float tol = params.depth_tolerance * max(p.view_depth, 1.0);
		if (abs(dot(p.normal, r.recv - p.pos)) > tol || dot(p.normal, r.recv_normal) < params.normal_min) {
			return false;
		}
	}
	return true;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= gi.screen_size.x || pixel.y >= gi.screen_size.y) {
		return;
	}
	uint index = pixel_index(pixel);
	Reservoir center = reservoir_load_in(index);
	Surface p;
	if ((center.flags & RES_VALID) == 0u || !surface_at(pixel, p)) {
		reservoir_store(index, center);
		return;
	}
	rng_state = pcg_hash(index ^ pcg_hash(params.frame * 3u + 2u));
	spatial_seed = pcg_hash(rng_state + 0x9E3779B9u);

	Reservoir r = reservoir_empty();
	r.flags = RES_VALID | (center.flags & RES_SAMPLE_FLAGS);
	r.hit = center.hit;
	r.recv = center.recv;
	r.radiance = center.radiance;
	r.hit_normal = center.hit_normal;
	r.id = center.id;
	bool mean_out = (params.flags & FLAG_OUTPUT_MEAN) != 0u;
	bool test_each = mean_out && (params.flags & FLAG_VISIBILITY) != 0u;
	// The neighbors whose samples this receiver cannot see, tested first:
	// such a candidate is dropped whole -- from the confidence and from
	// every MIS denominator -- as if its pixel were not on this surface.
	// Zeroed after the weights instead (the first form), its M stayed in
	// the sums and the game's spatial reuse read 7 % dark (section 115).
	uint occluded = 0u;
	if (test_each) {
		for (uint k = 1u; k <= params.neighbors; k++) {
			Reservoir s;
			vec3 cdir;
			float cjac, cdist;
			if (spatial_candidate(k, pixel, p, s) && s.w_sum > 0.0 && shift_ok(s, p.pos, p.footprint) && reconnect(s, p.pos, cdir, cjac, cdist) && !visible(p, cdir, cdist)) {
				occluded |= 1u << k;
			}
		}
	}
	vec3 mean_sum = vec3(0.0);
	for (uint i = 0u; i <= params.neighbors; i++) {
		Reservoir s;
		if ((occluded & (1u << i)) != 0u || !spatial_candidate(i, pixel, p, s)) {
			continue;
		}
		r.m += s.m;
		// A neighbour's sample reaches this receiver only by reconnection.
		if (!(s.w_sum > 0.0) || (i > 0u && !shift_ok(s, p.pos, p.footprint))) {
			continue;
		}
		float mis_sum = 0.0;
		for (uint j = 0u; j <= params.neighbors; j++) {
			Reservoir t;
			if (j == i) {
				t = s;
			} else if ((occluded & (1u << j)) != 0u || !spatial_candidate(j, pixel, p, t)) {
				continue;
			}
			if (j == i || shift_ok(s, t.recv, p.footprint)) {
				mis_sum += t.m * area_target(s, t.recv, t.recv_normal);
			}
		}
		float w = candidate_weight(s, s.m, mis_sum, p);
		if (w > 0.0) {
			vec3 cdir;
			float cjac, cdist;
			bool cok = reconnect(s, p.pos, cdir, cjac, cdist);
			// The candidate's colour per unit of weight: its own estimate's
			// (the chain's average), not its one sample's.
			mean_sum += cok ? w * (s.e > 1e-6 ? s.e_rgb / s.e : contribution_per_weight(s.radiance, p, cdir)) : vec3(0.0);
		}
		if (reservoir_pick(r, s, w)) {
			r.flags = i > 0u ? (r.flags | RES_NEIGHBOUR) : (r.flags & ~RES_NEIGHBOUR);
		}
	}
	reservoir_finish(r, p);

	// A neighbor's sample was seen from the neighbor, not from here.
	vec3 dir;
	float jacobian, dist;
	bool ok = reconnect(r, p.pos, dir, jacobian, dist);
	if (ok && !test_each && (r.flags & RES_NEIGHBOUR) != 0u && (params.flags & FLAG_VISIBILITY) != 0u && !visible(p, dir, dist)) {
		r.w_sum = 0.0;
	}
	vec3 estimate = ok ? contribution_per_weight(r.radiance, p, dir) * target(r.radiance, p.normal, dir) * r.w_sum : vec3(0.0);
	if (mean_out) {
		estimate = mean_sum;
	}
	if (any(isnan(estimate)) || any(isinf(estimate))) {
		estimate = vec3(0.0);
	}

	// The pixel's own rays' mean (the static share under FLAG_DYN_SPLIT):
	// what the raw buffer holds of them.
	uint slots = gi.ray_count + 1u;
	uint own_base = index * slots;
	uvec4 own0 = rays.data[own_base];
	uint own_rays = ((own0.w >> RAY_COUNT_SHIFT) & 3u) + 1u;
	vec3 own = vec3(0.0);
	for (uint k = 0u; k < own_rays; k++) {
		uvec4 rec = k == 0u ? own0 : rays.data[own_base + k];
		vec3 radiance = max(vec3(unpackHalf2x16(rec.x), unpackHalf2x16(rec.y).x), vec3(0.0));
		if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
			radiance *= 1.0 - float((rec.w >> RAY_DYN_SHIFT) & 255u) / 255.0;
		}
		own += radiance;
	}
	own /= float(own_rays);
	if (any(isnan(own)) || any(isinf(own))) {
		own = vec3(0.0);
	}
	if ((params.flags & FLAG_OUTPUT_RAW) != 0u) {
		estimate = own;
	}

	if ((params.flags & FLAG_PAINT) != 0u) {
		r.flags &= ~RES_NEIGHBOUR;
		reservoir_anchor(r, p);
		reservoir_store(index, r);
		return;
	}
	vec4 a = imageLoad(raw_ambient, pixel);
	vec4 d = imageLoad(raw_directional, pixel);
	float dyn_lum = (params.flags & FLAG_DYN_SPLIT) != 0u ? luminance(imageLoad(raw_ambient_dyn, pixel).rgb) : 0.0;
	float old_lum = luminance(a.rgb) + dyn_lum;
	vec3 new_a = max(a.rgb + estimate - own, vec3(0.0));
	float new_lum = luminance(new_a) + dyn_lum;
	imageStore(raw_ambient, pixel, vec4(new_a, a.a));
	imageStore(raw_directional, pixel, vec4(old_lum > 1e-5 ? d.xyz * (new_lum / old_lum) : vec3(0.0), d.w));

	// Next frame's history (GODOT_GI_RESTIR=tsf).
	r.e_rgb = mean_out ? mean_sum : estimate;
	r.flags &= ~RES_NEIGHBOUR;
	reservoir_anchor(r, p);
	reservoir_store(index, r);
}

#endif

#ifdef RESTIR_DUP

// The share of the window around this pixel holding its sample (the
// paper's 17 x 17 at full resolution, 288 others).
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= gi.screen_size.x || pixel.y >= gi.screen_size.y) {
		return;
	}
	uint index = pixel_index(pixel);
	uvec4 c0 = res_in.data[index * 4u + 2u];
	uint id = c0.w >> 8u;
	if ((c0.w & RES_VALID) == 0u || id == 0u) {
		dup.data[index] = 0.0;
		return;
	}
	int r = params.dup_radius;
	uint same = 0u;
	for (int y = -r; y <= r; y++) {
		for (int x = -r; x <= r; x++) {
			ivec2 q = pixel + ivec2(x, y);
			if ((x == 0 && y == 0) || any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, gi.screen_size))) {
				continue;
			}
			uint w = res_in.data[pixel_index(q) * 4u + 2u].w;
			same += ((w & RES_VALID) != 0u && (w >> 8u) == id) ? 1u : 0u;
		}
	}
	dup.data[index] = float(same) / float((2 * r + 1) * (2 * r + 1) - 1);
}

#endif
