#[compute]

#version 450

#VERSION_DEFINES

// Volumetric clouds (Godot-EX, 2026-11-09; atmosphere_effect.h, atmosphere_clouds.cpp), after Schneider's Nubis
// (Horizon Zero Dawn, SIGGRAPH 2015 and 2017) and Hillaire's energy-conserving integration: a layer between `base`
// and `base + thickness` metres above sea level on the curved planet (so it may lie below the peaks: the march stops at
// the scene's depth), its shape a Perlin-Worley base eroded by Worley detail, its cover from a weather map the wind
// carries, its profile by type (0 layered, 0.5 heaped, 1 towering). Lit by the light through its own shadow (a short
// march toward it), multiple scattering as octaves (Wrenninge), a two-lobe phase, and the sky. Variants:
// MODE_NOISE_BASE    the base noise, 128^3 RGBA: R Perlin-Worley, GBA Worley at rising frequencies (tiling)
// MODE_NOISE_DETAIL  the detail noise, 32^3 RGBA: RGB Worley at rising frequencies (tiling)
// MODE_MARCH         the clouds seen from the camera at the march resolution, blended with the reprojected last frame:
//                    light (rgb) and transmittance (a), and the distances to the first cloud and the light's mean
// MODE_PANORAMA      the same over the whole sky round the camera (equirectangular), for the sky's light probe
// MODE_SHADOW        the clouds' transmittance to the light from a plane at their base, around the camera, and where
//                    along each ray they stand (sun_modulator_inc.glsl reads it)
// MODE_MAP           the weather map round the camera, near and far layers (each frame): R the cover (0 at a cloud's
//                    edge, 1 at its core), G its type, B its base (m above sea level), A its thickness (m)
// Over time the cells form and dissolve, the regions change (cross-faded noise), clouds lean downwind with height and
// their billows rise. A thin high layer (cirrus streaks or altocumulus puffs, 2D) lies above, drawn behind the clouds
// and shading them a little.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PI 3.14159265358979

layout(set = 0, binding = 0, std140) uniform Clouds {
	mat4 inv_view_proj; // NDC to the world relative to the camera (rotation and projection), m
	mat4 prev_view_proj; // the world relative to last frame's camera to its clip space
	vec4 cam_pos; // xyz the camera (m, world), w the frame number
	vec4 prev_cam; // xyz last frame's camera, w 1 when the history is usable
	vec4 light_dir; // xyz toward the light, w the light's illuminance for the sky table
	vec4 light_color; // rgb the light's colour and energy (the DirectionalLight3D's), w the phase's forward g
	vec4 layer; // x the shell's bottom (m above sea level), y its height (m), z the type (0-1), w the cover (0-1)
	vec4 shape; // x density (extinction per m at full), y base noise scale (m), z detail scale (m), w the regions' scale (m)
	vec4 wind; // xyz how far the wind has carried the clouds (m), w the noise's erosion (1 Nubis's)
	vec4 planet; // x radius (m), y world y 0's altitude (m), z the march's reach (m), w history weight
	vec4 screen; // xy the march's size, zw the scene's size
	vec4 shadow; // xy the shadow map's origin (world x, z), z its side (m), w 1 when drawn
	vec4 sky; // x the sky light's share, y the multiple scattering's strength, z the steps, w the light march's first step (m)
	vec4 noise; // x the first slice the noise passes make this frame, y how many (spread over frames: a whole volume in one
	// dispatch outlasted the driver's timeout)
	vec4 weather; // x precipitation (0-1), y the rain shafts' lowest altitude (m above sea level)
	vec4 lightning; // xyz the strike's flash (world, at the cloud base), w its strength now
	vec4 field; // x the layer's base (m above sea level), y the thickness scale, z the type's spread, w the base's (m)
	vec4 map_near; // xy the near weather map's origin (world x, z), z its side (m), w the far map's side (m)
	vec4 map_far; // xy the far map's origin, z the time (s), w the cells' size (m)
	vec4 life; // x the cells' lifetime (s), y the cover's regional variation (0-1)
	vec4 motion; // xy the wind's direction (x, z), z the lean (m downwind per m up), w the billows' rise (m/s)
	vec4 high; // the high layer: x its cover, y its altitude (m above sea level), z its type (0 cirrus, 1 altocumulus), w its optical depth at full
}
clouds;

#if defined(MODE_NOISE_BASE) || defined(MODE_NOISE_DETAIL)
layout(set = 0, binding = 1, rgba8) uniform restrict writeonly image3D out_noise;
#elif defined(MODE_MAP)
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image2DArray out_map;
#else
layout(set = 0, binding = 2) uniform sampler3D noise_base;
layout(set = 0, binding = 3) uniform sampler3D noise_detail;
layout(set = 0, binding = 4) uniform sampler2D sky_view;
layout(set = 0, binding = 11) uniform sampler2DArray weather_map;
#endif
#ifdef MODE_MARCH
layout(set = 0, binding = 5) uniform sampler2D depth_texture;
layout(set = 0, binding = 6, rgba16f) uniform restrict writeonly image2D out_clouds;
layout(set = 0, binding = 7, rg32f) uniform restrict writeonly image2D out_depth;
layout(set = 0, binding = 8) uniform sampler2D history;
layout(set = 0, binding = 9) uniform sampler2D history_depth;
#endif
#ifdef MODE_PANORAMA
layout(set = 0, binding = 6, rgba16f) uniform restrict writeonly image2D out_clouds;
#endif
#ifdef MODE_SHADOW
layout(set = 0, binding = 10, rgba16f) uniform restrict writeonly image2D out_shadow;
#endif

