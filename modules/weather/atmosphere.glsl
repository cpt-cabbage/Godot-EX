#[compute]

#version 450

#VERSION_DEFINES

// A physically based atmosphere (Godot-EX, 2026-11-09), after Hillaire, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (EGSR 2020), as Unreal's Sky Atmosphere: Rayleigh and Mie scattering with exponential
// densities, ozone absorption in a tent around 25 km, in kilometres. One source, five variants (atmosphere_effect.cpp):
// MODE_TRANSMITTANCE  the transmittance to the top of the atmosphere by altitude and zenith angle (Bruneton's mapping)
// MODE_MULTISCATTER   multiple scattering's contribution by altitude and sun angle (Hillaire's Psi = L2 / (1 - f_ms))
// MODE_SKY_VIEW       the sky's radiance seen from the camera, by the view's azimuth from the light and its elevation
// MODE_AERIAL         the in-scattering and transmittance from the camera to each froxel of the view volume, marched
//                     once along each column, the sun's light in the air shadowed by the terrain; and for the sky, what
//                     the shadows take from the light scattered toward the camera within the volume's reach
// MODE_APPLY          the scene's colour through the aerial perspective, by its depth; the sky's pixels get the shadows
// MODE_TERRAIN_SHADOW the terrain's shadow in the air: for each texel of a map turned to the light, the height below
//                     which the light is hidden by the ground toward it (a running maximum along each row)
// Radiance is for a light of illuminance 1; the caller scales by the light's.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PI 3.14159265358979

layout(push_constant, std430) uniform Params {
	vec4 rayleigh; // rgb scattering per km, w scale height km
	vec4 mie_scattering; // rgb per km, w scale height km
	vec4 mie_extinction; // rgb per km, w anisotropy g
	vec4 ozone; // rgb absorption per km, w ground albedo
	vec4 radii; // bottom radius, top radius, camera radius (km), aerial max distance (km)
	vec4 light; // xyz direction toward the light (Y up), w its illuminance (for MODE_APPLY)
	vec4 misc; // x metres to km times the aerial scale, y the multiple scattering's factor, zw screen size (MODE_APPLY;
	// the other modes: z the art tint's height in radians)
	vec4 tint; // rgb: the art's tint and luminance on the sky's and the aerial perspective's light, w: its recolour
}
params;

layout(set = 0, binding = 7, std140) uniform Camera {
	mat4 inv_view_proj; // NDC to the world relative to the camera (rotation and projection only), metres
	vec4 cam_pos; // xyz the camera in the world (m), w 1 when the terrain shadows the air
	vec4 shadow_frame; // xy the shadow map's centre (world x, z), zw the direction toward the light (x, z), unit
	vec4 shadow_params; // x the map's side (m), y the shadow edge's softness (m), z its texels a side, w tan(light elevation)
	vec4 terrain_rect; // the height texture's world rectangle: x, z origin, x, z size (m)
	vec4 terrain_h; // x height_min, y height_range (m), z the mip to read at the map's texel size
	vec4 air_volume; // x the air volume's reach (m), y 1 when it is drawn (air_volume.glsl), z the camera's exposure
	// (MODE_APPLY: the scene's colour already holds it, so the light added here takes it too)
	vec4 cloud_flags; // x 1 when the clouds are drawn, y 1 when their shadow map is
	vec4 cloud_shadow; // xy the clouds' shadow map's origin (world x, z), z its side (m), w the clouds' base (world y)
}
camera;

