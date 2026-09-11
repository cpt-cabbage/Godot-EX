#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

// One level of the lighting atlas's mip chain, weighted by coverage: a
// mip texel averages only the texels under it that a card filled, and
// carries the covered fraction in its alpha for the next level. A plain
// box downsample averaged the cards with the black between and around
// them, and the gather's cone-filtered reads (a footprint of a quarter of
// the hit distance, mip levels 2-5 across a room) lost 6-12% of the
// bounce in the game room -- only when a flashlight lit it, the beam being
// the one bright thing the cards held. Level 0's alpha is the relight
// count, so its coverage comes from the depth atlas (a captured texel has
// a depth; an unfilled one is zero).
//
// Only the tiles the lighting pass wrote this frame are rebuilt: a tile is
// 32 level-0 texels square, one texel at the coarsest level, so each mip
// texel belongs to exactly one tile and a chain rebuilt tile by tile stays
// consistent. The dispatch still covers the whole level; a clean tile's
// thread returns at once.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D source; // The finer level.
layout(set = 0, binding = 1) uniform texture2D depth_atlas; // Level 0's coverage.
layout(set = 0, binding = 2, rgba16f) uniform restrict writeonly image2D destination;
layout(set = 0, binding = 3, std430) restrict buffer MipDirty {
	uint tiles[];
}
mip_dirty;

layout(push_constant, std430) uniform Params {
	uvec2 size; // The destination's.
	uint source_is_level0;
	uint level; // The destination's mip level.
	uint tiles_x; // Tiles across the atlas.
	uint full; // 1: rebuild every tile.
	uint last; // 1: the coarsest level, which clears the tiles' marks behind it (one texel per tile).
	uint pad;
}
params;

void main() {
	uvec2 p = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(p, params.size))) {
		return;
	}
	uvec2 tile = (p << params.level) >> 5u;
	uint tile_index = tile.y * params.tiles_x + tile.x;
	if (params.full == 0u && mip_dirty.tiles[tile_index] == 0u) {
		return;
	}
	if (params.last != 0u) {
		// The levels run in order with a barrier between: this one is the
		// last reader of the mark, and clears it for the next frame (no
		// buffer clear between the passes, whose encoder shifted the
		// profile's timestamps by half a millisecond).
		mip_dirty.tiles[tile_index] = 0u;
	}
	ivec2 base = ivec2(p) * 2;
	vec3 sum = vec3(0.0);
	float weight = 0.0;
	for (int dy = 0; dy < 2; dy++) {
		for (int dx = 0; dx < 2; dx++) {
			ivec2 t = base + ivec2(dx, dy);
			vec4 s = texelFetch(source, t, 0);
			float w = params.source_is_level0 != 0u ? (texelFetch(depth_atlas, t, 0).r > 0.0 ? 1.0 : 0.0) : s.a;
			// Nothing non-finite is carried up the chain.
			if (any(isnan(s.rgb)) || any(isinf(s.rgb))) {
				w = 0.0;
			}
			sum += s.rgb * w;
			weight += w;
		}
	}
	vec3 rgb = weight > 0.0 ? sum / weight : vec3(0.0);
	imageStore(destination, ivec2(p), vec4(rgb, weight * 0.25));
}
