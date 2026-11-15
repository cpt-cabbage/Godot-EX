#[compute]

#version 460

#VERSION_DEFINES

// The air near the camera as a volume (Godot-EX, 2026-11-09; atmosphere_effect.h): ground haze and valley mist in
// froxels out to `fog_distance`, lit by the light through the terrain's shadow map and, near the camera, by ray-traced
// visibility (alpha-tested, so the light falls between the leaves in shafts), and by the sky. Three variants:
// MODE_INJECT     each froxel's extinction and the sun's visibility at a jittered point in it, blended with the last
//                 frame's (reprojected), no ray tracing
// MODE_INJECT_RT  the same with the light's visibility ray-traced within `rt_distance`: the one thing a froxel takes
//                 in turns (a ray in four frames), kept in a volume of its own and carried between
// MODE_INTEGRATE  front to back down each column, lit by this frame's sun and sky: the light scattered toward the
//                 camera to each froxel, and the transmittance (atmosphere.glsl's MODE_APPLY lays it over the scene)

#ifdef MODE_INJECT_RT
#extension GL_EXT_ray_query : require
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define PI 3.14159265358979

layout(set = 0, binding = 0, std140) uniform Fog {
	mat4 inv_view_proj; // NDC to the world relative to the camera (rotation and projection), m
	mat4 prev_view_proj; // the world relative to last frame's camera to its clip space
	vec4 cam_pos; // xyz the camera (m), w the frame number
	vec4 prev_cam_pos; // xyz last frame's camera, w 1 when the history is usable
	vec4 light_dir; // xyz toward the light, w tan of the light's elevation
	vec4 light_color; // rgb the light's colour times its energy at the camera (the DirectionalLight3D's), w the phase's g
	vec4 ground; // x ground haze density (1/m), y its height above the ground (m), z valley mist density (1/m), w the mist's top (world y)
	vec4 mist; // x the mist's falloff above its top (m), y noise amount (0-1), z noise scale (m), w time (s)
	vec4 albedo; // rgb the haze's albedo, w the sky light's share
	vec4 range; // x the volume's reach (m), y the ray-traced reach (m), z history weight, w the shadow edge's softness (m)
	vec4 wind; // xyz how far the wind has carried the noise (m: AtmosphereEffect's fog_offset), w 1 when the terrain shadows
	vec4 shadow_frame; // atmosphere.glsl's Camera block's
	vec4 shadow_params;
	vec4 terrain_rect;
	vec4 terrain_h; // x height_min, y height_range, z the mip for the shadow map, w 1 when the heights are bound
	vec4 sky; // x the sky table's light illuminance, y the haze under the crowns (1/m at full cover, canopy_cover), zw unused
	vec4 cloud_shadow; // xy the clouds' shadow map's origin (world x, z), z its side (m), w the clouds' base (world y)
	vec4 cloud_flags; // x 1 when the clouds' shadow map is drawn, y 1 when the clouds' panorama is (the sky's light from above)
	vec4 weather; // x the weather's mist (1/m), y its top (world y), z precipitation (0-1), w the rain's haze (1/m at 1)
	mat4 view; // the world to the camera's view space (the cascades' matrices start there)
	vec4 cascades; // x 1 when the sun's shadow cascades are rendered (no ray-traced shadows)
	vec4 canopy_frame; // the crowns' map (AtmosphereEffect::set_canopy): xy its origin (world x, z), zw 1 / its sides (m)
	vec4 canopy_params; // x its heights' scale (m at 1), y the share of the sky a crown takes, z 1 when the map is bound, w the tallest crown (m)
}
fog;

