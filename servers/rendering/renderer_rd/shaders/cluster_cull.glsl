#[compute]

#version 450

#VERSION_DEFINES

// The cluster bake as a compute cull: a workgroup per screen tile, a thread
// per depth slice, every froxel tested against every element. It writes the
// same buffer the proxy rasterisation (cluster_render.glsl) does -- a used
// bit per element per tile, and the 32 depth bits per element per tile --
// so cluster_store.glsl packs it unchanged. The rasterisation's cost grew
// with the proxies' screen area (5 -> 53 ms from 40 to 500 lights at 1080p
// on an Apple M4); this is a fixed tiles x slices x elements loop.
//
// The tests are exact for a sphere and conservative for the cone (the
// cone against the froxel's bounding sphere, and the cone's bounding sphere
// against the froxel) and the box (the six face axes of the separating axis
// test), so a froxel never loses an element that touches it.

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 1, std140) uniform State {
	mat4 projection;

	float inv_z_far;
	uint screen_to_clusters_shift;
	uint cluster_screen_width;
	uint cluster_data_size;

	uint cluster_depth_offset;
	uint pad0;
	uint pad1;
	uint pad2;

	// The compute cull's own fields.
	mat4 inv_projection;
	vec2 screen_size; // The cluster grid's screen, in pixels.
	uint cluster_size; // Pixels per tile edge.
	uint camera_orthogonal;
	float z_far;
	uint cluster_screen_height;
	uint render_element_count;
	float log_z0; // The exponential slices' first bound (see cluster_render_log).
}
state;

struct RenderElement {
	uint type; // 0 omni, 1 spot, 2 area, 3 decal, 4 reflection probe.
	bool touches_near;
	bool touches_far;
	uint original_index;
	mat3x4 transform_inv; // The proxy's view-space placement (rotation and origin); vec4(p, 1) * m.
	vec3 scale; // The proxy's half extents (a sphere: its radius; a cone: base radius, base radius, height).
	uint has_wide_spot_angle; // A spot light drawn as a sphere.
};

layout(set = 0, binding = 2, std430) buffer restrict readonly RenderElements {
	RenderElement data[];
}
render_elements;

layout(set = 0, binding = 3, std430) buffer restrict ClusterRender {
	uint data[];
}
cluster_render;

// The same tile buffer with exponential depth slices, for the stochastic
// sampling pass: slice s from log_z0 * (z_far / log_z0)^(s / 32) to the next
// bound, slice 0 from the camera.
layout(set = 0, binding = 4, std430) buffer restrict ClusterRenderLog {
	uint data[];
}
cluster_render_log;

#define CHUNK 256u
shared uint masks[CHUNK];
shared uint masks_log[CHUNK];

vec3 unproject(vec2 ndc, float depth01) {
	vec4 p = state.inv_projection * vec4(ndc, depth01, 1.0);
	return p.xyz / p.w;
}

vec3 place(mat3x4 m, vec3 p) {
	return vec4(p, 1.0) * m;
}

// The froxel as six planes (normal, offset), normals inward: a point is
// inside where dot(n, p) + d >= 0 on all six. A box around a froxel is no
// use: a slice is z_far / 32 deep and its pyramid's box is nearly the whole
// tile frustum, so the tests are against the planes, as the proxies'
// rasterisation was against the tile's silhouette.
bool sphere_frustum(vec3 c, float r, vec4 planes[6]) {
	for (uint i = 0u; i < 6u; i++) {
		if (dot(planes[i].xyz, c) + planes[i].w < -r) {
			return false;
		}
	}
	return true;
}

// Cone (apex, unit direction, height, base radius): behind a plane when its
// apex and its farthest base point toward the plane both are.
bool cone_frustum(vec3 apex, vec3 dir, float height, float base_radius, vec4 planes[6]) {
	for (uint i = 0u; i < 6u; i++) {
		vec3 n = planes[i].xyz;
		float da = dot(n, apex) + planes[i].w;
		if (da >= 0.0) {
			continue;
		}
		vec3 m = n - dir * dot(n, dir);
		float ml = length(m);
		m = ml > 1e-6 ? m / ml : vec3(0.0);
		vec3 q = apex + dir * height + m * base_radius;
		if (dot(n, q) + planes[i].w < 0.0) {
			return false;
		}
	}
	return true;
}

// Oriented box (centre, unit axes, half extents): behind a plane when its
// centre is farther behind than its projected radius.
bool obb_frustum(vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, vec4 planes[6]) {
	for (uint i = 0u; i < 6u; i++) {
		vec3 n = planes[i].xyz;
		float r = h.x * abs(dot(n, ax)) + h.y * abs(dot(n, ay)) + h.z * abs(dot(n, az));
		if (dot(n, c) + planes[i].w < -r) {
			return false;
		}
	}
	return true;
}

