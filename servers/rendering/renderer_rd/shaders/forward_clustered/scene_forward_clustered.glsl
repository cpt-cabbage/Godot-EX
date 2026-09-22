#[vertex]

#version 460

#VERSION_DEFINES

/* Include half precision types. */
#include "../half_inc.glsl"
#include "scene_forward_clustered_inc.glsl"

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0

#ifdef USE_MULTIVIEW
#define OUTPUT_IS_MULTIVIEW true
#else
#define OUTPUT_IS_MULTIVIEW false
#endif

/* INPUT ATTRIBS */

// Always contains vertex position in XYZ, can contain tangent angle in W.
layout(location = 0) in vec4 vertex_angle_attrib;

//only for pure render depth when normal is not used

#if defined(NORMAL_USED) || defined(TANGENT_USED)
// Contains Normal/Axis in RG, can contain tangent in BA.
layout(location = 1) in vec4 axis_tangent_attrib;
#endif

// Location 2 is unused.

#if defined(COLOR_USED)
layout(location = 3) in vec4 color_attrib;
#endif

#ifdef UV_USED
layout(location = 4) in vec2 uv_attrib;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP) || defined(MODE_RENDER_MATERIAL)
layout(location = 5) in vec2 uv2_attrib;
#endif

#if defined(CUSTOM0_USED)
layout(location = 6) in vec4 custom0_attrib;
#endif

#if defined(CUSTOM1_USED)
layout(location = 7) in vec4 custom1_attrib;
#endif

#if defined(CUSTOM2_USED)
layout(location = 8) in vec4 custom2_attrib;
#endif

#if defined(CUSTOM3_USED)
layout(location = 9) in vec4 custom3_attrib;
#endif

#if defined(BONES_USED) || defined(USE_PARTICLE_TRAILS)
layout(location = 10) in uvec4 bone_attrib;
#endif

#if defined(WEIGHTS_USED) || defined(USE_PARTICLE_TRAILS)
layout(location = 11) in vec4 weight_attrib;
#endif

#ifdef MOTION_VECTORS
layout(location = 12) in vec4 previous_vertex_attrib;

#if defined(NORMAL_USED) || defined(TANGENT_USED)
layout(location = 13) in vec4 previous_normal_attrib;
#endif

#endif // MOTION_VECTORS

void axis_angle_to_tbn(vec3 axis, float angle, out vec3 tangent, out vec3 binormal, out vec3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	vec3 omc_axis = (1.0 - c) * axis;
	vec3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + vec3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
}

/* Varyings */

layout(location = 0) out vec3 vertex_interp;

#ifdef NORMAL_USED
layout(location = 1) out vec3 normal_interp;
#endif

#if defined(COLOR_USED)
layout(location = 2) out vec4 color_interp;
#endif

#ifdef UV_USED
layout(location = 3) out vec2 uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
layout(location = 4) out vec2 uv2_interp;
#endif

#ifdef TANGENT_USED
layout(location = 5) out vec3 tangent_interp;
layout(location = 6) out vec3 binormal_interp;
#endif

#ifdef MOTION_VECTORS
layout(location = 7) out vec4 screen_position;
layout(location = 8) out vec4 prev_screen_position;
#endif

#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = MATERIAL_UNIFORM_SET, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

float global_time;

#ifdef MODE_DUAL_PARABOLOID

layout(location = 9) out float dp_clip;

#endif

layout(location = 10) out flat uint instance_index_interp;

#ifdef USE_MULTIVIEW
#extension GL_EXT_multiview : enable
#define ViewIndex gl_ViewIndex

vec3 multiview_uv(vec2 uv) {
	return vec3(uv, ViewIndex);
}
ivec3 multiview_uv(ivec2 uv) {
	return ivec3(uv, int(ViewIndex));
}
layout(location = 11) out vec4 combined_projected;
#else // USE_MULTIVIEW
#define ViewIndex 0
vec2 multiview_uv(vec2 uv) {
	return uv;
}
ivec2 multiview_uv(ivec2 uv) {
	return uv;
}
#endif //USE_MULTIVIEW

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED) && defined(USE_VERTEX_LIGHTING)
layout(location = 12) out vec4 diffuse_light_interp;
layout(location = 13) out vec4 specular_light_interp;

#include "../scene_forward_vertex_lights_inc.glsl"

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;

	item_from = item_min >> 5;
	item_to = (item_max == 0) ? 0 : ((item_max - 1) >> 5) + 1; //side effect of how it is stored, as item_max 0 means no elements
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}
#endif // !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED) && defined(USE_VERTEX_LIGHTING)

#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
layout(location = 14) out vec2 point_coord_interp;
#endif

invariant gl_Position;

#GLOBALS

#ifdef USE_DOUBLE_PRECISION
// Helper functions for emulating double precision when adding floats.
vec3 quick_two_sum(vec3 a, vec3 b, out vec3 out_p) {
	vec3 s = a + b;
	out_p = b - (s - a);
	return s;
}

vec3 two_sum(vec3 a, vec3 b, out vec3 out_p) {
	vec3 s = a + b;
	vec3 v = s - a;
	out_p = (a - (s - v)) + (b - v);
	return s;
}

vec3 double_add_vec3(vec3 base_a, vec3 prec_a, vec3 base_b, vec3 prec_b, out vec3 out_precision) {
	vec3 s, t, se, te;
	s = two_sum(base_a, base_b, se);
	t = two_sum(prec_a, prec_b, te);
	se += t;
	s = quick_two_sum(s, se, se);
	se += te;
	s = quick_two_sum(s, se, out_precision);
	return s;
}
#endif

uint multimesh_stride() {
	uint stride = sc_multimesh_format_2d() ? 2 : 3;
	stride += sc_multimesh_has_color() ? 1 : 0;
	stride += sc_multimesh_has_custom_data() ? 1 : 0;
	return stride;
}

