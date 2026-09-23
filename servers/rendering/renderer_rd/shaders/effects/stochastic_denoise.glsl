#[compute]

#version 450

#VERSION_DEFINES

// Denoiser for the stochastic direct lighting buffers, following the
// MegaLights / SVGF structure: temporal accumulation of lighting and its
// luminance moments (giving a per-pixel variance estimate), then a
// variance-driven spatial pass iterated a-trous style (up to three
// strides, Raytracing::process_stochastic) using depth, normal and
// variance edge-stopping.
// The two filtered signals are demodulated: for direct lighting they are
// bounded [0;1] visibility ratios (the analytic lighting is multiplied back
// in at the end of the spatial pass), for GI they are radiance.

#include "../normal_roughness_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Params.flags bits.
#define FLAG_HAS_VELOCITY 1u // A real velocity buffer is bound (else the binding is a dummy and must not be fetched).
#define FLAG_HAS_META 2u // Temporal: raw_meta is a real shading-confidence texture.
#define FLAG_MODULATE_ANALYTIC 4u // Spatial: multiply the filtered ratios by the analytic lighting buffers.
#define FLAG_FALLBACK_ALL 32u // Spatial (GI, diagnostics): the cards' fallback at every pixel in place of the filtered GI.
// Temporal (GI, diagnostics): the reflection history keeps its frames through
// the named restart (GODOT_GI_SPEC_ABLATE=change,smear,mismatch).
#define FLAG_DYN_SPLIT 2097152u // GI: the moving lights' term is a history of its own (GODOT_GI_DYN_SPLIT, section 88): the temporal pass accumulates in_dyn against history_dyn, restarted by its own mark, and hands the spatial pass the sum; the meta's b is its frame count.
#define FLAG_SPEC_NO_CHANGE 64u
#define FLAG_SPEC_NO_SMEAR 128u
#define FLAG_SPEC_NO_MISMATCH 256u
#define FLAG_SPEC_PAINT 512u // Diagnostics (GODOT_GI_SPEC_ABLATE=paint): the reflection's frame count as a colour.
#define FLAG_SPEC_PAINT_WHY 1024u // Diagnostics (GODOT_GI_SPEC_ABLATE=why): why the history is short (see the store).
#define FLAG_SPEC_NO_YOUNG 131072u // Experiment (GODOT_GI_SPEC_ABLATE=young): the spatial pass does not filter a reflection for being young; its variance alone decides.
#define FLAG_SPATIAL_OFF 262144u // Experiment (GODOT_GI_SPATIAL=0): the spatial pass stores its input unfiltered (the temporal output reaches the scene shader).
#define FLAG_NO_OBJECTS 65536u // Experiment (GODOT_GI_OBJECTS=0): no moving-object classification from the velocity buffer; every history at the camera reprojection.
#define FLAG_VELOCITY_CURRENT 1048576u // The velocity buffer is this frame's (the motion-vector prepass): every history at uv + velocity, camera and objects alike, no classification.

// Frame-edge history borrowing (temporal pass, see the reprojection block):
// how far outside the previous frame a history may lie and still borrow the
// nearest in-frame one comes in as reprojection.borrow_band (GODOT_GI_BORROW).
// Temporal (GI): how far a reflection history tap's stored image depth may
// differ from the predicted one, relative, before the tap is left out (see
// the reflection fetch); the whole-pixel mismatch restart begins at 0.1.
#define SPEC_TAP_DEPTH_TOLERANCE 0.2
// The frame count a borrowed history is trusted as: it is lighting from a
// neighboring column, so it starts the accumulation and is then replaced by
// the pixel's own samples over the next few frames.
#define BORROW_FRAMES 4.0
// The meta's alpha: 1 at a reveal, less REVEAL_STEP every frame a usable
// history followed (exact in the 8-bit texture), so 1 - a is the frames
// since the reveal over REVEAL_STEP, up to 63.
#define REVEAL_STEP (4.0 / 255.0)
// Spatial pass (GI): frames of accumulation under which the pixel's
// hit-distance term (a ray or two's worth) is not trusted to shape the
// kernel, and the luminance stop stays off.
#define YOUNG_FRAMES 8.0
// Borrowed taps are compared against the depth of a surface point up to a
// pan's width away, so a wall at a grazing angle needs more room than the
// same-texel test; a different surface still fails.
#define BORROW_DEPTH_TOLERANCE 0.25

// The GI signal carries a directional companion buffer (first radiance moment
// + near-field visibility) that must be blended and filtered with exactly the
// diffuse weights. It rides on the GI-only variants (VALIDATE_DEPTH for the
// temporal pass, FILTER_DIRECTIONAL for the spatial one) rather than a runtime
// flag, so the direct lighting variants keep their existing set layouts.
//
// DEPTH_HISTORY (shared by the GI temporal variant and the direct lighting
// temporal variant via DIRECT_DEPTH_VALIDATION) binds the signal's own
// previous-frame view depth and rejects history whose reprojected depth
// disagrees: a revealed pixel inherits whatever surface used to be there
// otherwise. The direct ratios additionally keep their neighborhood clamp;
// the two gates complement each other.
#ifdef VALIDATE_DEPTH
#define HAS_DIRECTIONAL
// The GI accumulates unbounded HDR radiance through a 32-frame feedback loop,
// where the packed format's rounding compounds; direct lighting accumulates
// bounded [0;1] visibility ratios and stays packed.
#define LIGHTING_FORMAT rgba16f
#define DEPTH_HISTORY
#else
#define LIGHTING_FORMAT r11f_g11f_b10f
#endif
#ifdef DIRECT_DEPTH_VALIDATION
#define DEPTH_HISTORY
#endif
#ifdef FILTER_DIRECTIONAL
#define HAS_DIRECTIONAL
#endif
// The spatial pass can run several a-trous iterations at doubling strides. All
// but the last write to a scratch buffer that stays in the accumulation format
// and skips both the analytic modulation (a runtime flag) and the directional
// renormalization, which is not idempotent. Only the GI path needs the extra
// variant: the direct lighting buffers are packed end to end, so its
// intermediate iterations reuse the plain spatial variant with the modulation
// flag cleared.
#ifdef SPATIAL_HDR_OUT
#define SPATIAL_OUT_FORMAT rgba16f
#else
#define SPATIAL_OUT_FORMAT r11f_g11f_b10f
#endif
// The direct lighting's final iteration writes its specular with the analytic
// buffer's Fresnel weight in alpha (see stochastic_direct_lighting.glsl,
// light_eval), which the packed format has no room for.
#if defined(SPATIAL_SPEC_ALPHA_OUT) || defined(SPATIAL_HDR_OUT)
#define SPATIAL_SPEC_OUT_FORMAT rgba16f
#else
#define SPATIAL_SPEC_OUT_FORMAT r11f_g11f_b10f
#endif
// SPATIAL_MOMENTS_OUT: the intermediate a-trous iterations also write their
// filtered moments, so the next iteration's variance estimate, luminance edge
// stop and skip-if-converged gate describe the signal it is actually filtering
// instead of the unfiltered one (whose variance overstates what is left by
// then, keeping the edge stops loose and the gating conservative at every
// iteration after the first). The mean rides the a-trous weights and the
// variance the squared weights (SVGF 4.3), so the variance handed on is that
// of the average this iteration produced. The final iteration does not define
// this (nothing consumes its moments): the temporally accumulated moments are
// never overwritten, so the temporal pass's history stays an honest record of
// the accumulated -- not the spatially filtered -- signal.
#ifdef SPATIAL_MOMENTS_OUT
#define MOMENTS_OUTPUT
#endif

layout(set = 0, binding = 0) uniform sampler2D in_diffuse;
layout(set = 0, binding = 1) uniform sampler2D in_specular;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;

#ifdef MODE_TEMPORAL
layout(set = 0, binding = 3) uniform sampler2D history_diffuse;
layout(set = 0, binding = 4) uniform sampler2D history_specular;
layout(set = 0, binding = 5) uniform sampler2D history_moments;
// r: shading confidence from the sampling pass (FLAG_HAS_META).
layout(set = 0, binding = 6) uniform sampler2D raw_meta;
// r: diffuse frames / 64, g: specular frames / 64, b: shading confidence,
// a: disocclusion mark.
layout(set = 0, binding = 7) uniform sampler2D history_meta;
// Motion vectors from the previous frame's color pass (uv_prev = uv + velocity).
// A dummy binding when motion vectors are not rendered (FLAG_HAS_VELOCITY unset).
layout(set = 0, binding = 8) uniform sampler2D velocity_texture;
#ifdef DEPTH_HISTORY
// The signal's own view depth from the previous frame (at the signal's
// resolution), for disocclusion rejection.
layout(set = 0, binding = 9) uniform sampler2D prev_view_depth_texture;
#endif
#ifdef HAS_DIRECTIONAL
// This frame's directional term and its history.
layout(set = 0, binding = 10) uniform sampler2D in_directional;
layout(set = 0, binding = 11) uniform sampler2D history_directional;
// Roughness decides how far the reflection follows its virtual image
// rather than the surface when reprojecting.
layout(set = 0, binding = 12) uniform sampler2D normal_roughness_texture;
#endif

