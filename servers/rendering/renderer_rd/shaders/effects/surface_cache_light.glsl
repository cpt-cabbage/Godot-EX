#[compute]

#version 460

#VERSION_DEFINES

// The wavefront split (plan section 95, GODOT_CARD_WAVEFRONT): the kernel
// below is also built as three, as the GI gather was (section 94).
// SC_SETUP runs the kernel with its ray points emitting requests instead
// of tracing -- the texels, the light selection, every ray's direction --
// and skips the accumulation; SC_TRACE is a linear dispatch over the
// requests: the shadow rays with their coverage test, the bounce and the
// dynamic lights' rays through their holes (card_hole) and the dynamic
// landings' connections; SC_RESOLVE runs the kernel again with the ray
// points reading the answers. The card lookups, the light sums and the
// accumulation's filter taps then run without the intersector's registers
// beside them. Setup and resolve must not mention the acceleration
// structure (the Metal container routes on the capability). The planar
// mirrors' chains and image legs, and the per-texel (non-quad) layout,
// keep the single kernel: the host splits only a frame without them.
#if defined(SC_SETUP) || defined(SC_RESOLVE)
#define SC_NO_RAYS
#endif
#if defined(SC_SETUP) || defined(SC_TRACE) || defined(SC_RESOLVE)
#define SC_SPLIT
#endif

#ifndef SC_NO_RAYS
#extension GL_EXT_ray_query : require
#endif
#extension GL_KHR_shader_subgroup_basic : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable
#extension GL_KHR_shader_subgroup_ballot : enable
#extension GL_EXT_samplerless_texture_functions : enable

// Surface cache lighting: shades the card texels of this frame's active card
// sets. Per texel, the position and normal come back out of the capture
// (card depth plus the card's orthographic frame), direct light is the sun
// and the local lights (the world light grid's cell, else the set's box
// list) with one shadow ray per directional light and one for the drawn
// local, area or image light per relight (a ratio estimator over the
// list, like the direct pass; up to four legs through mirrors), indirect
// light is the cards' own cosine bounce (trace_bounce), the SDFGI
// lightprobes or the sky only where the ray finds no card, and emission
// rides on top. The result accumulates in the radiance atlas the GI gather
// reads at ray hits.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "../oct_inc.glsl"
#include "surface_cache_inc.glsl"

#define MAX_LIGHTS_PER_SET 32u
#define M_PI 3.14159265359
#define SDFGI_MAX_CASCADES 8
#define SDFGI_OCT_SIZE 6

#ifndef SC_NO_RAYS
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
#endif

layout(set = 0, binding = 1, std430) restrict readonly buffer Sets {
	CardSet data[];
}
sets;

// The prepare pass's work: the active sets (bit 31 of an entry: every block
// of the set, else the requested tiles alone) and the list of blocks to
// light, one workgroup each (entry | card << 16 | block << 19).
layout(set = 0, binding = 2, std430) restrict readonly buffer Active {
	uint count;
	uint rr_count;
	uint item_count;
	uint pending;
	uint period;
	uint item_count_full;
	uint pending_young;
	uint item_cost;
	uint item_cost_full;
	uint items_lod[4];
	uint pad[3];
	uint list[SURFACE_CACHE_MAX_SETS];
	uint items[];
}
active_sets;

layout(set = 0, binding = 3, std430) restrict readonly buffer SetLights {
	uint data[];
}
set_lights;

layout(set = 0, binding = 4, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 5, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

// A uniform buffer, as the scene shader binds it (8 = RendererSceneRender::MAX_DIRECTIONAL_LIGHTS).
layout(set = 0, binding = 6, std140) uniform DirectionalLights {
	DirectionalLightData data[8];
}
directional_lights;

#define MAX_MIRROR_PLANES 4u
struct MirrorPlane {
	vec4 plane;
	vec4 params;
	vec4 center;
	vec4 u_axis;
	vec4 v_axis;
};

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 world_from_view;
	vec4 camera_origin;
	vec4 sky_quat_or_color;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float sky_energy;
	vec2 sky_border;
	uint flags;
	uint temporal_frames;
	uint atlas_size;
	uint debug; // Ablations (GODOT_CARD_ABLATE): 1 no bounce ray, 2 no shadow rays, 4 no local lights, 8 no directional lights, 16 no gradient, 32 no bounce restart, 64 no visibility restart, 16384 strict card lookups; experiment 1048576 the bounce gradient as its group's share (GODOT_CARD_GRAD_MEAN); diagnostics 128/256/512/2048/131072/262144 paint views (paint, paint2, paint3, paint5, paint8, paintn), 4096 dynamic-ray stats, 8192 tier stats (GODOT_GI_TIER_PRINT).
	vec3 grid_origin; // The world light grid, when FLAG_GRID: its corner, cell size, cells per edge, entries per cell.
	float grid_cell;
	uint grid_n;
	uint grid_cap;
	float pad_bounce_floor;
	uint young_rays; // Extra bounce rays for a texel whose accumulation is young (see trace_bounce_young).
	uint dynamic_rays; // Light rays per dynamic light per texel per relight (see trace_dynamic).
	float dynamic_motion; // How far the dynamic lights moved this frame, over the distance that refreshes their bounce whole (0 at rest, 1 and above a full refresh): caps the dynamic histories' length at its inverse (see accumulate).
	float dynamic_window; // The most relights the dynamic histories accumulate.
	float dynamic_change; // How much the dynamic lights' intensity or colour changed this frame, relative (a hue turning at constant luminance is a change the luminance below cannot see).
	float dynamic_join; // On the frame a light joins the dynamic set (every set relit): the share of its bounce the static accumulation holds, which it sheds (see accumulate). 0 otherwise.
	uint area_light_count; // The area lights (every one in the population: a few, each tested for range per texel).
	float pad_dynamic_mark;
	uint lod_rays; // Per level, a byte: the cosine rays a representative traces per relight (see main).
	vec4 luma_weights; // The working colour space's luminance weights (ColorManagement), rgb.
	MirrorPlane mirrors[MAX_MIRROR_PLANES]; // The scene's planar mirrors (mirror_planes_inc.glsl), world space.
	uint mirror_count;
	uint mirror_order; // The longest image chain evaluated (1: single images, 2: pairs too).
	uint mirror_debug; // GODOT_MIRROR_ABLATE bits (profiling): 1 no image lights, 2 no mirror continuation of the bounce rays, 4 the F0 fold kept on the mirrors' texels, 8 no area-light images, 16 the area-light images weighted through the drawn rect point (see shade_images).
	uint mirror_pad2;
	// The wavefront split (see the header): x the work items it covers (the
	// single kernel takes the items from there on, all of them when 0), y
	// the bounce records per thread (the most cosine rays a texel traces),
	// z the light rays per dynamic light, w the ray records per thread (y,
	// the gradient's, and z per dynamic light).
	uvec4 split;
}
params;

#ifdef SC_SPLIT
// The split's hand-over, per thread of a covered work item (thread index
// item * 64 + the local index): a header -- x the shadow rays requested,
// a bit each (texel k's directional light i at k * (directional count + 1)
// + i, its drawn light at k * (count + 1) + count), y the ones found
// occluded (the trace kernel's atomicOr), z the ray records requested, w
// unused -- then per texel of the quad two uvec4, its ray origin and
// normal (octahedral) and the drawn light's target and caster mask, and
// params.split.w ray records: the bounce rays (split.y), the gradient's,
// the dynamic lights' (split.z each), each written by the setup kernel as
// the direction and the texel it leaves from and answered in place by the
// trace kernel (flags, t, the hit instance). The requests are one uint
// each, thread * 64 + the code (a shadow bit, or 32 + the record).
layout(set = 1, binding = 0, std430) restrict buffer SplitCount {
	uint count;
}
split_count;
layout(set = 1, binding = 1, std430) restrict buffer SplitArgs {
	uvec4 groups;
}
split_args;
layout(set = 1, binding = 2, std430) restrict buffer SplitRequests {
	uint data[];
}
split_requests;
layout(set = 1, binding = 3, std430) restrict buffer SplitHeaders {
	uvec4 data[];
}
split_headers;
layout(set = 1, binding = 4, std430) restrict buffer SplitTexels {
	uvec4 data[]; // Eight per thread.
}
split_texels;
layout(set = 1, binding = 5, std430) restrict buffer SplitRecords {
	uvec4 data[];
}
split_records;
// The single kernel's indirect arguments for the items past the split's
// (the setup kernel's first thread writes them; the host resets them).
layout(set = 1, binding = 6, std430) restrict buffer SplitRestArgs {
	uvec4 groups;
}
split_rest_args;

#define SPLIT_HIT 1u
#define SPLIT_CONNECTED 2u // A dynamic landing's connection to the texel is clear.
#define SPLIT_GRADIENT_SLOT params.split.y
#define SPLIT_DYN_SLOT(i, s) (params.split.y + 1u + (i) * params.split.z + (s))

uint split_thread = 0u;
uint split_shadow_mask = 0u; // Setup: the shadow bits requested; resolve: the header's x.
uint split_shadow_hits = 0u; // Resolve: the header's y.
uint split_ray_mask = 0u; // Setup: the records requested; resolve: the header's z.
#else
#define SPLIT_GRADIENT_SLOT 0u
#endif

#include "mirror_planes_inc.glsl"

#define FLAG_SDFGI 1u
#define FLAG_SKY_MODE_SKY 2u
#define FLAG_SKY_MODE_COLOR 4u
#define FLAG_SHARED_BOUNCE_RAY 16u // One bounce ray per 2x2 quad: a thread per quad, a workgroup per 16x16 texels.
#define FLAG_DYN_FILTER 128u // The static and the dynamic bounce histories filtered over the card at every age (filter_bounces; GODOT_CARD_DYN_FILTER=0 clears it).
#define FLAG_DYNAMIC_YOUNG 64u // The young texels' extra cosine rays while a dynamic light moves (GODOT_CARD_DYN_YOUNG).
#define FLAG_GRID 32u // The world light grid was built this frame: a texel inside it reads its cell's lights.
#define FLAG_SPLAT_CELL 256u // A coarse relight's cell (see light_main): the bounce filter's taps a cell apart, every texel's own material over the shared lighting (GODOT_CARD_LOD_SPLAT=0 clears it).

layout(set = 0, binding = 8) uniform texture2D albedo_atlas;
layout(set = 0, binding = 9) uniform texture2D normal_atlas;
layout(set = 0, binding = 10) uniform texture2D emission_atlas;
layout(set = 0, binding = 11) uniform texture2D depth_atlas;
layout(set = 0, binding = 39) uniform texture2D specular_atlas; // The capture's F0 (rgb) and roughness (a) per texel.
layout(set = 0, binding = 12, rgba16f) uniform restrict image2D lighting_atlas;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell;
	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 13, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	ProbeCascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

layout(set = 0, binding = 14) uniform texture2DArray lightprobe_texture;
layout(set = 0, binding = 15) uniform texture3D occlusion_texture;
layout(set = 0, binding = 16) uniform sampler linear_sampler_mipmaps;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 17) uniform texture2DArray sky_radiance;
#else
layout(set = 0, binding = 17) uniform texture2D sky_radiance;
#endif