#if defined(MODE_INJECT) || defined(MODE_INJECT_RT)
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image3D out_media;
layout(set = 0, binding = 2) uniform sampler3D history;
layout(set = 0, binding = 3) uniform sampler2D terrain_heights;
layout(set = 0, binding = 4) uniform sampler2D terrain_shadow;
layout(set = 0, binding = 5) uniform sampler2D sky_view;
layout(set = 0, binding = 9) uniform sampler2D cloud_shadow_map;
#if defined(MODE_INJECT) || defined(MODE_INJECT_RT)
layout(set = 0, binding = 14) uniform sampler2D cloud_panorama;
layout(set = 0, binding = 15) uniform sampler2D canopy;
#endif
#endif
#ifdef MODE_INJECT_RT
layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 12, r8) uniform restrict writeonly image3D out_visibility;
layout(set = 0, binding = 13) uniform sampler3D visibility_history;
#define RT_ALPHA_SET 1
#include "../../servers/rendering/renderer_rd/shaders/effects/rt_alpha_inc.glsl"
#define ALPHA_MAX_PASSES 16u
#endif
#ifdef MODE_INJECT
// The sun's shadow cascades (the raster tier: without ray-traced shadows they are rendered), read as Godot's own
// volumetric fog reads them: the tree shafts without rays.
#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8
#include "../../servers/rendering/renderer_rd/shaders/light_data_inc.glsl"
layout(set = 0, binding = 10, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;
layout(set = 0, binding = 11) uniform sampler2D directional_shadow_atlas;
#endif
#ifdef MODE_INTEGRATE
layout(set = 0, binding = 7) uniform sampler3D media;
layout(set = 0, binding = 8, rgba16f) uniform restrict writeonly image3D out_light;
layout(set = 0, binding = 5) uniform sampler2D sky_view;
layout(set = 0, binding = 14) uniform sampler2D cloud_panorama;
#endif

// What a froxel keeps from frame to frame (out_media, blended with its reprojected history): r the sun's share, the
// extinction times the light's visibility and phase (times MEDIA_SUN_SCALE, for half floats), g the sky's share, the
// extinction times the share of the sky the crowns round it leave open (canopy_sky), a the extinction. No
// light: the integrate pass lights them with this frame's sun and sky. While the history held the light itself, a
// lightning flash (the sky's light through the clouds' panorama) stayed in the froxels the view had held when it struck,
// fading over half a second, and turning the camera showed it as a lit window in the air, with the wet ground's
// reflection of it (the user, 2026-11-14).
#define MEDIA_SUN_SCALE 1024.0

// The froxel distribution: distance along the view ray = reach * w^2, w = (slice + 0.5) / Z at a slice's centre.
float slice_distance(float w) {
	return fog.range.x * w * w;
}

vec3 view_dir(vec2 uv) {
	vec4 p = fog.inv_view_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	return normalize(p.xyz / p.w);
}

#if defined(MODE_INJECT) || defined(MODE_INJECT_RT) || defined(MODE_INTEGRATE)
// The sky's light reaching the air, a mean over the upper sky in the sky table (u: azimuth from the light, v: elevation,
// the horizon at 0.5), as radiance. Under clouds (2026-11-12) the light from above is theirs and the clear sky's only
// where it shows through them: the clouds' panorama (rgb their light, a the sky's transmittance; u azimuth, v the angle
// from the zenith) over the upper sky, weighted by solid angle. Before, a storm's mist was lit by the clear sky above
// its cloud: blue, and as bright as a fair day's haze, it hid the cloud.
vec3 sky_ambient() {
	vec3 s = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 3; j++) {
			s += textureLod(sky_view, vec2((float(i) + 0.5) / 4.0, 0.5 + 0.5 * (float(j) + 0.5) / 3.0), 0.0).rgb;
		}
	}
	s *= fog.sky.x / 12.0;
	if (fog.cloud_flags.y > 0.5) {
		vec3 c = vec3(0.0);
		float a = 0.0, wsum = 0.0;
		for (int j = 0; j < 3; j++) {
			float v = (float(j) + 0.5) / 6.0; // 15, 45 and 75 degrees from the zenith
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
#endif

#if defined(MODE_INJECT) || defined(MODE_INJECT_RT)
float hash13(vec3 p) {
	uvec3 v = uvec3(ivec3(floor(p)) + 65536);
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	return float(v.x) * (1.0 / 4294967296.0);
}

float value_noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float n000 = hash13(i), n100 = hash13(i + vec3(1, 0, 0)), n010 = hash13(i + vec3(0, 1, 0)), n110 = hash13(i + vec3(1, 1, 0));
	float n001 = hash13(i + vec3(0, 0, 1)), n101 = hash13(i + vec3(1, 0, 1)), n011 = hash13(i + vec3(0, 1, 1)), n111 = hash13(i + vec3(1, 1, 1));
	return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float fbm(vec3 p) {
	return 0.55 * value_noise(p) + 0.3 * value_noise(p * 2.03 + 17.1) + 0.15 * value_noise(p * 4.11 + 41.7);
}

bool in_terrain(vec2 xz, out vec2 tuv) {
	tuv = (xz - fog.terrain_rect.xy) / fog.terrain_rect.zw;
	return fog.terrain_h.w > 0.5 && all(greaterThanEqual(tuv, vec2(0.0))) && all(lessThanEqual(tuv, vec2(1.0)));
}

float ground_height(vec3 w) {
	vec2 tuv;
	if (!in_terrain(w.xz, tuv)) {
		return -1e9;
	}
	return fog.terrain_h.x + fog.terrain_h.y * textureLod(terrain_heights, tuv, 2.0).r;
}

// The haze's extinction (1/m) at a world position: a layer hugging the ground, and mist pooled below an altitude,
// both broken up by noise the wind carries.
float density(vec3 w) {
	float above = w.y - ground_height(w);
	float d = fog.ground.x * exp(-max(above, 0.0) / max(fog.ground.y, 0.1));
	d += fog.ground.z * exp(-max(w.y - fog.ground.w, 0.0) / max(fog.mist.x, 0.1));
	// The weather's mist pools below its top.
	d += fog.weather.x * exp(-max(w.y - fog.weather.y, 0.0) / max(fog.mist.x, 0.1));
	if (fog.mist.y > 0.0 && d > 0.0) {
		vec3 q = (w - fog.wind.xyz) / max(fog.mist.z, 0.01);
		d *= mix(1.0, smoothstep(0.25, 0.75, fbm(q)) * 2.0, fog.mist.y);
	}
	// Rain thickens the air (the user, 2026-11-14: "use volumetrics closer range to sell the effect ... you're relying
	// on particles too much"): a veil everywhere, curtains tens of metres across that fall at the drops' speed and
	// sweep with the wind (the grey sheets crossing a hillside in a downpour), and close to the camera, from a few
	// metres to a few tens, a fine falling grain under a metre across and several tall, the rain's texture where it is
	// too dense to see drop by drop. The drops themselves are rain.glsl's, over the picture; here they are a medium,
	// lit and shadowed with the rest of the air.
	if (fog.weather.z > 0.0 && fog.weather.w > 0.0) {
		vec3 fall = vec3(fog.wind.x, -7.0 * fog.mist.w, fog.wind.z);
		vec3 q1 = (w - fall) / vec3(22.0, 80.0, 22.0);
		float sheets = smoothstep(0.3, 0.75, 0.7 * value_noise(q1) + 0.3 * value_noise(q1 * 2.3 + 5.1));
		float dist = length(w - fog.cam_pos.xyz);
		float grain = 0.0;
		float near = smoothstep(2.0, 6.0, dist) * (1.0 - smoothstep(15.0, 45.0, dist));
		if (near > 0.0) {
			vec3 q2 = (w - fall) / vec3(0.9, 7.0, 0.9);
			grain = smoothstep(0.4, 0.85, 0.75 * value_noise(q2) + 0.25 * value_noise(q2 * 2.6 + 3.3)) * near;
		}
		d += fog.weather.z * fog.weather.w * (0.35 + 1.6 * sheets + 6.0 * grain);
	}
	return d;
}

#include "../../servers/rendering/renderer_rd/shaders/effects/sun_modulator_inc.glsl"

// How much of the sky round a point in the air the trees' crowns cover, 0 to 1 (2026-11-15, the user: "full volumetric
// lighting ... with light shafts through trees"). Lit by the whole sky, the air under a closed canopy glowed as grey as
// the open valley's and the sun's shafts had nothing dark to stand in; and haze thick enough to show them there
// whitened the valley beyond. The air under crowns takes the sky's light by their share (canopy_params.y) and holds
// haze of its own (sky.y per m at full cover). Taps on a disc round the point, turned each frame (the history averages
// them): the share of them whose crown's top stands above the point. 0 above the tallest crown, outside the map, or
// with none bound.
float canopy_cover(vec3 w, float frame_angle) {
	if (fog.canopy_params.z < 0.5) {
		return 0.0;
	}
	float ground = ground_height(w);
	if (w.y - ground > fog.canopy_params.w) {
		return 0.0;
	}
	float blocked = 0.0;
	const int N = 9;
	for (int i = 0; i < N; i++) {
		// The centre, then two rings (about 3 and 7 m): the crowns overhead and the near ones that hide the low sky.
		float r = i == 0 ? 0.0 : (i < 5 ? 3.0 : 7.0);
		float a = frame_angle + float(i) * 2.39996323; // golden angle
		vec2 xz = w.xz + r * vec2(cos(a), sin(a));
		vec2 uv = (xz - fog.canopy_frame.xy) * fog.canopy_frame.zw;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
			continue;
		}
		float top = textureLod(canopy, uv, 0.0).r * fog.canopy_params.x;
		if (top <= 0.0) {
			continue;
		}
		float gt = i == 0 ? ground : ground_height(vec3(xz.x, w.y, xz.y));
		blocked += smoothstep(-1.0, 1.0, gt + top - w.y);
	}
	return blocked / float(N);
}

float cloud_visibility(vec3 w) {
	if (fog.cloud_flags.x < 0.5) {
		return 1.0;
	}
	return sun_modulator_visibility(cloud_shadow_map, w, fog.light_dir.xyz, fog.cloud_shadow, 1e9);
}

#ifdef MODE_INJECT
// The first directional light's (the sun's) cascades at a world position: 1 past them.
float cascade_visibility(vec3 w) {
	if (fog.cascades.x < 0.5 || directional_lights.data[0].shadow_opacity < 0.001) {
		return 1.0;
	}
	vec4 v = fog.view * vec4(w, 1.0);
	float depth_z = -v.z;
	vec4 splits = directional_lights.data[0].shadow_split_offsets;
	if (depth_z >= splits.w) {
		return 1.0;
	}
	vec4 c;
	float z_range;
	if (depth_z < splits.x) {
		c = directional_lights.data[0].shadow_matrix1 * v;
		z_range = directional_lights.data[0].shadow_z_range.x;
	} else if (depth_z < splits.y) {
		c = directional_lights.data[0].shadow_matrix2 * v;
		z_range = directional_lights.data[0].shadow_z_range.y;
	} else if (depth_z < splits.z) {
		c = directional_lights.data[0].shadow_matrix3 * v;
		z_range = directional_lights.data[0].shadow_z_range.z;
	} else {
		c = directional_lights.data[0].shadow_matrix4 * v;
		z_range = directional_lights.data[0].shadow_z_range.w;
	}
	c /= c.w;
	float d = textureLod(directional_shadow_atlas, c.xy, 0.0).r;
	return exp(min(0.0, c.z - d) * z_range * 10.0); // Godot's volumetric fog's fade
}
#endif

float terrain_visibility(vec3 w) {
	if (fog.wind.w < 0.5) {
		return 1.0;
	}
	vec2 dir = fog.shadow_frame.zw;
	vec2 perp = vec2(-dir.y, dir.x);
	vec2 rel = w.xz - fog.shadow_frame.xy;
	vec2 uv = vec2(dot(rel, dir), dot(rel, perp)) / fog.shadow_params.x + 0.5;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	float s = textureLod(terrain_shadow, uv, 0.0).r;
	return smoothstep(-fog.range.w, fog.range.w, w.y - s);
}

#ifdef MODE_INJECT_RT
// The light's visibility from a point in the air: anything in the ray-traced scene hides it (leaves where their
// material's alpha covers them).
float rt_visibility(vec3 w, float t_max) {
	rayQueryEXT q;
	rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | RT_ALPHA_RAY_FLAGS, 0xFFu, w, 0.05, fog.light_dir.xyz, t_max);
	uint passed = 0u;
	while (rayQueryProceedEXT(q)) {
		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			int covers = passed >= ALPHA_MAX_PASSES ? 1 : RT_ALPHA_CANDIDATE(q);
			if (covers != 0) {
				rayQueryConfirmIntersectionEXT(q);
			} else {
				passed++;
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT ? 0.0 : 1.0;
}
#endif

float phase_hg(float c, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

#endif

void main() {
#if defined(MODE_INJECT) || defined(MODE_INJECT_RT)
	// A thread a froxel (the dispatch is the volume's size): a thread a column looping down it was 14400 threads, the
	// pass as slow as its slowest column of 64 rays whatever share of the columns traced.
	ivec3 size = imageSize(out_media);
	ivec3 cell = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(cell, size))) {
		return;
	}
	float frame = fog.cam_pos.w;
	int k = cell.z;
	// The last frame's value at the froxel's centre, where it was on last frame's grid.
	vec4 h = vec4(0.0);
	bool have = false;
#ifdef MODE_INJECT_RT
	float seen_before = 1.0;
#endif
	if (fog.prev_cam_pos.w > 0.5) {
		vec2 uvc = (vec2(cell.xy) + 0.5) / vec2(size.xy);
		float wc = (float(k) + 0.5) / float(size.z);
		vec3 pc = fog.cam_pos.xyz + view_dir(uvc) * slice_distance(wc);
		vec4 clip = fog.prev_view_proj * vec4(pc - fog.prev_cam_pos.xyz, 1.0);
		if (clip.w > 0.0) {
			vec2 puv = clip.xy / clip.w * 0.5 + 0.5;
			float pw = sqrt(length(pc - fog.prev_cam_pos.xyz) / fog.range.x);
			if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0))) && pw <= 1.0) {
				h = textureLod(history, vec3(puv, pw), 0.0);
#ifdef MODE_INJECT_RT
				seen_before = textureLod(visibility_history, vec3(puv, pw), 0.0).r;
#endif
				have = true;
			}
		}
	}
	// A point in the froxel, moved each frame (R3), the history averaging them. (R2's first step with the golden ratio
	// stood nearly still for a froxel traced every fourth frame: four of its steps come to 3.02.)
	vec3 jitter = fract(vec3(0.5) + frame * vec3(0.8191725134, 0.6710436067, 0.5497004779) + hash13(vec3(cell))) - 0.5;
	vec2 uv = (vec2(cell.xy) + 0.5 + jitter.xy) / vec2(size.xy);
	float w = (float(k) + 0.5 + jitter.z) / float(size.z);
	vec3 d = view_dir(uv);
	float dist = slice_distance(w);
	vec3 p = fog.cam_pos.xyz + d * dist;
	float cover = canopy_cover(p, 6.2831853 * fract(frame * 0.618034 + hash13(vec3(cell) + 7.0)));
	float sigma = density(p) + fog.sky.y * cover;
	float sun_share = 0.0;
	float sky_share = 0.0;