layout(set = 1, binding = 0, LIGHTING_FORMAT) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, LIGHTING_FORMAT) uniform restrict writeonly image2D out_specular;
// xy: diffuse luminance 1st/2nd moment, zw: specular.
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_moments;
// r: diffuse frames / 64, g: specular frames / 64, b: shading confidence,
// a: disocclusion mark (decays over a few frames).
layout(set = 1, binding = 3, rgba8) uniform restrict writeonly image2D out_meta;
#ifdef HAS_DIRECTIONAL
layout(set = 1, binding = 5, rgba16f) uniform restrict writeonly image2D out_directional;
// The moving lights' term (FLAG_DYN_SPLIT): the gather's sample of it with
// its change mark, its history, and the two outputs -- the history for
// next frame and the sum of both histories for the spatial pass.
layout(set = 0, binding = 17) uniform sampler2D in_dyn;
layout(set = 0, binding = 18) uniform sampler2D history_dyn;
layout(set = 1, binding = 7, rgba16f) uniform restrict writeonly image2D out_dyn;
layout(set = 1, binding = 8, rgba16f) uniform restrict writeonly image2D out_sum;
#endif
#else // MODE_SPATIAL
layout(set = 0, binding = 3) uniform sampler2D moments_texture;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 5) uniform sampler2D meta_texture;
// Unshadowed analytic lighting, multiplied back into the filtered ratios when
// FLAG_MODULATE_ANALYTIC is set (dummy bindings otherwise, never fetched).
layout(set = 0, binding = 6) uniform sampler2D analytic_diffuse;
layout(set = 0, binding = 7) uniform sampler2D analytic_specular;
#ifdef FILTER_DIRECTIONAL
layout(set = 0, binding = 8) uniform sampler2D in_directional;
// The gather's fallback for young pixels: the card's bounce irradiance
// under the surface (rgb) and its relight count (a, / 64); see out_fallback
// there.
layout(set = 0, binding = 9) uniform sampler2D fallback_texture;
// The moving lights' share of the fallback (FLAG_DYN_SPLIT; see store_result).
layout(set = 0, binding = 11) uniform sampler2D fallback_dyn_texture;
#endif

// The spatial pass writes the final buffers, which stay packed on both paths:
// they are written once and read four times per fragment by the upsample, so
// the rounding never compounds. Intermediate a-trous iterations keep the
// accumulation format instead (SPATIAL_HDR_OUT).
layout(set = 1, binding = 0, SPATIAL_OUT_FORMAT) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, SPATIAL_SPEC_OUT_FORMAT) uniform restrict writeonly image2D out_specular;
#ifdef FILTER_DIRECTIONAL
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_directional;
#endif
#ifdef MOMENTS_OUTPUT
layout(set = 1, binding = 3, rgba16f) uniform restrict writeonly image2D out_moments;
#endif
#endif

#ifdef MODE_TEMPORAL
// The previous frame pair's reprojection (previous NDC -> the frame before).
// The velocity buffer is one frame stale (written by the previous color pass),
// so this is the camera-only motion it was rendered with: static pixels match
// it and only genuinely moving objects deviate.
layout(set = 1, binding = 4, std140) uniform ReprojectUBO {
	mat4 prev_reproject;
	// Temporal (GI): the frames a rough reflection's history restarted by
	// the change mark is worth once the raw 5x5 resolve has stood in for
	// the changed part (0: the raw sample stands in, as before). GODOT_GI_SPEC_FIX.
	// In the UBO: push constants are capped at 128 bytes.
	float spec_fix;
	float borrow_band; // How far outside the frame (UV) a history may lie and still borrow the edge's (GODOT_GI_BORROW; 0 never borrows).
	float young_rays; // The gather's diffuse rays for a pixel whose history is under 8 frames: that frame's sample counts for as many.
	float pad0;
	// Half the previous frame's TAA jitter minus this frame's, in NDC: the
	// motion vectors are unjittered (the scene shader subtracts both
	// frames' jitters) where this pass's pixels sit in the jittered image,
	// so a history predicted by uv + velocity lands here short of that
	// (FLAG_VELOCITY_CURRENT). The block is 96 bytes; the C++ struct
	// carries the same.
	vec2 jitter_delta;
}
reprojection;
#endif

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC (temporal only).
	ivec2 screen_size;
	float blend_alpha; // Minimum weight of the current frame.
	float depth_tolerance; // Spatial only: the temporal pass hardcodes its 0.1 (and BORROW_DEPTH_TOLERANCE for a borrowed history).
	float variance_threshold; // Relative variance below which filtering is skipped.
	int stride; // Spatial kernel stride.
	int depth_scale; // 2 when the lighting buffers are half resolution.
	// Neighborhood clamp width in standard deviations; <= 0 disables history
	// clipping entirely. Every signal takes 4.0 now (Raytracing::process_stochastic
	// measured 1.5 on the direct ratio as a darkening); sparse Monte Carlo
	// signals (the GI gather) must not clamp: a 5x5
	// neighborhood that catches no bright sample this frame would clip a
	// perfectly converged history to black every frame (the same failure the
	// STB lighting talk hits with TAA color clamping on noisy input).
	float clamp_gamma;
	// Camera planes, for linearizing the reprojected depth (VALIDATE_DEPTH).
	float z_near;
	float z_far;
	uint flags;
	// Spatial (GI): the card relight count at which a young pixel's card
	// stand-in reaches full weight (see store_result).
	float fallback_ramp;
	// Temporal (GI): the fewest frames the lighting-change mark may restart
	// the reflection history to (0: no floor; GODOT_GI_SPEC_RESTART_MIN).
	float spec_restart_min;
	// The working colour space's luminance weights (ColorManagement): the
	// moments and the luminance stop measure radiance with these.
	float luma_r;
	float luma_g;
	float luma_b;
}
params;

float luminance(vec3 c) {
	return dot(c, vec3(params.luma_r, params.luma_g, params.luma_b));
}

// Depth buffer value -> view-space distance. Comparing raw buffer depths
// directly would mean comparing a nonlinear quantity: the same world-space
// step is worth a large depth difference close to the camera and a vanishing
// one far away, so a fixed tolerance over-rejects near neighbors and lets
// distant surfaces bleed together.
float linearize_depth(float p_depth) {
	float ndc_z = clamp(p_depth, 0.0, 1.0) * 2.0 - 1.0;
	return 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + ndc_z * (params.z_far - params.z_near));
}

// The inverse, so a view-space tolerance can be turned into a pair of raw
// depth bounds once per pixel: the kernel's taps then cost a comparison each
// instead of a linearization.
float depth_from_linear(float p_view_depth) {
	float ndc_z = (2.0 * params.z_near * params.z_far / max(p_view_depth, 1e-6) - (params.z_far + params.z_near)) / (params.z_far - params.z_near);
	return (ndc_z + 1.0) * 0.5;
}