#if defined(MODE_TRANSMITTANCE) || defined(MODE_MULTISCATTER) || defined(MODE_SKY_VIEW)
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D out_lut;
#endif
#if !defined(MODE_TRANSMITTANCE)
layout(set = 0, binding = 0) uniform sampler2D transmittance_lut;
#endif
#if defined(MODE_SKY_VIEW) || defined(MODE_AERIAL)
layout(set = 0, binding = 1) uniform sampler2D multiscatter_lut;
#endif
#ifdef MODE_AERIAL
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image3D out_aerial;
#endif
#ifdef MODE_TERRAIN_SHADOW
layout(set = 0, binding = 8) uniform sampler2D terrain_heights;
layout(set = 0, binding = 9, r32f) uniform restrict writeonly image2D shadow_out;
#endif
#ifdef MODE_AERIAL
layout(set = 0, binding = 10) uniform sampler2D terrain_shadow;
layout(set = 0, binding = 11, rgba16f) uniform restrict writeonly image2D out_sky_shadow;
layout(set = 0, binding = 16) uniform sampler2D cloud_shadow_map;
#endif
#ifdef MODE_APPLY
layout(set = 0, binding = 12) uniform sampler2D sky_shadow;
layout(set = 0, binding = 13) uniform sampler3D air_light;
layout(set = 0, binding = 14) uniform sampler2D cloud_light;
layout(set = 0, binding = 15) uniform sampler2D cloud_depth;
layout(set = 0, binding = 4) uniform sampler3D aerial_lut;
layout(set = 0, binding = 5) uniform sampler2D depth_texture;
layout(set = 0, binding = 6, rgba16f) uniform restrict image2D color_image;
#endif

float Rg() {
	return params.radii.x;
}
float Rt() {
	return params.radii.y;
}

struct Medium {
	vec3 scatter_r;
	vec3 scatter_m;
	vec3 extinction;
};

Medium medium(float r) {
	float h = max(r - Rg(), 0.0);
	float dr = exp(-h / params.rayleigh.w);
	float dm = exp(-h / params.mie_scattering.w);
	float dz = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
	Medium m;
	m.scatter_r = params.rayleigh.rgb * dr;
	m.scatter_m = params.mie_scattering.rgb * dm;
	m.extinction = m.scatter_r + params.mie_extinction.rgb * dm + params.ozone.rgb * dz;
	return m;
}

// The nearest positive distance along d from o to a sphere of radius R at the origin, or -1.
float sphere(vec3 o, vec3 d, float R) {
	float b = dot(o, d);
	float c = dot(o, o) - R * R;
	float disc = b * b - c;
	if (disc < 0.0) {
		return -1.0;
	}
	float s = sqrt(disc);
	float t0 = -b - s, t1 = -b + s;
	if (t0 > 0.0) {
		return t0;
	}
	return t1 > 0.0 ? t1 : -1.0;
}

// Bruneton's transmittance mapping.
vec2 transmittance_uv(float r, float mu) {
	float H = sqrt(max(Rt() * Rt() - Rg() * Rg(), 0.0));
	float rho = sqrt(max(r * r - Rg() * Rg(), 0.0));
	float disc = r * r * (mu * mu - 1.0) + Rt() * Rt();
	float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
	float d_min = Rt() - r, d_max = rho + H;
	return vec2((d - d_min) / max(d_max - d_min, 1e-6), rho / H);
}

#if !defined(MODE_TRANSMITTANCE)
vec3 transmittance_to_top(float r, float mu) {
	return textureLod(transmittance_lut, transmittance_uv(r, mu), 0.0).rgb;
}

// The light's transmittance to a point, 0 where the planet hides it.
vec3 light_transmittance(vec3 p, vec3 l) {
	float r = length(p);
	vec3 up = p / r;
	float mu = dot(up, l);
	if (sphere(p, l, Rg()) > 0.0) {
		return vec3(0.0);
	}
	return transmittance_to_top(r, mu);
}
#endif

