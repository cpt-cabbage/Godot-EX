#[compute]

#version 450

#VERSION_DEFINES

// Builds the visible light list for each 8x8 screen tile from the lights that
// were actually hit by a visibility ray this frame. Next frame the sampling
// pass reads this list to guide its samples toward lights that are likely to
// be visible, instead of sampling the whole light set blindly.
//
// One workgroup per tile; the 64 threads each contribute the light their pixel
// found visible, then the group deduplicates and compacts them into the list.

#define TILE_SIZE 8
#define LIST_SIZE 8
#define INVALID_LIGHT 0xFFFFFFFFu

layout(local_size_x = TILE_SIZE, local_size_y = TILE_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0, r32ui) uniform restrict readonly uimage2D visible_light_image;

layout(set = 1, binding = 0, std430) restrict writeonly buffer LightList {
	uint data[];
}
light_list;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int tiles_x;
	int pad0;
}
params;

shared uint candidates[TILE_SIZE * TILE_SIZE];
shared uint list_count;
shared uint tile_list[LIST_SIZE];

void main() {
	uint local_index = gl_LocalInvocationIndex;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

	if (local_index == 0u) {
		list_count = 0u;
	}
	barrier();

	uint light = INVALID_LIGHT;
	if (pixel.x < params.screen_size.x && pixel.y < params.screen_size.y) {
		light = imageLoad(visible_light_image, pixel).r;
	}
	candidates[local_index] = light;
	barrier();

	// Keep only the first occurrence of each light in the group, then append
	// survivors to the tile list until it is full.
	if (light != INVALID_LIGHT) {
		bool duplicate = false;
		for (uint i = 0u; i < local_index; i++) {
			if (candidates[i] == light) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			uint slot = atomicAdd(list_count, 1u);
			if (slot < uint(LIST_SIZE)) {
				tile_list[slot] = light;
			}
		}
	}
	barrier();

	// Write the tile's list out, padding unused entries.
	if (local_index < uint(LIST_SIZE)) {
		ivec2 tile = ivec2(gl_WorkGroupID.xy);
		uint base = uint(tile.y * params.tiles_x + tile.x) * uint(LIST_SIZE);
		light_list.data[base + local_index] = local_index < min(list_count, uint(LIST_SIZE)) ? tile_list[local_index] : INVALID_LIGHT;
	}
}
