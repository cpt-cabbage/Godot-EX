#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : enable

// Stochastic direct lighting.
// Per pixel: weighted reservoir sampling over a candidate set built from the
// previous frame's visible light list (guided) and a strided subset of the
// clustered light grid cell (discovery), one ray-query visibility ray per
// unique selected sample. The outputs are fully demodulated: the noisy part
// of the shading is a bounded [0;1] visibility ratio per signal (what the
// denoiser filters), while the analytic unshadowed lighting (diffuse without
// albedo, full specular) goes into its own buffers and is multiplied back in
// after denoising, so lighting detail never passes through the filter.

#include "../normal_roughness_inc.glsl"
#include "../albedo_f0_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"

// Compute shaders cannot use implicit-LOD sampling; the LTC helpers only
// sample mipless LUTs through texture(), so base level is exact.
#define texture(s, uv) textureLod(s, uv, 0.0)
#include "../area_lights_inc.glsl"
#undef texture

// Light-type permutation (MegaLights' tile classification, reduced to the one
// axis that pays here): the LTC area-light paths cost registers in every
// pixel whether or not the frame has an area light, and this pass is
// occupancy-bound. Raytracing builds one pipeline per value and picks
// by the frame's area light count, so a scene without area lights never
// carries the code at all. Every area branch below tests this first.
layout(constant_id = 0) const bool sc_has_area_lights = true;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 3, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;
layout(set = 0, binding = 4, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

// Visible light list from the previous frame, one fixed-size list per 8x8 tile.
layout(set = 0, binding = 5, std430) restrict readonly buffer LightList {
	uint data[];
}
prev_light_list;

#define MAX_MIRROR_PLANES 4u
struct MirrorPlane {
	vec4 plane;
	vec4 params;
	vec4 center;
	vec4 u_axis;
	vec4 v_axis;
};

layout(set = 0, binding = 6, std140) uniform Params {
	mat4 view_from_ndc; // Inverse of the (depth-corrected) projection.
	mat4 ndc_from_view; // The (depth-corrected) projection, for screen traces.
	mat4 world_from_view; // Camera transform.
	mat4 reproject; // Current NDC -> previous frame NDC, for the tile lookup.
	ivec2 screen_size;
	uint omni_light_count;
	uint spot_light_count;
	uint frame_index;
	float ray_bias;
	int tiles_x;
	int tiles_y;
	uint cluster_shift;
	uint cluster_width;
	uint max_cluster_element_count_div_32;
	uint cluster_type_size;
	float z_far;
	uint area_light_count;
	ivec2 full_screen_size;
	// 2 when sampling at half resolution (screen_size is then the half size
	// and every depth / normal / cluster lookup scales up to full pixels).
	uint depth_scale;
	uint reservoir_count; // Rays per pixel, 1..MAX_RESERVOIRS.
	uint flags; // FLAG_*.
	float cluster_z0; // Nonzero: exponential depth slices from this depth (see ClusterBuilderRD).
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
	// A planar mirror (GODOT_GI_MIRROR, plan section 43, a prototype), in
	// view space: xyz its normal, w its offset (n . p = w; a zero normal is
	// off), and x its F0. Every local light then has an image entry beside
	// it (IMAGE_BIT): the light evaluated at the mirrored point with the
	// mirrored normal and view vector, times the Fresnel at the crossing.
	MirrorPlane mirrors[MAX_MIRROR_PLANES]; // The scene's planar mirrors (mirror_planes_inc.glsl), view space.
	uint mirror_count;
	uint mirror_order; // The longest image chain evaluated (1: single images, 2: pairs too).
	uint mirror_pad1;
	uint mirror_pad2;
}
params;

#include "mirror_planes_inc.glsl"

#define FLAG_LIGHT_GUIDING 1u
#define FLAG_SCREEN_TRACES 2u
#define FLAG_ALPHA_CASTERS 4u // Some TLAS instances are non-opaque: their hits are candidates, confirmed from the cards' coverage.
#define FLAG_GBUF 8u // Weigh the candidates with the pixel's material (GODOT_RT_GBUF=0 reverts to a 4% dielectric with unit albedo).

// The froxel light grid built by clustered forward culling. Same layout as the
// scene shader: per cell, per light type, max_cluster_element_count_div_32
// bitmask words followed by 32 packed z-slice min/max element ranges.
layout(set = 0, binding = 7, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

// Spatio-temporal blue noise, one 64x64 RG slice per frame over a 16 frame
// cycle. Blue in space (neighboring pixels make maximally different random
// decisions, which the spatial filter averages into smooth gradients instead
// of blotches) and blue in time (each pixel's sequence over frames converges
// faster under temporal accumulation than white noise would).
layout(set = 0, binding = 8) uniform sampler2DArray stbn_texture;

layout(set = 0, binding = 9, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;

// LTC lookup tables and the textured-area-light atlas, shared with the scene
// shader's analytic area light path.
layout(set = 0, binding = 10) uniform texture2D ltc_lut1;
layout(set = 0, binding = 11) uniform texture2D ltc_lut2;
layout(set = 0, binding = 12) uniform texture2D area_light_atlas;
layout(set = 0, binding = 13) uniform sampler material_sampler;
// Projector textures for omni/spot lights (same atlas as the scene shader).
layout(set = 0, binding = 14) uniform texture2D decal_atlas_srgb;

// The surface cache's tables, for the coverage of alpha-tested casters.
#include "surface_cache_inc.glsl"
layout(set = 0, binding = 15, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;
layout(set = 0, binding = 16, std430) restrict readonly buffer CardSets {
	CardSet data[];
}
card_sets;
layout(set = 0, binding = 17) uniform texture2D card_depth_atlas;
layout(set = 0, binding = 18) uniform texture2D card_albedo_atlas;
// The prepass G-buffer (albedo_f0_inc.glsl): the pixel's own material, for
// the light selection to weigh the diffuse and specular terms the way the
// scene shader will shade them (FLAG_GBUF).
layout(set = 0, binding = 19) uniform sampler2D gbuf_albedo_texture;
layout(set = 0, binding = 20) uniform sampler2D gbuf_f0_texture;

layout(set = 1, binding = 0, r11f_g11f_b10f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, r11f_g11f_b10f) uniform restrict writeonly image2D out_specular;
// One light this pixel found visible, gathered next frame into the tile lists.
layout(set = 1, binding = 2, r32ui) uniform restrict writeonly uimage2D out_visible_light;
// Shading confidence, consumed by the denoiser.
layout(set = 1, binding = 3, r8) uniform restrict writeonly image2D out_meta;
// View depth of the lit texel, for the half-resolution upsample.
layout(set = 1, binding = 4, r16f) uniform restrict writeonly image2D out_view_depth;
// Unshadowed analytic lighting, multiplied back into the denoised visibility
// ratios at the end of the denoiser's spatial pass.
layout(set = 1, binding = 5, r11f_g11f_b10f) uniform restrict writeonly image2D out_analytic_diffuse;
// rgb: the specular lobe without Fresnel, a: the Schlick weight (see light_eval).
layout(set = 1, binding = 6, rgba16f) uniform restrict writeonly image2D out_analytic_specular;

#define MAX_RESERVOIRS 4u
#define TILE_SIZE 8
#define LIST_SIZE 8
#define INVALID_LIGHT 0xFFFFFFFFu
// Entry encoding: bit 31 spot, bit 30 area, bits 26..29 payload, bits 0..25
// the per-type light index. The payload is a 2x2 rect visibility bitmask for
// area lights (quadrant order (-u,-v),(+u,-v),(-u,+v),(+u,+v)) and a 4-bit
// quantized visibility ratio for omni/spot lights, so guiding can down-weight
// lights the tile found mostly shadowed (STB lighting's shadow-in-PDF idea,
// evaluated one frame late for free).
#define SPOT_BIT 0x80000000u
#define AREA_BIT 0x40000000u
#define QUAD_MASK_SHIFT 26u
#define QUAD_MASK_BITS (0xFu << QUAD_MASK_SHIFT)
#define IMAGE_BIT 0x02000000u // The light's image through a planar mirror (mirror_planes_inc.glsl); bits 23..24 name the mirror nearest the pixel.
#define IMAGE_PLANE_SHIFT 23u
#define IMAGE_PLANE_MASK (0x3u << IMAGE_PLANE_SHIFT)
#define IMAGE2_BIT 0x00400000u // A second-order image: bits 20..21 name the next mirror toward the light (mirror_chain).
#define IMAGE2_PLANE_SHIFT 20u
#define IMAGE2_PLANE_MASK (0x3u << IMAGE2_PLANE_SHIFT)
#define IMAGE3_BIT 0x00080000u // A third-order image: bits 17..18 name the mirror nearest the light.
#define IMAGE3_PLANE_SHIFT 17u
#define IMAGE3_PLANE_MASK (0x3u << IMAGE3_PLANE_SHIFT)
#define ENTRY_ID_MASK 0x0001FFFFu
#define ENTRY_KEY_MASK (SPOT_BIT | AREA_BIT | IMAGE_BIT | IMAGE_PLANE_MASK | IMAGE2_BIT | IMAGE2_PLANE_MASK | IMAGE3_BIT | IMAGE3_PLANE_MASK | ENTRY_ID_MASK)
#define ENTRY_MIRROR(e) (((e) & IMAGE_PLANE_MASK) >> IMAGE_PLANE_SHIFT)
#define ENTRY_MIRROR2(e) ((((e) & IMAGE2_BIT) != 0u) ? (((e) & IMAGE2_PLANE_MASK) >> IMAGE2_PLANE_SHIFT) : MAX_MIRROR_PLANES)
#define ENTRY_MIRROR3(e) ((((e) & IMAGE3_BIT) != 0u) ? (((e) & IMAGE3_PLANE_MASK) >> IMAGE3_PLANE_SHIFT) : MAX_MIRROR_PLANES)
// Candidate set: the guided (visible list) candidates plus a strided subset of
// the cluster cell. Keeping the per-pixel candidate count fixed is what makes
// the cost independent of the total light count (MegaLights' central promise);
// the stride multiplier keeps the subset an unbiased representation.
// Slack on the per-reservoir firefly bound, as a multiple of a reservoir's
// natural share of the cell. See the bound itself for why it cannot be 1.
#define FIREFLY_HEADROOM 8.0

#define MAX_GUIDED_CANDIDATES 8u
#define MAX_DISCOVERY_CANDIDATES 12u

// The analytic (unshadowed) term bypasses the denoiser entirely -- it is the
// factor the filtered visibility ratio is multiplied back into, and the whole
// demodulation only holds if it is exact. It is also the ratio's own
// denominator (see the estimator below), which is what makes every sampling
// weight in this pass cancel: the numerator estimates the cell's shadowed sum
// and this is the cell's unshadowed sum, so the proposal appears in one and
// not the other only if the two describe different populations. They must
// describe the same one. It used to be built from the same
// strided subset that feeds the candidate set, scaled by the stride: unbiased
// in expectation, but every non-guided light was then a Bernoulli(1/stride)
// sample, all of them sharing one per-pixel phase, so a pixel's analytic value
// was one of only `stride` discrete levels. Unfiltered and re-rolled every
// frame, that read as grain on the floor, hard splotches on the walls, and
// boiling under camera motion, and no amount of denoising could touch it
// because the denoiser never sees this buffer.
//
// So the stride now selects candidates only. Up to this many lights the cell
// is summed exactly; the evaluations are shared with the candidate set, so a
// cell at the cap costs its own size in entry_eval calls rather than the ~17
// the candidate budget already pays. That is ALU, not registers.
//
// The cap sits at the cluster builder's own default element limit, which makes
// the sum exact for every scene it can represent, because the strided estimate
// past it is far more expensive than the evaluations it saves. It is the one
// noisy term the denoiser never sees -- it is multiplied into the ratio after
// filtering -- so its grain lands in the image whole, and it does not average
// out over frames either. Measured on stochastic_demo at 1440p, packing the
// scene until cells overflow:
//
//   200 lights   cap 64: 0.05743 noise, 73.2ms   exact: 0.00428, 73.3ms
//   500 lights   cap 64: 0.10096 noise, 142.8ms  exact: 0.00357, 148.0ms
//
// Evaluating every light in the cell costs 3.6% of the frame at 500 lights and
// removes 28x the noise. Only a project raising max_clustered_elements past
// this falls back to the estimate.
#define MAX_ANALYTIC_LIGHTS 512u

// r11f_g11f_b10f saturates near 65024, and an over-range value stores as +Inf.
// The denoiser multiplies this buffer into the ratio, so a fully shadowed
// pixel next to an over-range one becomes 0 * Inf = NaN: a black hole inside a
// blown highlight. Bound well inside the format instead.
#define ANALYTIC_MAX 32768.0

// GGX D peaks at 1/(pi * alpha^2), which the alpha floor below puts at ~3.2e5
// before the light color is even applied. Upstream's own D_GGX ends in
// saturateHalf() for exactly this reason (scene_forward_lights_inc.glsl); this
// copy had dropped the bound.
#define D_GGX_MAX 65504.0

// On a guiding miss (the pixel's history reprojected off screen or behind the
// camera) the tile list is borrowed from elsewhere on screen and is only a
// guess, so the paper shifts the sample budget from exploiting it to
// discovering what is actually there: "we still reuse closest tile, as likely
// it has some useful lights, but we also increase the ratio of hidden light
// samples in order to speedup new visible light discovery".
//
// The split moves but the total does not. Candidate evaluation, not tracing,
// dominates this pass (~17 candidates are evaluated on a typical pixel) and
// it is register-bound, so widening discovery outright would cost every
// pixel to fix a band at the frame edge. Re-splitting the same ~20
// evaluations is free: the miss path evaluates as many lights as the hit
// path. (The candidates are streamed into the reservoirs as they are
// evaluated and never stored, so the budgets bound evaluations, not slots.)
#define MISS_GUIDED_CANDIDATES 4u
#define MISS_DISCOVERY_CANDIDATES 16u

// Hidden lights (not on the visible list) are clamped to this share of the
// total sampling weight, relaxed when the visible lights are dim (paper's
// hidden light budget). Weight clamping keeps the RIS estimator unbiased.
// The paper raises the share to 50% on a reprojection miss.
#define HIDDEN_WEIGHT_BUDGET 0.2
#define MISS_HIDDEN_WEIGHT_BUDGET 0.5
#define DIM_VISIBLE_WEIGHT 0.25

// Samples whose unshadowed contribution is below this fraction of the pixel's
// total unshadowed luminance are dropped without tracing a ray (the paper's
// exposure-relative sample culling; we express the threshold relative to the
// pixel's own lighting since the pass runs before exposure).
#define CULL_CONTRIBUTION_FRACTION 0.002

// PCG hash: decorrelates the per-pixel random variables. A screen-space
// gradient pattern makes neighboring pixels select the same lights, which
// spatial filtering turns into blotches rather than smooth gradients.
uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
	return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

// STBN lookup for one random stream. Different streams (reservoir chains, the
// tile jitter) use toroidal shifts of the same slice: a shifted blue noise
// pattern stays blue, while staying decorrelated from the other streams. The
// 16 frame cycle is decorrelated across epochs the same way.
//
// The 64x64 pattern would otherwise repeat across the screen (a 1080p frame
// tiles it 17x30 times), making distant pixels with the same phase take the
// same decisions -- a faint repeating structure that also correlates the
// spatial filter's inputs. Each 64x64 screen tile therefore offsets the
// lookup by its own hashed amount; a constant offset preserves the blue
// spectrum within the tile.
vec2 stbn_sample(ivec2 pixel, uint stream) {
	uint epoch = params.frame_index >> 4;
	uint k = stream + epoch * 8u;
	// R2 low-discrepancy sequence for the shift.
	ivec2 shift = ivec2(fract(vec2(k) * vec2(0.7548776662, 0.5698402909)) * 64.0);
	uint tile_hash = pcg_hash(uint(pixel.x >> 6) ^ (uint(pixel.y >> 6) * 0x9E3779B9u));
	ivec2 tile_shift = ivec2(tile_hash & 63u, (tile_hash >> 6) & 63u);
	ivec2 p = (pixel + shift + tile_shift) & 63;
	return texelFetch(stbn_texture, ivec3(p, int(params.frame_index & 15u)), 0).rg;
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return nd * pow(max(dist, 0.0001), -decay);
}

struct Reservoir {
	uint entry; // The selected light's list entry; INVALID_LIGHT = none.
	float weight_sum;
	float selected_weight;
	float lum; // The selected light's unshadowed luminance, for culling.
};

// Streaming weighted reservoir sampling, warping the random variable back to
// [0;1) after each decision so one variable drives the whole loop.
//
// The candidates are never stored: each light is offered to the reservoirs as
// it is evaluated and forgotten. The pass used to fill three arrays of twenty
// (entry, weight, luminance) and run the reservoirs over them afterwards,
// which is the same draw at the cost of sixty dynamically indexed dwords in
// thread-private memory, written once and read back by every reservoir.
// What the arrays existed for -- the hidden-light budget, a uniform scale on
// the discovery weights known only once all of them are summed -- is done by
// keeping the guided and discovery candidates in reservoirs of their own and
// merging the pair with the scale applied to the discovery side's total: a
// uniform scale changes nothing about which discovery light won, only how
// the two sides weigh against each other, so the merged draw is the same
// distribution the single scaled chain gave.
void reservoir_update(inout Reservoir r, uint entry, float w, float lum, inout float rng) {
	if (w <= 0.0) {
		return;
	}
	r.weight_sum += w;
	float p = w / r.weight_sum;
	if (rng < p) {
		r.entry = entry;
		r.selected_weight = w;
		r.lum = lum;
		rng = rng / p;
	} else {
		rng = (rng - p) / (1.0 - p);
	}
	rng = clamp(rng, 0.0, 0.9999999);
}

void reservoir_init(inout Reservoir r) {
	r.entry = INVALID_LIGHT;
	r.weight_sum = 0.0;
	r.selected_weight = 0.0;
	r.lum = 0.0;
}

// Merges the discovery reservoir into the guided one, its weights scaled by
// p_scale (the hidden-light budget). Returns true when the discovery side won.
bool reservoir_merge(inout Reservoir g, Reservoir h, float p_scale, inout float rng) {
	float hw = h.weight_sum * p_scale;
	if (hw <= 0.0 || h.entry == INVALID_LIGHT) {
		return false;
	}
	float total = g.weight_sum + hw;
	float p = hw / total;
	bool take = rng < p;
	if (take) {
		g.entry = h.entry;
		g.selected_weight = h.selected_weight * p_scale;
		g.lum = h.lum;
		rng = rng / p;
	} else {
		rng = (rng - p) / (1.0 - p);
	}
	g.weight_sum = total;
	rng = clamp(rng, 0.0, 0.9999999);
	return take;
}

float luminance(vec3 c) {
	return dot(c, params.luma_weights.rgb);
}

// The pixel's material for the light selection (FLAG_GBUF), set once in
// main: the diffuse albedo's luminance and the Fresnel endpoints the scene
// shader reassembles the specular with (F = f0 + (f90 - f0) * fc). Without
// the G-buffer they stand at unit albedo and a 4% dielectric, the target
// function the pass had before it existed.
float mat_albedo_lum = 1.0;
vec3 mat_f0 = vec3(0.04);
float mat_f90 = 1.0;

// The luminance a candidate's lighting would have on this pixel: the
// diffuse radiance through the albedo, the specular lobe through the
// material's Fresnel. This is the reservoirs' target function; the ratio
// estimator divides the same weight out again, so any positive choice is
// unbiased and this one matches what the scene shader composites.
float candidate_luminance(vec3 f, vec4 ss) {
	vec3 spec = ss.rgb * (mat_f0 + (mat_f90 - mat_f0) * ss.a);
	return abs(luminance(f) * mat_albedo_lum + luminance(spec));
}

// Selection weight. MegaLights compresses this perceptually (log2(lum + 1)) to
// tame very strong lights, which is the right call when the estimator's output
// is radiance. Ours is a visibility ratio against a fixed analytic denominator,
// and there the compression is what makes it boil.
//
// Write the candidate weight as w_i = L_i * m_i, with m_i the inverse
// probability the candidate was offered (the stride). Then the sum of the
// weights is the cell's own analytic luminance, the estimator's
// weight_sum / selected_weight * m_c collapses to that same constant, and the
// pixel's ratio is the plain mean of the visibilities its rays measured -- the
// weights carry no variance at all, only the binary hit/miss does. Compress the
// weight and that cancellation breaks: L_c / w_c then ranges over two orders of
// magnitude across a set like a bright window plus two dozen dim fills, so
// which light the reservoir happened to draw moves the pixel's value far more
// than whether that light was actually occluded. Re-drawn every frame, that is
// the boil.
//
// It stayed invisible while the denominator was a second estimate from the same
// draw, because then the compression divided straight back out (see the ratio
// estimator). It was never doing the job its name claims; it was cancelling.
float light_weight(float lum) {
	return lum;
}

// Bounds an analytic buffer to what its storage format can hold. Scales by the
// max channel rather than clipping each one, so bounding a spike desaturates
// it toward the format ceiling instead of shifting its hue toward white. Also
// the last place a non-finite value can be stopped before it reaches the
// denoiser, where it would latch into the history permanently.
vec3 bound_analytic(vec3 c) {
	c = max(c, vec3(0.0));
	float l = max(max(c.r, c.g), c.b);
	if (!(l < 1e30)) { // False for NaN as well as Inf.
		return vec3(0.0);
	}
	return l > ANALYTIC_MAX ? c * (ANALYTIC_MAX / l) : c;
}

// Unshadowed diffuse (radiance, no albedo) and specular contribution of a
// light at a view-space point. Zero when out of range or facing away.
void light_eval_at(bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, vec3 v, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split, out vec3 light_rel_vec) {
	spec_split = vec4(0.0);
	LightData ld = is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
	light_rel_vec = ld.position - view_pos;
	float light_length = length(light_rel_vec);
	float attenuation = get_omni_attenuation(light_length, ld.inv_radius, ld.attenuation);
	if (is_spot) {
		float scos = max(dot(-normalize(light_rel_vec), ld.direction), ld.cone_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
		attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
	}

	// Projector texture, same mapping as the scene shader's analytic path
	// (spot: perspective projection through the shadow matrix; omni: dual
	// paraboloid). As there, the matrix is only valid while the light has
	// shadows enabled.
	vec3 color = ld.color;
	if (ld.projector_rect != vec4(0.0)) {
		if (is_spot) {
			vec4 splane = ld.shadow_matrix * vec4(view_pos, 1.0);
			splane /= splane.w;
			vec2 proj_uv = splane.xy * ld.projector_rect.zw;
			vec4 proj = textureLod(sampler2D(decal_atlas_srgb, material_sampler), proj_uv + ld.projector_rect.xy, 0.0);
			color *= proj.rgb * proj.a;
		} else {
			vec3 local_v = normalize((ld.shadow_matrix * vec4(view_pos, 1.0)).xyz);
			vec4 atlas_rect = ld.projector_rect;
			if (local_v.z >= 0.0) {
				atlas_rect.y += atlas_rect.w;
			}
			local_v.z = 1.0 + abs(local_v.z);
			local_v.xy /= local_v.z;
			local_v.xy = local_v.xy * 0.5 + 0.5;
			vec2 proj_uv = local_v.xy * atlas_rect.zw;
			vec4 proj = textureLod(sampler2D(decal_atlas_srgb, material_sampler), proj_uv + atlas_rect.xy, 0.0);
			color *= proj.rgb * proj.a;
		}
	}

	vec3 l = normalize(light_rel_vec);

	// Light size turns the point into a spherical area light: the same size_A
	// offset the analytic light_compute applies widens the diffuse terminator.
	float size_A = 0.0;
	if (ld.size > 0.0) {
		float t = ld.size / max(0.001, light_length);
		size_A = max(0.0, 1.0 - 1.0 / sqrt(1.0 + t * t));
	}

	float ndotl = clamp(size_A + dot(view_normal, l), 0.0, 1.0);

	vec3 h = normalize(v + l);
	float ndotv = max(dot(view_normal, v), 1e-4);
	// The size term folded into every cosine, as light_compute does with its
	// A parameter. This is what the scene shader renders for the same light
	// through the clustered path, and the composite multiplies this pass's
	// lobe by that shader's own Fresnel, so the two have to agree on the
	// lobe. (Widening alpha by the subtended angle and renormalising, Karis'
	// representative point, was tried here: it is the better sphere-light
	// model, and it read as the stochastic path losing half the highlight of
	// every sized lamp against the rest of the engine.)
	float ndoth = clamp(size_A + dot(view_normal, h), 0.0, 1.0);
	float ldoth = clamp(size_A + dot(l, h), 0.0, 1.0);

	// Burley diffuse, the scene shader's default (DIFFUSE_BURLEY in
	// scene_forward_lights_inc.glsl). Lambert alone under-lit rough surfaces
	// at grazing incidence by up to half -- exactly the band a lamp hanging
	// near a wall or ceiling lights -- and the gather then bounced the
	// shortfall around the room. Materials on the Lambert or wrap modes
	// still get this term; the prepass cannot tell them apart.
	float fd90_minus_1 = 2.0 * ldoth * ldoth * roughness - 0.5;
	float fd_v = 1.0 + fd90_minus_1 * pow(1.0 - ndotv, 5.0);
	float fd_l = 1.0 + fd90_minus_1 * pow(1.0 - ndotl, 5.0);
	diffuse = color * (ndotl * (1.0 / M_PI) * fd_v * fd_l * attenuation);

	// Schlick-GGX, dielectric F0. The prepass has no albedo/metallic, so the
	// specular is an approximation the composite cannot recover exactly.

	// Godot's D_GGX and V_GGX (scene_forward_lights_inc.glsl), term for term,
	// with the same half-float ceiling. V is the height-correlated Smith
	// visibility and includes the 1 / (4 NdotL NdotV) of the microfacet BRDF;
	// the separable Schlick-GGX it replaced fell off quadratically at grazing
	// incidence where this falls off linearly, which halved the specular tail
	// of rough metals away from the highlight.
	float alpha = roughness * roughness;
	float ndoth_alpha = ndoth * alpha;
	float ggx_k = alpha / max(1.0 - ndoth * ndoth + ndoth_alpha * ndoth_alpha, 1e-8);
	float D = min(ggx_k * ggx_k * (1.0 / M_PI), D_GGX_MAX);
	float V = min(0.5 / max(mix(2.0 * ndotl * ndotv, ndotl + ndotv, alpha), 1e-8), D_GGX_MAX);
	// The prepass has no albedo or metallic, so the Fresnel term cannot be
	// evaluated here. spec_split carries the lobe without it (rgb) and the
	// Schlick weight (1 - LdotH)^5 (a); the scene shader, which knows the
	// material's f0 and f90, reassembles F = f0 + (f90 - f0) * a there. The
	// dielectric estimate below only feeds sampling weights and the ratio.
	vec3 spec_base = color * attenuation * ndotl * D * V * ld.specular_amount;
	float fc = pow(1.0 - ldoth, 5.0);
	spec_split = vec4(spec_base, fc);
	const float f0 = 0.04;
	specular = spec_base * (f0 + (1.0 - f0) * fc);
}

void light_eval(bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split, out vec3 light_rel_vec) {
	light_eval_at(is_spot, idx, view_pos, view_normal, normalize(-view_pos), roughness, diffuse, specular, spec_split, light_rel_vec);
}

// A light's image through a planar mirror at a point: the light itself at
// the mirrored point with the mirrored normal and view vector (the
// attenuation, the cone, the cookie and the lobes come along), times the
// Fresnel at the crossing. Nothing when the point or the light is behind
// the mirror, when the light's range cannot reach the point from its
// image, or when the crossing misses the mirror's rectangle.
void light_eval_image(uint mi, uint mj, uint mk, bool is_spot, uint idx, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split) {
	diffuse = vec3(0.0);
	specular = vec3(0.0);
	spec_split = vec4(0.0);
	vec3 light_pos = is_spot ? spot_lights.data[idx].position : omni_lights.data[idx].position;
	float inv_radius = is_spot ? spot_lights.data[idx].inv_radius : omni_lights.data[idx].inv_radius;
	vec3 img, p_img, n_img, c1, c2, c3;
	float f;
	if (!mirror_chain(mi, mj, mk, view_pos, view_normal, light_pos, inv_radius, img, p_img, n_img, f, c1, c2, c3)) {
		return;
	}
	vec3 v_img = mirror_dir(mi, normalize(-view_pos));
	if (mj < MAX_MIRROR_PLANES) {
		v_img = mirror_dir(mj, v_img);
	}
	if (mk < MAX_MIRROR_PLANES) {
		v_img = mirror_dir(mk, v_img);
	}
	vec3 rel_unused;
	light_eval_at(is_spot, idx, p_img, n_img, v_img, roughness, diffuse, specular, spec_split, rel_unused);
	diffuse *= f;
	specular *= f;
	spec_split.rgb *= f;
}

// Unshadowed LTC diffuse and specular contribution of an area light, the
// analytic core of the scene shader's light_process_area (clearcoat,
// transmittance and material-dependent terms cannot apply here: the prepass
// carries only normal and roughness, so the specular assumes a dielectric).
void area_light_eval_at(uint idx, vec3 view_pos, vec3 view_normal, vec3 eye_vec, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split) {
	diffuse = vec3(0.0);
	specular = vec3(0.0);
	spec_split = vec4(0.0);
	LightData ld = area_lights.data[idx];
	vec3 area_width = ld.area_width;
	vec3 area_height = ld.area_height;
	if (dot(area_width, area_width) < 1e-7 || dot(area_height, area_height) < 1e-7) {
		return;
	}
	if (dot(ld.direction, view_pos - ld.position) <= 0.0) {
		return; // Point is behind the light.
	}

	// Attenuation from the closest point on the rect; the LTC integral already
	// falls off with inverse-square solid angle, so that part is compensated.
	vec3 light_to_vert = view_pos - ld.position;
	vec3 a_dir = normalize(area_width);
	vec3 b_dir = normalize(area_height);
	float a_half = length(area_width) * 0.5;
	float b_half = length(area_height) * 0.5;
	vec3 pos_local = vec3(dot(light_to_vert, a_dir), dot(light_to_vert, b_dir), dot(light_to_vert, -ld.direction));
	vec3 closest_local = vec3(clamp(pos_local.x, -a_half, a_half), clamp(pos_local.y, -b_half, b_half), 0.0);
	float dist = length(closest_local - pos_local);
	float att_raw = get_omni_attenuation(dist, ld.inv_radius, ld.attenuation);
	float att_ltc = att_raw * dist * dist;
	if (att_ltc <= 0.0) {
		return;
	}

	vec3 points[4];
	vec3 hw = area_width * 0.5;
	vec3 hh = area_height * 0.5;
	points[0] = ld.position - hw - hh - view_pos;
	points[1] = ld.position + hw - hh - view_pos;
	points[2] = ld.position + hw + hh - view_pos;
	points[3] = ld.position - hw + hh - view_pos;

	float max_mipmap = ld.cone_angle;

	float ltc_diffuse = 0.0;
	vec3 diffuse_tex_color = vec3(1.0);
	ltc_evaluate(view_normal, eye_vec, mat3(1.0), points, ld.projector_rect, max_mipmap, area_light_atlas, material_sampler, ltc_diffuse, diffuse_tex_color);
	diffuse = ltc_diffuse * diffuse_tex_color * ld.color * att_ltc;

	float ltc_specular = 0.0;
	vec2 ltc_fresnel = vec2(0.0);
	vec3 specular_tex_color = vec3(1.0);
	ltc_evaluate_specular(view_normal, eye_vec, roughness, points, ld.projector_rect, max_mipmap, area_light_atlas, material_sampler, material_sampler, ltc_lut1, ltc_lut2, ltc_specular, ltc_fresnel, specular_tex_color);
	// LTC gives the Fresnel as two coefficients, F = f0 * x + (f90 - f0) * y.
	// Same split as light_eval: rgb carries the f0 coefficient, a the ratio
	// y / x the scene shader multiplies (f90 - f0) by.
	float fx = max(ltc_fresnel.x, 0.0);
	float fy = max(ltc_fresnel.y, 0.0);
	vec3 spec_base = ltc_specular * specular_tex_color * ld.color * att_ltc * ld.specular_amount;
	spec_split = vec4(spec_base * fx, fx > 1e-6 ? clamp(fy / fx, 0.0, 1.0) : 0.0);
	const float f0 = 0.04;
	specular = spec_base * (f0 * fx + (1.0 - f0) * fy);
}

void area_light_eval(uint idx, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split) {
	area_light_eval_at(idx, view_pos, view_normal, normalize(-view_pos), roughness, diffuse, specular, spec_split);
}

// An area light's image through a planar mirror (see light_eval_image).
void area_light_eval_image(uint mi, uint mj, uint mk, uint idx, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split) {
	diffuse = vec3(0.0);
	specular = vec3(0.0);
	spec_split = vec4(0.0);
	vec3 img, p_img, n_img, c1, c2, c3;
	float f;
	if (!mirror_chain(mi, mj, mk, view_pos, view_normal, area_lights.data[idx].position, area_lights.data[idx].inv_radius, img, p_img, n_img, f, c1, c2, c3)) {
		return;
	}
	vec3 v_img = mirror_dir(mi, normalize(-view_pos));
	if (mj < MAX_MIRROR_PLANES) {
		v_img = mirror_dir(mj, v_img);
	}
	if (mk < MAX_MIRROR_PLANES) {
		v_img = mirror_dir(mk, v_img);
	}
	area_light_eval_at(idx, p_img, n_img, v_img, roughness, diffuse, specular, spec_split);
	diffuse *= f;
	specular *= f;
	spec_split.rgb *= f;
}

// Unshadowed contribution of any encoded light entry.
void entry_eval(uint entry, vec3 view_pos, vec3 view_normal, float roughness, out vec3 diffuse, out vec3 specular, out vec4 spec_split) {
	if (sc_has_area_lights && (entry & AREA_BIT) != 0u) {
		if ((entry & IMAGE_BIT) != 0u) {
			area_light_eval_image(ENTRY_MIRROR(entry), ENTRY_MIRROR2(entry), ENTRY_MIRROR3(entry), entry & ENTRY_ID_MASK, view_pos, view_normal, roughness, diffuse, specular, spec_split);
		} else {
			area_light_eval(entry & ENTRY_ID_MASK, view_pos, view_normal, roughness, diffuse, specular, spec_split);
		}
	} else {
		vec3 unused_rel;
		if ((entry & IMAGE_BIT) != 0u) {
			light_eval_image(ENTRY_MIRROR(entry), ENTRY_MIRROR2(entry), ENTRY_MIRROR3(entry), (entry & SPOT_BIT) != 0u, entry & ENTRY_ID_MASK, view_pos, view_normal, roughness, diffuse, specular, spec_split);
		} else {
			light_eval((entry & SPOT_BIT) != 0u, entry & ENTRY_ID_MASK, view_pos, view_normal, roughness, diffuse, specular, spec_split, unused_rel);
		}
	}
}


// Coverage of a non-opaque candidate hit (an alpha-tested caster), from its
// instance's cards: the card facing the ray most squarely whose stored depth
// agrees with the hit, and its captured albedo alpha at that texel. A hit
// with no card, or none agreeing, counts as covered, so a thick object never
// leaks; only a texel the material left below half alpha lets the ray on.
bool card_covers(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir) {
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardSet s = card_sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 4.0) {
		return true;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	float best_alpha = 1.0;
	bool any_filled = false;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = abs(dot(axis, local_dir)); // Either side: the ray may come from behind the leaf.
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = card_sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(card_depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		any_filled = true;
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_alpha = texelFetch(card_albedo_atlas, texel, 0).a;
		}
	}
	if (best_w <= 0.0) {
		return any_filled; // A covered layer counts as covered; a hole (no filled texel under the hit) does not.
	}
	return best_alpha >= 0.5;
}

bool trace_visible(vec3 world_origin, vec3 world_target, uint caster_mask) {
	vec3 delta = world_target - world_origin;
	float dist = length(delta);
	if (dist < 1e-4) {
		return true;
	}
	// Shadow maps see an occluder only through the faces its material does
	// not cull, so a light inside a closed fixture mesh lights the room. The
	// ray gets the same answer by culling the faces whose drawn side looks
	// away from the light. Which flag that is depends on the winding
	// convention the acceleration structure sees; on Metal, measured with
	// rt_lab/facing_test.gd, it is back-face culling. Instances drawn
	// double-sided carry the TLAS flag that turns culling off for them.
	// Non-opaque instances (alpha-tested casters) hand their hits back as
	// candidates, confirmed where the cards say the material covers the
	// point; without any of them the opaque flag skips that loop.
	vec3 dir = delta / dist;
	rayQueryEXT rq;
	if ((params.flags & FLAG_ALPHA_CASTERS) == 0u) {
		rayQueryInitializeEXT(rq, tlas,
				gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
				caster_mask, world_origin, params.ray_bias, dir, dist - params.ray_bias);
		rayQueryProceedEXT(rq);
	} else {
		rayQueryInitializeEXT(rq, tlas,
				gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
				caster_mask, world_origin, params.ray_bias, dir, dist - params.ray_bias);
		// Past a few layers the ray is inside foliage, where it is dark
		// anyway and every further leaf would cost a card lookup.
		uint passed = 0u;
		while (rayQueryProceedEXT(rq)) {
			if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
				vec3 hit = world_origin + dir * rayQueryGetIntersectionTEXT(rq, false);
				if (passed >= 4u || card_covers(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), hit, dir)) {
					rayQueryConfirmIntersectionEXT(rq);
				} else {
					passed++;
				}
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT;
}

#define SCREEN_TRACE_STEPS 6
#define SCREEN_TRACE_DISTANCE 0.4
#define SCREEN_TRACE_THICKNESS 0.25
#define SCREEN_TRACE_BIAS 0.02

// Short screen-space trace against the depth buffer over the first stretch of
// the shadow ray (MegaLights' screen traces, without the HZB: at this range a
// fixed-step march is enough). The depth buffer is pixel-accurate where the
// BVH only has render geometry with its own bias, so this adds the contact
// occlusion that ray bias erases and fixes proxy/self-shadowing mismatches.
//
// Only the receiver-side half of the segment is marched: the depth buffer
// contains geometry that does not cast shadows (lamp bulbs and shades around
// the light being the classic case), which the BVH correctly ignores but a
// screen march cannot tell apart. The far half is the BVH ray's job anyway.
bool screen_trace_occluded(vec3 view_origin, vec3 view_target, float jitter) {
	vec3 delta = view_target - view_origin;
	float dist = length(delta);
	if (dist < 0.15) {
		return false; // Too short for a meaningful march; the BVH ray decides.
	}
	vec3 dir = delta / dist;
	float trace_dist = min(dist * 0.5, SCREEN_TRACE_DISTANCE);
	for (int i = 0; i < SCREEN_TRACE_STEPS; i++) {
		float t = trace_dist * (float(i) + jitter + 0.5) / float(SCREEN_TRACE_STEPS);
		vec3 p = view_origin + dir * t;
		vec4 ndc = params.ndc_from_view * vec4(p, 1.0);
		if (ndc.w <= 0.0) {
			return false;
		}
		ndc.xyz /= ndc.w;
		vec2 suv = ndc.xy * 0.5 + 0.5;
		if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
			return false;
		}
		float scene_depth = textureLod(depth_texture, suv, 0.0).r;
		if (scene_depth == 0.0) {
			continue; // Sky.
		}
		vec4 scene_view = params.view_from_ndc * vec4(ndc.xy, scene_depth, 1.0);
		float scene_z = scene_view.z / scene_view.w;
		// View looks down -Z: larger z is closer to the camera. Occluded when
		// the depth buffer's surface lies between the sample point and the
		// camera, within a finite thickness so distant foreground geometry
		// does not shadow everything behind it.
		if (scene_z > p.z + SCREEN_TRACE_BIAS && scene_z < p.z + SCREEN_TRACE_THICKNESS) {
			return true;
		}
	}
	return false;
}

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;
	item_from = item_min >> 5;
	item_to = (item_max == 0u) ? 0u : ((item_max - 1u) >> 5) + 1u;
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	ivec2 full_pixel = min(pixel * int(params.depth_scale), params.full_screen_size - 1);
	float depth = texelFetch(depth_texture, full_pixel, 0).r;
	if (depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_visible_light, pixel, uvec4(INVALID_LIGHT));
		imageStore(out_meta, pixel, vec4(0.0));
		imageStore(out_view_depth, pixel, vec4(0.0));
		imageStore(out_analytic_diffuse, pixel, vec4(0.0));
		imageStore(out_analytic_specular, pixel, vec4(0.0));
		return;
	}

	vec2 uv = (vec2(full_pixel) + 0.5) / vec2(params.full_screen_size);
	vec4 view_pos4 = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 view_pos = view_pos4.xyz / view_pos4.w;

	vec4 nr = texelFetch(normal_roughness_texture, full_pixel, 0);
	vec3 view_normal = nr_normal(nr);
	// Face the normal toward the viewer: the scene shader decides a
	// double-sided material's side by winding, and a mesh whose triangles wind
	// against their vertex normals writes a normal pointing into the surface
	// (see the same test in stochastic_indirect_gi.glsl). Shading such a pixel
	// would put N.L below zero for every light in the room and start its shadow
	// rays behind the wall.
	if (dot(view_normal, view_pos) > 0.0) {
		view_normal = -view_normal;
	}
	float roughness = nr_roughness(nr);

	if ((params.flags & FLAG_GBUF) != 0u) {
		vec4 gb_a = texelFetch(gbuf_albedo_texture, full_pixel, 0);
		vec4 gb_f = texelFetch(gbuf_f0_texture, full_pixel, 0);
		mat_albedo_lum = luminance(gb_albedo(gb_a));
		mat_f0 = gb_f0(gb_f);
		// scene_forward_clustered.glsl's stochastic_f90.
		mat_f90 = clamp(50.0 * mat_f0.g, gb_metallic(gb_f), 1.0);
	}

	uint pixel_seed = pcg_hash(uint(pixel.x) + pcg_hash(uint(pixel.y) + pcg_hash(params.frame_index)));

	// Look up the visible light list built last frame, reprojecting into the
	// previous frame's tile grid. The tile is jittered stochastically so the
	// transition between neighboring tile lists is dithered instead of showing
	// up as an 8 pixel grid.
	uint visible_list[LIST_SIZE];
	uint visible_count = 0u;
	// Set while guiding is on but this pixel's history is not where the list it
	// gets was built: it reprojected off screen (rotating the camera sweeps a
	// band of these off the frame every frame) or behind the camera. The list is
	// still used -- the nearest tile is a better guess than nothing -- but the
	// candidate and hidden-weight budgets below shift toward discovery to hedge
	// it. Stays false when guiding is disabled outright: that is not a miss,
	// there is simply no guide to be wrong about.
	bool guide_miss = false;
	if ((params.flags & FLAG_LIGHT_GUIDING) != 0u) {
		guide_miss = true;
		vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, depth, 1.0);
		if (prev_ndc.w > 0.0) {
			vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
			vec2 jitter = stbn_sample(pixel, 4u) - 0.5;
			vec2 tile_coord = (prev_uv * vec2(params.screen_size)) / float(TILE_SIZE) + jitter;
			// Clamp into the grid rather than rejecting what falls outside it.
			// Rotating the camera pushes a band along the leading frame edges
			// off the previous frame every frame (and the jitter alone pushes
			// border pixels out half the time); dropping the guide there leaves
			// those pixels sampling from the discovery stride alone, a visibly
			// noisier estimate whose boundary reads as a hard edge tracking the
			// rotation. The nearest border tile is the best available guess for
			// what a pixel just outside it saw, and guiding only shapes the
			// proposal distribution -- RIS divides the same weight back out, so
			// a wrong guess costs variance, never bias.
			ivec2 want_tile = ivec2(floor(tile_coord));
			ivec2 tile = clamp(want_tile, ivec2(0), ivec2(params.tiles_x - 1, params.tiles_y - 1));
			// Clamping is exactly what makes this a miss: the list now describes
			// a different part of the screen than the pixel came from.
			guide_miss = tile != want_tile;
			uint base = uint(tile.y * params.tiles_x + tile.x) * uint(LIST_SIZE);
			for (uint i = 0u; i < uint(LIST_SIZE); i++) {
				uint entry = prev_light_list.data[base + i];
				if (entry == INVALID_LIGHT) {
					break;
				}
				// Drop stale entries from lights that no longer exist.
				uint idx = entry & ENTRY_ID_MASK;
				if (!sc_has_area_lights && (entry & AREA_BIT) != 0u) {
					continue; // An area entry from a frame that still had them.
				}
				uint count = (entry & AREA_BIT) != 0u ? params.area_light_count : ((entry & SPOT_BIT) != 0u ? params.spot_light_count : params.omni_light_count);
				if (idx < count) {
					visible_list[visible_count++] = entry;
				}
			}
		}
	}

	// Guided candidates: the lights the tile saw last frame.
	float guided_weight_sum = 0.0;
	// Total unshadowed luminance estimate, for the sample culling threshold.
	float total_lum = 0.0;
	// Analytic unshadowed light sum. Shading is separable: this factor carries
	// the full-quality lighting detail and the rays only estimate a visibility
	// ratio, so it must describe one well-defined population of lights. That
	// population is the pixel's cluster cell, accumulated by the discovery
	// block below -- the guided list is a sampling hint, not a light set, and
	// contributes nothing here. (A guided entry outside the cell evaluates to
	// zero radiance anyway and is dropped by the w <= 0.0 test.)
	vec3 analytic_diffuse = vec3(0.0);
	vec3 analytic_specular = vec3(0.0);
	// The specular lobe without its Fresnel term, and the luminance-weighted
	// mean of the Schlick weight, for the scene shader to apply the material's
	// own f0 / f90 to (see light_eval).
	vec3 analytic_spec_base = vec3(0.0);
	float analytic_fc_num = 0.0;
	float analytic_fc_den = 0.0;
	// The same two sums as scalar luminance, in the |luminance| convention the
	// ray terms use. These are the ratio's denominators: taking them from the
	// vec3 sums instead would normalize by |sum L| where the numerator estimates
	// sum |L|, which differ as soon as a negative light is in the cell.
	float analytic_lum_d = 0.0;
	float analytic_lum_s = 0.0;
	// One over the probability that a discovery candidate was offered to the
	// reservoirs at all: it is on the candidate list only when the cell stride's
	// phase picked it. Guided candidates are offered unconditionally, so theirs
	// is 1. The estimator needs this to scale a subset's estimate up to the
	// cell, which is the population the analytic denominator covers.
	float discovery_mult = 1.0;
	// The guided and discovery candidates each stream into reservoirs of their
	// own (see reservoir_update), merged once the discovery total is known.
	// Both banks are walked with constant trip counts so they stay in
	// registers: a loop bounded by the uniform reservoir count would index them
	// dynamically and spill them to memory in every light loop below.
	Reservoir reservoirs[MAX_RESERVOIRS];
	Reservoir discovery[MAX_RESERVOIRS];
	float rngs[MAX_RESERVOIRS];
	for (uint r = 0u; r < MAX_RESERVOIRS; r++) {
		reservoir_init(reservoirs[r]);
		reservoir_init(discovery[r]);
		// One STBN value drives each reservoir's whole selection chain (the
		// warping in reservoir_update stretches it back to [0;1) after every
		// decision), so the blue noise property survives the loop.
		rngs[r] = r < params.reservoir_count ? min(stbn_sample(pixel, r).r, 0.9999999) : 0.0;
	}
	// The guided keys, in an array only ever indexed by constants: the cell
	// walk below compares every cell light against the guided list, which at
	// hundreds of overlapping lights must be a register compare, not a memory
	// read.
	uint guided_keys[MAX_GUIDED_CANDIDATES];
	for (uint j = 0u; j < MAX_GUIDED_CANDIDATES; j++) {
		guided_keys[j] = INVALID_LIGHT;
	}
	uint guided_count = 0u;
	uint guided_budget = guide_miss ? MISS_GUIDED_CANDIDATES : MAX_GUIDED_CANDIDATES;
	for (uint i = 0u; i < visible_count && guided_count < guided_budget; i++) {
		uint entry = visible_list[i];
		vec3 f, s;
		vec4 ss;
		entry_eval(entry, view_pos, view_normal, roughness, f, s, ss);
		float lum = candidate_luminance(f, ss); // abs inside: negative lights sample too.
		float w = light_weight(lum);
		// Down-weight lights the tile found mostly shadowed last frame; the
		// epsilon floor keeps every guided light discoverable, so one that
		// becomes unoccluded is re-found within a few frames (the sampling
		// stays unbiased: the ratio estimator divides the same weight out).
		float vis_guide;
		if (sc_has_area_lights && (entry & AREA_BIT) != 0u) {
			vis_guide = float(bitCount((entry >> QUAD_MASK_SHIFT) & 0xFu)) * 0.25;
		} else {
			vis_guide = float((entry >> QUAD_MASK_SHIFT) & 0xFu) * (1.0 / 15.0);
		}
		w *= max(vis_guide, 0.125);
		if (w <= 0.0) {
			continue;
		}
		for (uint r = 0u; r < MAX_RESERVOIRS; r++) {
			if (r < params.reservoir_count) {
				reservoir_update(reservoirs[r], entry, w, lum, rngs[r]);
			}
		}
		for (uint j = 0u; j < MAX_GUIDED_CANDIDATES; j++) {
			if (j == guided_count) {
				guided_keys[j] = entry & ENTRY_KEY_MASK;
			}
		}
		guided_weight_sum += w;
		total_lum += lum;
		guided_count++;
	}
	float hidden_weight_sum = 0.0;
	// The image chains this pixel faces (mirror_chains_at: each mirror on
	// whose reflective side it lies, alone and in the pairs and triples
	// worth it, a lamp seen in the floor seen in the ceiling): each local
	// light gets an image entry beside it through each. The variants as
	// entry bits, listed once here.
	uint mirror_variant_bits[MIRROR_CHAINS_MAX];
	uint mirror_variants = mirror_on() ? mirror_chains_at(view_pos, mirror_variant_bits) : 0u;
	for (uint v = 0u; v < mirror_variants; v++) {
		uint c = mirror_variant_bits[v];
		uint bits = IMAGE_BIT | (MIRROR_CHAIN_A(c) << IMAGE_PLANE_SHIFT);
		if (MIRROR_CHAIN_B(c) < MAX_MIRROR_PLANES) {
			bits |= IMAGE2_BIT | (MIRROR_CHAIN_B(c) << IMAGE2_PLANE_SHIFT);
		}
		if (MIRROR_CHAIN_C(c) < MAX_MIRROR_PLANES) {
			bits |= IMAGE3_BIT | (MIRROR_CHAIN_C(c) << IMAGE3_PLANE_SHIFT);
		}
		mirror_variant_bits[v] = bits;
	}

	// Discovery candidates: a strided subset of this pixel's cluster cell, so
	// newly visible lights are still found, at a per-pixel cost that does not
	// grow with the scene's light count.
	//
	// The subset is a stand-in for the cell only if something scales its
	// estimate back up by the stride, and the multiplier on the candidate
	// weights is not that something: it lands on every discovery weight alike,
	// so it appears in weight_sum and selected_weight together and the
	// estimator divides it straight back out. It survives here to keep the
	// guided and discovery weights on one scale for the hidden-light budget;
	// the scaling that matters is discovery_mult, applied in the estimator.
	//
	// Getting that wrong is what made the cluster grid visible. cell_count, and
	// through it the stride, is constant across a 32 pixel cluster cell, so an
	// unscaled subset estimate normalized against the subset's own sum is a
	// quantity whose error is constant per cell: a cell where the stride ticks
	// 1 -> 2 halves the sampled population in one step, and the boundary shows
	// up as a stair-stepped edge on large flat surfaces. No ray count removes
	// it -- more reservoirs converge to the subset's answer, not the cell's.
	//
	// The same walk accumulates the analytic term, but on its own terms: up to
	// MAX_ANALYTIC_LIGHTS the cell is summed exactly, sharing its entry_eval
	// results with whichever lights the stride also picks as candidates. The
	// stride governs sampling; it no longer governs the analytic sum.
	{
		uvec2 cluster_pos = uvec2(full_pixel) >> params.cluster_shift;
		uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
		// The cluster the pass reads has exponential depth slices when the
		// compute cull built it (cells of the linear slicing are z_far / 32
		// deep and, with a far plane of kilometres, hold every light).
		float cluster_depth = -view_pos.z;
		uint cluster_z = params.cluster_z0 > 0.0
				? uint(clamp(log(max(cluster_depth, params.cluster_z0) / params.cluster_z0) / log(params.z_far / params.cluster_z0) * 32.0, 0.0, 31.0))
				: uint(clamp((cluster_depth / params.z_far) * 32.0, 0.0, 31.0));

		// First pass: count the candidates in the cell (omni, spot, area).
		const uint type_count = sc_has_area_lights ? 3u : 2u;
		uint cell_count = 0u;
		for (uint type = 0u; type < type_count; type++) {
			uint type_offset = cluster_offset + type * params.cluster_type_size;
			uint item_min, item_max, item_from, item_to;
			cluster_get_item_range(type_offset + params.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);
			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[type_offset + i] & cluster_get_range_clip_mask(i, item_min, item_max);
				cell_count += uint(bitCount(mask));
			}
		}

		// "If we detect a history miss we increase number of evaluated lights
		// from the light grid cell" -- a shorter stride over the cell, paid for
		// by the guided slots the miss just gave back.
		uint discovery_budget = guide_miss ? MISS_DISCOVERY_CANDIDATES : MAX_DISCOVERY_CANDIDATES;
		// Small enough to sum the analytic term exactly, so it stops being an
		// estimate at all.
		bool analytic_exact = cell_count <= MAX_ANALYTIC_LIGHTS;
		// Every light the analytic sum evaluates is a candidate for free: the
		// reservoirs stream (see reservoir_update), so offering a light costs
		// an update per reservoir and no storage, against the evaluation the
		// sum already paid. So while the sum is exact the discovery proposal
		// is the whole cell, weighted by each light's exact unshadowed
		// luminance -- the proposal a big-tile reservoir (the STB lighting
		// talk's two-stage sampling) can only approximate, and the reason
		// one is not built here. The stride only exists past the exact cap,
		// where evaluating every light is what the cap says is too expensive.
		uint stride = analytic_exact ? 1u : max(1u, (cell_count + discovery_budget - 1u) / discovery_budget);
		uint start = uint(stbn_sample(pixel, 5u).r * float(stride));
		float stride_mult = float(stride);
		discovery_mult = stride_mult;

		uint cell_index = 0u;
		for (uint type = 0u; type < type_count; type++) {
			uint type_offset = cluster_offset + type * params.cluster_type_size;
			uint item_min, item_max, item_from, item_to;
			cluster_get_item_range(type_offset + params.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);
			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[type_offset + i] & cluster_get_range_clip_mask(i, item_min, item_max);
				while (mask != 0u) {
					uint bit = findLSB(mask);
					mask &= ~(1u << bit);
					uint take = cell_index++;
					uint entry = (32u * i + bit) | (type == 1u ? SPOT_BIT : (type == 2u ? AREA_BIT : 0u));

					// Which lights this pixel samples. Past the exact cap each
					// light draws its own phase instead of sharing one across
					// the cell: a shared phase makes every light present or
					// absent together, so the analytic sum lands on one of only
					// `stride` levels and reads as hard banding. Independent
					// phases decorrelate them, and the relative variance falls
					// off as 1/sqrt(cell_count) instead of staying at
					// sqrt(stride - 1).
					uint phase = analytic_exact ? start : (pcg_hash(pixel_seed ^ entry) % stride);
					bool sampled = (take % stride) == phase;
					if (!analytic_exact && !sampled) {
						continue;
					}

					// The light, and beside it its image through each planar
					// mirror the pixel faces (a candidate and an analytic term
					// of its own, sharing the light's draw in the stride).
					uint variants = 1u + mirror_variants;
					for (uint variant = 0u; variant < variants; variant++) {
						uint e = variant == 0u ? entry : (entry | mirror_variant_bits[variant - 1u]);
						vec3 f, s;
						vec4 ss;
						entry_eval(e, view_pos, view_normal, roughness, f, s, ss);
						// One evaluation serves both the analytic sum and, below,
						// the candidate set. The analytic sum takes every cell light
						// unscaled when exact, and the stride-scaled sampled subset
						// otherwise -- unconditionally either way, so a full
						// candidate array cannot darken the lighting.
						float analytic_scale = analytic_exact ? 1.0 : stride_mult;
						analytic_diffuse += f * analytic_scale;
						analytic_specular += s * analytic_scale;
						analytic_spec_base += ss.rgb * analytic_scale;
						float ss_lum = abs(luminance(ss.rgb)) * analytic_scale;
						analytic_fc_num += ss_lum * ss.a;
						analytic_fc_den += ss_lum;
						analytic_lum_d += abs(luminance(f)) * analytic_scale;
						analytic_lum_s += abs(luminance(s)) * analytic_scale;
						if (!sampled) {
							continue;
						}

						// Skip lights already on the guided list.
						bool listed = false;
						for (uint j = 0u; j < MAX_GUIDED_CANDIDATES; j++) {
							listed = listed || (guided_keys[j] == e);
						}
						if (listed) {
							continue;
						}
						float lum = candidate_luminance(f, ss);
						float w = light_weight(lum) * stride_mult;
						if (w <= 0.0) {
							continue;
						}
						for (uint r = 0u; r < MAX_RESERVOIRS; r++) {
							if (r < params.reservoir_count) {
								reservoir_update(discovery[r], e, w, lum, rngs[r]);
							}
						}
						hidden_weight_sum += w;
						total_lum += lum * stride_mult;
					}
				}
			}
		}
	}

	// Hidden light budget: clamp discovery weights to a fixed share of the
	// total, relaxed when the guided lights are dim so a brighter light that
	// just became visible can still win quickly. Applied as the discovery
	// side's scale in the merge.
	float hidden_scale = 1.0;
	if (guided_count > 0u && hidden_weight_sum > 0.0) {
		float share = guide_miss ? MISS_HIDDEN_WEIGHT_BUDGET : HIDDEN_WEIGHT_BUDGET;
		float budget = (share / (1.0 - share)) * guided_weight_sum;
		float scale = min(1.0, budget / hidden_weight_sum);
		float relax = clamp(1.0 - guided_weight_sum / DIM_VISIBLE_WEIGHT, 0.0, 1.0);
		hidden_scale = mix(scale, 1.0, relax);
	}
	bool selected_hidden[MAX_RESERVOIRS];
	for (uint r = 0u; r < MAX_RESERVOIRS; r++) {
		selected_hidden[r] = r < params.reservoir_count && reservoir_merge(reservoirs[r], discovery[r], hidden_scale, rngs[r]);
	}

	vec3 world_pos = (params.world_from_view * vec4(view_pos, 1.0)).xyz;
	mat3 world_basis = mat3(params.world_from_view);

	// Trace each unique selected light once (reservoirs frequently agree when
	// few lights dominate; duplicate rays would hit the same target).
	uint traced_keys[MAX_RESERVOIRS];
	float traced_visibility[MAX_RESERVOIRS];
	uint traced_quadrant[MAX_RESERVOIRS];
	uint traced_count = 0u;

	// Ratio estimator: the rays only measure what fraction of the unshadowed
	// luminance survives occlusion; the analytic cell sum carries the lighting
	// itself. The numerator below is an unbiased estimate of the cell's
	// shadowed luminance and the denominator is the cell's unshadowed
	// luminance, computed exactly.
	//
	// The denominator used to be a second estimate built from the same samples
	// (sum of estimator * lum). That is a self-normalized estimator, and it
	// cancels the RIS estimator rather than dividing the proposal out: with one
	// reservoir it collapses to the selected light's visibility exactly, so the
	// pixel converges to sum_i p_i V_i under the *proposal* p, not to
	// sum_i L_i V_i / sum_i L_i. Every weight this pass applies for sampling
	// reasons -- light_weight's log2 compression, the stride subset, the guided
	// list's visibility hint, the hidden-light budget -- then landed in the
	// image as bias instead of cancelling. Dividing by a fixed reference is
	// what makes them cancel; dividing by another estimate from the same draw
	// is what stopped them.
	float vis_num_d = 0.0;
	float vis_num_s = 0.0;
	// Per-unique-light visible energy, for the shading confidence heuristic.
	float traced_energy[MAX_RESERVOIRS];
	for (uint t = 0u; t < MAX_RESERVOIRS; t++) {
		traced_energy[t] = 0.0;
	}
	uint chosen_visible_light = INVALID_LIGHT;
	uint traced_found = 0u;
	for (uint r = 0u; r < MAX_RESERVOIRS; r++) {
		if (r >= params.reservoir_count) {
			break;
		}
		uint entry = reservoirs[r].entry;
		if (entry == INVALID_LIGHT) {
			continue;
		}
		uint key = entry & ENTRY_KEY_MASK;

		// Exposure-relative culling: samples too dim to matter skip their ray.
		// They can no longer drop out of the ratio the way they did when the
		// denominator was built from the samples themselves -- the denominator
		// is the whole cell now, so a skipped sample that contributed nothing
		// would read as fully shadowed. Count it unshadowed instead: it is
		// below CULL_CONTRIBUTION_FRACTION of the pixel's luminance either way,
		// which bounds the error at that threshold's own share.
		bool culled = reservoirs[r].lum < CULL_CONTRIBUTION_FRACTION * total_lum;

		vec3 f, s;
		vec4 ss_unused;
		entry_eval(entry, view_pos, view_normal, roughness, f, s, ss_unused);

		float visibility = 1.0;
		uint quadrant = 0u;
		uint slot = 0u;
		bool found = false;
		for (uint t = 0u; t < traced_count; t++) {
			if (traced_keys[t] == key) {
				visibility = traced_visibility[t];
				quadrant = traced_quadrant[t];
				slot = t;
				found = true;
				break;
			}
		}
		// Lights with a sampling extent (area rects, sized omni/spot) trace a
		// fresh sample point per reservoir instead of reusing the first ray:
		// when few lights dominate, the reservoirs all agree, and deduplicating
		// them would collapse the pixel to a single binary penumbra sample per
		// frame no matter the rays_per_pixel setting. Zero-extent lights keep
		// the dedupe (their duplicate rays would be identical, and a duplicate
		// double-counts in both the ratio numerator and denominator, so it
		// cancels there -- tracing a replacement instead was measured to add
		// variance on both the rt_lab and game-project scenes and reverted).
		bool has_extent = sc_has_area_lights && (entry & AREA_BIT) != 0u;
		if (!has_extent) {
			has_extent = ((entry & SPOT_BIT) != 0u ? spot_lights.data[entry & ENTRY_ID_MASK].size : omni_lights.data[entry & ENTRY_ID_MASK].size) > 0.0;
		}
		if (!culled && (!found || has_extent)) {
			// The first reservoir keeps the blue-noise stream; duplicates
			// decorrelate with a per-reservoir Cranley-Patterson rotation.
			vec2 sample_rnd = stbn_sample(pixel, 6u);
			if (r > 0u) {
				sample_rnd = fract(sample_rnd + vec2(hash_to_float(pcg_hash(pixel_seed + 0x9E37u * r)), hash_to_float(pcg_hash(pixel_seed + 0x85EBu * r))));
			}
			vec3 view_target;
			if (sc_has_area_lights && (entry & AREA_BIT) != 0u) {
				// Sample the rect uniformly, recording which quadrant the sample
				// landed in so the tile mask still says where the light was
				// reachable.
				//
				// This used to warp the sample toward the quadrants the tile saw
				// unoccluded, which is a place a ratio estimator cannot follow.
				// The importance weight that would undo the warp cancels: a light
				// contributes one sample per frame, so the frame's ratio is that
				// sample's visibility whatever weight it carries, and the temporal
				// average then converges to the visibility of the quadrants we
				// chose to look at rather than of the light. Measured against a
				// converged uniformly-sampled reference on stochastic_area_demo,
				// the warp sat 62% further from the truth (0.00112 vs 0.00069) for
				// 3% less noise: it read penumbrae as brighter than they are. The
				// 2x2 mask keeps its other job, down-weighting mostly-shadowed
				// lights in the candidate weights, where the estimator does divide
				// the same weight back out.
				LightData ld = area_lights.data[entry & ENTRY_ID_MASK];
				vec2 rnd = sample_rnd;
				quadrant = (rnd.x < 0.5 ? 0u : 1u) | (rnd.y < 0.5 ? 0u : 2u);
				view_target = ld.position + ld.area_width * (rnd.x - 0.5) + ld.area_height * (rnd.y - 0.5);
				if ((entry & IMAGE_BIT) != 0u) {
					if ((entry & IMAGE3_BIT) != 0u) {
						view_target = mirror_point(ENTRY_MIRROR3(entry), view_target);
					}
					if ((entry & IMAGE2_BIT) != 0u) {
						view_target = mirror_point(ENTRY_MIRROR2(entry), view_target);
					}
					view_target = mirror_point(ENTRY_MIRROR(entry), view_target);
				}
			} else {
				LightData ld = (entry & SPOT_BIT) != 0u ? spot_lights.data[entry & ENTRY_ID_MASK] : omni_lights.data[entry & ENTRY_ID_MASK];
				view_target = ld.position;
				if ((entry & IMAGE_BIT) != 0u) {
					if ((entry & IMAGE3_BIT) != 0u) {
						view_target = mirror_point(ENTRY_MIRROR3(entry), view_target);
					}
					if ((entry & IMAGE2_BIT) != 0u) {
						view_target = mirror_point(ENTRY_MIRROR2(entry), view_target);
					}
					view_target = mirror_point(ENTRY_MIRROR(entry), view_target);
				}
				// Light size drives the penumbra: sample a disk of that
				// radius perpendicular to the shadow ray, like the paper's
				// area sampling but for the sphere approximation.
				if (ld.size > 0.0) {
					vec3 dir = normalize(view_target - view_pos);
					vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
					vec3 tangent = normalize(cross(up, dir));
					vec3 bitangent = cross(dir, tangent);
					vec2 rnd = sample_rnd;
					float ang = rnd.x * 6.2831853;
					float rad = sqrt(rnd.y) * ld.size;
					view_target += (tangent * cos(ang) + bitangent * sin(ang)) * rad;
				}
			}
			// The light's own shadow settings: shadows disabled (or a caster
			// mask matching nothing) skips the ray entirely, partial opacity
			// blends toward unshadowed, and the caster mask culls which
			// objects can occlude this light's rays (render layers 1-8).
			uint light_index = entry & ENTRY_ID_MASK;
			float shadow_opacity;
			uint caster_mask;
			if (sc_has_area_lights && (entry & AREA_BIT) != 0u) {
				shadow_opacity = area_lights.data[light_index].shadow_opacity;
				caster_mask = area_lights.data[light_index].shadow_caster_mask;
			} else if ((entry & SPOT_BIT) != 0u) {
				shadow_opacity = spot_lights.data[light_index].shadow_opacity;
				caster_mask = spot_lights.data[light_index].shadow_caster_mask;
			} else {
				shadow_opacity = omni_lights.data[light_index].shadow_opacity;
				caster_mask = omni_lights.data[light_index].shadow_caster_mask;
			}
			if (shadow_opacity < 0.001 || caster_mask == 0u) {
				visibility = 1.0;
			} else if ((entry & IMAGE_BIT) != 0u) {
				// The image's shadow in legs: to a centimetre above each
				// mirror of the chain (along its normal), the target
				// mirrored back a step each time, then from the last mirror
				// to the real light (the jittered target).
				uint chain[3];
				chain[0] = ENTRY_MIRROR(entry);
				chain[1] = ENTRY_MIRROR2(entry);
				chain[2] = ENTRY_MIRROR3(entry);
				vec3 from = view_pos;
				vec3 w_from = world_pos;
				vec3 target = view_target;
				bool occluded = false;
				for (uint s = 0u; s < 3u && !occluded; s++) {
					uint m = chain[s];
					if (m >= MAX_MIRROR_PLANES) {
						break;
					}
					vec3 n = params.mirrors[m].plane.xyz;
					float hp = mirror_height(m, from);
					vec3 dir = normalize(target - from);
					float cos_p = max(-dot(n, dir), 1e-3);
					vec3 leg_end = from + dir * (max(hp - 0.01, 0.0) / cos_p);
					occluded = hp <= 0.0 || !trace_visible(w_from, world_pos + world_basis * (leg_end - view_pos), caster_mask);
					from = from + dir * (hp / cos_p) + n * 0.01;
					w_from = world_pos + world_basis * (from - view_pos);
					target = mirror_point(m, target);
				}
				occluded = occluded || !trace_visible(w_from, world_pos + world_basis * (target - view_pos), caster_mask);
				visibility = occluded ? 1.0 - shadow_opacity : 1.0;
			} else {
				bool occluded;
				// Screen traces cannot honor caster masks; skip them when the
				// light culls casters so the BVH ray decides alone.
				if (caster_mask == 0xFFu && (params.flags & FLAG_SCREEN_TRACES) != 0u && screen_trace_occluded(view_pos, view_target, stbn_sample(pixel, 7u).r)) {
					occluded = true;
				} else {
					vec3 world_light = world_pos + world_basis * (view_target - view_pos);
					occluded = !trace_visible(world_pos, world_light, caster_mask);
				}
				visibility = occluded ? 1.0 - shadow_opacity : 1.0;
			}
			if (!found) {
				traced_keys[traced_count] = key;
				traced_visibility[traced_count] = visibility;
				traced_quadrant[traced_count] = quadrant;
				slot = traced_count;
				traced_count++;
			}
		}

		// RIS estimator weight_sum / selected_weight, times one over the
		// probability that this candidate was offered at all, averaged over the
		// reservoirs. The second factor is what lifts a strided subset's
		// estimate to the cell the analytic denominator covers; guided
		// candidates were offered unconditionally, so theirs is 1. It costs no
		// storage -- a candidate is guided exactly when its index is below
		// guided_count.
		//
		// There is no clamp on weight_sum / selected_weight any more. It bounded
		// fireflies back when the estimator cancelled between numerator and
		// denominator, which is to say it bounded nothing; against a fixed
		// denominator it would be a systematic darkening of every pixel whose
		// reservoir picked a dim light. The ratio's own [0;1] clamp below is the
		// bound now, and it is the one the denoiser was designed around.
		float estimator = reservoirs[r].weight_sum / max(reservoirs[r].selected_weight, 1e-6);
		estimator *= selected_hidden[r] ? discovery_mult : 1.0;
		estimator /= float(params.reservoir_count);
		float lum_d = abs(luminance(f));
		float lum_s = abs(luminance(s));

		// Pick one traced light uniformly to seed next frame's tile list,
		// carrying how visible its ray found it (area lights record which rect
		// quadrant the ray reached instead), so guiding can down-weight
		// shadowed lights without dropping them from the list. A culled sample
		// never traced a ray, so it has no measured visibility to report and
		// does not enter the draw.
		if (!culled) {
			traced_found++;
			if (hash_to_float(pcg_hash(pixel_seed + 0xB5u + traced_found)) < 1.0 / float(traced_found)) {
				chosen_visible_light = entry & ENTRY_KEY_MASK;
				if (sc_has_area_lights && (entry & AREA_BIT) != 0u) {
					if (visibility > 0.0) {
						chosen_visible_light |= 1u << (QUAD_MASK_SHIFT + quadrant);
					}
				} else {
					chosen_visible_light |= uint(clamp(visibility, 0.0, 1.0) * 15.0 + 0.5) << QUAD_MASK_SHIFT;
				}
			}
		}

		if (visibility <= 0.0) {
			continue;
		}
		// Firefly bound. A reservoir's term is an estimate of the cell's whole
		// shadowed sum divided by the reservoir count, so its natural scale is
		// analytic_lum / reservoir_count -- which is exactly why the bound has
		// to sit well above that and not at it. Capping at the natural scale
		// clips a symmetric fluctuation at its own mean and removes the upper
		// half of it, darkening every pixel whose proposal is even slightly
		// mismatched; measured on the game project it cost 18% of the frame's
		// mean brightness. FIREFLY_HEADROOM is the slack: below it the term is
		// ordinary variance the denoiser is there to average, above it the
		// reservoir picked a light whose share of the selection weight bears no
		// relation to its share of this signal (the reservoirs select on
		// diffuse + specular luminance, so a nearly-all-specular pick divides a
		// small lum_d by a small probability) and no amount of averaging brings
		// it back.
		float share = FIREFLY_HEADROOM / float(params.reservoir_count);
		vis_num_d += min(estimator * lum_d, analytic_lum_d * share) * visibility;
		vis_num_s += min(estimator * lum_s, analytic_lum_s * share) * visibility;
		// A culled sample has no slot of its own (it never traced), and its
		// energy is below the cull threshold anyway, so it stays out of the
		// dominance heuristic rather than landing on another light's slot.
		if (!culled) {
			traced_energy[slot] += estimator * (lum_d + lum_s) * visibility;
		}
	}

	// The ceiling is FIREFLY_HEADROOM, not 1, and that is not a rounding of the
	// physical bound -- it is the difference between an unbiased estimator and a
	// darkened one.
	//
	// The true ratio cannot exceed 1, but an unbiased estimate of it can, and
	// clipping at 1 removes only the upper half of that spread. The old
	// self-normalized form was a weighted average of per-light visibilities, so
	// it sat inside [0;1] by construction and the clamp never fired; against a
	// fixed denominator it fires constantly, and it takes energy out every time.
	// Measured on the game project's 26 lights it cost 22% of the frame's
	// brightness, and the giveaway was that the mean rose with the ray count --
	// 0.520, 0.566, 0.584 of the reference at 1, 2 and 4 rays. An unbiased
	// estimator's mean cannot depend on how many samples it averages; a clipped
	// one can, because more samples narrow the spread that was being clipped.
	//
	// Letting the frame's value overshoot is what keeps the mean right, and the
	// denoiser is where it comes back down: the temporal average converges to
	// the true ratio, and the neighborhood clamp bounds what a single frame can
	// do. The per-reservoir cap above is then the one bound in the pass, instead
	// of two that disagree.
	float ratio_d = clamp(analytic_lum_d > 0.0 ? vis_num_d / analytic_lum_d : 0.0, 0.0, FIREFLY_HEADROOM);
	float ratio_s = clamp(analytic_lum_s > 0.0 ? vis_num_s / analytic_lum_s : 0.0, 0.0, FIREFLY_HEADROOM);

	// Shading confidence: the share of visible energy carried by the single
	// strongest light. Where one light dominates, the shadow signal is nearly
	// binary and converges fast temporally; the denoiser skips its spatial
	// filter there to keep the edge (the paper's ~80% heuristic).
	float visible_energy = vis_num_d + vis_num_s;
	float dominance = 0.0;
	if (visible_energy > 0.0) {
		for (uint t = 0u; t < traced_count; t++) {
			dominance = max(dominance, traced_energy[t]);
		}
		dominance /= visible_energy;
	}

	// The ratios are the denoiser's input (replicated to the shared vec3
	// filter path); the analytic terms bypass the filter entirely, which is
	// why bound_analytic has to be the one place they are made safe -- nothing
	// downstream inspects them again before they are multiplied back in. The
	// unsigned buffer format drops negative-light energy, as the modulated
	// signal always did.
	imageStore(out_diffuse, pixel, vec4(vec3(ratio_d), 0.0));
	imageStore(out_specular, pixel, vec4(vec3(ratio_s), 0.0));
	imageStore(out_analytic_diffuse, pixel, vec4(bound_analytic(analytic_diffuse), 0.0));
	imageStore(out_analytic_specular, pixel, vec4(bound_analytic(analytic_spec_base), analytic_fc_den > 0.0 ? analytic_fc_num / analytic_fc_den : 0.0));
	imageStore(out_visible_light, pixel, uvec4(chosen_visible_light));
	imageStore(out_meta, pixel, vec4(dominance));
	imageStore(out_view_depth, pixel, vec4(-view_pos.z));
}