void vertex_shader(vec3 vertex_input,
#ifdef NORMAL_USED
		in vec3 normal_input,
#endif
#ifdef TANGENT_USED
		in vec3 tangent_input,
		in vec3 binormal_input,
#endif
		in uint instance_index, in uint multimesh_offset, in SceneData scene_data, in mat3x4 in_model_matrix,
#ifdef USE_DOUBLE_PRECISION
		in vec3 model_precision,
#endif
		out vec4 screen_pos) {
	vec4 instance_custom = vec4(0.0);
#if defined(COLOR_USED)
	color_interp = color_attrib;
#endif

	mat4 inv_view_matrix = transpose(mat4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	mat4 model_matrix = transpose(mat4(in_model_matrix[0],
			in_model_matrix[1],
			in_model_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef USE_DOUBLE_PRECISION
	vec3 view_precision = scene_data.inv_view_precision.xyz;
#endif

	mat3 model_normal_matrix;
	if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(inverse(mat3(model_matrix)));
	} else {
		model_normal_matrix = mat3(model_matrix);
	}

	mat4 matrix;
	mat4 read_model_matrix = model_matrix;

	if (sc_multimesh()) {
		//multimesh, instances are for it

#ifdef USE_PARTICLE_TRAILS
		uint trail_size = (instances.data[instance_index].flags >> INSTANCE_FLAGS_PARTICLE_TRAIL_SHIFT) & INSTANCE_FLAGS_PARTICLE_TRAIL_MASK;
		uint stride = 3 + 1 + 1; //particles always uses this format

		uint offset = trail_size * stride * INSTANCE_INDEX;

#ifdef COLOR_USED
		vec4 pcolor;
#endif
		{
			uint boffset = offset + bone_attrib.x * stride;
			matrix = mat4(transforms.data[boffset + 0], transforms.data[boffset + 1], transforms.data[boffset + 2], vec4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.x;
#ifdef COLOR_USED
			pcolor = transforms.data[boffset + 3] * weight_attrib.x;
#endif
		}
		if (weight_attrib.y > 0.001) {
			uint boffset = offset + bone_attrib.y * stride;
			matrix += mat4(transforms.data[boffset + 0], transforms.data[boffset + 1], transforms.data[boffset + 2], vec4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.y;
#ifdef COLOR_USED
			pcolor += transforms.data[boffset + 3] * weight_attrib.y;
#endif
		}
		if (weight_attrib.z > 0.001) {
			uint boffset = offset + bone_attrib.z * stride;
			matrix += mat4(transforms.data[boffset + 0], transforms.data[boffset + 1], transforms.data[boffset + 2], vec4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.z;
#ifdef COLOR_USED
			pcolor += transforms.data[boffset + 3] * weight_attrib.z;
#endif
		}
		if (weight_attrib.w > 0.001) {
			uint boffset = offset + bone_attrib.w * stride;
			matrix += mat4(transforms.data[boffset + 0], transforms.data[boffset + 1], transforms.data[boffset + 2], vec4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.w;
#ifdef COLOR_USED
			pcolor += transforms.data[boffset + 3] * weight_attrib.w;
#endif
		}

		instance_custom = transforms.data[offset + 4];

#ifdef COLOR_USED
		color_interp *= pcolor;
#endif

#else
		uint stride = multimesh_stride();
		uint offset = stride * (INSTANCE_INDEX + multimesh_offset);

		if (sc_multimesh_format_2d()) {
			matrix = mat4(transforms.data[offset + 0], transforms.data[offset + 1], vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
			offset += 2;
		} else {
			matrix = mat4(transforms.data[offset + 0], transforms.data[offset + 1], transforms.data[offset + 2], vec4(0.0, 0.0, 0.0, 1.0));
			offset += 3;
		}

		if (sc_multimesh_has_color()) {
#ifdef COLOR_USED
			color_interp *= transforms.data[offset];
#endif
			offset += 1;
		}

		if (sc_multimesh_has_custom_data()) {
			instance_custom = transforms.data[offset];
		}

#endif
		//transpose
		matrix = transpose(matrix);
#if !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED) || defined(MODEL_MATRIX_USED)
		// Normally we can bake the multimesh transform into the model matrix, but when using double precision
		// we avoid baking it in so we can emulate high precision.
		read_model_matrix = model_matrix * matrix;
#if !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED)
		model_matrix = read_model_matrix;
#endif // !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED)
#endif // !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED) || defined(MODEL_MATRIX_USED)
		model_normal_matrix = model_normal_matrix * mat3(matrix);
	}

	vec3 vertex = vertex_input;
#ifdef NORMAL_USED
	vec3 normal_highp = normal_input;
#endif

#ifdef TANGENT_USED
	vec3 tangent = tangent_input;
	vec3 binormal = binormal_input;
#endif

#ifdef UV_USED
	uv_interp = uv_attrib;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	uv2_interp = uv2_attrib;
#endif

	vec4 uv_scale = instances.data[instance_index].uv_scale;

	if (uv_scale != vec4(0.0)) { // Compression enabled
#ifdef UV_USED
		uv_interp = (uv_interp - 0.5) * uv_scale.xy;
#endif
#if defined(UV2_USED) || defined(USE_LIGHTMAP)
		uv2_interp = (uv2_interp - 0.5) * uv_scale.zw;
#endif
	}

#ifdef OVERRIDE_POSITION
	vec4 position = vec4(1.0);
#endif

#ifdef USE_MULTIVIEW
	mat4 combined_projection = scene_data.projection_matrix;
	mat4 projection_matrix = scene_data.projection_matrix_view[ViewIndex];
	mat4 inv_projection_matrix = scene_data.inv_projection_matrix_view[ViewIndex];
	vec3 eye_offset = scene_data.eye_offset[ViewIndex].xyz;
#else
	mat4 projection_matrix = scene_data.projection_matrix;
	mat4 inv_projection_matrix = scene_data.inv_projection_matrix;
	vec3 eye_offset = vec3(0.0, 0.0, 0.0);
#endif //USE_MULTIVIEW

//using world coordinates
#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)

	vertex = (model_matrix * vec4(vertex, 1.0)).xyz;

#ifdef NORMAL_USED
	// For correct non-uniform scale handling, normal has to be transformed by normal matrix, but tangent vectors need to use model matrix as is
	normal_highp = model_normal_matrix * normal_highp;
#endif

#ifdef TANGENT_USED
	// For non-uniform scale, this produces non-orthogonal TBNs; ideally binormal should be reconstructed in fragment shader with cross
	tangent = mat3(model_matrix) * tangent;
	binormal = mat3(model_matrix) * binormal;
#endif
#endif

#ifdef Z_CLIP_SCALE_USED
	float z_clip_scale = 1.0;
#endif

	float roughness_highp = 1.0;

	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef USE_DOUBLE_PRECISION
	mat4 modelview = read_view_matrix * model_matrix;

	// We separate the basis from the origin because the basis is fine with single point precision.
	// Then we combine the translations from the model matrix and the view matrix using emulated doubles.
	// We add the result to the vertex and ignore the final lost precision.
	vec3 model_origin = model_matrix[3].xyz;
	if (sc_multimesh()) {
		modelview = modelview * matrix;

		vec3 instance_origin = mat3(model_matrix) * matrix[3].xyz;
		model_origin = double_add_vec3(model_origin, model_precision, instance_origin, vec3(0.0), model_precision);
	}

	// Overwrite the translation part of modelview with improved precision.
	vec3 temp_precision; // Will be ignored.
	modelview[3].xyz = double_add_vec3(model_origin, model_precision, inv_view_matrix[3].xyz, view_precision, temp_precision);
	modelview[3].xyz = mat3(read_view_matrix) * modelview[3].xyz;
#else
	mat4 modelview = read_view_matrix * model_matrix;
#endif
	mat3 modelview_normal = mat3(read_view_matrix) * model_normal_matrix;
	vec2 read_viewport_size = scene_data.viewport_size;

#ifdef POINT_SIZE_USED
	float point_size = 1.0;
#endif

	{
#CODE : VERTEX
	}

	float roughness = roughness_highp;
#ifdef NORMAL_USED
	vec3 normal = normal_highp;
#endif

// using local coordinates (default)
#if !defined(SKIP_TRANSFORM_USED) && !defined(VERTEX_WORLD_COORDS_USED)

	vertex = (modelview * vec4(vertex, 1.0)).xyz;

#ifdef NORMAL_USED
	// For correct non-uniform scale handling, normal has to be transformed by normal matrix, but tangent vectors need to use model matrix as is
	normal = modelview_normal * normal;
#endif

#ifdef TANGENT_USED
	// For non-uniform scale, this produces non-orthogonal TBNs; ideally binormal should be reconstructed in fragment shader with cross
	tangent = mat3(modelview) * tangent;
	binormal = mat3(modelview) * binormal;
#endif
#endif // !defined(SKIP_TRANSFORM_USED) && !defined(VERTEX_WORLD_COORDS_USED)

//using world coordinates
#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)

	vertex = (read_view_matrix * vec4(vertex, 1.0)).xyz;
#ifdef NORMAL_USED
	normal = (read_view_matrix * vec4(normal, 0.0)).xyz;
#endif

#ifdef TANGENT_USED
	binormal = (read_view_matrix * vec4(binormal, 0.0)).xyz;
	tangent = (read_view_matrix * vec4(tangent, 0.0)).xyz;
#endif
#endif

	vertex_interp = vertex;

	// Normalize TBN vectors to account for model/normal transforms that may have scale
#ifdef NORMAL_USED
	normal_interp = normalize(normal);
#endif

#ifdef TANGENT_USED
	tangent_interp = normalize(tangent);
	binormal_interp = normalize(binormal);
#endif

#ifdef MODE_RENDER_DEPTH

#ifdef MODE_DUAL_PARABOLOID

	vertex_interp.z *= scene_data.dual_paraboloid_side;

	dp_clip = vertex_interp.z; //this attempts to avoid noise caused by objects sent to the other parabolloid side due to bias

	//for dual paraboloid shadow mapping, this is the fastest but least correct way, as it curves straight edges

	vec3 vtx = vertex_interp;
	float distance = length(vtx);
	vtx = normalize(vtx);
	vtx.xy /= 1.0 - vtx.z;
	vtx.z = (distance / scene_data.z_far);
	vtx.z = vtx.z * 2.0 - 1.0;
	vertex_interp = vtx;

#endif

#endif //MODE_RENDER_DEPTH

#ifdef OVERRIDE_POSITION
	gl_Position = position;
#else
	gl_Position = projection_matrix * vec4(vertex_interp, 1.0);
#endif

#ifdef USE_MULTIVIEW
	combined_projected = combined_projection * vec4(vertex_interp, 1.0);
#endif

#ifdef MOTION_VECTORS
	screen_pos = gl_Position;
#endif

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED) && defined(USE_VERTEX_LIGHTING)
	diffuse_light_interp = vec4(0.0);
	specular_light_interp = vec4(0.0);

#ifdef USE_MULTIVIEW
	vec3 view = -normalize(vertex_interp - eye_offset);
	vec2 clip_pos = clamp((combined_projected.xy / combined_projected.w) * 0.5 + 0.5, 0.0, 1.0);
#else
	vec3 view = -normalize(vertex_interp);
	vec2 clip_pos = clamp((gl_Position.xy / gl_Position.w) * 0.5 + 0.5, 0.0, 1.0);
#endif

	uvec2 screen_size = uvec2(1.0 / scene_data.screen_pixel_size);
	uvec2 screen_pixel = clamp(uvec2(clip_pos * vec2(screen_size)), uvec2(0), screen_size - uvec2(1));
	uvec2 cluster_pos = screen_pixel >> implementation_data.cluster_shift;
	uint cluster_offset = (implementation_data.cluster_width * cluster_pos.y + cluster_pos.x) * (implementation_data.max_cluster_element_count_div_32 + 32);
	uint cluster_z = uint(clamp((-vertex_interp.z / scene_data.z_far) * 32.0, 0.0, 31.0));

	{ //omni lights

		uint cluster_omni_offset = cluster_offset;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_omni_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_omni_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);
			uint merged_mask = mask;

			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);
				uint light_index = 32 * i + bit;

				if (!bool(omni_lights.data[light_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (omni_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				light_process_omni_vertex(light_index, vertex, view, normal, roughness,
						diffuse_light_interp.rgb, specular_light_interp.rgb);
			}
		}
	}

	{ //spot lights
		uint cluster_spot_offset = cluster_offset + implementation_data.cluster_type_size;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_spot_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_spot_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);
			uint merged_mask = mask;

			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				uint light_index = 32 * i + bit;

				if (!bool(spot_lights.data[light_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (spot_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				light_process_spot_vertex(light_index, vertex, view, normal, roughness,
						diffuse_light_interp.rgb, specular_light_interp.rgb);
			}
		}
	}

	{ // Directional light.

		// We process the first directional light separately as it may have shadows.
		vec3 directional_diffuse = vec3(0.0);
		vec3 directional_specular = vec3(0.0);

		for (uint i = 0; i < scene_data.directional_light_count; i++) {
			if (!bool(directional_lights.data[i].mask & instances.data[instance_index].layer_mask)) {
				continue; // Not masked, skip.
			}

			if (directional_lights.data[i].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
				continue; // Statically baked light and object uses lightmap, skip.
			}
			if (i == 0) {
				light_compute_vertex(normal, directional_lights.data[0].direction, view,
						directional_lights.data[0].color * directional_lights.data[0].energy,
						true, roughness,
						directional_diffuse,
						directional_specular);
			} else {
				light_compute_vertex(normal, directional_lights.data[i].direction, view,
						directional_lights.data[i].color * directional_lights.data[i].energy,
						true, roughness,
						diffuse_light_interp.rgb,
						specular_light_interp.rgb);
			}
		}

		// Calculate the contribution from the shadowed light so we can scale the shadows accordingly.
		float diff_avg = dot(diffuse_light_interp.rgb, vec3(0.33333));
		float diff_dir_avg = dot(directional_diffuse, vec3(0.33333));
		if (diff_avg > 0.0) {
			diffuse_light_interp.a = diff_dir_avg / (diff_avg + diff_dir_avg);
		} else {
			diffuse_light_interp.a = 1.0;
		}

		diffuse_light_interp.rgb += directional_diffuse;

		float spec_avg = dot(specular_light_interp.rgb, vec3(0.33333));
		float spec_dir_avg = dot(directional_specular, vec3(0.33333));
		if (spec_avg > 0.0) {
			specular_light_interp.a = spec_dir_avg / (spec_avg + spec_dir_avg);
		} else {
			specular_light_interp.a = 1.0;
		}

		specular_light_interp.rgb += directional_specular;
	}

#endif //!defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED) && defined(USE_VERTEX_LIGHTING)

#ifdef MODE_RENDER_DEPTH
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_PANCAKE_SHADOWS)) {
		if (gl_Position.z >= 0.9999) {
			gl_Position.z = 0.9999;
		}
	}
#endif
#ifdef MODE_RENDER_MATERIAL
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_UV2_MATERIAL)) {
		vec2 uv_dest_attrib;
		if (uv_scale != vec4(0.0)) {
			uv_dest_attrib = (uv2_attrib.xy - 0.5) * uv_scale.zw;
		} else {
			uv_dest_attrib = uv2_attrib.xy;
		}

		vec2 uv_offset = unpackHalf2x16(draw_call.uv_offset);
		gl_Position.xy = (uv_dest_attrib + uv_offset) * 2.0 - 1.0;
		gl_Position.z = 0.00001;
		gl_Position.w = 1.0;
	}
#endif

#ifdef Z_CLIP_SCALE_USED
	if (!bool(scene_data.flags & SCENE_DATA_FLAGS_IN_SHADOW_PASS)) {
		gl_Position.z = mix(gl_Position.w, gl_Position.z, z_clip_scale);
	}
#endif

#ifdef POINT_SIZE_USED
	if (sc_emulate_point_size) {
		vec2 point_coords[6] = vec2[](
				vec2(0, 1),
				vec2(0, 0),
				vec2(1, 1),
				vec2(0, 0),
				vec2(1, 0),
				vec2(1, 1));

		vec2 point_coord = point_coords[gl_VertexIndex % 6];
		gl_Position.xy += (point_coord * 2.0 - 1.0) * (point_size / scene_data.viewport_size) * gl_Position.w;

#ifdef POINT_COORD_USED
		point_coord_interp = point_coord;
#endif
	} else {
		gl_PointSize = point_size;
	}
#endif
}

void _unpack_vertex_attributes(vec4 p_vertex_in, vec3 p_compressed_aabb_position, vec3 p_compressed_aabb_size,
#if defined(NORMAL_USED) || defined(TANGENT_USED)
		vec4 p_normal_in,
#ifdef NORMAL_USED
		out vec3 r_normal,
#endif
		out vec3 r_tangent,
		out vec3 r_binormal,
#endif
		out vec3 r_vertex) {

	r_vertex = p_vertex_in.xyz * p_compressed_aabb_size + p_compressed_aabb_position;
#ifdef NORMAL_USED
	r_normal = oct_to_vec3(p_normal_in.xy * 2.0 - 1.0);
#endif

#if defined(NORMAL_USED) || defined(TANGENT_USED)

	float binormal_sign;

	// This works because the oct value (0, 1) maps onto (0, 0, -1) which encodes to (1, 1).
	// Accordingly, if p_normal_in.z contains octahedral values, it won't equal (0, 1).
	if (p_normal_in.z > 0.0 || p_normal_in.w < 1.0) {
		// Uncompressed format.
		vec2 signed_tangent_attrib = p_normal_in.zw * 2.0 - 1.0;
		r_tangent = oct_to_vec3(vec2(signed_tangent_attrib.x, abs(signed_tangent_attrib.y) * 2.0 - 1.0));
		binormal_sign = sign(signed_tangent_attrib.y);
		r_binormal = normalize(cross(r_normal, r_tangent) * binormal_sign);
	} else {
		// Compressed format.
		float angle = p_vertex_in.w;
		binormal_sign = angle > 0.5 ? 1.0 : -1.0; // 0.5 does not exist in UNORM16, so values are either greater or smaller.
		angle = abs(angle * 2.0 - 1.0) * M_PI; // 0.5 is basically zero, allowing to encode both signs reliably.
		vec3 axis = r_normal;
		axis_angle_to_tbn(axis, angle, r_tangent, r_binormal, r_normal);
		r_binormal *= binormal_sign;
	}
#endif
}

void main() {
	uint instance_index = draw_call.instance_index;
	if (!sc_multimesh()) {
		instance_index += INSTANCE_INDEX;
	}

	instance_index_interp = instance_index;

#ifdef MOTION_VECTORS
	// Previous vertex.
	vec3 prev_vertex;
#ifdef NORMAL_USED
	vec3 prev_normal;
#endif
#if defined(NORMAL_USED) || defined(TANGENT_USED)
	vec3 prev_tangent;
	vec3 prev_binormal;
#endif

	_unpack_vertex_attributes(
			previous_vertex_attrib,
			instances.data[instance_index].compressed_aabb_position_pad.xyz,
			instances.data[instance_index].compressed_aabb_size_pad.xyz,

#if defined(NORMAL_USED) || defined(TANGENT_USED)
			previous_normal_attrib,
#ifdef NORMAL_USED
			prev_normal,
#endif
			prev_tangent,
			prev_binormal,
#endif
			prev_vertex);

	global_time = scene_data_block.prev_data.time;
	vertex_shader(prev_vertex,
#ifdef NORMAL_USED
			prev_normal,
#endif
#ifdef TANGENT_USED
			prev_tangent,
			prev_binormal,
#endif
			instance_index, draw_call.multimesh_motion_vectors_previous_offset, scene_data_block.prev_data, instances.data[instance_index].prev_transform,
#ifdef USE_DOUBLE_PRECISION
			instances.data[instance_index].prev_model_precision.xyz,
#endif
			prev_screen_position);
#else
	// Unused output.
	vec4 screen_position;
#endif

	vec3 vertex;
#ifdef NORMAL_USED
	vec3 normal;
#endif
#if defined(NORMAL_USED) || defined(TANGENT_USED)
	vec3 tangent;
	vec3 binormal;
#endif

	_unpack_vertex_attributes(
			vertex_angle_attrib,
			instances.data[instance_index].compressed_aabb_position_pad.xyz,
			instances.data[instance_index].compressed_aabb_size_pad.xyz,
#if defined(NORMAL_USED) || defined(TANGENT_USED)
			axis_tangent_attrib,
#ifdef NORMAL_USED
			normal,
#endif
			tangent,
			binormal,
#endif
			vertex);

	// Current vertex.
	global_time = scene_data_block.data.time;
	vertex_shader(vertex,
#ifdef NORMAL_USED
			normal,
#endif
#ifdef TANGENT_USED
			tangent,
			binormal,
#endif
			instance_index, draw_call.multimesh_motion_vectors_current_offset, scene_data_block.data, instances.data[instance_index].transform,
#ifdef USE_DOUBLE_PRECISION
			instances.data[instance_index].model_precision.xyz,
#endif

			screen_position);
}

#[fragment]

#version 460

#VERSION_DEFINES

#ifdef RT_TRANSPARENT_SHADOWS
// The transparent pass traces its own shadow rays (see rt_transparent_* below).
#extension GL_EXT_ray_query : enable
#define RT_TRANSPARENT_SUN_RAY bool(implementation_data.rt_transparent_shadows & 2u)
#else
#define RT_TRANSPARENT_SUN_RAY false
#endif

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0

#ifdef USE_MULTIVIEW
#define OUTPUT_IS_MULTIVIEW true
#else
#define OUTPUT_IS_MULTIVIEW false
#endif

/* Include half precision types. */
#include "../half_inc.glsl"
#include "scene_forward_clustered_inc.glsl"

#if defined(RT_TRANSPARENT_SHADOWS) && !defined(MODE_RENDER_SDF)
// The transparent pass's shadow rays trace this (see rt_transparent_* further
// down). Every variant of this shader shares one render-pass uniform set
// (the SDF variant excepted: it has its own set 1 in the include above), so
// every variant has to declare the binding -- and, unlike an unreferenced
// texture, an unreferenced acceleration structure is dropped from the SPIR-V,
// which would give the depth and material variants a different set layout.
// rt_transparent_keep_binding() below is the reference that keeps it.
layout(set = 1, binding = 47) uniform accelerationStructureEXT rt_tlas;

void rt_transparent_keep_binding() {
	// Never taken: the budget is clamped to 16 on the CPU. The compiler
	// cannot prove that, so the query, and the binding, survive.
	if (implementation_data.rt_transparent_max_rays == 0xFFFFFFFFu) {
		rayQueryEXT rq;
		rayQueryInitializeEXT(rq, rt_tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, vec3(0.0), 0.01, vec3(0.0, 1.0, 0.0), 0.02);
		rayQueryProceedEXT(rq);
		if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
			discard;
		}
	}
}
#endif // RT_TRANSPARENT_SHADOWS && !MODE_RENDER_SDF

/* Varyings */

#if defined(TEXTURE_STREAMING) && !defined(MODE_RENDER_DEPTH) && (defined(UV_USED) || defined(STREAMING_UV_USED))
// Since material feedback writes to a ssbo buffer, early fragment tests likely get disabled by the
// driver so unless we want really bad performance, we need to force enable it again.
//
// To Early-Z, or Not To Early-Z
//  - https://therealmjp.github.io/posts/to-earlyz-or-not-to-earlyz/#uavsstorage-texturesstorage-buffers
#if defined(ALPHA_SCISSOR_USED) || defined(ALPHA_HASH_USED) || defined(ENABLE_CLIP_ALPHA) || defined(UBERSHADER) || defined(DISCARD_USED)
#define TEXTURE_STREAMING_MAY_DISCARD
#endif

#if defined(DEPTH_TEST_DISABLED_USED) || defined(DEPTH_DRAW_NEVER_USED) || (defined(TEXTURE_STREAMING_DEPTH_PREPASS) && !defined(DEPTH_DRAW_ALWAYS_USED))
#define TEXTURE_STREAMING_NO_DEPTH_WRITE
#endif

#if !defined(DEPTH_USED) && (!defined(TEXTURE_STREAMING_MAY_DISCARD) || (defined(TEXTURE_STREAMING_NO_DEPTH_WRITE) && !defined(STENCIL_WRITE_USED)))
layout(early_fragment_tests) in;
#endif
#endif // TEXTURE_STREAMING

layout(location = 0) in vec3 vertex_interp;

#ifdef NORMAL_USED
layout(location = 1) in vec3 normal_interp;
#endif

#if defined(COLOR_USED)
layout(location = 2) in vec4 color_interp;
#endif

#ifdef UV_USED
layout(location = 3) in vec2 uv_interp;
#endif

#if defined(TEXTURE_STREAMING)
vec2 streaming_uv;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
layout(location = 4) in vec2 uv2_interp;
#endif

#ifdef TANGENT_USED
layout(location = 5) in vec3 tangent_interp;
layout(location = 6) in vec3 binormal_interp;
#endif

#ifdef MOTION_VECTORS
layout(location = 7) in vec4 screen_position;
layout(location = 8) in vec4 prev_screen_position;
#endif

#ifdef MODE_DUAL_PARABOLOID

layout(location = 9) in float dp_clip;

#endif

layout(location = 10) in flat uint instance_index_interp;

#ifdef USE_LIGHTMAP
#include "../bicubic_filter_inc.glsl"
#endif //USE_LIGHTMAP

#ifdef USE_MULTIVIEW
#extension GL_EXT_multiview : enable
#define ViewIndex gl_ViewIndex
vec3 multiview_uv(vec2 uv) {
	return vec3(uv, ViewIndex);
}
ivec3 multiview_uv(ivec2 uv) {
	return ivec3(uv, int(ViewIndex));
}
layout(location = 11) in vec4 combined_projected;
#else // USE_MULTIVIEW
#define ViewIndex 0
vec2 multiview_uv(vec2 uv) {
	return uv;
}
ivec2 multiview_uv(ivec2 uv) {
	return uv;
}
#endif // !USE_MULTIVIEW
#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED) && defined(USE_VERTEX_LIGHTING)
layout(location = 12) in vec4 diffuse_light_interp;
layout(location = 13) in vec4 specular_light_interp;
#endif

#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
layout(location = 14) in vec2 point_coord_interp;
#endif

//defines to keep compatibility with vertex

#ifdef USE_MULTIVIEW
#define projection_matrix scene_data.projection_matrix_view[ViewIndex]
#define inv_projection_matrix scene_data.inv_projection_matrix_view[ViewIndex]
#else
#define projection_matrix scene_data.projection_matrix
#define inv_projection_matrix scene_data.inv_projection_matrix
#endif

#define global_time scene_data_block.data.time

#if defined(ENABLE_SSS) && defined(ENABLE_TRANSMITTANCE)
//both required for transmittance to be enabled
#define LIGHT_TRANSMITTANCE_USED
#endif

#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = MATERIAL_UNIFORM_SET, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

#GLOBALS

#ifdef MODE_RENDER_DEPTH

#ifdef MODE_RENDER_MATERIAL

layout(location = 0) out vec4 albedo_output_buffer;
layout(location = 1) out vec4 normal_output_buffer;
layout(location = 2) out vec4 orm_output_buffer;
layout(location = 3) out vec4 emission_output_buffer;
layout(location = 4) out float depth_output_buffer;

#endif // MODE_RENDER_MATERIAL

#ifdef MODE_RENDER_NORMAL_ROUGHNESS
layout(location = 0) out vec4 normal_roughness_output_buffer;
// The prepass G-buffer (albedo_f0_inc.glsl), always written with the normal.
layout(location = 1) out vec4 gbuf_albedo_output_buffer;
layout(location = 2) out vec4 gbuf_f0_output_buffer;

#ifdef MODE_RENDER_VOXEL_GI
layout(location = 3) out uvec2 voxel_gi_buffer;
#endif

#endif //MODE_RENDER_NORMAL
#else // RENDER DEPTH

#ifdef MODE_SEPARATE_SPECULAR

layout(location = 0) out vec4 diffuse_buffer; //diffuse (rgb) and roughness
layout(location = 1) out vec4 specular_buffer; //specular and SSS (subsurface scatter)
#else

layout(location = 0) out vec4 frag_color;
#endif // MODE_SEPARATE_SPECULAR

#endif // RENDER DEPTH

#ifdef MOTION_VECTORS
#ifdef MODE_RENDER_NORMAL_ROUGHNESS
// The prepass writing motion vectors for the ray-traced temporal passes
// (render_forward_clustered.cpp PASS_MODE_DEPTH_NORMAL_ROUGHNESS_MOTION):
// after the normal and the G-buffer.
layout(location = 3) out vec2 motion_vector;
#else
layout(location = 2) out vec2 motion_vector;
#endif
#endif

#include "../scene_forward_aa_inc.glsl"

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

// Default to SPECULAR_SCHLICK_GGX.
#if !defined(SPECULAR_DISABLED) && !defined(SPECULAR_SCHLICK_GGX) && !defined(SPECULAR_TOON)
#define SPECULAR_SCHLICK_GGX
#endif

#include "../scene_forward_gi_inc.glsl"

// Local shadow maps are skipped on the frames the stochastic pass ray traces
// their shadows instead. The mobile renderer shares the light include but has
// no implementation data block, so the condition is a macro with a default.
// The half-resolution direct lighting's per-pixel analytic term (see the
// composite in main) runs the light loops unshadowed whatever the frame
// rendered: the shadows are the ratios' business.
bool stochastic_analytic_unshadowed = false;
#define LOCAL_SHADOW_MAPS_RENDERED (implementation_data.local_shadow_maps != 0u && !stochastic_analytic_unshadowed)

#ifdef RT_TRANSPARENT_SHADOWS
// Transparent surfaces are shaded analytically, per fragment, with no
// history: the stochastic pass's traced visibility is a screen-space result
// keyed to the opaque depth, and the local shadow maps it replaces are no
// longer rendered. So on the frames the stochastic pass owns local shadows,
// the transparent pass traces one hard shadow ray per light itself, from the
// fragment toward the light's center, up to a per-fragment budget. The TLAS
// is the same one every other traced pass uses (world space, absolute).
// (Declared with the render-pass bindings above, since every variant must
// carry it.)

// Rays this fragment may still trace for local lights; main() fills it from
// the budget when the pass is active and leaves it zero otherwise, which is
// what keeps the opaque pass and every other consumer of the light include
// bit-identical to before.
uint rt_transparent_ray_budget = 0u;

// One hard visibility ray from a view-space fragment toward a view-space
// target, both converted to the TLAS's world space. The origin steps off the
// surface along the side of its normal that faces the target, by the same
// bias the stochastic pass uses; t_min repeats the bias so an origin that
// still touches the surface cannot hit it.
bool rt_transparent_trace_visible(vec3 view_origin, vec3 view_normal, vec3 view_target, uint caster_mask) {
	mat4 rt_inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 delta = view_target - view_origin;
	float dist = length(delta);
	if (dist < 1e-4) {
		return true;
	}
	vec3 dir = delta / dist;
	float bias = implementation_data.rt_ray_bias;
	vec3 n = normalize(view_normal);
	n *= sign(dot(n, dir)) >= 0.0 ? 1.0 : -1.0;
	vec3 world_origin = (rt_inv_view * vec4(view_origin + n * bias, 1.0)).xyz;
	vec3 world_dir = normalize(mat3(rt_inv_view) * dir);
	float t_max = dist - bias;
	if (t_max <= bias) {
		return true;
	}
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, rt_tlas,
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
			caster_mask, world_origin, bias, world_dir, t_max);
	rayQueryProceedEXT(rq);
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT;
}

// The same ray toward a directional light: a view-space direction instead of
// a target, and a range that covers the whole scene.
bool rt_transparent_trace_visible_dir(vec3 view_origin, vec3 view_normal, vec3 view_to_light, uint caster_mask) {
	return rt_transparent_trace_visible(view_origin, view_normal, view_origin + normalize(view_to_light) * 16384.0, caster_mask);
}
#endif // RT_TRANSPARENT_SHADOWS

#include "../scene_forward_lights_inc.glsl"

#endif //!defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

#ifndef MODE_RENDER_DEPTH

vec4 volumetric_fog_process(vec2 screen_uv, float z) {
	vec3 fog_pos = vec3(screen_uv, z * implementation_data.volumetric_fog_inv_length);
	if (fog_pos.z < 0.0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	} else if (fog_pos.z < 1.0) {
		fog_pos.z = pow(fog_pos.z, implementation_data.volumetric_fog_detail_spread);
	}

	return texture(sampler3D(volumetric_fog_texture, SAMPLER_LINEAR_CLAMP), fog_pos);
}

vec4 fog_process(vec3 vertex) {
	vec3 fog_color = scene_data_block.data.fog_light_color;

	if (scene_data_block.data.fog_aerial_perspective > 0.0) {
		vec3 sky_fog_color = vec3(0.0);
		vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
		// mip_level always reads from the second mipmap and higher so the fog is always slightly blurred
		float mip_level = mix(1.0 / MAX_ROUGHNESS_LOD, 1.0, 1.0 - (abs(vertex.z) - scene_data_block.data.z_near) / (scene_data_block.data.z_far - scene_data_block.data.z_near));
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		float roughness_lod, blend;
		blend = modf(mip_level * MAX_ROUGHNESS_LOD, roughness_lod);
		float cube_lod = vec3_to_oct_lod(dFdx(cube_view), dFdy(cube_view), scene_data_block.data.radiance_pixel_size);
		vec2 cube_uv = vec3_to_oct_with_border(cube_view, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		vec3 sky_sample_a = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(cube_uv, roughness_lod), cube_lod).rgb;
		vec3 sky_sample_b = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(cube_uv, roughness_lod + 1), cube_lod).rgb;
		sky_fog_color = mix(sky_sample_a, sky_sample_b, blend);
#else
		float roughness_lod = mip_level * MAX_ROUGHNESS_LOD;
		vec2 cube_uv = vec3_to_oct_with_border(cube_view, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		sky_fog_color = textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), cube_uv, roughness_lod).rgb;
#endif //USE_RADIANCE_OCTMAP_ARRAY
		fog_color = mix(fog_color, sky_fog_color, scene_data_block.data.fog_aerial_perspective);
	}

	if (scene_data_block.data.fog_sun_scatter > 0.001) {
		vec4 sun_scatter = vec4(0.0);
		float sun_total = 0.0;
		vec3 view = normalize(vertex);

		for (uint i = 0; i < scene_data_block.data.directional_light_count; i++) {
			vec3 light_color = directional_lights.data[i].color * directional_lights.data[i].energy;
			float light_amount = pow(max(dot(view, directional_lights.data[i].direction), 0.0), 8.0);
			fog_color += light_color * light_amount * scene_data_block.data.fog_sun_scatter;
		}
	}

	float fog_amount = 0.0;

	if (sc_use_depth_fog()) {
		float fog_z = smoothstep(scene_data_block.data.fog_depth_begin, scene_data_block.data.fog_depth_end, length(vertex));
		float fog_quad_amount = pow(fog_z, scene_data_block.data.fog_depth_curve) * scene_data_block.data.fog_density;
		fog_amount = fog_quad_amount;
	} else {
		fog_amount = 1 - exp(min(0.0, -length(vertex) * scene_data_block.data.fog_density));
	}

	if (abs(scene_data_block.data.fog_height_density) >= 0.0001) {
		mat4 inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
				scene_data_block.data.inv_view_matrix[1],
				scene_data_block.data.inv_view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));

		float y = (inv_view_matrix * vec4(vertex, 1.0)).y;

		float y_dist = y - scene_data_block.data.fog_height;

		float vfog_amount = 1.0 - exp(min(0.0, y_dist * scene_data_block.data.fog_height_density));

		fog_amount = max(vfog_amount, fog_amount);
	}

	return vec4(fog_color, fog_amount);
}

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;

	item_from = item_min >> 5;
	item_to = (item_max == 0) ? 0 : ((item_max - 1) >> 5) + 1; //side effect of how it is stored, as item_max 0 means no elements
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

#endif //!MODE_RENDER DEPTH

#if defined(MODE_RENDER_NORMAL_ROUGHNESS) || defined(MODE_RENDER_MATERIAL)
// https://advances.realtimerendering.com/s2010/Kaplanyan-CryEngine3(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf
vec3 encode24(vec3 v) {
	// Unsigned normal (handles most symmetry)
	vec3 vNormalUns = abs(v);
	// Get the major axis for our collapsed cubemap lookup
	float maxNAbs = max(vNormalUns.z, max(vNormalUns.x, vNormalUns.y));
	// Get the collapsed cubemap texture coordinates
	vec2 vTexCoord = vNormalUns.z < maxNAbs ? (vNormalUns.y < maxNAbs ? vNormalUns.yz : vNormalUns.xz) : vNormalUns.xy;
	vTexCoord /= maxNAbs;
	vTexCoord = vTexCoord.x < vTexCoord.y ? vTexCoord.yx : vTexCoord.xy;
	// Stretch:
	vTexCoord.y /= vTexCoord.x;
	float fFittingScale = texture(sampler2D(best_fit_normal_texture, SAMPLER_NEAREST_CLAMP), vTexCoord).r;
	// Make vector touch unit cube
	vec3 result = v / maxNAbs;
	// scale the normal to get the best fit
	result *= fFittingScale;
	return result;
}
#endif // MODE_RENDER_NORMAL_ROUGHNESS

// The composites' fast path: the view depth the fragment's own plane
// (through vertex, facing p_normal) has along the view ray through a
// sampling-grid tap's full-resolution pixel, so a tap on the same flat
// surface passes at any grazing angle (a fixed depth tolerance failed the
// whole floor: four full-resolution pixels of a floor seen at a grazing
// angle are more than two percent of its depth apart).
float plane_depth_at_tap(mat4 p_inv_projection, ivec2 p_full_pixel, ivec2 p_full_size, vec3 p_vertex, vec3 p_normal) {
	vec2 ndc = (vec2(p_full_pixel) + 0.5) / vec2(p_full_size) * 2.0 - 1.0;
	vec4 r4 = p_inv_projection * vec4(ndc, 1.0, 1.0);
	vec3 r = r4.xyz / r4.w;
	float denom = dot(p_normal, r);
	if (abs(denom) < 1e-6) {
		return -p_vertex.z;
	}
	return -dot(p_normal, p_vertex) / denom * r.z;
}

