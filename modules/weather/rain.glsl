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
	{
		ivec2 dp = clamp(ivec2(uv * params.size.zw), ivec2(0), ivec2(params.size.zw) - 1);
		float depth = texelFetch(depth_texture, dp, 0).r;
		if (depth > 0.0) {
			vec4 wp = params.inv_proj * vec4(ndc, depth, 1.0);
			scene = length(wp.xyz / wp.w);
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
			float reach = half_len * 1.4 + max(radius, params.rain.w * along_ray) + 1e-3;
			bool near_ray = dot(cr, cr) - along_ray * along_ray <= reach * reach && along_ray >= t_min - reach && along_ray <= scene + reach;
			if (r.x < chance && near_ray) {
				float len = half_len * mix(0.6, 1.4, fract(r.x * 97.0 / max(chance, 1e-6)));
				// The closest approach of the view ray (t along dir) and the streak (s along sv, |s| <= len).
				float b = dot(dir, sv);
				float d = -dot(dir, cr);
				float e = -dot(sv, cr);
				float den = max(1.0 - b * b, 1e-5);
				float s = clamp((e - b * d) / den, -len, len);
				vec3 q = cr + sv * s;
				float tc = dot(dir, q);
				float dist = length(dir * tc - q);
				float near_fade = smoothstep(0.25, params.cam.w, tc);
				float lf = smoothstep(range.x - range.z, range.x + range.z, tc) * (1.0 - smoothstep(range.y - range.z, range.y + range.z, tc));
				if (tc > t_min && tc < scene && lf * near_fade > 0.0) {
					// Thinner than a pixel, a streak is drawn two pixels wide (a tent a pixel each side of its line, so a
					// pixel's centre cannot miss it) and as much fainter: its light is kept.
					float foot = params.rain.w * tc;
					float reff = max(radius, foot);
					float across = max(1.0 - dist / reff, 0.0);
					if (across > 0.0 && !covered(params.cam.xyz + cr)) {
						float along = s / max(len, 1e-4);
						float a = params.rain.z * cell.w * min(radius / reff, 1.0) * across * (1.0 - along * along) * lf * near_fade;
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
	if (trans > 0.999) {
		return;
	}
	vec4 bg = imageLoad(color_image, px);
	imageStore(color_image, px, vec4(bg.rgb * trans + acc, bg.a));
}