float phase_rayleigh(float c) {
	return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

float phase_mie(float c, float g) {
	// Cornette-Shanks.
	float g2 = g * g;
	return 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + c * c)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

#ifdef MODE_AERIAL
vec2 shadow_perp() {
	return vec2(-camera.shadow_frame.w, camera.shadow_frame.z);
}

// The light's visibility past the terrain at a world position (m): 1 above the shadow's height there, 0 below.
float terrain_visibility(vec3 w) {
	if (camera.cam_pos.w < 0.5) {
		return 1.0;
	}
	vec2 rel = w.xz - camera.shadow_frame.xy;
	vec2 uv = vec2(dot(rel, camera.shadow_frame.zw), dot(rel, shadow_perp())) / camera.shadow_params.x + 0.5;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	float s = textureLod(terrain_shadow, uv, 0.0).r;
	return smoothstep(-camera.shadow_params.y, camera.shadow_params.y, w.y - s);
}

// The clouds' transmittance to the light from a world position (1 off their map).
float cloud_visibility(vec3 w) {
	if (camera.cloud_flags.y < 0.5) {
		return 1.0;
	}
	vec3 l = params.light.xyz;
	vec3 q = w + l * (max(camera.cloud_shadow.w - w.y, 0.0) / max(l.y, 0.02));
	vec2 uv = (q.xz - camera.cloud_shadow.xy) / camera.cloud_shadow.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	return textureLod(cloud_shadow_map, uv, 0.0).r;
}
#endif

// The art's tint for a view direction d: a multiply, or (by tint.w) the light's luminance in the tint's colour, fading
// with the view's elevation over misc.z radians.
vec3 art_tint(vec3 L, vec3 d) {
	vec3 t = mix(L, vec3(dot(L, vec3(0.2126, 0.7152, 0.0722))), params.tint.w) * params.tint.rgb;
	float el = asin(clamp(d.y, 0.0, 1.0));
	return mix(L, t, exp(-el / max(params.misc.z, 0.01)));
}

#if defined(MODE_SKY_VIEW) || defined(MODE_AERIAL)
vec3 multiscatter(float r, float mu_light) {
	vec2 uv = vec2(mu_light * 0.5 + 0.5, clamp((r - Rg()) / (Rt() - Rg()), 0.0, 1.0));
	return textureLod(multiscatter_lut, uv, 0.0).rgb;
}

// Light scattered toward the camera along a ray from o in direction d over t_max km, in `steps`; r_t the transmittance.
vec3 integrate(vec3 o, vec3 d, float t_max, int steps, vec3 l, out vec3 r_t) {
	float c = dot(d, l);
	float pr = phase_rayleigh(c), pm = phase_mie(c, params.mie_extinction.w);
	vec3 L = vec3(0.0);
	vec3 T = vec3(1.0);
	float dt = t_max / float(steps);
	for (int i = 0; i < steps; i++) {
		vec3 p = o + d * (float(i) + 0.5) * dt;
		float r = length(p);
		Medium m = medium(r);
		vec3 lt = light_transmittance(p, l);
		vec3 ms = multiscatter(r, dot(p / r, l)) * params.misc.y;
		vec3 S = (m.scatter_r * pr + m.scatter_m * pm) * lt + (m.scatter_r + m.scatter_m) * ms;
		vec3 step_t = exp(-m.extinction * dt);
		vec3 S_int = (S - S * step_t) / max(m.extinction, vec3(1e-7)); // Hillaire's integration over the step
		L += T * S_int;
		T *= step_t;
	}
	r_t = T;
	return L;
}
#endif

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);

#ifdef MODE_TRANSMITTANCE
	ivec2 size = imageSize(out_lut);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	float H = sqrt(Rt() * Rt() - Rg() * Rg());
	float rho = H * uv.y;
	float r = sqrt(rho * rho + Rg() * Rg());
	float d_min = Rt() - r, d_max = rho + H;
	float d = d_min + uv.x * (d_max - d_min);
	float mu = d == 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
	vec3 o = vec3(0.0, r, 0.0);
	vec3 dir = vec3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
	float t = sphere(o, dir, Rt());
	vec3 depth = vec3(0.0);
	const int N = 40;
	float dt = max(t, 0.0) / float(N);
	for (int i = 0; i < N; i++) {
		depth += medium(length(o + dir * (float(i) + 0.5) * dt)).extinction * dt;
	}
	imageStore(out_lut, px, vec4(exp(-depth), 1.0));
#endif

