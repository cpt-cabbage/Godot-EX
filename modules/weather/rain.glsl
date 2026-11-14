#[compute]

#version 450

#VERSION_DEFINES

// Rain (Godot-EX, 2026-11-14; rain_effect.h): drops in the world, found per pixel. Each of three grids of cells (fine
// near the camera, coarse far off) is carried down and along by the rain's fall (the cells' offset), and a cell holds
// one drop or none (a hash of its index against the rain's density): a streak along the drop's motion relative to the
// camera over an exposure, so the drops stand in the world and the camera moves past them, and a car's speed slants
// them toward it. Each pixel's view ray walks the cells it crosses (Amanatides and Woo) up to the scene's depth, so a
// wall or a windscreen frame hides the drops behind it, and a drop under the cover (the top-down heightfield of the
// valley's particle collider: roofs, a car's cab, tree crowns) is not drawn. A drop is lit as a lens: the sky's and
// the clouds' light from above, the light's glint through it and the spot lights (a torch, headlights), and it is
// seen through the air volume's light and transmittance to its distance, as the scene behind it is. Laid over the
// upscaled picture before tonemapping, so the temporal upscaler never smears or drops the thin moving streaks.
// Besides the drops (the user, 2026-11-14: "rain effect should be more than just shap particles"; after Tatarchuk's
// ToyShop rain, SIGGRAPH 2006, and Lagarde's for Remember Me): splashes where the scene faces up under the open sky (a
// ring and droplets thrown up, from the depth, so never on a crown or in the air), and a rain mist in sheets that
// fall with the rain and drift with the wind, with spray where the view meets the ground.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PI 3.14159265358979
#define LEVELS 3
#define MAX_SPOTS 4

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1, rgba16f) uniform restrict image2D color_image;
layout(set = 0, binding = 2) uniform sampler2D sky_view;
layout(set = 0, binding = 3) uniform sampler2D cloud_panorama;
layout(set = 0, binding = 4) uniform sampler3D air_light;
layout(set = 0, binding = 5) uniform sampler2D cover_map;
layout(set = 0, binding = 7) uniform sampler3D mist_noise; // 32^3 random values, repeating, filtered: value noise
layout(set = 0, binding = 6, std140) uniform Params {
	mat4 inv_proj; // NDC (unjittered) to the world relative to the camera (m)
	vec4 cam; // xyz the camera (world), w the near fade's end (m)
	vec4 streak; // xyz the streak's direction (world, unit: the drop's motion against the camera's), w its length (m)
	vec4 rain; // x the density (0-1), y a drop's radius (m), z the opacity, w a pixel's angle (rad)
	vec4 level_cell[LEVELS]; // x a cell's side across, y its height (m), z the chance a cell holds a drop at density 1, w the opacity's scale
	vec4 level_range[LEVELS]; // x from (m), y to (m), z the crossfade's half-width (m), w steps at most
	vec4 level_base[LEVELS]; // xyz the fall's offset in whole cells
	vec4 level_frac[LEVELS]; // xyz the rest of it (m)
	vec4 cover_centre; // xyz the cover's box centre (world), w 1 when the cover map is there
	vec4 cover_extents; // xyz its half-size (m)
	vec4 air; // x the air volume's reach (m), y 1 when it is drawn, z the camera's exposure, w 1 when the clouds' panorama is
	vec4 sky; // x the sky table's light illuminance, y the sky light's share, z the light's glint, w the spot lights' share
	vec4 light_dir; // xyz toward the light, w the glint's forward lobe (g)
	vec4 light_color; // rgb the light at the camera
	vec4 size; // xy the output's size, zw the depth's
	vec4 spot_pos[MAX_SPOTS]; // xyz the light, w its range (m)
	vec4 spot_dir[MAX_SPOTS]; // xyz where it points, w the cosine of its outer edge
	vec4 spot_color[MAX_SPOTS]; // rgb colour times energy, w the cosine of its inner edge
	vec4 counts; // x spot lights, y the colour's tint share
	vec4 tint; // rgb the drops' own tint
	vec4 splash; // x splashes a cell a second at density 1, y a cell's side (m), z their reach (m), w their size's scale
	vec4 mist; // x the rain mist's density (1/m) at density 1, y its noise's scale across (m), z up and down (m), w its reach (m)
	vec4 mist_offset; // xyz the rain's fall so far (m, wrapped at the noise's period: 32 of its cells), w the time (s, wrapped)
}
params;

