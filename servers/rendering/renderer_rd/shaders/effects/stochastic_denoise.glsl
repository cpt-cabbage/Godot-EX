#[compute]

#version 450

#VERSION_DEFINES

// Denoiser for the stochastic direct lighting buffers, following the
// MegaLights / SVGF structure: temporal accumulation of lighting and its
// luminance moments (giving a per-pixel variance estimate), then a single
// variance-driven spatial pass using depth, normal and variance edge-stopping.
// The two filtered signals are demodulated: for direct lighting they are
// bounded [0;1] visibility ratios (the analytic lighting is multiplied back
// in at the end of the spatial pass), for GI they are radiance.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Params.flags bits.
#define FLAG_HAS_VELOCITY 1u // A real velocity buffer is bound (else the binding is a dummy and must not be fetched).
#define FLAG_HAS_META 2u // Temporal: raw_meta is a real shading-confidence texture.
#define FLAG_MODULATE_ANALYTIC 4u // Spatial: multiply the filtered ratios by the analytic lighting buffers.

// The GI signal carries a directional companion buffer (first radiance moment
// + near-field visibility) that must be blended and filtered with exactly the
// diffuse weights. It rides on the GI-only variants (VALIDATE_DEPTH for the
// temporal pass, FILTER_DIRECTIONAL for the spatial one) rather than a runtime
// flag, so the direct lighting variants keep their existing set layouts.
#ifdef VALIDATE_DEPTH
#define HAS_DIRECTIONAL
// The GI accumulates unbounded HDR radiance through a 32-frame feedback loop,
// where the packed format's rounding compounds; direct lighting accumulates
// bounded [0;1] visibility ratios and stays packed.
#define LIGHTING_FORMAT rgba16f
#else
#define LIGHTING_FORMAT r11f_g11f_b10f
#endif
#ifdef FILTER_DIRECTIONAL
#define HAS_DIRECTIONAL
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
#ifdef VALIDATE_DEPTH
// The signal's own view depth from the previous frame (at the signal's
// resolution), for disocclusion rejection when history clipping is off.
layout(set = 0, binding = 9) uniform sampler2D prev_view_depth_texture;
// This frame's directional term and its history.
layout(set = 0, binding = 10) uniform sampler2D in_directional;
layout(set = 0, binding = 11) uniform sampler2D history_directional;
#endif

layout(set = 1, binding = 0, LIGHTING_FORMAT) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, LIGHTING_FORMAT) uniform restrict writeonly image2D out_specular;
// xy: diffuse luminance 1st/2nd moment, zw: specular.
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_moments;
// r: diffuse frames / 64, g: specular frames / 64, b: shading confidence,
// a: disocclusion mark (decays over a few frames).
layout(set = 1, binding = 3, rgba8) uniform restrict writeonly image2D out_meta;
// The previous frame pair's reprojection (previous NDC -> the frame before).
// The velocity buffer is one frame stale (written by the previous color pass),
// so this is the camera-only motion it was rendered with: static pixels match
// it and only genuinely moving objects deviate.
layout(set = 1, binding = 4, std140) uniform ReprojectUBO {
	mat4 prev_reproject;
}
reprojection;
#ifdef HAS_DIRECTIONAL
layout(set = 1, binding = 5, rgba16f) uniform restrict writeonly image2D out_directional;
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
#endif

// The spatial pass writes the final buffers, which stay packed on both paths:
// they are written once and read four times per fragment by the upsample, so
// the rounding never compounds.
layout(set = 1, binding = 0, r11f_g11f_b10f) uniform restrict writeonly image2D out_diffuse;
layout(set = 1, binding = 1, r11f_g11f_b10f) uniform restrict writeonly image2D out_specular;
#ifdef FILTER_DIRECTIONAL
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image2D out_directional;
#endif
#endif