#ifdef MODE_MULTISCATTER
	ivec2 size = imageSize(out_lut);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	float mu_l = uv.x * 2.0 - 1.0;
	float r = Rg() + uv.y * (Rt() - Rg()) + 0.01;
	vec3 o = vec3(0.0, r, 0.0);
	vec3 l = vec3(sqrt(max(1.0 - mu_l * mu_l, 0.0)), mu_l, 0.0);
	vec3 L2 = vec3(0.0), fms = vec3(0.0);
	const int SQ = 8;
	const int N = 20;
	float albedo = params.ozone.w;
	for (int a = 0; a < SQ; a++) {
		for (int b = 0; b < SQ; b++) {
			// Uniform directions on the sphere.
			float u = (float(a) + 0.5) / float(SQ), v = (float(b) + 0.5) / float(SQ);
			float cos_t = 1.0 - 2.0 * u, sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0)), ph = 2.0 * PI * v;
			vec3 d = vec3(sin_t * cos(ph), cos_t, sin_t * sin(ph));
			float tg = sphere(o, d, Rg());
			float tt = sphere(o, d, Rt());
			float t_max = tg > 0.0 ? tg : tt;
			float dt = max(t_max, 0.0) / float(N);
			vec3 T = vec3(1.0);
			for (int i = 0; i < N; i++) {
				vec3 p = o + d * (float(i) + 0.5) * dt;
				Medium m = medium(length(p));
				vec3 sc = m.scatter_r + m.scatter_m;
				vec3 step_t = exp(-m.extinction * dt);
				vec3 k = (vec3(1.0) - step_t) / max(m.extinction, vec3(1e-7));
				L2 += T * sc * light_transmittance(p, l) / (4.0 * PI) * k;
				fms += T * sc * k;
				T *= step_t;
			}
			if (tg > 0.0) {
				vec3 g = o + d * tg;
				vec3 n = normalize(g);
				L2 += T * light_transmittance(g, l) * max(dot(n, l), 0.0) * albedo / PI;
			}
		}
	}
	float w = 1.0 / float(SQ * SQ);
	L2 *= w;
	fms *= w; // the share of light scattered again (an isotropic phase integrates to 1), averaged over the sphere
	vec3 psi = L2 / max(vec3(1.0) - fms, vec3(1e-3));
	imageStore(out_lut, px, vec4(psi, 1.0));
#endif

#ifdef MODE_SKY_VIEW
	ivec2 size = imageSize(out_lut);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	// u: the view's azimuth from the light, 0 to pi; v: its elevation, the horizon at 0.5, finer near it.
	float az = uv.x * PI;
	float s = uv.y * 2.0 - 1.0;
	float el = sign(s) * s * s * 0.5 * PI;
	vec3 d = vec3(cos(el) * cos(az), sin(el), cos(el) * sin(az));
	float le = asin(clamp(params.light.y, -1.0, 1.0));
	vec3 l = vec3(cos(le), sin(le), 0.0);
	vec3 o = vec3(0.0, params.radii.z, 0.0);
	float tg = sphere(o, d, Rg());
	float tt = sphere(o, d, Rt());
	float t_max = tg > 0.0 ? tg : tt;
	vec3 T;
	vec3 L = t_max > 0.0 ? integrate(o, d, t_max, 30, l, T) : vec3(0.0);
	imageStore(out_lut, px, vec4(art_tint(L, d), 1.0));
#endif