// The cache reads itself for its indirect term: each 2x2 quad of texels
// traces one cosine ray per relight (FLAG_SHARED_BOUNCE_RAY; one per texel
// without it, young texels four) and, where it lands on another card, takes that
// card's outgoing radiance from the previous update. Emission and direct
// light so propagate bounce by bounce through the cards, with nothing
// depending on the probes for geometry that moves.
layout(set = 0, binding = 18, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;

layout(set = 0, binding = 19, rgba16f) uniform restrict image2D indirect_atlas;

// The relight requests (surface_cache_inc.glsl). The bounce rays' landings
// do not ask: a request from here relit what the ray happened to land on,
// whose relight's ray asked for more, and within a few frames the pending
// set was the whole reachable level (20k blocks on the TPS bridge against
// a 2k budget), the screen's own surfaces relit no oftener than the rest.
// The gather's reads ask; a wall only ever seen through a bounce is the
// round robin's, which keeps a share of the budget (section 77).
layout(set = 0, binding = 20, std430) restrict buffer CardRequests {
	uint frame[SURFACE_CACHE_MAX_SETS];
	uint tiles[];
}
card_requests;

// The direct term's temporal gradients and its accumulated visibility, six
// halves packed in four uints (see change_load / change_store):
// - the unshadowed direct radiance (times albedo, plus emission) at the
//   last relight, in colour: a hue change at constant brightness is a
//   change too;
// - its relative change since the relight before, the radiance gradient.
//   The unshadowed term is deterministic, so a change in it is a change in
//   the lights, not noise: A-SVGF's temporal gradient with the relight as
//   the re-shade. It restarts the bounce accumulation and the GI gather
//   reads it at hits to restart the pixel's history;
// - the local lights' geometric sum (attenuation times cosine, unit
//   colour) at the last relight. Its change is the geometric gradient: a
//   light that moved, appeared or vanished, which is what can change a
//   texel's visibility; a light's colour or intensity changing does not,
//   so it restarts the visibility ratio and nothing else;
// - the accumulated visibility ratio of the local lights (the ratio
//   estimator: the unshadowed sum is exact every relight, the shadow ray
//   only measures what fraction of it arrives);
// - what the last relight's bounce ray hit: the set (16 bits, 0xFFFF for
//   none) and the distance (a half). The next relight traces that same ray
//   again (its seed is the relight's frame, see Relit) and compares:
//   A-SVGF's gradient sample for the bounce, on the hit's identity rather
//   than its radiance. It is what catches a surface moving out of the ray's
//   way -- an emissive box that swept past a floor and left, whose glow the
//   accumulated bounce otherwise kept for the whole window (rt_lab
//   temporal_test descend: 0.078 against the converged reference at the
//   stop, still 0.035 thirty frames on); the radiance gradient above sees
//   only a light changing. Comparing the radiance instead was measured and
//   is a chain reaction: a restarted texel's radiance is one sample's, every
//   texel whose re-traced ray lands on it reads that as a change and
//   restarts too, and in the game project's dense room the cards never
//   settled (the glossy floor flickered at 46 hot pixels per thousand at
//   rest, 0.2 with the gradient off). What a ray hits is deterministic on a
//   still scene, and a light's change already reaches the bounce through
//   the hit card's own gradient.
layout(set = 0, binding = 21, rgba32ui) uniform restrict uimage2D change_atlas;

// Per set, two frames: the relight before the last and the last (the prepare
// pass promotes the last to the previous when it selects the set).
#define TILE_STAMP_STRIDE 1024u
#define TILE_STAMPS (TILE_STAMP_STRIDE * TILE_STAMP_STRIDE)
layout(set = 0, binding = 23, std430) restrict buffer Relit {
	uint frame[SURFACE_CACHE_MAX_SETS * 2u];
	uint tile_prev[TILE_STAMPS]; // Per 8x8 atlas block (a tile's first): the relight before this one (the prepare pass promotes as it lists the tile).
	uint tile_last[TILE_STAMPS];
	uint tile_lod[TILE_STAMPS]; // The level of this relight (low four bits) and of the one before (the next four), set by the prepare pass.
}
relit;

// A relight at a coarse level (plan section 92): the thread's texel stands
// for its cell of splat_side texels square, and everything accumulate
// stores for it is written over the cell's captured texels, so every
// reader of the level-0 atlases (the gather's depth test and youth read,
// the hit shader, the translucency froxels, this pass's own bounce reads
// and filters) sees a complete, if blocky, card. The reads accumulate
// makes are the representative's own. splat_mask marks the captured
// texels of the cell, row-major, found once per thread.
ivec2 splat_origin = ivec2(0);
int splat_side = 1;
uvec2 splat_mask = uvec2(0u);
// The tile's previous relight was at another level: the stored terms this
// relight would compare against are another texel's (the representative's,
// or this texel's own where the last relight was finer), so nothing is
// read as a change and the previous ray is not re-traced; the
// accumulations carry on from what the cell holds.
bool level_changed = false;
#define SPLAT_STORE(img, texel, value) \
	{ \
		if (splat_side == 1) { \
			imageStore(img, texel, value); \
		} else { \
			for (int sy = 0; sy < splat_side; sy++) { \
				for (int sx = 0; sx < splat_side; sx++) { \
					uint bi = uint(sy * splat_side + sx); \
					if (((bi < 32u ? splat_mask.x >> bi : splat_mask.y >> (bi - 32u)) & 1u) != 0u) { \
						imageStore(img, splat_origin + ivec2(sx, sy), value); \
					} \
				} \
			} \
		} \
	}

// The dynamic lights' bounce (see trace_dynamic), its own history: rgb the
// accumulated term, a the luminance of the dynamic lights' direct term at
// the last relight (for the total change, see accumulate).
layout(set = 0, binding = 24, rgba16f) uniform restrict image2D indirect_dyn_atlas;

// The dynamic lights (see trace_dynamic): the lights that moved or changed
// lately, in world space, with their weights (1 while a light changes,
// fading to 0 once it has rested; LightStorage tracks them, surface_cache.cpp
// uploads them). pad is 1 for a spot. The GI gather reads the same buffer:
// their direct term is not in the lighting atlas (see accumulate), it is
// added at the hits from the lights' current state.
layout(set = 0, binding = 27, std430) restrict readonly buffer DynamicLights {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
	vec4 weights[2];
	vec4 prev_color[8]; // Each light's colour last frame (the gather reads it; the cards do not).
	LightData data[8];
}
dyn_lights;
// The dynamic lights' landings, shared by every texel this frame (see
// trace_dynamic; dyn_lights.pad1 of them per light, 0 when each texel
// traces its own light rays): two uvec4 each, the landing's position and
// area density (0: the light ray found no lit card face), then its
// radiance from the light alone and its card's change (halves) and its
// normal (octahedral, unorm16 x 2). The SC_LANDINGS kernel writes them.
layout(set = 0, binding = 36, std430) restrict buffer DynLandings {
	uvec4 data[];
}
dyn_landings;
// Their bounces after the first, apart: the cosine rays' reading of both
// histories at their hits (see trace_bounce); alpha its relights over 64.
layout(set = 0, binding = 26, rgba16f) uniform restrict image2D indirect_dyn2_atlas;
// The static lights' radiance alone (the lighting atlas less the dynamic
// bounces), what the static cosine rays read at their hits, and in alpha
// the visibility ratio for the dynamic lights' direct term at the gather's
// hits. Subtracting the dynamic histories from the lighting atlas instead
// read the two from different moments of the same dispatch.
layout(set = 0, binding = 28, rgba16f) uniform restrict image2D static_atlas;

// The area lights (the population's, like the omni and spot buffers) and
// the atlas their textures live in, read through linear_sampler_mipmaps.
layout(set = 0, binding = 29, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;
layout(set = 0, binding = 30) uniform texture2D area_light_atlas;
// The projector textures (a spot's cookie, an omni's dual paraboloid map),
// sampled where a light's projector_rect is set (see projector_factor).
layout(set = 0, binding = 31) uniform texture2D decal_atlas_srgb;
// The dynamic spots' cookies as sampling tables (LightStorage::ProjectorTable,
// one slot per dynamic light, dyn_lights.pad0 bit i set where slot i has
// one): the marginal CDF over the 16 rows of the projector's frame, the
// conditional CDF within each row, then the density (integrating to one
// over the frame). The light rays draw their direction from it
// (cookie_sample) and both estimators divide by its solid-angle density
// (cookie_pdf_omega), so a cookie that transmits a fifth on average no
// longer wastes four rays in five on its dark texels.
layout(set = 0, binding = 32, std430) restrict readonly buffer ProjectorTables {
	float data[];
}
proj_tables;
// The dynamic bounces as the readers take them (the gather's fallback and
// hits, the hit shader, this pass's own recursion): both histories summed,
// and filtered over the card (filter_bounces); the
// alpha is the age the readers should read it at (the filter's samples
// counted in), 64ths.
layout(set = 0, binding = 33, rgba16f) uniform restrict image2D indirect_dyn_filtered_atlas;
// The static bounce accumulation the same way: filtered over the card for
// the readers (the gather's fallback and youth, the hit shader), the
// accumulation itself staying raw in indirect_atlas. Alpha: its relights.
layout(set = 0, binding = 34, rgba16f) uniform restrict image2D indirect_filtered_atlas;
// The atlas tiles written this relight (32 texels square), for the mip
// chain to rebuild only those (surface_cache.cpp update_lighting).
layout(set = 0, binding = 37, std430) restrict writeonly buffer MipDirty {
	uint tiles[];
}
mip_dirty;
// Per set: [0] once any texel was relit, [1] once a filled one was (see
// SurfaceCache::set_captured_empty).
layout(set = 0, binding = 38, std430) restrict writeonly buffer SetState {
	uint state[];
}
set_state;

// How settled the cards are, for the editor's idle repaints (see
// RenderForwardClustered::_request_ray_tracing_convergence): of the texels
// relit this frame, how many still have a bounce accumulation short of its
// window or a live change mark. Counted when FLAG 4096 is set.
layout(set = 0, binding = 35, std430) restrict buffer Converge {
	uint young;
	uint relit;
	// The bounce's signed drift this relight, summed over the texels as the
	// luminance that rose and the luminance that fell (fixed point): a
	// converged field's relights cancel, a field still climbing through its
	// bounces (every texel's window full, all of them trending) does not.
	uint up;
	uint down;
	uint gradient_t[8]; // Diagnostics (gradset): the bounce gradient's verdicts: same / near / far / another set, then the other-set ones by their distance: same / near / far.
	// Diagnostics (GODOT_CARD_CHANGE_PRINT): of the texels relit this frame
	// with a change mark over 0.05, which term set it (CHANGE_SRC_*), and
	// in [7] the texels reset.
	uint change_src[8];
}
converge;
#define CHANGE_SRC_STATIC 0u // The static lights' unshadowed term moved.
#define CHANGE_SRC_DYN 1u // The dynamic lights' direct term moved.
#define CHANGE_SRC_HIT 2u // The bounce ray's hit carried a change.
#define CHANGE_SRC_GRADIENT 3u // The re-traced bounce ray hit at another distance.
#define CHANGE_SRC_SPREAD 4u // A neighbour's mark.
#define CHANGE_SRC_PREV 5u // The texel's own mark, fading.
#define CHANGE_SRC_GEOM 6u // The local lights' geometric sum moved (not a mark; counted when over 0.02).
#define PROJ_TABLE_N 16u
#define PROJ_TABLE_FLOATS 528u

bool cookie_table(uint i) {
	return (dyn_lights.pad0 & (1u << i)) != 0u;
}

// Half the spot angle's tangent: the frame the cookie is projected through
// (light_storage.cpp _fill_card_light_data, a perspective over twice the
// spot angle, aspect one).
float cookie_tan_half(LightData ld) {
	float c = clamp(ld.cone_angle, 0.01, 0.9999);
	return sqrt(1.0 - c * c) / c;
}

// The sampling's density in solid angle at a world point: the cell's
// density over the frame times the perspective's Jacobian r^3 / (4 t^2)
// (a frame cell of area du dv at NDC (x, y) covers the solid angle
// 4 t^2 du dv / r^3, r the distance to the image plane's point). Zero
// outside the frame.
float cookie_pdf_omega(uint i, LightData ld, vec3 world_pos) {
	vec4 sp = ld.shadow_matrix * vec4(world_pos, 1.0);
	if (sp.w <= 0.0) {
		return 0.0;
	}
	sp.xy /= sp.w;
	if (any(lessThan(sp.xy, vec2(0.0))) || any(greaterThanEqual(sp.xy, vec2(1.0)))) {
		return 0.0;
	}
	uvec2 cell = uvec2(clamp(sp.x * 16.0, 0.0, 15.0), clamp((1.0 - sp.y) * 16.0, 0.0, 15.0));
	float density = proj_tables.data[i * PROJ_TABLE_FLOATS + 272u + cell.y * 16u + cell.x];
	float t = cookie_tan_half(ld);
	vec2 ndc = sp.xy * 2.0 - 1.0;
	float r2 = 1.0 + dot(ndc * t, ndc * t);
	return density * r2 * sqrt(r2) / (4.0 * t * t);
}

// A direction drawn from the table: r0 picks the row, r1 the column in it,
// and what is left of each within its cell jitters the point. The frame's
// rows run from the top (splane.y = 1) down, as the cookie is stored.
vec3 cookie_sample(uint i, LightData ld, vec3 axis, float r0, float r1, out float r_pdf_omega) {
	uint base = i * PROJ_TABLE_FLOATS;
	uint j = 0u;
	float c0 = 0.0;
	for (; j < 15u; j++) {
		float c1 = proj_tables.data[base + j];
		if (r0 < c1) {
			break;
		}
		c0 = c1;
	}
	float cj = proj_tables.data[base + j];
	float jv = clamp((r0 - c0) / max(cj - c0, 1e-6), 0.0, 1.0);
	uint rbase = base + 16u + j * 16u;
	uint k = 0u;
	float d0 = 0.0;
	for (; k < 15u; k++) {
		float d1 = proj_tables.data[rbase + k];
		if (r1 < d1) {
			break;
		}
		d0 = d1;
	}
	float dk = proj_tables.data[rbase + k];
	float ju = clamp((r1 - d0) / max(dk - d0, 1e-6), 0.0, 1.0);
	vec2 sp = vec2((float(k) + ju) / 16.0, 1.0 - (float(j) + jv) / 16.0);
	float t = cookie_tan_half(ld);
	vec2 ndc = sp * 2.0 - 1.0;
	// Light space: the frame's right and up (the spot's area fields carry
	// them), the axis at -z.
	vec3 d_l = vec3(ndc * t, -1.0);
	float r2 = dot(d_l, d_l);
	r_pdf_omega = proj_tables.data[base + 272u + j * 16u + k] * r2 * sqrt(r2) / (4.0 * t * t);
	return normalize(ld.area_width * d_l.x + ld.area_height * d_l.y + axis);
}

// Diagnostics (GODOT_CARD_ABLATE=stats): the dynamic rays' fate, counted
// (see trace_dynamic; surface_cache.cpp prints and clears it).
layout(set = 0, binding = 25, std430) restrict buffer DynStats {
	// [0..15] the dynamic rays' fate (dyn_stat); [16..19] and [20..23] the
	// static bounce ray's source, counts and luminance sums (tier_stat);
	// [24..28] why a card lookup failed (card_reject); [29..30] the count and
	// luminance of the bounces read through a failed depth test.
	uint count[32];
}
dyn_stats;

// Diagnostics (GODOT_GI_TIER_PRINT, debug bit 8192): where a texel's static
// bounce ray got its radiance. i: 0 a card, 1 the SDFGI probes at the
// texel (the hit had no card), 2 the sky at the texel (no card, no probes),
// 3 a miss (sky).
// Luminance sums are in 1/16 units.
float luminance(vec3 c);
void tier_stat(uint i, vec3 radiance) {
	if ((params.debug & 8192u) != 0u) {
		atomicAdd(dyn_stats.count[16u + i], 1u);
		atomicAdd(dyn_stats.count[20u + i], uint(min(luminance(max(radiance, vec3(0.0))), 64.0) * 16.0));
	}
}

struct Change {
	vec3 unshadowed; // The static lights' unshadowed term (the dynamic lights' is in indirect_dyn_atlas.a).
	float change; // The whole lighting's change, for the GI gather's screen history.
	float change_static; // The static lights' change, for the bounce accumulation's restart.
	float geom;
	float vis;
	uint bounce_set; // The last bounce ray's hit set, 0xFFFFu for none.
	float bounce_t; // Its hit distance.
};

Change change_load(ivec2 texel) {
	uvec4 p = imageLoad(change_atlas, texel);
	Change c;
	c.unshadowed.rg = unpackHalf2x16(p.x);
	c.unshadowed.b = unpackHalf2x16(p.y & 0xFFFFu).x;
	c.change = float((p.y >> 16u) & 0xFFu) / 255.0;
	c.change_static = float((p.y >> 24u) & 0xFFu) / 255.0;
	vec2 gv = unpackHalf2x16(p.z);
	c.geom = gv.x;
	c.vis = gv.y;
	c.bounce_set = p.w >> 16u;
	c.bounce_t = unpackHalf2x16(p.w).x;
	return c;
}

void change_store(ivec2 texel, Change c) {
	uint y = (packHalf2x16(vec2(c.unshadowed.b, 0.0)) & 0xFFFFu) | (uint(clamp(c.change, 0.0, 1.0) * 255.0 + 0.5) << 16u) | (uint(clamp(c.change_static, 0.0, 1.0) * 255.0 + 0.5) << 24u);
	SPLAT_STORE(change_atlas, texel, uvec4(packHalf2x16(c.unshadowed.rg), y, packHalf2x16(vec2(c.geom, c.vis)), (packHalf2x16(vec2(c.bounce_t, 0.0)) & 0xFFFFu) | (c.bounce_set << 16u)));
}

// The static lights' change (the bounce accumulation's restart), and the
// whole lighting's (the GI gather's, which reads it the same way): the two
// bytes above the second uint's half.
float change_load_gradient(ivec2 texel) {
	return float((imageLoad(change_atlas, texel).y >> 24u) & 0xFFu) / 255.0;
}

float change_load_total(ivec2 texel) {
	return float((imageLoad(change_atlas, texel).y >> 16u) & 0xFFu) / 255.0;
}

// The world light grid: per cell, a count then grid_cap entries (a light
// index, bit 31 set for a spot). See surface_cache_grid.glsl.
layout(set = 0, binding = 22, std430) restrict readonly buffer LightGrid {
	uint data[];
}
light_grid;
#define GRID_SPOT_BIT 0x80000000u

uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
	return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

float luminance(vec3 c) {
	return dot(c, params.luma_weights.rgb);
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

#include "../area_light_diffuse_inc.glsl"

vec3 sky_eval(vec3 world_dir) {
	if (bool(params.flags & FLAG_SKY_MODE_SKY)) {
		vec4 q = params.sky_quat_or_color;
		vec3 t = cross(q.xyz, world_dir);
		vec3 dir = world_dir + ((t * q.w) + cross(q.xyz, t)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sampler2DArray(sky_radiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(dir, params.sky_border), 0.0), 3.0).rgb * params.sky_energy;
#else
		return textureLod(sampler2D(sky_radiance, linear_sampler_mipmaps), vec3_to_oct_with_border(dir, params.sky_border), 3.0).rgb * params.sky_energy;
#endif
	} else if (bool(params.flags & FLAG_SKY_MODE_COLOR)) {
		return params.sky_quat_or_color.rgb * params.sky_energy;
	}
	return vec3(0.0);
}

vec2 octahedron_wrap(vec2 v) {
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

vec2 octahedron_encode(vec3 n) {
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : octahedron_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

// The SDFGI lightprobe irradiance at a camera-relative position (the same
// evaluation the gather and the deferred resolve use). False outside every
// cascade.
bool sdfgi_probe_irradiance(vec3 rel_pos, vec3 normal, out vec3 r_irradiance) {
	r_irradiance = vec3(0.0);
	if (!bool(params.flags & FLAG_SDFGI)) {
		return false;
	}
	vec3 p = vec3(rel_pos.x, rel_pos.y * sdfgi.y_mult, rel_pos.z);
	vec3 n = normalize(vec3(normal.x, normal.y * sdfgi.y_mult, normal.z));

	for (uint c = 0u; c < sdfgi.max_cascades; c++) {
		vec3 cascade_pos = (p - sdfgi.cascades[c].position) * sdfgi.cascades[c].to_probe;
		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue;
		}
		cascade_pos += n * sdfgi.normal_bias;

		ivec3 probe_base_pos = ivec3(floor(cascade_pos));
		ivec3 tex_pos = ivec3(probe_base_pos.xy, int(c));
		tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
		tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);
		vec3 diffuse_posf = (vec3(tex_pos) + vec3(octahedron_encode(n) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

		vec4 accum = vec4(0.0);
		for (uint j = 0u; j < 8u; j++) {
			ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
			ivec3 probe_posi = probe_base_pos + offset;

			vec3 probe_pos = vec3(probe_posi);
			vec3 probe_to_pos = cascade_pos - probe_pos;
			vec3 probe_dir = normalize(-probe_to_pos);
			vec3 trilinear = vec3(1.0) - abs(probe_to_pos);
			float weight = trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(n, probe_dir));

			if (sdfgi.use_occlusion) {
				ivec3 occ_indexv = abs((sdfgi.cascades[c].probe_world_offset + probe_posi) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
				vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

				vec3 occ_pos = clamp(cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
				occ_pos.z += float(c);
				if (occ_indexv.z != 0) {
					occ_pos.x += 1.0;
				}
				occ_pos *= sdfgi.occlusion_renormalize;
				float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler_mipmaps), occ_pos, 0.0), occ_mask);
				weight *= max(occlusion, 0.01);
			}

			vec3 pos_uvw = diffuse_posf;
			pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
			pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
			accum += vec4(textureLod(sampler2DArray(lightprobe_texture, linear_sampler_mipmaps), pos_uvw, 0.0).rgb * weight, weight);
		}

		if (accum.a > 0.0) {
			accum.rgb /= accum.a;
		}
		r_irradiance = accum.rgb * sdfgi.cascades[c].exposure_normalization * sdfgi.energy;
		return true;
	}
	return false;
}

// A texel's diffuse albedo. The capture folded the material's F0 into the
// albedo as if Lambertian (the energy a card can hold); a planar mirror's
// texel takes that fold back out of the specular atlas, since the mirror's
// reflection is carried by the image lights and the reflected rays instead.
// Every reader of a texel's albedo comes through here: the dynamic
// estimator (the light rays' landings, the cosine rays' analytic first
// bounce, the gather's direct term at a hit) once multiplied the folded
// albedo where the static radiance used the diffuse one, so a lamp that
// changed once read its bounce off a mirror floor as diffuse for the hold,
// then darkened over the fade as the static accumulation took the bounce
// back (4% on the lab's pillar, section 57).
vec3 card_diffuse_albedo(ivec2 texel, vec3 world_pos) {
	vec3 albedo = texelFetch(albedo_atlas, texel, 0).rgb;
	if (mirror_on() && (params.mirror_debug & 4u) == 0u && mirror_at(world_pos) < MAX_MIRROR_PLANES) {
		albedo = max(albedo - texelFetch(specular_atlas, texel, 0).rgb, vec3(0.0));
	}
	return albedo;
}

// The gather's card lookup (stochastic_indirect_gi.glsl surface_cache_lookup),
// over this pass's own bindings: nearest texel of the lit atlas.
uint gradient_gap = 0u; // Diagnostics (gradset): frames since the tile's previous relight.

// Diagnostics (tier_stat): why the last card_lookup() failed. 0 the hit's
// instance is not in the cache, 1 it has no card set, 2 the set is not
// captured yet (or too small), 3 no card faces the ray with a filled texel
// under the hit, 4 a card does but its stored depth disagrees with the hit.
uint card_reject = 0u;

bool card_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance, out uint r_set, out float r_change, out float r_change_total, out vec3 r_n_world, out vec3 r_albedo, out ivec2 r_texel) {
	r_radiance = vec3(0.0);
	r_set = SURFACE_CACHE_INVALID;
	r_change = 0.0;
	r_change_total = 0.0;
	r_n_world = vec3(0.0);
	r_albedo = vec3(0.0);
	r_texel = ivec2(0);
	card_reject = 0u;
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	card_reject = 1u;
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	r_set = inst.set;
	CardSet s = sets.data[inst.set];
	card_reject = 2u;
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 8.0) {
		return false;
	}
	card_reject = 3u;
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	ivec2 best_texel = ivec2(0);
	uint best_k = 0u;
	// The best-facing card with a filled texel under the hit whatever its
	// stored depth says: the stand-in when every card fails the depth test.
	float alt_w = 0.0;
	ivec2 alt_texel = ivec2(0);
	uint alt_k = 0u;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = -dot(axis, local_dir);
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		if (facing > alt_w) {
			alt_w = facing;
			alt_texel = texel;
			alt_k = k;
		}
		// The box's longest extent over the card's longer edge, as when the
		// cards were square: a card's own (shorter) texel made the tolerance
		// reject grazing hits that then paid for the probe fallback.
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			card_reject = 4u;
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_texel = texel;
			best_k = k;
		}
	}
	if (best_w <= 0.0) {
		// Every facing card disagrees on depth: the hit is on another layer
		// of the same instance than the card captured (a back face reached
		// from inside a wall, a face behind the captured one, a corner
		// within a texel). A tenth of the bounce rays end here on the
		// closed box and the game room alike, and the SDFGI probe that
		// used to stand in read 130x darker than the card rays in the box
		// and 4.5x brighter in the game. The card's own texel -- the same
		// instance and material, lit by the same lights -- is the nearer
		// answer, and the one that stays when SDFGI is off.
		// GODOT_CARD_ABLATE=strict (debug bit 16384) keeps the rejection.
		if (alt_w <= 0.0 || (params.debug & 16384u) != 0u) {
			return false;
		}
		best_w = alt_w;
		best_texel = alt_texel;
		best_k = alt_k;
		card_reject = 5u;
	}
	r_radiance = imageLoad(lighting_atlas, best_texel).rgb;
	r_change = change_load_gradient(best_texel);
	r_change_total = change_load_total(best_texel);
	r_albedo = card_diffuse_albedo(best_texel, p_world_hit);
	r_texel = best_texel;
	// The captured normal, as read_texel decodes it: the dynamic bounce needs
	// the surface's orientation at the hit for its geometry terms.
	vec3 n_cam = normalize(texelFetch(normal_atlas, best_texel, 0).rgb * 2.0 - 1.0);
	vec3 axis, u, v;
	card_basis(best_k, axis, u, v);
	r_n_world = normalize(mat3(s.world_from_local) * (u * n_cam.x + v * n_cam.y + axis * n_cam.z));
	return true;
}