#ifdef MODE_INJECT_RT
	// The ray-traced light a froxel in four frames, by work group (8 x 8 columns: threads skipped within a group wait for
	// its tracing ones), a 2 x 2 pattern of groups turning each frame: between, a group carries the visibility it had
	// where it reprojects, and a fresh ray blends in more strongly to make up. A ray each froxel each frame cost 2 ms in
	// the forest at 4K (2026-11-09). Only the visibility takes turns: the density, the shadow maps and the lighting are
	// worked out for every froxel every frame and blended alike. While a group's whole value waited for its turn, a
	// moving camera showed the groups as a board of blocks 8 froxels wide, the fresh one apart from the three carried
	// (reprojection blurs what it carries), settling when the camera stopped (2026-11-10).
	ivec2 grp = ivec2(gl_WorkGroupID.xy);
	bool turn = ((grp.x & 1) + 2 * (grp.y & 1)) == (int(frame) & 3);
	float seen = dist < fog.range.y ? seen_before : 1.0;
#endif
	if (sigma > 1e-7) {
		float vis = terrain_visibility(p) * cloud_visibility(p);
#ifdef MODE_INJECT
		vis *= cascade_visibility(p);
#endif
#ifdef MODE_INJECT_RT
		if (dist < fog.range.y) {
			if (vis > 0.0 && (turn || !have)) {
				seen = rt_visibility(p, 2000.0);
				if (have) {
					seen = mix(seen, seen_before, clamp(1.0 - (1.0 - fog.range.z) * 4.0, 0.0, 0.98));
				}
			}
			vis *= seen;
		}
#endif
		sun_share = sigma * vis * phase_hg(dot(d, fog.light_dir.xyz), fog.light_color.w) * MEDIA_SUN_SCALE;
		sky_share = sigma * (1.0 - fog.canopy_params.y * cover);
	}
