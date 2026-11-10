#[compute]

#version 450

#VERSION_DEFINES

// The sun's shadow from the terrain and the clouds as a screen mask (Godot-EX, 2026-11-10; sun_shadow_effect.h), for a
// renderer without ray-traced shadows: the scene shader multiplies the first directional light by the mask it reads at
// binding 38 (the ray-traced mask's, white when unused) on top of its shadow cascades. So the cascades give the near
// shadows (trees, rocks, the walker) and the mask the mountains' at any distance and the clouds'.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1, r8) uniform restrict writeonly image2D out_mask;
layout(set = 0, binding = 2) uniform sampler2D terrain_shadow;
layout(set = 0, binding = 3) uniform sampler2D cloud_shadow;
layout(set = 0, binding = 4, std140) uniform Params {
	mat4 inv_view_proj; // NDC to the world (m)
	vec4 shadow_frame; // atmosphere.glsl's: xy the terrain map's centre (world x, z), zw toward the light (x, z)
	vec4 shadow_params; // x its side (m), y the edge's softness (m)
	vec4 cloud_frame; // xy the clouds' map's corner (world x, z), z its side (m), w the clouds' base (world y)
	vec4 light; // xyz toward the light, w unused
	vec4 flags; // x 1 the terrain shadows, y 1 the clouds shadow, zw the screen's size
	vec4 canopy_frame; // see canopy_visibility; canopy_params.x 0 without a canopy map
	vec4 canopy_params;
	vec4 ground_frame;
	vec4 ground_params;
}
params;
layout(set = 0, binding = 5) uniform sampler2D sun_canopy;
layout(set = 0, binding = 6) uniform sampler2D sun_canopy_ground;

#include "../../servers/rendering/renderer_rd/shaders/effects/sun_modulator_inc.glsl"

// The sun's light under the crowns of trees that cast no shadow of their own (far ones, drawn as impostors; Godot-EX,
// 2026-11-10): a height field's shadow from a map of the crowns (`canopy`: R their tops above the ground, times cp.x m;
// G how far from the camera that tree's last shadow-casting level ends, times cp.y m; frames are a corner in world x, z
// and one over the sides) and the ground's heights (gp.x + gp.y * R). A point `p` is shaded where the ray to the sun
// passes under a crown before it clears the tallest (gp.z m), and a crown counts only once its own tree is past that
// distance from the camera: nearer, the tree casts its real shadow, and a second one from the map put every tree still
// drawn as a mesh in deeper shade the further off it stood (a Protea's crown 1.3 times brighter 15 m nearer). No
// pixel nearer than cp.z less the ray's reach is touched; gp.w is how much of the sun a crown takes. Eight taps
// along the ray, the ground between its ends taken as a plane; nothing where a coarse read finds no crown along it.
float canopy_visibility(sampler2D canopy, sampler2D ground, vec3 p, vec3 to_sun, vec3 cam, vec4 cf, vec4 cp, vec4 gf, vec4 gp) {
	float dist = distance(p, cam);
	if (to_sun.y < 0.03) {
		return 1.0;
	}
	float g0 = gp.x + gp.y * textureLod(ground, (p.xz - gf.xy) * gf.zw, 0.0).r;
	float above = max(p.y - g0, 0.0);
	if (above >= gp.z) {
		return 1.0;
	}
	float reach = min((gp.z - above) / to_sun.y, 160.0);
	float across = reach * length(to_sun.xz);
	if (dist + across < cp.z) {
		return 1.0;
	}
	vec2 mid = (p.xz + to_sun.xz * reach * 0.5 - cf.xy) * cf.zw;
	if (any(lessThan(mid, vec2(0.0))) || any(greaterThan(mid, vec2(1.0)))) {
		return 1.0;
	}
	if (textureLod(canopy, mid, log2(max(across, cp.w) / cp.w) + 2.0).r <= 0.0) {
		return 1.0;
	}
	vec3 end = p + to_sun * reach;
	float g1 = gp.x + gp.y * textureLod(ground, (end.xz - gf.xy) * gf.zw, 0.0).r;
	// The taps crowd toward the point (the square of an even spacing), each read as coarsely as the gap to the next: a
	// small tree's shadow is short and needs the map's own texels, a tall one's long and its crown wide. Evenly spaced
	// over the tallest tree's reach, a 6 m Protea alone cast a third of the shadow its meshes did.
	// A point up in a crown starts beyond it (0.7 of its height above the ground along the ray): the map's dome is
	// no measure of which leaves of its own crown shade it, and the taps at its foot put every far crown in shade
	// (leaves at 0.16 of the grass's luminance against 0.34 for the same tree's meshes).
	float near = min(0.7 * above, reach);
	float occ = 0.0;
	for (int i = 0; i < 8; i++) {
		float e = (float(i) + 0.5) / 8.0;
		float f = e * e;
		vec3 q = p + to_sun * mix(near, reach, f);
		float lod = log2(max(max(dist * 0.0015, across * e / 6.0) / cp.w, 1.0));
		vec2 c = textureLod(canopy, (q.xz - cf.xy) * cf.zw, lod).rg;
		float h = c.r * cp.x;
		// Past its last shadow-casting level, over the stretch that level's crossfade and the engine's slack take.
		float own = c.g * cp.y;
		float far = smoothstep(own, own * 1.15, distance(q, cam));
		occ = max(occ, far * smoothstep(0.5, 2.5, h) * smoothstep(0.0, 2.0, mix(g0, g1, f) + h - q.y));
	}
	return 1.0 - occ * gp.w;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.flags.zw);
	if (any(greaterThanEqual(px, size))) {
		return;
	}
	float depth = texelFetch(depth_texture, px, 0).r;
	if (depth == 0.0) {
		imageStore(out_mask, px, vec4(1.0));
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec4 wp = params.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 w = wp.xyz / wp.w;
	float vis = 1.0;
	if (params.flags.x > 0.5) {
		vec2 dir = params.shadow_frame.zw;
		vec2 rel = w.xz - params.shadow_frame.xy;
		vec2 suv = vec2(dot(rel, dir), dot(rel, vec2(-dir.y, dir.x))) / params.shadow_params.x + 0.5;
		if (all(greaterThanEqual(suv, vec2(0.0))) && all(lessThanEqual(suv, vec2(1.0)))) {
			float s = textureLod(terrain_shadow, suv, 0.0).r;
			// The map holds the shadow arriving from the ground toward the light: lit above it, a few metres for the map's
			// 11 m texels' averaged heights.
			vis *= smoothstep(-3.0, 3.0, w.y + 1.5 - s);
		}
	}
	if (params.flags.y > 0.5) {
		vis *= sun_modulator_visibility(cloud_shadow, w, params.light.xyz, params.cloud_frame, 1e9);
	}
	if (params.canopy_params.x > 0.0 && vis > 0.0) {
		vec4 cam = params.inv_view_proj * vec4(0.0, 0.0, 1.0, 1.0); // the near plane's middle: the camera, within centimetres
		vis *= canopy_visibility(sun_canopy, sun_canopy_ground, w, params.light.xyz, cam.xyz / cam.w, params.canopy_frame, params.canopy_params, params.ground_frame, params.ground_params);
	}
	imageStore(out_mask, px, vec4(vis));
}
