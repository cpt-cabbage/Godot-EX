/**************************************************************************/
/*  rig_kernel.cpp                                                        */
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

#include "rig_kernel.h"

#include "core/object/class_db.h"
#include "servers/physics_3d/direct_states/physics_direct_body_state_3d.h"
#include "servers/physics_3d/direct_states/physics_direct_space_state_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

Vector3 RigKernel::rotation_vector(const Basis &p_basis) {
	Quaternion q = p_basis.get_rotation_quaternion();
	if (q.w < 0.0f) {
		q = -q;
	}
	const real_t angle = q.get_angle();
	return angle > 1e-6f ? q.get_axis() * angle : Vector3();
}

Basis RigKernel::heading_of(const Basis &p_basis) {
	Vector3 f = p_basis.get_column(2);
	f.y = 0.0f;
	if (f.length_squared() < 1e-6f) {
		return Basis();
	}
	return Basis::looking_at(-f.normalized(), Vector3(0, 1, 0)); // Its -Z looks along -f: +Z is forward.
}

void RigKernel::set_bodies(const TypedArray<RID> &p_bodies, const PackedFloat32Array &p_masses, const PackedInt32Array &p_parents) {
	ERR_FAIL_COND_MSG(p_bodies.size() != p_masses.size() || p_bodies.size() != p_parents.size(), "Bodies, masses and parents differ in count.");
	bodies.clear();
	body_set.clear();
	for (int i = 0; i < p_bodies.size(); i++) {
		RID rid = p_bodies[i];
		bodies.push_back(rid);
		body_set.insert(rid);
	}
	masses = p_masses;
	parents = p_parents;
	xform.resize(bodies.size());
	lin_vel.resize(bodies.size());
	ang_vel.resize(bodies.size());
}

void RigKernel::set_pelvis_head(int p_pelvis, int p_head) {
	ERR_FAIL_INDEX(p_pelvis, bodies.size());
	ERR_FAIL_INDEX(p_head, bodies.size());
	pelvis = p_pelvis;
	head = p_head;
}

void RigKernel::add_drive(RID p_joint, int p_body, const PackedInt32Array &p_axes, const Vector3 &p_nominal, const Vector3 &p_scale, const Vector3 &p_lo, const Vector3 &p_hi) {
	ERR_FAIL_INDEX(p_body, bodies.size());
	ERR_FAIL_INDEX_MSG(parents[p_body], bodies.size(), "A driven body needs a parent body.");
	Drive d;
	d.joint = p_joint;
	d.body = p_body;
	for (int i = 0; i < p_axes.size(); i++) {
		ERR_FAIL_INDEX(p_axes[i], 3);
		d.axes.push_back(p_axes[i]);
	}
	d.nominal = p_nominal;
	d.scale = p_scale;
	d.lo = p_lo;
	d.hi = p_hi;
	drives.push_back(d);
	action_size += d.axes.size();
}

void RigKernel::set_contacts(const PackedInt32Array &p_left, const PackedInt32Array &p_right) {
	contacts_left = p_left;
	contacts_right = p_right;
}

void RigKernel::set_keypoints(const PackedInt32Array &p_bodies, const PackedVector3Array &p_offsets) {
	ERR_FAIL_COND(p_bodies.size() != p_offsets.size());
	key_bodies = p_bodies;
	key_offsets = p_offsets;
}

void RigKernel::set_scan(const PackedFloat32Array &p_x, const PackedFloat32Array &p_z) {
	scan_x = p_x;
	scan_z = p_z;
}

void RigKernel::update() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Vector3 p;
	Vector3 v;
	real_t m = 0.0f;
	for (int i = 0; i < bodies.size(); i++) {
		xform.write[i] = ps->body_get_state(bodies[i], PS3DE::BODY_STATE_TRANSFORM);
		lin_vel.write[i] = ps->body_get_state(bodies[i], PS3DE::BODY_STATE_LINEAR_VELOCITY);
		ang_vel.write[i] = ps->body_get_state(bodies[i], PS3DE::BODY_STATE_ANGULAR_VELOCITY);
		p += xform[i].origin * masses[i];
		v += lin_vel[i] * masses[i];
		m += masses[i];
	}
	com = m > 0.0f ? p / m : Vector3();
	com_vel = m > 0.0f ? v / m : Vector3();
	heading = heading_of(xform[pelvis].basis);
}

Transform3D RigKernel::get_transform(int p_body) const {
	ERR_FAIL_INDEX_V(p_body, xform.size(), Transform3D());
	return xform[p_body];
}

Vector3 RigKernel::get_linear_velocity(int p_body) const {
	ERR_FAIL_INDEX_V(p_body, lin_vel.size(), Vector3());
	return lin_vel[p_body];
}