layout(push_constant, std430) uniform Params {
	mat4 reproject; // Current NDC -> previous frame NDC (temporal only).
	ivec2 screen_size;
	float blend_alpha; // Minimum weight of the current frame.
	float depth_tolerance;
	float variance_threshold; // Relative variance below which filtering is skipped.
	int stride; // Spatial kernel stride.
	int depth_scale; // 2 when the lighting buffers are half resolution.
	// Neighborhood clamp width in standard deviations; <= 0 disables history
	// clipping entirely. Dense signals (the direct lighting ratio) use ~1.5;
	// sparse Monte Carlo signals (the GI gather) must not clamp: a 5x5
	// neighborhood that catches no bright sample this frame would clip a
	// perfectly converged history to black every frame (the same failure the
	// STB lighting talk hits with TAA color clamping on noisy input).
	float clamp_gamma;
	// Camera planes, for linearizing the reprojected depth (VALIDATE_DEPTH).
	float z_near;
	float z_far;
	uint flags;
	uint pad2;
}
params;

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
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
#endif
		return;
	}

	vec3 current_diffuse = texelFetch(in_diffuse, pixel, 0).rgb;
	vec3 current_specular = texelFetch(in_specular, pixel, 0).rgb;
#ifdef HAS_DIRECTIONAL
	vec4 current_directional = texelFetch(in_directional, pixel, 0);
	vec4 result_directional = current_directional;
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
	float frames_s = 1.0;
	// Disocclusion mark for the spatial pass; set until a usable history
	// proves the pixel is not freshly revealed.
	float reveal = 1.0;
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
		if ((params.flags & FLAG_HAS_VELOCITY) != 0u) {
			vec2 velocity = texelFetch(velocity_texture, pixel * params.depth_scale, 0).xy;
			vec4 prevprev_ndc = reprojection.prev_reproject * vec4(prev_ndc.xyz / prev_ndc.w, 1.0);
			if (velocity != vec2(0.0) && prevprev_ndc.w > 0.0) {
				vec2 static_motion = (prevprev_ndc.xy / prevprev_ndc.w) * 0.5 + 0.5 - prev_uv;
				// Threshold of 2 full-resolution pixels: the velocity buffer is
				// unjittered while the reprojection matrices carry the TAA
				// jitter of both frames.
				vec2 object_pixels = (velocity - static_motion) * vec2(params.screen_size * params.depth_scale);
				if (any(greaterThan(abs(object_pixels), vec2(2.0)))) {
					prev_uv = uv + velocity;
				}
			}
		}
		bool history_usable = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
#ifdef VALIDATE_DEPTH
		if (history_usable) {
			// Disocclusion check: without history clipping, a revealed pixel
			// would otherwise inherit whatever surface used to be there. The
			// signal recorded its own view depth last frame; compare it to
			// where this surface reprojects to.
			ivec2 prev_pixel = clamp(ivec2(prev_uv * vec2(params.screen_size)), ivec2(0), params.screen_size - 1);
			float prev_depth = texelFetch(prev_view_depth_texture, prev_pixel, 0).r;
			float prev_ndc_z = clamp(prev_ndc.z / prev_ndc.w, 0.0, 1.0) * 2.0 - 1.0;
			float predicted_depth = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + prev_ndc_z * (params.z_far - params.z_near));
			if (prev_depth <= 0.0 || abs(prev_depth - predicted_depth) > 0.1 * max(predicted_depth, 1.0)) {
				history_usable = false;
			}
		}