void fragment_shader(in SceneData scene_data) {
	uint instance_index = instance_index_interp;

#ifdef PREMUL_ALPHA_USED
	float premul_alpha = 1.0;
#endif // PREMUL_ALPHA_USED
	//lay out everything, whatever is unused is optimized away anyway
	vec3 vertex = vertex_interp;
#ifdef USE_MULTIVIEW
	vec3 eye_offset = scene_data.eye_offset[ViewIndex].xyz;
	vec3 view_highp = -normalize(vertex_interp - eye_offset);

	// UV in our combined frustum space is used for certain screen uv processes where it's
	// overkill to render separate left and right eye views
	vec2 combined_uv = (combined_projected.xy / combined_projected.w) * 0.5 + 0.5;
#else
	vec3 eye_offset = vec3(0.0, 0.0, 0.0);
	vec3 view_highp = -normalize(vertex_interp);
#endif
	vec3 albedo_highp = vec3(1.0);
	vec3 backlight = vec3(0.0);
	vec4 transmittance_color = vec4(0.0, 0.0, 0.0, 1.0);
	float transmittance_depth = 0.0;
	float transmittance_boost = 0.0;
	float metallic_highp = 0.0;
	float specular = 0.5;
	vec3 emission = vec3(0.0);
	float roughness_highp = 1.0;
	float rim = 0.0;
	float rim_tint = 0.0;
	float clearcoat = 0.0;
	float clearcoat_roughness = 0.0;
	float anisotropy = 0.0;
	vec2 anisotropy_flow = vec2(1.0, 0.0);
	vec3 energy_compensation = vec3(1.0);
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
#if defined(CUSTOM_RADIANCE_USED)
	vec4 custom_radiance = vec4(0.0);
#endif
#if defined(CUSTOM_IRRADIANCE_USED)
	vec4 custom_irradiance = vec4(0.0);
#endif

	float ao = 1.0;
	float ao_light_affect = 0.0;

	float alpha_highp = float(instances.data[instance_index].flags >> INSTANCE_FLAGS_FADE_SHIFT) / float(255.0);

#ifdef TANGENT_USED
	vec3 binormal = binormal_interp;
	vec3 tangent = tangent_interp;
#else
	vec3 binormal = vec3(0.0);
	vec3 tangent = vec3(0.0);
#endif

#ifdef NORMAL_USED
	vec3 normal_highp = normal_interp;
#if defined(DO_SIDE_CHECK)
	if (!gl_FrontFacing) {
		normal_highp = -normal_highp;
	}
	{
		// Winding says which side of the triangle is being drawn; it does not
		// say which side its vertex normals belong to. A mesh whose triangles
		// wind against their normals (imported assets do this) passes the test
		// above with a normal pointing into the surface, away from the camera,
		// and every N.V term downstream -- Fresnel, the reflection direction,
		// specular occlusion, the ray-traced gather's hemisphere -- then runs
		// on the back of the wall. The face's own plane, from the position
		// derivatives, settles it: a normal more than 120 degrees from the
		// camera-facing side of that plane can only be an inverted one. The
		// margin leaves silhouette interpolation alone, where a smooth normal
		// legitimately leans past the face by tens of degrees.
		vec3 face_normal = cross(dFdx(vertex_interp), dFdy(vertex_interp));
		if (dot(face_normal, vertex_interp) > 0.0) {
			face_normal = -face_normal;
		}
		if (dot(normal_highp, face_normal) < -0.5 * length(normal_highp) * length(face_normal)) {
			normal_highp = -normal_highp;
		}
	}
#endif // DO_SIDE_CHECK
#endif // NORMAL_USED

#ifdef UV_USED
	vec2 uv = uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	vec2 uv2 = uv2_interp;
#endif

#if defined(COLOR_USED)
	vec4 color = color_interp;
#endif

#if defined(NORMAL_MAP_USED)

	vec3 normal_map = vec3(0.5);
#endif

#if defined(BENT_NORMAL_MAP_USED)
	vec3 bent_normal_vector = vec3(0.5);
	vec3 bent_normal_map = vec3(0.5);
#endif

	float normal_map_depth = 1.0;

	vec2 screen_uv = gl_FragCoord.xy * scene_data.screen_pixel_size;

	float sss_strength = 0.0;

#ifdef ALPHA_SCISSOR_USED
	float alpha_scissor_threshold = 1.0;
#endif // ALPHA_SCISSOR_USED

#ifdef ALPHA_HASH_USED
	float alpha_hash_scale = 1.0;
#endif // ALPHA_HASH_USED

#ifdef ALPHA_ANTIALIASING_EDGE_USED
	float alpha_antialiasing_edge = 0.0;
	vec2 alpha_texture_coordinate = vec2(0.0, 0.0);
#endif // ALPHA_ANTIALIASING_EDGE_USED

	mat4 inv_view_matrix = transpose(mat4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 read_model_matrix = transpose(mat4(instances.data[instance_index].transform[0],
			instances.data[instance_index].transform[1],
			instances.data[instance_index].transform[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef LIGHT_VERTEX_USED
	vec3 light_vertex = vertex;
#endif //LIGHT_VERTEX_USED

	mat3 model_normal_matrix;
	if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(inverse(mat3(read_model_matrix)));
	} else {
		model_normal_matrix = mat3(read_model_matrix);
	}

	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec2 read_viewport_size = scene_data.viewport_size;

#ifdef POINT_COORD_USED
#ifdef POINT_SIZE_USED
	vec2 point_coord;
	if (sc_emulate_point_size) {
		point_coord = point_coord_interp;
	} else {
		point_coord = gl_PointCoord;
	}
#else // !POINT_SIZE_USED
	vec2 point_coord = vec2(0.5);
#endif
#endif

	{
#CODE : FRAGMENT
	}

	float roughness = roughness_highp;
	float metallic = metallic_highp;
	vec3 albedo = albedo_highp;
	float alpha = alpha_highp;
#ifdef NORMAL_USED
	vec3 normal = normal_highp;
#endif

#ifdef LIGHT_TRANSMITTANCE_USED
	transmittance_color.a *= sss_strength;
#endif

#ifdef LIGHT_VERTEX_USED
	vertex = light_vertex;
#ifdef USE_MULTIVIEW
	vec3 view = -normalize(vertex - eye_offset);
#else
	vec3 view = -normalize(vertex);
#endif //USE_MULTIVIEW
#else
	vec3 view = view_highp;
#endif //LIGHT_VERTEX_USED

#ifdef NORMAL_USED
	vec3 geo_normal = normalize(normal);
#endif // NORMAL_USED

#ifndef USE_SHADOW_TO_OPACITY

#ifdef ALPHA_SCISSOR_USED
#ifdef MODE_RENDER_MATERIAL
	if (alpha < alpha_scissor_threshold) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
#endif // MODE_RENDER_MATERIAL
#endif // ALPHA_SCISSOR_USED

#if defined(UV_USED) || defined(STREAMING_UV_USED)
#if defined(TEXTURE_STREAMING)
#if !defined(MODE_RENDER_DEPTH)
	if (sc_material_feedback()) {
// When STREAMING_UV_USED is not used just use normal UVs.
#if !defined(STREAMING_UV_USED)
		streaming_uv = uv_interp;
#endif
		// Instance has materials which require feedback.
		vec2 uv_dx = dFdx(streaming_uv);
		vec2 uv_dy = dFdy(streaming_uv);

		if (!gl_HelperInvocation) {
			// Calculate the mip level needed for the current fragment based on UV derivatives.
			float px_sq = dot(uv_dx, uv_dx);
			float py_sq = dot(uv_dy, uv_dy);
			float min_sq = min(px_sq, py_sq);
			float max_sq = max(px_sq, py_sq);

			// Anisotropic filtering allows using the mip level of the minor axis (min_sq),
			// but limited by the max anisotropy (usually 16x).
			// If the anisotropy ratio exceeds 16, we are forced to use a lower res mip.
			const float MAX_ANISOTROPY = 16.0;
			float lod_sq = max(min_sq, max_sq / (MAX_ANISOTROPY * MAX_ANISOTROPY));

			// Bitwise NOT inverts the ordering so that smaller lod_sq (higher quality)
			// maps to larger uint values, allowing atomicMax with a 0-cleared buffer.
			uint required_mip = ~floatBitsToUint(lod_sq);

			// Reduce atomic contention using subgroup operations.
			// Find maximum inverted mip level across all invocations in the subgroup, then only
			// one invocation performs the atomic write.
			// Right now this assumes all invocations will have the same instance index.
			// If that is not true then probably need a subgroupAllEqual check first + fallback.
			uint subgroup_max_mip = subgroupMax(required_mip);
			if (subgroupElect()) {
				const uint material_feedback_index = instances.data[instance_index].material_feedback_index;
				atomicMax(material_feedback.data[material_feedback_index], subgroup_max_mip);
			}
		}
	}
#endif // MODE_RENDER_DEPTH
#endif // TEXTURE_STREAMING
#endif // UV_USED || STREAMING_UV_USED

// alpha hash can be used in unison with alpha antialiasing
#ifdef ALPHA_HASH_USED
	vec3 object_pos = (inverse(read_model_matrix) * inv_view_matrix * vec4(vertex, 1.0)).xyz;
#ifdef MODE_RENDER_MATERIAL
	if (alpha < compute_alpha_hash_threshold(object_pos, alpha_hash_scale)) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < compute_alpha_hash_threshold(object_pos, alpha_hash_scale)) {
		discard;
	}
#endif // MODE_RENDER_MATERIAL
#endif // ALPHA_HASH_USED

// If we are not edge antialiasing, we need to remove the output alpha channel from scissor and hash
#if (defined(ALPHA_SCISSOR_USED) || defined(ALPHA_HASH_USED)) && !defined(ALPHA_ANTIALIASING_EDGE_USED) && !defined(MODE_RENDER_MATERIAL)
	alpha = 1.0;
#endif

#ifdef ALPHA_ANTIALIASING_EDGE_USED
// If alpha scissor is used, we must further the edge threshold, otherwise we won't get any edge feather
#ifdef ALPHA_SCISSOR_USED
	alpha_antialiasing_edge = clamp(alpha_scissor_threshold + alpha_antialiasing_edge, 0.0, 1.0);
#endif
	alpha = compute_alpha_antialiasing_edge(alpha, alpha_texture_coordinate, alpha_antialiasing_edge);
#endif // ALPHA_ANTIALIASING_EDGE_USED

#if defined(ALPHA_USED) && !defined(MODE_RENDER_DEPTH) && !defined(MODE_RENDER_MATERIAL) && !defined(ALPHA_SCISSOR_USED) && !defined(ALPHA_HASH_USED) && !defined(ALPHA_ANTIALIASING_EDGE_USED) && !defined(BLEND_MUL_USED) && !defined(BLEND_PREMUL_ALPHA_USED) && !defined(DEPTH_DRAW_ALWAYS_USED) && !defined(STENCIL_WRITE_USED) && !defined(DEPTH_USED)
	// A fragment of a blended material whose alpha came out zero contributes
	// nothing (mix, add and subtract all scale the source by it) and, in this
	// pass, writes neither depth nor stencil: it ends here rather than shading
	// every light for nothing. Content ships alpha-zero helper meshes, and one
	// filling the screen cost as much as the rest of the frame.
	if (alpha <= 0.0) {
		discard;
	}
#endif

#if defined(USE_OPAQUE_PREPASS) && !defined(MODE_RENDER_DEPTH) && !defined(MODE_RENDER_MATERIAL)
	// Profiling (GODOT_TRANSPARENT_ABLATE=core / fringe): a depth-pre-pass
	// surface drops the fragments its pre-pass wrote (the 0.99 threshold of
	// the pre-pass), or the rest, so each part's cost can be timed.
	if (implementation_data.transparent_debug != 0u) {
		if ((implementation_data.transparent_debug & 32u) != 0u && alpha >= 0.99) {
			discard;
		}
		if ((implementation_data.transparent_debug & 64u) != 0u && alpha < 0.99) {
			discard;
		}
	}
#endif

#ifdef MODE_RENDER_DEPTH
#if defined(USE_OPAQUE_PREPASS) || defined(ALPHA_ANTIALIASING_EDGE_USED)
	if (alpha < scene_data.opaque_prepass_threshold) {
		discard;
	}
#endif // USE_OPAQUE_PREPASS || ALPHA_ANTIALIASING_EDGE_USED
#endif // MODE_RENDER_DEPTH

#if defined(MODE_RENDER_MATERIAL) && (defined(ALPHA_SCISSOR_USED) || defined(ALPHA_HASH_USED) || defined(USE_OPAQUE_PREPASS))
	// The surface cache's card capture (not the lightmapper's UV2 capture,
	// which keeps the alpha in the texel): a fragment an alpha-tested material
	// draws under half alpha is a hole, and its texel stays unfilled (depth 0),
	// so the card lookups find the hole through the depth they test anyway,
	// the ray goes on through it, and the mips and filters never average the
	// texel in. A mesh whose material draws nothing (an invisible dome) then
	// has no filled texel at all.
	if (!bool(scene_data.flags & SCENE_DATA_FLAGS_USE_UV2_MATERIAL) && alpha < 0.5) {
		discard;
	}
#endif

#endif // !USE_SHADOW_TO_OPACITY

#if defined(NORMAL_MAP_USED)
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy))); //always ignore Z, as it can be RG packed, Z may be pos/neg, etc.

	// Tangent-space transformation is performed using unnormalized TBN vectors, per MikkTSpace.
	// See: http://www.mikktspace.com/
	normal = normalize(mix(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#elif defined(NORMAL_USED)
	normal = geo_normal;
#endif // NORMAL_MAP_USED

#ifdef BENT_NORMAL_MAP_USED
	bent_normal_map.xy = bent_normal_map.xy * 2.0 - 1.0;
	bent_normal_map.z = sqrt(max(0.0, 1.0 - dot(bent_normal_map.xy, bent_normal_map.xy)));

	bent_normal_vector = normalize(tangent * bent_normal_map.x + binormal * bent_normal_map.y + normal * bent_normal_map.z);
#endif

#ifdef LIGHT_ANISOTROPY_USED

	if (anisotropy > 0.01) {
		mat3 rot = mat3(normalize(tangent), normalize(binormal), normal);
		// Make local to space.
		tangent = normalize(rot * vec3(anisotropy_flow.x, anisotropy_flow.y, 0.0));
		binormal = normalize(rot * vec3(-anisotropy_flow.y, anisotropy_flow.x, 0.0));
	}

#endif

#ifdef ENABLE_CLIP_ALPHA
#ifdef MODE_RENDER_MATERIAL
	if (albedo.a < 0.99) {
		// Used for doublepass and shadowmapping.
		albedo.a = 0.0;
		alpha = 0.0;
	} else {
		albedo.a = 1.0;
		alpha = 1.0;
	}
#else
	if (albedo.a < 0.99) {
		//used for doublepass and shadowmapping
		discard;
	}
#endif // MODE_RENDER_MATERIAL
#endif

	/////////////////////// FOG //////////////////////
#ifndef MODE_RENDER_DEPTH

#ifndef FOG_DISABLED
#ifndef CUSTOM_FOG_USED
	// fog must be processed as early as possible and then packed.
	// to maximize VGPR usage
	// Draw "fixed" fog before volumetric fog to ensure volumetric fog can appear in front of the sky.

	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_FOG)) {
		fog = fog_process(vertex);
		// Premultiply by opacity and convert opacity to transmittance to match volumetric fog.
		fog.rgb *= fog.a;
		fog.a = 1.0 - fog.a;
	}

	if (implementation_data.volumetric_fog_enabled) {
#ifdef USE_MULTIVIEW
		vec4 volumetric_fog = volumetric_fog_process(combined_uv, -vertex.z);
#else
		vec4 volumetric_fog = volumetric_fog_process(screen_uv, -vertex.z);
#endif
		vec4 res = vec4(0.0);
		if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_FOG)) {
			//must use the full blending equation here to blend fogs
			if (sc_fog_use_legacy_blending()) {
				res.a = mix(0.0, fog.a, volumetric_fog.a);
				res.rgb = mix(volumetric_fog.rgb, fog.rgb, volumetric_fog.a);
			} else {
				res.a = fog.a * volumetric_fog.a;
				res.rgb = fog.rgb * volumetric_fog.a + volumetric_fog.rgb;
			}
		} else {
			res = volumetric_fog;
			if (sc_fog_use_legacy_blending()) {
				res.rgb *= 1.0 - res.a;
			}
		}
		fog = res;
	}
#else
	// Premultiply by opacity and convert opacity to transmittance to match volumetric fog.
	fog.rgb *= fog.a;
	fog.a = 1.0 - fog.a;
#endif //!CUSTOM_FOG_USED

	uint fog_rg = packHalf2x16(fog.rg);
	uint fog_ba = packHalf2x16(fog.ba);

#endif //!FOG_DISABLED
#endif //!MODE_RENDER_DEPTH

	/////////////////////// DECALS ////////////////////////////////

#ifndef MODE_RENDER_DEPTH

#ifdef USE_MULTIVIEW
	uvec2 cluster_pos = uvec2(combined_uv.xy / scene_data.screen_pixel_size) >> implementation_data.cluster_shift;
#else
	uvec2 cluster_pos = uvec2(gl_FragCoord.xy) >> implementation_data.cluster_shift;
#endif
	uint cluster_offset = (implementation_data.cluster_width * cluster_pos.y + cluster_pos.x) * (implementation_data.max_cluster_element_count_div_32 + 32);

	uint cluster_z = uint(clamp((-vertex.z / scene_data.z_far) * 32.0, 0.0, 31.0));

	//used for interpolating anything cluster related
	vec3 vertex_ddx = dFdx(vertex);
	vec3 vertex_ddy = dFdy(vertex);

	{ // process decals

		uint cluster_decal_offset = cluster_offset + implementation_data.cluster_type_size * 3;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_decal_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = subgroupBroadcastFirst(subgroupMin(item_from));
		item_to = subgroupBroadcastFirst(subgroupMax(item_to));

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_decal_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = subgroupBroadcastFirst(subgroupOr(mask));
			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) { //do not process if not originally here
					continue;
				}

				uint decal_index = 32 * i + bit;

				if (!bool(decals.data[decal_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				vec3 uv_local = (decals.data[decal_index].xform * vec4(vertex, 1.0)).xyz;
				if (any(lessThan(uv_local, vec3(0.0, -1.0, 0.0))) || any(greaterThan(uv_local, vec3(1.0)))) {
					continue; //out of decal
				}

				float fade = pow(1.0 - (uv_local.y > 0.0 ? uv_local.y : -uv_local.y), uv_local.y > 0.0 ? decals.data[decal_index].upper_fade : decals.data[decal_index].lower_fade);

				if (decals.data[decal_index].normal_fade > 0.0) {
					fade *= smoothstep(decals.data[decal_index].normal_fade, 1.0, dot(geo_normal, decals.data[decal_index].normal) * 0.5 + 0.5);
				}

				//we need ddx/ddy for mipmaps, so simulate them
				vec2 ddx = (decals.data[decal_index].xform * vec4(vertex_ddx, 0.0)).xz;
				vec2 ddy = (decals.data[decal_index].xform * vec4(vertex_ddy, 0.0)).xz;

				if (decals.data[decal_index].albedo_rect != vec4(0.0)) {
					//has albedo
					vec4 decal_albedo;
					if (sc_decal_use_mipmaps()) {
						decal_albedo = textureGrad(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].albedo_rect.zw + decals.data[decal_index].albedo_rect.xy, ddx * decals.data[decal_index].albedo_rect.zw, ddy * decals.data[decal_index].albedo_rect.zw);
					} else {
						decal_albedo = textureLod(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].albedo_rect.zw + decals.data[decal_index].albedo_rect.xy, 0.0);
					}
					decal_albedo *= decals.data[decal_index].modulate;
					decal_albedo.a *= fade;
					albedo = mix(albedo, decal_albedo.rgb, decal_albedo.a * decals.data[decal_index].albedo_mix);

					if (decals.data[decal_index].normal_rect != vec4(0.0)) {
						vec3 decal_normal;
						if (sc_decal_use_mipmaps()) {
							decal_normal = textureGrad(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].normal_rect.zw + decals.data[decal_index].normal_rect.xy, ddx * decals.data[decal_index].normal_rect.zw, ddy * decals.data[decal_index].normal_rect.zw).xyz;
						} else {
							decal_normal = textureLod(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].normal_rect.zw + decals.data[decal_index].normal_rect.xy, 0.0).xyz;
						}
						decal_normal.xy = decal_normal.xy * vec2(2.0, -2.0) - vec2(1.0, -1.0); //users prefer flipped y normal maps in most authoring software
						decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));
						//convert to view space, use xzy because y is up
						decal_normal = (decals.data[decal_index].normal_xform * decal_normal.xzy).xyz;

						normal = normalize(mix(normal, decal_normal, decal_albedo.a));
					}

					if (decals.data[decal_index].orm_rect != vec4(0.0)) {
						vec3 decal_orm;
						if (sc_decal_use_mipmaps()) {
							decal_orm = textureGrad(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].orm_rect.zw + decals.data[decal_index].orm_rect.xy, ddx * decals.data[decal_index].orm_rect.zw, ddy * decals.data[decal_index].orm_rect.zw).xyz;
						} else {
							decal_orm = textureLod(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].orm_rect.zw + decals.data[decal_index].orm_rect.xy, 0.0).xyz;
						}
						ao = mix(ao, decal_orm.r, decal_albedo.a);
						roughness = mix(roughness, decal_orm.g, decal_albedo.a);
						metallic = mix(metallic, decal_orm.b, decal_albedo.a);
					}
				}

				if (decals.data[decal_index].emission_rect != vec4(0.0)) {
					//emission is additive, so its independent from albedo
					if (sc_decal_use_mipmaps()) {
						emission += textureGrad(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].emission_rect.zw + decals.data[decal_index].emission_rect.xy, ddx * decals.data[decal_index].emission_rect.zw, ddy * decals.data[decal_index].emission_rect.zw).xyz * decals.data[decal_index].modulate.rgb * decals.data[decal_index].emission_energy * fade;
					} else {
						emission += textureLod(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].emission_rect.zw + decals.data[decal_index].emission_rect.xy, 0.0).xyz * decals.data[decal_index].modulate.rgb * decals.data[decal_index].emission_energy * fade;
					}
				}
			}
		}
	}

	//pack albedo until needed again, saves 2 VGPRs in the meantime

#endif //not render depth
	/////////////////////// LIGHTING //////////////////////////////

#ifdef NORMAL_USED
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_ROUGHNESS_LIMITER)) {
		//https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf
		float roughness2 = roughness * roughness;
		vec3 dndu = dFdx(normal), dndv = dFdy(normal);
		float variance = scene_data.roughness_limiter_amount * (dot(dndu, dndu) + dot(dndv, dndv));
		float kernelRoughness2 = min(2.0 * variance, scene_data.roughness_limiter_limit); //limit effect
		float filteredRoughness2 = min(1.0, roughness2 + kernelRoughness2);
		roughness = sqrt(filteredRoughness2);

		// Reject very small roughness values. Lack of precision can collapse
		// roughness^4 to 0 in GGX specular equations and cause divisions by zero.
		if (roughness < 0.00000001) {
			roughness = 0.0;
		}
	}