Vector3 RigKernel::get_angular_velocity(int p_body) const {
	ERR_FAIL_INDEX_V(p_body, ang_vel.size(), Vector3());
	return ang_vel[p_body];
}

bool RigKernel::_touches_world(int p_body) const {
	PhysicsDirectBodyState3D *state = PhysicsServer3D::get_singleton()->body_get_direct_state(bodies[p_body]);
	if (state == nullptr) {
		return false;
	}
	for (int i = 0; i < state->get_contact_count(); i++) {
		if (!body_set.has(state->get_contact_collider(i))) {
			return true;
		}
	}
	return false;
}

bool RigKernel::contact(int p_side) const {
	const Vector<int> &list = p_side == 0 ? contacts_left : contacts_right;
	for (int b : list) {
		if (b >= 0 && b < bodies.size() && _touches_world(b)) {
			return true;
		}
	}
	return false;
}

float RigKernel::ground(const Vector3 &p_point) const {
	return _ground_from(p_point, 1.0f);
}

// The first hit of a ray from p_up above the point to 4 m below it; without one, 1 m below the point.
float RigKernel::_ground_from(const Vector3 &p_point, float p_up) const {
	ERR_FAIL_COND_V(bodies.is_empty(), p_point.y - 1.0f);
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PhysicsDirectSpaceState3D *space = ps->space_get_direct_state(ps->body_get_space(bodies[pelvis]));
	ERR_FAIL_NULL_V(space, p_point.y - 1.0f);
	PS3DT::RayParameters ray;
	ray.from = p_point + Vector3(0, p_up, 0);
	ray.to = p_point - Vector3(0, 4, 0);
	ray.collision_mask = ground_mask;
	PS3DT::RayResult hit;
	return space->intersect_ray(ray, hit) ? hit.position.y : p_point.y - 1.0f;
}

PackedFloat32Array RigKernel::observation(const PackedFloat32Array &p_action, const Vector3 &p_command, bool p_scan) const {
	ERR_FAIL_COND_V_MSG(p_action.size() != action_size, PackedFloat32Array(), vformat("Expected %d action values, got %d.", action_size, p_action.size()));
	const int n = 10 + 3 * action_size + 5 + (p_scan ? scan_x.size() * scan_z.size() : 0);
	PackedFloat32Array out;
	out.resize(n);
	float *o = out.ptrw();
	const Transform3D &pt = xform[pelvis];
	const Basis pbi = pt.basis.inverse();
	const Vector3 g = pbi.xform(Vector3(0, -1, 0));
	const Vector3 w = pbi.xform(ang_vel[pelvis]);
	const Vector3 v = heading.inverse().xform(com_vel);
	const float g_com = ground(com);
	const float head_values[10] = { g.x, g.y, g.z, w.x, w.y, w.z, v.x, v.y, v.z, com.y - g_com };
	int k = 0;
	for (; k < 10; k++) {
		o[k] = head_values[k];
	}
	for (const Drive &d : drives) {
		const int parent = parents[d.body];
		const Basis parent_inverse = xform[parent].basis.inverse();
		const Vector3 rv = rotation_vector(parent_inverse * xform[d.body].basis);
		const Vector3 rel = parent_inverse.xform(ang_vel[d.body] - ang_vel[parent]);
		for (int axis : d.axes) {
			o[k] = rv[axis];
			o[k + action_size] = 0.1f * rel[axis];
			k++;
		}
	}
	k += action_size;
	for (int i = 0; i < action_size; i++) {
		o[k++] = p_action[i];
	}
	o[k++] = p_command.x;
	o[k++] = p_command.y;
	o[k++] = p_command.z;
	o[k++] = contact(0) ? 1.0f : 0.0f;
	o[k++] = contact(1) ? 1.0f : 0.0f;
	if (p_scan) {
		for (int iz = 0; iz < scan_z.size(); iz++) {
			for (int ix = 0; ix < scan_x.size(); ix++) {
				const Vector3 q = pt.origin + heading.xform(Vector3(scan_x[ix], 0, scan_z[iz]));
				o[k++] = _ground_from(q, scan_height) - g_com;
			}
		}
	}
	return out;
}