vec3 rgb_to_ycocg(vec3 c) {
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
			0.5 * c.r - 0.5 * c.b,
			-0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 ycocg_to_rgb(vec3 c) {
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

#ifdef MODE_TEMPORAL

// Variance clipping in YCoCg (Salvi 2016, Karis 2014): clips history to an
// ellipsoid around the local mean, which avoids the color shifts that a
// per-channel RGB min/max clamp introduces.
// The direct lighting signals are one visibility ratio replicated across all
// three channels, so they carry no chroma of their own. What YCoCg then reads
// as Co and Cg is the r11f_g11f_b10f rounding -- B keeps five mantissa bits
// where R and G keep six, so a gray value does not survive the round trip gray.
// The neighborhood's chroma extent is the spread of that rounding, microscopic
// and floored at 1e-5, while the history's chroma differs from the mean's by a
// comparable amount, so the normalized distance_outside comes back large for a
// pixel that is not drifting at all.
//
// That number is not just a clip: it feeds confidence, confidence multiplies
// the accumulated frame count, and frames = frames * confidence + 1 has fixed
// point 1 / (1 - confidence). A confidence permanently short of 1 pins the
// history to a handful of frames, alpha stays near its ceiling, and the result
// boils at close to the raw sampling noise -- with temporal_frames inert,
// because the cap is never the binding constraint. Clip the scalar the signal
// actually is, and the chroma axis stops voting.
vec3 clip_ratio(vec3 history, vec3 mean, vec3 extent, out float distance_outside) {
	float h = luminance(history);
	float m = luminance(mean);
	float e = max(luminance(extent), 1e-5);
	float clipped = clamp(h, m - e, m + e);
	distance_outside = abs(clipped - h) / e;
	return vec3(clipped);
}

vec3 clip_to_aabb(vec3 history, vec3 mean, vec3 extent, out float distance_outside) {
	vec3 h = rgb_to_ycocg(history);
	vec3 m = rgb_to_ycocg(mean);
	vec3 e = max(abs(rgb_to_ycocg(mean + extent) - m), vec3(1e-5));
	vec3 clipped = clamp(h, m - e, m + e);
	distance_outside = length((clipped - h) / e);
	return ycocg_to_rgb(clipped);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel * params.depth_scale, 0).r;
	if (center_depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
		imageStore(out_moments, pixel, vec4(0.0));
		imageStore(out_meta, pixel, vec4(0.0));
#ifdef HAS_DIRECTIONAL
		imageStore(out_directional, pixel, vec4(0.0, 0.0, 0.0, 1.0));
		if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
			imageStore(out_dyn, pixel, vec4(0.0));
			imageStore(out_sum, pixel, vec4(0.0));
		}
#endif
		return;
	}

	vec4 current_diffuse4 = texelFetch(in_diffuse, pixel, 0);
	vec3 current_diffuse = current_diffuse4.rgb;
	float change_age = current_diffuse4.a; // GI only; see the restart below.
	vec4 current_specular4 = texelFetch(in_specular, pixel, 0);
	vec3 current_specular = current_specular4.rgb;
#ifdef HAS_DIRECTIONAL
	// The moving lights' term (FLAG_DYN_SPLIT): its own sample, mark,
	// history and frame count (the meta's b, the direct pass's dominance,
	// which the GI never had).
	const bool dyn_split = (params.flags & FLAG_DYN_SPLIT) != 0u;
	vec4 current_dyn4 = dyn_split ? texelFetch(in_dyn, pixel, 0) : vec4(0.0);
	vec3 current_dyn = current_dyn4.rgb;
	float change_age_dyn = current_dyn4.a;
	vec3 result_dyn = current_dyn;
	float frames_dyn = 1.0;
	vec4 current_directional = texelFetch(in_directional, pixel, 0);
	vec4 result_directional = current_directional;
	// The reflection's virtual view depth (gather output alpha), and how much
	// of the way toward it the reflection's history is looked up: all of it
	// for a mirror, none for a rough surface whose lobe has no single image.
	float virtual_view_depth = current_specular4.a;
	float nr_rough = nr_roughness(texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0));
	float virtual_weight = 1.0 - smoothstep(0.15, 0.6, nr_rough);
#endif

	// 5x5 neighborhood statistics for history rectification.
	vec3 mean_d = vec3(0.0);
	vec3 mean_s = vec3(0.0);
	vec3 m2_d = vec3(0.0);
	vec3 m2_s = vec3(0.0);
	float count = 0.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			ivec2 sp = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
			vec3 d = texelFetch(in_diffuse, sp, 0).rgb;
			vec3 s = texelFetch(in_specular, sp, 0).rgb;
			mean_d += d;
			mean_s += s;
			m2_d += d * d;
			m2_s += s * s;
			count += 1.0;
		}
	}
	mean_d /= count;
	mean_s /= count;
	vec3 stddev_d = sqrt(max(m2_d / count - mean_d * mean_d, vec3(0.0)));
	vec3 stddev_s = sqrt(max(m2_s / count - mean_s * mean_s, vec3(0.0)));

	// Dense bounded signals (the visibility ratios) also clamp the CURRENT
	// sample into the neighborhood ellipsoid: a pixel whose reservoirs keep
	// selecting an occluded light can otherwise stay a stuck outlier forever
	// (its history converges to the outlier, so nothing ever rejects it). At
	// real shadow edges the 5x5 spans both populations, so the ellipsoid is
	// wide and detail survives. Sparse Monte Carlo signals must not do this:
	// it would reject the rare bright samples that carry all the energy.
	if (params.clamp_gamma > 0.0) {
		float unused_d;
		float unused_s;
		current_diffuse = clip_to_aabb(current_diffuse, mean_d, stddev_d * params.clamp_gamma, unused_d);
		current_specular = clip_to_aabb(current_specular, mean_s, stddev_s * params.clamp_gamma, unused_s);
	}

	vec3 result_diffuse = current_diffuse;
	vec3 result_specular = current_specular;
	vec4 moments = vec4(0.0);
	float frames_d = 1.0;
	float samples_d = 1.0; // Samples this frame's diffuse carries (the gather's young rays).
	float frames_s = 1.0;
	// Disocclusion mark for the spatial pass; set until a usable history
	// proves the pixel is not freshly revealed.
	float reveal = 1.0;
	// Diagnostics (FLAG_SPEC_PAINT_WHY): 1 history off frame, 2 borrowed, 3 every depth tap failed, 4 velocity-classified moving object.
	int paint_why = 0;
	// Shading confidence from the sampling pass (share of energy carried by
	// the strongest single light).
	float dominance = (params.flags & FLAG_HAS_META) != 0u ? texelFetch(raw_meta, pixel, 0).r : 0.0;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 prev_ndc = params.reproject * vec4(uv * 2.0 - 1.0, center_depth, 1.0);
	if (prev_ndc.w > 0.0) {
		vec2 prev_uv = (prev_ndc.xy / prev_ndc.w) * 0.5 + 0.5;
		// The camera reprojection is exact for static geometry, but a nonzero
		// velocity does not mean a moving object: Godot velocity buffers carry
		// camera motion for static geometry too, and this buffer is one frame
		// stale (written by the previous color pass). Classify by comparing
		// against the camera-only motion of the frame pair the buffer was
		// rendered with: only where the two disagree is the pixel a moving
		// object, and the stale velocity is then the best predictor available
		// of where its history lives.
		// The buffer being the previous frame's, the point's velocity sits
		// at its previous-frame pixel, prev_uv, not at the current one: the
		// current pixel held other geometry a frame ago, and under a fast
		// rotation the flow there differs from the flow here by tens of
		// pixels (the flow of a yaw varies across the frame with the
		// perspective), which classified every pixel of an 84-degree flick as a
		// moving object (section 59, every pixel cyan under =why). Read at
		// prev_uv the field is the one the matrix predicts for static geometry
		// at any speed, and a moving object at a static camera reads as before
		// (prev_uv is uv).
		// A moving object's shift beyond the camera's, carried to the
		// reflection's reprojection below (the reflector took its image
		// along).
		vec2 object_delta = vec2(0.0);
		bool velocity_in_frame = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));
		if ((params.flags & FLAG_VELOCITY_CURRENT) != 0u && (params.flags & FLAG_HAS_VELOCITY) != 0u) {
			// This frame's motion vectors, from the prepass (section 71): the
			// point under this pixel was at uv + velocity last frame, whatever
			// moved -- the camera, the object, or both -- so every history is
			// fetched there and the classification below, built for a buffer
			// a frame stale, is not needed. A static pixel lands where the
			// camera reprojection puts it to the precision of the two; the
			// difference is kept as the object's own motion for the
			// reflection's virtual image.
			vec2 velocity = texelFetch(velocity_texture, pixel * params.depth_scale, 0).xy;
			vec2 predicted = uv + velocity + reprojection.jitter_delta;
			object_delta = predicted - prev_uv;
			prev_uv = predicted;
		} else if ((params.flags & FLAG_HAS_VELOCITY) != 0u && (params.flags & FLAG_NO_OBJECTS) == 0u && velocity_in_frame) {
			ivec2 velocity_pixel = ivec2(prev_uv * vec2(params.screen_size * params.depth_scale));
			vec2 velocity = texelFetch(velocity_texture, velocity_pixel, 0).xy;
			vec4 prevprev_ndc = reprojection.prev_reproject * vec4(prev_ndc.xyz / prev_ndc.w, 1.0);
			// Under FSR2 the pixels no geometry wrote carry a (-1, -1)
			// sentinel, not a motion.
			bool has_velocity = velocity != vec2(0.0) && all(greaterThan(velocity, vec2(-0.99)));
			if (has_velocity && prevprev_ndc.w > 0.0) {
				vec2 static_motion = (prevprev_ndc.xy / prevprev_ndc.w) * 0.5 + 0.5 - prev_uv;
				// Threshold of 2 full-resolution pixels: the velocity buffer is
				// unjittered while the reprojection matrices carry the TAA
				// jitter of both frames.
				vec2 object_pixels = (velocity - static_motion) * vec2(params.screen_size * params.depth_scale);
				if (any(greaterThan(abs(object_pixels), vec2(2.0)))) {
					object_delta = uv + velocity - prev_uv;
					prev_uv = uv + velocity;
					paint_why = 4;
				}
			}
		}