#ifdef MODE_INJECT_RT
	imageStore(out_visibility, cell, vec4(seen));
#endif
	vec4 cur = vec4(sun_share, sky_share, 0.0, sigma);
	if (have) {
		cur = mix(cur, h, clamp(fog.range.z, 0.0, 0.98));
	}
	imageStore(out_media, cell, cur);
#endif

#ifdef MODE_INTEGRATE
	ivec3 size = imageSize(out_light);
	ivec2 col = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(col, size.xy))) {
		return;
	}
	// This frame's light on what the froxels keep (MEDIA_SUN_SCALE's note).
	vec3 sun = fog.light_color.rgb / MEDIA_SUN_SCALE;
	vec3 ambient = sky_ambient() * fog.albedo.w;
	vec3 L = vec3(0.0);
	float T = 1.0;
	float prev = 0.0;
	for (int k = 0; k < size.z; k++) {
		float dist = slice_distance((float(k) + 0.5) / float(size.z));
		float dt = dist - prev;
		prev = dist;
		vec4 m = texelFetch(media, ivec3(col, k), 0);
		vec3 scatter = fog.albedo.rgb * (sun * m.r + ambient * m.g);
		float st = exp(-m.a * dt);
		L += T * (m.a > 1e-7 ? scatter * (1.0 - st) / m.a : scatter * dt);
		T *= st;
		imageStore(out_light, ivec3(col, k), vec4(L, T));
	}
#endif
}