PackedFloat32Array RigKernel::features(const Transform3D &p_pelvis_prev, float p_ground_y, float p_fps) const {
	PackedFloat32Array out;
	out.resize(1 + 6 + 3 + 3 + action_size + 3 * key_bodies.size());
	float *o = out.ptrw();
	int k = 0;
	const Transform3D &p = xform[pelvis];
	const Basis hi = heading_of(p.basis).inverse();
	o[k++] = p.origin.y - p_ground_y;
	const Basis pr = hi * p.basis;
	const Vector3 y = pr.get_column(1);
	const Vector3 z = pr.get_column(2);
	o[k++] = y.x;
	o[k++] = y.y;
	o[k++] = y.z;
	o[k++] = z.x;
	o[k++] = z.y;
	o[k++] = z.z;
	const Vector3 lin = hi.xform(p.origin - p_pelvis_prev.origin) * p_fps;
	const Vector3 ang = hi.xform(rotation_vector(p.basis * p_pelvis_prev.basis.inverse())) * p_fps;
	o[k++] = lin.x;
	o[k++] = lin.y;
	o[k++] = lin.z;
	o[k++] = ang.x;
	o[k++] = ang.y;
	o[k++] = ang.z;
	for (const Drive &d : drives) {
		const Vector3 rv = rotation_vector(xform[parents[d.body]].basis.inverse() * xform[d.body].basis);
		for (int axis : d.axes) {
			o[k++] = rv[axis];
		}
	}
	for (int i = 0; i < key_bodies.size(); i++) {
		const Vector3 r = hi.xform(xform[key_bodies[i]].xform(key_offsets[i]) - p.origin);
		o[k++] = r.x;
		o[k++] = r.y;
		o[k++] = r.z;
	}
	return out;
}

void RigKernel::write_targets(const PackedFloat32Array &p_action) const {
	ERR_FAIL_COND_MSG(p_action.size() != action_size, vformat("Expected %d action values, got %d.", action_size, p_action.size()));
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	int k = 0;
	for (const Drive &d : drives) {
		Vector3 rv = d.nominal;
		for (int axis : d.axes) {
			rv[axis] += d.scale[axis] * CLAMP(p_action[k], -1.0f, 1.0f);
			k++;
		}
		rv = rv.clamp(d.lo, d.hi);
		const real_t a = rv.length();
		ps->generic_6dof_joint_set_angular_target_rotation(d.joint, a > 1e-6f ? Quaternion(rv / a, a) : Quaternion());
	}
}

void RigKernel::_bind_methods() {
	ClassDB::bind_static_method("RigKernel", D_METHOD("rotation_vector", "basis"), &RigKernel::rotation_vector);
	ClassDB::bind_static_method("RigKernel", D_METHOD("heading_of", "basis"), &RigKernel::heading_of);
	ClassDB::bind_method(D_METHOD("set_bodies", "bodies", "masses", "parents"), &RigKernel::set_bodies);
	ClassDB::bind_method(D_METHOD("set_pelvis_head", "pelvis", "head"), &RigKernel::set_pelvis_head);
	ClassDB::bind_method(D_METHOD("add_drive", "joint", "body", "axes", "nominal", "scale", "lo", "hi"), &RigKernel::add_drive);
	ClassDB::bind_method(D_METHOD("set_contacts", "left", "right"), &RigKernel::set_contacts);
	ClassDB::bind_method(D_METHOD("set_keypoints", "bodies", "offsets"), &RigKernel::set_keypoints);
	ClassDB::bind_method(D_METHOD("set_scan", "x", "z"), &RigKernel::set_scan);
	ClassDB::bind_method(D_METHOD("set_ground_mask", "mask"), &RigKernel::set_ground_mask);
	ClassDB::bind_method(D_METHOD("set_scan_height", "height"), &RigKernel::set_scan_height);
	ClassDB::bind_method(D_METHOD("get_action_size"), &RigKernel::get_action_size);
	ClassDB::bind_method(D_METHOD("update"), &RigKernel::update);
	ClassDB::bind_method(D_METHOD("get_transform", "body"), &RigKernel::get_transform);
	ClassDB::bind_method(D_METHOD("get_linear_velocity", "body"), &RigKernel::get_linear_velocity);
	ClassDB::bind_method(D_METHOD("get_angular_velocity", "body"), &RigKernel::get_angular_velocity);
	ClassDB::bind_method(D_METHOD("get_center_of_mass"), &RigKernel::get_center_of_mass);
	ClassDB::bind_method(D_METHOD("get_center_of_mass_velocity"), &RigKernel::get_center_of_mass_velocity);
	ClassDB::bind_method(D_METHOD("get_heading"), &RigKernel::get_heading);
	ClassDB::bind_method(D_METHOD("contact", "side"), &RigKernel::contact);
	ClassDB::bind_method(D_METHOD("ground", "point"), &RigKernel::ground);
	ClassDB::bind_method(D_METHOD("observation", "action", "command", "scan"), &RigKernel::observation);
	ClassDB::bind_method(D_METHOD("features", "pelvis_prev", "ground_y", "fps"), &RigKernel::features);
	ClassDB::bind_method(D_METHOD("write_targets", "action"), &RigKernel::write_targets);
}