// Whether card_lookup() would call the hit a hole (card_reject 3: the set
// is captured and no card facing the ray has a filled texel under it), the
// test the bounce and light rays go on through; the trace kernel's, which
// reads no lighting.
bool card_hole(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir) {
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardSet s = sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 8.0) {
		return false;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		if (-dot(axis, local_dir) <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		if (texelFetch(depth_atlas, texel, 0).r > 0.0) {
			return false;
		}
	}
	return true;
}

vec3 basis_around(vec3 n, vec2 rnd) {
	vec3 t = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 b1 = normalize(cross(n, t));
	vec3 b2 = cross(n, b1);
	float phi = rnd.x * 2.0 * M_PI;
	float r = sqrt(rnd.y);
	return normalize(b1 * (r * cos(phi)) + b2 * (r * sin(phi)) + n * sqrt(max(1.0 - rnd.y, 0.0)));
}

// Coverage of a non-opaque candidate hit (an alpha-tested caster), from its
// instance's cards: the card facing the ray most squarely whose stored depth
// agrees with the hit, and its captured albedo alpha at that texel. A hit
// with no card, or with filled texels that all disagree on depth, counts as
// covered, so a thick object never leaks; a hit under which no facing card
// has a filled texel is a hole (the capture leaves an alpha-tested
// material's texels under half alpha unfilled) and lets the ray on.
bool card_covers(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir) {
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return true;
	}
	CardSet s = sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 8.0) {
		return true;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	float best_alpha = 1.0;
	bool any_filled = false;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = abs(dot(axis, local_dir)); // Either side: the ray may come from behind the leaf.
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		any_filled = true;
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		float tolerance = max(2.0 * texel_world, 0.02 * longest);
		if (abs(stored - depth) > tolerance) {
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_alpha = texelFetch(albedo_atlas, texel, 0).a;
		}
	}
	if (best_w <= 0.0) {
		return any_filled; // A covered layer, or a hole.
	}
	return best_alpha >= 0.5;
}

#ifndef SC_NO_RAYS
bool occluded(vec3 origin, vec3 dir, float t_max, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			if (card_covers(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false), origin + dir * rayQueryGetIntersectionTEXT(rq, false), dir)) {
				rayQueryConfirmIntersectionEXT(rq);
			}
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Occlusion as the bounce rays see it: everything opaque, the first hit ends
// the query (the dynamic bounce's connection, see trace_dynamic).
bool occluded_opaque(vec3 origin, vec3 dir, float t_max) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, origin, 0.0, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}
#endif

// A texel's shadow ray, at its bit of the split's header: traced here in
// the single kernel, requested by the setup kernel (which reports it
// clear), read back by the resolve (clear where it was not requested).
bool shadow_ray(uint bit, vec3 origin, vec3 dir, float t_max, uint mask) {
#if defined(SC_SETUP)
	split_shadow_mask |= 1u << bit;
	return false;
#elif defined(SC_RESOLVE)
	return (split_shadow_mask & split_shadow_hits & (1u << bit)) != 0u;
#else
	return occluded(origin, dir, t_max, mask);
#endif
}

// The local lights that reach a point: its cell of the world light grid
// (every light that reaches it), or the set's list (the first 32
// overlapping its box) where the grid is off or the point lies outside it.
void light_list(uint set, vec3 world_pos, out uint r_base, out uint r_count, out bool r_from_grid) {
	r_base = set * (1u + MAX_LIGHTS_PER_SET);
	r_count = min(set_lights.data[r_base], MAX_LIGHTS_PER_SET);
	r_from_grid = false;
	if (bool(params.flags & FLAG_GRID)) {
		vec3 rel = (world_pos - params.grid_origin) / params.grid_cell;
		if (all(greaterThanEqual(rel, vec3(0.0))) && all(lessThan(rel, vec3(float(params.grid_n))))) {
			uvec3 c = uvec3(rel);
			r_base = (c.x + params.grid_n * (c.y + params.grid_n * c.z)) * (1u + params.grid_cap);
			r_count = min(light_grid.data[r_base], params.grid_cap);
			r_from_grid = true;
		}
	}
	if ((params.debug & 4u) != 0u) {
		r_count = 0u;
	}
}

LightData light_at(uint base, uint j, bool from_grid, out bool r_is_spot) {
	uint idx = from_grid ? light_grid.data[base + 1u + j] : set_lights.data[base + 1u + j];
	r_is_spot = from_grid ? (idx & GRID_SPOT_BIT) != 0u : idx >= params.omni_light_count;
	if (from_grid) {
		idx &= ~GRID_SPOT_BIT;
	} else if (r_is_spot) {
		idx -= params.omni_light_count;
	}
	return r_is_spot ? spot_lights.data[idx] : omni_lights.data[idx];
}

// A local light's unshadowed contribution at a point: its colour times the
// cosine, the attenuation and the cone, over pi (the direct pass's terms).
// r_pos is the light's world position, r_geom the geometric factor
// (attenuation times cosine) the geometric gradient sums.
// A light's projector texture at a world point (the direct pass's mapping:
// a spot's cookie through a perspective projection, an omni's map through
// the dual paraboloid), read through the cards' own world-space projector
// matrix (light_storage.cpp _fill_card_light_data). One where the light has
// none.
vec3 projector_factor(LightData ld, bool is_spot, vec3 world_pos) {
	if (ld.projector_rect == vec4(0.0)) {
		return vec3(1.0);
	}
	vec4 proj;
	if (is_spot) {
		vec4 splane = ld.shadow_matrix * vec4(world_pos, 1.0);
		splane /= splane.w;
		vec2 proj_uv = splane.xy * ld.projector_rect.zw;
		proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), proj_uv + ld.projector_rect.xy, 0.0);
	} else {
		vec3 local_v = normalize((ld.shadow_matrix * vec4(world_pos, 1.0)).xyz);
		vec4 atlas_rect = ld.projector_rect;
		if (local_v.z >= 0.0) {
			atlas_rect.y += atlas_rect.w;
		}
		local_v.z = 1.0 + abs(local_v.z);
		local_v.xy /= local_v.z;
		local_v.xy = local_v.xy * 0.5 + 0.5;
		vec2 proj_uv = local_v.xy * atlas_rect.zw;
		proj = textureLod(sampler2D(decal_atlas_srgb, linear_sampler_mipmaps), proj_uv + atlas_rect.xy, 0.0);
	}
	return proj.rgb * proj.a;
}

vec3 light_contribution_world(LightData ld, bool is_spot, vec3 light_pos, vec3 spot_dir, vec3 world_pos, vec3 n, out float r_geom) {
	vec3 rel = light_pos - world_pos;
	float len = length(rel);
	float attenuation = get_omni_attenuation(len, ld.inv_radius, ld.attenuation);
	vec3 l = rel / max(len, 1e-5);
	if (is_spot) {
		float scos = max(dot(-l, spot_dir), ld.cone_angle);
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
		attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
	}
	float ndotl = max(dot(n, l), 0.0);
	r_geom = ndotl * attenuation;
	if (r_geom <= 0.0) {
		return vec3(0.0);
	}
	return ld.color * projector_factor(ld, is_spot, world_pos) * (r_geom * (1.0 / M_PI));
}

// The scene's light buffers hold view-space positions and directions.
vec3 light_contribution(LightData ld, bool is_spot, vec3 world_pos, vec3 n, out vec3 r_pos, out float r_geom) {
	r_pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
	vec3 spot_dir = is_spot ? normalize(mat3(params.world_from_view) * ld.direction) : vec3(0.0, -1.0, 0.0);
	return light_contribution_world(ld, is_spot, r_pos, spot_dir, world_pos, n, r_geom);
}

// The dynamic lights' unshadowed direct term at a point, each by its weight.
vec3 dynamic_direct(vec3 world_pos, vec3 n) {
	vec3 sum = vec3(0.0);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		float geom;
		sum += light_contribution_world(ld, ld.pad > 0.5, ld.position, normalize(ld.direction), world_pos, n, geom) * dyn_lights.weights[i >> 2u][i & 3u];
	}
	return sum;
}

struct Texel {
	vec3 world_pos;
	vec3 n_world;
	vec3 origin;
	vec3 albedo;
	vec3 emission;
};

// The texel's position, normal and material back out of the capture. False
// where nothing was captured.
bool read_texel(CardSet s, uint card, ivec2 dims, ivec2 texel_in_card, ivec2 texel, out Texel t) {
	float depth = texelFetch(depth_atlas, texel, 0).r;
	if (depth <= 0.0) {
		return false;
	}
	vec3 n_cam = normalize(texelFetch(normal_atlas, texel, 0).rgb * 2.0 - 1.0);
	t.emission = texelFetch(emission_atlas, texel, 0).rgb;
	vec2 uv01 = (vec2(texel_in_card) + 0.5) / vec2(dims);
	vec3 local_pos = card_local_point(s, card, uv01, depth);
	vec3 axis, u, v;
	card_basis(card, axis, u, v);
	vec3 n_local = u * n_cam.x + v * n_cam.y + axis * n_cam.z;
	t.world_pos = (s.world_from_local * vec4(local_pos, 1.0)).xyz;
	t.albedo = card_diffuse_albedo(texel, t.world_pos);
	t.n_world = normalize(mat3(s.world_from_local) * n_local);
	t.origin = t.world_pos + t.n_world * params.ray_bias;
	return true;
}

// Direct light at the texel: the sun, and the local lights (the world
// light grid's cell with FLAG_GRID, else the list culled to the set's box).
// The direct term in its parts: the directional lights with their one hard
// shadow ray each (deterministic: the same ray every relight, so exact, no
// accumulation), the local lights' unshadowed analytic sum (exact) with its
// geometric sum, and one visibility sample of a light drawn from them (the
// only stochastic part, accumulated as a ratio by accumulate()). The whole
// unshadowed term is returned for the radiance gradient.
struct Direct {
	vec3 exact; // The shadowed directional term.
	vec3 unshadowed; // Every light, unshadowed.
	vec3 local_sum; // The local lights, unshadowed.
	vec3 dyn_sum; // The dynamic lights among them, unshadowed (a part of local_sum).
	float local_geom; // Their geometric sum.
	float vis; // The drawn light's visibility, when one was drawn.
	bool sampled;
	uint local_count; // The lights the texel's list held, and the nearest one's distance over its range (diagnostics, paintl).
	float local_nearest;
};

// The image lights' part of a texel's direct term (the planar mirrors,
// mirror_planes_inc.glsl): every local and area light's image through
// each chain the texel faces, summed, with one image drawn among them by
// weight for the shadow legs. Computed at a quad's first texel and shared
// by its other three (main's quad mode): the images are smooth over a
// quad and cost as much as the real lights again per chain.
struct ImageCache {
	bool valid;
	vec3 sum; // The images, unshadowed.
	float geom; // Their geometric sum.
	float weight_sum; // Their draw weights' sum.
	bool has_sel; // An image was drawn among them.
	uint sel_chain;
	vec3 sel_pos; // The drawn image's position (the shadow legs' target).
	float sel_opacity;
	uint sel_mask;
};

void shade_images(uint set, Texel t, inout uint seed, out ImageCache c) {
	c.valid = true;
	c.sum = vec3(0.0);
	c.geom = 0.0;
	c.weight_sum = 0.0;
	c.has_sel = false;
	c.sel_chain = 0u;
	c.sel_pos = vec3(0.0);
	c.sel_opacity = 0.0;
	c.sel_mask = 0u;
	// The image chains this texel faces (mirror_chains_at: each mirror on
	// whose reflective side it lies, alone and in the pairs and triples
	// worth it); see mirror_chain.
	uint chain[MIRROR_CHAINS_MAX];
	uint chains = mirror_on() ? mirror_chains_at(t.world_pos, chain) : 0u;
	if (chains == 0u) {
		return;
	}
	uint base;
	uint light_count;
	bool from_grid;
	light_list(set, t.world_pos, base, light_count, from_grid);
	for (uint j = 0u; j < light_count; j++) {
		bool is_spot;
		LightData ld = light_at(base, j, from_grid, is_spot);
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		for (uint ch = 0u; ch < chains; ch++) {
			// The light's image through each chain the texel faces
			// (evaluated whether or not the light itself reaches the texel:
			// a ceiling sees its own lights only in the floor).
			vec3 img, p_img, n_img, c1, c2, c3;
			float f;
			if (!mirror_chain(MIRROR_CHAIN_A(chain[ch]), MIRROR_CHAIN_B(chain[ch]), MIRROR_CHAIN_C(chain[ch]), t.world_pos, t.n_world, pos, ld.inv_radius, img, p_img, n_img, f, c1, c2, c3)) {
				continue;
			}
			vec3 pos_img;
			float geom_img;
			vec3 ci = light_contribution(ld, is_spot, p_img, n_img, pos_img, geom_img) * f;
			float wi = luminance(abs(ci));
			if (wi > 0.0) {
				c.sum += ci;
				c.geom += geom_img;
				c.weight_sum += wi;
				seed = pcg_hash(seed);
				if (hash_to_float(seed) * c.weight_sum < wi) {
					c.has_sel = true;
					c.sel_chain = chain[ch];
					c.sel_pos = img;
					c.sel_opacity = ld.shadow_opacity;
					c.sel_mask = ld.shadow_caster_mask & 0xFFu;
				}
			}
		}
	}
	uint area_count = (params.debug & 4u) != 0u || (params.mirror_debug & 8u) != 0u ? 0u : params.area_light_count;
	for (uint j = 0u; j < area_count; j++) {
		LightData ld = area_lights.data[j];
		seed = pcg_hash(seed);
		float xi0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float xi1 = hash_to_float(seed);
		for (uint ch = 0u; ch < chains; ch++) {
			// The area light's image through each chain the texel faces:
			// the rect stays, the texel mirrors. The chain's weight (the
			// Fresnel and the rectangle's coverage) is taken through the
			// rect's center, and the drawn point on the rect names only the
			// shadow legs' target. Taking the weight through the drawn
			// point too (mirror_debug bit 16, the first form) made the
			// unshadowed sum a random variable on every texel an area
			// light's image reached -- its coverage came and went with the
			// point, and a broken chain dropped the whole term -- which the
			// radiance gradient below, built on that sum being exact,
			// read as the light changing at every relight: the bounce
			// accumulation and the screen's GI history restarted at random
			// over the whole room (pose E0 at rest, frame-to-frame
			// difference 0.00071 against 0.00046 without the images; the
			// centre's weight 0.00047). The cost is the coverage at a
			// mirror's edge, all-or-nothing where the rect's reflection
			// straddles it (the lobe's footprint softens it, not the
			// rect's extent), the same approximation as evaluating the LTC
			// term at the mirrored texel alone.
			vec3 img, p_img, n_img, c1, c2, c3;
			float f;
			if (!mirror_chain(MIRROR_CHAIN_A(chain[ch]), MIRROR_CHAIN_B(chain[ch]), MIRROR_CHAIN_C(chain[ch]), t.world_pos, t.n_world, (params.world_from_view * vec4(ld.position, 1.0)).xyz, ld.inv_radius, img, p_img, n_img, f, c1, c2, c3)) {
				continue;
			}
			float geom_img;
			vec3 point_img;
			vec3 ci = area_light_contribution(ld, params.world_from_view, p_img, n_img, vec2(xi0, xi1), area_light_atlas, linear_sampler_mipmaps, geom_img, point_img);
			vec3 img_pt, p_pt, n_pt, c1_pt, c2_pt, c3_pt;
			float f_pt;
			if (mirror_chain(MIRROR_CHAIN_A(chain[ch]), MIRROR_CHAIN_B(chain[ch]), MIRROR_CHAIN_C(chain[ch]), t.world_pos, t.n_world, point_img, 0.0, img_pt, p_pt, n_pt, f_pt, c1_pt, c2_pt, c3_pt)) {
				img = img_pt;
				if ((params.mirror_debug & 16u) != 0u) {
					f = f_pt;
				}
			} else if ((params.mirror_debug & 16u) != 0u) {
				continue;
			}
			ci *= f;
			float wi = luminance(abs(ci));
			if (wi > 0.0) {
				c.sum += ci;
				c.geom += geom_img;
				c.weight_sum += wi;
				seed = pcg_hash(seed);
				if (hash_to_float(seed) * c.weight_sum < wi) {
					c.has_sel = true;
					c.sel_chain = chain[ch];
					c.sel_pos = img;
					c.sel_opacity = ld.shadow_opacity;
					c.sel_mask = ld.shadow_caster_mask & 0xFFu;
				}
			}
		}
	}
}