#ifdef MODE_AERIAL
	ivec3 size = imageSize(out_aerial);
	ivec2 col = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(col, size.xy))) {
		return;
	}
	vec2 uv = (vec2(col) + 0.5) / vec2(size.xy);
	vec4 far_p = camera.inv_view_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	vec3 d = normalize(far_p.xyz / far_p.w);
	vec3 o = vec3(0.0, params.radii.z, 0.0);
	vec3 l = params.light.xyz;
	float c = dot(d, l);
	float pr = phase_rayleigh(c), pm = phase_mie(c, params.mie_extinction.w);
	float m_per_km = 1.0 / max(params.misc.x, 1e-9); // the air's km in the world's metres (the aerial scale)
	// Down the column, slice by slice (slice k holds the light from the camera to ((k + 0.5) / Z)^2 of the reach), the
	// light scattered once from the light shadowed by the terrain where each sample is; the multiple scattering (the
	// sky's own light in the air) is not. Lu, unshadowed, gives the sky its shadows' share at the far end.
	vec3 L = vec3(0.0), Lu = vec3(0.0), T = vec3(1.0);
	float t_prev = 0.0;
	const int SUB = 2;
	for (int k = 0; k < size.z; k++) {
		float wk = (float(k) + 0.5) / float(size.z);
		float t_end = wk * wk * params.radii.w;
		float dt = (t_end - t_prev) / float(SUB);
		for (int j = 0; j < SUB; j++) {
			float t = t_prev + (float(j) + 0.5) * dt;
			vec3 p = o + d * t;
			float r = length(p);
			Medium m = medium(r);
			vec3 single = (m.scatter_r * pr + m.scatter_m * pm) * light_transmittance(p, l);
			vec3 multi = (m.scatter_r + m.scatter_m) * multiscatter(r, dot(p / r, l)) * params.misc.y;
			vec3 wpos = camera.cam_pos.xyz + d * (t * m_per_km);
			float vis = terrain_visibility(wpos) * cloud_visibility(wpos);
			vec3 step_t = exp(-m.extinction * dt);
			vec3 k_int = (vec3(1.0) - step_t) / max(m.extinction, vec3(1e-7));
			L += T * (single * vis + multi) * k_int;
			Lu += T * (single + multi) * k_int;
			T *= step_t;
		}
		t_prev = t_end;
		imageStore(out_aerial, ivec3(col, k), vec4(art_tint(L, d), dot(T, vec3(1.0 / 3.0))));
	}
	imageStore(out_sky_shadow, col, vec4(art_tint(L, d) - art_tint(Lu, d), 1.0));
#endif

#ifdef MODE_TERRAIN_SHADOW
	if (params.radii.x <= 0.0) {
		return; // never: keeps the push constants in this variant's layout, as every pass sets them
	}
	int n = int(camera.shadow_params.z);
	int row = int(gl_GlobalInvocationID.x);
	if (gl_GlobalInvocationID.y != 0u || row >= n) {
		return;
	}
	float side = camera.shadow_params.x;
	float texel = side / float(n);
	vec2 dir = camera.shadow_frame.zw;
	vec2 perp = vec2(-dir.y, dir.x);
	float v = ((float(row) + 0.5) / float(n) - 0.5) * side;
	float drop = texel * camera.shadow_params.w; // how far the shadow's edge falls in a texel away from the light
	float s = -1e9;
	// From the light's side away from it: each texel is in shadow below the higher of its ground and the shadow coming
	// from the texel before it, dropped by a texel's fall.
	for (int i = n - 1; i >= 0; i--) {
		float u = ((float(i) + 0.5) / float(n) - 0.5) * side;
		vec2 p = camera.shadow_frame.xy + dir * u + perp * v;
		vec2 tuv = (p - camera.terrain_rect.xy) / camera.terrain_rect.zw;
		float h = -1e9;
		if (all(greaterThanEqual(tuv, vec2(0.0))) && all(lessThanEqual(tuv, vec2(1.0)))) {
			h = camera.terrain_h.x + camera.terrain_h.y * textureLod(terrain_heights, tuv, camera.terrain_h.z).r;
		}
		// Stored: the shadow arriving from the texels toward the light, without this texel's own ground (so the ground
		// here is not in its own shadow; the air above the ground tests the same either way). Carried on: with it.
		float arriving = s - drop;
		s = max(h, arriving);
		imageStore(shadow_out, ivec2(i, row), vec4(arriving));
	}
#endif

#ifdef MODE_APPLY
	// The air volume near the camera over a colour at a distance (m): its light scattered toward the camera and its
	// transmittance, the reach and past it the whole column.