#endif
		if (history_usable) {
			vec4 hist_d4 = textureLod(history_diffuse, prev_uv, 0.0);
			vec4 hist_s4 = textureLod(history_specular, prev_uv, 0.0);
			vec4 hist_moments = textureLod(history_moments, prev_uv, 0.0);
			vec4 hist_meta = textureLod(history_meta, prev_uv, 0.0);

			vec3 hist_d = hist_d4.rgb;
			vec3 hist_s = hist_s4.rgb;
			float confidence_d = 1.0;
			float confidence_s = 1.0;
			if (params.clamp_gamma > 0.0) {
				float outside_d;
				float outside_s;
				hist_d = clip_to_aabb(hist_d4.rgb, mean_d, stddev_d * params.clamp_gamma, outside_d);
				hist_s = clip_to_aabb(hist_s4.rgb, mean_s, stddev_s * params.clamp_gamma, outside_s);

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
			frames_d = min(hist_meta.r * 64.0 * confidence_d + 1.0, frames_cap);
			frames_s = min(hist_meta.g * 64.0 * confidence_s + 1.0, frames_cap);
			float alpha_d = max(1.0 / frames_d, params.blend_alpha);
			float alpha_s = max(1.0 / frames_s, params.blend_alpha);

			float lum_d = luminance(current_diffuse);
			float lum_s = luminance(current_specular);
			result_diffuse = mix(hist_d, current_diffuse, alpha_d);
			result_specular = mix(hist_s, current_specular, alpha_s);
#ifdef HAS_DIRECTIONAL
			// The moment is stored unnormalized precisely so this blend is a
			// linear average: with one ray per pixel a single frame's moment
			// is a delta and useless on its own, but its running mean over
			// frames_d frames is the estimate we want. It must use the same
			// alpha as the diffuse signal it is paired with, or the ratio
			// between them stops being bounded.
			result_directional = mix(textureLod(history_directional, prev_uv, 0.0), current_directional, alpha_d);
#endif
			moments = vec4(mix(hist_moments.xy, vec2(lum_d, lum_d * lum_d), alpha_d),
					mix(hist_moments.zw, vec2(lum_s, lum_s * lum_s), alpha_s));
			dominance = mix(hist_meta.b, dominance, alpha_d);
			// A usable history clears the disocclusion mark over a few frames.
			reveal = max(hist_meta.a - 0.25, 0.0);
		}
	}
	if (reveal == 1.0) {
		float lum_d = luminance(current_diffuse);
		float lum_s = luminance(current_specular);
		moments = vec4(lum_d, lum_d * lum_d, lum_s, lum_s * lum_s);
	}

	imageStore(out_diffuse, pixel, vec4(result_diffuse, 0.0));
	imageStore(out_specular, pixel, vec4(result_specular, 0.0));
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
void store_result(ivec2 pixel, vec3 d, vec3 s, vec4 dir, float p_confidence) {
	if ((params.flags & FLAG_MODULATE_ANALYTIC) != 0u) {
		d *= texelFetch(analytic_diffuse, pixel, 0).rgb;
		s *= texelFetch(analytic_specular, pixel, 0).rgb;
	}
	imageStore(out_diffuse, pixel, vec4(d, 0.0));
	imageStore(out_specular, pixel, vec4(s, 0.0));
#ifdef FILTER_DIRECTIONAL
	float l0 = max(luminance(d), 1e-6);
	float len = length(dir.xyz);
	float ratio = mix(2.0 / 3.0, len / l0, p_confidence);
	dir.xyz = len > 1e-9 ? dir.xyz * (ratio * l0 / len) : vec3(0.0);
	imageStore(out_directional, pixel, vec4(dir.xyz, mix(1.0, dir.w, p_confidence)));
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
		return;
	}

	// Denoiser disabled (sentinel threshold): pass the input through.
	if (params.variance_threshold >= 1e5) {
		store_result(pixel, center_d4.rgb, center_s4.rgb, center_dir, 1.0);
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
	// Only a true disocclusion (history rejected, not merely clipped) widens
	// the kernel and drops the luminance stop below; the variance estimate is
	// meaningless there. A signal whose accumulation is still young (reset by
	// history clipping) also has no usable variance yet, so it filters
	// unconditionally at normal stride until a few frames have accumulated.
	bool newly_revealed = meta.a > 0.25;
	bool young_d = frames_d < 4.0;
	bool young_s = frames_s < 4.0;
	bool filter_d = newly_revealed || young_d || (rel_d >= params.variance_threshold && !(dominance > 0.8 && frames_d >= 8.0 && rel_d < 0.25));
	bool filter_s = newly_revealed || young_s || (rel_s >= params.variance_threshold && !(dominance > 0.8 && frames_s >= 8.0 && rel_s < 0.25));
	// Ramp the directional term in over the first frames of accumulation.
	float dir_confidence = clamp((frames_d - 4.0) * 0.125, 0.0, 1.0);
	if (!filter_d && !filter_s) {
		store_result(pixel, center_d4.rgb, center_s4.rgb, center_dir, dir_confidence);
		return;
	}

	vec3 center_normal = normalize(texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0).xyz * 2.0 - 1.0);
	float sigma_d = 4.0 * sqrt(var_d) + 1e-4;
	float sigma_s = 4.0 * sqrt(var_s) + 1e-4;

	// Newly revealed pixels have no usable variance estimate yet, so widen the
	// kernel and ignore the luminance stopping function for a few frames.
	int stride = newly_revealed ? params.stride * 2 : params.stride;