uint hash_u(uvec3 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v.x;
}

float hash1(uvec3 v) {
	return float(hash_u(v)) * (1.0 / 4294967296.0);
}

#if defined(MODE_NOISE_BASE) || defined(MODE_NOISE_DETAIL)
vec3 hash3(ivec3 c, int period) {
	uvec3 w = uvec3((c % period + period) % period);
	return vec3(hash1(w), hash1(w + 7919u), hash1(w + 104729u));
}

// Tiling Worley: 1 at a feature point, falling to 0 a cell away (inverted F1).
float worley(vec3 p, int cells) {
	vec3 q = p * float(cells);
	ivec3 base = ivec3(floor(q));
	float best = 1e9;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 c = base + ivec3(x, y, z);
				vec3 f = vec3(c) + hash3(c, cells);
				best = min(best, dot(q - f, q - f));
			}
		}
	}
	return 1.0 - clamp(sqrt(best), 0.0, 1.0);
}

vec3 grad(ivec3 c, int period) {
	vec3 h = hash3(c, period) * 2.0 - 1.0;
	return normalize(h + 1e-4);
}

// Tiling Perlin, about -1 to 1.
float perlin(vec3 p, int period) {
	vec3 q = p * float(period);
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float n000 = dot(grad(i, period), f);
	float n100 = dot(grad(i + ivec3(1, 0, 0), period), f - vec3(1, 0, 0));
	float n010 = dot(grad(i + ivec3(0, 1, 0), period), f - vec3(0, 1, 0));
	float n110 = dot(grad(i + ivec3(1, 1, 0), period), f - vec3(1, 1, 0));
	float n001 = dot(grad(i + ivec3(0, 0, 1), period), f - vec3(0, 0, 1));
	float n101 = dot(grad(i + ivec3(1, 0, 1), period), f - vec3(1, 0, 1));
	float n011 = dot(grad(i + ivec3(0, 1, 1), period), f - vec3(0, 1, 1));
	float n111 = dot(grad(i + ivec3(1, 1, 1), period), f - vec3(1, 1, 1));
	return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z) * 1.4;
}

float worley_fbm(vec3 p, int cells) {
	return worley(p, cells) * 0.625 + worley(p, cells * 2) * 0.25 + worley(p, cells * 4) * 0.125;
}
#endif

float remap(float v, float a, float b, float c, float d) {
	return c + (v - a) / max(b - a, 1e-5) * (d - c);
}

// A cloud's thickness (m) at a type: stratus a few hundred metres, fair cumulus about a kilometre, cumulonimbus 8 km
// (AtmosphereEffect::cloud_type_thickness: the same curve).
float type_thickness(float t) {
	t = clamp(t, 0.0, 1.0);
	return t < 0.5 ? mix(300.0, 1000.0, t * 2.0) : 1000.0 * pow(8.0, pow((t - 0.5) * 2.0, 1.5));
}