// k: the texel's place in its quad (0 outside the quad layout), which names
// its shadow rays for the split.
void shade_direct(uint set, Texel t, uint k, inout uint seed, inout ImageCache images, out Direct d) {
	vec3 direct = vec3(0.0);
	vec3 direct_unshadowed = vec3(0.0);
	d.local_geom = 0.0;
	d.vis = 1.0;
	d.sampled = false;

	// Directional lights: a shadow ray each. (Folding the sun into the
	// estimator below was measured to save nothing: its ray is cheap.)
	uint directional_count = (params.debug & 8u) != 0u ? 0u : params.directional_light_count;
	for (uint i = 0u; i < directional_count; i++) {
		DirectionalLightData dl = directional_lights.data[i];
		vec3 l = normalize(mat3(params.world_from_view) * dl.direction);
		float ndotl = dot(t.n_world, l);
		if (ndotl <= 0.0) {
			continue;
		}
		vec3 c = dl.color * dl.energy * (ndotl * (1.0 / M_PI));
		direct_unshadowed += c;
		if (dl.shadow_opacity <= 0.001 || (params.debug & 2u) != 0u) {
			direct += c;
			continue;
		}
		float vis = mix(1.0, shadow_ray(k * (params.directional_light_count + 1u) + i, t.origin, l, 1e4, 0xFFu) ? 0.0 : 1.0, dl.shadow_opacity);
		direct += c * vis;
	}
	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	vec3 sel_pos = vec3(0.0);
	float sel_opacity = 0.0;
	uint sel_mask = 0u;
	bool selected = false;
	bool sel_image = false;
	uint sel_chain = 0u;

	uint base;
	uint light_count;
	bool from_grid;
	light_list(set, t.world_pos, base, light_count, from_grid);
	float nearest = 1.0;
	for (uint j = 0u; j < light_count; j++) {
		bool is_spot;
		LightData ld = light_at(base, j, from_grid, is_spot);
		vec3 pos;
		float geom;
		vec3 c = light_contribution(ld, is_spot, t.world_pos, t.n_world, pos, geom);
		if ((params.debug & 524288u) != 0u) {
			nearest = min(nearest, length(pos - t.world_pos) * ld.inv_radius);
		}
		float w = luminance(abs(c));
		if (w <= 0.0) {
			continue;
		}
		sum += c;
		d.local_geom += geom;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = pos;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	// Area lights, in the same estimator: their LTC diffuse term, with the
	// shadow ray to a point drawn uniformly on the rect (a soft shadow over
	// the relights). Not culled: a scene holds a few, and the range test in
	// the term is the first thing it does.
	uint area_count = (params.debug & 4u) != 0u ? 0u : params.area_light_count;
	for (uint j = 0u; j < area_count; j++) {
		LightData ld = area_lights.data[j];
		seed = pcg_hash(seed);
		float xi0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float xi1 = hash_to_float(seed);
		float geom;
		vec3 point;
		vec3 c = area_light_contribution(ld, params.world_from_view, t.world_pos, t.n_world, vec2(xi0, xi1), area_light_atlas, linear_sampler_mipmaps, geom, point);
		float w = luminance(abs(c));
		// Written as the negation so a NaN term is rejected too (every
		// comparison with a NaN is false): the accumulation below keeps
		// whatever enters it for the texel's lifetime, and a single NaN
		// texel poisons every ray that reads it, then the gather's history
		// and its spatial filter, which spreads it a stride further every
		// frame (growing black voids over the whole frame).
		if (!(w > 0.0)) {
			if ((params.debug & 8192u) != 0u && (isnan(w) || isinf(w))) {
				atomicAdd(dyn_stats.count[31], 1u); // Diagnostics (GODOT_GI_TIER_PRINT): the area terms rejected as NaN.
			}
			continue;
		}
		sum += c;
		d.local_geom += geom;
		weight_sum += w;
		seed = pcg_hash(seed);
		if (hash_to_float(seed) * weight_sum < w) {
			selected = true;
			sel_pos = point;
			sel_opacity = ld.shadow_opacity;
			sel_mask = ld.shadow_caster_mask & 0xFFu;
		}
	}
	// The planar mirrors: the lights' images join the sum and the draw (the
	// images' own reservoir merged with the real lights' by weight; a drawn
	// image is shadowed in legs, to each mirror then to the light).
	if (mirror_on() && (params.mirror_debug & 1u) == 0u) {
		if (!images.valid) {
			shade_images(set, t, seed, images);
		}
		if (images.weight_sum > 0.0) {
			sum += images.sum;
			d.local_geom += images.geom;
			weight_sum += images.weight_sum;
			seed = pcg_hash(seed);
			if (images.has_sel && hash_to_float(seed) * weight_sum < images.weight_sum) {
				selected = true;
				sel_image = true;
				sel_chain = images.sel_chain;
				sel_pos = images.sel_pos;
				sel_opacity = images.sel_opacity;
				sel_mask = images.sel_mask;
			}
		}
	}
	direct_unshadowed += sum;
	if (selected) {
		float vis = 1.0;
		if (sel_opacity > 0.001 && sel_mask != 0u && (params.debug & 2u) == 0u) {
			vec3 to_light = sel_pos - t.origin;
			float dist = length(to_light);
			bool blocked;
			if (sel_image) {
#ifdef SC_NO_RAYS
				blocked = false; // No mirrors in a split frame (see the header).
#else
				// Legs: to a centimeter above each mirror of the chain (along
				// its normal), the target mirrored back a step each time,
				// then from the last mirror to the real light.
				uint mchain[3];
				mchain[0] = MIRROR_CHAIN_A(sel_chain);
				mchain[1] = MIRROR_CHAIN_B(sel_chain);
				mchain[2] = MIRROR_CHAIN_C(sel_chain);
				vec3 from = t.origin;
				vec3 target = sel_pos;
				blocked = false;
				for (uint s = 0u; s < 3u && !blocked; s++) {
					uint m = mchain[s];
					if (m >= MAX_MIRROR_PLANES) {
						break;
					}
					vec3 n = params.mirrors[m].plane.xyz;
					float hp = mirror_height(m, from);
					vec3 dir = normalize(target - from);
					float cos_p = max(-dot(n, dir), 1e-3);
					blocked = hp <= 0.0 || occluded(from, dir, max(hp - 0.01, 0.0) / cos_p, sel_mask);
					from = from + dir * (hp / cos_p) + n * 0.01;
					target = mirror_point(m, target);
				}
				vec3 to_real = target - from;
				float dr = length(to_real);
				blocked = blocked || occluded(from, to_real / max(dr, 1e-5), max(dr - 0.01, 0.0), sel_mask);
#endif
			} else {
#ifdef SC_SETUP
				// The trace kernel's target: the ray is rebuilt from it and the
				// texel's origin, as here.
				split_texels.data[split_thread * 8u + k * 2u + 1u] = uvec4(floatBitsToUint(sel_pos), sel_mask);
#endif
				blocked = shadow_ray(k * (params.directional_light_count + 1u) + params.directional_light_count, t.origin, to_light / max(dist, 1e-5), max(dist - params.ray_bias, 0.0), sel_mask);
			}
			vis = mix(1.0, blocked ? 0.0 : 1.0, sel_opacity);
		}
		d.vis = vis;
		d.sampled = true;
	}
	d.exact = direct;
	d.unshadowed = direct_unshadowed;
	d.local_sum = sum;
	d.dyn_sum = dynamic_direct(t.world_pos, t.n_world);
	d.local_count = light_count;
	d.local_nearest = nearest;
}

// The dynamic lights' bounce. A flashlight's spot on the floor is small
// and bright, and its bounce on the ceiling is what a cosine ray per texel
// cannot estimate: one texel in a few lands a ray on the spot per relight,
// and the field over the ceiling is a mottle of texels that saw it and
// texels that did not -- coherent across every mip the hits read it
// through, so no screen-space filter averages it -- that the window
// (twice temporal_frames, 32 relights by default) averages out, which a
// light that keeps moving never gives them,
// and that every move restarts (the change of a lit texel is carried by
// the rays that hit it and restarts the readers to one sample, at random
// relights over the eight the mark lasts; the static lamps' converged
// bounce goes with it). So a light that moves or changes is estimated
// apart, with a history of its own that follows the light's motion (see
// accumulate) and needs no restart. Two strategies estimate its first
// bounce, combined by the balance heuristic: rays from the light, uniform
// over its cone (or the sphere), landing on the lit surface and connected
// to the texel by a shadow ray (every texel sees the spot every relight,
// the density known exactly); and the static cosine rays, whose hit
// carries the light's direct term analytically (see trace_bounce) -- the
// better strategy for a lamp lighting a whole wall, where a light ray is
// one sample of a wide area. The lighting atlas holds no dynamic light's
// direct term (see accumulate), so nothing is subtracted against a stale
// state; the cosine rays read the static lights' radiance plus the
// dynamic lights' bounces, and hand the first of those to the dynamic
// term's second-bounce history, so it too follows the light closely; the
// third and after stay with the static accumulation. A light is dynamic
// while LightStorage has seen it change lately (GODOT_CARD_DYN_HOLD
// frames), then its weight fades (GODOT_CARD_DYN_FADE) and the static
// accumulation absorbs its bounce as slowly as the weight leaves.
//
// Directions come from the R2 sequence per texel, advanced per relight: the
// samples of one texel's window tile the cone rather than clump. The
// squared distance of the connection is softened by ten centimeters
// (d*d + 0.01).
void dyn_stat(uint i, bool ceiling) {
	if ((params.debug & 4096u) != 0u) {
		atomicAdd(dyn_stats.count[i], 1u);
		if (ceiling) {
			atomicAdd(dyn_stats.count[i + 8u], 1u);
		}
	}
}

// The resolve's answer to a split ray: its record where the setup kernel
// requested it, nothing (a miss) otherwise.
#ifdef SC_RESOLVE
uvec4 split_record(uint r) {
	return (split_ray_mask & (1u << r)) != 0u ? split_records.data[split_thread * params.split.w + r] : uvec4(0u);
}
#endif
#ifdef SC_SETUP
// The setup kernel's request of ray record r: its direction and the texel
// of the quad it leaves from (or connects to).
void split_request(uint r, vec3 dir, uint k) {
	split_records.data[split_thread * params.split.w + r] = uvec4(floatBitsToUint(dir), k);
	split_ray_mask |= 1u << r;
}
// A shared dynamic landing's request (trace_dynamic): its index, not a direction.
void split_request_landing(uint r, uint landing, uint k) {
	split_records.data[split_thread * params.split.w + r] = uvec4(landing, 0u, 0u, k);
	split_ray_mask |= 1u << r;
}
#endif

// A light ray's direction: uniform over the spot's cone (or the sphere),
// or by the cookie's table.
vec3 dyn_direction(uint i, LightData ld, bool is_spot, vec3 axis, float r0, float r1, out float r_pdf_omega) {
	float cone_cos = is_spot ? clamp(ld.cone_angle, -1.0, 0.9999) : -1.0;
	r_pdf_omega = 1.0 / (2.0 * M_PI * (1.0 - cone_cos));
	if (is_spot && cookie_table(i)) {
		return cookie_sample(i, ld, axis, r0, r1, r_pdf_omega);
	}
	vec3 tng = abs(axis.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 b1 = normalize(cross(axis, tng));
	vec3 b2 = cross(axis, b1);
	float phi = r1 * 2.0 * M_PI;
	float ct = 1.0 - r0 * (1.0 - cone_cos);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	return normalize(b1 * (st * cos(phi)) + b2 * (st * sin(phi)) + axis * ct);
}

// The landing a texel's sample sidx of light i draws from the shared pool:
// uniform, with replacement, so a draw is distributed exactly as a light
// ray of the texel's own would be (the same density pdf_area at its
// landing, the same balance-heuristic weight against the cosine rays),
// without the light ray, its layers through the holes and the card lookup
// at the landing, which the pool paid once for every texel. Setup, trace
// and resolve draw the same index.
uint dyn_landing_index(ivec2 texel, uint i, uint sidx) {
	uint m = dyn_lights.pad1;
	uint h = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) ^ 0x2545F491u) + params.frame * 0x9E3779B9u + (i * 8u + sidx) * 0x85EBCA6Bu);
	return i * m + h % m;
}

void trace_dynamic_pool(ivec2 texel, Texel t, uint k_tracer, float n_cosine, float n_light, inout vec3 dyn_sample, inout float landed, inout float change_total) {
	bool ceiling = t.n_world.y < -0.7;
	uint n_rays = uint(n_light);
	float share = (1.0 / float(n_rays)) / float(dyn_lights.count);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		if (ld.inv_radius <= 0.0 || dyn_lights.weights[i >> 2u][i & 3u] <= 0.0) {
			continue;
		}
		for (uint sidx = 0u; sidx < n_rays; sidx++) {
			uint j = dyn_landing_index(texel, i, sidx);
			uvec4 d0 = dyn_landings.data[j * 2u];
			float pdf_area = uintBitsToFloat(d0.w);
			if (pdf_area <= 0.0) {
				continue;
			}
			uvec4 d1 = dyn_landings.data[j * 2u + 1u];
			vec3 p = uintBitsToFloat(d0.xyz);
			vec3 n_p = oct_to_vec3(unpackUnorm2x16(d1.z) * 2.0 - 1.0);
			vec2 rg = unpackHalf2x16(d1.x);
			vec2 b_change = unpackHalf2x16(d1.y);
			vec3 l_dyn = vec3(rg, b_change.x);
			vec3 rel = p - t.origin;
			float d = length(rel);
			vec3 l = rel / max(d, 1e-4);
			float cos_t = dot(t.n_world, l);
			float cos_pt = dot(n_p, -l);
#ifdef SC_SETUP
			// Only a pair that faces each other asks for its connection: three
			// of four draws do not (the trace kernel's threads, not its rays,
			// were the cost), and the resolve takes an unasked one as blocked.
			if (d >= 1e-4 && cos_t > 0.0 && cos_pt > 0.0) {
				split_request_landing(SPLIT_DYN_SLOT(i, sidx), j, k_tracer);
			}
#else
			dyn_stat(4u, ceiling && cos_t > 0.0);
			dyn_stat(5u, ceiling && cos_t > 0.0 && cos_pt > 0.0);
#ifdef SC_RESOLVE
			bool connected = (split_record(SPLIT_DYN_SLOT(i, sidx)).x & SPLIT_CONNECTED) != 0u;
#else
			bool connected = d >= 1e-4 && cos_t > 0.0 && cos_pt > 0.0 && !occluded_opaque(t.origin, l, max(d - params.ray_bias, 0.0));
#endif
			if (d >= 1e-4 && cos_t > 0.0 && cos_pt > 0.0 && connected) {
				dyn_stat(6u, ceiling);
				float geom = cos_t * cos_pt / (d * d + 0.01);
				dyn_sample += l_dyn * (geom / (M_PI * n_light * pdf_area + n_cosine * geom));
				landed += share;
				change_total = max(change_total, b_change.y - 0.25); // See trace_dynamic.
			}
#endif
		}
	}
}

void trace_dynamic(ivec2 texel, Texel t, uint k_tracer, float n_cosine, float n_light, out vec3 dyn_sample, out float landed, out float change_total) {
	dyn_sample = vec3(0.0);
	landed = 0.0;
	change_total = 0.0;
	bool ceiling = t.n_world.y < -0.7;
	uint n_rays = uint(n_light);
	if (dyn_lights.count == 0u || n_rays == 0u || (params.debug & 1u) != 0u) {
		return;
	}
	if (dyn_lights.pad1 != 0u) {
		trace_dynamic_pool(texel, t, k_tracer, n_cosine, n_light, dyn_sample, landed, change_total);
		return;
	}
	uint h = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) ^ 0x2545F491u));
	float o0 = hash_to_float(h);
	float o1 = hash_to_float(pcg_hash(h));
	float inv_rays = 1.0 / float(n_rays);
	float share = inv_rays / float(dyn_lights.count);
	for (uint i = 0u; i < dyn_lights.count; i++) {
		LightData ld = dyn_lights.data[i];
		bool is_spot = ld.pad > 0.5;
		float weight = dyn_lights.weights[i >> 2u][i & 3u];
		if (ld.inv_radius <= 0.0 || weight <= 0.0) {
			continue;
		}
		vec3 pos = ld.position;
		float range = 1.0 / ld.inv_radius;
		vec3 axis = is_spot ? normalize(ld.direction) : vec3(0.0, 0.0, 1.0);
		float cone_cos = is_spot ? clamp(ld.cone_angle, -1.0, 0.9999) : -1.0;
		float pdf_dir = 1.0 / (2.0 * M_PI * (1.0 - cone_cos));
		vec3 tng = abs(axis.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
		vec3 b1 = normalize(cross(axis, tng));
		vec3 b2 = cross(axis, b1);
		bool cookie = is_spot && cookie_table(i);
		for (uint sidx = 0u; sidx < n_rays; sidx++) {
			float k = float(((params.frame & 63u) * n_rays + sidx) * (i + 1u));
			float r0 = fract(o0 + k * 0.7548776662);
			float r1 = fract(o1 + k * 0.5698402910);
			vec3 dir;
			float pdf_omega = pdf_dir;
			if (cookie) {
				dir = cookie_sample(i, ld, axis, r0, r1, pdf_omega);
				if (pdf_omega <= 0.0) {
					continue;
				}
			} else {
				float phi = r1 * 2.0 * M_PI;
				float ct = 1.0 - r0 * (1.0 - cone_cos);
				float st = sqrt(max(1.0 - ct * ct, 0.0));
				dir = normalize(b1 * (st * cos(phi)) + b2 * (st * sin(phi)) + axis * ct);
			}
#ifdef SC_SETUP
			split_request(SPLIT_DYN_SLOT(i, sidx), dir, k_tracer);
			continue;
#endif
			dyn_stat(0u, ceiling);
			// As the bounce ray: a hit under which no facing card has a
			// filled texel is a hole, and the ray goes on through it.
			float d_lp = 0.0;
			vec3 p = pos;
			vec3 unused_radiance;
			uint hit_set = SURFACE_CACHE_INVALID;
			float unused_change;
			float hit_change_total = 0.0;
			vec3 n_p = axis;
			vec3 albedo_p = vec3(0.0);
			ivec2 texel_p = ivec2(0);
			bool blocked = false;
			bool landed_on_card = false;
#ifdef SC_RESOLVE
			uvec4 rec = split_record(SPLIT_DYN_SLOT(i, sidx));
			if ((rec.x & SPLIT_HIT) != 0u) {
				d_lp = uintBitsToFloat(rec.y);
				p = pos + dir * d_lp;
				blocked = true;
				landed_on_card = card_lookup(rec.z, p, dir, unused_radiance, hit_set, unused_change, hit_change_total, n_p, albedo_p, texel_p);
			}
#define DYN_CONNECTED ((rec.x & SPLIT_CONNECTED) != 0u)
#elif defined(SC_SETUP)
#define DYN_CONNECTED false
#else
#define DYN_CONNECTED !occluded_opaque(t.origin, l, max(d - params.ray_bias, 0.0))
			rayQueryEXT rq;
			vec3 ray_origin = pos;
			float d_base = 0.0;
			for (uint layer = 0u; layer < 4u; layer++) {
				rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, ray_origin, 0.0, dir, range - d_base);
				while (rayQueryProceedEXT(rq)) {
				}
				if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
					break;
				}
				d_lp = d_base + rayQueryGetIntersectionTEXT(rq, true);
				p = pos + dir * d_lp;
				blocked = true;
				landed_on_card = card_lookup(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true), p, dir, unused_radiance, hit_set, unused_change, hit_change_total, n_p, albedo_p, texel_p);
				if (landed_on_card || card_reject != 3u) {
					break;
				}
				blocked = false;
				landed_on_card = false;
				d_base = d_lp + params.ray_bias;
				ray_origin = pos + dir * d_base;
			}
#endif
			if (!blocked) {
				continue;
			}
			dyn_stat(1u, ceiling);
			if (!landed_on_card) {
				continue;
			}
			dyn_stat(2u, ceiling);
			float cos_pl = dot(n_p, -dir);
			if (cos_pl <= 0.0 || d_lp < 1e-4) {
				continue;
			}
			dyn_stat(3u, ceiling);
			float unused_geom;
			vec3 c_p = light_contribution_world(ld, is_spot, pos, axis, p, n_p, unused_geom);
			// The landing's radiance from this light's direct term alone.
			vec3 l_dyn = albedo_p * c_p * weight;
			float pdf_area = pdf_omega * cos_pl / (d_lp * d_lp);
			// The texel connected to the landing. The estimator of the
			// irradiance over pi is radiance * geom / (pi * pdf_area); its
			// balance-heuristic weight against the cosine rays (n_cosine of
			// them, density geom / pi per unit area at the landing) folds in
			// as radiance * geom / (pi * rays * pdf_area + n_cosine * geom),
			// which a landing beside the texel cannot blow up.
			{
				vec3 rel = p - t.origin;
				float d = length(rel);
				vec3 l = rel / max(d, 1e-4);
				float cos_t = dot(t.n_world, l);
				float cos_pt = dot(n_p, -l);
				dyn_stat(4u, ceiling && cos_t > 0.0);
				dyn_stat(5u, ceiling && cos_t > 0.0 && cos_pt > 0.0);
				if (d >= 1e-4 && cos_t > 0.0 && cos_pt > 0.0 && DYN_CONNECTED) {
					dyn_stat(6u, ceiling);
					float geom = cos_t * cos_pt / (d * d + 0.01);
					dyn_sample += l_dyn * (geom / (M_PI * n_light * pdf_area + n_cosine * geom));
					landed += share;
					// The landing's change reaches this texel only through a
					// connection, like the cosine ray's hit (accumulate weighs
					// it by the dynamic sample's share). Carried from every
					// landing whether or not it connected, two spotlights of
					// the TPS demo's forklifts (50 m range, flying) handed
					// the cone edge's full marks to every texel within their
					// range each frame, and through the cosine hits to the
					// rest of the level: the GI screen history restarted on
					// 98% of the pixels every frame the level was played.
					change_total = max(change_total, hit_change_total - 0.25);
				}
			}
		}
	}
}

