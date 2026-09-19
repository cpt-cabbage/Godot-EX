#[compute]

#version 450

#VERSION_DEFINES

// The rough reflection's spatial resolve:
// before the temporal pass, a pixel's one GGX sample is replaced by the
// lobe-weighted average of its neighbourhood's rays. Each neighbour's ray
// was drawn from that neighbour's lobe with a known density and hit a
// point; the direction from this pixel to that point, evaluated under this
// pixel's lobe, over the density the ray was drawn with, is the weight
// that turns the neighbourhood's hits into samples of this lobe
// (Stachowiak 2015's ray reuse; the neighbour's own direction stood in
// first and read worse than no resolve at all on a strafe past a lit wall,
// a hit a meter away being a different direction from a pixel ten pixels
// over), with depth and normal stops so a different surface's rays stay
// out. What the temporal pass then
// accumulates -- and restarts to, under a lighting change -- is twenty-five
// rays' worth, never one: the mark's restart of the reflection was the
// sparkle on every glossy wall after a flashlight move (section 27). A
// mirror keeps its own sample: its lobe admits no neighbour's ray, and the
// resolve would only blur its image.

#include "../normal_roughness_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// rgb the ray's radiance, a the virtual view depth (kept as the pixel's own).
layout(set = 0, binding = 0) uniform sampler2D raw_reflection;
// xy the ray's direction in view space (octahedral), z its sampling
// density (0: no rough ray; negated: the pixel's diffuse ray at the cosine
// density, standing in for a reflection ray -- the gather's shared form),
// w the hit distance (1e4 and above: a miss).
layout(set = 0, binding = 1) uniform sampler2D spec_ray;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_texture;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_reflection;

layout(push_constant, std430) uniform Params {
	mat4 view_from_ndc;
	ivec2 screen_size;
	int depth_scale; // 2 when the reflection buffers are half resolution.
	int radius; // Taps from -radius to radius on each axis.
	float rough_min; // Below this roughness the sample passes through (the mirror path).
	float rough_full; // From this roughness the resolve replaces the sample whole.
	float weight_cap; // The most a neighbour's density ratio may weigh.
	int fill; // 1: the half-rate form -- only a pixel without a ray of its own is resolved, from the neighbors that traced; the rest pass through. 3: the shared form -- only a pixel whose own ray is its diffuse ray is resolved, that ray and the neighbours' reweighted into its lobe; the rest pass through.
}
params;

#define M_PI 3.14159265359