#if !defined(MODE_NOISE_BASE) && !defined(MODE_NOISE_DETAIL)
// Value noise in the plane, not tiling, 0 to 1.
float vnoise(vec2 p) {
	ivec2 i = ivec2(floor(p));
	vec2 f = fract(p);
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float a = hash1(uvec3(uvec2(i), 17u));
	float b = hash1(uvec3(uvec2(i + ivec2(1, 0)), 17u));
	float c = hash1(uvec3(uvec2(i + ivec2(0, 1)), 17u));
	float d = hash1(uvec3(uvec2(i + ivec2(1, 1)), 17u));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float vfbm(vec2 p) {
	return vnoise(p) * 0.5333 + vnoise(p * 2.03 + 17.1) * 0.2667 + vnoise(p * 4.07 + 41.7) * 0.1333 + vnoise(p * 8.11 + 73.3) * 0.0667;
}
#endif

#ifdef MODE_MAP
// The same, changing over time: two fields cross-faded every four cells' lifetimes (renormalised so the blend keeps
// the contrast), sixteen of them round the clock's wrap.
float vfbm_evolving(vec2 p, float seed) {
	float tt = clouds.map_far.z / (clouds.life.x * 4.0);
	float k = floor(tt);
	float s = smoothstep(0.0, 1.0, tt - k);
	float a = vfbm(p + seed + mod(k, 16.0) * 37.1);
	float b = vfbm(p + seed + mod(k + 1.0, 16.0) * 37.1);
	return 0.5 + (mix(a, b, s) - 0.5) / sqrt(s * s + (1.0 - s) * (1.0 - s));
}

// The cells' mean radius (in cells) for a cover: calibrated so the share of the sky clouded (a cell's cover above about
// 0.45, where the noise leaves a cloud) is about the cover with cells alive at min(1, 0.25 + 2 cover) (2026-11-09).
float cell_radius(float c) {
	const float cs[7] = float[](0.0, 0.05, 0.15, 0.25, 0.35, 0.5, 0.65);
	// (Measured on the footprint; seen from the ground a cloud's sides fill more of the sky, so 0.72 of it.)
	const float rs[7] = float[](0.22, 0.28, 0.38, 0.42, 0.44, 0.53, 0.63);
	for (int i = 1; i < 7; i++) {
		if (c <= cs[i]) {
			return mix(rs[i - 1], rs[i], (c - cs[i - 1]) / (cs[i] - cs[i - 1]));
		}
	}
	return min(rs[6] + (c - 0.65) * 0.6, 0.85);
}

// The weather at a point of the field (the world less the wind's carry, m): cover, type, base, thickness.
vec4 weather_field(vec2 f) {
	float cover = clouds.layer.w;
	float mean_type = clouds.layer.z;
	// Regions: some clear, some clumped; the cover's lift there heaps the clouds higher.
	float region = vfbm_evolving(f / clouds.shape.w, 0.0);
	float c = clamp(cover + (region * 2.0 - 1.0) * clouds.life.y * 2.0 * min(cover, 1.0 - cover), 0.0, 1.0);
	float region_t = vfbm_evolving(f / (clouds.shape.w * 0.7), 91.7) * 2.0 - 1.0;
	float t_local = mean_type + region_t * clouds.field.z + (c - cover) * 0.3;
	float region_b = vfbm_evolving(f / (clouds.shape.w * 1.3), 53.1) * 2.0 - 1.0;
	float base_local = clouds.field.x + region_b * clouds.field.w;

	// Cumulus: a cloud a cell, alive by the cover, its size, type, height and phase of life its own.
	float cell = clouds.map_far.w;
	vec2 g = f / cell;
	g += (vec2(vnoise(g * 0.7), vnoise(g * 0.7 + 31.3)) - 0.5) * 0.6; // irregular outlines
	ivec2 gi = ivec2(floor(g));
	float alive_p = min(1.0, 0.25 + 2.0 * c);
	float rm = cell_radius(c);
	// The cover the nearest clouds give (the most of them), their type and height blended by it (a hard switch between
	// two touching clouds made a wall where their heights differ).
	float best = 0.0;
	float wsum = 0.0, wt = 0.0, wth = 0.0, wb = 0.0;
	float rough = 0.8 + 0.4 * vnoise(g * 3.1 + 11.0); // lobes in the outline
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 id = gi + ivec2(x, y);
			uvec3 h = uvec3(uvec2(id), 3u);
			if (hash1(h) >= alive_p) {
				continue;
			}
			vec2 centre = vec2(id) + 0.1 + 0.8 * vec2(hash1(h + uvec3(0, 0, 1)), hash1(h + uvec3(0, 0, 2)));
			// Its life: forms, grows, dissolves (a cell is clear a third of the time, which the radius makes up).
			float phase = fract(clouds.map_far.z / clouds.life.x + hash1(h + uvec3(0, 0, 3)));
			float grow = smoothstep(0.0, 0.25, phase) * (1.0 - smoothstep(0.6, 1.0, phase));
			// At most a cell: the search reaches only the next cells (a larger one was cut off straight at a cell's edge).
			float size = hash1(h + uvec3(0, 0, 4));
			float r = min(rm * (0.6 + 0.8 * size) * mix(0.25, 1.15, grow), 1.0);
			float b = clamp((1.0 - length(g - centre) / r) * rough, 0.0, 1.0);
			if (b <= 0.0) {
				continue;
			}
			best = max(best, b);
			float wgt = b * b * b;
			wsum += wgt;
			float tc = t_local + (hash1(h + uvec3(0, 0, 5)) * 2.0 - 1.0) * clouds.field.z * 0.5;
			wt += wgt * tc;
			// Its height: its type's, with its width (a small young or dying cloud tall was a pillar) and a little of its
			// own; at most its width (heaped cloud is about as tall as wide, a tower taller).
			float thc = type_thickness(tc) * clouds.field.y * (0.55 + 0.45 * size + 0.3 * hash1(h + uvec3(0, 0, 6))) * mix(0.3, 1.0, grow);
			float aspect = mix(0.7, 2.5, smoothstep(0.75, 1.0, tc));
			wth += wgt * min(thc, aspect * 2.0 * r * cell);
			wb += wgt * (hash1(h + uvec3(0, 0, 7)) - 0.5) * 80.0;
		}
	}
	float best_t = wsum > 0.0 ? wt / wsum : t_local;
	float best_th = wsum > 0.0 ? wth / wsum : 0.0;
	float best_b = wsum > 0.0 ? wb / wsum : 0.0;
	// Layered cloud: a sheet with holes (the cover thresholding a noise), for the low types and an overcast.
	float sheet_n = vfbm(f / (cell * 1.5) + 7.7);
	float sheet = clamp(remap(sheet_n, 1.0 - c - 0.15, 1.0 - c + 0.2, 0.0, 1.0), 0.0, 1.0);
	float sheetiness = 1.0 - smoothstep(0.3, 0.55, t_local);
	float cov = mix(best, sheet, sheetiness);
	cov = max(cov, smoothstep(0.65, 1.0, c) * (0.75 + 0.25 * sheet_n)); // an overcast merges them
	float t = mix(best_t, t_local, sheetiness);
	float th = mix(best_th, type_thickness(t) * clouds.field.y, sheetiness);
	return vec4(cov, clamp(t, 0.0, 1.0), base_local + best_b * (1.0 - sheetiness), th);
}
#endif