#ifdef HAS_DIRECTIONAL
		// Where the reflection's image reprojects to, at the virtual depth, run
		// through the camera reprojection like the surface. Its distance from
		// the surface's own reprojection is the parallax: how far the reflected
		// image slid over the surface this frame. Zero under a pure rotation,
		// and what a camera translation does to every glossy highlight.
		vec2 prev_uv_virtual = prev_uv;
		float parallax_px = 0.0;
		float predicted_virtual_depth = 0.0;
		if (virtual_view_depth > 0.0) {
			vec4 prev_ndc_v = params.reproject * vec4(uv * 2.0 - 1.0, depth_from_linear(virtual_view_depth), 1.0);
			if (prev_ndc_v.w > 0.0) {
				prev_uv_virtual = (prev_ndc_v.xy / prev_ndc_v.w) * 0.5 + 0.5 + object_delta;
				parallax_px = length((prev_uv_virtual - prev_uv) * vec2(params.screen_size));
				predicted_virtual_depth = linearize_depth(prev_ndc_v.z / prev_ndc_v.w);
			}
		}
#endif
		bool history_usable = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
		if (!history_usable) {
			paint_why = 1;
		}
		// Frame-edge reveal: the pixel's history lies just off the previous
		// frame. Rotating the camera sweeps a band of these along the entering
		// edge every frame, and restarting each of them from a single raw
		// sample draws that band as a hard, noisy stripe (with half-resolution
		// GI, blotches) against the converged interior; the spatial pass cannot
		// bridge it since the band is as wide as the pan is fast. The nearest
		// in-frame history is a far better start than nothing -- the lighting
		// is continuous across the frame edge wherever the surface is -- so
		// borrow it, depth-validated below like any other tap, but at a low
		// frame count so the borrowed value yields to real samples within a
		// few frames. Bounded to a band near the edge: a camera cut reprojects
		// the whole screen far outside, and stretching the edge columns over
		// it would be worse than the noise.
		bool borrowed = false;
#ifdef DEPTH_HISTORY
		if (!history_usable && reprojection.borrow_band > 0.0 && all(greaterThanEqual(prev_uv, vec2(-reprojection.borrow_band))) && all(lessThanEqual(prev_uv, vec2(1.0 + reprojection.borrow_band)))) {
			prev_uv = clamp(prev_uv, vec2(0.0), vec2(1.0));
			history_usable = true;
			borrowed = true;
			paint_why = 2;
		}
#endif
		vec4 hist_d4 = vec4(0.0);
		vec4 hist_s4 = vec4(0.0);
		vec4 hist_moments = vec4(0.0);
		vec4 hist_meta = vec4(0.0);
#ifdef HAS_DIRECTIONAL
		vec4 hist_dir = vec4(0.0);
		vec4 hist_dyn4 = vec4(0.0);
#endif
#ifdef DEPTH_HISTORY
		if (history_usable) {
			// Disocclusion check: a revealed pixel would otherwise inherit
			// whatever surface used to be there. The signal recorded its own
			// view depth last frame; compare it to where this surface
			// reprojects to. On the direct ratios this complements the
			// neighborhood clamp (which catches lighting changes but is blind
			// to a surface swap at equal brightness); on the sparse GI signal
			// it is the only gate, since clipping is off there.
			//
			// The test runs per bilinear tap, not once at the nearest texel: a
			// filtered fetch straddling a silhouette would otherwise blend the
			// far surface's history into the near one at up to half weight,
			// with nothing downstream to reject it. Taps that fail drop out and
			// the rest renormalise; when every tap fails the pixel is revealed.
			float predicted_depth = linearize_depth(prev_ndc.z / prev_ndc.w);
			vec2 hist_pos = prev_uv * vec2(params.screen_size) - 0.5;
			ivec2 hist_base = ivec2(floor(hist_pos));
			vec2 hist_fr = hist_pos - vec2(hist_base);
			float hist_weight = 0.0;
			float depth_tolerance = borrowed ? BORROW_DEPTH_TOLERANCE : 0.1;
			for (int i = 0; i < 4; i++) {
				ivec2 off = ivec2(i & 1, i >> 1);
				ivec2 tp = hist_base + off;
				if (any(lessThan(tp, ivec2(0))) || any(greaterThanEqual(tp, params.screen_size))) {
					continue;
				}
				float w = (off.x == 1 ? hist_fr.x : 1.0 - hist_fr.x) * (off.y == 1 ? hist_fr.y : 1.0 - hist_fr.y);
				if (w <= 1e-4) {
					continue;
				}
				float prev_depth = texelFetch(prev_view_depth_texture, tp, 0).r;
				if (prev_depth <= 0.0 || abs(prev_depth - predicted_depth) > depth_tolerance * max(predicted_depth, 1.0)) {
					continue;
				}
				hist_d4 += texelFetch(history_diffuse, tp, 0) * w;
				hist_s4 += texelFetch(history_specular, tp, 0) * w;
				hist_moments += texelFetch(history_moments, tp, 0) * w;
				hist_meta += texelFetch(history_meta, tp, 0) * w;
#ifdef HAS_DIRECTIONAL
				hist_dir += texelFetch(history_directional, tp, 0) * w;
				if (dyn_split) {
					hist_dyn4 += texelFetch(history_dyn, tp, 0) * w;
				}
#endif
				hist_weight += w;
			}
			history_usable = hist_weight > 0.05;
			if (!history_usable) {
				paint_why = 3;
			}
			if (history_usable) {
				float inv_weight = 1.0 / hist_weight;
				hist_d4 *= inv_weight;
				hist_s4 *= inv_weight;
				hist_moments *= inv_weight;
				hist_meta *= inv_weight;
#ifdef HAS_DIRECTIONAL
				hist_dir *= inv_weight;
				hist_dyn4 *= inv_weight;
#endif
			}
		}
#else
		if (history_usable) {
			hist_d4 = textureLod(history_diffuse, prev_uv, 0.0);
			hist_s4 = textureLod(history_specular, prev_uv, 0.0);
			hist_moments = textureLod(history_moments, prev_uv, 0.0);
			hist_meta = textureLod(history_meta, prev_uv, 0.0);
#ifdef HAS_DIRECTIONAL
			hist_dir = textureLod(history_directional, prev_uv, 0.0);
			if (dyn_split) {
				hist_dyn4 = textureLod(history_dyn, prev_uv, 0.0);
			}
#endif
		}
#endif
#ifdef HAS_DIRECTIONAL
		// Reflection history at the virtual image's reprojection: the same
		// screen position, at the virtual depth, run through the same
		// reprojection, then blended toward the surface reprojection by
		// roughness. Fetched bilinearly with only the in-frame test: the
		// reflecting surface's stored depth says nothing about where the
		// image was, and a reflection that reprojects off frame keeps the
		// surface's history rather than restarting.
		//
		// Fetched per bilinear tap, each tap validated against the depth the
		// image is predicted at, like the surface's taps above. The image of
		// a chair leg, a railing or a sofa's edge against the window is a
		// depth edge in the reflection, and a plain bilinear fetch across it
		// blends the two sides' depths into one that matches neither; the
		// mismatch check below then restarted the reflection every frame the
		// camera moved, over every reflected edge and (a normal-mapped mirror
		// scatters those edges over its whole surface) the whole image of any
		// thin geometry: the game project's floor buzzed wherever it showed
		// the dining chairs or the upper floor's railing. The taps on the
		// image's own side of the edge carry its history; only when none
		// agrees does the mismatch restart below still fire.
		if (history_usable && virtual_weight > 0.0 && virtual_view_depth > 0.0) {
			vec2 prev_uv_v = mix(prev_uv, prev_uv_virtual, virtual_weight);
			if (all(greaterThanEqual(prev_uv_v, vec2(0.0))) && all(lessThanEqual(prev_uv_v, vec2(1.0)))) {
				vec4 hist_blend = textureLod(history_specular, prev_uv_v, 0.0);
				bool validate = predicted_virtual_depth > 0.0 && (params.flags & FLAG_SPEC_NO_MISMATCH) == 0u;
				vec4 acc = vec4(0.0);
				float acc_w = 0.0;
				if (validate) {
					vec2 hp = prev_uv_v * vec2(params.screen_size) - 0.5;
					ivec2 hb = ivec2(floor(hp));
					vec2 hf = hp - vec2(hb);
					for (int i = 0; i < 4; i++) {
						ivec2 off = ivec2(i & 1, i >> 1);
						ivec2 tp = clamp(hb + off, ivec2(0), params.screen_size - 1);
						float w = (off.x == 1 ? hf.x : 1.0 - hf.x) * (off.y == 1 ? hf.y : 1.0 - hf.y);
						if (w <= 1e-4) {
							continue;
						}
						vec4 h = texelFetch(history_specular, tp, 0);
						if (h.a > 0.0 && abs(h.a - predicted_virtual_depth) > SPEC_TAP_DEPTH_TOLERANCE * max(predicted_virtual_depth, 1.0)) {
							continue;
						}
						acc += h * w;
						acc_w += w;
					}
				}
				hist_s4 = (validate && acc_w > 0.05) ? acc / acc_w : hist_blend;
			}
		}
