/**************************************************************************/
/*  ground_data.h                                                         */
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

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// What a training terrain's static ground is, as data: a height field's samples and props (boxes and
// cylinders), for casting the height scan's rays without the physics engine (a ray costs ~0.38 us in
// Jolt's batched probe, ~20x what reading the data costs; design/v2_recipe.md, perception). The rays are
// Jolt's: straight down from each point for a length, the first surface met; a convex shape the ray
// starts inside is not hit (RayCastSettings::mTreatConvexAsSolid false); the height field's cells split
// as Godot's Jolt module splits them (the diagonal from (x + 1, z) to (x, z + 1)). The data must be the
// physics: the caller builds the same shapes into both, and a test holds the two together.
class GroundData : public RefCounted {
	GDCLASS(GroundData, RefCounted);

	struct Prop {
		enum Kind {
			BOX,
			CYLINDER,
		};
		Kind kind = BOX;
		Transform3D xform;
		Transform3D inverse;
		Vector3 size; // BOX: half extents; CYLINDER: radius, half height, unused
		AABB aabb;
	};

	bool enabled = true;
	bool has_field = false;
	Transform3D field_xform; // the height field's collision shape's, scale included (axis-aligned)
	int field_width = 0;
	int field_depth = 0;
	LocalVector<float> field;
	LocalVector<Prop> props;
	HashMap<int64_t, LocalVector<int>> buckets; // prop indices by BUCKET-sized cell of their bounds in x and z
	static constexpr float BUCKET = 2.0f;

	// Water (design/v2_recipe.md item 9): a surface's height and its flow (x, z) at a grid's samples, NaN
	// where dry, placed by water_xform (a scale and a translation; its y scale multiplies the heights)
	// and the flow times water_flow_scale. Jolt has no water: WaterForces reads it for buoyancy and drag,
	// and RigKernel for the policy's water inputs.
	bool has_water = false;
	Transform3D water_xform;
	float water_flow_scale = 1.0f;
	int water_width = 0;
	int water_depth = 0;
	LocalVector<float> water_surface;
	LocalVector<Vector2> water_flow;

	static int64_t _bucket_key(int p_x, int p_z) { return (int64_t(p_x) << 32) ^ int64_t(uint32_t(p_z)); }
	void _add_prop(const Prop &p_prop);
	bool _field_hit(const Vector3 &p_from, float p_length, float &r_y) const;
	static bool _prop_hit(const Prop &p_prop, const Vector3 &p_from, float p_length, float &r_y);

protected:
	static void _bind_methods();

public:
	void set_enabled(bool p_enabled) { enabled = p_enabled; }
	bool is_enabled() const { return enabled; }
	void set_height_field(const Transform3D &p_xform, int p_width, int p_depth, const PackedFloat32Array &p_heights);
	void set_height_field_transform(const Transform3D &p_xform);
	void clear_props();
	void add_box(const Transform3D &p_xform, const Vector3 &p_half_extents);
	void add_cylinder(const Transform3D &p_xform, float p_radius, float p_height);
	int get_prop_count() const { return props.size(); }

	// The p_count nearest trunks (upright cylinders at least TRUNK_HEIGHT tall: trees, posts; what the
	// height scan cannot show, design/v2_recipe.md) within p_range of p_xform's origin, nearest first, in
	// its frame: x, z, radius, 1 for each, zeros past the last found (4 x p_count values).
	PackedFloat32Array nearest_trunks(const Transform3D &p_xform, int p_count, float p_range) const;
	static constexpr float TRUNK_HEIGHT = 1.0f;

	void set_water_field(const Transform3D &p_xform, int p_width, int p_depth, const PackedFloat32Array &p_surface, const PackedVector2Array &p_flow);
	void set_water_transform(const Transform3D &p_xform, float p_flow_scale);
	void clear_water();
	bool has_water_field() const { return has_water; }
	// The water at p_point's x and z: its surface's height (NaN where dry) and its flow (x, z; zero where
	// dry), bilinear over its cell; a cell with a dry corner is dry. The field is to reach past the shore
	// (a cell's diagonal or more) with the surface there, and the caller decides wet by the surface over
	// the ground.
	bool water_sample(float p_x, float p_z, float &r_surface, Vector2 &r_flow) const;
	Vector3 water_at(const Vector3 &p_point) const; // surface, flow x, flow z
	// The highest surface over the samples within p_radius of p_point in x and z, or -INF (a rig's early out).
	float water_top_near(const Vector3 &p_point, float p_radius) const;

	// The first surface straight down from p_from within p_length (its y), or NaN.
	float cast_down(const Vector3 &p_from, float p_length) const;
	PackedFloat32Array cast_down_batch(const PackedVector3Array &p_from, float p_length) const;
};