shared vec3 group_ambient;

uvec4 hash44(uvec4 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	v ^= v >> 16u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	return v;
}

vec4 rand4(ivec3 c, int level) {
	return vec4(hash44(uvec4(uvec3(c + 1048576), uint(level) * 7919u + 17u))) * (1.0 / 4294967296.0);
}

// The closest approach of the view ray (t along dir, from the camera) and a segment centred at c (relative to the
// camera) along unit v, |s| <= len: x the distance between them, y the ray's t there, z s.
vec3 ray_segment(vec3 dir, vec3 c, vec3 v, float len) {
	float b = dot(dir, v);
	float d = -dot(dir, c);
	float e = -dot(v, c);
	float den = max(1.0 - b * b, 1e-5);
	float s = clamp((e - b * d) / den, -len, len);
	vec3 q = c + v * s;
	float tc = dot(dir, q);
	return vec3(length(dir * tc - q), tc, s);
}

// air_volume.glsl's sky_ambient: the clear sky's mean light over the upper sky, under clouds theirs where they cover it.
vec3 sky_ambient() {
	vec3 s = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 3; j++) {
			s += textureLod(sky_view, vec2((float(i) + 0.5) / 4.0, 0.5 + 0.5 * (float(j) + 0.5) / 3.0), 0.0).rgb;
		}
	}
	s *= params.sky.x / 12.0;
	if (params.air.w > 0.5) {
		vec3 c = vec3(0.0);
		float a = 0.0, wsum = 0.0;
		for (int j = 0; j < 3; j++) {
			float v = (float(j) + 0.5) / 6.0;
			float w = sin(v * PI);
			for (int i = 0; i < 8; i++) {
				vec4 cp = textureLod(cloud_panorama, vec2((float(i) + 0.5) / 8.0, v), 0.0);
				c += cp.rgb * w;
				a += cp.a * w;
				wsum += w;
			}
		}
		s = s * (a / wsum) + c / wsum;
	}
	return s;
}

bool covered(vec3 w) {
	if (params.cover_centre.w < 0.5) {
		return false;
	}
	vec3 l = (w - params.cover_centre.xyz) / params.cover_extents.xyz;
	if (any(greaterThan(abs(l.xz), vec2(1.0)))) {
		return false;
	}
	float h = textureLod(cover_map, l.xz * 0.5 + 0.5, 0.0).r;
	return l.y < h * 2.0 - 1.0;
}