#endif
	//apply energy conservation

	vec3 direct_specular_light = vec3(0.0, 0.0, 0.0);
	vec3 indirect_specular_light = vec3(0.0, 0.0, 0.0);
	vec3 diffuse_light = vec3(0.0, 0.0, 0.0);
	vec3 ambient_light = vec3(0.0, 0.0, 0.0);
#ifndef MODE_UNSHADED
	// Used in regular draw pass and when drawing SDFs for SDFGI and materials for VoxelGI.
	emission *= scene_data.emissive_exposure_normalization;
#endif

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

#ifndef AMBIENT_LIGHT_DISABLED
// Use bent normal for indirect lighting where possible
#ifdef BENT_NORMAL_MAP_USED
	vec3 indirect_normal = bent_normal_vector;
#else
	vec3 indirect_normal = normal;
#endif

	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP)) {
#ifdef LIGHT_ANISOTROPY_USED
		// https://google.github.io/filament/Filament.html#lighting/imagebasedlights/anisotropy
		vec3 anisotropic_direction = anisotropy >= 0.0 ? binormal : tangent;
		vec3 anisotropic_tangent = cross(anisotropic_direction, view);
		vec3 anisotropic_normal = cross(anisotropic_tangent, anisotropic_direction);
		vec3 bent_normal = normalize(mix(normal, anisotropic_normal, abs(anisotropy) * clamp(5.0 * roughness, 0.0, 1.0)));
		vec3 ref_vec = reflect(-view, bent_normal);
		ref_vec = mix(ref_vec, bent_normal, roughness * roughness);
#else
		vec3 ref_vec = reflect(-view, normal);
		ref_vec = mix(ref_vec, normal, roughness * roughness);
#endif

		float horizon = min(1.0 + dot(ref_vec, normal), 1.0);
		ref_vec = scene_data.radiance_inverse_xform * ref_vec;

#ifdef USE_RADIANCE_OCTMAP_ARRAY

		float roughness_lod, blend;

		blend = modf(sqrt(roughness) * MAX_ROUGHNESS_LOD, roughness_lod);

		float ref_lod = vec3_to_oct_lod(dFdx(ref_vec), dFdy(ref_vec), scene_data_block.data.radiance_pixel_size);
		vec2 ref_uv = vec3_to_oct_with_border(ref_vec, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		vec3 indirect_sample_a = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ref_uv, roughness_lod), ref_lod).rgb;
		vec3 indirect_sample_b = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ref_uv, roughness_lod + 1), ref_lod).rgb;
		indirect_specular_light = mix(indirect_sample_a, indirect_sample_b, blend);

#else
		float roughness_lod = sqrt(roughness) * MAX_ROUGHNESS_LOD;
		vec2 ref_uv = vec3_to_oct_with_border(ref_vec, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		indirect_specular_light = textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), ref_uv, roughness_lod).rgb;

#endif //USE_RADIANCE_OCTMAP_ARRAY
		indirect_specular_light *= scene_data.IBL_exposure_normalization;
		indirect_specular_light *= horizon * horizon;
		indirect_specular_light *= scene_data.ambient_light_color_energy.a;
	} else if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_REFLECTION_COLOR)) {
		indirect_specular_light = scene_data.reflection_color.rgb;
	}

#if defined(CUSTOM_RADIANCE_USED)
	indirect_specular_light = mix(indirect_specular_light, custom_radiance.rgb, custom_radiance.a);
#endif

#ifndef USE_LIGHTMAP
	//lightmap overrides everything
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT)) {
		ambient_light = scene_data.ambient_light_color_energy.rgb;

		if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP)) {
			vec3 ambient_dir = scene_data.radiance_inverse_xform * indirect_normal;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
			float ambient_lod = vec3_to_oct_lod(dFdx(ambient_dir), dFdy(ambient_dir), scene_data_block.data.radiance_pixel_size);
			vec2 ambient_uv = vec3_to_oct_with_border(ambient_dir, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
			vec3 cubemap_ambient = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ambient_uv, MAX_ROUGHNESS_LOD), ambient_lod).rgb;
#else
			float roughness_lod = MAX_ROUGHNESS_LOD;
			vec2 ambient_uv = vec3_to_oct_with_border(ambient_dir, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
			vec3 cubemap_ambient = textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), ambient_uv, roughness_lod).rgb;
#endif //USE_RADIANCE_OCTMAP_ARRAY
			cubemap_ambient *= scene_data.IBL_exposure_normalization;
			ambient_light = mix(ambient_light, cubemap_ambient * scene_data.ambient_light_color_energy.a, scene_data.ambient_color_sky_mix);
		}
	}
#endif // USE_LIGHTMAP
#if defined(CUSTOM_IRRADIANCE_USED)
	ambient_light = mix(ambient_light, custom_irradiance.rgb, custom_irradiance.a);
#endif

#ifdef LIGHT_CLEARCOAT_USED
	vec3 cc_specular_light = vec3(0.0);
	vec3 cc_ref_vec = vec3(0.0);

	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP)) {
		cc_ref_vec = reflect(-view, geo_normal);
		cc_ref_vec = mix(cc_ref_vec, geo_normal, mix(0.001, 0.1, clearcoat_roughness));

		vec3 cc_radiance_ref_vec = scene_data.radiance_inverse_xform * cc_ref_vec;
		float roughness_lod = sqrt(mix(0.001, 0.1, clearcoat_roughness)) * MAX_ROUGHNESS_LOD;
#ifdef USE_RADIANCE_OCTMAP_ARRAY

		float lod, blend;
		blend = modf(roughness_lod, lod);

		float ref_lod = vec3_to_oct_lod(dFdx(cc_radiance_ref_vec), dFdy(cc_radiance_ref_vec), scene_data_block.data.radiance_pixel_size);
		vec2 ref_uv = vec3_to_oct_with_border(cc_radiance_ref_vec, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		vec3 clearcoat_sample_a = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ref_uv, lod), ref_lod).rgb;
		vec3 clearcoat_sample_b = textureLod(sampler2DArray(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ref_uv, lod + 1), ref_lod).rgb;
		vec3 clearcoat_light = mix(clearcoat_sample_a, clearcoat_sample_b, blend);

#else
		vec2 ref_uv = vec3_to_oct_with_border(cc_radiance_ref_vec, vec2(scene_data_block.data.radiance_border_size, 1.0 - scene_data_block.data.radiance_border_size * 2.0));
		vec3 clearcoat_light = textureLod(sampler2D(radiance_octmap, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), ref_uv, roughness_lod).rgb;

#endif //USE_RADIANCE_OCTMAP_ARRAY
		cc_specular_light += clearcoat_light * scene_data.IBL_exposure_normalization * scene_data.ambient_light_color_energy.a;
	} else if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_REFLECTION_COLOR)) {
		cc_specular_light = scene_data.reflection_color.rgb;
	}
#endif // LIGHT_CLEARCOAT_USED
#endif // !AMBIENT_LIGHT_DISABLED
#endif //!defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

	//radiance

/// GI ///
#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)
	// Directional occlusion measured by the ray-traced gather, consumed by the
	// specular occlusion term further down. Defaults describe an unoccluded
	// surface, which is what every path that does not trace leaves in place.
	vec3 rt_gi_bent_normal = normal;
	float rt_gi_visibility = 1.0;
	bool rt_gi_occlusion_valid = false;
	// The traced reflection is held back until the specular occlusion term
	// has run: a ray that hits the nearby wall already returns the wall, so
	// occluding it again by the same geometry darkens it twice. Only the
	// untraced share (sky, probes) is occluded; the traced share is added
	// after, scaled by how much of the pixel it owns.
	float rt_gi_spec_traced = 0.0;
	vec3 rt_gi_spec_traced_color = vec3(0.0);
#ifndef AMBIENT_LIGHT_DISABLED
#ifdef USE_LIGHTMAP

	//lightmap
	if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP_CAPTURE)) { //has lightmap capture
		uint index = instances.data[instance_index].gi_offset;

		// The world normal.
		vec3 wnormal = mat3(inv_view_matrix) * indirect_normal;

		// The SH coefficients used for evaluating diffuse data from SH probes.
		const float c[5] = float[](
				0.886227, // l0 				sqrt(1.0/(4.0*PI)) 	* PI
				1.023327, // l1 				sqrt(3.0/(4.0*PI)) 	* PI*2.0/3.0
				0.858086, // l2n2, l2n1, l2p1	sqrt(15.0/(4.0*PI)) * PI*1.0/4.0
				0.247708, // l20 				sqrt(5.0/(16.0*PI)) * PI*1.0/4.0
				0.429043 // l2p2 				sqrt(15.0/(16.0*PI))* PI*1.0/4.0
		);

		ambient_light += (c[0] * lightmap_captures.data[index].sh[0].rgb +
								 c[1] * lightmap_captures.data[index].sh[1].rgb * wnormal.y +
								 c[1] * lightmap_captures.data[index].sh[2].rgb * wnormal.z +
								 c[1] * lightmap_captures.data[index].sh[3].rgb * wnormal.x +
								 c[2] * lightmap_captures.data[index].sh[4].rgb * wnormal.x * wnormal.y +
								 c[2] * lightmap_captures.data[index].sh[5].rgb * wnormal.y * wnormal.z +
								 c[3] * lightmap_captures.data[index].sh[6].rgb * (3.0 * wnormal.z * wnormal.z - 1.0) +
								 c[2] * lightmap_captures.data[index].sh[7].rgb * wnormal.x * wnormal.z +
								 c[4] * lightmap_captures.data[index].sh[8].rgb * (wnormal.x * wnormal.x - wnormal.y * wnormal.y)) *
				scene_data.IBL_exposure_normalization;

	} else if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) { // has actual lightmap
		bool uses_sh = bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_SH_LIGHTMAP);
		uint ofs = instances.data[instance_index].gi_offset & 0xFFFF;
		uint slice = instances.data[instance_index].gi_offset >> 16;
		vec3 uvw;
		uvw.xy = uv2 * instances.data[instance_index].lightmap_uv_scale.zw + instances.data[instance_index].lightmap_uv_scale.xy;
		uvw.z = float(slice);

		if (uses_sh) {
			uvw.z *= 4.0; //SH textures use 4 times more data
			vec3 lm_light_l0;
			vec3 lm_light_l1n1;
			vec3 lm_light_l1_0;
			vec3 lm_light_l1p1;

			if (sc_use_lightmap_bicubic_filter()) {
				lm_light_l0 = texture_array_bicubic(lightmap_textures[ofs], uvw + vec3(0.0, 0.0, 0.0), lightmaps.data[ofs].light_texture_size).rgb;
				lm_light_l1n1 = (texture_array_bicubic(lightmap_textures[ofs], uvw + vec3(0.0, 0.0, 1.0), lightmaps.data[ofs].light_texture_size).rgb - vec3(0.5)) * 2.0;
				lm_light_l1_0 = (texture_array_bicubic(lightmap_textures[ofs], uvw + vec3(0.0, 0.0, 2.0), lightmaps.data[ofs].light_texture_size).rgb - vec3(0.5)) * 2.0;
				lm_light_l1p1 = (texture_array_bicubic(lightmap_textures[ofs], uvw + vec3(0.0, 0.0, 3.0), lightmaps.data[ofs].light_texture_size).rgb - vec3(0.5)) * 2.0;
			} else {
				lm_light_l0 = textureLod(sampler2DArray(lightmap_textures[ofs], SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, 0.0), 0.0).rgb;
				lm_light_l1n1 = (textureLod(sampler2DArray(lightmap_textures[ofs], SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, 1.0), 0.0).rgb - vec3(0.5)) * 2.0;
				lm_light_l1_0 = (textureLod(sampler2DArray(lightmap_textures[ofs], SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, 2.0), 0.0).rgb - vec3(0.5)) * 2.0;
				lm_light_l1p1 = (textureLod(sampler2DArray(lightmap_textures[ofs], SAMPLER_LINEAR_CLAMP), uvw + vec3(0.0, 0.0, 3.0), 0.0).rgb - vec3(0.5)) * 2.0;
			}

			mat3 normal_xform = mat3(lightmaps.data[ofs].normal_xform_and_specular_intensity);
			vec3 n = normalize(normal_xform * indirect_normal);
			float en = lightmaps.data[ofs].exposure_normalization;

			vec3 sh_light = lm_light_l0;
			sh_light += lm_light_l0 * lm_light_l1n1 * n.y * 4.0;
			sh_light += lm_light_l0 * lm_light_l1_0 * n.z * 4.0;
			sh_light += lm_light_l0 * lm_light_l1p1 * n.x * 4.0;
			sh_light *= lightmaps.data[ofs].exposure_normalization;
			ambient_light += sh_light;

			if (sc_use_lightmap_specular()) {
				// fake specular light to create some direct light specular lobes for directional lightmaps.
				// https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc2018-precomputedgiobalilluminationinfrostbite.pdf (slides 66-71)
				const vec3 luminance_weights = vec3(0.2126, 0.7152, 0.0722);
				vec3 l1 = vec3(
						dot(lm_light_l0 * lm_light_l1p1, luminance_weights),
						dot(lm_light_l0 * lm_light_l1n1, luminance_weights),
						dot(lm_light_l0 * lm_light_l1_0, luminance_weights));
				float l1_len = length(l1);
				float l0_luminance = dot(lm_light_l0, luminance_weights);

				if (l1_len > 1e-5 && l0_luminance > 1e-5) {
					vec3 lightmap_direction = l1 / l1_len;
					vec3 L_view = normalize(lightmap_direction * normal_xform);
					float NdotL = max(dot(normal, L_view), 0.0);

					if (NdotL > 1e-4) {
						vec3 specular_lightmap_normal = normalize(normal_xform * normal);
						vec3 specular_irradiance = lm_light_l0;
						specular_irradiance += lm_light_l0 * lm_light_l1n1 * specular_lightmap_normal.y * 4.0;
						specular_irradiance += lm_light_l0 * lm_light_l1_0 * specular_lightmap_normal.z * 4.0;
						specular_irradiance += lm_light_l0 * lm_light_l1p1 * specular_lightmap_normal.x * 4.0;
						specular_irradiance *= lightmaps.data[ofs].exposure_normalization;
						vec3 specular_light_color = max(specular_irradiance, vec3(0.0)) / max(NdotL, 0.1);

						vec3 f0 = F0(metallic, specular, albedo);

						vec3 diffuse_light_discarded = diffuse_light;
						float directionality = clamp(l1_len / l0_luminance, 0.0, 1.0);
						float specular_intensity = directionality * lightmaps.data[ofs].normal_xform_and_specular_intensity[0][3] * 2.0;

						light_compute(normal, L_view, view, 0.0, specular_light_color, true, 1.0, f0, roughness, metallic, specular_intensity, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
								backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
								transmittance_color,
								transmittance_depth,
								transmittance_boost,
								transmittance_z,
#endif
#ifdef LIGHT_RIM_USED
								rim, rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
								clearcoat, clearcoat_roughness, geo_normal,
#endif // LIGHT_CLEARCOAT_USED
#ifdef LIGHT_ANISOTROPY_USED
								binormal,
								tangent, anisotropy,
#endif
								diffuse_light_discarded,
								direct_specular_light);
					}
				}
			}

		} else {
			if (sc_use_lightmap_bicubic_filter()) {
				ambient_light += texture_array_bicubic(lightmap_textures[ofs], uvw, lightmaps.data[ofs].light_texture_size).rgb * lightmaps.data[ofs].exposure_normalization;
			} else {
				ambient_light += textureLod(sampler2DArray(lightmap_textures[ofs], SAMPLER_LINEAR_CLAMP), uvw, 0.0).rgb * lightmaps.data[ofs].exposure_normalization;
			}
		}
	}