// The indirect term of the static lights: one cosine ray into the scene
// (see main), the card's radiance at its hit less the dynamic lights' part.
// r_dyn1 / r_dyn2: the cosine ray's share of the dynamic lights' first and
// second bounce (see trace_dynamic), n_cosine rays of it this relight.
// r_slot and k: the ray's record and the texel of the quad it leaves from,
// for the split.
void trace_bounce(Texel t, uint r_slot, uint k, inout uint seed, float n_cosine, float n_light, out vec3 indirect_sample, out vec3 r_dyn1, out vec3 r_dyn2, out float bounce_change, out float bounce_change_total, out uint hit_set_id, out float hit_t) {
	indirect_sample = vec3(0.0);
	r_dyn1 = vec3(0.0);
	r_dyn2 = vec3(0.0);
	bounce_change = 0.0;
	bounce_change_total = 0.0;
	hit_set_id = 0xFFFFu;
	hit_t = 0.0;
	if ((params.debug & 1u) != 0u) {
		// Ablated: the probes or the sky stand in for the ray.
		if (!sdfgi_probe_irradiance(t.world_pos - params.camera_origin.xyz, t.n_world, indirect_sample)) {
			indirect_sample = sky_eval(t.n_world);
		}
	} else {
		seed = pcg_hash(seed);
		float r0 = hash_to_float(seed);
		seed = pcg_hash(seed);
		float r1 = hash_to_float(seed);
		vec3 ray_dir = basis_around(t.n_world, vec2(r0, r1));
#ifdef SC_SETUP
		split_request(r_slot, ray_dir, k);
		return;
#endif
		// Opaque: alpha-tested casters occlude the bounce ray whole (the
		// shadow rays above consult the cards' coverage at every candidate;
		// the bounce is a diffuse term and those lookups cost a millisecond
		// on the game project). But a hit under which no facing card has a
		// filled texel is a hole -- the capture leaves an alpha-tested
		// material's texels under half alpha unfilled: a gap in a leaf, or a
		// mesh whose material draws nothing (a room's invisible dome carried
		// a flashlight's light from inside to the walls outside) -- so the
		// ray goes on from it, a few layers at most: a ray more per hole
		// hit, and the lookup it needed anyway.
		float t_hit = 0.0;
		uint hit_instance = SURFACE_CACHE_INVALID;
		vec3 hit_pos = t.origin;
		vec3 card_radiance = vec3(0.0);
		uint hit_set = SURFACE_CACHE_INVALID;
		float hit_change = 0.0;
		float hit_change_total = 0.0;
		vec3 n_hit = t.n_world;
		vec3 albedo_hit = vec3(0.0);
		ivec2 texel_hit = ivec2(0);
		bool blocked = false;
		bool on_card = false;
		vec3 ray_origin = t.origin;
		float t_base = 0.0;
		float mirror_f = 1.0; // The planar mirror's Fresnel, per reflection the ray took.
		vec3 plane_diffuse = vec3(0.0); // The plane's own diffuse card, read on the way.
		uint mirror_bounces = 0u;
#ifdef SC_RESOLVE
		uvec4 rec = split_record(r_slot);
		if ((rec.x & SPLIT_HIT) != 0u) {
			t_hit = uintBitsToFloat(rec.y);
			hit_instance = rec.z;
			hit_pos = t.origin + ray_dir * t_hit;
			blocked = true;
			on_card = card_lookup(hit_instance, hit_pos, ray_dir, card_radiance, hit_set, hit_change, hit_change_total, n_hit, albedo_hit, texel_hit);
		}
#elif !defined(SC_SETUP)
		rayQueryEXT rq;
		for (uint layer = 0u; layer < 4u; layer++) {
			rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, ray_origin, 0.0, ray_dir, 1e4);
			while (rayQueryProceedEXT(rq)) {
			}
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
				break;
			}
			t_hit = t_base + rayQueryGetIntersectionTEXT(rq, true);
			hit_instance = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
			hit_pos = ray_origin + ray_dir * (t_hit - t_base);
			uint hit_mirror = (mirror_on() && (params.mirror_debug & 2u) == 0u && mirror_bounces < MIRROR_BOUNCES_MAX) ? mirror_at(hit_pos) : MAX_MIRROR_PLANES;
			if (hit_mirror < MAX_MIRROR_PLANES) {
				// A planar mirror: its texel's diffuse card is read, then the
				// ray reflects through the mirror's lobe and goes on,
				// weighted by the Fresnel at the bounce (MIRROR_BOUNCES_MAX
				// times; after that a mirror hit reads its diffuse card like
				// any other).
				mirror_bounces++;
				vec3 n = params.mirrors[hit_mirror].plane.xyz;
				if (card_lookup(hit_instance, hit_pos, ray_dir, card_radiance, hit_set, hit_change, hit_change_total, n_hit, albedo_hit, texel_hit)) {
					plane_diffuse += mirror_f * max(card_radiance, vec3(0.0));
				}
				mirror_f *= mirror_fresnel(hit_mirror, abs(dot(n, ray_dir)));
				seed = pcg_hash(seed);
				if (mirror_bounces > 1u && !mirror_roulette(mirror_f, hash_to_float(seed))) {
					mirror_f = 0.0;
					break; // The chain ends here: what it held is in plane_diffuse.
				}
				seed = pcg_hash(seed);
				float u0 = hash_to_float(seed);
				seed = pcg_hash(seed);
				float u1 = hash_to_float(seed);
				ray_dir = mirror_reflect(hit_mirror, ray_dir, vec2(u0, u1));
				t_base = t_hit + params.ray_bias;
				ray_origin = hit_pos + n * params.ray_bias;
				continue;
			}
			blocked = true;
			on_card = card_lookup(hit_instance, hit_pos, ray_dir, card_radiance, hit_set, hit_change, hit_change_total, n_hit, albedo_hit, texel_hit);
			if (on_card || card_reject != 3u) {
				break;
			}
			blocked = false;
			on_card = false;
			t_base = t_hit + params.ray_bias;
			ray_origin = t.origin + ray_dir * t_base;
		}
#endif
		if (blocked) {
			hit_t = t_hit;
			if (on_card) {
				hit_set_id = hit_set & 0xFFFFu;
				if (dyn_lights.count > 0u) {
					// The atlas holds no dynamic light's direct term (see
					// accumulate): the ray reads the static lights' radiance
					// and the dynamic lights' bounces. The first of those is
					// the second-bounce history's (r_dyn2); it comes out of
					// the static sample, from the same relight as the
					// radiance. The direct term at the hit, analytic, with
					// the card's visibility ratio for its shadow, is this
					// ray's estimate of the first bounce, weighted against
					// the light rays by the balance heuristic (each light's
					// density at the hit: its cone's, times the cosine over
					// the distance squared, where the hit is in its cone and
					// range and faces it).
					// Both histories at the hit: the first bounce's and the
					// later bounces', so this reading is the second bounce
					// and, through the later history's own readings, every
					// one after (a bounce of lag apiece); the static
					// accumulation holds none of the dynamic lights' light.
					vec3 dyn_hit = max(imageLoad(indirect_dyn_filtered_atlas, texel_hit).rgb, vec3(0.0));
					card_radiance = max(imageLoad(static_atlas, texel_hit).rgb, vec3(0.0));
					r_dyn2 = albedo_hit * dyn_hit;
					float vis_hit = imageLoad(static_atlas, texel_hit).a;
					float cos_ht = max(dot(n_hit, -ray_dir), 0.0);
					float p_cos = n_cosine * max(dot(t.n_world, ray_dir), 0.0) * cos_ht / (M_PI * max(t_hit * t_hit, 1e-4));
					for (uint i = 0u; i < dyn_lights.count; i++) {
						LightData ld = dyn_lights.data[i];
						bool is_spot = ld.pad > 0.5;
						vec3 axis = is_spot ? normalize(ld.direction) : vec3(0.0, 0.0, 1.0);
						float geom_unused;
						vec3 c = light_contribution_world(ld, is_spot, ld.position, axis, hit_pos, n_hit, geom_unused) * dyn_lights.weights[i >> 2u][i & 3u];
						if (luminance(c) <= 0.0) {
							continue;
						}
						float pdf_light = 0.0;
						if (n_light > 0.0 && ld.inv_radius > 0.0) {
							vec3 rel = ld.position - hit_pos;
							float d = length(rel);
							vec3 l = rel / max(d, 1e-4);
							float cos_hl = dot(n_hit, l);
							float cone_cos = is_spot ? clamp(ld.cone_angle, -1.0, 0.9999) : -1.0;
							if (d < 1.0 / ld.inv_radius && cos_hl > 0.0 && (!is_spot || dot(-l, axis) > cone_cos)) {
								// The light rays' density at this hit: cookie_sample's where
								// the spot has a table, the uniform cone's otherwise.
								float pdf_omega = (is_spot && cookie_table(i)) ? cookie_pdf_omega(i, ld, hit_pos) : 1.0 / (2.0 * M_PI * (1.0 - cone_cos));
								pdf_light = pdf_omega * cos_hl / (d * d);
							}
						}
						// A hit reached through a mirror is a path the light
						// rays cannot make (they land and connect straight to
						// the texel): the cosine ray is its only estimator and
						// takes the whole weight. It had none: p_cos is the
						// direct path's, with the texel's cosine of the ray as
						// it is after the reflection (clamped to zero for a
						// ceiling texel's ray bouncing back up), so the weight
						// read zero against the light rays' density; and as a
						// zero light density instead of this branch, 0 / 0
						// poisoned the histories to a restart (the lab's pillar
						// 0.8% under its static level over the hold either way).
						float w = mirror_bounces > 0u ? 1.0 : (p_cos > 0.0 ? p_cos / (p_cos + n_light * pdf_light) : 0.0);
						r_dyn1 += albedo_hit * c * vis_hit * w;
					}
				}
				indirect_sample = card_radiance;
				tier_stat(0u, card_radiance);
				if (card_reject == 5u && (params.debug & 8192u) != 0u) {
					atomicAdd(dyn_stats.count[29u], 1u); // Read through a depth mismatch (see card_lookup).
					atomicAdd(dyn_stats.count[30u], uint(min(luminance(max(card_radiance, vec3(0.0))), 64.0) * 16.0));
				}
				// The bounce carries the change of the card it came from,
				// weaker by a quarter per bounce, so lighting that reaches
				// this texel only indirectly restarts it too.
				bounce_change = hit_change - 0.25;
				bounce_change_total = hit_change_total - 0.25;
			} else {
				if ((params.debug & 8192u) != 0u) {
					atomicAdd(dyn_stats.count[24u + min(card_reject, 4u)], 1u); // Why the lookup failed (card_reject), 5 slots at 24..28.
				}
				if (sdfgi_probe_irradiance(t.world_pos - params.camera_origin.xyz, t.n_world, indirect_sample)) {
					tier_stat(1u, indirect_sample);
				} else {
					indirect_sample = sky_eval(t.n_world);
					tier_stat(2u, indirect_sample);
				}
			}
		} else {
			indirect_sample = sky_eval(ray_dir);
			tier_stat(3u, indirect_sample);
		}
		indirect_sample = indirect_sample * mirror_f + plane_diffuse;
		r_dyn1 *= mirror_f;
		r_dyn2 *= mirror_f;
	}
}

// The young texel's extra rays. A lighting change restarts the bounce
// accumulation (see accumulate), and for the relights after it the texel is
// one sample, then a few: every gather ray landing near it reads that same
// sample, so its noise is not per pixel but a mottle over the whole surface,
// which no screen-space filter averages and the cards' mip levels only
// spread. More rays where the history is young buy the samples back at the
// restart, and cost nothing where it is not. (A moving light's bounce is
// the dynamic term's now, see trace_dynamic; this is for the rest: a fresh
// capture, a lamp switched, a surface moved.)
#define YOUNG_RELIGHTS 8.0

// The bounce atlas's alpha: the relights (whole, up to 64) over 64, and in
// the half-relight under them the hand-over fraction of a light joining
// the dynamic set (see accumulate), about five bits of it. Readers that
// want a relight count take the alpha times 64 as it is: the fraction is
// under half a relight.
float ind_pack(float relights, float join) {
	return (relights + 0.5 * clamp(join, 0.0, 1.0)) / 64.0;
}

vec2 ind_unpack(float a) {
	float x = a * 64.0;
	float relights = floor(x + 1e-3);
	return vec2(relights, clamp((x - relights) * 2.0, 0.0, 1.0));
}

bool texel_young(ivec2 texel) {
	return imageLoad(indirect_atlas, texel).a * 64.0 < YOUNG_RELIGHTS;
}

// The dynamic histories under YOUNG_RELIGHTS (their age is the second one's
// alpha, see accumulate): a light that moved, and the relights after it.
bool texel_young_dynamic(ivec2 texel) {
	return dyn_lights.count > 0u && imageLoad(indirect_dyn2_atlas, texel).a * 64.0 < YOUNG_RELIGHTS;
}

// The light rays this relight: two at least while the dynamic histories are young.
float light_rays(bool young_dynamic) {
	return float(params.dynamic_rays == 0u ? 0u : (young_dynamic ? max(params.dynamic_rays, 2u) : params.dynamic_rays));
}

// How many cosine rays the texel traces this relight (see trace_bounce_young):
// the extra ones for a young accumulation, and while a dynamic light moves
// (its histories are then a relight or two long, and the cosine rays are
// the better half of its estimate wherever it lights a wide area).
uint cosine_rays(bool young, bool young_dynamic) {
	bool dynamic_young = (params.flags & FLAG_DYNAMIC_YOUNG) != 0u && young_dynamic;
	return ((young || dynamic_young) && (params.debug & 1u) == 0u) ? params.young_rays + 1u : 1u;
}

void trace_bounce_young(uint n_cosine, float n_light, Texel t, uint k, inout uint seed, inout vec3 indirect_sample, inout vec3 dyn1, inout vec3 dyn2, inout float bounce_change, inout float bounce_change_total) {
	if (n_cosine <= 1u) {
		return;
	}
	for (uint r = 1u; r < n_cosine; r++) {
		vec3 extra;
		vec3 extra_dyn1;
		vec3 extra_dyn2;
		float extra_change;
		float extra_total;
		uint extra_set;
		float extra_t;
		trace_bounce(t, r, k, seed, float(n_cosine), n_light, extra, extra_dyn1, extra_dyn2, extra_change, extra_total, extra_set, extra_t);
		indirect_sample += extra;
		dyn1 += extra_dyn1;
		dyn2 += extra_dyn2;
		bounce_change = max(bounce_change, extra_change);
		bounce_change_total = max(bounce_change_total, extra_total);
	}
	indirect_sample /= float(n_cosine);
	dyn1 /= float(n_cosine);
	dyn2 /= float(n_cosine);
}

// The bounce gradient: the previous relight's ray traced again from the same
// texel with the same seed, what it hits now against what it hit then
// (stored per texel): another set, or the same one at a distance changed by
// more than a tenth, is a surface that moved into or out of the ray's way.
// A ray that finds the same surface at the same distance gives nothing,
// whatever its radiance did. Off for a fresh texel and when the set has no
// previous relight.
float bounce_gradient(ivec2 texel, Texel t, uint k, uint prev_seed, bool have_prev) {
	if (!have_prev || (params.debug & 16u) != 0u) {
		return -1.0;
	}
	Change prev = change_load(texel);
	vec4 old_indirect = imageLoad(indirect_atlas, texel);
	if (old_indirect.a <= 0.0) {
		return -1.0;
	}
	vec3 again;
	float unused_change;
	uint set_now;
	float t_now;
	uint seed = prev_seed;
	float unused_total;
	vec3 unused_dyn1;
	vec3 unused_dyn2;
	trace_bounce(t, SPLIT_GRADIENT_SLOT, k, seed, 1.0, 0.0, again, unused_dyn1, unused_dyn2, unused_change, unused_total, set_now, t_now);
#ifdef SC_SETUP
	return -1.0;
#endif
	// The verdict is the distance's alone. The set's identity was compared
	// first, and on the TPS bridge it restarted 1.7% of the relit quads
	// every relight with the geometry still: coincident faces of two
	// instances trade the hit between two builds of the TLAS (the
	// intersection-query traversal halves the count, so it is the
	// traversal's tie, not a ray of ours), and the mark's spread and decay
	// turned that into a screen history two frames long everywhere
	// (section 77). A surface that moved into or out of the ray's way is
	// at another distance; one that only changed its name is not a change.
	// GODOT_CARD_ABLATE=gradset counts the verdicts (converge.gradient_t)
	// and applies none.
	if (set_now == 0xFFFFu && prev.bounce_set == 0xFFFFu && (t_now <= 0.0 || prev.bounce_t <= 0.0)) {
		return 0.0; // The sky both times.
	}
	float rel = abs(t_now - prev.bounce_t) / max(max(t_now, prev.bounce_t), 0.05);
	if ((params.debug & (32768u | 65536u)) != 0u) {
		atomicAdd(converge.gradient_t[set_now != prev.bounce_set ? 3u : (rel >= 0.3 ? 2u : (rel >= 0.05 ? 1u : 0u))], 1u);
		if (set_now != prev.bounce_set) {
			atomicAdd(converge.gradient_t[gradient_gap <= 1u ? 4u : (gradient_gap <= 4u ? 5u : (gradient_gap <= 12u ? 6u : 7u))], 1u);
		}
		if ((params.debug & 32768u) != 0u) {
			return 0.0;
		}
	}
	return smoothstep(0.05, 0.3, rel);
}

// A verdict is one ray's, and one ray's verdict restarts nothing on its
// own: the SIMD group's rays (32 quads of the block) vote, and the verdict
// stands from three agreeing. A surface moving into or out of the way of
// the block's rays is seen by several of them; the one ray in two thousand
// that came back from another face of the same still level (the TPS
// bridge's residue, section 77) is not, and through the readers' chain and
// the neighbours' spread it had restarted the whole level's history.
// Under GODOT_CARD_GRAD_MEAN: the voted verdict at its full size, for the
// texel's own bounce restart (accumulate); the returned one is the group's
// share, the mark it hands on.
float gradient_local = -1.0;