#define AIR_VOLUME(c, dist) \
	if (camera.air_volume.y > 0.5) { \
		float aw = min(sqrt(max(dist, 0.0) / camera.air_volume.x), 1.0); \
		vec4 av = textureLod(air_light, vec3(uv, aw), 0.0); \
		c.rgb = c.rgb * av.a + av.rgb * camera.air_volume.z; \
	}
	ivec2 size = ivec2(params.misc.zw);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
// The four march texels round the pixel, each weighted by its bilinear share and by whether its first cloud lies in
// front of the pixel's scene (so a ridge's edge does not take the clouds behind it).
#define CLOUDS(c, dist) \
	if (camera.cloud_flags.x > 0.5) { \
		vec2 csz = vec2(textureSize(cloud_light, 0)); \
		vec2 cp = uv * csz - 0.5; \
		ivec2 c0 = ivec2(floor(cp)); \
		vec2 cf = cp - vec2(c0); \
		vec4 cl = vec4(0.0); \
		float cmean = 0.0, cwsum = 0.0; \
		for (int cj = 0; cj < 4; cj++) { \
			ivec2 ct = clamp(c0 + ivec2(cj & 1, cj >> 1), ivec2(0), ivec2(csz) - 1); \
			float cb = ((cj & 1) != 0 ? cf.x : 1.0 - cf.x) * ((cj >> 1) != 0 ? cf.y : 1.0 - cf.y); \
			vec2 ctd = texelFetch(cloud_depth, ct, 0).rg; \
			float cwt = cb * (dist > ctd.r ? 1.0 : 0.0); \
			cl += texelFetch(cloud_light, ct, 0) * cwt; \
			cmean += ctd.g * cwt; \
			cwsum += cwt; \
		} \
		/* No texel's cloud stands in front of the scene here: none is drawn. (Each had a token weight, which the */ \
		/* division made whole again, so a ridge's pixels took the clouds behind them from every march texel that */ \
		/* also held sky: a skyline against a cloud was eaten in blocks the march's size. 2026-11-10.) */ \
		if (cwsum > 1e-5) { \
			cl /= cwsum; \
			cmean /= cwsum; \
		} else { \
			cl = vec4(0.0, 0.0, 0.0, 1.0); \
		} \
		if (cl.a < 0.999) { \
			float cw = sqrt(clamp(cmean * params.misc.x / params.radii.w, 0.0, 1.0)); \
			vec4 apc = textureLod(aerial_lut, vec3(uv, cw), 0.0); \
			c.rgb = c.rgb * cl.a + (cl.rgb * apc.a + apc.rgb * params.light.w * (1.0 - cl.a)) * camera.air_volume.z; \
		} \
	}
	float depth = texelFetch(depth_texture, px, 0).r;
	if (depth == 0.0) {
		// The sky (reverse Z): its own shader drew the atmosphere, unshadowed; the terrain's shadows in the air are taken
		// from it here (the crepuscular rays), as far as the volume reaches.
		vec4 c = imageLoad(color_image, px);
		if (camera.cam_pos.w > 0.5) {
			c.rgb = max(c.rgb + textureLod(sky_shadow, uv, 0.0).rgb * params.light.w * camera.air_volume.z, vec3(0.0));
		}
		CLOUDS(c, 1e12)
		AIR_VOLUME(c, 1e9)
		imageStore(color_image, px, c);
		return;
	}
	vec4 rel = camera.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	float dist = length(rel.xyz / rel.w) * params.misc.x; // km, times the aerial scale
	float w = sqrt(clamp(dist / params.radii.w, 0.0, 1.0));
	vec4 ap = textureLod(aerial_lut, vec3(uv, w), 0.0);
	// Near the camera the first slice's values fade in from none.
	float near = clamp(w * 32.0 - 0.5, 0.0, 1.0);
	vec4 c = imageLoad(color_image, px);
	c.rgb = c.rgb * mix(1.0, ap.a, near) + ap.rgb * params.light.w * near * camera.air_volume.z;
	float dist_m = dist / max(params.misc.x, 1e-12); // back to metres
	CLOUDS(c, dist_m)
	AIR_VOLUME(c, dist_m)
	imageStore(color_image, px, c);
#endif
}