#if !defined(MODE_NOISE_BASE) && !defined(MODE_NOISE_DETAIL) && !defined(MODE_MAP)
// The planet's centre in the world (m): under the camera, its surface at sea level.
vec3 planet_centre() {
	return vec3(clouds.cam_pos.x, -(clouds.planet.x + clouds.planet.y), clouds.cam_pos.z);
}

float altitude(vec3 w) {
	return length(w - planet_centre()) - clouds.planet.x;
}

// The nearest positive distance along d from o to the sphere of radius R about the planet's centre, in km for the
// precision, or -1.
vec2 sphere_hits(vec3 o, vec3 d, float R) {
	vec3 rel = (o - planet_centre()) * 0.001;
	float r = R * 0.001;
	float b = dot(rel, d);
	float c = dot(rel, rel) - r * r;
	float disc = b * b - c;
	if (disc < 0.0) {
		return vec2(-1.0);
	}
	float s = sqrt(disc);
	return vec2(-b - s, -b + s) * 1000.0;
}

// The weather at a world point: the map's near layer, else its far one; nothing past it.
vec4 weather_at(vec3 w) {
	vec2 t = (w.xz - clouds.map_near.xy) / clouds.map_near.z;
	if (all(greaterThan(t, vec2(0.002))) && all(lessThan(t, vec2(0.998)))) {
		return textureLod(weather_map, vec3(t, 0.0), 0.0);
	}
	t = (w.xz - clouds.map_far.xy) / clouds.map_near.w;
	if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) {
		return vec4(0.0);
	}
	return textureLod(weather_map, vec3(t, 1.0), 0.0);
}

// The vertical profile by height in the cloud (0 its base, 1 its top) and type (Nubis's, analytic): a sheet for the
// layered types, a dome for cumulus (falling with height, so the cover's edge rounds its top), a column for towers.
float vertical_profile(float h, float t) {
	float stratus = smoothstep(0.0, 0.2, h) * (1.0 - smoothstep(0.55, 1.0, h));
	float cumulus = smoothstep(0.0, 0.06, h) * (1.0 - h * h);
	float tower = smoothstep(0.0, 0.04, h) * (1.0 - pow(h, 4.0));
	return t < 0.5 ? mix(stratus, cumulus, smoothstep(0.25, 0.5, t)) : mix(cumulus, tower, smoothstep(0.7, 1.0, t));
}

struct CloudSample {
	float density; // extinction (1/m)
	float profile; // the dimensional profile: 0 outside, rising to 1 at a cloud's core
	float h; // the height in its cloud (0 the base, 1 the top)
};

// The clouds at a world point, after Schneider's Nubis (SIGGRAPH 2022 and 2023): the dimensional profile (the vertical
// profile times the map's cover, which falls from a cloud's core to its edge) eroded by a noise composite, wispy for
// layered cloud and a cumulus's base, billowy for heaped cloud (billows erode only the profile's outer 0.3, so the
// outline keeps the envelope's shape); cheap leaves the noise out.
CloudSample cloud_sample(vec3 w, bool cheap) {
	CloudSample cs;
	cs.density = 0.0;
	cs.profile = 0.0;
	cs.h = 0.0;
	float a = altitude(w);
	// Lean: the wind grows with height, so a cloud's top runs ahead of its base (read where it came from).
	vec3 ws = w;
	ws.xz -= clouds.motion.xy * (clouds.motion.z * max(a - clouds.layer.x, 0.0));
	// Torn outlines: where the map is read wanders by up to a quarter of a cell over a few kilometres (the map's cones
	// alone gave every cloud a round, convex outline: blobs, against the photos' ragged masses).
	vec3 wn = textureLod(noise_base, (ws - clouds.wind.xyz) / (clouds.shape.y * 2.5), 0.0).gba;
	vec4 m = weather_at(ws + vec3(wn.x - 0.5, 0.0, wn.y - 0.5) * (clouds.map_far.w * 0.5));
	float h = (a - m.b) / max(m.a, 1.0);
	cs.h = clamp(h, 0.0, 1.0);
	if (h < 0.0) {
		// Rain shafts: below the base where the cover is thick, streaked and drifting with the wind, thinning toward the
		// ground.
		if (cheap || clouds.weather.x < 0.01 || a < clouds.weather.y || m.r < 0.3) {
			return cs;
		}
		vec3 q = (w - clouds.wind.xyz) / vec3(600.0, 3000.0, 600.0);
		float streak = textureLod(noise_base, q, 0.0).g;
		float below = clamp((m.b - a) / max(m.b - clouds.weather.y, 1.0), 0.0, 1.0);
		cs.density = clouds.weather.x * smoothstep(0.3, 0.8, m.r) * smoothstep(0.35, 0.7, streak) * (1.0 - 0.6 * below) * 0.0012;
		return cs;
	}
	if (h > 1.0 || m.r <= 0.0) {
		return cs;
	}
	float t = m.g;
	// A cumulonimbus's anvil: the cover's edge spreads out near its top.
	float anvil = smoothstep(0.85, 1.0, t) * smoothstep(0.7, 0.92, h);
	float dp = vertical_profile(h, t) * pow(m.r, mix(1.0, 0.35, anvil));
	cs.profile = dp;
	if (dp <= 0.0) {
		return cs;
	}
	// The noise carves the profile (Nubis Evolved's sky clouds): cloud where the noise outweighs the profile's lack,
	// saturate(noise - (1 - profile)), so the noise's lumps make the outline and the profile its envelope.
	float d;
	if (cheap) {
		d = clamp(dp - 0.45, 0.0, 1.0) * smoothstep(0.0, 0.6, dp);
	} else {
		// Squashed: a lump of the noise wider than it is tall.
		vec3 q = (ws - clouds.wind.xyz) / clouds.shape.y;
		q.y *= 1.6;
		// Billows rise through the cloud (the noise's tile repeats, so wrapped).
		q.y -= fract(clouds.motion.w * clouds.map_far.z * 1.6 / clouds.shape.y);
		vec4 n = textureLod(noise_base, q, 0.0);
		float low = dot(n.gba, vec3(0.625, 0.25, 0.125));
		float heaped = clamp(remap(n.r, -(1.0 - low), 1.0, 0.0, 1.0), 0.0, 1.0); // Perlin-Worley heaped on Worley's cells
		vec3 dq = (ws - clouds.wind.xyz * 1.3) / clouds.shape.z;
		dq.y *= 1.3;
		// The small ones faster, curled by the large (Nubis Cubed's curly wisps, here a distortion by the base noise).
		dq.y -= fract(clouds.motion.w * 1.5 * clouds.map_far.z * 1.3 / clouds.shape.z);
		dq += (n.gba - 0.5) * 0.45;
		float fine = dot(textureLod(noise_detail, dq, 0.0).rgb, vec3(0.625, 0.25, 0.125)); // Worley cells, 1 at a cell's point
		// Billows: the heaped lumps with smaller ones on them; wisps: Perlin-Worley frayed by inverted Worley (webs),
		// finer inward.
		float billowy = mix(heaped, fine, 0.2);
		float wispy = mix(n.r, 1.0 - fine, 0.35 + 0.3 * dp);
		float detail_type = smoothstep(0.25, 0.55, t) * mix(0.6, 1.0, smoothstep(0.0, 0.25, h));
		float noise = mix(1.0, mix(wispy, billowy, detail_type), clouds.wind.w);
		// Billowed tops crisp (sharpened, as Nubis does), the rest thinner toward the outside of the profile: a base's and
		// a layer's fringe translucent and wispy (the photos' clouds fade there; a dense fringe all round read as cotton).
		float dl = clamp(noise - (1.0 - dp), 0.0, 1.0);
		float crisp = detail_type * smoothstep(0.25, 0.6, h);
		d = mix(dl * smoothstep(0.0, 0.6, dp), sqrt(dl), crisp);
	}
	cs.density = d * clouds.shape.x;
	return cs;
}

