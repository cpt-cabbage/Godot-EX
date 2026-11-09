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
// MODE_SHADOW        the clouds' transmittance to the light from a plane at their base, around the camera

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PI 3.14159265358979

layout(set = 0, binding = 0, std140) uniform Clouds {
	mat4 inv_view_proj; // NDC to the world relative to the camera (rotation and projection), m
	mat4 prev_view_proj; // the world relative to last frame's camera to its clip space
	vec4 cam_pos; // xyz the camera (m, world), w the frame number
	vec4 prev_cam; // xyz last frame's camera, w 1 when the history is usable
	vec4 light_dir; // xyz toward the light, w the light's illuminance for the sky table
	vec4 light_color; // rgb the light's colour and energy (the DirectionalLight3D's), w the phase's forward g
	vec4 layer; // x base (m above sea level), y thickness (m), z type (0-1), w cover (0-1)
	vec4 shape; // x density (extinction per m at full), y base noise scale (m), z detail scale (m), w weather map scale (m)
	vec4 wind; // xyz how far the wind has carried the clouds (m), w the detail's erosion (0-1)
	vec4 planet; // x radius (m), y world y 0's altitude (m), z the march's reach (m), w history weight
	vec4 screen; // xy the march's size, zw the scene's size
	vec4 shadow; // xy the shadow map's origin (world x, z), z its side (m), w 1 when drawn
	vec4 sky; // x the sky light's share, y the multiple scattering's strength, z the steps, w the light march's first step (m)
	vec4 noise; // x the first slice the noise passes make this frame, y how many (spread over frames: a whole volume in one
	// dispatch outlasted the driver's timeout)
	vec4 weather; // x precipitation (0-1), y the rain shafts' lowest altitude (m above sea level)
	vec4 lightning; // xyz the strike's flash (world, at the cloud base), w its strength now
}
clouds;

#if defined(MODE_NOISE_BASE) || defined(MODE_NOISE_DETAIL)
layout(set = 0, binding = 1, rgba8) uniform restrict writeonly image3D out_noise;
#else
layout(set = 0, binding = 2) uniform sampler3D noise_base;
layout(set = 0, binding = 3) uniform sampler3D noise_detail;
layout(set = 0, binding = 4) uniform sampler2D sky_view;
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
layout(set = 0, binding = 10, r16f) uniform restrict writeonly image2D out_shadow;
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

#if !defined(MODE_NOISE_BASE) && !defined(MODE_NOISE_DETAIL)
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

float layer_type() {
	return clouds.layer.z;
}

// The layer's profile by height within it (0 at the base, 1 at the top), for its type.
float height_profile(float h) {
	float t = layer_type();
	float stratus = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.25, 0.45, h));
	float cumulus = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.4, 0.85, h));
	float tower = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.75, 1.0, h));
	return t < 0.5 ? mix(stratus, cumulus, t * 2.0) : mix(cumulus, tower, t * 2.0 - 1.0);
}

// The cover at a point: low-frequency noise the wind carries, thresholded by the cover; full cover is overcast.
float weather_cover(vec3 w) {
	vec2 uv = (w.xz - clouds.wind.xz * 0.5) / clouds.shape.w;
	vec4 n = textureLod(noise_base, vec3(uv, 0.31), 0.0);
	// The noise mostly spans 0.25 to 0.75: stretched to 0 to 1, so the cover is about the share of the sky clouded.
	float v = clamp(((n.r * 0.6 + n.g * 0.4) - 0.25) / 0.5, 0.0, 1.0);
	float cover = clouds.layer.w;
	return clamp(remap(v, 1.0 - cover - 0.1, 1.0 - cover + 0.25, 0.0, 1.0), 0.0, 1.0) * smoothstep(0.0, 0.1, cover) + smoothstep(0.85, 1.0, cover) * 0.5;
}