#else

	// Bit 2 (value 4) of translucency_volume: the volume's froxels traced bounce rays,
	// so a blended fragment reads the room's indirect light from it and the
	// per-fragment SDFGI below would be that same bounce a second time. Only
	// the transparent pass sets the volume at all.
	if (sc_use_forward_gi() && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_SDFGI) && (implementation_data.translucency_volume & 4u) == 0u) { //has lightmap capture

		//make vertex orientation the world one, but still align to camera
		vec3 cam_pos = mat3(inv_view_matrix) * vertex;
		vec3 cam_normal = mat3(inv_view_matrix) * indirect_normal;
		vec3 cam_reflection = mat3(inv_view_matrix) * reflect(-view, indirect_normal);

		//apply y-mult
		cam_pos.y *= sdfgi.y_mult;
		cam_normal.y *= sdfgi.y_mult;
		cam_normal = normalize(cam_normal);
		cam_reflection.y *= sdfgi.y_mult;
		cam_normal = normalize(cam_normal);
		cam_reflection = normalize(cam_reflection);

		vec4 light_accum = vec4(0.0);
		float weight_accum = 0.0;

		vec4 light_blend_accum = vec4(0.0);
		float weight_blend_accum = 0.0;

		float blend = -1.0;

		// helper constants, compute once

		uint cascade = 0xFFFFFFFF;
		vec3 cascade_pos;
		vec3 cascade_normal;

		for (uint i = 0; i < sdfgi.max_cascades; i++) {
			cascade_pos = (cam_pos - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;

			if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
				continue; //skip cascade
			}

			cascade = i;
			break;
		}

		if (cascade < SDFGI_MAX_CASCADES) {
			bool use_specular = true;
			float blend;
			vec3 diffuse, specular;
			sdfgi_process(cascade, cascade_pos, cam_pos, cam_normal, cam_reflection, use_specular, roughness, diffuse, specular, blend);

			if (blend > 0.0) {
				//blend
				if (cascade == sdfgi.max_cascades - 1) {
					diffuse = mix(diffuse, ambient_light, blend);
					if (use_specular) {
						indirect_specular_light = mix(specular, indirect_specular_light, blend);
					}
				} else {
					vec3 diffuse2, specular2;
					float blend2;
					cascade_pos = (cam_pos - sdfgi.cascades[cascade + 1].position) * sdfgi.cascades[cascade + 1].to_probe;
					sdfgi_process(cascade + 1, cascade_pos, cam_pos, cam_normal, cam_reflection, use_specular, roughness, diffuse2, specular2, blend2);
					diffuse = mix(diffuse, diffuse2, blend);
					if (use_specular) {
						specular = mix(specular, specular2, blend);
					}
				}
			}

			ambient_light = diffuse;
			if (use_specular) {
				indirect_specular_light = specular;
			}
		}
	}

	if (sc_use_forward_gi() && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_VOXEL_GI)) { // process voxel_gi_instances
		uint index1 = instances.data[instance_index].gi_offset & 0xFFFF;
		// Make vertex orientation the world one, but still align to camera.
		vec3 cam_pos = mat3(inv_view_matrix) * vertex;
		vec3 cam_normal = mat3(inv_view_matrix) * indirect_normal;
		vec3 ref_vec = mat3(inv_view_matrix) * normalize(reflect(-view, indirect_normal));

		//find arbitrary tangent and bitangent, then build a matrix
		vec3 v0 = abs(cam_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
		vec3 tangent = normalize(cross(v0, cam_normal));
		vec3 bitangent = normalize(cross(tangent, cam_normal));
		mat3 normal_mat = mat3(tangent, bitangent, cam_normal);

		vec4 amb_accum = vec4(0.0);
		vec4 spec_accum = vec4(0.0);
		voxel_gi_compute(index1, cam_pos, cam_normal, ref_vec, normal_mat, roughness * roughness, ambient_light, indirect_specular_light, spec_accum, amb_accum);

		uint index2 = instances.data[instance_index].gi_offset >> 16;

		if (index2 != 0xFFFF) {
			voxel_gi_compute(index2, cam_pos, cam_normal, ref_vec, normal_mat, roughness * roughness, ambient_light, indirect_specular_light, spec_accum, amb_accum);
		}

		if (amb_accum.a > 0.0) {
			amb_accum.rgb /= amb_accum.a;
		}

		if (spec_accum.a > 0.0) {
			spec_accum.rgb /= spec_accum.a;
		}

		indirect_specular_light = spec_accum.rgb;
		ambient_light = amb_accum.rgb;
	}

	if (!sc_use_forward_gi() && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_GI_BUFFERS)) { //use GI buffers

		vec2 coord;

		if (implementation_data.gi_upscale_for_msaa) {
			vec2 base_coord = screen_uv;
			vec2 closest_coord = base_coord;
#ifdef USE_MULTIVIEW
			float closest_ang = dot(indirect_normal, nr_normal(textureLod(sampler2DArray(normal_roughness_buffer, SAMPLER_LINEAR_CLAMP), vec3(base_coord, ViewIndex), 0.0)));
#else // USE_MULTIVIEW
			float closest_ang = dot(indirect_normal, nr_normal(textureLod(sampler2D(normal_roughness_buffer, SAMPLER_LINEAR_CLAMP), base_coord, 0.0)));
#endif // USE_MULTIVIEW

			for (int i = 0; i < 4; i++) {
				const vec2 neighbors[4] = vec2[](vec2(-1, 0), vec2(1, 0), vec2(0, -1), vec2(0, 1));
				vec2 neighbour_coord = base_coord + neighbors[i] * scene_data.screen_pixel_size;
#ifdef USE_MULTIVIEW
				float neighbour_ang = dot(indirect_normal, nr_normal(textureLod(sampler2DArray(normal_roughness_buffer, SAMPLER_LINEAR_CLAMP), vec3(neighbour_coord, ViewIndex), 0.0)));
#else // USE_MULTIVIEW
				float neighbour_ang = dot(indirect_normal, nr_normal(textureLod(sampler2D(normal_roughness_buffer, SAMPLER_LINEAR_CLAMP), neighbour_coord, 0.0)));
#endif // USE_MULTIVIEW
				if (neighbour_ang > closest_ang) {
					closest_ang = neighbour_ang;
					closest_coord = neighbour_coord;
				}
			}

			coord = closest_coord;

		} else {
			coord = screen_uv;
		}

#ifdef USE_MULTIVIEW
		vec4 buffer_ambient = textureLod(sampler2DArray(ambient_buffer, SAMPLER_LINEAR_CLAMP), vec3(coord, ViewIndex), 0.0);
		vec4 buffer_reflection = textureLod(sampler2DArray(reflection_buffer, SAMPLER_LINEAR_CLAMP), vec3(coord, ViewIndex), 0.0);
#else // USE_MULTIVIEW
		vec4 buffer_ambient = textureLod(sampler2D(ambient_buffer, SAMPLER_LINEAR_CLAMP), coord, 0.0);
		vec4 buffer_reflection = textureLod(sampler2D(reflection_buffer, SAMPLER_LINEAR_CLAMP), coord, 0.0);
#endif // USE_MULTIVIEW

		ambient_light = mix(ambient_light, buffer_ambient.rgb, buffer_ambient.a);
		indirect_specular_light = mix(indirect_specular_light, buffer_reflection.rgb, buffer_reflection.a);
	}

	// Ray-traced indirect lighting: the gather's rays already carry sky
	// visibility and SDFGI bounce, so its demodulated irradiance replaces the
	// ambient estimate outright. With the surface cache's mirror path
	// (rt_gi bit 64, the default with the cache on) the traced reflection
	// covers every roughness and SSR is skipped; without it the reflection
	// feeds only the rough specular band and sharp reflections stay with
	// probes / SSR, whose sharpness the blurry radiance cache cannot match.
	// Reflection probes later still override inside their volumes, as with
	// SDFGI.
	if (implementation_data.rt_gi != 0u) {
		vec3 rt_gi_ambient = vec3(0.0);
		vec3 rt_gi_reflection = vec3(0.0);
		// xyz: first radiance moment (world space), w: near-field visibility.
		vec4 rt_gi_directional = vec4(0.0, 0.0, 0.0, 1.0);
		// The gather faces every normal it reads toward the viewer (a mesh wound
		// against its vertex normals writes them pointing into the surface;
		// see stochastic_indirect_gi.glsl), so its moment is expressed against
		// the viewer-facing side. The re-basing below has to compare against
		// normals facing the same way, or such a wall has its irradiance
		// re-based onto its own back and comes out half as bright.
		vec3 rt_gi_face = dot(indirect_normal, view) < 0.0 ? -indirect_normal : indirect_normal;
		// The normal the gather actually sampled around, which the directional
		// reconstruction re-bases onto this fragment's normal.
		vec3 rt_gi_gather_normal = rt_gi_face;
		bool rt_gi_valid = false;
		if ((implementation_data.rt_gi & 3u) == 1u) {
#ifdef USE_MULTIVIEW
			rt_gi_ambient = textureLod(sampler2DArray(rt_gi_ambient_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0).rgb;
			rt_gi_reflection = textureLod(sampler2DArray(rt_gi_reflection_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0).rgb;
			rt_gi_directional = textureLod(sampler2DArray(rt_gi_directional_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0);
#else
			rt_gi_ambient = textureLod(sampler2D(rt_gi_ambient_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0).rgb;
			rt_gi_reflection = textureLod(sampler2D(rt_gi_reflection_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0).rgb;
			rt_gi_directional = textureLod(sampler2D(rt_gi_directional_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0);
#endif
			rt_gi_valid = true;
		} else {
			// Half resolution: the same depth-aware upsample the stochastic
			// direct lighting uses, so GI does not bleed across silhouettes.
			// Same grid derivation as the direct lighting upsample below: round
			// the reciprocal (truncation lands one pixel short for sizes whose
			// inverse is inexact), and place the taps where the gather actually
			// sampled -- full-res pixel 2p for half-res texel p, i.e. continuous
			// coordinate 2p + 0.5, not the center of the 2x2 block.
			// The gather's scale: 2 at half resolution, 4 at a quarter (rt_gi bits 1|2).
			int rtgi_scale = 1 << int((implementation_data.rt_gi & 3u) - 1u);
			ivec2 rtgi_full_size = ivec2(round(1.0 / scene_data.screen_pixel_size));
			ivec2 rtgi_half_size = (rtgi_full_size + ivec2(rtgi_scale - 1)) / rtgi_scale;
			vec2 rtgi_pos = (screen_uv * vec2(rtgi_full_size) - 0.5) / float(rtgi_scale);
			ivec2 rtgi_base = ivec2(floor(rtgi_pos));
			vec2 rtgi_fr = rtgi_pos - vec2(rtgi_base);
			float rtgi_own_depth = -vertex.z;
			float rtgi_weight = 0.0;
			// The tap nearest this fragment by bilinear fraction, and the one the
			// stopping functions trusted most: the basis normal comes from the
			// first unless its weight was rejected, in which case the normal
			// would be the surface on the other side of an edge.
			ivec2 rtgi_near = ivec2(greaterThanEqual(rtgi_fr, vec2(0.5)));
			int rtgi_near_i = rtgi_near.x + rtgi_near.y * 2;
			float rtgi_near_w = 0.0;
			vec3 rtgi_near_n = rt_gi_face;
			float rtgi_best_w = -1.0;
			vec3 rtgi_best_n = rt_gi_face;
			// Best depth match on its own, for when every tap is rejected (the
			// direct path's thin-feature fallback): a mismatched sample is far
			// closer to the truth than the probe ambient a silhouette pixel
			// would otherwise fall back to, which reads as a crawling speckle.
			float rtgi_near_depth_w = -1.0;
			vec3 rtgi_near_ambient = vec3(0.0);
			vec3 rtgi_near_reflection = vec3(0.0);
			vec4 rtgi_near_directional = vec4(0.0);
			rt_gi_directional = vec4(0.0);
			// The fast path, as the direct composite's: the four taps on this
			// surface by one gather each of the gather's depth and guide
			// normal, then the three signals through the bilinear filter.
			bool rtgi_fast = false;
			if ((implementation_data.rt_gi & (256u | 512u)) == 512u) {
				vec2 gsize = vec2(rtgi_half_size);
				vec2 corner = (vec2(rtgi_base) + 1.0) / gsize;
#ifdef USE_MULTIVIEW
				vec4 gd = textureGather(sampler2DArray(rt_gi_depth_buffer, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 0);
				vec4 gnx = textureGather(sampler2DArray(rt_gi_guide_normal, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 0);
				vec4 gny = textureGather(sampler2DArray(rt_gi_guide_normal, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 1);
#else
				vec4 gd = textureGather(sampler2D(rt_gi_depth_buffer, SAMPLER_LINEAR_CLAMP), corner, 0);
				vec4 gnx = textureGather(sampler2D(rt_gi_guide_normal, SAMPLER_LINEAR_CLAMP), corner, 0);
				vec4 gny = textureGather(sampler2D(rt_gi_guide_normal, SAMPLER_LINEAR_CLAMP), corner, 1);
#endif
				rtgi_fast = true;
				// Gather order: (x0,y1) (x1,y1) (x1,y0) (x0,y0); the tap
				// nearest this fragment carries the normal the irradiance
				// was integrated around (the re-basing below).
				int near_g = rtgi_fr.y >= 0.5 ? (rtgi_fr.x >= 0.5 ? 1 : 0) : (rtgi_fr.x >= 0.5 ? 2 : 3);
				for (int k = 0; k < 4 && rtgi_fast; k++) {
					ivec2 goff = ivec2(k == 1 || k == 2 ? 1 : 0, k < 2 ? 1 : 0);
					ivec2 ghp = clamp(rtgi_base + goff, ivec2(0), rtgi_half_size - 1);
					ivec2 gfp = min(ghp * rtgi_scale, rtgi_full_size - ivec2(1));
					float pd = plane_depth_at_tap(inv_projection_matrix, gfp, rtgi_full_size, vertex, rt_gi_face);
					vec3 gn = nr_normal(vec4(gnx[k], gny[k], 0.0, 0.0));
					gn = dot(gn, view) < 0.0 ? -gn : gn;
					rtgi_fast = abs(gd[k] - pd) <= 0.01 * max(rtgi_own_depth, 1e-4) && dot(rt_gi_face, gn) >= 0.9;
					if (k == near_g) {
						rtgi_near_n = gn;
					}
				}
				rtgi_fast = subgroupAll(rtgi_fast);
				if (rtgi_fast) {
					vec2 guv = (rtgi_pos + 0.5) / gsize;
#ifdef USE_MULTIVIEW
					rt_gi_ambient = texture(sampler2DArray(rt_gi_ambient_buffer, SAMPLER_LINEAR_CLAMP), vec3(guv, ViewIndex)).rgb;
					rt_gi_reflection = texture(sampler2DArray(rt_gi_reflection_buffer, SAMPLER_LINEAR_CLAMP), vec3(guv, ViewIndex)).rgb;
					rt_gi_directional = texture(sampler2DArray(rt_gi_directional_buffer, SAMPLER_LINEAR_CLAMP), vec3(guv, ViewIndex));
#else
					rt_gi_ambient = texture(sampler2D(rt_gi_ambient_buffer, SAMPLER_LINEAR_CLAMP), guv).rgb;
					rt_gi_reflection = texture(sampler2D(rt_gi_reflection_buffer, SAMPLER_LINEAR_CLAMP), guv).rgb;
					rt_gi_directional = texture(sampler2D(rt_gi_directional_buffer, SAMPLER_LINEAR_CLAMP), guv);
#endif
					rtgi_weight = 1.0;
					rtgi_near_w = 1.0;
				}
			}
			for (int i = 0; i < 4 && !rtgi_fast; i++) {
				ivec2 off = ivec2(i & 1, i >> 1);
				ivec2 hp = clamp(rtgi_base + off, ivec2(0), rtgi_half_size - 1);
				// The full-res pixel the gather read its normal from (it clamps
				// the same way for odd sizes).
				ivec2 rtgi_fp = min(hp * rtgi_scale, rtgi_full_size - ivec2(1));
#ifdef USE_MULTIVIEW
				float sd = texelFetch(sampler2DArray(rt_gi_depth_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).r;
				vec3 sn = (implementation_data.rt_gi & 256u) == 0u ? nr_normal((implementation_data.rt_gi & 512u) != 0u ? texelFetch(sampler2DArray(rt_gi_guide_normal, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0) : texelFetch(sampler2DArray(normal_roughness_buffer, SAMPLER_NEAREST_CLAMP), ivec3(rtgi_fp, int(ViewIndex)), 0)) : rt_gi_face;
#else
				float sd = texelFetch(sampler2D(rt_gi_depth_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).r;
				vec3 sn = (implementation_data.rt_gi & 256u) == 0u ? nr_normal((implementation_data.rt_gi & 512u) != 0u ? texelFetch(sampler2D(rt_gi_guide_normal, SAMPLER_NEAREST_CLAMP), hp, 0) : texelFetch(sampler2D(normal_roughness_buffer, SAMPLER_NEAREST_CLAMP), rtgi_fp, 0)) : rt_gi_face;
#endif
				// Faced the way the gather faced it.
				sn = dot(sn, view) < 0.0 ? -sn : sn;
				float w = (1.0 - abs(float(off.x) - rtgi_fr.x)) * (1.0 - abs(float(off.y) - rtgi_fr.y));
				float rtgi_depth_w = exp(-abs(sd - rtgi_own_depth) / max(rtgi_own_depth * 0.1, 1e-4));
				w *= rtgi_depth_w;
				// Depth alone lets GI bleed across a silhouette where two
				// surfaces meet at similar depth but face different ways -- a
				// box edge being the everyday case. A gentler exponent than
				// the denoiser's, since half-res taps are legitimately a
				// little further apart in normal.
				w *= pow(max(dot(rt_gi_face, sn), 0.0), 8.0);
#ifdef USE_MULTIVIEW
				vec3 rtgi_tap_ambient = texelFetch(sampler2DArray(rt_gi_ambient_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).rgb;
				vec3 rtgi_tap_reflection = texelFetch(sampler2DArray(rt_gi_reflection_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).rgb;
				vec4 rtgi_tap_directional = texelFetch(sampler2DArray(rt_gi_directional_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0);
#else
				vec3 rtgi_tap_ambient = texelFetch(sampler2D(rt_gi_ambient_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).rgb;
				vec3 rtgi_tap_reflection = texelFetch(sampler2D(rt_gi_reflection_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).rgb;
				vec4 rtgi_tap_directional = texelFetch(sampler2D(rt_gi_directional_buffer, SAMPLER_NEAREST_CLAMP), hp, 0);
#endif
				rt_gi_ambient += rtgi_tap_ambient * w;
				rt_gi_reflection += rtgi_tap_reflection * w;
				rt_gi_directional += rtgi_tap_directional * w;
				// Ranked on depth alone: where the taps disagree it is the one
				// at our own depth that is right, however far it sits.
				if (rtgi_depth_w > rtgi_near_depth_w) {
					rtgi_near_depth_w = rtgi_depth_w;
					rtgi_near_ambient = rtgi_tap_ambient;
					rtgi_near_reflection = rtgi_tap_reflection;
					rtgi_near_directional = rtgi_tap_directional;
				}
				// The nearest tap is the one whose gather most likely covered
				// this fragment, so its normal is the basis the irradiance was
				// integrated around. Taken unweighted on purpose: the weighted
				// mean would converge on this fragment's own normal and the
				// reconstruction below would collapse to a no-op. It used to be
				// tap 0, which is the top-left of the footprint, nearest only
				// for a quarter of fragments and taken even with zero weight.
				if (i == rtgi_near_i) {
					rtgi_near_n = sn;
					rtgi_near_w = w;
				}
				if (w > rtgi_best_w) {
					rtgi_best_w = w;
					rtgi_best_n = sn;
				}
				rtgi_weight += w;
			}
			if (rtgi_weight > 1e-6) {
				// One weight set for all three, so the bound relating the
				// moment to the irradiance survives the upsample.
				rt_gi_ambient /= rtgi_weight;
				rt_gi_reflection /= rtgi_weight;
				rt_gi_directional /= rtgi_weight;
				rt_gi_gather_normal = rtgi_near_w > 0.05 * rtgi_weight ? rtgi_near_n : rtgi_best_n;
				rt_gi_valid = true;
			} else {
				// Every tap disagreed on depth and normal: the closest in depth,
				// on this fragment's own normal (no gather basis to re-base from).
				rt_gi_ambient = rtgi_near_ambient;
				rt_gi_reflection = rtgi_near_reflection;
				rt_gi_directional = rtgi_near_directional;
				rt_gi_gather_normal = rt_gi_face;
				rt_gi_valid = true;
			}
		}
		if (rt_gi_valid) {
			ambient_light = rt_gi_ambient;
			if (bool(implementation_data.rt_gi & 8u)) {
				// Re-base the gathered irradiance onto this fragment's own
				// normal. The gather integrates a cosine lobe around the
				// half-resolution normal, so a normal-mapped surface gets one
				// flat value across every bump. The stored first moment says
				// where the light came from, and its length relative to the
				// irradiance says how directional it was: 2/3 for a uniform
				// hemisphere (what cosine-weighted sampling yields), rising
				// toward 1 as the light concentrates in one direction.
				// The moment is stored in world space (a view-space history
				// would lag by the camera's own rotation); rotate it here,
				// where the normals live.
				vec3 gi_l1 = transpose(mat3(scene_data.inv_view_matrix)) * rt_gi_directional.xyz;
				float gi_l0 = max(dot(rt_gi_ambient, scene_data.luminance_weights.rgb), 1e-6);
				float gi_len = length(gi_l1);
				if (gi_len > 1e-6 * gi_l0) {
					vec3 gi_dir = gi_l1 / gi_len;
					// The same rays give a range-limited visibility, which the
					// specular occlusion below uses in place of its luminance
					// heuristic (a switch between the two was tried first and
					// sparkled: a converged moment sits near 2/3 on any surface
					// lit from all around, and its noise carried pixels back and
					// forth over the threshold).
					//
					// The cone's axis is the surface normal, not the moment's
					// direction. The moment is the first moment of the
					// *radiance*: it points at the light, and where the light is
					// has nothing to do with where the occluders are. Aimed by
					// it, the cone occluded every reflection that did not look
					// toward the brightest lamp -- a glossy floor beside a window
					// went dark wherever it mirrored the room, and did so slowly,
					// as the moment converged and swung the axis away from the
					// normal. A bent normal would have to be the mean of the
					// *unoccluded* directions, which these rays could give but the
					// buffer has no room for; a visibility cone on the normal is
					// the well-defined limit without one (occlusion by the
					// range-limited visibility alone, no direction claimed), and
					// it is what a bent normal map degenerates to as well.
					float gi_q = clamp(gi_len / gi_l0, 0.0, 1.0);
					rt_gi_bent_normal = normal;
					rt_gi_visibility = clamp(rt_gi_directional.w, 0.0, 1.0);
					rt_gi_occlusion_valid = bool(implementation_data.rt_gi & 16u);
					// The useful range of this ratio is narrow and it is worth
					// being exact about it. Under cosine-weighted sampling a
					// uniform hemisphere yields 2/3, and a pure cosine lobe --
					// the most directional field a single moment can describe
					// -- yields 3/4 (its moment is B/2 against an irradiance of
					// 2B/3). So 1/12 of range separates "no direction at all"
					// from "as directional as this representation goes", and
					// mapping it against 1 instead would leave the term an
					// invisible few percent.
					float gi_s = clamp((gi_q - 2.0 / 3.0) * 12.0, 0.0, 1.0) * implementation_data.rt_gi_directionality;
					gi_s = min(gi_s, 0.75); // Keeps the denominator away from zero.
					float gi_num = (1.0 - gi_s) + gi_s * max(dot(gi_dir, rt_gi_face), 0.0);
					float gi_den = (1.0 - gi_s) + gi_s * max(dot(gi_dir, rt_gi_gather_normal), 0.0);
					// Exactly 1 when the two normals agree, so flat surfaces
					// and every pixel of a scene without normal maps are
					// unchanged. The range is what a one-ray, 32-frame moment
					// can support: a factor of two either way, no more.
					ambient_light *= clamp(gi_num / max(gi_den, 0.25), 0.5, 2.0);
				}
			}
			if (bool(implementation_data.rt_gi & 4u)) {
				// With the surface cache the gather traces a mirror ray for
				// smooth surfaces too, so the traced term covers every
				// roughness and stock SSR is not run (it would overwrite the
				// traced term below where its march hits); without it the
				// smooth band stays with the probes and SSR.
				float rt_gi_spec_blend = bool(implementation_data.rt_gi & 64u) ? 1.0 : smoothstep(0.2, 0.35, roughness);
				// Composed after the specular occlusion term (see the
				// declaration): the untraced share stays here to be occluded,
				// the traced share waits.
				rt_gi_spec_traced = rt_gi_spec_blend;
				rt_gi_spec_traced_color = rt_gi_reflection;
				indirect_specular_light *= 1.0 - rt_gi_spec_blend;
			}
		}
	}
#endif // !USE_LIGHTMAP

	if (bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_USE_SSAO)) {
#ifdef USE_MULTIVIEW
		float ssao = texture(sampler2DArray(ao_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex)).r;
#else
		float ssao = texture(sampler2D(ao_buffer, SAMPLER_LINEAR_CLAMP), screen_uv).r;
#endif
		ao = min(ao, ssao);
		ao_light_affect = mix(ao_light_affect, max(ao_light_affect, implementation_data.ssao_light_affect), implementation_data.ssao_ao_affect);
	}

	{ // process reflections

		vec4 reflection_accum = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 ambient_accum = vec4(0.0, 0.0, 0.0, 0.0);
#ifdef LIGHT_CLEARCOAT_USED
		vec4 cc_reflection_accum = vec4(0.0, 0.0, 0.0, 0.0);
#endif

		uint cluster_reflection_offset = cluster_offset + implementation_data.cluster_type_size * 4;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_reflection_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = subgroupBroadcastFirst(subgroupMin(item_from));
		item_to = subgroupBroadcastFirst(subgroupMax(item_to));

#ifdef LIGHT_ANISOTROPY_USED
		// https://google.github.io/filament/Filament.html#lighting/imagebasedlights/anisotropy
		vec3 anisotropic_direction = anisotropy >= 0.0 ? binormal : tangent;
		vec3 anisotropic_tangent = cross(anisotropic_direction, view);
		vec3 anisotropic_normal = cross(anisotropic_tangent, anisotropic_direction);
		vec3 bent_normal = normalize(mix(normal, anisotropic_normal, abs(anisotropy) * clamp(5.0 * roughness, 0.0, 1.0)));
#else
		vec3 bent_normal = normal;
#endif
		vec3 ref_vec = normalize(reflect(-view, bent_normal));
		// Interpolate between mirror and rough reflection by using linear_roughness * linear_roughness.
		ref_vec = mix(ref_vec, bent_normal, roughness * roughness * roughness * roughness);

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_reflection_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = subgroupBroadcastFirst(subgroupOr(mask));
			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) { //do not process if not originally here
					continue;
				}

				uint reflection_index = 32 * i + bit;

				if (!bool(reflections.data[reflection_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

#ifndef LIGHT_CLEARCOAT_USED
				if (reflection_accum.a >= 1.0 && ambient_accum.a >= 1.0) {
					break;
				}
#else
				if (reflection_accum.a >= 1.0 && cc_reflection_accum.a >= 1.0 && ambient_accum.a >= 1.0) {
					break;
				}
#endif // LIGHT_CLEARCOAT_USED

				reflection_process(reflection_index, vertex, ref_vec, normal, roughness, ambient_light,
#ifdef LIGHT_CLEARCOAT_USED
						cc_ref_vec, mix(0.001, 0.1, clearcoat_roughness), cc_reflection_accum,
#endif
						ambient_accum, reflection_accum);
			}
		}

		if (ambient_accum.a < 1.0) {
			ambient_accum.rgb = ambient_light * (1.0 - ambient_accum.a) + ambient_accum.rgb;
		}

		if (reflection_accum.a < 1.0) {
			reflection_accum.rgb = indirect_specular_light * (1.0 - reflection_accum.a) + reflection_accum.rgb;
		}

#ifdef LIGHT_CLEARCOAT_USED
		if (cc_reflection_accum.a < 1.0) {
			cc_reflection_accum.rgb = cc_specular_light * (1.0 - cc_reflection_accum.a) + cc_reflection_accum.rgb;
		}
#endif

		if (reflection_accum.a > 0.0) {
			indirect_specular_light = reflection_accum.rgb;
			// Probes keep owning indirect specular: the traced share yields
			// to them exactly as the untraced share did above.
			rt_gi_spec_traced *= 1.0 - min(reflection_accum.a, 1.0);
		}

#ifdef LIGHT_CLEARCOAT_USED
		if (cc_reflection_accum.a > 0.0) {
			cc_specular_light = cc_reflection_accum.rgb;
		}
#endif

#if !defined(USE_LIGHTMAP)
		// A probe's ambient is the diffuse lighting captured at bake time. With
		// a traced per-frame irradiance available it is strictly the staler of
		// the two, and letting it overwrite the live value is what pins interior
		// GI to whatever the light was when the probe was captured. Probes keep
		// owning indirect specular, which is the term they actually resolve
		// better than a blurry radiance cache does.
		if (ambient_accum.a > 0.0 && !bool(implementation_data.rt_gi & 32u)) {
			ambient_light = ambient_accum.rgb;
		}
#endif
	}

	//finalize ambient light here
	{
		// Experiment (GODOT_GI_NO_AO): the material's and the screen-space
		// occlusion left out of the ambient term, to measure what the GI
		// gather's screen reads inherit from it (section 33).
		if (bool(implementation_data.rt_gi & 128u)) {
			ao = 1.0;
		}
#ifdef MULTI_BOUNCE_OCCLUSION_ENABLED
		// Apply multi-bounce ambient-occlusion approximation to ambient light:
		// https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf#page=78
		{
			vec3 a = vec3(2.0404) * albedo - vec3(0.3324);
			vec3 b = vec3(-4.7951) * albedo + vec3(0.6417);
			vec3 c = vec3(2.7552) * albedo + vec3(0.6903);
			vec3 x = vec3(ao);
			vec3 bounce_occlusion = max(x, ((x * a + b) * x + c) * x);
			ambient_light *= bounce_occlusion;
		}
#else // MULTI_BOUNCE_OCCLUSION_ENABLED
		ambient_light *= ao;
#endif // MULTI_BOUNCE_OCCLUSION_ENABLED
#ifndef SPECULAR_OCCLUSION_DISABLED
		// A bent normal plus a visibility cone gives a real answer here; a
		// bent normal map is one source of them, and the ray-traced gather is
		// another, measured rather than authored. Its visibility is range
		// limited, which is what makes it usable: raw ray occlusion would
		// count a wall a hundred meters away as an occluder.
		vec3 so_bent_normal;
		float so_visibility;
		bool so_use_cone;
#ifdef BENT_NORMAL_MAP_USED
		so_bent_normal = bent_normal_vector;
		so_visibility = ao;
		so_use_cone = true;
#else
		so_bent_normal = rt_gi_bent_normal;
		so_visibility = rt_gi_visibility;
		so_use_cone = rt_gi_occlusion_valid;
#endif // BENT_NORMAL_MAP_USED
		float specular_occlusion;
		if (so_use_cone) {
			// Apply cone to cone intersection with cosine weighted assumption:
			// https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf
			float cos_a_v = sqrt(1.0 - so_visibility);
			float limited_roughness = max(roughness, 0.01); // Avoid artifacts at really low roughness.
			float cos_a_s = exp2((-log(10.0) / log(2.0)) * limited_roughness * limited_roughness);
			float cos_b = dot(so_bent_normal, reflect(-view, normal));

			// Intersection between the spherical caps of the visibility and specular cone.
			// Based on Christopher Oat and Pedro V. Sander's "Ambient aperture lighting":
			// https://advances.realtimerendering.com/s2006/Chapter8-Ambient_Aperture_Lighting.pdf
			float r1 = acos(cos_a_v);
			float r2 = acos(cos_a_s);
			float d = acos(cos_b);
			float area = 0.0;

			if (d <= max(r1, r2) - min(r1, r2)) {
				// One cap is enclosed in the other.
				area = M_TAU - M_TAU * max(cos_a_v, cos_a_s);
			} else if (d >= r1 + r2) {
				// No intersection.
				area = 0.0;
			} else {
				float delta = abs(r1 - r2);
				float x = 1.0 - clamp((d - delta) / (r1 + r2 - delta), 0.0, 1.0);
				area = smoothstep(0.0, 1.0, x);
				area *= M_TAU - M_TAU * max(cos_a_v, cos_a_s);
			}

			specular_occlusion = area / (M_TAU * (1.0 - cos_a_s));
		} else {
			specular_occlusion = (ambient_light.r * 0.3 + ambient_light.g * 0.59 + ambient_light.b * 0.11) * 2.0; // Luminance of ambient light.
			specular_occlusion = min(specular_occlusion * 4.0, 1.0); // This multiplication preserves speculars on bright areas.

			float reflective_f = (1.0 - roughness) * metallic;
			// 10.0 is a magic number, it gives the intended effect in most scenarios.
			// Low enough for occlusion, high enough for reaction to lights and shadows.
			specular_occlusion = max(min(reflective_f * specular_occlusion * 10.0, 1.0), specular_occlusion);
		}
		indirect_specular_light *= specular_occlusion;
#endif // SPECULAR_OCCLUSION_DISABLED
	   // The traced reflection, occluded by its own ray and by nothing else.
		indirect_specular_light += rt_gi_spec_traced_color * rt_gi_spec_traced;
		ambient_light *= albedo.rgb;

		if (bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_USE_SSIL)) {
#ifdef USE_MULTIVIEW
			vec4 ssil = textureLod(sampler2DArray(ssil_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0);
#else
			vec4 ssil = textureLod(sampler2D(ssil_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0);
#endif // USE_MULTIVIEW
			ambient_light *= 1.0 - ssil.a;
			ambient_light += ssil.rgb * albedo.rgb;
		}

		//process ssr
		if (bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_USE_SSR)) {
			bool resolve_ssr = bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_RESOLVE_SSR);

			float ssr_mip_level = 0.0;
			if (resolve_ssr) {
#ifdef USE_MULTIVIEW
				ssr_mip_level = textureLod(sampler2DArray(ssr_mip_level_buffer, SAMPLER_NEAREST_CLAMP), vec3(screen_uv, ViewIndex), 0.0).x;
#else
				ssr_mip_level = textureLod(sampler2D(ssr_mip_level_buffer, SAMPLER_NEAREST_CLAMP), screen_uv, 0.0).x;
#endif // USE_MULTIVIEW

				ssr_mip_level *= 14.0;
			}

#ifdef USE_MULTIVIEW
			vec4 ssr = textureLod(sampler2DArray(ssr_buffer, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(screen_uv, ViewIndex), ssr_mip_level);
#else
			vec4 ssr = textureLod(sampler2D(ssr_buffer, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), screen_uv, ssr_mip_level);
#endif // USE_MULTIVIEW

			if (resolve_ssr) {
				const vec3 rec709_luminance_weights = vec3(0.2126, 0.7152, 0.0722);
				ssr.rgb /= 1.0 - dot(ssr.rgb, rec709_luminance_weights);
			}

			// Apply fade when approaching 0.7 roughness to smoothen the harsh cutoff in the main SSR trace pass.
			ssr *= smoothstep(0.0, 1.0, 1.0 - clamp((roughness - 0.6) / (0.7 - 0.6), 0.0, 1.0));

			// Alpha is premultiplied.
			indirect_specular_light = indirect_specular_light * (1.0 - ssr.a) + ssr.rgb;
		}
	}
#endif // AMBIENT_LIGHT_DISABLED

	// convert ao to direct light ao
	ao = mix(1.0, ao, ao_light_affect);

	//this saves some VGPRs
	vec3 f0 = F0(metallic, specular, albedo);

#ifdef LIGHT_CLEARCOAT_USED
	// The base layer's f0 is computed assuming an interface from air to an IOR
	// of 1.5, but the clear coat layer forms an interface from IOR 1.5 to IOR
	// 1.5. We recompute f0 by first computing its IOR, then reconverting to f0
	// by using the correct interface
	f0 = mix(f0, f0_Clear_Coat_To_Surface(f0), clearcoat);
#endif

#ifndef AMBIENT_LIGHT_DISABLED
	{
#if defined(DIFFUSE_TOON)
		//simplify for toon, as
		indirect_specular_light *= specular * metallic * albedo * 2.0;
#else
		// Base Layer
		float NdotV = clamp(dot(normal, view), 0.0001, 1.0);
		vec2 envBRDF = prefiltered_dfg(roughness, NdotV);
		// Multiscattering
		energy_compensation = get_energy_compensation(f0, envBRDF.y);

		// cheap luminance approximation
		float f90 = clamp(50.0 * f0.g, metallic, 1.0);
		indirect_specular_light *= energy_compensation * ((f90 - f0) * envBRDF.x + f0 * envBRDF.y);

#ifdef LIGHT_CLEARCOAT_USED
		float geo_NdotV = max(dot(geo_normal, view), 0.0001); // We want to use geometric normal, not normal_map
		// The clearcoat layer assumes an IOR of 1.5 (4% reflectance).
		// Attenuate underlying diffuse/specular by clearcoat fresnel (ONLY fresnel, hence we don't just invert the BRDF below).
		float NdotV5 = SchlickFresnel(geo_NdotV);
		float F = mix(0.04, 1.0, NdotV5) * clearcoat;
		float cc_attenuation = 1.0 - F;

		ambient_light *= cc_attenuation;
		indirect_specular_light *= cc_attenuation;

		// Clearcoat Layer
		// We don't need DFG for clearcoat, so we can use the fresnel directly.
		indirect_specular_light += cc_specular_light * F;
#endif

#endif // DIFFUSE_TOON
	}

#endif // !AMBIENT_LIGHT_DISABLED
#endif //GI !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

// LIGHTING
#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

#ifdef USE_VERTEX_LIGHTING
	diffuse_light += diffuse_light_interp.rgb;
	direct_specular_light += specular_light_interp.rgb * f0;
#endif

	// The translucency lighting volume (transparent pass): a blended
	// fragment reads its direct light from the froxel grid the ray tracer
	// filled this frame (Raytracing::process_translucency_volume) in
	// place of the light loops and shadow rays below. The fragments an alpha
	// depth pre-pass wrote keep the per-fragment shading unless the flag's
	// second bit says otherwise: they are the surface's opaque core, where
	// the shadow detail shows; the fringe is what the blend hides. The
	// diffuse term is the L1 irradiance for the fragment's normal; the
	// specular one treats the volume's dominant direction as a directional
	// light of the fraction of the light that comes from it.
	bool tv_active = false;
#ifndef USE_VERTEX_LIGHTING
	if (implementation_data.translucency_volume != 0u) {
		bool tv_core_fragment = false;
#ifdef USE_OPAQUE_PREPASS
		tv_core_fragment = alpha >= 0.99; // The pre-pass's threshold (scene_data's is zero in this pass).
#endif
		if (!tv_core_fragment || (implementation_data.translucency_volume & 2u) != 0u) {
			float tv_d = -vertex.z;
			if (tv_d > 0.0 && tv_d < implementation_data.tv_length) {
				vec3 tv_unit;
				tv_unit.xy = (vertex.xy / (tv_d * implementation_data.tv_inv_proj_xy.xy)) * 0.5 + 0.5;
				tv_unit.z = pow(tv_d / implementation_data.tv_length, 1.0 / implementation_data.tv_spread);
				if (all(greaterThanEqual(tv_unit, vec3(0.0))) && all(lessThanEqual(tv_unit, vec3(1.0)))) {
					tv_active = true;
					vec3 tv_a = textureLod(sampler3D(translucency_volume_a, SAMPLER_LINEAR_CLAMP), tv_unit, 0.0).rgb;
					vec3 tv_bx = textureLod(sampler3D(translucency_volume_bx, SAMPLER_LINEAR_CLAMP), tv_unit, 0.0).rgb;
					vec3 tv_by = textureLod(sampler3D(translucency_volume_by, SAMPLER_LINEAR_CLAMP), tv_unit, 0.0).rgb;
					vec3 tv_bz = textureLod(sampler3D(translucency_volume_bz, SAMPLER_LINEAR_CLAMP), tv_unit, 0.0).rgb;
					vec3 tv_e = max(0.25 * tv_a + 0.5 * (tv_bx * normal.x + tv_by * normal.y + tv_bz * normal.z), vec3(0.0));
					diffuse_light += tv_e * (1.0 / M_PI);
					const vec3 tv_lum = scene_data.luminance_weights.rgb;
					vec3 tv_dom = vec3(dot(tv_bx, tv_lum), dot(tv_by, tv_lum), dot(tv_bz, tv_lum));
					float tv_dom_len = length(tv_dom);
					float tv_a_lum = dot(tv_a, tv_lum);
					if (tv_dom_len > 1e-5 && tv_a_lum > 1e-5) {
						float tv_fraction = clamp(tv_dom_len / tv_a_lum, 0.0, 1.0);
						vec3 tv_diffuse_unused = vec3(0.0);
						light_compute(normal, tv_dom / tv_dom_len, normalize(view), 0.0, tv_a * tv_fraction, true, 1.0, f0, roughness, metallic, 0.5, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
								backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
								transmittance_color,
								transmittance_depth,
								transmittance_boost,
								transmittance_depth,
#endif
#ifdef LIGHT_RIM_USED
								rim, rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
								clearcoat, clearcoat_roughness, geo_normal,
#endif // LIGHT_CLEARCOAT_USED
#ifdef LIGHT_ANISOTROPY_USED
								binormal,
								tangent, anisotropy,
#endif
								tv_diffuse_unused,
								direct_specular_light);
					}
				}
			}
		}
	}
#endif // !USE_VERTEX_LIGHTING

	{ // Directional light.

		// Do shadow and lighting in two passes to reduce register pressure.
#ifndef SHADOWS_DISABLED
		uint shadow0 = 0;
		uint shadow1 = 0;

		float shadowmask = 1.0;

#ifdef USE_LIGHTMAP
		uint shadowmask_mode = LIGHTMAP_SHADOWMASK_MODE_NONE;

		if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
			const uint ofs = instances.data[instance_index].gi_offset & 0xFFFF;
			shadowmask_mode = lightmaps.data[ofs].flags;

			if (shadowmask_mode != LIGHTMAP_SHADOWMASK_MODE_NONE) {
				const uint slice = instances.data[instance_index].gi_offset >> 16;
				const vec2 scaled_uv = uv2 * instances.data[instance_index].lightmap_uv_scale.zw + instances.data[instance_index].lightmap_uv_scale.xy;
				const vec3 uvw = vec3(scaled_uv, float(slice));

				if (sc_use_lightmap_bicubic_filter()) {
					shadowmask = texture_array_bicubic(lightmap_textures[MAX_LIGHTMAP_TEXTURES + ofs], uvw, lightmaps.data[ofs].light_texture_size).x;
				} else {
					shadowmask = textureLod(sampler2DArray(lightmap_textures[MAX_LIGHTMAP_TEXTURES + ofs], SAMPLER_LINEAR_CLAMP), uvw, 0.0).x;
				}
			}
		}

		if (shadowmask_mode != LIGHTMAP_SHADOWMASK_MODE_ONLY) {
#endif // USE_LIGHTMAP

#ifdef USE_VERTEX_LIGHTING
			// Only process the first light's shadow for vertex lighting.
			for (uint i = 0; i < 1; i++) {
#else
		for (uint i = 0; i < 8; i++) {
			if (i >= scene_data.directional_light_count) {
				break;
			}
#endif

				if (!bool(directional_lights.data[i].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (directional_lights.data[i].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				float shadow = 1.0;

				// The ray traced mask owns the first directional light's shadow
				// when active; its shadow map is stale and must not be sampled.
				// Same for the transparent pass on the frames it traces the
				// sun itself: the cascades were not rendered either.
				if (directional_lights.data[i].shadow_opacity > 0.001 && !((implementation_data.rt_sun_shadow != 0u || RT_TRANSPARENT_SUN_RAY) && i == 0u)) {
					float depth_z = -vertex.z;
					vec3 light_dir = directional_lights.data[i].direction;
					vec3 base_normal_bias = geo_normal * (1.0 - max(0.0, dot(light_dir, -geo_normal)));

#define BIAS_FUNC(m_var, m_idx) \
	m_var.xyz += light_dir * directional_lights.data[i].shadow_bias[m_idx]; \
	vec3 normal_bias = base_normal_bias * directional_lights.data[i].shadow_normal_bias[m_idx]; \
	normal_bias -= light_dir * dot(light_dir, normal_bias); \
	m_var.xyz += normal_bias;

					//version with soft shadows, more expensive
					if (sc_use_directional_soft_shadows() && directional_lights.data[i].softshadow_angle > 0) {
						uint blend_count = 0;
						const uint blend_max = directional_lights.data[i].blend_splits ? 2 : 1;

						if (depth_z < directional_lights.data[i].shadow_split_offsets.x) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 0)

							vec4 pssm_coord = (directional_lights.data[i].shadow_matrix1 * v);
							pssm_coord /= pssm_coord.w;

							float range_pos = dot(directional_lights.data[i].direction, v.xyz);
							float range_begin = directional_lights.data[i].shadow_range_begin.x;
							float test_radius = (range_pos - range_begin) * directional_lights.data[i].softshadow_angle;
							vec2 tex_scale = directional_lights.data[i].uv_scale1 * test_radius;
							shadow = sample_directional_soft_shadow(directional_shadow_atlas, pssm_coord.xyz, tex_scale * directional_lights.data[i].soft_shadow_scale, scene_data.taa_frame_count);
							blend_count++;
						}

						if (blend_count < blend_max && depth_z < directional_lights.data[i].shadow_split_offsets.y) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 1)

							vec4 pssm_coord = (directional_lights.data[i].shadow_matrix2 * v);
							pssm_coord /= pssm_coord.w;

							float range_pos = dot(directional_lights.data[i].direction, v.xyz);
							float range_begin = directional_lights.data[i].shadow_range_begin.y;
							float test_radius = (range_pos - range_begin) * directional_lights.data[i].softshadow_angle;
							vec2 tex_scale = directional_lights.data[i].uv_scale2 * test_radius;
							float s = sample_directional_soft_shadow(directional_shadow_atlas, pssm_coord.xyz, tex_scale * directional_lights.data[i].soft_shadow_scale, scene_data.taa_frame_count);

							if (blend_count == 0) {
								shadow = s;
							} else {
								//blend
								float blend = smoothstep(0.0, directional_lights.data[i].shadow_split_offsets.x, depth_z);
								shadow = mix(shadow, s, blend);
							}

							blend_count++;
						}

						if (blend_count < blend_max && depth_z < directional_lights.data[i].shadow_split_offsets.z) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 2)

							vec4 pssm_coord = (directional_lights.data[i].shadow_matrix3 * v);
							pssm_coord /= pssm_coord.w;

							float range_pos = dot(directional_lights.data[i].direction, v.xyz);
							float range_begin = directional_lights.data[i].shadow_range_begin.z;
							float test_radius = (range_pos - range_begin) * directional_lights.data[i].softshadow_angle;
							vec2 tex_scale = directional_lights.data[i].uv_scale3 * test_radius;
							float s = sample_directional_soft_shadow(directional_shadow_atlas, pssm_coord.xyz, tex_scale * directional_lights.data[i].soft_shadow_scale, scene_data.taa_frame_count);

							if (blend_count == 0) {
								shadow = s;
							} else {
								//blend
								float blend = smoothstep(directional_lights.data[i].shadow_split_offsets.x, directional_lights.data[i].shadow_split_offsets.y, depth_z);
								shadow = mix(shadow, s, blend);
							}

							blend_count++;
						}

						if (blend_count < blend_max && depth_z < directional_lights.data[i].shadow_split_offsets.w) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 3)

							vec4 pssm_coord = (directional_lights.data[i].shadow_matrix4 * v);
							pssm_coord /= pssm_coord.w;

							float range_pos = dot(directional_lights.data[i].direction, v.xyz);
							float range_begin = directional_lights.data[i].shadow_range_begin.w;
							float test_radius = (range_pos - range_begin) * directional_lights.data[i].softshadow_angle;
							vec2 tex_scale = directional_lights.data[i].uv_scale4 * test_radius;
							float s = sample_directional_soft_shadow(directional_shadow_atlas, pssm_coord.xyz, tex_scale * directional_lights.data[i].soft_shadow_scale, scene_data.taa_frame_count);

							if (blend_count == 0) {
								shadow = s;
							} else {
								//blend
								float blend = smoothstep(directional_lights.data[i].shadow_split_offsets.y, directional_lights.data[i].shadow_split_offsets.z, depth_z);
								shadow = mix(shadow, s, blend);
							}
						}

					} else { //no soft shadows

						vec4 pssm_coord;
						float blur_factor;

						if (depth_z < directional_lights.data[i].shadow_split_offsets.x) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 0)

							pssm_coord = (directional_lights.data[i].shadow_matrix1 * v);
							blur_factor = 1.0;
						} else if (depth_z < directional_lights.data[i].shadow_split_offsets.y) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 1)

							pssm_coord = (directional_lights.data[i].shadow_matrix2 * v);
							// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
							blur_factor = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.y;
						} else if (depth_z < directional_lights.data[i].shadow_split_offsets.z) {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 2)

							pssm_coord = (directional_lights.data[i].shadow_matrix3 * v);
							// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
							blur_factor = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.z;
						} else {
							vec4 v = vec4(vertex, 1.0);

							BIAS_FUNC(v, 3)

							pssm_coord = (directional_lights.data[i].shadow_matrix4 * v);
							// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
							blur_factor = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.w;
						}

						pssm_coord /= pssm_coord.w;

						shadow = sample_directional_pcf_shadow(directional_shadow_atlas, scene_data.directional_shadow_pixel_size * directional_lights.data[i].soft_shadow_scale * (blur_factor + (1.0 - blur_factor) * float(directional_lights.data[i].blend_splits)), pssm_coord, scene_data.taa_frame_count);

						if (directional_lights.data[i].blend_splits) {
							float pssm_blend;
							float blur_factor2;

							if (depth_z < directional_lights.data[i].shadow_split_offsets.x) {
								vec4 v = vec4(vertex, 1.0);
								BIAS_FUNC(v, 1)
								pssm_coord = (directional_lights.data[i].shadow_matrix2 * v);
								pssm_blend = smoothstep(directional_lights.data[i].shadow_split_offsets.x - directional_lights.data[i].shadow_split_offsets.x * 0.1, directional_lights.data[i].shadow_split_offsets.x, depth_z);
								// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
								blur_factor2 = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.y;
							} else if (depth_z < directional_lights.data[i].shadow_split_offsets.y) {
								vec4 v = vec4(vertex, 1.0);
								BIAS_FUNC(v, 2)
								pssm_coord = (directional_lights.data[i].shadow_matrix3 * v);
								pssm_blend = smoothstep(directional_lights.data[i].shadow_split_offsets.y - directional_lights.data[i].shadow_split_offsets.y * 0.1, directional_lights.data[i].shadow_split_offsets.y, depth_z);
								// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
								blur_factor2 = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.z;
							} else if (depth_z < directional_lights.data[i].shadow_split_offsets.z) {
								vec4 v = vec4(vertex, 1.0);
								BIAS_FUNC(v, 3)
								pssm_coord = (directional_lights.data[i].shadow_matrix4 * v);
								pssm_blend = smoothstep(directional_lights.data[i].shadow_split_offsets.z - directional_lights.data[i].shadow_split_offsets.z * 0.1, directional_lights.data[i].shadow_split_offsets.z, depth_z);
								// Adjust shadow blur with reference to the first split to reduce discrepancy between shadow splits.
								blur_factor2 = directional_lights.data[i].shadow_split_offsets.x / directional_lights.data[i].shadow_split_offsets.w;
							} else {
								pssm_blend = 0.0; //if no blend, same coord will be used (divide by z will result in same value, and already cached)
								blur_factor2 = 1.0;
							}

							pssm_coord /= pssm_coord.w;

							float shadow2 = sample_directional_pcf_shadow(directional_shadow_atlas, scene_data.directional_shadow_pixel_size * directional_lights.data[i].soft_shadow_scale * (blur_factor2 + (1.0 - blur_factor2) * float(directional_lights.data[i].blend_splits)), pssm_coord, scene_data.taa_frame_count);
							shadow = mix(shadow, shadow2, pssm_blend);
						}
					}

#ifdef USE_LIGHTMAP
					if (shadowmask_mode == LIGHTMAP_SHADOWMASK_MODE_REPLACE) {
						shadow = mix(shadow, shadowmask, smoothstep(directional_lights.data[i].fade_from, directional_lights.data[i].fade_to, vertex.z)); //done with negative values for performance
					} else if (shadowmask_mode == LIGHTMAP_SHADOWMASK_MODE_OVERLAY) {
						shadow = shadowmask * mix(shadow, 1.0, smoothstep(directional_lights.data[i].fade_from, directional_lights.data[i].fade_to, vertex.z)); //done with negative values for performance
					} else {
#endif
						shadow = mix(shadow, 1.0, smoothstep(directional_lights.data[i].fade_from, directional_lights.data[i].fade_to, vertex.z)); //done with negative values for performance
#ifdef USE_LIGHTMAP
					}
#endif

#ifdef USE_VERTEX_LIGHTING
					diffuse_light *= mix(1.0, shadow, diffuse_light_interp.a);
					direct_specular_light *= mix(1.0, shadow, specular_light_interp.a);
#endif

#undef BIAS_FUNC

					//process sscs
					if (bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_USE_SSCS) && directional_lights.data[i].sscs_index != 0xFFFFFFFF) {
#ifdef USE_MULTIVIEW
						float sscs_layer = float(directional_lights.data[i].sscs_index * 2u + uint(ViewIndex));
#else
					float sscs_layer = float(directional_lights.data[i].sscs_index);
#endif // USE_MULTIVIEW
						float sscs_shadow = textureLod(sampler2DArray(sscs_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, sscs_layer), 0.0).r;

						shadow = min(shadow, sscs_shadow);
					}
				} // shadows

				if (i < 4) {
					shadow0 |= uint(clamp(shadow * 255.0, 0.0, 255.0)) << (i * 8);
				} else {
					shadow1 |= uint(clamp(shadow * 255.0, 0.0, 255.0)) << ((i - 4) * 8);
				}
			}

#ifdef USE_LIGHTMAP
		} else { // shadowmask_mode == LIGHTMAP_SHADOWMASK_MODE_ONLY

#ifdef USE_VERTEX_LIGHTING
			diffuse_light *= mix(1.0, shadowmask, diffuse_light_interp.a);
			direct_specular_light *= mix(1.0, shadowmask, specular_light_interp.a);
#endif

			shadow0 |= uint(clamp(shadowmask * 255.0, 0.0, 255.0));
		}
#endif // USE_LIGHTMAP

#endif // SHADOWS_DISABLED

#ifndef USE_VERTEX_LIGHTING

		for (uint i = 0; i < 8 && !tv_active; i++) {
			if (i >= scene_data.directional_light_count) {
				break;
			}

			if (!bool(directional_lights.data[i].mask & instances.data[instance_index].layer_mask)) {
				continue; //not masked
			}

			if (directional_lights.data[i].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
				continue; // Statically baked light and object uses lightmap, skip
			}

#ifdef LIGHT_TRANSMITTANCE_USED
			float transmittance_z = transmittance_depth;
#ifndef SHADOWS_DISABLED
			if (directional_lights.data[i].shadow_opacity > 0.001) {
				float depth_z = -vertex.z;

				if (depth_z < directional_lights.data[i].shadow_split_offsets.x) {
					vec4 trans_vertex = vec4(vertex - geo_normal * directional_lights.data[i].shadow_transmittance_bias.x, 1.0);
					vec4 trans_coord = directional_lights.data[i].shadow_matrix1 * trans_vertex;
					trans_coord /= trans_coord.w;

					float shadow_z = textureLod(sampler2D(directional_shadow_atlas, SAMPLER_LINEAR_CLAMP), trans_coord.xy, 0.0).r;
					shadow_z *= directional_lights.data[i].shadow_z_range.x;
					float z = trans_coord.z * directional_lights.data[i].shadow_z_range.x;

					transmittance_z = z - shadow_z;
				} else if (depth_z < directional_lights.data[i].shadow_split_offsets.y) {
					vec4 trans_vertex = vec4(vertex - geo_normal * directional_lights.data[i].shadow_transmittance_bias.y, 1.0);
					vec4 trans_coord = directional_lights.data[i].shadow_matrix2 * trans_vertex;
					trans_coord /= trans_coord.w;

					float shadow_z = textureLod(sampler2D(directional_shadow_atlas, SAMPLER_LINEAR_CLAMP), trans_coord.xy, 0.0).r;
					shadow_z *= directional_lights.data[i].shadow_z_range.y;
					float z = trans_coord.z * directional_lights.data[i].shadow_z_range.y;

					transmittance_z = z - shadow_z;
				} else if (depth_z < directional_lights.data[i].shadow_split_offsets.z) {
					vec4 trans_vertex = vec4(vertex - geo_normal * directional_lights.data[i].shadow_transmittance_bias.z, 1.0);
					vec4 trans_coord = directional_lights.data[i].shadow_matrix3 * trans_vertex;
					trans_coord /= trans_coord.w;

					float shadow_z = textureLod(sampler2D(directional_shadow_atlas, SAMPLER_LINEAR_CLAMP), trans_coord.xy, 0.0).r;
					shadow_z *= directional_lights.data[i].shadow_z_range.z;
					float z = trans_coord.z * directional_lights.data[i].shadow_z_range.z;

					transmittance_z = z - shadow_z;
				} else {
					vec4 trans_vertex = vec4(vertex - geo_normal * directional_lights.data[i].shadow_transmittance_bias.w, 1.0);
					vec4 trans_coord = directional_lights.data[i].shadow_matrix4 * trans_vertex;
					trans_coord /= trans_coord.w;

					float shadow_z = textureLod(sampler2D(directional_shadow_atlas, SAMPLER_LINEAR_CLAMP), trans_coord.xy, 0.0).r;
					shadow_z *= directional_lights.data[i].shadow_z_range.w;
					float z = trans_coord.z * directional_lights.data[i].shadow_z_range.w;

					transmittance_z = z - shadow_z;
				}
			}
#endif // !SHADOWS_DISABLED
#endif // LIGHT_TRANSMITTANCE_USED

			float shadow = 1.0;
#ifndef SHADOWS_DISABLED
			if (i < 4) {
				shadow = float(shadow0 >> (i * 8u) & 0xFFu) / 255.0;
			} else {
				shadow = float(shadow1 >> ((i - 4u) * 8u) & 0xFFu) / 255.0;
			}

			shadow = mix(1.0, shadow, directional_lights.data[i].shadow_opacity);
#endif

			// Ray-traced shadow mask (bound to a white texture when inactive).
			// Only the first directional light is traced for now; the light's
			// shadow opacity applies to the traced result too.
			if (i == 0) {
#ifdef RT_TRANSPARENT_SHADOWS
				if (RT_TRANSPARENT_SUN_RAY) {
					// The screen-space mask is keyed to the opaque depth, so
					// a transparent surface floating in front of it would
					// wear whatever shadow lies behind it. Trace the sun from
					// the fragment instead (hard: the pass has no history to
					// resolve a penumbra into).
					if (directional_lights.data[i].shadow_opacity > 0.001 && implementation_data.rt_sun_caster_mask != 0u) {
						bool sun_visible = rt_transparent_trace_visible_dir(vertex, geo_normal, directional_lights.data[i].direction, implementation_data.rt_sun_caster_mask);
						shadow = min(shadow, sun_visible ? 1.0 : 1.0 - directional_lights.data[i].shadow_opacity);
					}
				} else
#endif
				{
#ifdef USE_MULTIVIEW
					float rt_mask = texture(sampler2DArray(rt_shadow_mask, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex)).r;
#else
					float rt_mask = texture(sampler2D(rt_shadow_mask, SAMPLER_LINEAR_CLAMP), screen_uv).r;
#endif
					shadow = min(shadow, mix(1.0, rt_mask, directional_lights.data[i].shadow_opacity));
				}
			}

			blur_shadow(shadow);

#ifdef DEBUG_DRAW_PSSM_SPLITS
			vec3 tint = vec3(1.0);
			if (-vertex.z < directional_lights.data[i].shadow_split_offsets.x) {
				tint = vec3(1.0, 0.0, 0.0);
			} else if (-vertex.z < directional_lights.data[i].shadow_split_offsets.y) {
				tint = vec3(0.0, 1.0, 0.0);
			} else if (-vertex.z < directional_lights.data[i].shadow_split_offsets.z) {
				tint = vec3(0.0, 0.0, 1.0);
			} else {
				tint = vec3(1.0, 1.0, 0.0);
			}
			tint = mix(tint, vec3(1.0), shadow);
			shadow = 1.0;
#endif

			// The sun's angular size broadens the lobe whether or not the
			// PCSS soft-shadow gate is on (ray-traced sun shadows leave it
			// off; size is stored as 1-cos(angle), zero for sizeless lights).
			float size_A = directional_lights.data[i].size;

			light_compute(normal, directional_lights.data[i].direction, normalize(view), size_A,
#ifndef DEBUG_DRAW_PSSM_SPLITS
					directional_lights.data[i].color * directional_lights.data[i].energy,
#else
					directional_lights.data[i].color * directional_lights.data[i].energy * tint,
#endif
					true, shadow, f0, roughness, metallic, directional_lights.data[i].specular, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
					backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
					transmittance_color,
					transmittance_depth,
					transmittance_boost,
					transmittance_z,
#endif
#ifdef LIGHT_RIM_USED
					rim, rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
					clearcoat, clearcoat_roughness, geo_normal,
#endif // LIGHT_CLEARCOAT_USED
#ifdef LIGHT_ANISOTROPY_USED
					binormal,
					tangent, anisotropy,
#endif
					diffuse_light,
					direct_specular_light);
		}
#endif // USE_VERTEX_LIGHTING
	}

#ifdef RT_TRANSPARENT_SHADOWS
	// Local lights below shade analytically; hand them a ray budget only on
	// the frames their shadow maps were skipped (see rt_transparent_* above).
	rt_transparent_ray_budget = (!LOCAL_SHADOW_MAPS_RENDERED && bool(implementation_data.rt_transparent_shadows & 1u)) ? implementation_data.rt_transparent_max_rays : 0u;
#endif

	// Half resolution, composited per pixel (stochastic_direct_lights == 2):
	// the three loops below run unshadowed into sums of their own, this
	// pixel's analytic term with its own normal, which the composite after
	// them multiplies by the upsampled visibility ratios. (Declared outside
	// the vertex-lighting guard: the composite reads them either way.)
	bool stochastic_pixel_analytic = (implementation_data.stochastic_direct_lights & 3u) == 2u;
	vec3 analytic_px_diffuse = vec3(0.0);
	vec3 analytic_px_specular = vec3(0.0);
#ifndef USE_VERTEX_LIGHTING
	vec3 saved_diffuse_light = diffuse_light;
	vec3 saved_direct_specular_light = direct_specular_light;
	if (stochastic_pixel_analytic) {
		diffuse_light = vec3(0.0);
		direct_specular_light = vec3(0.0);
		stochastic_analytic_unshadowed = true;
	}
	bool analytic_lights_here = (implementation_data.stochastic_direct_lights == 0u || stochastic_pixel_analytic) && !tv_active;
	if (analytic_lights_here) { //omni lights

		uint cluster_omni_offset = cluster_offset;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_omni_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = subgroupBroadcastFirst(subgroupMin(item_from));
		item_to = subgroupBroadcastFirst(subgroupMax(item_to));

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_omni_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = subgroupBroadcastFirst(subgroupOr(mask));
			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) { //do not process if not originally here
					continue;
				}

				uint light_index = 32 * i + bit;

				if (!bool(omni_lights.data[light_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (omni_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				light_process_omni(light_index, vertex, view, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, scene_data.taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
						backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
						transmittance_color,
						transmittance_depth,
						transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
						rim,
						rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
						clearcoat, clearcoat_roughness, geo_normal,
#endif // LIGHT_CLEARCOAT_USED
#ifdef LIGHT_ANISOTROPY_USED
						binormal, tangent, anisotropy,
#endif
						diffuse_light, direct_specular_light);
			}
		}
	}

	if (analytic_lights_here) { //spot lights

		uint cluster_spot_offset = cluster_offset + implementation_data.cluster_type_size;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_spot_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = subgroupBroadcastFirst(subgroupMin(item_from));
		item_to = subgroupBroadcastFirst(subgroupMax(item_to));

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_spot_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = subgroupBroadcastFirst(subgroupOr(mask));
			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) { //do not process if not originally here
					continue;
				}

				uint light_index = 32 * i + bit;

				if (!bool(spot_lights.data[light_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (spot_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				light_process_spot(light_index, vertex, view, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, scene_data.taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
						backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
						transmittance_color,
						transmittance_depth,
						transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
						rim,
						rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
						clearcoat, clearcoat_roughness, geo_normal,
#endif // LIGHT_CLEARCOAT_USED
#ifdef LIGHT_ANISOTROPY_USED
						binormal, tangent, anisotropy,
#endif
						diffuse_light, direct_specular_light);
			}
		}
	}

	// Area lights are owned by the stochastic pass when it is active.
	if (sc_cluster_has_area_light() && analytic_lights_here) { // area lights

		uint cluster_area_offset = cluster_offset + implementation_data.cluster_type_size * 2;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_area_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = subgroupBroadcastFirst(subgroupMin(item_from));
		item_to = subgroupBroadcastFirst(subgroupMax(item_to));

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_area_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = subgroupBroadcastFirst(subgroupOr(mask));
			while (merged_mask != 0) {
				uint bit = findMSB(merged_mask);
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) { //do not process if not originally here
					continue;
				}

				uint light_index = 32 * i + bit;

				if (!bool(area_lights.data[light_index].mask & instances.data[instance_index].layer_mask)) {
					continue; //not masked
				}

				if (area_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_LIGHTMAP)) {
					continue; // Statically baked light and object uses lightmap, skip
				}

				light_process_area(light_index, vertex, view, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, scene_data.taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
						backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
						transmittance_color,
						transmittance_depth,
						transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
						rim,
						rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
						clearcoat, clearcoat_roughness, geo_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
						binormal, tangent, anisotropy,
#endif
						diffuse_light, direct_specular_light);
			}
		}
	}
	if (stochastic_pixel_analytic) {
		analytic_px_diffuse = diffuse_light;
		analytic_px_specular = direct_specular_light;
		diffuse_light = saved_diffuse_light;
		direct_specular_light = saved_direct_specular_light;
		stochastic_analytic_unshadowed = false;
	}
#endif // !USE_VERTEX_LIGHTING

	// Stochastic direct lighting: omni/spot/area
	// contribution computed by the ray-traced compute pass. Demodulated:
	// albedo, AO and metallic are applied by the common composite below.
	// The specular buffer holds the lobe without its Fresnel term (rgb) and the
	// Schlick weight (a): the compute pass has no material, so the Fresnel is
	// applied here with this fragment's f0 / f90, as light_compute would. That
	// is what lets metals and colored f0 keep their highlights.
	float stochastic_f90 = clamp(50.0 * f0.g, metallic, 1.0);
	if ((implementation_data.stochastic_direct_lights & 3u) == 1u) {
#ifdef USE_MULTIVIEW
		diffuse_light += textureLod(sampler2DArray(stochastic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0).rgb;
		vec4 stochastic_spec = textureLod(sampler2DArray(stochastic_specular_buffer, SAMPLER_LINEAR_CLAMP), vec3(screen_uv, ViewIndex), 0.0);
#else
		diffuse_light += textureLod(sampler2D(stochastic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0).rgb;
		vec4 stochastic_spec = textureLod(sampler2D(stochastic_specular_buffer, SAMPLER_LINEAR_CLAMP), screen_uv, 0.0);
#endif
		direct_specular_light += stochastic_spec.rgb * (f0 + (stochastic_f90 - f0) * stochastic_spec.a) * energy_compensation;
	} else if ((implementation_data.stochastic_direct_lights & 3u) >= 2u) {
		// Half resolution buffers: depth-aware upsample. Each half-res texel
		// stores the view depth it was lit at; weight the four nearest by
		// bilinear distance and how well that depth matches ours, so lighting
		// does not bleed across silhouettes.
		//
		// Two forms. 3 (the first, GODOT_RT_HALF_ANALYTIC=0): the buffers hold
		// the denoised ratios already multiplied by the half-res analytic
		// term, and the product is upsampled. 2: the buffers hold the ratios
		// alone, and they are multiplied here by this pixel's own analytic
		// term (the loops above, unshadowed) plus the upsampled image lights'
		// share, which this shader cannot evaluate. The analytic term carries
		// the normal map: at half resolution it is one pixel's normal for the
		// block, blended back at the fragment's jittered position, and on a
		// bumpy wall under grazing light that is a mottle that never settles
		// (2.4x the full-res frame-to-frame change with no upscaler, 8.5x
		// under MetalFX's jitter, plan section 52). The ratios are smooth;
		// the bumps belong to the pixel.
		bool pixel_analytic = (implementation_data.stochastic_direct_lights & 3u) == 2u;
		// The sampling grid's scale: 2, or 4 at the quarter tier (bit 2).
		int stochastic_scale = (implementation_data.stochastic_direct_lights & 4u) != 0u ? 4 : 2;
		// Bit 3: no image lights this frame (no planar mirror in the scene),
		// so the two image buffers are zero and are not read. Bit 4:
		// diagnostics, the taps' normal test skipped (GODOT_RT_OPAQUE_ABLATE=normal).
		bool read_images = pixel_analytic && (implementation_data.stochastic_direct_lights & 8u) == 0u;
		bool stochastic_normal_test = (implementation_data.stochastic_direct_lights & 16u) == 0u;
		// Bit 5: the taps' normals from the denoisers' guide at the sampling
		// grid's resolution, one packed texel a tap, instead of the
		// full-resolution buffer at a stride of the scale (the four
		// strided fetches were 2.3 ms of the opaque pass on the TPS bridge
		// at 1080p, against 0.5 of the whole test with them gone).
		bool stochastic_guide = (implementation_data.stochastic_direct_lights & 32u) != 0u;
		//
		// Round rather than truncate: screen_pixel_size is 1/size, and its
		// reciprocal lands just under the integer for sizes whose inverse is
		// not exactly representable. One pixel short here misaligns the whole
		// grid.
		ivec2 full_size = ivec2(round(1.0 / scene_data.screen_pixel_size));
		ivec2 half_size = (full_size + ivec2(stochastic_scale - 1)) / stochastic_scale;
		// The sampling pass lit full-res pixel 2 * p for half-res texel p, so
		// its result describes continuous full-res coordinate 2p + 0.5 -- the
		// center of that one pixel, not the center of the 2x2 block. Placing
		// the taps at block centers instead (screen_uv * half_size - 0.5) put
		// every weight half a full pixel off. Solve 2p + 0.5 <= f + 0.5 for the
		// tap index. The denoised signal is smooth enough that correcting this
		// is not visible in the scenes measured so far; it is fixed because the
		// reconstruction should stand on the sampling grid that exists.
		vec2 pos = (screen_uv * vec2(full_size) - 0.5) / float(stochastic_scale);
		ivec2 base = ivec2(floor(pos));
		vec2 fr = pos - vec2(base);
		float own_depth = -vertex.z;
		vec3 up_diffuse = vec3(0.0);
		vec4 up_specular = vec4(0.0);
		vec3 up_image_diffuse = vec3(0.0);
		vec4 up_image_specular = vec4(0.0);
		vec3 up_analytic_diffuse = vec3(0.0);
		vec4 up_analytic_specular = vec4(0.0);
		float up_weight = 0.0;
		// Best depth match, kept for when no tap agrees well enough to trust
		// the blend.
		vec3 near_diffuse = vec3(0.0);
		vec4 near_specular = vec4(0.0);
		vec3 near_image_diffuse = vec3(0.0);
		vec4 near_image_specular = vec4(0.0);
		vec3 near_analytic_diffuse = vec3(0.0);
		vec4 near_analytic_specular = vec4(0.0);
		float near_depth_weight = -1.0;
		// The pixel's normal faced the way the sampling pass faces the
		// prepass normal it lit each texel with (toward the viewer), for the
		// normal test below.
		vec3 stochastic_face = dot(normal, view) < 0.0 ? -normal : normal;
		// The fast path. The opaque pass is latency-bound on this shader,
		// and each texture instruction of the composite costs about the
		// same whatever it reads (a strided full-resolution fetch and a
		// packed one alike, measured on the TPS bridge at 1080p: the four
		// normal fetches 2.3 ms either way). So the count is what matters:
		// where the four taps sit on this fragment's surface -- nearly
		// every fragment -- one gather each of the sampling grid's depth
		// and guide normal decides it, and the signals are read with the
		// hardware's bilinear filter at the same weights the loop below
		// computes, one instruction a signal instead of four. Edges and
		// thin features take the loop.
		bool stochastic_fast = false;
		vec2 stochastic_guv = vec2(0.0);
		if (stochastic_guide && stochastic_normal_test) {
			vec2 gsize = vec2(half_size);
			vec2 corner = (vec2(base) + 1.0) / gsize;
#ifdef USE_MULTIVIEW
			vec4 gd = textureGather(sampler2DArray(stochastic_depth_buffer, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 0);
			vec4 gnx = textureGather(sampler2DArray(stochastic_guide_normal, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 0);
			vec4 gny = textureGather(sampler2DArray(stochastic_guide_normal, SAMPLER_LINEAR_CLAMP), vec3(corner, ViewIndex), 1);
#else
			vec4 gd = textureGather(sampler2D(stochastic_depth_buffer, SAMPLER_LINEAR_CLAMP), corner, 0);
			vec4 gnx = textureGather(sampler2D(stochastic_guide_normal, SAMPLER_LINEAR_CLAMP), corner, 0);
			vec4 gny = textureGather(sampler2D(stochastic_guide_normal, SAMPLER_LINEAR_CLAMP), corner, 1);
#endif
			// Within two percent of the depth (the loop's weight is then
			// above 0.8 on every tap) and a degree or so of the normal
			// (its weight above 0.9): the normalized blend is then the
			// bilinear one to within a few percent of relative weight.
			// Each tap against the depth this fragment's plane has along
			// the tap's own view ray (a percent of the depth), and within
			// 25 degrees of the normal (the guide's normal is the mapped
			// one, and a bumpy floor's taps sit further apart than an
			// edge test would like; the loop's weight is still 0.43 there,
			// and an edge between surfaces is far past it): the loop's
			// weights are then all near one and its normalized blend is
			// the bilinear one.
			stochastic_fast = true;
			for (int k = 0; k < 4 && stochastic_fast; k++) {
				// Gather order: (x0,y1) (x1,y1) (x1,y0) (x0,y0).
				ivec2 goff = ivec2(k == 1 || k == 2 ? 1 : 0, k < 2 ? 1 : 0);
				ivec2 ghp = clamp(base + goff, ivec2(0), half_size - 1);
				ivec2 gfp = min(ghp * stochastic_scale, full_size - ivec2(1));
				float pd = plane_depth_at_tap(inv_projection_matrix, gfp, full_size, vertex, stochastic_face);
				vec3 gn = nr_normal(vec4(gnx[k], gny[k], 0.0, 0.0));
				gn = dot(gn, view) < 0.0 ? -gn : gn;
				stochastic_fast = abs(gd[k] - pd) <= 0.01 * max(own_depth, 1e-4) && dot(stochastic_face, gn) >= 0.9;
			}
			stochastic_guv = (pos + 0.5) / gsize;
			// One verdict for the SIMD group: a lane on the loop makes the
			// group execute it, and with a slow lane in most groups (every
			// edge) the fast lanes' gathers only added to the count. The
			// group takes the fast path when every lane may.
			stochastic_fast = subgroupAll(stochastic_fast);
		}
		if (stochastic_fast) {
#ifdef USE_MULTIVIEW
			up_diffuse = texture(sampler2DArray(stochastic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex)).rgb;
			up_specular = texture(sampler2DArray(stochastic_specular_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex));
			if (read_images) {
				up_image_diffuse = texture(sampler2DArray(stochastic_image_diffuse_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex)).rgb;
				up_image_specular = texture(sampler2DArray(stochastic_image_specular_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex));
			}
			if (pixel_analytic && tv_active) {
				up_analytic_diffuse = texture(sampler2DArray(stochastic_analytic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex)).rgb;
				up_analytic_specular = texture(sampler2DArray(stochastic_analytic_specular_buffer, SAMPLER_LINEAR_CLAMP), vec3(stochastic_guv, ViewIndex));
			}
#else
			up_diffuse = texture(sampler2D(stochastic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv).rgb;
			up_specular = texture(sampler2D(stochastic_specular_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv);
			if (read_images) {
				up_image_diffuse = texture(sampler2D(stochastic_image_diffuse_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv).rgb;
				up_image_specular = texture(sampler2D(stochastic_image_specular_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv);
			}
			if (pixel_analytic && tv_active) {
				up_analytic_diffuse = texture(sampler2D(stochastic_analytic_diffuse_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv).rgb;
				up_analytic_specular = texture(sampler2D(stochastic_analytic_specular_buffer, SAMPLER_LINEAR_CLAMP), stochastic_guv);
			}
#endif
			up_weight = 1.0;
		}
		for (int i = 0; i < 4 && !stochastic_fast; i++) {
			ivec2 off = ivec2(i & 1, i >> 1);
			ivec2 hp = clamp(base + off, ivec2(0), half_size - 1);
			// The full-res pixel the sampling pass lit texel hp at (it clamps
			// the same way for odd sizes).
			ivec2 fp = min(hp * stochastic_scale, full_size - ivec2(1));
			vec3 tap_image_diffuse = vec3(0.0);
			vec4 tap_image_specular = vec4(0.0);
			vec3 tap_analytic_diffuse = vec3(0.0);
			vec4 tap_analytic_specular = vec4(0.0);
#ifdef USE_MULTIVIEW
			float sd = texelFetch(sampler2DArray(stochastic_depth_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).r;
			vec3 sn = stochastic_normal_test ? nr_normal(stochastic_guide ? texelFetch(sampler2DArray(stochastic_guide_normal, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0) : texelFetch(sampler2DArray(normal_roughness_buffer, SAMPLER_NEAREST_CLAMP), ivec3(fp, int(ViewIndex)), 0)) : stochastic_face;
			vec3 tap_diffuse = texelFetch(sampler2DArray(stochastic_diffuse_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).rgb;
			vec4 tap_specular = texelFetch(sampler2DArray(stochastic_specular_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0);
			if (read_images) {
				tap_image_diffuse = texelFetch(sampler2DArray(stochastic_image_diffuse_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).rgb;
				tap_image_specular = texelFetch(sampler2DArray(stochastic_image_specular_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0);
			}
			if (pixel_analytic) {
				if (tv_active) {
					tap_analytic_diffuse = texelFetch(sampler2DArray(stochastic_analytic_diffuse_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0).rgb;
					tap_analytic_specular = texelFetch(sampler2DArray(stochastic_analytic_specular_buffer, SAMPLER_NEAREST_CLAMP), ivec3(hp, int(ViewIndex)), 0);
				}
			}
#else
			float sd = texelFetch(sampler2D(stochastic_depth_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).r;
			vec3 sn = stochastic_normal_test ? nr_normal(stochastic_guide ? texelFetch(sampler2D(stochastic_guide_normal, SAMPLER_NEAREST_CLAMP), hp, 0) : texelFetch(sampler2D(normal_roughness_buffer, SAMPLER_NEAREST_CLAMP), fp, 0)) : stochastic_face;
			vec3 tap_diffuse = texelFetch(sampler2D(stochastic_diffuse_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).rgb;
			vec4 tap_specular = texelFetch(sampler2D(stochastic_specular_buffer, SAMPLER_NEAREST_CLAMP), hp, 0);
			if (read_images) {
				tap_image_diffuse = texelFetch(sampler2D(stochastic_image_diffuse_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).rgb;
				tap_image_specular = texelFetch(sampler2D(stochastic_image_specular_buffer, SAMPLER_NEAREST_CLAMP), hp, 0);
			}
			if (pixel_analytic) {
				if (tv_active) {
					tap_analytic_diffuse = texelFetch(sampler2D(stochastic_analytic_diffuse_buffer, SAMPLER_NEAREST_CLAMP), hp, 0).rgb;
					tap_analytic_specular = texelFetch(sampler2D(stochastic_analytic_specular_buffer, SAMPLER_NEAREST_CLAMP), hp, 0);
				}
			}
#endif
			float depth_weight = exp(-abs(sd - own_depth) / max(own_depth * 0.1, 1e-4));
			float w = (1.0 - abs(float(off.x) - fr.x)) * (1.0 - abs(float(off.y) - fr.y)) * depth_weight;
			// The depth test alone (a tenth of the depth) lets a shelf's edge
			// take the wall's visibility a hand behind it, and a wall's top
			// the ceiling's: with the ratios raw (the denoiser off) that is a
			// one-pixel bright line along every silhouette whose two sides
			// see a light differently, and denoised a faint rim (plan section
			// 62). The GI upsample's normal test, with the prepass normal the
			// sampling pass lit the texel with, keeps the taps on this
			// pixel's own surface; the exponent tolerates a normal map's bumps.
			sn = dot(sn, view) < 0.0 ? -sn : sn;
			w *= pow(max(dot(stochastic_face, sn), 0.0), 8.0);
			up_diffuse += tap_diffuse * w;
			up_specular += tap_specular * w;
			up_image_diffuse += tap_image_diffuse * w;
			up_image_specular += tap_image_specular * w;
			up_analytic_diffuse += tap_analytic_diffuse * w;
			up_analytic_specular += tap_analytic_specular * w;
			up_weight += w;
			// Ranked on depth alone: where the taps disagree it is the one at
			// our own depth that is right, however far it sits on screen.
			if (depth_weight > near_depth_weight) {
				near_depth_weight = depth_weight;
				near_diffuse = tap_diffuse;
				near_specular = tap_specular;
				near_image_diffuse = tap_image_diffuse;
				near_image_specular = tap_image_specular;
				near_analytic_diffuse = tap_analytic_diffuse;
				near_analytic_specular = tap_analytic_specular;
			}
		}
		// Any positive sum normalizes correctly, however small -- this only has
		// to catch the case where every tap was rejected to nothing.
		vec4 stochastic_spec;
		vec3 image_diffuse;
		vec4 image_specular;
		vec3 half_analytic_diffuse;
		vec4 half_analytic_specular;
		if (up_weight > 1e-6) {
			up_diffuse /= up_weight;
			stochastic_spec = up_specular / up_weight;
			image_diffuse = up_image_diffuse / up_weight;
			image_specular = up_image_specular / up_weight;
			half_analytic_diffuse = up_analytic_diffuse / up_weight;
			half_analytic_specular = up_analytic_specular / up_weight;
		} else {
			// Every tap disagreed on depth. Adding nothing, as this did, leaves
			// the pixel with no direct lighting whatsoever -- a black speckle
			// that crawls along silhouettes as the camera moves. A single
			// mismatched sample is far closer to the truth than darkness.
			up_diffuse = near_diffuse;
			stochastic_spec = near_specular;
			image_diffuse = near_image_diffuse;
			image_specular = near_image_specular;
			half_analytic_diffuse = near_analytic_diffuse;
			half_analytic_specular = near_analytic_specular;
		}
		if ((implementation_data.stochastic_direct_lights & 64u) != 0u) {
			// Diagnostics (GODOT_RT_OPAQUE_ABLATE=paint): green where the
			// fast path served the fragment, red where the loop did.
			diffuse_light = stochastic_fast ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
			direct_specular_light = vec3(0.0);
			ambient_light = vec3(0.0);
		} else if (pixel_analytic) {
			// The ratios (a luminance visibility fraction, replicated over
			// rgb) times the analytic term: this pixel's own for the real
			// lights, the half-res buffer's for the images, and -- where the
			// translucency volume shades the fragment and the loops above did
			// not run -- the half-res whole, as the first form had it.
			vec3 ratio_d = up_diffuse;
			vec3 ratio_s = stochastic_spec.rgb;
			vec3 image_spec = image_specular.rgb * (f0 + (stochastic_f90 - f0) * image_specular.a) * energy_compensation;
			if (tv_active) {
				diffuse_light += ratio_d * half_analytic_diffuse;
				direct_specular_light += ratio_s * half_analytic_specular.rgb * (f0 + (stochastic_f90 - f0) * half_analytic_specular.a) * energy_compensation;
			} else {
				diffuse_light += ratio_d * (analytic_px_diffuse + image_diffuse);
				direct_specular_light += ratio_s * (analytic_px_specular + image_spec);
			}
		} else {
			diffuse_light += up_diffuse;
			direct_specular_light += stochastic_spec.rgb * (f0 + (stochastic_f90 - f0) * stochastic_spec.a) * energy_compensation;
		}
	}
#endif //!defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)

#ifdef USE_SHADOW_TO_OPACITY
#ifndef MODE_RENDER_DEPTH
	alpha = min(alpha, clamp(length(ambient_light), 0.0, 1.0));

#if defined(ALPHA_SCISSOR_USED)
#ifdef MODE_RENDER_MATERIAL
	if (alpha < alpha_scissor_threshold) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
#endif // MODE_RENDER_MATERIAL
#endif // ALPHA_SCISSOR_USED

#endif // !MODE_RENDER_DEPTH
#endif // USE_SHADOW_TO_OPACITY

#ifdef MODE_RENDER_DEPTH

#ifdef MODE_RENDER_SDF

	{
		vec3 local_pos = (implementation_data.sdf_to_bounds * vec4(vertex, 1.0)).xyz;
		ivec3 grid_pos = implementation_data.sdf_offset + ivec3(local_pos * vec3(implementation_data.sdf_size));

		uint albedo16 = 0x1; //solid flag
		albedo16 |= clamp(uint(albedo.r * 31.0), 0, 31) << 11;
		albedo16 |= clamp(uint(albedo.g * 31.0), 0, 31) << 6;
		albedo16 |= clamp(uint(albedo.b * 31.0), 0, 31) << 1;

		imageStore(albedo_volume_grid, grid_pos, uvec4(albedo16));

		uint facing_bits = 0;
		const vec3 aniso_dir[6] = vec3[](
				vec3(1, 0, 0),
				vec3(0, 1, 0),
				vec3(0, 0, 1),
				vec3(-1, 0, 0),
				vec3(0, -1, 0),
				vec3(0, 0, -1));

		vec3 cam_normal = mat3(inv_view_matrix) * normalize(normal_interp);

		float closest_dist = -1e20;

		for (uint i = 0; i < 6; i++) {
			float d = dot(cam_normal, aniso_dir[i]);
			if (d > closest_dist) {
				closest_dist = d;
				facing_bits = (1 << i);
			}
		}

#ifdef NO_IMAGE_ATOMICS
		imageStore(geom_facing_grid, grid_pos, uvec4(imageLoad(geom_facing_grid, grid_pos).r | facing_bits)); //store facing bits
#else
		imageAtomicOr(geom_facing_grid, grid_pos, facing_bits); //store facing bits
#endif

		if (length(emission) > 0.001) {
			float lumas[6];
			vec3 light_total = vec3(0);

			for (int i = 0; i < 6; i++) {
				float strength = max(0.0, dot(cam_normal, aniso_dir[i]));
				vec3 light = emission * strength;
				light_total += light;
				lumas[i] = max(light.r, max(light.g, light.b));
			}

			float luma_total = max(light_total.r, max(light_total.g, light_total.b));

			uint light_aniso = 0;

			for (int i = 0; i < 6; i++) {
				light_aniso |= min(31, uint((lumas[i] / luma_total) * 31.0)) << (i * 5);
			}

			//compress to RGBE9995 to save space

			const float pow2to9 = 512.0f;
			const float B = 15.0f;
			const float N = 9.0f;
			const float LN2 = 0.6931471805599453094172321215;

			float cRed = clamp(light_total.r, 0.0, 65408.0);
			float cGreen = clamp(light_total.g, 0.0, 65408.0);
			float cBlue = clamp(light_total.b, 0.0, 65408.0);

			float cMax = max(cRed, max(cGreen, cBlue));

			float expp = max(-B - 1.0f, floor(log(cMax) / LN2)) + 1.0f + B;

			float sMax = floor((cMax / pow(2.0f, expp - B - N)) + 0.5f);

			float exps = expp + 1.0f;

			if (0.0 <= sMax && sMax < pow2to9) {
				exps = expp;
			}

			float sRed = floor((cRed / pow(2.0f, exps - B - N)) + 0.5f);
			float sGreen = floor((cGreen / pow(2.0f, exps - B - N)) + 0.5f);
			float sBlue = floor((cBlue / pow(2.0f, exps - B - N)) + 0.5f);
			//store as 8985 to have 2 extra neighbor bits
			uint light_rgbe = ((uint(sRed) & 0x1FFu) >> 1) | ((uint(sGreen) & 0x1FFu) << 8) | (((uint(sBlue) & 0x1FFu) >> 1) << 17) | ((uint(exps) & 0x1Fu) << 25);

			imageStore(emission_grid, grid_pos, uvec4(light_rgbe));
			imageStore(emission_aniso_grid, grid_pos, uvec4(light_aniso));
		}
	}

#endif

#ifdef MODE_RENDER_MATERIAL

	albedo_output_buffer.rgb = albedo;
	vec3 card_f0 = vec3(0.0);
	if (!bool(scene_data.flags & SCENE_DATA_FLAGS_USE_UV2_MATERIAL)) {
		// The surface cache's card capture: the cards bounce diffusely and
		// hold one radiance per texel, so a surface's specular reflectance
		// (its F0: the metal's albedo, a dielectric's 4%) joins its diffuse
		// albedo as if Lambertian -- the energy right, the direction not.
		// Measured (plan section 40): a metal mirror floor's cards taken as
		// its raw albedo lit the radiosity box's walls to 0.85-0.89 of the
		// image-method truth, taken as diffuse alone (albedo * (1 - metallic))
		// to 0.39-0.56; the game's floor is metallic 0.45 and read 21% darker
		// on diffuse alone. The lightmapper's UV2 capture keeps the raw albedo.
		vec3 f0 = mix(vec3(0.16 * specular * specular), albedo, metallic);
		albedo_output_buffer.rgb = albedo * (1.0 - metallic) + f0;
		card_f0 = f0;
	}
	albedo_output_buffer.a = alpha;

	normal_output_buffer.rgb = encode24(normal) * 0.5 + 0.5;
	normal_output_buffer.a = 0.0;
	depth_output_buffer.r = -vertex.z;

	orm_output_buffer.r = ao;
	orm_output_buffer.g = roughness;
	orm_output_buffer.b = metallic;
	orm_output_buffer.a = sss_strength;
	if (!bool(scene_data.flags & SCENE_DATA_FLAGS_USE_UV2_MATERIAL)) {
		// The cards' specular atlas: the F0 the albedo above folded in, and
		// the roughness, so a planar mirror's texels can take the fold back
		// out (surface_cache_light.glsl read_texel).
		orm_output_buffer = vec4(card_f0, roughness);
	}

	emission_output_buffer.rgb = emission;
	emission_output_buffer.a = 0.0;
#endif

#ifdef MODE_RENDER_NORMAL_ROUGHNESS
	// Octahedral normal, ten-bit roughness, the dynamic flag (normal_roughness_inc.glsl).
	normal_roughness_output_buffer = nr_encode(normal, roughness, bool(instances.data[instance_index].flags & INSTANCE_FLAGS_DYNAMIC));

	// The material the colour pass will shade with, for the ray traced
	// passes that run in between (albedo_f0_inc.glsl).
#ifdef MODE_UNSHADED
	gbuf_albedo_output_buffer = gb_encode_albedo(albedo, 0.0, true);
	gbuf_f0_output_buffer = vec4(0.0);
#else
	gbuf_albedo_output_buffer = gb_encode_albedo(albedo, metallic, false);
	gbuf_f0_output_buffer = gb_encode_f0(albedo, metallic, specular);
#endif

#ifdef MODE_RENDER_VOXEL_GI
	if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_USE_VOXEL_GI)) { // process voxel_gi_instances
		uint index1 = instances.data[instance_index].gi_offset & 0xFFFF;
		uint index2 = instances.data[instance_index].gi_offset >> 16;
		voxel_gi_buffer.x = index1 & 0xFFu;
		voxel_gi_buffer.y = index2 & 0xFFu;
	} else {
		voxel_gi_buffer.x = 0xFF;
		voxel_gi_buffer.y = 0xFF;
	}
#endif

#endif //MODE_RENDER_NORMAL_ROUGHNESS

//nothing happens, so a tree-ssa optimizer will result in no fragment shader :)
#else

	// multiply by albedo
	diffuse_light *= albedo; // ambient must be multiplied by albedo at the end

	// apply direct light AO
	diffuse_light *= ao;
	direct_specular_light *= ao;

	// apply metallic
	diffuse_light *= 1.0 - metallic;
	ambient_light *= 1.0 - metallic;

#ifndef FOG_DISABLED
	//restore fog
	fog = vec4(unpackHalf2x16(fog_rg), unpackHalf2x16(fog_ba));
#endif //!FOG_DISABLED

#ifdef MODE_SEPARATE_SPECULAR

#ifdef MODE_UNSHADED
	diffuse_buffer = vec4(albedo.rgb, 0.0);
	specular_buffer = vec4(0.0);

#else

#ifdef SSS_MODE_SKIN
	sss_strength = -sss_strength;
#endif
	diffuse_buffer = vec4(emission + diffuse_light + ambient_light, sss_strength);
	specular_buffer = vec4(direct_specular_light + indirect_specular_light, metallic);
#endif

#ifndef FOG_DISABLED
	diffuse_buffer.rgb = diffuse_buffer.rgb * fog.a + fog.rgb;
	specular_buffer.rgb = specular_buffer.rgb * fog.a;
#endif //!FOG_DISABLED

#else //MODE_SEPARATE_SPECULAR

	alpha *= scene_data.pass_alpha_multiplier;

#ifdef MODE_UNSHADED
	frag_color = vec4(albedo, alpha);
#else
	frag_color = vec4(emission + ambient_light + diffuse_light + direct_specular_light + indirect_specular_light, alpha);
//frag_color = vec4(1.0);
#endif //USE_NO_SHADING

#ifndef FOG_DISABLED
	frag_color.rgb = frag_color.rgb * fog.a + fog.rgb;
#endif //!FOG_DISABLED

#if defined(PREMUL_ALPHA_USED) && !defined(MODE_RENDER_DEPTH)
	frag_color.rgb *= premul_alpha;
#endif //PREMUL_ALPHA_USED

#endif //MODE_SEPARATE_SPECULAR

#endif //MODE_RENDER_DEPTH
#ifdef MOTION_VECTORS
	vec2 position_clip = (screen_position.xy / screen_position.w) - scene_data.taa_jitter;
	vec2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - scene_data_block.prev_data.taa_jitter;

	vec2 position_uv = position_clip * vec2(0.5, 0.5);
	vec2 prev_position_uv = prev_position_clip * vec2(0.5, 0.5);

	motion_vector = prev_position_uv - position_uv;
#endif
}

void main() {
#ifdef UBERSHADER
	bool front_facing = gl_FrontFacing;
	if (uc_cull_mode() == POLYGON_CULL_BACK && !front_facing) {
		discard;
	} else if (uc_cull_mode() == POLYGON_CULL_FRONT && front_facing) {
		discard;
	}
#endif
#ifdef MODE_DUAL_PARABOLOID

	if (dp_clip > 0.0) {
		discard;
	}
#endif
#if defined(RT_TRANSPARENT_SHADOWS) && !defined(MODE_RENDER_SDF)
	rt_transparent_keep_binding();
#endif

	fragment_shader(scene_data_block.data);
}