float bounce_gradient_voted(float gradient) {
	uint votes = subgroupAdd(gradient > 0.3 ? 1u : 0u);
	if ((params.debug & 65536u) != 0u && gradient > 0.3) {
		// Diagnostics (GODOT_CARD_ABLATE=gradvote): verdicts by their votes.
		atomicAdd(converge.gradient_t[votes >= 3u ? 5u : 4u], 1u);
	}
	float gate = smoothstep(1.0, 3.0, float(votes));
	gradient_local = gradient >= 0.0 ? gradient * gate : gradient;
	if ((params.debug & 1048576u) != 0u) {
		// Experiment (GODOT_CARD_GRAD_MEAN=1): the verdict's size is the
		// share of the group's rays that saw the move, A-SVGF's gradient
		// normalized over its stratum, not the one ray's. A ray is one
		// direction of the texel's hemisphere: a gray box crossing the lab
		// changed a sliver of it for the few texels whose rays met it, and a
		// whole-size mark from each, carried on by the readers' chain and the
		// neighbours' spread, restarted 97% of the screen's GI history every
		// frame of the move. A wall that swings into the way of most of the
		// block's rays still reads near one. The texel's own bounce still
		// restarts by its ray's whole verdict (gradient_local): the lab's
		// wall beside the sweeping box, restarted by the share alone, held
		// the box's gray bounce, and the floor's reflection of the wall's
		// foot read it (its raw sample 0.052 green against 0.031 converged).
		float traced = subgroupAdd(gradient >= 0.0 ? 1.0 : 0.0);
		float share = subgroupAdd(max(gradient, 0.0)) / max(traced, 1.0);
		return gradient >= 0.0 ? share * gate : gradient;
	}
	return gradient * gate;
}

// The bounce a texel hands its readers: its histories filtered over the
// card -- an a-trous 5x5 at a stride from the histories' age (3 texels at
// one or two relights, 2 up to six, 1 from there on), the taps weighted by
// the binomial kernel, by how alike the captured normals are and by how
// near the stored depths (a tilted plane's depth climbs a few centimeters
// a texel; another object in the same card sits tens of centimeters off).
// The bounce is a smooth field and one cosine ray per quad per relight
// estimates it: in a room lit by bounce alone the accumulation still
// wandered by 6% of itself at three hundred relights, and where a moving
// light kept the dynamic histories at a relight or two the readers used
// to read them through the youth mip -- the 8x8 blocks the beam's bounce
// blotched into -- as noisy as the block's few texels. The 5x5 binomial
// cuts the noise by 3.7; at stride one its twelve lightest taps are
// dropped (thirteen taps, the noise cut by 3.2) for half the fetches. The static and the dynamic
// histories share the taps (one stride, the younger's); a texel whose
// histories are both past sixteen relights refreshes its filtered value
// every eighth relight only, staggered (the field moves slowly by then,
// and the taps were two thirds of the lighting pass). The ages the
// readers should take the results for count the taps in.
void filter_bounces(ivec2 texel, vec3 own_static, float static_age, vec3 own_dyn, float dyn_age, bool dynamic, ivec2 card_min, ivec2 card_max, out vec3 r_static, out float r_static_age, out vec3 r_dyn, out float r_dyn_age) {
	r_static = own_static;
	r_static_age = static_age;
	r_dyn = own_dyn;
	r_dyn_age = dyn_age;
	if ((params.flags & FLAG_DYN_FILTER) == 0u) {
		return;
	}
	float age = dynamic ? min(static_age, dyn_age) : static_age;
	if (age > 16.0 && ((params.frame + uint(texel.x) + uint(texel.y)) & 7u) != 0u) {
		// Between refreshes: the last filtered values stand.
		vec4 prev_static = imageLoad(indirect_filtered_atlas, texel);
		vec4 prev_dyn = imageLoad(indirect_dyn_filtered_atlas, texel);
		if (prev_static.a > 0.0) {
			r_static = prev_static.rgb;
			r_static_age = prev_static.a * 64.0;
		}
		if (dynamic && prev_dyn.a > 0.0) {
			r_dyn = prev_dyn.rgb;
			r_dyn_age = prev_dyn.a * 64.0;
		}
		return;
	}
	int stride = age <= 2.0 ? 3 : (age <= 6.0 ? 2 : 1);
	// A coarse relight's representative (see light_main): at a texel's
	// stride the taps fall in its own cell, which holds its own splatted
	// history, so the filter averaged one estimate with itself (at level 3
	// every tap) and counted it as twenty-five. A cell's stride reaches the
	// neighboring cells' representatives. The depth tolerance stays the
	// texel stride's, so the far taps join only on surfaces nearly parallel
	// to the card: grown with the step (to two meters at level 3's widest)
	// they reached across the machines' parts, fireflies +80% at rest
	// (plan section 96).
	int step = (params.flags & FLAG_SPLAT_CELL) != 0u ? stride * splat_side : stride;
	float depth_c = texelFetch(depth_atlas, texel, 0).r;
	vec3 n_c = normalize(texelFetch(normal_atlas, texel, 0).rgb * 2.0 - 1.0);
	float depth_tol = 0.1 + 0.08 * float(stride);
	const float kernel[5] = float[5](1.0, 4.0, 6.0, 4.0, 1.0);
	vec3 sum_static = vec3(0.0);
	vec3 sum_dyn = vec3(0.0);
	float weight = 0.0;
	float taps = 0.0;
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			ivec2 n = texel + ivec2(dx, dy) * step;
			if (any(lessThan(n, card_min)) || any(greaterThan(n, card_max))) {
				continue;
			}
			float w = kernel[dx + 2] * kernel[dy + 2];
			if (stride == 1 && w <= 4.0) {
				continue; // At stride one (the histories past six relights) the twelve lightest taps go: a seventh of the weight for half the fetches.
			}
			vec3 v_static;
			vec3 v_dyn;
			if (dx == 0 && dy == 0) {
				v_static = own_static;
				v_dyn = own_dyn;
			} else {
				float depth_n = texelFetch(depth_atlas, n, 0).r;
				if (depth_n <= 0.0 || abs(depth_n - depth_c) >= depth_tol) {
					continue;
				}
				vec3 n_n = normalize(texelFetch(normal_atlas, n, 0).rgb * 2.0 - 1.0);
				float align = max(dot(n_c, n_n), 0.0);
				w *= align * align * align * align;
				if (w <= 0.0) {
					continue;
				}
				v_static = max(imageLoad(indirect_atlas, n).rgb, vec3(0.0));
				v_dyn = dynamic ? (max(imageLoad(indirect_dyn_atlas, n).rgb, vec3(0.0)) + max(imageLoad(indirect_dyn2_atlas, n).rgb, vec3(0.0))) : vec3(0.0);
				if (any(isnan(v_static)) || any(isinf(v_static)) || any(isnan(v_dyn)) || any(isinf(v_dyn))) {
					continue;
				}
			}
			sum_static += v_static * w;
			sum_dyn += v_dyn * w;
			weight += w;
			taps += w / 36.0; // In centre-tap units: the kernel-equivalent sample count.
		}
	}
	if (weight <= 0.0) {
		return;
	}
	r_static = sum_static / weight;
	r_static_age = min(static_age * max(taps, 1.0), 64.0);
	if (dynamic) {
		r_dyn = sum_dyn / weight;
		r_dyn_age = min(dyn_age * max(taps, 1.0), 64.0);
	}
}

// The temporal gradients and the accumulations, into the atlases. The card's
// radiance is assembled here every relight from an exact direct term and two
// accumulated factors, never accumulated itself: a light's colour or
// intensity changing, or a light moving, shows in the cards the frame the
// set is relit, and the histories hold only what is stochastic -- the local
// lights' visibility ratio and the bounce.
// bounce_gradient is the re-traced previous ray's relative change (or a
// negative value when there was no previous ray to re-trace); card_min /
// card_max bound the card, so the change read from the neighbors never
// crosses into another card packed beside it in the atlas.
// The change mark's decay this relight: an eighth a frame since the tile's
// last relight, so a mark fades in the same eight frames however often the
// tile is relit. Per relight, as it was, a level whose tiles are relit
// every ten or fifteen frames (the TPS bridge under the texel budget)
// kept every mark for a hundred frames, and with the neighbours' spread
// and the readers' hops that outran the fade: the change field sat at a
// third everywhere and the screen history at three frames (section 77).
float mark_decay = 0.125;