float cloud_density(vec3 w, bool cheap) {
	return cloud_sample(w, cheap).density;
}

float phase_hg(float c, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// The sky's light on the clouds: the sky table's mean over the upper sky (as radiance), and less from below.
vec3 sky_ambient(float h) {
	vec3 s = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 3; j++) {
			s += textureLod(sky_view, vec2((float(i) + 0.5) / 4.0, 0.5 + 0.5 * (float(j) + 0.5) / 3.0), 0.0).rgb;
		}
	}
	s *= clouds.light_dir.w / 12.0;
	// Under a covered sky the light from above is cloud, not blue sky: greyer with the cover.
	s = mix(s, vec3(dot(s, vec3(0.2126, 0.7152, 0.0722))), clamp(clouds.layer.w, 0.0, 1.0) * 0.8);
	return s * mix(0.35, 1.0, h) * clouds.sky.x;
}

// The optical depth toward the light from w: six steps, each twice the last.
float light_depth(vec3 w) {
	float tau = 0.0;
	float step_len = clouds.sky.w;
	float t = 0.0;
	for (int i = 0; i < 6; i++) {
		float dt = step_len * pow(2.0, float(i));
		vec3 p = w + clouds.light_dir.xyz * (t + dt * 0.5);
		tau += cloud_density(p, i > 2) * dt;
		t += dt;
	}
	return tau;
}