// The cloud's extinction (1/m) at a world position; cheap leaves out the detail.
float cloud_density(vec3 w, bool cheap) {
	float a = altitude(w);
	float h = (a - clouds.layer.x) / max(clouds.layer.y, 1.0);
	if (h > 1.0) {
		return 0.0;
	}
	if (h < 0.0) {
		// Rain shafts: below the base where the cover is thick, streaked and drifting with the wind, thinning toward the
		// ground.
		if (cheap || clouds.weather.x < 0.01 || a < clouds.weather.y) {
			return 0.0;
		}
		float wc = weather_cover(w);
		if (wc < 0.3) {
			return 0.0;
		}
		vec3 q = (w - clouds.wind.xyz) / vec3(600.0, 3000.0, 600.0);
		float streak = textureLod(noise_base, q, 0.0).g;
		float below = clamp((clouds.layer.x - a) / max(clouds.layer.x - clouds.weather.y, 1.0), 0.0, 1.0);
		return clouds.weather.x * smoothstep(0.3, 0.8, wc) * smoothstep(0.35, 0.7, streak) * (1.0 - 0.6 * below) * 0.0012;
	}
	float wc = weather_cover(w);
	if (wc <= 0.0) {
		return 0.0;
	}
	vec3 q = (w - clouds.wind.xyz) / clouds.shape.y;
	vec4 n = textureLod(noise_base, q, 0.0);
	float low = dot(n.gba, vec3(0.625, 0.25, 0.125));
	float base = remap(n.r, -(1.0 - low), 1.0, 0.0, 1.0);
	base *= height_profile(h);
	float shape = clamp(remap(base, 1.0 - wc, 1.0, 0.0, 1.0), 0.0, 1.0) * wc;
	if (shape <= 0.0) {
		return 0.0;
	}
	if (!cheap) {
		vec3 dq = (w - clouds.wind.xyz * 1.3) / clouds.shape.z;
		float detail = dot(textureLod(noise_detail, dq, 0.0).rgb, vec3(0.625, 0.25, 0.125));
		// Wisps at the base, billows above.
		detail = mix(detail, 1.0 - detail, clamp(h * 4.0, 0.0, 1.0));
		shape = clamp(remap(shape, detail * clouds.wind.w, 1.0, 0.0, 1.0), 0.0, 1.0);
	}
	return shape * clouds.shape.x;
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
	float dt = (t1 - t0) / float(steps);
	float c = dot(d, clouds.light_dir.xyz);
	float g = clouds.light_color.w;
	float ms = clouds.sky.y;
	vec3 sun = clouds.light_color.rgb;
	for (int i = 0; i < steps; i++) {
		float t = t0 + (float(i) + jitter) * dt;
		vec3 p = o + d * t;
		float sigma = cloud_density(p, false);
		if (sigma > 1e-6) {
			if (first < 0.0) {
				first = t;
			}
			float h = clamp((altitude(p) - clouds.layer.x) / max(clouds.layer.y, 1.0), 0.0, 1.0);
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
			// The powder effect: edges facing away from the light darker.
			float powder = 1.0 - exp(-sigma * 2.0 * 60.0);
			S *= mix(1.0, powder * 2.0, 0.35);
			S += sky_ambient(h);
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
	if (span.y > span.x) {
		float first, mean;
		cur = march(o, d, span.x, span.y, 32, 0.5, first, mean);
	}
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
	float T = 1.0;
	if (l.y > 0.02) {
		float len = clouds.layer.y / l.y;
		const int N = 24;
		float dt = len / float(N);
		float tau = 0.0;
		for (int i = 0; i < N; i++) {
			vec3 p = vec3(xz.x, base_y, xz.y) + l * ((float(i) + 0.5) * dt);
			// The shapes the eye sees (eroded): the uneroded ones cast a fuller shadow than the clouds overhead.
			tau += cloud_density(p, false) * dt;
		}
		T = exp(-tau);
	}
	imageStore(out_shadow, px, vec4(T));
#endif
}