#endif
		if (history_usable) {
			vec3 hist_d = hist_d4.rgb;
			vec3 hist_s = hist_s4.rgb;
			float confidence_d = 1.0;
			float confidence_s = 1.0;
			if (params.clamp_gamma > 0.0) {
				float outside_d;
				float outside_s;
#ifdef HAS_DIRECTIONAL
				// GI carries real colour, so its chroma is signal and the
				// ellipsoid clip is the right one.
				hist_d = clip_to_aabb(hist_d4.rgb, mean_d, stddev_d * params.clamp_gamma, outside_d);
				hist_s = clip_to_aabb(hist_s4.rgb, mean_s, stddev_s * params.clamp_gamma, outside_s);
#else
				hist_d = clip_ratio(hist_d4.rgb, mean_d, stddev_d * params.clamp_gamma, outside_d);
				hist_s = clip_ratio(hist_s4.rgb, mean_s, stddev_s * params.clamp_gamma, outside_s);
#endif

				// History confidence: the further the history was from the
				// current neighborhood, the faster it is discarded (reduces
				// ghosting). Per signal: a noisy specular neighborhood must
				// not throw away the converged diffuse history (or vice
				// versa), and a clipping reset is not a disocclusion.
				confidence_d = clamp(1.0 - outside_d, 0.0, 1.0);
				confidence_s = clamp(1.0 - outside_s, 0.0, 1.0);
			}
			// No clamping of any kind on the sparse Monte Carlo path (see the
			// clamp_gamma comment): the rare bright samples carry all the
			// energy, and even a generous mean-relative firefly limit feeds
			// back into a progressively darker mean.
			float frames_cap = 1.0 / max(params.blend_alpha, 1e-3);
			// A young pixel's frame carries young_rays samples (the gather
			// applies the same rule to last frame's count at its own
			// reprojection): the history grows by as many, and the blend
			// below weighs the sample by as many.
			float hist_frames_d = hist_meta.r * 64.0 * confidence_d;
#ifdef HAS_DIRECTIONAL
			samples_d = hist_frames_d < 8.0 ? max(reprojection.young_rays, 1.0) : 1.0;
#endif
			frames_d = min(hist_frames_d + samples_d, frames_cap);
			frames_s = min(hist_meta.g * 64.0 * confidence_s + 1.0, frames_cap);
#ifdef HAS_DIRECTIONAL
			if (dyn_split) {
				frames_dyn = min(hist_meta.b * 64.0 * confidence_d + 1.0, frames_cap);
			}
#endif
			if (borrowed) {
				frames_d = min(frames_d, BORROW_FRAMES);
#ifdef HAS_DIRECTIONAL
				frames_dyn = min(frames_dyn, BORROW_FRAMES);
				// The lighting is continuous across the frame edge; a
				// mirror's image is not. A borrowed reflection on the game's
				// mirror floor was the bottom row's image copied up every
				// floor pixel whose history lay below the frame after a
				// flick (a yaw's motion field has a vertical component in
				// the corners): streaks along the columns, gone by +16
				// (section 59). A mirror takes its own sample instead; a
				// rough lobe's blur is as continuous as the lighting and
				// keeps the borrow.
				frames_s = min(frames_s, mix(BORROW_FRAMES, 1.0, virtual_weight));
#else
				frames_s = min(frames_s, BORROW_FRAMES);
#endif
			}
#ifdef HAS_DIRECTIONAL
			// The gather hands each pixel the largest lighting change its
			// rays landed on (the raw buffer's alpha; the cards measure it as
			// the relative change of their deterministic direct term, the
			// temporal gradient A-SVGF re-shades for). A history that
			// describes lighting that has since changed by a fraction c is
			// worth about 1 / c frames of the new one (alpha = max(alpha, c)),
			// so it restarts to that many. The mark decays in the history's
			// alpha over eight frames, where the gather's on-screen hits read
			// it, so a change propagates through the screen bounces.
			//
			// Measured and not kept: an accumulated drift in place of the
			// decaying max, restarting to 1 / sqrt(c) under a steady change.
			// It followed the slow ones closer (the game project's
			// flashlight, a cycling hue) and made a fast flicker worse, an
			// oscillation's swings counting as distance travelled. Nor a
			// correction of the history by the cards' field under the
			// surface, in its mix and its delta form (sections 27 and 67),
			// removed 2026-09-22.
			//
			// The mark decays an eighth a frame in the history. Restarting to
			// 1 / mark frames every frame it lasts gave four frames of
			// restart for one change (1, 1.14, 1.33, 1.6, 2, 2.67, 4, 8 at
			// +0..+7); the cap applies only where this frame's own mark
			// exceeds the decayed one (a new change, or a larger one), and
			// the decayed mark is still carried out for the gather's
			// propagation and the reflection's fix, which read the age, not
			// the restart. The game flick's error after the stop 5-12% lower
			// at every capture, its flicker level (section 58).
			float mark_now = change_age;
			float mark_hist = hist_d4.a - 0.125;
			change_age = max(change_age, mark_hist);
			bool changed = change_age > 0.02;
			bool mark_new = mark_now > mark_hist;
			// The moving lights' history restarts by its own mark (the
			// cards' whole change at the rays' hits), the same way; the
			// static history's mark, the static lights' alone, is what a
			// beam sweeping the level leaves untouched (section 88). The
			// reflection, one history for both, restarts by the larger.
			vec3 hist_dyn = max(hist_dyn4.rgb, vec3(0.0));
			float change_age_s = change_age;
			if (dyn_split) {
				float mark_hist_dyn = hist_dyn4.a - 0.125;
				bool mark_new_dyn = change_age_dyn > mark_hist_dyn;
				change_age_dyn = max(change_age_dyn, mark_hist_dyn);
				if (change_age_dyn > 0.02 && mark_new_dyn) {
					frames_dyn = min(frames_dyn, max(1.0, 1.0 / change_age_dyn));
				}
				change_age_s = max(change_age, change_age_dyn);
			}
			if (changed && mark_new) {
				frames_d = min(frames_d, max(1.0, 1.0 / change_age));
			}
			if (change_age_s > 0.02) {
				if ((params.flags & FLAG_SPEC_NO_CHANGE) == 0u) {
					// The reflection is one GGX sample per pixel with no
					// stand-in for its young frames (the diffuse has the
					// cards'), so it may not be restarted below a floor.
					float keep_s = max(max(1.0, 1.0 / change_age_s), params.spec_restart_min);
					// A rough lobe's history fix (reprojection.spec_fix > 0):
					// the changed fraction of the history is replaced by the
					// raw 5x5 resolve rather than by the pixel's one sample --
					// a rough reflection of the beam's spot is as wide as the
					// resolve, and the restart to one sample was the sparkle
					// the glossy ceiling showed at every stop of the
					// flashlight (the mark restarting it: ceiling error at the
					// stop 0.047 with flicker 0.023, without it 0.057 and
					// 0.013, lagging for thirty frames). A mirror keeps its
					// sample: the resolve would blur its image.
					float lobe = smoothstep(0.15, 0.4, nr_rough);
					if (reprojection.spec_fix > 0.0 && lobe > 0.0) {
						hist_s = mix(hist_s, mean_s, change_age_s * lobe);
						keep_s = max(keep_s, mix(1.0, reprojection.spec_fix, lobe));
					}
					frames_s = min(frames_s, keep_s);
				}
			}
			// The reflection's history is fetched part way between the surface
			// and its virtual image (virtual_weight), so the rest of the
			// parallax is a smear: every frame the reflected image moves that
			// many pixels over the surface while the history stays put. A
			// history of N frames has smeared over N times that. A rough lobe
			// blurs its image over many pixels anyway and hides a wider smear;
			// a glossy one shows its highlight dragging along the surface
			// (measured: a dolly through the game project's glossy room read
			// 0.044 mean error at the stop against a 0.001 floor, and the
			// whole of it was the highlights on the ceiling and walls). So the
			// frames are capped where the smear would exceed what the lobe
			// hides. Pure rotation has no parallax and keeps everything.
			{
				float smear_px = parallax_px * (1.0 - virtual_weight);
				float allowed_px = 4.0 + 12.0 * clamp(nr_rough, 0.0, 1.0);
				if (smear_px > 1e-3 && (params.flags & FLAG_SPEC_NO_SMEAR) == 0u) {
					frames_s = min(frames_s, max(allowed_px / smear_px, 1.0));
				}
			}
			// The reflection's own depth check, the counterpart of the surface
			// depth validation above: the history recorded the view depth of
			// the image it held; the image this frame's ray found is at a
			// known depth in that frame too. Where the two disagree the
			// reflected content changed -- an object moved into or out of
			// the reflection -- and a history of the old content would trail
			// behind it for the whole temporal window (there is no
			// neighbourhood clamp on this path to catch it). Only the smooth
			// end of the roughness range has a hit depth stable enough to
			// compare; the rough end's lobe lands somewhere new every frame.
			if (virtual_weight > 0.0 && predicted_virtual_depth > 0.0 && hist_s4.a > 0.0 && (params.flags & FLAG_SPEC_NO_MISMATCH) == 0u) {
				float rel = abs(hist_s4.a - predicted_virtual_depth) / max(predicted_virtual_depth, 1.0);
				float mismatch = smoothstep(0.1, 0.5, rel) * virtual_weight;
				frames_s = min(frames_s, mix(frames_cap, 2.0, mismatch));
			}
#endif
			// Never past 1: a restart below (the change mark, a borrow) can
			// leave fewer frames than this frame's samples, and a blend that
			// extrapolated ran the screen-fed loop away within thirty
			// frames (37% of the frame clipped white).
			float alpha_d = min(max(samples_d / frames_d, params.blend_alpha), 1.0);
			float alpha_s = max(1.0 / frames_s, params.blend_alpha);
#ifdef HAS_DIRECTIONAL
			if ((params.flags & FLAG_SPEC_PAINT) != 0u) {
				// Diagnostics: the reflection's frame count as a colour (red
				// under 2, green 2..8, blue above).
				current_specular = frames_s < 2.0 ? vec3(1.0, 0.0, 0.0) : (frames_s < 8.0 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
				alpha_s = 1.0;
			}
#endif

			float lum_d = luminance(current_diffuse);
			float lum_s = luminance(current_specular);
			result_diffuse = mix(hist_d, current_diffuse, alpha_d);
			result_specular = mix(hist_s, current_specular, alpha_s);
			// The sum's derived quantities (the moments, the directional
			// moment) follow the younger of the two diffuse histories: a
			// restart of either is a restart of the sum's estimate.
			float alpha_m = alpha_d;
#ifdef HAS_DIRECTIONAL
			if (dyn_split) {
				float alpha_dyn = min(max(1.0 / frames_dyn, params.blend_alpha), 1.0);
				result_dyn = mix(hist_dyn, current_dyn, alpha_dyn);
				alpha_m = max(alpha_d, alpha_dyn);
				lum_d = luminance(current_diffuse + current_dyn);
			}
			// The moment is stored unnormalized precisely so this blend is a
			// linear average: with one ray per pixel a single frame's moment
			// is a delta and useless on its own, but its running mean over
			// frames_d frames is the estimate we want. It must use the same
			// alpha as the diffuse signal it is paired with, or the ratio
			// between them stops being bounded.
			result_directional = mix(hist_dir, current_directional, alpha_m);
#endif
			moments = vec4(mix(hist_moments.xy, vec2(lum_d, lum_d * lum_d), alpha_m),
					mix(hist_moments.zw, vec2(lum_s, lum_s * lum_s), alpha_s));
			dominance = mix(hist_meta.b, dominance, alpha_d);
#ifdef HAS_DIRECTIONAL
			if (dyn_split) {
				dominance = frames_dyn / 64.0; // The meta's b carries the moving lights' frame count.
			}
#endif
			// A usable history clears the disocclusion mark over a few frames.
			// A borrowed one is still young enough to want the widened kernel
			// for a couple of frames, but not the full reset.
			// The mark counts the frames since the reveal (REVEAL_STEP a
			// frame, exact in the 8-bit meta): the spatial pass reads it as
			// "revealed within two frames" as it did the old quarter steps,
			// and the gather reads the age itself, one the change marks
			// cannot shorten (its stand-in mark, section 60).
			reveal = borrowed ? 1.0 - 2.0 * REVEAL_STEP : max(hist_meta.a - REVEAL_STEP, 0.0);
		}
	}
	if (reveal == 1.0) {
		float lum_d = luminance(current_diffuse);
		float lum_s = luminance(current_specular);
#ifdef HAS_DIRECTIONAL
		if (dyn_split) {
			lum_d = luminance(current_diffuse + current_dyn);
			dominance = 1.0 / 64.0;
		}
#endif
		moments = vec4(lum_d, lum_d * lum_d, lum_s, lum_s * lum_s);
	}

	// The history never holds a non-finite value: it would keep it for the
	// whole temporal window, and the spatial filter's taps would carry it to
	// the neighbors, a stride further every frame (growing black voids). A
	// poisoned history restarts at this pixel instead.
	if (any(isnan(result_diffuse)) || any(isinf(result_diffuse))) {
		result_diffuse = vec3(0.0);
		frames_d = 0.0;
	}
	if (any(isnan(result_specular)) || any(isinf(result_specular))) {
		result_specular = vec3(0.0);
		frames_s = 0.0;
	}