// The light scattered toward the camera along a ray (o, d) between t0 and t1 (m), and its transmittance; first: the
// distance to the first cloud, mean: the light's mean distance.
vec4 march(vec3 o, vec3 d, float t0, float t1, int steps, float jitter, out float first, out float mean) {
	vec3 L = vec3(0.0);
	float T = 1.0;
	first = -1.0;
	float wsum = 0.0, dsum = 0.0;
	float c = dot(d, clouds.light_dir.xyz);
	float g = clouds.light_color.w;
	float ms = clouds.sky.y;
	vec3 sun = clouds.light_color.rgb;
	// Adaptive (as Nubis): coarse steps through clear air, probing the profile only (no noise); on meeting a cloud a step
	// back and a third of the step inside it, coarse again after four empty ones. The fine detail was undersampled at
	// one step size (a grainy rim). Not while it rains: the rain shafts have no profile to find.
	float dt_big = (t1 - t0) / float(steps);
	float dt_fine = dt_big / 3.0;
	bool adaptive = clouds.weather.x < 0.01;
	bool fine = !adaptive;
	int empty = 0;
	float t = t0 + jitter * dt_big;
	for (int i = 0; i < steps * 3 && t < t1; i++) {
		vec3 p = o + d * t;
		if (!fine) {
			if (cloud_sample(p, true).profile <= 0.0) {
				t += dt_big;
				continue;
			}
			fine = true;
			empty = 0;
			t = max(t - dt_big + jitter * dt_fine, t0);
			continue;
		}
		float dt = adaptive ? dt_fine : dt_big;
		CloudSample cs = cloud_sample(p, false);
		float sigma = cs.density;
		if (sigma > 1e-6) {
			if (first < 0.0) {
				first = t;
			}
			float h = cs.h;
			float tau = light_depth(p);
			// Multiple scattering as octaves (Wrenninge): each fainter, less forward, reaching deeper.
			vec3 S = vec3(0.0);
			float a = 1.0, b = 1.0, e = 1.0;
			for (int k = 0; k < 3; k++) {
				float phase = mix(phase_hg(c, g * b), phase_hg(c, -0.2 * b), 0.3);
				S += sun * a * phase * exp(-tau * e);
				a *= 0.5 * ms;
				b *= 0.5;
				e *= 0.4;
			}
			// Deep in a thick cloud the light arrives diffused, not cut off: the diffusion's transmission through an
			// optical depth tau, about 1 / (1 + 0.75 tau (1 - g)) for a forward-scattering medium.
			S += sun * (ms / PI) / (1.0 + 0.75 * tau * (1.0 - g)); // the diffuse light leaving: its share over pi
			// The powder effect: thin edges facing away from the light darker (it brightened the cores by a third too).
			float powder = 1.0 - exp(-sigma * 2.0 * 60.0);
			S *= mix(1.0, powder, 0.35);
			// The sky's light reaches the cloud's skin, less its core (Nubis: pow(1 - profile, 0.5)).
			S += sky_ambient(h) * mix(0.4, 1.0, sqrt(1.0 - cs.profile));
			// Lightning: the flash lights the cloud round it, falling off over a kilometre or so.
			if (clouds.lightning.w > 0.0) {
				float r = length(p - clouds.lightning.xyz);
				S += vec3(0.75, 0.8, 1.0) * clouds.lightning.w * exp(-r / 900.0) / (4.0 * PI);
			}
			float st = exp(-sigma * dt);
			vec3 Sint = S * (1.0 - st); // albedo 1: scattering = extinction
			L += T * Sint;
			float wgt = T * (1.0 - st);
			wsum += wgt;
			dsum += wgt * t;
			T *= st;
			if (T < 0.01) {
				T = 0.0; // opaque: what is left would let the sun's disc through
				break;
			}
		}
		if (adaptive) {
			empty = cs.profile > 0.0 ? 0 : empty + 1;
			fine = empty < 4;
		}
		t += dt;
	}
	mean = wsum > 0.0 ? dsum / wsum : t1;
	return vec4(L, T);
}

// The ray's stretch through the layer (from the camera), at most reach m: x start, y end; y <= x when it misses.
vec2 layer_span(vec3 o, vec3 d, float reach) {
	float base = clouds.weather.x > 0.01 ? clouds.weather.y : clouds.layer.x; // the rain shafts' bottom when it rains
	float Rb = clouds.planet.x + base;
	float Rt = clouds.planet.x + clouds.layer.x + clouds.layer.y;
	float a = altitude(o);
	vec2 hb = sphere_hits(o, d, Rb);
	vec2 ht = sphere_hits(o, d, Rt);
	float t0, t1;
	if (a < base) {
		// Below: from the base up to the top (rays down never reach it, nor those the planet hides).
		if (hb.y <= 0.0) {
			return vec2(0.0, -1.0);
		}
		t0 = hb.y;
		t1 = ht.y;
	} else if (a > clouds.layer.x + clouds.layer.y) {
		// Above: from the top down to the base, or out through the top again.
		if (ht.x <= 0.0) {
			return vec2(0.0, -1.0);
		}
		t0 = ht.x;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	} else {
		// Inside: to the base below or the top above.
		t0 = 0.0;
		t1 = hb.x > 0.0 ? hb.x : ht.y;
	}
	return vec2(t0, min(t1, reach));
}

// The high layer's share of cloud at a point of it (world x, z): cirrus fibres along the wind, hooked by a slow wave,
// or rows of altocumulus puffs; patchy over tens of kilometres.
float high_density(vec2 xz) {
	float cover = clouds.high.x;
	if (cover <= 0.001) {
		return 0.0;
	}
	vec2 f = xz - clouds.wind.xz * 1.6; // the wind aloft is stronger
	vec2 dir = clouds.motion.xy;
	vec2 uv = vec2(dot(f, dir), dot(f, vec2(-dir.y, dir.x)));
	float bend = (vfbm(uv / 9000.0 + 5.0) - 0.5) * 5000.0;
	float streak = vfbm(vec2(uv.x / 9000.0, (uv.y + bend) / 2200.0));
	float fibre = vfbm(vec2(uv.x / 3000.0, (uv.y + bend) / 450.0) + 13.0);
	float cirrus = streak * 0.85 + fibre * 0.15;
	float puffs = textureLod(noise_detail, vec3(f / 2400.0, 0.37), 0.0).r * 0.7 + vfbm(f / 4000.0 + 7.0) * 0.3;
	float n = clamp((mix(cirrus, puffs, clouds.high.z) - 0.3) / 0.4, 0.0, 1.0);
	float patches = vfbm(f / 25000.0 + 3.1);
	float c = clamp(cover + (patches - 0.5) * 1.2 * min(cover, 1.0 - cover), 0.0, 1.0);
	// Soft: thin veils fraying to nothing, not drawn lines.
	return smoothstep(1.0 - c - 0.1, 1.0 - c + 0.5, n) * smoothstep(0.0, 0.05, cover);
}