void accumulate(ivec2 texel, Texel t, bool reset, Direct d, vec3 indirect_sample, float bounce_change, float bounce_change_total, float dyn_change_total, vec3 dyn_sample, vec3 dyn2_sample, float dyn_landed, float bounce_gradient, uint bounce_set, float bounce_t, ivec2 card_min, ivec2 card_max) {
#ifdef SC_SETUP
	return; // The setup kernel only requests the rays.
#endif
	vec4 old = imageLoad(lighting_atlas, texel);
	Change prev = change_load(texel);
	// A fresh capture has nothing to compare with, and neither has a texel
	// never lit since its capture (no frames accumulated): the capture's
	// reset flag is raised on the frame its record is built, which is not
	// always the frame it is first lit.
	bool fresh = reset || old.a <= 0.0;
	// The change the ray's hit carried, weighted by what the hit is worth
	// here: the sample's luminance over the accumulation's. A wall lit by a
	// panel reads the panel's change whole (the sample is the accumulation);
	// a ray that landed on a dim thing that moved reads little of it. The
	// change carried whole, a level's three actors breathing (0.3% of the
	// rays' hits changing a relight) reached every texel within four hops
	// and the screen's history never passed eight frames (section 77).
	// A restarted accumulation (nothing to weigh against) reads it whole.
	{
		vec4 acc = imageLoad(indirect_atlas, texel);
		float l_acc = luminance(max(acc.rgb, vec3(0.0)));
		float share = (fresh || acc.a <= 0.0 || l_acc <= 1e-5) ? 1.0 : clamp(luminance(max(indirect_sample, vec3(0.0))) / l_acc, 0.0, 1.0);
		bounce_change *= share;
		bounce_change_total *= share;
		// The light rays' landings (trace_dynamic) the same way, by the
		// dynamic sample's luminance over the whole accumulation, static
		// and dynamic: a texel the moving light's bounce barely reaches
		// reads little of what its landings saw.
		float l_all = l_acc + luminance(max(imageLoad(indirect_dyn_atlas, texel).rgb, vec3(0.0))) + luminance(max(imageLoad(indirect_dyn2_atlas, texel).rgb, vec3(0.0)));
		float dyn_share = (fresh || acc.a <= 0.0 || l_all <= 1e-5) ? 1.0 : clamp(luminance(max(dyn_sample, vec3(0.0))) / l_all, 0.0, 1.0);
		bounce_change_total = max(bounce_change_total, dyn_change_total * dyn_share);
	}

	// The radiance gradient: how much the deterministic part of this texel's
	// lighting moved since the last relight, relative to itself, per channel
	// and the largest taken (a hue turning at constant luminance is a change
	// the eye sees, and a luminance gradient read it as nothing: the cards
	// and the GI's history held the old hue for their whole window). Each
	// channel is relative to its own size, floored at a quarter of the
	// luminance so a channel that is nearly nothing cannot restart by
	// doubling (and the denominator is not the luminance: on a red wall
	// that read the red channel's move at five times its size, restarted
	// every history to one frame at every relight, and the glossy floor's
	// sparse reflections came out bright). Static lights give exactly zero
	// (the numbers are recomputed from the same inputs).
	// The static lights' term: the dynamic lights (see trace_dynamic) are
	// gradients of their own.
	vec3 unshadowed = t.albedo * (d.unshadowed - d.dyn_sum) + t.emission;
	float dyn_lum = luminance(t.albedo * d.dyn_sum);
	vec4 dyn_old = imageLoad(indirect_dyn_atlas, texel);
	vec3 delta = abs(unshadowed - prev.unshadowed);
	float lum_floor = 0.25 * max(luminance(unshadowed), luminance(prev.unshadowed));
	vec3 rel = delta / max(max(unshadowed, prev.unshadowed), vec3(max(lum_floor, 1e-4)));
	float change = (fresh || level_changed) ? 0.0 : max(max(rel.r, rel.g), rel.b);
	// The whole lighting's change, for the GI gather's screen history: the
	// static change, and the dynamic lights' direct term here against the
	// last relight's (by luminance, floored at a quarter of the whole), and
	// what the rays carried from their hits.
	float total_lum = luminance(unshadowed) + dyn_lum;
	float prev_total_lum = luminance(prev.unshadowed) + dyn_old.a;
	float dyn_change = (fresh || level_changed) ? 0.0 : abs(dyn_lum - dyn_old.a) / max(max(dyn_lum, dyn_old.a), max(0.25 * max(total_lum, prev_total_lum), 1e-4));
	// A cap on a moving light's mark was measured (section 86): the TPS
	// demo's forklift beams kept the screen at one frame uncapped, but the
	// cap is the screen's lag on the change; uncapped since the moving
	// lights' term became a screen history of its own (section 88).
	// A light switching or changing colour (params.dynamic_change) is a
	// step the screen should take whole.
	if (!fresh && dyn_lum > 0.05 * total_lum) {
		dyn_change = max(dyn_change, params.dynamic_change);
	}
	float change_total = max(max(change, dyn_change), max(prev.change - mark_decay, bounce_change_total));
	uint change_src = change_total <= 0.05 ? 8u : (change_total == change ? CHANGE_SRC_STATIC : (change_total == dyn_change ? CHANGE_SRC_DYN : (change_total == bounce_change_total ? CHANGE_SRC_HIT : CHANGE_SRC_PREV)));
	// The change outlives the relight that found it, fading over eight: the
	// gather's one ray per pixel lands on a given card only now and then,
	// and a change seen for one frame would restart almost no pixel.
	change = max(change, max(prev.change_static - mark_decay, bounce_change));
	// The bounce gradient is one ray's verdict, so only the texels whose
	// last ray happened to see what moved would restart on their own and
	// the rest would hold the stale bounce beside them: the change spreads
	// to the eight neighbors, a texel per relight, fading as it goes.
	if (!fresh) {
		change = max(change, bounce_gradient);
		float spread = 0.0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (dx == 0 && dy == 0) {
					continue;
				}
				ivec2 n = texel + ivec2(dx, dy);
				if (any(lessThan(n, card_min)) || any(greaterThan(n, card_max))) {
					continue;
				}
				spread = max(spread, change_load_gradient(n));
			}
		}
		change = max(change, spread - mark_decay);
		if (change > change_total && change > 0.05) {
			change_src = change == bounce_gradient ? CHANGE_SRC_GRADIENT : (change == spread - mark_decay ? CHANGE_SRC_SPREAD : CHANGE_SRC_PREV);
		}
	}
	change_total = max(change_total, change);
	if ((params.flags & 4096u) != 0u) {
		if (change_src < 8u) {
			atomicAdd(converge.change_src[change_src], 1u);
		}
		if (reset) {
			atomicAdd(converge.change_src[7], 1u);
		}
	}

	// The geometric gradient: the local lights' geometric sum against the
	// last relight's. Only movement (or a light appearing or vanishing) can
	// change a texel's visibility; a colour or intensity change leaves the
	// ratio exactly right and is not a reason to lose its history.
	// Floored where no light to speak of reaches the texel: the stored sum
	// is a half, and below a thousandth its rounding alone read as a move
	// (a few hundred texels a frame at rest restarting their ratio).
	float geom_change = (fresh || level_changed) ? 0.0 : abs(d.local_geom - prev.geom) / max(max(d.local_geom, prev.geom), 1e-3);
	if ((params.flags & 4096u) != 0u && geom_change > 0.02) {
		atomicAdd(converge.change_src[CHANGE_SRC_GEOM], 1u);
	}

	// Two histories, each restarted to the frame count its gradient leaves
	// credible (A-SVGF: alpha = max(alpha, gradient)); a small change barely
	// touches them, a light switching costs them all their frames but one.
	// The window is twice the setting: before the ratio, the composite was
	// accumulated over the accumulated bounce, and the two cascaded windows
	// smoothed as much as one of double the length.
	float window = float(min(2u * params.temporal_frames, 64u));

	// The visibility ratio (one for every local light, dynamic or not; a
	// ratio of the dynamic lights' own, drawn on alternate relights, was
	// measured and changed nothing).
	float keep_vis = (geom_change > 0.02 && (params.debug & 64u) == 0u) ? max(1.0, 1.0 / geom_change) : 64.0;
	float frames = reset ? 0.0 : min(old.a * 64.0, keep_vis);
	float vis = prev.vis;
	if (d.sampled) {
		float alpha = max(1.0 / (frames + 1.0), 1.0 / window);
		vis = frames <= 0.0 ? d.vis : mix(prev.vis, d.vis, alpha);
	} else if (frames <= 0.0) {
		vis = 1.0; // No local light reaches this texel; the sum is zero anyway.
	}
	float vis_dyn = vis;
	frames = min(frames + 1.0, 64.0);
	Change now;
	now.unshadowed = unshadowed;
	now.change = change_total;
	now.change_static = change;
	now.geom = d.local_geom;
	now.vis = vis;
	now.bounce_set = bounce_set;
	now.bounce_t = bounce_t;
	change_store(texel, now);

	// The bounce, one ray per quad per relight: restarted by the radiance gradient (a
	// light that changed here changed at the texels this one bounces off,
	// near enough) and by the change the ray's own hit carried.
	// The restart is A-SVGF's, to 1 / change relights, for the gradient as
	// for a light. Two softer forms were measured against rt_lab's moving
	// box and neither kept: a quarter-strength change (a restart to four
	// relights) left the descend at 0.058 eight frames after the stop, the
	// same as no gradient, because the spread to the neighbors dies a
	// texel out and the glow stays wherever no ray saw the box leave; and a
	// floor of four relights on the bounce alone, with the change carried
	// whole, cleared the glow as the full restart does and left the flicker
	// at rest exactly where it was (0.0050 against 0.0051 under the box
	// still sweeping) -- that flicker is the GI screen history restarting
	// at the pixels whose rays hit the tracked texels, not the bounce's
	// own sample count.
	// The dynamic bounces: histories of their own, accumulated like the
	// static one but never restarted by a change; instead the lights'
	// motion caps their length (A-SVGF's alpha = max(alpha, gradient), the
	// gradient being how far the lights moved this frame): a sweep keeps
	// them at a frame or two, at rest they grow to the window.
	vec3 dyn = vec3(0.0);
	vec3 dyn2 = vec3(0.0);
	float dyn_frames = 0.0;
	vec4 dyn2_old = imageLoad(indirect_dyn2_atlas, texel);
	if (dyn_lights.count > 0u) {
		// Floored at one old relight on purpose: a full-refresh motion keeps
		// half of the last relight. Measured without the floor (the history
		// worth 1 / motion relights including this one, so nothing kept at a
		// full refresh) on the game flick: the cards' field 0.057 -> 0.062 at
		// the stop, 0.022 -> 0.028 four frames on, the screen 0.069 -> 0.071;
		// one relight's two light rays are noisier than the half-relight of
		// lag they replace.
		float keep_dyn = params.dynamic_motion > 0.0 ? max(1.0, 1.0 / params.dynamic_motion) : params.dynamic_window;
		// A join (see below) starts them afresh: a light changing again while
		// its weight fades had its histories at that weight.
		dyn_frames = (fresh || params.dynamic_join > 0.0) ? 0.0 : min(min(dyn2_old.a * 64.0, keep_dyn), params.dynamic_window);
		float dyn_alpha = 1.0 / (dyn_frames + 1.0);
		dyn = mix(max(dyn_old.rgb, vec3(0.0)), dyn_sample, dyn_alpha);
		dyn2 = mix(max(dyn2_old.rgb, vec3(0.0)), dyn2_sample, dyn_alpha);
		dyn_frames = min(dyn_frames + 1.0, 64.0);
	}
	// Under GODOT_CARD_GRAD_MEAN the mark is the group's share; the texel's
	// own bounce restarts by its ray's whole verdict (see
	// bounce_gradient_voted).
	float change_ind = (params.debug & 1048576u) != 0u ? max(change, gradient_local) : change;
	float keep_ind = (change_ind > 0.02 && (params.debug & 32u) == 0u) ? max(1.0, 1.0 / change_ind) : 64.0;
	vec4 old_indirect = imageLoad(indirect_atlas, texel);
	// The hand-over of a light joining the dynamic set (params.dynamic_join,
	// the frame every set is relit). Until it moved, the light was one of
	// the static lights: the accumulation holds its bounce, and from this
	// relight the dynamic histories estimate the whole of it, so the room
	// would read the beam's bounce twice, fading over the static window (a
	// flashlight's first move brightened the ceiling by a tenth for half a
	// second). Neither a restart nor a subtraction of the first relight's
	// estimate answers it: one relight's light rays are a fraction of the
	// bounce, and its second bounce is not in them at all. Instead the
	// texel keeps the fraction of its pre-join content the accumulation
	// still holds -- one at the join, times one less the blend weight every
	// relight, so a restart clears it -- and stores the accumulation less
	// that fraction of the dynamic histories: what the readers sum (this
	// and the dynamic histories) is then the pre-join value exactly at the
	// join, and moves to the static accumulation plus the dynamic estimate
	// at the accumulation's own pace, unbiased at every relight, the
	// estimate's noise entering only as fast as the accumulation forgets.
	// The fraction rides in the alpha's half-relight (the relight count is
	// whole; see ind_unpack).
	vec2 ind_old = ind_unpack(old_indirect.a);
	float join = 0.0;
	if (dyn_lights.count > 0u && !fresh) {
		// The stored value plus the fraction of the histories it was stored
		// less: the accumulation itself.
		old_indirect.rgb += ind_old.y * (max(dyn_old.rgb, vec3(0.0)) + max(dyn2_old.rgb, vec3(0.0)));
		join = max(ind_old.y, params.dynamic_join);
	}
	float ind_frames = reset ? 0.0 : min(ind_old.x, keep_ind);
	float ind_alpha = max(1.0 / (ind_frames + 1.0), 1.0 / window);
	vec3 indirect = ind_frames <= 0.0 ? indirect_sample : mix(old_indirect.rgb, indirect_sample, ind_alpha);
	join *= ind_frames <= 0.0 ? 0.0 : 1.0 - ind_alpha;
	if ((params.flags & 4096u) != 0u) {
		// The editor's convergence count (the Converge buffer above): this
		// texel is settled once its bounce has a window's worth of relights
		// behind it and nothing marked it changed.
		atomicAdd(converge.relit, 1u);
		if (ind_frames + 1.0 < window - 1.0 || change_total > 0.05) {
			atomicAdd(converge.young, 1u);
		} else {
			float drift = luminance(indirect) - luminance(old_indirect.rgb);
			uint q = uint(min(abs(drift), 16.0) * 1024.0);
			if (drift >= 0.0) {
				atomicAdd(converge.up, q);
			} else {
				atomicAdd(converge.down, q);
			}
		}
	}
	if ((params.debug & 128u) != 0u) {
		// Diagnostics (GODOT_CARD_ABLATE=paint, seen through GODOT_GI_FALLBACK=all):
		// the texel's own radiance gradient, the re-traced bounce gradient,
		// and the capture reset, as colour. The paint replaces the
		// accumulation, and the rays of other texels read it through their
		// hits, so the whole scene tints within a few relights: read the
		// first frames.
		indirect = vec3(fresh ? 0.0 : max(max(rel.r, rel.g), rel.b), max(bounce_gradient, 0.0), reset ? 1.0 : 0.0);
	} else if ((params.debug & 256u) != 0u) {
		// (paint2) The change the bounce ray carried from its hit, the
		// neighbours' spread, and the fading change from the last relight.
		indirect = vec3(max(bounce_change, 0.0), 0.0, max(prev.change - mark_decay, 0.0));
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				ivec2 n = texel + ivec2(dx, dy);
				if ((dx != 0 || dy != 0) && all(greaterThanEqual(n, card_min)) && all(lessThanEqual(n, card_max))) {
					indirect.g = max(indirect.g, change_load_gradient(n));
				}
			}
		}
	}
	if ((params.debug & 512u) != 0u) {
		// (paint3) Whether any light is dynamic, the share of the dynamic
		// rays that landed on a card and connected, and the dynamic term's
		// share of the bounce.
		indirect = vec3(dyn_lights.count > 0u ? 1.0 : 0.0, dyn_landed, luminance(dyn + dyn2) / max(luminance(indirect + dyn + dyn2), 1e-4));
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	} else if ((params.debug & 131072u) != 0u) {
		// (paint8) The static lights' visibility ratio (r) and the dynamic lights' (g).
		indirect = vec3(vis, vis_dyn, 0.0);
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	} else if ((params.debug & 2048u) != 0u) {
		// (paint5) The share of the dynamic rays that landed and connected,
		// as gray: a luminance readout (rt_lab/radiosity_box.gd prints it
		// at its sample points).
		indirect = vec3(dyn_landed);
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
	}
	// Nothing non-finite reaches the atlases: they persist, and every reader
	// (the gather's rays, the other texels' bounce rays, the hit shader)
	// would carry it on. A poisoned accumulation restarts instead.
	if (any(isnan(indirect)) || any(isinf(indirect))) {
		indirect = vec3(0.0);
		ind_frames = 0.0;
		join = 0.0;
		if ((params.debug & 8192u) != 0u) {
			atomicAdd(dyn_stats.count[31], 1u);
		}
	}
	if (any(isnan(dyn)) || any(isinf(dyn)) || any(isnan(dyn2)) || any(isinf(dyn2))) {
		dyn = vec3(0.0);
		dyn2 = vec3(0.0);
		dyn_frames = 0.0;
	}
	SPLAT_STORE(indirect_dyn_atlas, texel, vec4(dyn, dyn_lum));
	SPLAT_STORE(indirect_dyn2_atlas, texel, vec4(dyn2, dyn_frames / 64.0));
	// The accumulation less the joining light's share of the dynamic
	// histories (see the hand-over above); the readers add the histories.
	vec3 indirect_stored = max(indirect - join * (dyn + dyn2), vec3(0.0));
	SPLAT_STORE(indirect_atlas, texel, vec4(indirect_stored, ind_pack(min(ind_frames + 1.0, 64.0), join)));
	// What the readers take for the bounces: the accumulations filtered
	// over the card (the accumulations themselves stay raw above).
	vec3 ind_read;
	float ind_read_age;
	vec3 dyn_read;
	float dyn_read_age;
	filter_bounces(texel, indirect_stored, min(ind_frames + 1.0, 64.0), dyn + dyn2, dyn_frames, dyn_lights.count > 0u, card_min, card_max, ind_read, ind_read_age, dyn_read, dyn_read_age);
	if (dyn_lights.count == 0u) {
		dyn_read = vec3(0.0);
		dyn_read_age = 0.0;
	}
	if ((params.debug & 262144u) != 0u) {
		// (paintn) The captured world-space normal as colour, in the bounce
		// the gather's fallback reads (GODOT_GI_FALLBACK=all shows every
		// pixel's own texel): a texel captured from the wrong side shows here.
		ind_read = t.n_world * 0.5 + 0.5;
		dyn_read = vec3(0.0);
	}
	SPLAT_STORE(indirect_filtered_atlas, texel, vec4(ind_read, ind_read_age / 64.0));
	SPLAT_STORE(indirect_dyn_filtered_atlas, texel, vec4(dyn_read, dyn_read_age / 64.0));

	// The radiance the rays read: without the dynamic lights' direct term,
	// which every reader adds from the lights' current state (the gather at
	// its hits, the light rays at their landings): a card relit with the
	// beam where it was a frame ago never hands that beam to a reader
	// subtracting it where it is now.
	vec3 direct = d.exact + max(d.local_sum - d.dyn_sum, vec3(0.0)) * vis;
	vec3 radiance = max(t.albedo * (direct + ind_read + dyn_read) + t.emission, vec3(0.0));
	if ((params.debug & 262144u) != 0u) {
		// (paintn) The captured world-space normal, as colour (seen through
		// GODOT_GI_FALLBACK=all): a texel lit from the wrong side shows here.
		radiance = t.n_world * 0.5 + 0.5;
	} else if ((params.debug & 524288u) != 0u) {
		// (paintl) The local lights' unshadowed sum (r), how many the texel's
		// list held over eight (g) and the nearest one's distance over its
		// range (b, 1 when none is in range): a level whose cards stay dark
		// (the TPS demo's) is read here as lights that never reach the
		// texels or lights that do and contribute nothing.
		radiance = vec3(luminance(d.local_sum), float(d.local_count) / 8.0, d.local_nearest);
	}
	vec3 static_radiance = max(t.albedo * (direct + ind_read) + t.emission, vec3(0.0));
	bool splat_material = splat_side > 1 && (params.flags & FLAG_SPLAT_CELL) != 0u && (params.debug & (262144u | 524288u)) == 0u;
	if (any(isnan(radiance)) || any(isinf(radiance)) || any(isnan(static_radiance)) || any(isinf(static_radiance))) {
		radiance = vec3(0.0);
		static_radiance = vec3(0.0);
		splat_material = false;
		if ((params.debug & 8192u) != 0u) {
			atomicAdd(dyn_stats.count[31], 1u);
		}
	}
	if (splat_material) {
		// A coarse relight's cell shares the representative's lighting, not
		// its material: every texel's radiance is its own albedo and emission
		// over the cell's light, so a textured or partly emissive cell keeps
		// its pattern (the representative's colour splatted over sixty-four
		// texels made hot blocks of a bright fleck), and the mip chain's
		// average is the cell's.
		vec3 lit = direct + ind_read + dyn_read;
		vec3 lit_static = direct + ind_read;
		for (int sy = 0; sy < splat_side; sy++) {
			for (int sx = 0; sx < splat_side; sx++) {
				uint bi = uint(sy * splat_side + sx);
				if (((bi < 32u ? splat_mask.x >> bi : splat_mask.y >> (bi - 32u)) & 1u) != 0u) {
					ivec2 c = splat_origin + ivec2(sx, sy);
					vec3 albedo_c = card_diffuse_albedo(c, t.world_pos);
					vec3 emission_c = texelFetch(emission_atlas, c, 0).rgb;
					imageStore(static_atlas, c, vec4(max(albedo_c * lit_static + emission_c, vec3(0.0)), vis_dyn));
					imageStore(lighting_atlas, c, vec4(max(albedo_c * lit + emission_c, vec3(0.0)), frames / 64.0));
				}
			}
		}
	} else {
		SPLAT_STORE(static_atlas, texel, vec4(static_radiance, vis_dyn));
		SPLAT_STORE(lighting_atlas, texel, vec4(radiance, frames / 64.0));
	}
	mip_dirty.tiles[uint(texel.y >> 5) * (params.atlas_size >> 5u) + uint(texel.x >> 5)] = 1u;
	if (splat_side > 1) {
		ivec2 far_corner = splat_origin + ivec2(splat_side - 1);
		mip_dirty.tiles[uint(far_corner.y >> 5) * (params.atlas_size >> 5u) + uint(far_corner.x >> 5)] = 1u;
	}
}

// The setup kernel's record of a texel's ray origin and normal, for the
// trace kernel.
void split_texel_store(uint k, Texel t) {
#ifdef SC_SETUP
	split_texels.data[split_thread * 8u + k * 2u] = uvec4(floatBitsToUint(t.origin), packUnorm2x16(vec3_to_oct(t.n_world)));
#endif
}