#ifdef HAS_DIRECTIONAL
	if (any(isnan(result_dyn)) || any(isinf(result_dyn))) {
		result_dyn = vec3(0.0);
		dominance = 0.0;
	}
#endif
	if (any(isnan(moments)) || any(isinf(moments))) {
		moments = vec4(0.0);
	}
#ifdef HAS_DIRECTIONAL
	if (any(isnan(result_directional)) || any(isinf(result_directional))) {
		result_directional = vec4(0.0, 0.0, 0.0, 1.0);
	}
	if ((params.flags & FLAG_SPEC_PAINT_WHY) != 0u && paint_why != 0) {
		// The diffuse takes the reflection's paint (below) for the four
		// verdicts, so they show on a rough surface too (a gray box's
		// specular is too faint to read).
		result_diffuse = paint_why == 1 ? vec3(1.0, 0.0, 0.0) : (paint_why == 2 ? vec3(1.0, 1.0, 0.0) : (paint_why == 3 ? vec3(1.0, 0.0, 1.0) : vec3(0.0, 1.0, 1.0)));
	}
	imageStore(out_diffuse, pixel, vec4(result_diffuse, clamp(change_age, 0.0, 1.0)));
	if (dyn_split) {
		imageStore(out_dyn, pixel, vec4(result_dyn, clamp(change_age_dyn, 0.0, 1.0)));
		// The sum's alpha: the moving lights' luminance, which the spatial
		// iterations filter along with the sum (its share of the filtered
		// value is what the stand-in replaces; see store_result).
		imageStore(out_sum, pixel, vec4(result_diffuse + result_dyn, luminance(result_dyn)));
	}
#else
	imageStore(out_diffuse, pixel, vec4(result_diffuse, 0.0));
#endif
#ifdef HAS_DIRECTIONAL
	// The reflection's virtual view depth rides in the alpha for next frame's
	// depth check (the spatial pass reads only the colour).
	if ((params.flags & FLAG_SPEC_PAINT_WHY) != 0u) {
		// Diagnostics: red history off frame, yellow borrowed, magenta every
		// depth tap failed, cyan a velocity-classified moving object; else
		// the reflection's frames in red and the diffuse's in green, over 32.
		result_specular = paint_why == 1 ? vec3(1.0, 0.0, 0.0) : (paint_why == 2 ? vec3(1.0, 1.0, 0.0) : (paint_why == 3 ? vec3(1.0, 0.0, 1.0) : (paint_why == 4 ? vec3(0.0, 1.0, 1.0) : vec3(frames_s / 32.0, frames_d / 32.0, 0.0))));
	}
	imageStore(out_specular, pixel, vec4(result_specular, min(virtual_view_depth, 30000.0)));
#else
	imageStore(out_specular, pixel, vec4(result_specular, 0.0));
#endif
	imageStore(out_moments, pixel, moments);
	imageStore(out_meta, pixel, vec4(frames_d / 64.0, frames_s / 64.0, dominance, reveal));
#ifdef HAS_DIRECTIONAL
	// Stored unscaled: the spatial pass applies the convergence ramp, so the
	// history this feeds back stays an honest running mean.
	imageStore(out_directional, pixel, result_directional);
#endif
}

#else // MODE_SPATIAL