// The high layer seen along a ray (o, d) from below, nearer than `limit` m: its light (rgb) and transmittance (a), and
// its distance (or -1).
vec4 high_layer(vec3 o, vec3 d, float limit, out float dist) {
	dist = -1.0;
	if (clouds.high.x <= 0.001 || altitude(o) > clouds.high.y) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	if (sphere_hits(o, d, clouds.planet.x).x > 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0); // the ground in the way
	}
	float t = sphere_hits(o, d, clouds.planet.x + clouds.high.y).y;
	if (t <= 0.0 || t > limit) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec3 p = o + d * t;
	float dens = high_density(p.xz);
	if (dens <= 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// Thin: the path through it longer the more slanting, the light in it hardly dimmed.
	float mu = max(abs(dot(d, normalize(p - planet_centre()))), 0.06);
	float alpha = (1.0 - exp(-dens * clouds.high.w / mu)) * smoothstep(300000.0, 150000.0, t);
	float c = dot(d, clouds.light_dir.xyz);
	float g = clouds.light_color.w;
	vec3 S = clouds.light_color.rgb * (mix(phase_hg(c, g), phase_hg(c, -0.2), 0.3) + clouds.sky.y / (4.0 * PI)) + sky_ambient(1.0);
	dist = t;
	return vec4(S * alpha, 1.0 - alpha);
}

// The high layer's transmittance to the light from a point below it.
float high_shadow(vec3 w) {
	vec3 l = clouds.light_dir.xyz;
	if (clouds.high.x <= 0.001 || l.y <= 0.02) {
		return 1.0;
	}
	float up = clouds.high.y - clouds.planet.y - w.y;
	vec3 p = w + l * (max(up, 0.0) / l.y);
	return exp(-high_density(p.xz) * clouds.high.w / l.y);
}

// The high layer behind the clouds already marched (cur: their light and transmittance; first, mean: their distances).
void add_high_layer(vec3 o, vec3 d, float limit, inout vec4 cur, inout float first, inout float mean) {
	float th;
	vec4 hl = high_layer(o, d, limit, th);
	if (th < 0.0) {
		return;
	}
	float low_w = 1.0 - cur.a;
	float high_w = cur.a * (1.0 - hl.a);
	cur.rgb += cur.a * hl.rgb;
	mean = (low_w * (first < 0.0 ? th : mean) + high_w * th) / max(low_w + high_w, 1e-5);
	first = first < 0.0 ? th : first;
	cur.a *= hl.a;
}
#endif

void main() {
#if defined(MODE_NOISE_BASE) || defined(MODE_NOISE_DETAIL)
	ivec3 size = imageSize(out_noise);
	int z_end = min(size.z, int(clouds.noise.x + clouds.noise.y));
	for (int z = int(clouds.noise.x); z < z_end; z++) {
		ivec3 cell = ivec3(gl_GlobalInvocationID.xy, z);
		if (any(greaterThanEqual(cell, size))) {
			return;
		}
		vec3 p = (vec3(cell) + 0.5) / vec3(size);
#ifdef MODE_NOISE_BASE
		float pn = perlin(p, 4) * 0.5 + perlin(p, 8) * 0.25 + perlin(p, 16) * 0.125;
		pn = clamp(pn * 0.5 + 0.5, 0.0, 1.0);
		float wf = worley_fbm(p, 4);
		float pw = remap(pn, wf - 1.0, 1.0, 0.0, 1.0); // Perlin-Worley: billows from the cells
		imageStore(out_noise, cell, vec4(clamp(pw, 0.0, 1.0), worley_fbm(p, 4), worley_fbm(p, 8), worley_fbm(p, 16)));
#else
		imageStore(out_noise, cell, vec4(worley_fbm(p, 2), worley_fbm(p, 4), worley_fbm(p, 8), 1.0));
#endif
	}
#endif

#ifdef MODE_MAP
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_map).xy;
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	for (int layer = 0; layer < 2; layer++) {
		vec2 origin = layer == 0 ? clouds.map_near.xy : clouds.map_far.xy;
		float side = layer == 0 ? clouds.map_near.z : clouds.map_near.w;
		vec2 xz = origin + (vec2(px) + 0.5) / vec2(size) * side;
		imageStore(out_map, ivec3(px, layer), weather_field(xz - clouds.wind.xz));
	}
#endif

