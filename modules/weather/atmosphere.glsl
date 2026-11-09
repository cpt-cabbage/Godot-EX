#[compute]

#version 450

#VERSION_DEFINES

// A physically based atmosphere (Godot-EX, 2026-11-09), after Hillaire, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (EGSR 2020), as Unreal's Sky Atmosphere: Rayleigh and Mie scattering with exponential
// densities, ozone absorption in a tent around 25 km, in kilometres. One source, five variants (atmosphere_effect.cpp):
// MODE_TRANSMITTANCE  the transmittance to the top of the atmosphere by altitude and zenith angle (Bruneton's mapping)
// MODE_MULTISCATTER   multiple scattering's contribution by altitude and sun angle (Hillaire's Psi = L2 / (1 - f_ms))
// MODE_SKY_VIEW       the sky's radiance seen from the camera, by the view's azimuth from the light and its elevation
// MODE_AERIAL         the in-scattering and transmittance from the camera to each froxel of a 32-slice view volume
// MODE_APPLY          the scene's colour through the aerial perspective, by its depth (the sky's pixels left alone)
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
#ifdef MODE_APPLY
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
	ivec3 cell = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(cell, size))) {
		return;
	}
	vec2 uv = (vec2(cell.xy) + 0.5) / vec2(size.xy);
	vec4 far_p = camera.inv_view_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	vec3 d = normalize(far_p.xyz / far_p.w);
	float w = (float(cell.z) + 0.5) / float(size.z);
	float dist = w * w * params.radii.w; // km: finer near the camera
	vec3 o = vec3(0.0, params.radii.z, 0.0);
	vec3 l = params.light.xyz;
	vec3 T;
	vec3 L = integrate(o, d, dist, 8 + cell.z / 2, l, T);
	imageStore(out_aerial, cell, vec4(art_tint(L, d), dot(T, vec3(1.0 / 3.0))));
#endif

#ifdef MODE_APPLY
	ivec2 size = ivec2(params.misc.zw);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	float depth = texelFetch(depth_texture, px, 0).r;
	if (depth == 0.0) {
		return; // the sky (reverse Z): its own shader drew the atmosphere
	}
	vec4 rel = camera.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	float dist = length(rel.xyz / rel.w) * params.misc.x; // km, times the aerial scale
	float w = sqrt(clamp(dist / params.radii.w, 0.0, 1.0));
	vec4 ap = textureLod(aerial_lut, vec3(uv, w), 0.0);
	// Near the camera the first slice's values fade in from none.
	float near = clamp(w * 32.0 - 0.5, 0.0, 1.0);
	vec4 c = imageLoad(color_image, px);
	c.rgb = c.rgb * mix(1.0, ap.a, near) + ap.rgb * params.light.w * near;
	imageStore(color_image, px, c);
#endif
}