// Multiplies the analytic lighting back into the filtered visibility ratios
// (direct lighting only; GI filters radiance directly).
//
// p_confidence fades the directional term in as the temporal accumulation
// converges. With one ray per pixel the instantaneous moment is a delta -- its
// length always equals the luminance, reading as "all light comes from exactly
// one direction" on every pixel of every frame. Only the running mean over
// many frames is meaningful, so a young pixel is pulled back toward the
// isotropic ratio (2/3, the value a uniform hemisphere produces under
// cosine-weighted sampling) and the consumer sees a flat irradiance instead of
// a wild reconstruction. Scaling the moment by a positive scalar is safe: it
// scales the ratio by the same factor and cannot break its bound.
//
// The visibility in w gets no such ramp. It is a plain mean like the
// irradiance beside it, filtered by the same kernel, and a young pixel's value
// is as honest as its irradiance. Faded in from 1 it made the specular
// occlusion arrive late everywhere the history had restarted: after every
// camera move the reflections were unoccluded, then darkened over the next
// dozen frames -- "dark patches slowly appearing".
// p_fallback_weight (GI, last iteration only): how much of the card's
// bounce irradiance under the surface stands in for the pixel's still-young
// filtered history. This output is not fed back into the history (only
// through the screen radiance the next gather reads, at a bounce's weight),
// so the fade costs the convergence nothing; measured on
// rt_lab/temporal_suite.sh game_fixed_flick (14 degrees a frame).
// Under FLAG_DYN_SPLIT the two histories fade in on their own (p_fallback_weight
// the static one's youth, p_fallback_weight_dyn the moving lights'): each
// takes its own share of the filtered value (p_dyn_lum the moving lights'
// luminance in it, filtered with it) for its own share of the stand-in, so
// a beam sweeping the level replaces its own term with the cards' estimate
// of that term and leaves the converged rest as the screen has it. The share
// is the screen's, not the cards': apportioned by the cards' split, half of
// a one-sample spike of the moving lights' term survived the fade (the
// colour case's hot pixels x32, section 88).
void store_result(ivec2 pixel, vec3 d, vec3 s, vec4 dir, float p_confidence, float p_fallback_weight, float p_fallback_weight_dyn, float p_dyn_lum) {
	float spec_fresnel_weight = 0.0;
	if ((params.flags & FLAG_MODULATE_ANALYTIC) != 0u) {
		d *= texelFetch(analytic_diffuse, pixel, 0).rgb;
		vec4 analytic_s = texelFetch(analytic_specular, pixel, 0);
		s *= analytic_s.rgb;
		spec_fresnel_weight = analytic_s.a;
	}
#if defined(FILTER_DIRECTIONAL) && !defined(SPATIAL_HDR_OUT)
	if ((params.flags & FLAG_FALLBACK_ALL) != 0u) {
		vec4 fb = texelFetch(fallback_texture, pixel, 0);
		d = fb.a > 0.0 ? fb.rgb : vec3(0.0);
	} else if (p_fallback_weight > 0.0 || p_fallback_weight_dyn > 0.0) {
		vec4 fb = texelFetch(fallback_texture, pixel, 0);
		// The card's own accumulation counts too: a texel relit once is no
		// better than the pixel's sample.
		float trust = clamp(fb.a * 64.0 / max(params.fallback_ramp, 1.0), 0.0, 1.0);
		if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
			vec3 fb_dyn = max(texelFetch(fallback_dyn_texture, pixel, 0).rgb, vec3(0.0));
			vec3 fb_static = max(fb.rgb - fb_dyn, vec3(0.0));
			float share = clamp(p_dyn_lum / max(luminance(d), 1e-6), 0.0, 1.0);
			float w_s = p_fallback_weight * trust;
			float w_d = p_fallback_weight_dyn * trust;
			d = d * (1.0 - w_s * (1.0 - share) - w_d * share) + w_s * fb_static + w_d * fb_dyn;
		} else {
			d = mix(d, fb.rgb, p_fallback_weight * trust);
		}
	}
#endif
#if defined(FILTER_DIRECTIONAL) && defined(SPATIAL_HDR_OUT)
	// The moving lights' luminance rides along to the next iteration (FLAG_DYN_SPLIT; 0 otherwise, as before).
	imageStore(out_diffuse, pixel, vec4(d, (params.flags & FLAG_DYN_SPLIT) != 0u ? p_dyn_lum : 0.0));
#else
	imageStore(out_diffuse, pixel, vec4(d, 0.0));
#endif
	imageStore(out_specular, pixel, vec4(s, spec_fresnel_weight));
#if defined(FILTER_DIRECTIONAL) && defined(SPATIAL_HDR_OUT)
	// Intermediate iteration: carry the moment through unchanged, the last one
	// renormalizes it against the irradiance it will actually be paired with.
	imageStore(out_directional, pixel, dir);
#elif defined(FILTER_DIRECTIONAL)
	float l0 = max(luminance(d), 1e-6);
	float len = length(dir.xyz);
	float ratio = mix(2.0 / 3.0, len / l0, p_confidence);
	dir.xyz = len > 1e-9 ? dir.xyz * (ratio * l0 / len) : vec3(0.0);
	imageStore(out_directional, pixel, vec4(dir.xyz, dir.w));
#endif
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_texture, pixel * params.depth_scale, 0).r;
	vec4 center_d4 = texelFetch(in_diffuse, pixel, 0);
	vec4 center_s4 = texelFetch(in_specular, pixel, 0);
#ifdef FILTER_DIRECTIONAL
	vec4 center_dir = texelFetch(in_directional, pixel, 0);
#else
	vec4 center_dir = vec4(0.0, 0.0, 0.0, 1.0);
#endif
	if (center_depth == 0.0) {
		imageStore(out_diffuse, pixel, vec4(0.0));
		imageStore(out_specular, pixel, vec4(0.0));
#ifdef FILTER_DIRECTIONAL
		imageStore(out_directional, pixel, vec4(0.0, 0.0, 0.0, 1.0));
#endif
#ifdef MOMENTS_OUTPUT
		imageStore(out_moments, pixel, vec4(0.0));
#endif
		return;
	}

	// Denoiser disabled (sentinel threshold): pass the input through.
	if (params.variance_threshold >= 1e5) {
		store_result(pixel, center_d4.rgb, center_s4.rgb, center_dir, 1.0, 0.0, 0.0, center_d4.a);
#ifdef MOMENTS_OUTPUT
		imageStore(out_moments, pixel, texelFetch(moments_texture, pixel, 0));
#endif
		return;
	}

	vec4 moments = texelFetch(moments_texture, pixel, 0);
	float var_d = max(moments.y - moments.x * moments.x, 0.0);
	float var_s = max(moments.w - moments.z * moments.z, 0.0);

	// Only filter where the signal is actually noisy relative to its
	// magnitude; elsewhere the temporal result is already converged and
	// filtering would only cost sharpness. Also skip where a single light
	// carried ~80%+ of the energy (shading confidence) AND the raw signal is
	// itself steady: fully lit or fully shadowed under a dominant light is
	// nearly binary, converges fast temporally, and spatial filtering would
	// only soften the edge. Its penumbra is the opposite case (high raw
	// variance that the capped temporal accumulation can never average out),
	// so the dominance skip must not apply there.
	float rel_d = var_d / max(moments.x * moments.x, 1e-6);
	float rel_s = var_s / max(moments.z * moments.z, 1e-6);
	vec4 meta = texelFetch(meta_texture, pixel, 0);
	float frames_d = meta.r * 64.0;
	float frames_s = meta.g * 64.0;
	float dominance = meta.b;
	// The moving lights' history (FLAG_DYN_SPLIT): the kernel and the
	// directional ramp follow the younger of the two diffuse histories (the
	// sum's variance restarted with either), the stand-in each one's own.
	float frames_d_static = frames_d;
	float frames_dyn = frames_d;
	if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
		frames_dyn = meta.b * 64.0;
		frames_d = min(frames_d, frames_dyn);
		dominance = 0.0;
	}
	// Only a true disocclusion (history rejected, not merely clipped) widens
	// the kernel and drops the luminance stop below; the variance estimate is
	// meaningless there. A signal whose accumulation is still young (reset by
	// history clipping) also has no usable variance yet, so it filters
	// unconditionally at normal stride until a few frames have accumulated.
	bool newly_revealed = meta.a > 1.0 - 2.5 * REVEAL_STEP;
	bool young_d = frames_d < 4.0;
	bool young_s = frames_s < 4.0;
	// A young GI pixel's hit-distance term is one or two rays' worth: it
	// must not shape the kernel (below, the stride tightening and the tap
	// weight both read it), or the entering band of a fast turn keeps its
	// single samples as sparkle. The luminance stop stays off as long.
#ifdef FILTER_DIRECTIONAL
	float youth = clamp(1.0 - (frames_d - 1.0) / (YOUNG_FRAMES - 1.0), 0.0, 1.0);
	young_d = young_d || youth > 0.0;
	float youth_static = clamp(1.0 - (frames_d_static - 1.0) / (YOUNG_FRAMES - 1.0), 0.0, 1.0);
	float youth_dyn = clamp(1.0 - (frames_dyn - 1.0) / (YOUNG_FRAMES - 1.0), 0.0, 1.0);
#else
	float youth = 0.0;
	float youth_static = 0.0;
	float youth_dyn = 0.0;