#ifdef MODE_MARCH
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(clouds.screen.xy);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec4 far_p = clouds.inv_view_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	vec3 d = normalize(far_p.xyz / far_p.w);
	vec3 o = clouds.cam_pos.xyz;
	// The scene's distance here: the farthest of the scene's pixels under this one.
	ivec2 full = ivec2(clouds.screen.zw);
	ivec2 q0 = ivec2(vec2(px) * vec2(full) / vec2(size));
	ivec2 q1 = min(ivec2(vec2(px + 1) * vec2(full) / vec2(size)) - 1, full - 1);
	float depth = 1.0;
	depth = min(depth, texelFetch(depth_texture, q0, 0).r);
	depth = min(depth, texelFetch(depth_texture, ivec2(q1.x, q0.y), 0).r);
	depth = min(depth, texelFetch(depth_texture, ivec2(q0.x, q1.y), 0).r);
	depth = min(depth, texelFetch(depth_texture, q1, 0).r);
	float scene = 1e12;
	if (depth > 0.0) {
		vec4 rel = clouds.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
		scene = length(rel.xyz / rel.w);
	}
	vec2 span = layer_span(o, d, min(clouds.planet.z, scene));
	vec4 cur = vec4(0.0, 0.0, 0.0, 1.0);
	float first = -1.0, mean = 1e9;
	if (span.y > span.x) {
		float jitter = hash1(uvec3(uvec2(px), uint(clouds.cam_pos.w)));
		int steps = int(clouds.sky.z);
		// Fewer steps for short stretches.
		steps = clamp(int(float(steps) * clamp((span.y - span.x) / 4000.0, 0.25, 1.0)), 16, steps);
		cur = march(o, d, span.x, span.y, steps, jitter, first, mean);
	}
	add_high_layer(o, d, scene, cur, first, mean);
	// The last frame's clouds where they were.
	float front = first < 0.0 ? 1e9 : first;
	if (clouds.prev_cam.w > 0.5) {
		float dist = cur.a < 0.999 ? mean : (span.y > span.x ? 0.5 * (span.x + span.y) : 1e5);
		vec3 wp = o + d * dist;
		vec4 clip = clouds.prev_view_proj * vec4(wp - clouds.prev_cam.xyz, 1.0);
		if (clip.w > 0.0) {
			vec2 puv = clip.xy / clip.w * 0.5 + 0.5;
			if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)))) {
				vec4 h = textureLod(history, puv, 0.0);
				vec2 hd = textureLod(history_depth, puv, 0.0).rg;
				// Where the scene stands in front of last frame's clouds now, their history does not count.
				float w = clouds.planet.w;
				if (hd.r > scene + 50.0) {
					w = 0.0;
				}
				cur = mix(cur, h, w);
				front = min(front, mix(front, hd.r, w));
				mean = mix(mean, hd.g, w);
			}
		}
	}
	imageStore(out_clouds, px, cur);
	imageStore(out_depth, px, vec4(front, mean, 0.0, 0.0));
#endif

#ifdef MODE_PANORAMA
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_clouds);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	// Equirectangular: u the azimuth (0 at -Z, as the sky shader's SKY_COORDS), v from the zenith down.
	float phi = (uv.x - 0.5) * 2.0 * PI;
	float theta = uv.y * PI;
	vec3 d = vec3(sin(theta) * sin(phi), cos(theta), -sin(theta) * cos(phi));
	vec3 o = clouds.cam_pos.xyz;
	vec2 span = layer_span(o, d, clouds.planet.z);
	vec4 cur = vec4(0.0, 0.0, 0.0, 1.0);
	float first = -1.0, mean = 1e9;
	if (span.y > span.x) {
		cur = march(o, d, span.x, span.y, 32, 0.5, first, mean);
	}
	add_high_layer(o, d, 1e12, cur, first, mean);
	imageStore(out_clouds, px, cur);
#endif

#ifdef MODE_SHADOW
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_shadow);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	// A point on the plane at the clouds' base (flat: the map is local), marched to the layer's top along the light.
	vec2 xz = clouds.shadow.xy + (vec2(px) + 0.5) / vec2(size) * clouds.shadow.z;
	float base_y = clouds.layer.x - clouds.planet.y;
	vec3 l = clouds.light_dir.xyz;
	// R the transmittance along the whole ray, G the mean height above the plane of the cloud on it and B half the height
	// it takes up (m; the optical depth's mean and spread, a slab's half height being its deviation times root 3), A the
	// high layer's transmittance: a point above the base is shaded only by the cloud still between it and the light.
	vec4 shade = vec4(1.0, 0.0, 0.0, 1.0);
	if (l.y > 0.02) {
		float len = clouds.layer.y / l.y;
		// Jittered per texel (fixed in the world, as the map's origin snaps): steps aligned across texels showed as
		// blocks in the shadow where a step of a thick shell crossed a thin cloud.
		const int N = 40;
		float dt = len / float(N);
		float jitter = hash1(uvec3(uvec2(ivec2(floor(xz / (clouds.shadow.z / vec2(size))))), 29u));
		float tau = 0.0, tau_u = 0.0, tau_uu = 0.0;
		for (int i = 0; i < N; i++) {
			float u = (float(i) + jitter) / float(N); // the share of the layer's height
			vec3 p = vec3(xz.x, base_y, xz.y) + l * (u * len);
			// The shapes the eye sees (eroded): the uneroded ones cast a fuller shadow than the clouds overhead.
			float dtau = cloud_density(p, false) * dt;
			tau += dtau;
			tau_u += dtau * u;
			tau_uu += dtau * u * u;
		}
		shade.r = exp(-tau);
		if (tau > 1e-4) {
			float mean_u = tau_u / tau;
			shade.g = mean_u * clouds.layer.y;
			shade.b = (sqrt(3.0 * max(tau_uu / tau - mean_u * mean_u, 0.0)) + 0.5 / float(N)) * clouds.layer.y;
		}
		shade.a = high_shadow(vec3(xz.x, base_y, xz.y));
	}
	imageStore(out_shadow, px, shade);
#endif
}
