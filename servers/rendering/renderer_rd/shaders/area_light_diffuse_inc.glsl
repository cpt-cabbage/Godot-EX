// An area light's diffuse term at a world-space point, for the passes that
// light the surface cache's cards and the deferred hit shading: the LTC
// integral of the rect's clamped cosine (the diffuse core of the stochastic
// direct pass's area_light_eval, the same attenuation and the same filtered
// texture), in the convention of the local lights' terms around it (the
// colour times the geometric factor over pi, with the LTC integral standing
// in for the cosine). One-sided, like the direct pass. The light is in view
// space as the light buffers hold it, placed in the world by world_from_view.
//
// Expects light_data_inc.glsl and get_omni_attenuation() before it; the
// includer's M_PI must be area_lights_inc.glsl's.

#include "area_lights_inc.glsl"

// r_geom is the colourless factor (the LTC integral times the attenuation),
// for the geometric sums the card lighting tracks; r_point a point on the
// rect, uniform in it by xi, for the shadow ray.
vec3 area_light_contribution(LightData ld, mat4 world_from_view, vec3 world_pos, vec3 n, vec2 xi, texture2D area_light_atlas, sampler area_light_sampler, out float r_geom, out vec3 r_point) {
	r_geom = 0.0;
	r_point = world_pos;
	vec3 area_width = mat3(world_from_view) * ld.area_width;
	vec3 area_height = mat3(world_from_view) * ld.area_height;
	if (dot(area_width, area_width) < 1e-7 || dot(area_height, area_height) < 1e-7) {
		return vec3(0.0);
	}
	vec3 light_pos = (world_from_view * vec4(ld.position, 1.0)).xyz;
	vec3 light_dir = normalize(mat3(world_from_view) * ld.direction);
	r_point = light_pos + area_width * (xi.x - 0.5) + area_height * (xi.y - 0.5);
	vec3 light_to_vert = world_pos - light_pos;
	if (dot(light_dir, light_to_vert) <= 0.0) {
		return vec3(0.0); // Behind the light.
	}

	// Attenuation from the closest point on the rect; the LTC integral already
	// falls off with inverse-square solid angle, so that part is compensated.
	vec3 a_dir = normalize(area_width);
	vec3 b_dir = normalize(area_height);
	float a_half = length(area_width) * 0.5;
	float b_half = length(area_height) * 0.5;
	vec3 pos_local = vec3(dot(light_to_vert, a_dir), dot(light_to_vert, b_dir), dot(light_to_vert, -light_dir));
	vec3 closest_local = vec3(clamp(pos_local.x, -a_half, a_half), clamp(pos_local.y, -b_half, b_half), 0.0);
	float dist = length(closest_local - pos_local);
	if (dist * ld.inv_radius >= 1.0) {
		return vec3(0.0); // Out of range.
	}
	float att_ltc = get_omni_attenuation(dist, ld.inv_radius, ld.attenuation) * dist * dist;
	if (att_ltc <= 0.0) {
		return vec3(0.0);
	}

	vec3 points[4];
	vec3 hw = area_width * 0.5;
	vec3 hh = area_height * 0.5;
	points[0] = light_pos - hw - hh - world_pos;
	points[1] = light_pos + hw - hh - world_pos;
	points[2] = light_pos + hw + hh - world_pos;
	points[3] = light_pos - hw + hh - world_pos;
	// The whole rect below the texel's horizon: nothing to integrate (the
	// clip inside the LTC would find the same, after the frame and the
	// texture's form factor).
	if (max(max(dot(points[0], n), dot(points[1], n)), max(dot(points[2], n), dot(points[3], n))) <= 0.0) {
		return vec3(0.0);
	}

	float ltc_diffuse = 0.0;
	vec3 tex_color = vec3(1.0);
	ltc_evaluate_diff(n, points, ld.projector_rect, ld.cone_angle, area_light_atlas, area_light_sampler, ltc_diffuse, tex_color);
	// ltc_evaluate_diff leaves out the 1 / (2 pi) ltc_evaluate applies.
	r_geom = ltc_diffuse * (1.0 / (2.0 * M_PI)) * att_ltc;
	return ld.color * tex_color * r_geom;
}