bool intersects(RenderElement e, vec4 planes[6]) {
	vec3 origin = place(e.transform_inv, vec3(0.0));
	if (e.type == 0u || (e.type == 1u && e.has_wide_spot_angle != 0u)) {
		return sphere_frustum(origin, e.scale.x, planes);
	}
	if (e.type == 1u) {
		// Apex at the origin, base at -z: height scale.z, base radius scale.x.
		vec3 dir = normalize(place(e.transform_inv, vec3(0.0, 0.0, -1.0)) - origin);
		return cone_frustum(origin, dir, e.scale.z, e.scale.x, planes);
	}
	vec3 ax = place(e.transform_inv, vec3(1.0, 0.0, 0.0)) - origin;
	vec3 ay = place(e.transform_inv, vec3(0.0, 1.0, 0.0)) - origin;
	vec3 az = place(e.transform_inv, vec3(0.0, 0.0, 1.0)) - origin;
	return obb_frustum(origin, ax, ay, az, e.scale, planes);
}

vec4 plane_through(vec3 a, vec3 b, vec3 c, vec3 inside) {
	vec3 n = normalize(cross(b - a, c - a));
	if (dot(n, inside - a) < 0.0) {
		n = -n;
	}
	return vec4(n, -dot(n, a));
}

void main() {
	uvec2 tile = gl_WorkGroupID.xy;
	uint slice = gl_LocalInvocationID.x;
	if (tile.x >= state.cluster_screen_width || tile.y >= state.cluster_screen_height) {
		return;
	}

	// The tile's four corner lines through view space, near point to far
	// point, then this slice's froxel as six planes: the four sides through
	// the corner lines and the two depths (linear in view depth, as the depth
	// bits are).
	vec2 px0 = vec2(tile * state.cluster_size);
	vec2 px1 = min(px0 + vec2(state.cluster_size), state.screen_size);
	vec2 ndc0 = px0 / state.screen_size * 2.0 - 1.0;
	vec2 ndc1 = px1 / state.screen_size * 2.0 - 1.0;
	vec2 corners[4] = vec2[4](ndc0, vec2(ndc1.x, ndc0.y), ndc1, vec2(ndc0.x, ndc1.y)); // Around the tile.
	float d0 = float(slice) / 32.0 * state.z_far;
	float d1 = float(slice + 1u) / 32.0 * state.z_far;
	vec3 near_p[4];
	vec3 far_p[4];
	vec3 centre = vec3(0.0);
	for (uint i = 0u; i < 4u; i++) {
		vec3 a = unproject(corners[i], 0.0);
		vec3 b = unproject(corners[i], 1.0);
		if (-a.z > -b.z) {
			vec3 t = a;
			a = b;
			b = t;
		}
		near_p[i] = a;
		far_p[i] = b;
		float da = -a.z;
		float db = -b.z;
		float inv = 1.0 / max(db - da, 1e-6);
		centre += a + (b - a) * (((d0 + d1) * 0.5 - da) * inv);
	}
	centre *= 0.25;
	vec4 planes[6];
	for (uint i = 0u; i < 4u; i++) {
		uint k = (i + 1u) & 3u;
		planes[i] = plane_through(near_p[i], near_p[k], far_p[i], centre);
	}
	planes[4] = vec4(0.0, 0.0, 1.0, d1); // z >= -d1
	planes[5] = vec4(0.0, 0.0, -1.0, -d0); // z <= -d0
	float log_ratio = log(state.z_far / state.log_z0) / 32.0;
	float l0 = slice == 0u ? 0.0 : state.log_z0 * exp(float(slice) * log_ratio);
	float l1 = state.log_z0 * exp(float(slice + 1u) * log_ratio);
	vec4 planes_log[6] = planes;
	planes_log[4] = vec4(0.0, 0.0, 1.0, l1);
	planes_log[5] = vec4(0.0, 0.0, -1.0, -l0);

	uint cluster_offset = (tile.x + state.cluster_screen_width * tile.y) * state.cluster_data_size;
	uint count = state.render_element_count;
	for (uint chunk = 0u; chunk < count; chunk += CHUNK) {
		for (uint i = slice; i < CHUNK; i += 32u) {
			masks[i] = 0u;
			masks_log[i] = 0u;
		}
		barrier();
		uint chunk_count = min(CHUNK, count - chunk);
		for (uint j = 0u; j < chunk_count; j++) {
			RenderElement e = render_elements.data[chunk + j];
			if (intersects(e, planes)) {
				atomicOr(masks[j], 1u << slice);
			}
			if (intersects(e, planes_log)) {
				atomicOr(masks_log[j], 1u << slice);
			}
		}
		barrier();
		for (uint j = slice; j < chunk_count; j += 32u) {
			uint index = chunk + j;
			uint m = masks[j];
			if (m != 0u) {
				atomicOr(cluster_render.data[cluster_offset + (index >> 5u)], 1u << (index & 31u));
				cluster_render.data[cluster_offset + state.cluster_depth_offset + index] = m;
			}
			uint ml = masks_log[j];
			if (ml != 0u) {
				atomicOr(cluster_render_log.data[cluster_offset + (index >> 5u)], 1u << (index & 31u));
				cluster_render_log.data[cluster_offset + state.cluster_depth_offset + index] = ml;
			}
		}
		barrier();
	}
}
