#[compute]

#version 460

#VERSION_DEFINES

// The air near the camera as a volume (Godot-EX, 2026-11-09; atmosphere_effect.h): ground haze and valley mist in
// froxels out to `fog_distance`, lit by the light through the terrain's shadow map and, near the camera, by ray-traced
// visibility (alpha-tested, so the light falls between the leaves in shafts), and by the sky. Three variants:
// MODE_INJECT     each froxel's scattering and extinction at a jittered point in it, blended with the last frame's
//                 (reprojected), no ray tracing
// MODE_INJECT_RT  the same with the light's visibility ray-traced within `rt_distance`: the one thing a froxel takes
//                 in turns (a ray in four frames), kept in a volume of its own and carried between
// MODE_INTEGRATE  front to back down each column: the light scattered toward the camera to each froxel, and the
//                 transmittance (atmosphere.glsl's MODE_APPLY lays it over the scene)

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
	vec4 wind; // xyz the wind carrying the noise (m/s), w 1 when the terrain shadows
	vec4 shadow_frame; // atmosphere.glsl's Camera block's
	vec4 shadow_params;
	vec4 terrain_rect;
	vec4 terrain_h; // x height_min, y height_range, z the mip for the shadow map, w 1 when the heights are bound
	vec4 sky; // x the sky table's light illuminance, yzw unused
	vec4 cloud_shadow; // xy the clouds' shadow map's origin (world x, z), z its side (m), w the clouds' base (world y)
	vec4 cloud_flags; // x 1 when the clouds' shadow map is drawn
	vec4 weather; // x the weather's mist (1/m), y its top (world y), z precipitation (0-1)
	mat4 view; // the world to the camera's view space (the cascades' matrices start there)
	vec4 cascades; // x 1 when the sun's shadow cascades are rendered (no ray-traced shadows)
}
fog;

#if defined(MODE_INJECT) || defined(MODE_INJECT_RT)
layout(set = 0, binding = 1, rgba16f) uniform restrict writeonly image3D out_media;
layout(set = 0, binding = 2) uniform sampler3D history;
layout(set = 0, binding = 3) uniform sampler2D terrain_heights;
layout(set = 0, binding = 4) uniform sampler2D terrain_shadow;
layout(set = 0, binding = 5) uniform sampler2D sky_view;
layout(set = 0, binding = 9) uniform sampler2D cloud_shadow_map;
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
#endif

// The froxel distribution: distance along the view ray = reach * w^2, w = (slice + 0.5) / Z at a slice's centre.
float slice_distance(float w) {
	return fog.range.x * w * w;
}

vec3 view_dir(vec2 uv) {
	vec4 p = fog.inv_view_proj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	return normalize(p.xyz / p.w);
}

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
	// The weather's mist pools below its top, and rain thickens the air (a heavy shower: about 1 km of sight).
	d += fog.weather.x * exp(-max(w.y - fog.weather.y, 0.0) / max(fog.mist.x, 0.1));
	d += fog.weather.z * 0.0015;
	if (fog.mist.y > 0.0 && d > 0.0) {
		vec3 q = (w - fog.wind.xyz * fog.mist.w) / max(fog.mist.z, 0.01);
		d *= mix(1.0, smoothstep(0.25, 0.75, fbm(q)) * 2.0, fog.mist.y);
	}
	return d;
}

#include "../../servers/rendering/renderer_rd/shaders/effects/sun_modulator_inc.glsl"

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

// The sky's light reaching the air, a mean over the upper sky in the sky table (u: azimuth from the light, v: elevation,
// the horizon at 0.5), as radiance.
vec3 sky_ambient() {
	vec3 s = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 3; j++) {
			s += textureLod(sky_view, vec2((float(i) + 0.5) / 4.0, 0.5 + 0.5 * (float(j) + 0.5) / 3.0), 0.0).rgb;
		}
	}
	return s / 12.0 * fog.sky.x;
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
	vec3 ambient = sky_ambient() * fog.albedo.w;
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
	float sigma = density(p);
	vec3 scatter = vec3(0.0);
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
		vec3 sun = fog.light_color.rgb * phase_hg(dot(d, fog.light_dir.xyz), fog.light_color.w) * vis;
		scatter = fog.albedo.rgb * sigma * (sun + ambient);
	}
#ifdef MODE_INJECT_RT
	imageStore(out_visibility, cell, vec4(seen));
#endif
	vec4 cur = vec4(scatter, sigma);
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
	vec3 L = vec3(0.0);
	float T = 1.0;
	float prev = 0.0;
	for (int k = 0; k < size.z; k++) {
		float dist = slice_distance((float(k) + 0.5) / float(size.z));
		float dt = dist - prev;
		prev = dist;
		vec4 m = texelFetch(media, ivec3(col, k), 0);
		float st = exp(-m.a * dt);
		L += T * (m.a > 1e-7 ? m.rgb * (1.0 - st) / m.a : m.rgb * dt);
		T *= st;
		imageStore(out_light, ivec3(col, k), vec4(L, T));
	}
#endif
}
