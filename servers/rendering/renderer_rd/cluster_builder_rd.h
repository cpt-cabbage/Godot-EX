/**************************************************************************/
/*  cluster_builder_rd.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/cluster_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/cluster_render.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/cluster_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/cluster_store.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

class ClusterBuilderSharedDataRD {
	friend class ClusterBuilderRD;

	RID sphere_vertex_buffer;
	RID sphere_vertex_array;
	RID sphere_index_buffer;
	RID sphere_index_array;
	float sphere_overfit = 0.0; // Because an icosphere is not a perfect sphere, we need to enlarge it to cover the sphere area.

	RID cone_vertex_buffer;
	RID cone_vertex_array;
	RID cone_index_buffer;
	RID cone_index_array;
	float cone_overfit = 0.0; // Because an cone mesh is not a perfect cone, we need to enlarge it to cover the actual cone area.

	RID box_vertex_buffer;
	RID box_vertex_array;
	RID box_index_buffer;
	RID box_index_array;

	enum Divisor {
		DIVISOR_1,
		DIVISOR_2,
		DIVISOR_4,
	};

	struct ClusterRender {
		struct PushConstant {
			uint32_t base_index;
			uint32_t pad0;
			uint32_t pad1;
			uint32_t pad2;
		};

		ClusterRenderShaderRD cluster_render_shader;
		RID shader_version;
		RID shader;

		enum ShaderVariant {
			SHADER_NORMAL,
			SHADER_USE_ATTACHMENT,
		};

		enum PipelineVersion {
			PIPELINE_NORMAL,
			PIPELINE_MSAA,
			PIPELINE_MAX
		};

		RID shader_pipelines[PIPELINE_MAX];
	} cluster_render;

	struct ClusterStore {
		struct PushConstant {
			uint32_t cluster_render_data_size; // how much data for a single cluster takes
			uint32_t max_render_element_count_div_32; // divided by 32
			uint32_t cluster_screen_size[2];
			uint32_t render_element_count_div_32; // divided by 32
			uint32_t max_cluster_element_count_div_32; // divided by 32

			uint32_t pad1;
			uint32_t pad2;
		};

		ClusterStoreShaderRD cluster_store_shader;
		RID shader_version;
		RID shader;
		RID shader_pipeline;
	} cluster_store;

	// The bake as a compute cull instead of the proxy rasterisation (see
	// cluster_cull.glsl); writes what cluster_render.glsl writes.
	struct ClusterCull {
		ClusterCullShaderRD cluster_cull_shader;
		RID shader_version;
		RID shader;
		RID shader_pipeline;
	} cluster_cull;

	struct ClusterDebug {
		struct PushConstant {
			uint32_t screen_size[2];
			uint32_t cluster_screen_size[2];

			uint32_t cluster_shift;
			uint32_t cluster_type;
			float z_near;
			float z_far;

			uint32_t orthogonal;
			uint32_t max_cluster_element_count_div_32;

			uint32_t pad1;
			uint32_t pad2;
		};

		ClusterDebugShaderRD cluster_debug_shader;
		RID shader_version;
		RID shader;
		RID shader_pipeline;
	} cluster_debug;

public:
	ClusterBuilderSharedDataRD();
	~ClusterBuilderSharedDataRD();
};

class ClusterBuilderRD {
public:
	static constexpr float WIDE_SPOT_ANGLE_THRESHOLD_DEG = 60.0f;

	enum LightType {
		LIGHT_TYPE_OMNI,
		LIGHT_TYPE_SPOT,
		LIGHT_TYPE_AREA,
	};

	enum BoxType {
		BOX_TYPE_REFLECTION_PROBE,
		BOX_TYPE_DECAL,
	};

	enum ElementType {
		ELEMENT_TYPE_OMNI_LIGHT,
		ELEMENT_TYPE_SPOT_LIGHT,
		ELEMENT_TYPE_AREA_LIGHT,
		ELEMENT_TYPE_DECAL,
		ELEMENT_TYPE_REFLECTION_PROBE,
		ELEMENT_TYPE_MAX,
	};

private:
	ClusterBuilderSharedDataRD *shared = nullptr;

	struct RenderElementData {
		uint32_t type; // 0-4
		uint32_t touches_near;
		uint32_t touches_far;
		uint32_t original_index;
		float transform_inv[12]; // Transposed transform for less space.
		float scale[3];
		uint32_t has_wide_spot_angle; // Bit 0: a spot light drawn as a sphere. Bit 1: an image light (see add_light_image).
	}; // Keep aligned to 32 bytes.

	uint32_t cluster_count_by_type[ELEMENT_TYPE_MAX] = {};
	uint32_t max_elements_by_type = 0;

	RenderElementData *render_elements = nullptr;
	uint32_t render_element_count = 0;
	uint32_t render_element_max = 0;

	Transform3D view_xform;
	Projection adjusted_projection;
	Projection projection;
	float z_far = 0;
	float z_near = 0;
	bool camera_orthogonal = false;

	enum Divisor {
		DIVISOR_1,
		DIVISOR_2,
		DIVISOR_4,
	};

	uint32_t cluster_size = 32;
	bool use_msaa = true;
	Divisor divisor = DIVISOR_4;

	Size2i screen_size;
	Size2i cluster_screen_size;

	RID framebuffer;
	RID cluster_render_buffer; // Used for creating.
	RID cluster_buffer; // Used for rendering.
	RID element_buffer; // Used for storing, to hint element touches far plane or near plane.
	uint32_t cluster_render_buffer_size = 0;
	uint32_t cluster_buffer_size = 0;

	RID cluster_render_uniform_set;
	RID cluster_store_uniform_set;
	RID cluster_cull_uniform_set;

	// A second cluster from the compute cull with exponential depth slices,
	// for the stochastic sampling pass: the linear slices reach z_far in 32
	// steps, so with a far plane of kilometres a cell is over a hundred metres
	// deep and holds every light in the scene, and the pass sums each cell's
	// lights exactly. Slice s spans log_z0 * (z_far / log_z0)^(s / 32) to the
	// next, slice 0 from the camera. Same layout, same store pass.
	RID cluster_render_buffer_log;
	RID cluster_buffer_log;
	RID cluster_store_log_uniform_set;
	float log_z0 = 0.0f;
	bool log_valid = false;
	// Whether this frame's bake is the compute cull (read at begin, so the
	// image lights, which only that path culls, are added on the same rule).
	bool compute_cull = false;

	// Persistent data.

	void _clear();

	struct StateUniform {
		float projection[16];
		float inv_z_far;
		uint32_t screen_to_clusters_shift; // Shift to obtain coordinates in block indices.
		uint32_t cluster_screen_width;
		uint32_t cluster_data_size; // How much data is needed for a single cluster.
		uint32_t cluster_depth_offset;

		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;

		// The compute cull's fields (cluster_cull.glsl); the raster shaders
		// declare the block without them.
		float inv_projection[16];
		float screen_size[2];
		uint32_t cluster_size;
		uint32_t camera_orthogonal;
		float z_far;
		uint32_t cluster_screen_height;
		uint32_t render_element_count;
		float log_z0;
	};

	RID state_uniform;

	RID debug_uniform_set;

public:
	void setup(Size2i p_screen_size, uint32_t p_max_elements, RID p_depth_buffer, RID p_depth_buffer_sampler, RID p_color_buffer);

	void begin(const Transform3D &p_view_transform, const Projection &p_cam_projection, bool p_flip_y);

	_FORCE_INLINE_ void add_light(LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size) {
		_add_light(p_type, p_transform, p_radius, p_spot_aperture, p_area_size, UINT32_MAX);
	}

	// A local light's image through a planar mirror chain (the light's
	// transform mirrored through the chain), as an element of the
	// exponential-depth cluster only, under an index of the caller's past
	// the real lights of its type: the stochastic pass decodes it back into
	// the light and the chain. The image's proxy is placed where the image
	// shines, which is what the pass's cell walk needs; walking the real
	// light's cell for its images tied their presence to whether the real
	// light's proxy happened to cover the pixel's froxel (plan section 53).
	// Nothing on the raster paths sees these: the compute cull keeps them
	// out of the linear cluster, and none are added when it is not running.
	_FORCE_INLINE_ void add_light_image(LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size, uint32_t p_original_index) {
		if (!compute_cull || p_original_index >= max_elements_by_type) {
			return;
		}
		_add_light(p_type, p_transform, p_radius, p_spot_aperture, p_area_size, p_original_index);
	}

	_FORCE_INLINE_ void _add_light(LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture, const Vector2 &p_area_size, uint32_t p_image_index) {
		const bool image = p_image_index != UINT32_MAX;
		if (!image) {
			if (p_type == LIGHT_TYPE_OMNI && cluster_count_by_type[ELEMENT_TYPE_OMNI_LIGHT] == max_elements_by_type) {
				return; // Max number elements reached.
			}
			if (p_type == LIGHT_TYPE_SPOT && cluster_count_by_type[ELEMENT_TYPE_SPOT_LIGHT] == max_elements_by_type) {
				return; // Max number elements reached.
			}
			if (p_type == LIGHT_TYPE_AREA && cluster_count_by_type[ELEMENT_TYPE_AREA_LIGHT] == max_elements_by_type) {
				return; // Max number elements reached.
			}
		}
		if (render_element_count >= render_element_max) {
			return; // The element list is full (the images share it with every type).
		}

		RenderElementData &e = render_elements[render_element_count];
		e.has_wide_spot_angle = 0;

		Transform3D xform = view_xform * p_transform;

		float radius = xform.basis.get_uniform_scale();
		if (radius < 0.98 || radius > 1.02) {
			xform.basis.orthonormalize();
		}

		radius *= p_radius;

		// Spotlights with wide angle are trated as Omni lights.
		// If the spot angle is above the threshold, we need a sphere instead of a cone for building the clusters
		// since the cone gets too flat/large (spot angle close to 90 degrees) or
		// can't even cover the affected area of the light (spot angle above 90 degrees).
		if (p_type == LIGHT_TYPE_OMNI || (p_type == LIGHT_TYPE_SPOT && p_spot_aperture > WIDE_SPOT_ANGLE_THRESHOLD_DEG)) {
			radius *= shared->sphere_overfit; // Overfit icosphere.

			float depth = -xform.origin.z;
			if (camera_orthogonal) {
				e.touches_near = (depth - radius) < z_near;
			} else {
				// Contains camera inside light.
				float radius2 = radius * shared->sphere_overfit; // Overfit again for outer size (camera may be outside actual sphere but behind an icosphere vertex)
				e.touches_near = xform.origin.length_squared() < radius2 * radius2;
			}

			e.touches_far = (depth + radius) > z_far;
			e.scale[0] = radius;
			e.scale[1] = radius;
			e.scale[2] = radius;
			if (p_type == LIGHT_TYPE_OMNI) {
				e.type = ELEMENT_TYPE_OMNI_LIGHT;
				e.original_index = image ? p_image_index : cluster_count_by_type[ELEMENT_TYPE_OMNI_LIGHT]++;
			} else { // LIGHT_TYPE_SPOT with wide angle.
				e.type = ELEMENT_TYPE_SPOT_LIGHT;
				e.has_wide_spot_angle = 1;
				e.original_index = image ? p_image_index : cluster_count_by_type[ELEMENT_TYPE_SPOT_LIGHT]++;
			}

			RendererRD::MaterialStorage::store_transform_transposed_3x4(xform, e.transform_inv);

		} else if (p_type == LIGHT_TYPE_SPOT) { /*LIGHT_TYPE_SPOT with no wide angle*/
			radius *= shared->cone_overfit; // Overfit cone.

			real_t len = Math::tan(Math::deg_to_rad(p_spot_aperture)) * radius;
			// Approximate, probably better to use a cone support function.
			float max_d = -1e20;
			float min_d = 1e20;