#endif
	bool filter_d = newly_revealed || young_d || (rel_d >= params.variance_threshold && !(dominance > 0.8 && frames_d >= 8.0 && rel_d < 0.25));
	bool filter_s = newly_revealed || (young_s && (params.flags & FLAG_SPEC_NO_YOUNG) == 0u) || (rel_s >= params.variance_threshold && !(dominance > 0.8 && frames_s >= 8.0 && rel_s < 0.25));
	if ((params.flags & FLAG_SPATIAL_OFF) != 0u) {
		filter_d = false;
		filter_s = false;
	}
	// Ramp the directional term in over the first frames of accumulation.
	float dir_confidence = clamp((frames_d - 4.0) * 0.125, 0.0, 1.0);
	// And the cards' stand-in out (GI only; see store_result).
	float fallback_weight = youth_static;
	float fallback_weight_dyn = youth_dyn;
	if (!filter_d && !filter_s) {
		store_result(pixel, center_d4.rgb, center_s4.rgb, center_dir, dir_confidence, fallback_weight, fallback_weight_dyn, center_d4.a);
#ifdef MOMENTS_OUTPUT
		// Nothing was filtered: the moments this pixel hands to the next
		// iteration are still the ones that describe its signal.
		imageStore(out_moments, pixel, moments);
#endif
		return;
	}

	// Depth edge stopping, as the raw-buffer window matching a relative
	// view-space tolerance around this pixel.
	float center_view_depth = linearize_depth(center_depth);
	float depth_bound_a = depth_from_linear(center_view_depth * (1.0 - params.depth_tolerance));
	float depth_bound_b = depth_from_linear(center_view_depth * (1.0 + params.depth_tolerance));
	float depth_min = min(depth_bound_a, depth_bound_b);
	float depth_max = max(depth_bound_a, depth_bound_b);
	vec3 center_normal = nr_normal(texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0));
	float sigma_d = 4.0 * sqrt(var_d) + 1e-4;
	float sigma_s = 4.0 * sqrt(var_s) + 1e-4;

	// Newly revealed pixels have no usable variance estimate yet, so widen the
	// kernel and ignore the luminance stopping function for a few frames.
	int stride = newly_revealed ? params.stride * 2 : params.stride;

	int stride_s = stride;
#ifdef FILTER_DIRECTIONAL
	// Where the gather's rays hit close by, the irradiance varies over the same
	// short scale (the contact darkening under and beside objects), so a wide
	// kernel would average that detail away. Tighten the footprint in
	// proportion to how far the rays actually got.
	stride = max(1, int(round(float(stride) * mix(max(center_dir.w, 0.25), 1.0, youth))));
	// The reflection's kernel follows roughness: a rough lobe is as wide as
	// the diffuse one, a mirror's image must not be filtered at all.
	{
		float r = nr_roughness(texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0));
		float spec_scale = clamp(r / 0.35, 0.0, 1.0);
		if (spec_scale < 0.25) {
			// A mirror's image is never filtered. Filtering a young mirror
			// pixel at stride 1 on the first iteration was measured without
			// gain on the slow floor-yaw's entering band (section 64).
			filter_s = false;
			stride_s = 1;
		} else {
			stride_s = max(1, int(round(float(stride_s) * spec_scale)));
		}
	}
	if (!filter_d && !filter_s) {
		store_result(pixel, center_d4.rgb, center_s4.rgb, center_dir, dir_confidence, fallback_weight, fallback_weight_dyn, center_d4.a);
#ifdef MOMENTS_OUTPUT
		imageStore(out_moments, pixel, moments);
#endif
		return;
	}
#endif

	// Rotate the sparse kernel per pixel so its footprint does not imprint a
	// grid pattern on the result; the rotation is static (not per frame) to
	// avoid shimmer after temporal accumulation.
	float angle = fract(dot(vec2(pixel), vec2(0.7548776662, 0.5698402909))) * 6.2831853;
	mat2 rot = mat2(vec2(cos(angle), -sin(angle)), vec2(sin(angle), cos(angle)));

	vec3 sum_d = center_d4.rgb;
	float sum_da = center_d4.a; // The moving lights' luminance (FLAG_DYN_SPLIT), filtered with the sum.
	vec3 sum_s = center_s4.rgb;
	vec4 sum_dir = center_dir;
	float weight_d = 1.0;
	float weight_s = 1.0;
	// The next iteration's variance is that of the weighted average this one
	// outputs (SVGF 4.3): the mean rides the weights, the variance the squared
	// weights, so it drops by the kernel's effective tap count wherever the
	// neighborhood agrees. Averaging the raw moments instead (the Q2RTX
	// construction) leaves the variance of a flat region untouched and raises
	// it at edges by the neighborhood's spread, which loosens the luminance
	// stops exactly where they matter; measured 3-9% worse on the game
	// project, so it is not what is done here. The center is at weight 1.
	float sum_m1_d = moments.x;
	float sum_m1_s = moments.z;
	float sum_var_d = var_d;
	float sum_var_s = var_s;
	float sum_w2_d = 1.0;
	float sum_w2_s = 1.0;

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 sp = clamp(pixel + ivec2(round(rot * (vec2(x, y) * float(stride)))), ivec2(0), params.screen_size - 1);
			float sd = texelFetch(depth_texture, sp * params.depth_scale, 0).r;
			// SVGF edge-stopping functions: depth, normal and luminance. The
			// depth test is the view-space tolerance expressed as a raw-depth
			// window, so it means the same thing at every range.
			float w_spatial = 0.0;
			if (sd != 0.0 && sd >= depth_min && sd <= depth_max) {
				vec3 n = nr_normal(texelFetch(normal_roughness_texture, sp * params.depth_scale, 0));
				float w_normal = pow(max(dot(center_normal, n), 0.0), 32.0);
				w_spatial = exp(-0.3 * float(x * x + y * y)) * w_normal;
			}
			// The specular taps sit on their own stride.
			ivec2 sp_s = sp;
			float w_spatial_s = w_spatial;
			if (stride_s != stride) {
				sp_s = clamp(pixel + ivec2(round(rot * (vec2(x, y) * float(stride_s)))), ivec2(0), params.screen_size - 1);
				float sd_s = texelFetch(depth_texture, sp_s * params.depth_scale, 0).r;
				w_spatial_s = 0.0;
				if (sd_s != 0.0 && sd_s >= depth_min && sd_s <= depth_max) {
					vec3 n_s = nr_normal(texelFetch(normal_roughness_texture, sp_s * params.depth_scale, 0));
					w_spatial_s = exp(-0.3 * float(x * x + y * y)) * pow(max(dot(center_normal, n_s), 0.0), 32.0);
				}
			}
			if (w_spatial <= 0.0 && w_spatial_s <= 0.0) {
				continue;
			}

			vec4 d4 = texelFetch(in_diffuse, sp, 0);
			vec3 d = d4.rgb;
			vec3 s = texelFetch(in_specular, sp_s, 0).rgb;

			float wd = w_spatial;
			float ws = w_spatial_s;
			if (!newly_revealed && !young_d) {
				wd *= exp(-abs(luminance(d) - moments.x) / sigma_d);
			}
			if (!newly_revealed && !young_s) {
				ws *= exp(-abs(luminance(s) - moments.z) / sigma_s);
			}
#ifdef FILTER_DIRECTIONAL
			// Neighbors whose rays travelled a very different distance are
			// looking at a different part of the scene even where depth and
			// normal agree, so they must not average together.
			vec4 sdir = texelFetch(in_directional, sp, 0);
			wd *= exp(-abs(sdir.w - center_dir.w) * 4.0 * (1.0 - youth));
#endif

			sum_d += d * wd;
			sum_da += d4.a * wd;
			sum_s += s * ws;
			weight_d += wd;
			weight_s += ws;
#ifdef MOMENTS_OUTPUT
			vec4 m = texelFetch(moments_texture, sp, 0);
			sum_m1_d += m.x * wd;
			sum_var_d += max(m.y - m.x * m.x, 0.0) * wd * wd;
			sum_w2_d += wd * wd;
			if (stride_s != stride) {
				m = texelFetch(moments_texture, sp_s, 0);
			}
			sum_m1_s += m.z * ws;
			sum_var_s += max(m.w - m.z * m.z, 0.0) * ws * ws;
			sum_w2_s += ws * ws;
#endif
#ifdef FILTER_DIRECTIONAL
			// Same weight as the diffuse signal, deliberately: the pair only
			// stays consistent while both are averaged identically.
			sum_dir += sdir * wd;
#endif
		}
	}

#ifdef MOMENTS_OUTPUT
	// A skipped signal keeps its own moments (its output is its input).
	vec2 out_mom_d = moments.xy;
	vec2 out_mom_s = moments.zw;
	if (filter_d) {
		float m1 = sum_m1_d / weight_d;
		out_mom_d = vec2(m1, m1 * m1 + sum_var_d / (weight_d * weight_d));
	}
	if (filter_s) {
		float m1 = sum_m1_s / weight_s;
		out_mom_s = vec2(m1, m1 * m1 + sum_var_s / (weight_s * weight_s));
	}
	imageStore(out_moments, pixel, vec4(out_mom_d, out_mom_s));
#endif

	store_result(pixel, filter_d ? sum_d / weight_d : center_d4.rgb,
			filter_s ? sum_s / weight_s : center_s4.rgb,
			filter_d ? sum_dir / weight_d : center_dir, dir_confidence, fallback_weight, fallback_weight_dyn, filter_d ? sum_da / weight_d : center_d4.a);
}

#endif