#ifdef FILTER_DIRECTIONAL
	// Where the gather's rays hit close by, the irradiance varies over the same
	// short scale (the contact darkening under and beside objects), so a wide
	// kernel would average that detail away. Tighten the footprint in
	// proportion to how far the rays actually got.
	stride = max(1, int(round(float(stride) * max(center_dir.w, 0.25))));
#endif

	// Rotate the sparse kernel per pixel so its footprint does not imprint a
	// grid pattern on the result; the rotation is static (not per frame) to
	// avoid shimmer after temporal accumulation.
	float angle = fract(dot(vec2(pixel), vec2(0.7548776662, 0.5698402909))) * 6.2831853;
	mat2 rot = mat2(vec2(cos(angle), -sin(angle)), vec2(sin(angle), cos(angle)));

	vec3 sum_d = center_d4.rgb;
	vec3 sum_s = center_s4.rgb;
	vec4 sum_dir = center_dir;
	float weight_d = 1.0;
	float weight_s = 1.0;

	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 sp = clamp(pixel + ivec2(round(rot * (vec2(x, y) * float(stride)))), ivec2(0), params.screen_size - 1);
			float sd = texelFetch(depth_texture, sp * params.depth_scale, 0).r;
			if (sd == 0.0) {
				continue;
			}

			// SVGF edge-stopping functions: depth, normal and luminance.
			float depth_diff = abs(sd - center_depth) / max(center_depth, 1e-6);
			if (depth_diff >= params.depth_tolerance) {
				continue;
			}
			vec3 n = normalize(texelFetch(normal_roughness_texture, sp * params.depth_scale, 0).xyz * 2.0 - 1.0);
			float w_normal = pow(max(dot(center_normal, n), 0.0), 32.0);
			if (w_normal <= 0.0) {
				continue;
			}
			float w_spatial = exp(-0.3 * float(x * x + y * y)) * w_normal;

			vec3 d = texelFetch(in_diffuse, sp, 0).rgb;
			vec3 s = texelFetch(in_specular, sp, 0).rgb;

			float wd = w_spatial;
			float ws = w_spatial;
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
			wd *= exp(-abs(sdir.w - center_dir.w) * 4.0);
#endif

			sum_d += d * wd;
			sum_s += s * ws;
			weight_d += wd;
			weight_s += ws;
#ifdef FILTER_DIRECTIONAL
			// Same weight as the diffuse signal, deliberately: the pair only
			// stays consistent while both are averaged identically.
			sum_dir += sdir * wd;
#endif
		}
	}

	store_result(pixel, filter_d ? sum_d / weight_d : center_d4.rgb,
			filter_s ? sum_s / weight_s : center_s4.rgb,
			filter_d ? sum_dir / weight_d : center_dir, dir_confidence);
}

#endif