// The gather's octahedron_encode, undone.
vec3 oct_decode(vec2 f) {
	f = f * 2.0 - 1.0;
	vec3 n = vec3(f.xy, 1.0 - abs(f.x) - abs(f.y));
	float t = max(-n.z, 0.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

vec3 view_position(ivec2 pixel, float depth) {
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 p = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return p.xyz / p.w;
}

// The density the gather draws its rough ray with: GGX over the half
// vector (stochastic_indirect_gi.glsl), turned to a density over
// directions.
float ggx_dir_pdf(vec3 n, vec3 v, vec3 dir, float alpha) {
	vec3 h = normalize(v + dir);
	float ndh = dot(n, h);
	float vdh = dot(v, h);
	if (ndh <= 0.0 || vdh <= 1e-4) {
		return 0.0;
	}
	float a2 = alpha * alpha;
	float d = ndh * ndh * (a2 - 1.0) + 1.0;
	float D = a2 / (M_PI * d * d);
	return D * ndh / (4.0 * vdh);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}
	vec4 center = texelFetch(raw_reflection, pixel, 0);
	float depth = texelFetch(depth_texture, pixel * params.depth_scale, 0).r;
	vec4 nr = texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0);
	float roughness = nr_roughness(nr);
	vec4 own_ray = texelFetch(spec_ray, pixel, 0);
	// The half-rate fill: a pixel with a ray passes through, one without
	// (a rough pixel the gather skipped this frame) takes the plain mean of
	// the four neighbors on its surface (the depth and normal stops; the
	// gather skipped it only because one exists). Their lobes are its own
	// -- the same roughness, a view vector a pixel apart -- so the density
	// ratio below has nothing to correct, and what it did was trim: a
	// neighbour's hit a hand away is a different direction from here and
	// weighed the bright near hits down (the filled set read 20% dimmer
	// than the traced set on the TPS bridge). A mirror (at or below
	// rough_min) traced its own ray and is never filled.
	bool sharing = params.fill == 3;
	bool filling = params.fill != 0 && !sharing;
	bool paint = params.fill == 2; // Diagnostics (GODOT_GI_SPEC_FILL_PAINT=1): why each pixel got what it got.
	// The shared form resolves the pixels whose own ray is their diffuse
	// ray (the density negated) and passes the rest through, GGX ray or
	// none; the fill resolves the pixels without a ray from those with.
	bool skip = sharing ? own_ray.z >= 0.0 : ((own_ray.z == 0.0) != filling);
	if (depth == 0.0 || (filling ? roughness <= params.rough_min : roughness < params.rough_min) || skip) {
		imageStore(out_reflection, pixel, paint ? (depth == 0.0 ? vec4(0.0) : (roughness <= params.rough_min ? vec4(1.0, 1.0, 0.0, 0.0) : vec4(0.0, 0.0, 1.0, 0.0))) : center);
		return;
	}
	// Decoded as the neighbours' are: the buffer went octahedral three days
	// after this pass was written (4c106eb895) and the pixel's own normal
	// stayed on the raw read, so every neighbor failed the normal stop and
	// the pass was a pass-through until the half-rate fill found it.
	vec3 n = nr_normal(nr);
	vec3 pos = view_position(pixel, depth);
	vec3 v = -normalize(pos);
	float alpha = roughness * roughness;
	float view_depth = -pos.z;

	// The pixel's own sample first, at the weight its own density gives it:
	// one for a GGX ray; for its diffuse ray, the lobe's density at the
	// direction over the cosine density, as for any neighbour's.
	vec3 sum = center.rgb;
	float weight = 1.0;
	if (own_ray.z < 0.0) {
		vec3 own_dir = oct_decode(own_ray.xy);
		weight = min(ggx_dir_pdf(n, v, own_dir, alpha) / -own_ray.z, params.weight_cap);
		sum = center.rgb * weight;
	}
	vec4 plain_sum = vec4(0.0);
	float plain_weight = 0.0;
	for (int y = -params.radius; y <= params.radius; y++) {
		for (int x = -params.radius; x <= params.radius; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 sp = pixel + ivec2(x, y);
			if (any(lessThan(sp, ivec2(0))) || any(greaterThanEqual(sp, params.screen_size))) {
				continue;
			}
			float sd = texelFetch(depth_texture, sp * params.depth_scale, 0).r;
			if (sd == 0.0) {
				continue;
			}
			vec4 ray = texelFetch(spec_ray, sp, 0);
			if (ray.z == 0.0) {
				continue;
			}
			ray.z = abs(ray.z); // A diffuse ray's density, negated to mark it, weighs the same way.
			vec3 spos = view_position(sp, sd);
			if (abs(-spos.z - view_depth) > 0.05 * max(view_depth, 1.0)) {
				continue;
			}
			vec3 sn = nr_normal(texelFetch(normal_roughness_texture, sp * params.depth_scale, 0));
			float w_normal = pow(max(dot(n, sn), 0.0), 32.0);
			if (w_normal <= 1e-3) {
				continue;
			}
			if (filling) {
				plain_sum += texelFetch(raw_reflection, sp, 0) * w_normal;
				plain_weight += w_normal;
				continue;
			}
			vec3 ray_dir = oct_decode(ray.xy);
			// The neighbour's hit, seen from this pixel (a miss keeps the
			// ray's direction: the sky is at infinity for both).
			vec3 dir = ray.w >= 1e4 ? ray_dir : normalize(spos + ray_dir * ray.w - pos);
			if (dot(dir, n) <= 1e-4) {
				continue;
			}
			// This lobe's density at that direction over the density the
			// ray was drawn with: the reuse weight.
			float w = min(ggx_dir_pdf(n, v, dir, alpha) / ray.z, params.weight_cap) * w_normal;
			sum += texelFetch(raw_reflection, sp, 0).rgb * w;
			weight += w;
		}
	}
	if (filling) {
		imageStore(out_reflection, pixel, paint ? (plain_weight > 0.0 ? vec4(1.0, 0.0, 0.0, 0.0) : vec4(0.0, 1.0, 0.0, 0.0)) : (plain_weight > 0.0 ? plain_sum / plain_weight : center));
		return;
	}
	if (weight <= 0.0) {
		// Not one ray of the neighbourhood lies in the lobe (a shared ray
		// at a grazing view, nothing traced nearby): the pixel's own sample
		// rather than nothing.
		imageStore(out_reflection, pixel, center);
		return;
	}
	vec3 resolved = sum / weight;
	float blend = sharing ? 1.0 : smoothstep(params.rough_min, params.rough_full, roughness);
	imageStore(out_reflection, pixel, vec4(mix(center.rgb, resolved, blend), center.a));
}