#define CONE_MINMAX(m_x, m_y) \
	{ \
		float d = -xform.xform(Vector3(len * m_x, len * m_y, -radius)).z; \
		min_d = MIN(d, min_d); \
		max_d = MAX(d, max_d); \
	}

			CONE_MINMAX(1, 1);
			CONE_MINMAX(-1, 1);
			CONE_MINMAX(-1, -1);
			CONE_MINMAX(1, -1);

			if (camera_orthogonal) {
				e.touches_near = min_d < z_near;
			} else {
				Plane base_plane(-xform.basis.get_column(Vector3::AXIS_Z), xform.origin);
				float dist = base_plane.distance_to(Vector3());
				if (dist >= 0 && dist < radius) {
					// Contains camera inside light, check angle.
					float angle = Math::rad_to_deg(Math::acos((-xform.origin.normalized()).dot(-xform.basis.get_column(Vector3::AXIS_Z))));
					e.touches_near = angle < p_spot_aperture * 1.05; //overfit aperture a little due to cone overfit
				} else {
					e.touches_near = false;
				}
			}

			e.touches_far = max_d > z_far;
			e.scale[0] = len * shared->cone_overfit;
			e.scale[1] = len * shared->cone_overfit;
			e.scale[2] = radius;
			e.type = ELEMENT_TYPE_SPOT_LIGHT;
			e.original_index = image ? p_image_index : cluster_count_by_type[ELEMENT_TYPE_SPOT_LIGHT]++;

			RendererRD::MaterialStorage::store_transform_transposed_3x4(xform, e.transform_inv);
		} else { /* LIGHT_TYPE_AREA */
			Vector3 scale = Vector3(p_area_size.x / 2.0 + radius, p_area_size.y / 2.0 + radius, radius / 2.0);

			for (uint32_t i = 0; i < 3; i++) {
				float s = xform.basis.rows[i].length();
				//scale[i] *= s; // lights ignore scale
				xform.basis.rows[i] /= s;
			}
			xform.origin -= xform.basis.get_column(Vector3::AXIS_Z) * scale.z; // translate center to center of box

			float depth = -xform.origin.z;
			float box_depth = Math::abs(xform.basis.xform_inv(Vector3(0, 0, -1)).dot(scale));

			if (camera_orthogonal) {
				e.touches_near = (depth - box_depth) < z_near;
			} else {
				// Contains camera inside box.
				Vector3 inside = xform.xform_inv(Vector3(0, 0, 0)).abs();
				e.touches_near = inside.x < scale.x && inside.y < scale.y && inside.z < scale.z;
			}

			e.touches_far = depth + box_depth > z_far;

			e.scale[0] = scale.x;
			e.scale[1] = scale.y;
			e.scale[2] = scale.z;

			e.type = ELEMENT_TYPE_AREA_LIGHT;
			e.original_index = image ? p_image_index : cluster_count_by_type[ELEMENT_TYPE_AREA_LIGHT]++;

			RendererRD::MaterialStorage::store_transform_transposed_3x4(xform, e.transform_inv);
		}
		if (image) {
			e.has_wide_spot_angle |= 2;
		}

		render_element_count++;
	}

	// The real lights of a type added so far (the index the next one gets).
	_FORCE_INLINE_ uint32_t get_light_count(LightType p_type) const {
		return cluster_count_by_type[p_type == LIGHT_TYPE_OMNI ? ELEMENT_TYPE_OMNI_LIGHT : (p_type == LIGHT_TYPE_SPOT ? ELEMENT_TYPE_SPOT_LIGHT : ELEMENT_TYPE_AREA_LIGHT)];
	}
	_FORCE_INLINE_ uint32_t get_max_elements_by_type() const { return max_elements_by_type; }

	_FORCE_INLINE_ void add_box(BoxType p_box_type, const Transform3D &p_transform, const Vector3 &p_half_size) {
		if (p_box_type == BOX_TYPE_DECAL && cluster_count_by_type[ELEMENT_TYPE_DECAL] == max_elements_by_type) {
			return; // Max number elements reached.
		}
		if (p_box_type == BOX_TYPE_REFLECTION_PROBE && cluster_count_by_type[ELEMENT_TYPE_REFLECTION_PROBE] == max_elements_by_type) {
			return; // Max number elements reached.
		}

		RenderElementData &e = render_elements[render_element_count];
		Transform3D xform = view_xform * p_transform;

		// Extract scale and scale the matrix by it, makes things simpler.
		Vector3 scale = p_half_size;
		for (uint32_t i = 0; i < 3; i++) {
			float s = xform.basis.rows[i].length();
			scale[i] *= s;
			xform.basis.rows[i] /= s;
		};

		float box_depth = Math::abs(xform.basis.xform_inv(Vector3(0, 0, -1)).dot(scale));
		float depth = -xform.origin.z;

		if (camera_orthogonal) {
			e.touches_near = depth - box_depth < z_near;
		} else {
			// Contains camera inside box.
			Vector3 inside = xform.xform_inv(Vector3(0, 0, 0)).abs();
			e.touches_near = inside.x < scale.x && inside.y < scale.y && inside.z < scale.z;
		}

		e.touches_far = depth + box_depth > z_far;

		e.scale[0] = scale.x;
		e.scale[1] = scale.y;
		e.scale[2] = scale.z;

		e.type = (p_box_type == BOX_TYPE_DECAL) ? ELEMENT_TYPE_DECAL : ELEMENT_TYPE_REFLECTION_PROBE;
		e.original_index = cluster_count_by_type[e.type];

		RendererRD::MaterialStorage::store_transform_transposed_3x4(xform, e.transform_inv);

		cluster_count_by_type[e.type]++;
		render_element_count++;
	}

	_FORCE_INLINE_ uint32_t get_cluster_count_by_type(ElementType p_element_type) const {
		DEV_ASSERT(p_element_type < ELEMENT_TYPE_MAX);
		return cluster_count_by_type[p_element_type];
	}

	void bake_cluster();
	void debug(ElementType p_element);

	RID get_cluster_buffer() const;
	uint32_t get_cluster_buffer_size() const;
	// The exponential-depth cluster when the compute cull built it this frame
	// (its z0 nonzero), else the linear one (z0 zero).
	RID get_cluster_buffer_log() const;
	float get_cluster_log_z0() const;
	uint32_t get_cluster_size() const;
	uint32_t get_max_cluster_elements() const;

	void set_shared(ClusterBuilderSharedDataRD *p_shared);

	ClusterBuilderRD();
	~ClusterBuilderRD();
};
