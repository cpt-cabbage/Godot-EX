/**************************************************************************/
/*  rig_kernel.h                                                          */
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

#include "core/math/basis.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"

// The per-step work of a learned controller driving a physics rig (a ragdoll of rigid bodies joined
// by 6DOF joints with angular drives): reading every body's state from the physics server once,
// building the policy's observation and a motion prior's features from it, casting rays for the
// ground under the body, and writing an action to the joints' drive targets. It exists because the
// same work in a scripting language dominated a training server's step (about 70 % of it, against
// the physics engine's 30 %) and runs every control step in the game.
//
// Setup, once: set_bodies() (rigid bodies, masses, parent indices), set_pelvis_head(), add_drive()
// per actuated joint in action order (which also defines the observed joints), set_contacts(),
// set_keypoints(), set_scan() (and set_scan_height()) and set_ground_mask(). Each step: update(), then any of the queries.
class RigKernel : public RefCounted {
	GDCLASS(RigKernel, RefCounted);

	struct Drive {
		RID joint;
		int body = -1;
		Vector<int> axes;
		Vector3 nominal;
		Vector3 scale;
		Vector3 lo;
		Vector3 hi;
	};

	Vector<RID> bodies;
	HashSet<RID> body_set;
	Vector<float> masses;
	Vector<int> parents;
	int pelvis = 0;
	int head = 0;
	Vector<Drive> drives;
	int action_size = 0;
	Vector<int> contacts_left;
	Vector<int> contacts_right;
	Vector<int> key_bodies;
	Vector<Vector3> key_offsets;
	Vector<float> scan_x;
	Vector<float> scan_z;
	uint32_t ground_mask = 1;
	float scan_height = 1.0f;

	Vector<Transform3D> xform;
	Vector<Vector3> lin_vel;
	Vector<Vector3> ang_vel;
	Vector3 com;
	Vector3 com_vel;
	Basis heading;

	bool _touches_world(int p_body) const;
	float _ground_from(const Vector3 &p_point, float p_up) const;

protected:
	static void _bind_methods();

public:
	static Vector3 rotation_vector(const Basis &p_basis);
	static Basis heading_of(const Basis &p_basis);

	void set_bodies(const TypedArray<RID> &p_bodies, const PackedFloat32Array &p_masses, const PackedInt32Array &p_parents);
	void set_pelvis_head(int p_pelvis, int p_head);
	void add_drive(RID p_joint, int p_body, const PackedInt32Array &p_axes, const Vector3 &p_nominal, const Vector3 &p_scale, const Vector3 &p_lo, const Vector3 &p_hi);
	void set_contacts(const PackedInt32Array &p_left, const PackedInt32Array &p_right);
	void set_keypoints(const PackedInt32Array &p_bodies, const PackedVector3Array &p_offsets);
	void set_scan(const PackedFloat32Array &p_x, const PackedFloat32Array &p_z);
	void set_ground_mask(int p_mask) { ground_mask = p_mask; }
	void set_scan_height(float p_height) { scan_height = p_height; }
	int get_action_size() const { return action_size; }

	void update();
	Transform3D get_transform(int p_body) const;
	Vector3 get_linear_velocity(int p_body) const;
	Vector3 get_angular_velocity(int p_body) const;
	Vector3 get_center_of_mass() const { return com; }
	Vector3 get_center_of_mass_velocity() const { return com_vel; }
	Basis get_heading() const { return heading; }
	bool contact(int p_side) const;
	float ground(const Vector3 &p_point) const;

	PackedFloat32Array observation(const PackedFloat32Array &p_action, const Vector3 &p_command, bool p_scan) const;
	PackedFloat32Array features(const Transform3D &p_pelvis_prev, float p_ground_y, float p_fps) const;
	void write_targets(const PackedFloat32Array &p_action) const;
};