// The light a drop at p (world) sends toward the eye, looking along view, from the spot lights.
vec3 spot_light(vec3 p, vec3 view) {
	vec3 s = vec3(0.0);
	for (int i = 0; i < int(params.counts.x); i++) {
		vec3 to = p - params.spot_pos[i].xyz;
		float d = length(to);
		float range = params.spot_pos[i].w;
		if (d >= range || d < 1e-3) {
			continue;
		}
		vec3 l = to / d;
		float cone = smoothstep(params.spot_dir[i].w, params.spot_color[i].w, dot(l, params.spot_dir[i].xyz));
		if (cone <= 0.0) {
			continue;
		}
		float x = d / range;
		float fall = max(1.0 - x * x * x * x, 0.0);
		// A drop glints far brighter with the light coming toward the eye (a beam seen across or against it) than lit
		// from the eye's side: l runs from the light to the drop, view from the eye to it.
		float lobe = mix(0.6, 4.0, pow(max(-dot(l, view), 0.0), 4.0));
		s += params.spot_color[i].rgb * cone * fall * fall / max(d * d, 0.04) * lobe;
	}
	return s;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (gl_LocalInvocationIndex == 0u) {
		group_ambient = sky_ambient();
	}
	barrier();
	ivec2 size = ivec2(params.size.xy);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec2 ndc = uv * 2.0 - 1.0;
	vec4 np = params.inv_proj * vec4(ndc, 1.0, 1.0);
	vec3 dir = normalize(np.xyz / np.w);
	float scene = 1e9;
	vec3 surface = vec3(0.0); // the scene's point (relative to the camera) and its normal, from the depth
	vec3 normal = vec3(0.0);
	{
		ivec2 dsize = ivec2(params.size.zw);
		ivec2 dp = clamp(ivec2(uv * params.size.zw), ivec2(0), dsize - 1);
		float depth = texelFetch(depth_texture, dp, 0).r;
		if (depth > 0.0) {
			vec4 wp = params.inv_proj * vec4(ndc, depth, 1.0);
			surface = wp.xyz / wp.w;
			scene = length(surface);
			// The normal from the next texels across and down (away from the screen's edge).
			ivec2 ox = ivec2(dp.x + 1 < dsize.x ? 1 : -1, 0);
			ivec2 oy = ivec2(0, dp.y + 1 < dsize.y ? 1 : -1);
			float dx = texelFetch(depth_texture, dp + ox, 0).r;
			float dy = texelFetch(depth_texture, dp + oy, 0).r;
			if (dx > 0.0 && dy > 0.0) {
				vec2 tuv = (vec2(dp) + 0.5) / params.size.zw;
				vec4 a0 = params.inv_proj * vec4(tuv * 2.0 - 1.0, depth, 1.0);
				vec4 ax = params.inv_proj * vec4((tuv + vec2(ox) / params.size.zw) * 2.0 - 1.0, dx, 1.0);
				vec4 ay = params.inv_proj * vec4((tuv + vec2(oy) / params.size.zw) * 2.0 - 1.0, dy, 1.0);
				vec3 p0 = a0.xyz / a0.w;
				normal = normalize(cross(ax.xyz / ax.w - p0, ay.xyz / ay.w - p0));
				if (dot(normal, dir) > 0.0) {
					normal = -normal;
				}
			}
		}
	}

	// Under a roof (a car's cab, a room), the drops start past where the view leaves the cover, and a little more: glass
	// is not in the cover's map, so the space under a car's sloping windscreen reads as open sky.
	float t_min = 0.0;
	if (covered(params.cam.xyz + vec3(0.0, 0.1, 0.0))) {
		t_min = 4.0;
		for (int i = 1; i <= 40; i++) {
			float t = float(i) * 0.1;
			if (!covered(params.cam.xyz + dir * t)) {
				t_min = t + 0.6;
				break;
			}
		}
	}

	vec3 ambient = group_ambient * params.sky.y;
	vec3 sv = params.streak.xyz;
	float half_len = params.streak.w * 0.5;
	float radius = params.rain.y;
	vec3 sun = params.light_color.rgb * params.sky.z;
	float g = params.light_dir.w;
	float hg = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * dot(dir, params.light_dir.xyz), 1e-4), 1.5));
	vec3 drop_lit = (ambient + sun * hg * 4.0 * PI) * mix(vec3(1.0), params.tint.rgb, params.counts.y);

	vec3 acc = vec3(0.0);
	float trans = 1.0;
	int debug_steps = 0;
	for (int lv = 0; lv < LEVELS; lv++) {
		vec4 cell = params.level_cell[lv];
		vec4 range = params.level_range[lv];
		float chance = cell.z * params.rain.x;
		if (chance <= 0.0 || trans < 0.02) {
			continue;
		}
		float t0 = max(range.x - range.z, 0.05);
		float t1 = min(range.y + range.z, scene);
		if (t0 >= t1) {
			continue;
		}
		vec3 cs = vec3(cell.x, cell.y, cell.x);
		vec3 frac = params.level_frac[lv].xyz;
		ivec3 base = ivec3(params.level_base[lv].xyz);
		// The ray in the cells' frame (relative to the camera, so the numbers stay small): the cell index of a world
		// point w is floor((w - frac) / cs) - base.
		vec3 o = (params.cam.xyz - frac) / cs;
		vec3 ocell = floor(o);
		vec3 ofs = o - ocell; // the camera within its cell
		vec3 dg = dir / cs;
		vec3 p0 = ofs + dg * t0;
		ivec3 c = ivec3(floor(p0));
		ivec3 stp = ivec3(sign(dg));
		vec3 inv = 1.0 / max(abs(dg), vec3(1e-6));
		vec3 nextb = vec3(c) + max(vec3(stp), vec3(0.0));
		vec3 tmax = t0 + (nextb - p0) * vec3(stp) * inv;
		tmax = mix(tmax, vec3(1e9), lessThan(abs(dg), vec3(1e-6)));
		vec3 tdelta = inv;
		ivec3 cam_cell = ivec3(ocell) - base;
		float t = t0;
		int steps = int(range.w);
		// Margins keep a streak inside its cell: its half-length along each axis and its width.
		vec3 margin = (abs(sv) * half_len * 1.4 + vec3(radius * 4.0)) / cs;
		margin = min(margin, vec3(0.45));
		for (int i = 0; i < steps && t < t1; i++) {
			debug_steps++;
			vec4 r = rand4(c + cam_cell, lv);
			// The drop's centre relative to the camera, and how far it passes from the ray: most pass wide of it (more
			// than the streak's half-length and a pixel's width there) and are done with at once.
			vec3 local = margin + r.yzw * (1.0 - 2.0 * margin);
			vec3 cr = (vec3(c) + local - ofs) * cs;
			float along_ray = dot(dir, cr);
			float reach = half_len * 1.4 + 3.0 * max(radius, params.rain.w * along_ray * 0.8) + 1e-3;
			bool near_ray = dot(cr, cr) - along_ray * along_ray <= reach * reach && along_ray >= t_min - reach && along_ray <= scene + reach;
			if (r.x < chance && near_ray) {
				// Each drop its own length and brightness (from the hash's spare bits).
				float own = r.x / max(chance, 1e-6);
				float len = half_len * mix(0.6, 1.4, fract(own * 97.0));
				float bright = mix(0.35, 1.0, fract(own * 61.0));
				vec3 rs = ray_segment(dir, cr, sv, len);
				float dist = rs.x;
				float tc = rs.y;
				float s = rs.z;
				float near_fade = smoothstep(0.25, params.cam.w, tc);
				float lf = smoothstep(range.x - range.z, range.x + range.z, tc) * (1.0 - smoothstep(range.y - range.z, range.y + range.z, tc));
				if (tc > t_min && tc < scene && lf * near_fade > 0.0) {
					// Soft across (a Gaussian, at least 0.8 of a pixel: the streaks were hard lines), and as much fainter as
					// it is wider than the drop, so its light is kept; brighter in bands along its length, as an
					// oscillating drop's streak is (Garg and Nayar 2006).
					float sigma = max(radius, params.rain.w * tc * 0.8);
					float across = exp(-0.5 * (dist * dist) / (sigma * sigma));
					if (dist < 3.0 * sigma && !covered(params.cam.xyz + cr)) {
						float along = s / max(len, 1e-4);
						float bands = 0.8 + 0.2 * sin(along * 7.0 + fract(own * 13.0) * 40.0);
						float a = params.rain.z * cell.w * min(radius / sigma, 1.0) * 0.8 * across * bright * bands * (1.0 - along * along) * lf * near_fade;
						a = clamp(a, 0.0, 0.95);
						vec3 lit = drop_lit + spot_light(params.cam.xyz + cr, dir) * params.sky.w;
						vec3 col = lit;
						if (params.air.y > 0.5) {
							vec4 av = textureLod(air_light, vec3(uv, min(sqrt(tc / params.air.x), 1.0)), 0.0);
							col = lit * av.a + av.rgb;
						}
						if (params.counts.z > 2.5) {
							// GODOT_RAIN_DEBUG=3: the drops in a bright flat colour (their alpha alone); 4: their colour
							// as lit, without the air.
							col = params.counts.z > 3.5 ? lit : vec3(4.0);
						}
						acc += trans * a * col * params.air.z;
						trans *= 1.0 - a;
						if (trans < 0.02) {
							break;
						}
					}
				}
			}
			// The next cell.
			if (tmax.x < tmax.y && tmax.x < tmax.z) {
				t = tmax.x;
				tmax.x += tdelta.x;
				c.x += stp.x;
			} else if (tmax.y < tmax.z) {
				t = tmax.y;
				tmax.y += tdelta.y;
				c.y += stp.y;
			} else {
				t = tmax.z;
				tmax.z += tdelta.z;
				c.z += stp.z;
			}
		}
	}
	// Splashes on open ground: a drop's crown rising from where it lands and a ring opening on the surface, from cells
	// on the ground round the pixel's surface (and back toward the camera, at a grazing view, for crowns standing in
	// front of it), each with splashes coming at random times. Only on surfaces facing up, under the open sky.
	vec4 splash = vec4(0.0); // premultiplied light and coverage
	if (params.splash.x > 0.0 && params.rain.x > 0.0 && scene < params.splash.z && normal.y > 0.7) {
		vec3 pw = params.cam.xyz + surface;
		if (!covered(pw + vec3(0.0, 0.05, 0.0))) {
			float cs = params.splash.y;
			float rate = params.splash.x * params.rain.x; // a cell's splashes a second
			const float DUR = 0.2; // s a splash lasts
			float time = params.mist_offset.w;
			float px_m = params.rain.w * scene; // a pixel's width at the surface
			vec2 back = length(dir.xz) > 1e-3 ? normalize(dir.xz) : vec2(0.0);
			vec2 centres[2] = vec2[2](pw.xz, pw.xz - back * min(0.12 / max(-dir.y, 0.08), 0.8));
			ivec2 first = ivec2(floor(centres[0] / cs));
			for (int k = 0; k < 2; k++) {
				ivec2 cc = ivec2(floor(centres[k] / cs));
				if (k == 1 && all(lessThanEqual(abs(cc - first), ivec2(1)))) {
					continue;
				}
				for (int j = 0; j < 9; j++) {
					ivec2 cell = cc + ivec2(j % 3 - 1, j / 3 - 1);
					vec4 r = rand4(ivec3(cell, 0), 7);
					float phase = time * rate + r.x;
					float age = fract(phase) / rate;
					if (age > DUR) {
						continue;
					}
					vec4 e = rand4(ivec3(cell.x, int(floor(phase)) & 65535, cell.y), 8);
					vec2 ixz = (vec2(cell) + e.xy) * cs;
					// On the surface's plane through the pixel's point.
					float iy = pw.y - (normal.x * (ixz.x - pw.x) + normal.z * (ixz.y - pw.z)) / normal.y;
					vec3 ic = vec3(ixz.x, iy, ixz.y) - params.cam.xyz;
					float f = age / DUR;
					float size = params.splash.w * mix(0.6, 1.4, e.z);
					float a = 0.0;
					// The ring on the surface (only where the pixel is that surface).
					float ds = length((surface - ic).xz);
					if (k == 0 && ds < size * 0.07 + 2.0 * px_m) {
						float rr = size * (0.01 + 0.05 * sqrt(f));
						float rw = max(0.003 * size, px_m * 0.7);
						float dr = ds - rr;
						a = exp(-0.5 * dr * dr / (rw * rw)) * min(0.003 * size / rw, 1.0) * 0.18 * (1.0 - f) * (1.0 - f);
					}
					// The crown: droplets thrown up and out from where the drop landed, falling back on their arcs, each a
					// short streak over the drops' exposure. Drawn as a solid sheet a crown read as a white blob, and as a
					// thin-walled cylinder as a little tombstone; a filmed splash would do better still.
					// Whether the view passes near the crown at all (its droplets stay within about 8 cm of where it
					// landed), before any droplet.
					vec3 r0 = ray_segment(dir, ic + vec3(0.0, 0.04 * size, 0.0), vec3(0.0, 1.0, 0.0), 0.04 * size);
					bool near_crown = r0.x < 0.08 * size + 3.0 * params.rain.w * r0.y && r0.y < scene + 0.1;
					for (int m = 0; m < 4 && near_crown; m++) {
						vec4 dk = fract(e.wzyx * vec4(1.6180, 2.4142, 3.3028, 4.2361) * float(m + 1) + vec4(0.123, 0.456, 0.789, 0.321) * float(m));
						float ang = (dk.x + float(m)) * (PI * 0.5);
						float vo = size * mix(0.25, 0.6, dk.y); // m/s out
						float vu = size * mix(0.7, 1.3, dk.z); // m/s up
						float ta = age;
						float y = vu * ta - 4.9 * ta * ta;
						if (y < 0.0) {
							continue;
						}
						vec3 pos = ic + vec3(cos(ang) * vo * ta, y, sin(ang) * vo * ta);
						vec3 vel = vec3(cos(ang) * vo, vu - 9.8 * ta, sin(ang) * vo);
						float vl = length(vel);
						float hl = max(vl * 0.01, 1e-4); // half its streak over 0.02 s
						vec3 rs = ray_segment(dir, pos - vel * 0.01, vel / max(vl, 1e-4), hl);
						float rad = 0.0012 * size * mix(0.6, 1.2, dk.w);
						float sg = max(rad, params.rain.w * rs.y * 0.7);
						if (rs.y < scene + 0.02 && rs.x < 3.0 * sg) {
							a = max(a, exp(-0.5 * rs.x * rs.x / (sg * sg)) * min(rad / sg, 1.0) * 1.6 * (1.0 - f * f));
						}
					}
					// Fading out with distance: a crown below a pixel, drawn a pixel wide, reads as glitter.
					a = clamp(a * params.rain.z * mix(0.5, 1.0, e.w) * (1.0 - smoothstep(params.splash.z * 0.3, params.splash.z, scene)), 0.0, 0.9);
					splash += (1.0 - splash.a) * vec4(a * (drop_lit + spot_light(params.cam.xyz + ic, dir) * params.sky.w), a);
				}
			}
			splash.rgb *= params.air.z;
		}
	}

	// The rain mist: curtains of rain too far and too many to draw drop by drop, a thin medium along the view ray from
	// a few metres out to its reach whose density follows a noise that falls with the rain and drifts with the wind
	// (sheets sweeping across a hillside), and spray where the view meets the ground. Lit as the drops are; 8 samples,
	// spaced finer near, each pixel's offset by its own interleaved noise.
	float mist_t = 1.0;
	vec3 mist_l = vec3(0.0);
	if (params.mist.x > 0.0 && params.rain.x > 0.0) {
		float t_start = max(t_min, 4.0);
		float t_end = min(scene, params.mist.w);
		if (t_end > t_start) {
			float jitter = fract(52.9829189 * fract(dot(vec2(px), vec2(0.06711056, 0.00583715))));
			float sa = sqrt(t_start), sb = sqrt(t_end);
			vec3 scale = vec3(params.mist.y, params.mist.z, params.mist.y);
			for (int i = 0; i < 8; i++) {
				float ta = mix(sa, sb, float(i) / 8.0);
				float tb = mix(sa, sb, float(i + 1) / 8.0);
				ta *= ta;
				tb *= tb;
				float t = mix(ta, tb, jitter);
				vec3 w = params.cam.xyz + dir * t;
				vec3 q = (w - params.mist_offset.xyz) / scale;
				float n = 0.65 * textureLod(mist_noise, q / 32.0, 0.0).r + 0.35 * textureLod(mist_noise, q * (2.7 / 32.0) + 0.37, 0.0).r;
				float d = params.mist.x * params.rain.x * smoothstep(0.5, 0.85, n);
				// Spray in the last metre or so before the view meets the ground.
				if (scene < 1e8 && normal.y > 0.5) {
					d += params.mist.x * params.rain.x * 6.0 * exp(-(scene - t) / 1.0);
				}
				if (covered(w)) {
					d = 0.0;
				}
				float tau = d * (tb - ta);
				float tr = exp(-tau);
				mist_l += mist_t * (1.0 - tr) * drop_lit;
				mist_t *= tr;
			}
			mist_l *= params.air.z;
		}
	}

	if (params.counts.z > 4.5) {
		// GODOT_RAIN_DEBUG=5: red where a point 0.3 m above the scene's surface is under the cover, green the cover map's
		// height there (0 at its box's foot, 1 at its top), blue 1 when the map is bound.
		vec3 w = params.cam.xyz + dir * min(scene, 40.0) + vec3(0.0, 0.3, 0.0);
		vec3 l = (w - params.cover_centre.xyz) / params.cover_extents.xyz;
		float h = textureLod(cover_map, l.xz * 0.5 + 0.5, 0.0).r;
		// On or off (1000 or 0), whatever the exposure: red covered, green the map's height above half its box, blue
		// bands of a tenth of it.
		imageStore(color_image, px, vec4(covered(w) ? 1000.0 : 0.0, h > 0.5 ? 1000.0 : 0.0, (params.cover_centre.w > 0.5 && fract(h * 10.0) > 0.5) ? 1000.0 : 0.0, 1.0));
		return;
	}
	if (params.counts.z > 1.5 && params.counts.z < 2.5) {
		// GODOT_RAIN_DEBUG=2: the drops' light (before the air) over the whole screen, at the exposure.
		imageStore(color_image, px, vec4(drop_lit * params.air.z, 1.0));
		return;
	}
	if (params.counts.z > 0.5 && params.counts.z < 1.5) {
		// GODOT_RAIN_DEBUG=1: red the scene's distance (a tenth of it, to 10 m), green the drops' cover, blue cells walked.
		imageStore(color_image, px, vec4(min(scene / 10.0, 1.0), 1.0 - trans, float(debug_steps) / 100.0, 1.0));
		return;
	}
	if (trans > 0.999 && splash.a <= 0.0 && mist_t > 0.999) {
		return;
	}
	// Splashes on the surface, the mist in front of them, the drops in front of all.
	vec4 bg = imageLoad(color_image, px);
	vec3 c = bg.rgb * (1.0 - splash.a) + splash.rgb;
	c = c * mist_t + mist_l;
	imageStore(color_image, px, vec4(c * trans + acc, bg.a));
}