void light_main() {
	// One workgroup per work item the prepare pass listed: the block of a
	// card of an active set (a requested tile, or every block of a set due
	// in full). Nothing here walks blocks a ray never asked for. With the
	// split, the single kernel is dispatched over the items past the ones
	// it covers (the setup kernel writes that count).
#ifdef SC_SPLIT
	uint item_index = gl_WorkGroupID.x;
#else
	uint item_index = gl_WorkGroupID.x + params.split.x;
#endif
	if (item_index >= active_sets.item_count) {
		return;
	}
#ifdef SC_SPLIT
	if (item_index >= params.split.x) {
		return; // The single kernel's.
	}
	split_thread = item_index * 64u + gl_LocalInvocationIndex;
#ifdef SC_RESOLVE
	uvec4 header = split_headers.data[split_thread];
	split_shadow_mask = header.x;
	split_shadow_hits = header.y;
	split_ray_mask = header.z;
#endif
#endif
	uint item = active_sets.items[item_index];
	uint entry = item & SURFACE_CACHE_ITEM_ENTRY_MASK;
	uint lod = (item >> SURFACE_CACHE_ITEM_LOD_SHIFT) & 7u;
	uint set = active_sets.list[entry] & 0x7FFFFFFFu;
	CardSet s = sets.data[set];
	// The workgroup tiles 8x8 texels, so a set under eight an edge cannot
	// be lit here; min_card_size is clamped to 8 (surface_cache.cpp), so
	// none is ever captured smaller, and the lookups gate at the same edge.
	if (s.card_size < 8.0) {
		return;
	}
	// A workgroup covers 8x8 texels, one per thread, or with the bounce ray
	// shared 16x16, a 2x2 quad per thread: the quad's one ray keeps every
	// lane of the group tracing, which is what makes the fourfold fewer rays
	// cost a quarter of the time (rays idle in a lane still cost its group).
	// The cards differ in size, so the group finds its card by walking the
	// cards' block counts.
	bool quad_mode = bool(params.flags & FLAG_SHARED_BOUNCE_RAY);
	uint tile = quad_mode ? 16u : 8u;
	uint card = (item >> SURFACE_CACHE_ITEM_CARD_SHIFT) & 7u;
	uint block = item >> SURFACE_CACHE_ITEM_BLOCK_SHIFT;
	uint card_packed = sets.data[set].cards[card];
	ivec2 dims = card_dims_packed(card_packed);
	uvec2 n = max(uvec2(dims) / tile, uvec2(1u));
	if (card >= SURFACE_CACHE_CARDS || block >= n.x * n.y) {
		return;
	}
	set_state.state[set * 2u] = 1u;
	ivec2 block_origin = ivec2(int(block % n.x), int(block / n.x)) * int(tile);
	ivec2 origin_texel = card_origin_packed(card_packed);
	if (lod > 0u && quad_mode) {
		// A coarse relight (plan section 92): the item covers 8 << lod texels
		// square from its top-left tile, and each thread lights one
		// representative texel at the center of its cell of 2^lod, with its
		// own bounce ray and direct term, and stores the result over the
		// cell (SPLAT_STORE). One texel for a cell of four, sixteen or
		// sixty-four: what a tile read only from far away costs, since its
		// readers average the cell through the mip chain anyway. The
		// representative's own history accumulates at its texel and is
		// copied over the cell, so a later relight at a finer level starts
		// every texel from it, and a coarser one replaces the cell's
		// histories with it.
		int stride = 1 << lod;
		ivec2 cell_in_card = block_origin + ivec2(gl_LocalInvocationID.xy) * stride;
		if (any(greaterThanEqual(cell_in_card, dims))) {
			return;
		}
		// The captured texels of the cell, and the representative: the
		// center, or the first captured one when the center is empty.
		splat_side = stride;
		splat_origin = origin_texel + cell_in_card;
		splat_mask = uvec2(0u);
		int center = (stride * stride + stride) / 2; // (stride / 2, stride / 2), row-major.
		int rep = -1;
		for (int i = 0; i < stride * stride; i++) {
			ivec2 c = splat_origin + ivec2(i % stride, i / stride);
			if (texelFetch(depth_atlas, c, 0).r > 0.0) {
				if (i < 32) {
					splat_mask.x |= 1u << uint(i);
				} else {
					splat_mask.y |= 1u << uint(i - 32);
				}
				if (rep < 0 || i == center) {
					rep = i;
				}
			}
		}
		if (rep < 0) {
			return;
		}
		ivec2 texel_in_card = cell_in_card + ivec2(rep % stride, rep / stride);
		ivec2 texel = origin_texel + texel_in_card;
		Texel t;
		if (!read_texel(s, card, dims, texel_in_card, texel, t)) {
			return;
		}
		split_texel_store(0u, t);
		set_state.state[set * 2u + 1u] = 1u;
		// The stamps of the representative's own tile (the item spans up to
		// sixteen, each stamped by the prepare pass as it listed the item),
		// and the previous relight's ray re-traced only when that relight
		// was at this level: at another level the ray left another texel.
		ivec2 tile_texel = origin_texel + (texel_in_card & ~15);
		uint stamp = uint(tile_texel.y >> 3) * TILE_STAMP_STRIDE + uint(tile_texel.x >> 3);
		uint prev_frame = relit.tile_prev[stamp];
		uint prev_lod = (relit.tile_lod[stamp] >> 4u) & 0xFu;
		bool reset = (s.flags & SURFACE_CACHE_SET_FLAG_RESET) != 0u || prev_frame == 0u || prev_frame <= s.captured_frame;
		level_changed = !reset && prev_lod != lod;
		bool have_prev = !reset && !level_changed && prev_frame != 0u && prev_frame < params.frame;
		relit.frame[set * 2u + 1u] = params.frame;
		mark_decay = 0.125 * float(clamp(have_prev ? params.frame - prev_frame : 1u, 1u, 8u));
		gradient_gap = have_prev ? params.frame - prev_frame : 0u;
		ivec2 card_min = origin_texel;
		ivec2 card_max = origin_texel + dims - ivec2(1);
		uint seed = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		uint bounce_seed = pcg_hash(seed ^ 0x9E3779B9u);
		uint prev_seed = pcg_hash(pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(prev_frame))) ^ 0x9E3779B9u);
		Direct d;
		ImageCache images;
		images.valid = false;
		shade_direct(set, t, 0u, seed, images, d);
		float gradient = bounce_gradient_voted(bounce_gradient(texel, t, 0u, prev_seed, have_prev));
		vec3 indirect_sample;
		float bounce_change;
		float bounce_change_total;
		uint bounce_set;
		float bounce_t;
		bool young_dynamic = texel_young_dynamic(texel);
		// The cell's rays: a cell of 2^lod texels square traced a ray per
		// quad as tiles; its one representative traces what lod_rays says
		// (one at every level: the far reads average the cells through the
		// mip chain, and the relights come round oftener within the same
		// budget; measured against the cell's four, section 92).
		uint n_cosine = max(cosine_rays(texel_young(texel), young_dynamic), max((params.lod_rays >> (lod * 8u)) & 0xFFu, 1u));
		float n_light = light_rays(young_dynamic);
		vec3 dyn_sample;
		vec3 dyn2_sample;
		trace_bounce(t, 0u, 0u, bounce_seed, float(n_cosine), n_light, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total, bounce_set, bounce_t);
		trace_bounce_young(n_cosine, n_light, t, 0u, bounce_seed, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total);
		vec3 dyn_light;
		float dyn_landed;
		float dyn_change_total;
		trace_dynamic(texel, t, 0u, float(n_cosine), n_light, dyn_light, dyn_landed, dyn_change_total);
		dyn_sample += dyn_light;
		accumulate(texel, t, reset, d, indirect_sample, bounce_change, bounce_change_total, dyn_change_total, dyn_sample, dyn2_sample, dyn_landed, gradient, bounce_set, bounce_t, card_min, card_max);
		return;
	}
	// The tile's relight before this one, whose bounce ray the gradient
	// re-traces (the tile's own stamp: the tiles take turns, and a set's
	// stamp named a frame most of its tiles were not relit on, so the
	// re-traced ray was another ray and every relight read as a change).
	// Zero, or from before the cards' capture: this is the first relight
	// since, a reset whether or not the flag's one upload reached this
	// frame (the select pass keeps a fresh set urgent until it is relit).
	ivec2 tile_texel = origin_texel + block_origin;
	uint tile_stamp = uint(tile_texel.y >> 3) * TILE_STAMP_STRIDE + uint(tile_texel.x >> 3);
	uint prev_frame = relit.tile_prev[tile_stamp];
	bool reset = (s.flags & SURFACE_CACHE_SET_FLAG_RESET) != 0u || prev_frame == 0u || prev_frame <= s.captured_frame;
	// The previous relight at a coarser level: its stored terms are the
	// representatives' (see level_changed).
	level_changed = !reset && ((relit.tile_lod[tile_stamp] >> 4u) & 0xFu) != 0u;
	ivec2 card_min = origin_texel;
	ivec2 card_max = origin_texel + dims - ivec2(1);
	bool have_prev = !reset && !level_changed && prev_frame != 0u && prev_frame < params.frame;
	relit.frame[set * 2u + 1u] = params.frame;
	mark_decay = 0.125 * float(clamp(have_prev ? params.frame - prev_frame : 1u, 1u, 8u));
	gradient_gap = have_prev ? params.frame - prev_frame : 0u;

	if (!quad_mode) {
		ivec2 texel_in_card = block_origin + ivec2(gl_LocalInvocationID.xy);
		ivec2 texel = origin_texel + texel_in_card;
		Texel t;
		if (!read_texel(s, card, dims, texel_in_card, texel, t)) {
			return;
		}
		split_texel_store(0u, t);
		set_state.state[set * 2u + 1u] = 1u;
		uint seed = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		// The bounce ray's seed is its own, not the direct term's advanced
		// one: the previous relight's ray must be reproducible from its
		// frame alone, without replaying that relight's light sampling.
		uint bounce_seed = pcg_hash(seed ^ 0x9E3779B9u);
		uint prev_seed = pcg_hash(pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(prev_frame))) ^ 0x9E3779B9u);
		Direct d;
		ImageCache images;
		images.valid = false;
		shade_direct(set, t, 0u, seed, images, d);
		float gradient = bounce_gradient_voted(bounce_gradient(texel, t, 0u, prev_seed, have_prev));
		vec3 indirect_sample;
		float bounce_change;
		float bounce_change_total;
		uint bounce_set;
		float bounce_t;
		bool young_dynamic = texel_young_dynamic(texel);
		uint n_cosine = cosine_rays(texel_young(texel), young_dynamic);
		float n_light = light_rays(young_dynamic);
		vec3 dyn_sample;
		vec3 dyn2_sample;
		trace_bounce(t, 0u, 0u, bounce_seed, float(n_cosine), n_light, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total, bounce_set, bounce_t);
		trace_bounce_young(n_cosine, n_light, t, 0u, bounce_seed, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total);
		vec3 dyn_light;
		float dyn_landed;
		float dyn_change_total;
		trace_dynamic(texel, t, 0u, float(n_cosine), n_light, dyn_light, dyn_landed, dyn_change_total);
		dyn_sample += dyn_light;
		accumulate(texel, t, reset, d, indirect_sample, bounce_change, bounce_change_total, dyn_change_total, dyn_sample, dyn2_sample, dyn_landed, gradient, bounce_set, bounce_t, card_min, card_max);
		return;
	}

	ivec2 quad_in_card = block_origin + ivec2(gl_LocalInvocationID.xy) * 2;
	if (any(greaterThanEqual(quad_in_card, dims))) {
		return; // An 8-texel edge fills half of the tile.
	}
	Texel t[4];
	bool valid[4];
	uint valid_count = 0u;
	for (uint k = 0u; k < 4u; k++) {
		ivec2 texel_in_card = quad_in_card + ivec2(int(k & 1u), int(k >> 1u));
		valid[k] = read_texel(s, card, dims, texel_in_card, origin_texel + texel_in_card, t[k]);
		if (valid[k]) {
			split_texel_store(k, t[k]);
			set_state.state[set * 2u + 1u] = 1u;
		}
		valid_count += valid[k] ? 1u : 0u;
	}
	if (valid_count == 0u) {
		return;
	}
	// The ray leaves a different texel of the quad each frame, skipping the
	// uncaptured ones.
	uint tracer = 0u;
	for (uint j = 0u; j < 4u; j++) {
		uint k = (params.frame + j) & 3u;
		if (valid[k]) {
			tracer = k;
			break;
		}
	}
	ivec2 tracer_texel = origin_texel + quad_in_card + ivec2(int(tracer & 1u), int(tracer >> 1u));
	uint seed = pcg_hash(uint(tracer_texel.x) + pcg_hash(uint(tracer_texel.y) + pcg_hash(params.frame)));
	// The previous relight's tracer and seed: the quad's ray left the texel
	// that frame chose, with the seed that frame gave it.
	float gradient = -1.0;
	if (have_prev) {
		uint prev_tracer = 0u;
		for (uint j = 0u; j < 4u; j++) {
			uint k = (prev_frame + j) & 3u;
			if (valid[k]) {
				prev_tracer = k;
				break;
			}
		}
		ivec2 prev_tracer_texel = origin_texel + quad_in_card + ivec2(int(prev_tracer & 1u), int(prev_tracer >> 1u));
		uint prev_seed = pcg_hash(uint(prev_tracer_texel.x) + pcg_hash(uint(prev_tracer_texel.y) + pcg_hash(prev_frame)));
		gradient = bounce_gradient(prev_tracer_texel, t[prev_tracer], prev_tracer, prev_seed, true);
	}
	gradient = bounce_gradient_voted(gradient);
	vec3 indirect_sample;
	float bounce_change;
	float bounce_change_total;
	uint bounce_set;
	float bounce_t;
	bool young_dynamic = texel_young_dynamic(tracer_texel);
	uint n_cosine = cosine_rays(texel_young(tracer_texel), young_dynamic);
	float n_light = light_rays(young_dynamic);
	vec3 dyn_sample;
	vec3 dyn2_sample;
	trace_bounce(t[tracer], 0u, tracer, seed, float(n_cosine), n_light, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total, bounce_set, bounce_t);
	trace_bounce_young(n_cosine, n_light, t[tracer], tracer, seed, indirect_sample, dyn_sample, dyn2_sample, bounce_change, bounce_change_total);
	vec3 dyn_light;
	float dyn_landed;
	float dyn_change_total;
	trace_dynamic(tracer_texel, t[tracer], tracer, float(n_cosine), n_light, dyn_light, dyn_landed, dyn_change_total);
	dyn_sample += dyn_light;
	// The image lights' sum, computed at the quad's first texel and shared.
	ImageCache images;
	images.valid = false;
	for (uint k = 0u; k < 4u; k++) {
		if (!valid[k]) {
			continue;
		}
		ivec2 texel = origin_texel + quad_in_card + ivec2(int(k & 1u), int(k >> 1u));
		uint seed_k = pcg_hash(uint(texel.x) + pcg_hash(uint(texel.y) + pcg_hash(params.frame)));
		Direct d;
		shade_direct(set, t[k], k, seed_k, images, d);
		accumulate(texel, t[k], reset, d, indirect_sample, bounce_change, bounce_change_total, dyn_change_total, dyn_sample, dyn2_sample, dyn_landed, gradient, bounce_set, bounce_t, card_min, card_max);
	}
}

#ifdef SC_SETUP
// The setup kernel: the kernel with its ray points requesting, then the
// requests appended in uniform control flow (a workgroup is covered or not
// whole): one atomic per SIMD group for the count and one for the trace
// dispatch's group count. The header goes out for every thread; the
// resolve reads it wherever it reaches a ray point.
void setup_main() {
	if (gl_WorkGroupID.x == 0u && gl_LocalInvocationIndex == 0u) {
		split_rest_args.groups.x = active_sets.item_count > params.split.x ? active_sets.item_count - params.split.x : 0u;
	}
	if (gl_WorkGroupID.x >= active_sets.item_count || gl_WorkGroupID.x >= params.split.x) {
		return;
	}
	light_main();
	split_headers.data[split_thread] = uvec4(split_shadow_mask, 0u, split_ray_mask, 0u);
	uint n = bitCount(split_shadow_mask) + bitCount(split_ray_mask);
	uint base = subgroupExclusiveAdd(n);
	uint total = subgroupAdd(n);
	if (total == 0u) {
		return;
	}
	uint first = 0u;
	if (subgroupElect()) {
		first = atomicAdd(split_count.count, total);
		atomicMax(split_args.groups.x, (first + total + 63u) / 64u);
	}
	first = subgroupBroadcastFirst(first) + base;
	uint m = split_shadow_mask;
	while (m != 0u) {
		uint b = uint(findLSB(m));
		m &= m - 1u;
		split_requests.data[first++] = split_thread * 64u + b;
	}
	m = split_ray_mask;
	while (m != 0u) {
		uint b = uint(findLSB(m));
		m &= m - 1u;
		split_requests.data[first++] = split_thread * 64u + 32u + b;
	}
}
#endif

#ifdef SC_LANDINGS
// The dynamic lights' shared landings (see trace_dynamic): per light
// dyn_lights.pad1 light rays, their directions an R2 sequence over the
// cone offset per frame and light, each through the holes to the lit card
// face it lands on, stored with the light's radiance there and its area
// density.
void landings_main() {
	uint g = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
	uint m = dyn_lights.pad1;
	uint i = g / max(m, 1u);
	if (m == 0u || i >= dyn_lights.count) {
		return;
	}
	uint s = g - i * m;
	uvec4 d0 = uvec4(0u);
	uvec4 d1 = uvec4(0u);
	LightData ld = dyn_lights.data[i];
	float weight = dyn_lights.weights[i >> 2u][i & 3u];
	if (ld.inv_radius > 0.0 && weight > 0.0) {
		bool is_spot = ld.pad > 0.5;
		vec3 pos = ld.position;
		float range = 1.0 / ld.inv_radius;
		vec3 axis = is_spot ? normalize(ld.direction) : vec3(0.0, 0.0, 1.0);
		uint h = pcg_hash(params.frame * 0x9E3779B9u + i * 0x85EBCA6Bu);
		float r0 = fract(hash_to_float(h) + float(s) * 0.7548776662);
		float r1 = fract(hash_to_float(pcg_hash(h)) + float(s) * 0.5698402910);
		float pdf_omega;
		vec3 dir = dyn_direction(i, ld, is_spot, axis, r0, r1, pdf_omega);
		if (pdf_omega > 0.0) {
			dyn_stat(0u, false);
			float d_lp = 0.0;
			vec3 p = pos;
			vec3 unused_radiance;
			uint hit_set = SURFACE_CACHE_INVALID;
			float unused_change;
			float hit_change_total = 0.0;
			vec3 n_p = axis;
			vec3 albedo_p = vec3(0.0);
			ivec2 texel_p = ivec2(0);
			bool landed_on_card = false;
			rayQueryEXT rq;
			vec3 ray_origin = pos;
			float d_base = 0.0;
			for (uint layer = 0u; layer < 4u; layer++) {
				rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, ray_origin, 0.0, dir, range - d_base);
				while (rayQueryProceedEXT(rq)) {
				}
				if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
					break;
				}
				d_lp = d_base + rayQueryGetIntersectionTEXT(rq, true);
				p = pos + dir * d_lp;
				landed_on_card = card_lookup(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true), p, dir, unused_radiance, hit_set, unused_change, hit_change_total, n_p, albedo_p, texel_p);
				if (landed_on_card || card_reject != 3u) {
					break;
				}
				landed_on_card = false;
				d_base = d_lp + params.ray_bias;
				ray_origin = pos + dir * d_base;
			}
			if (d_lp > 0.0) {
				dyn_stat(1u, false);
			}
			if (landed_on_card) {
				dyn_stat(2u, false);
			}
			float cos_pl = dot(n_p, -dir);
			if (landed_on_card && cos_pl > 0.0 && d_lp >= 1e-4) {
				dyn_stat(3u, false);
				float unused_geom;
				vec3 l_dyn = albedo_p * light_contribution_world(ld, is_spot, pos, axis, p, n_p, unused_geom) * weight;
				float pdf_area = pdf_omega * cos_pl / (d_lp * d_lp);
				d0 = uvec4(floatBitsToUint(p), floatBitsToUint(pdf_area));
				d1 = uvec4(packHalf2x16(l_dyn.rg), packHalf2x16(vec2(l_dyn.b, hit_change_total)), packUnorm2x16(vec3_to_oct(n_p)), 0u);
			}
		}
	}
	dyn_landings.data[g * 2u] = d0;
	dyn_landings.data[g * 2u + 1u] = d1;
}
#endif

#ifdef SC_TRACE
// The trace kernel: one request, one ray (a layered one through holes, and
// a dynamic landing's connection after it), the answer into the header's
// occlusion bits or the ray's record. Each ray is the single kernel's,
// rebuilt from the texel's origin and what the setup kernel stored.
void trace_main() {
	uint i = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
	if (i >= split_count.count) {
		return;
	}
	uint key = split_requests.data[i];
	uint thread = key >> 6u;
	uint code = key & 63u;
	if (code < 32u) {
		uint lights = params.directional_light_count + 1u;
		uint k = code / lights;
		uint light = code % lights;
		vec3 origin = uintBitsToFloat(split_texels.data[thread * 8u + k * 2u].xyz);
		bool occ;
		if (light < params.directional_light_count) {
			vec3 l = normalize(mat3(params.world_from_view) * directional_lights.data[light].direction);
			occ = occluded(origin, l, 1e4, 0xFFu);
		} else {
			uvec4 sel = split_texels.data[thread * 8u + k * 2u + 1u];
			vec3 to_light = uintBitsToFloat(sel.xyz) - origin;
			float dist = length(to_light);
			occ = occluded(origin, to_light / max(dist, 1e-5), max(dist - params.ray_bias, 0.0), sel.w);
		}
		if (occ) {
			atomicOr(split_headers.data[thread].y, 1u << code);
		}
		return;
	}
	uint r = code - 32u;
	uint record = thread * params.split.w + r;
	uvec4 req = split_records.data[record];
	vec3 dir = uintBitsToFloat(req.xyz);
	uvec4 frame = split_texels.data[thread * 8u + req.w * 2u];
	vec3 texel_origin = uintBitsToFloat(frame.xyz);
	bool dynamic = r > params.split.y;
	if (dynamic && dyn_lights.pad1 != 0u) {
		// A shared landing (see trace_dynamic): its connection alone, where
		// the texel and the landing face each other.
		uint j = req.x;
		uvec4 d0 = dyn_landings.data[j * 2u];
		uvec4 answer = uvec4(0u);
		if (uintBitsToFloat(d0.w) > 0.0) {
			vec3 p = uintBitsToFloat(d0.xyz);
			vec3 n_p = oct_to_vec3(unpackUnorm2x16(dyn_landings.data[j * 2u + 1u].z) * 2.0 - 1.0);
			vec3 rel = p - texel_origin;
			float d = length(rel);
			vec3 l = rel / max(d, 1e-4);
			vec3 n = oct_to_vec3(unpackUnorm2x16(frame.w) * 2.0 - 1.0);
			answer.x = SPLIT_HIT;
			if (d >= 1e-4 && dot(n, l) > 0.0 && dot(n_p, -l) > 0.0 && !occluded_opaque(texel_origin, l, max(d - params.ray_bias, 0.0))) {
				answer.x |= SPLIT_CONNECTED;
			}
		}
		split_records.data[record] = answer;
		return;
	}
	vec3 pos = texel_origin;
	float range = 1e4;
	if (dynamic) {
		LightData ld = dyn_lights.data[(r - params.split.y - 1u) / params.split.z];
		pos = ld.position;
		range = 1.0 / ld.inv_radius;
	}
	// The layers, as trace_bounce and trace_dynamic walk them: a hit on a
	// hole goes on from just past it.
	uvec4 answer = uvec4(0u);
	rayQueryEXT rq;
	vec3 ray_origin = pos;
	float t_base = 0.0;
	for (uint layer = 0u; layer < 4u; layer++) {
		rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, ray_origin, 0.0, dir, dynamic ? range - t_base : 1e4);
		while (rayQueryProceedEXT(rq)) {
		}
		if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
			break;
		}
		float t_hit = t_base + rayQueryGetIntersectionTEXT(rq, true);
		uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		if (!card_hole(instance, ray_origin + dir * (t_hit - t_base), dir)) {
			answer = uvec4(SPLIT_HIT, floatBitsToUint(t_hit), instance, 0u);
			break;
		}
		t_base = t_hit + params.ray_bias;
		ray_origin = pos + dir * t_base;
	}
	if (dynamic && answer.x != 0u) {
		// The landing's connection to the texel, where the texel faces it
		// (the resolve tests the landing's side, which needs its card).
		vec3 p = pos + dir * uintBitsToFloat(answer.y);
		vec3 rel = p - texel_origin;
		float d = length(rel);
		vec3 l = rel / max(d, 1e-4);
		vec3 n = oct_to_vec3(unpackUnorm2x16(frame.w) * 2.0 - 1.0);
		if (d >= 1e-4 && dot(n, l) > 0.0 && !occluded_opaque(texel_origin, l, max(d - params.ray_bias, 0.0))) {
			answer.x |= SPLIT_CONNECTED;
		}
	}
	split_records.data[record] = answer;
}
#endif

void main() {
#if defined(SC_SETUP)
	setup_main();
#elif defined(SC_TRACE)
	trace_main();
#elif defined(SC_LANDINGS)
	landings_main();
#else
	light_main();
#endif
}
