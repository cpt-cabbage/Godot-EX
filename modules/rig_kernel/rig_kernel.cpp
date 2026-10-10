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

#include "core/config/engine.h"

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
	scan_points.clear();
}

void RigKernel::set_scan_points(const PackedVector2Array &p_points) {
	scan_points.resize(p_points.size());
	for (int i = 0; i < p_points.size(); i++) {
		scan_points.write[i] = p_points[i];
	}
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
	for (const int b : sensed) {
		touch_friction.write[b] = _world_friction(b);
		touch.write[b] = Math::is_nan(touch_friction[b]) ? 0 : 1;
	}
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

// The friction of the first collider outside the rig body p_body touches, NaN when none (as the
// reward's script reads it: its first contact's collider's body friction).
float RigKernel::_world_friction(int p_body) const {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PhysicsDirectBodyState3D *state = ps->body_get_direct_state(bodies[p_body]);
	if (state == nullptr) {
		return Math::NaN;
	}
	for (int i = 0; i < state->get_contact_count(); i++) {
		const RID collider = state->get_contact_collider(i);
		if (!body_set.has(collider)) {
			return ps->body_get_param(collider, PS3DE::BODY_PARAM_FRICTION);
		}
	}
	return Math::NaN;
}

// Whether p_body touches the world: update()'s read when it is sensed, else the physics server's now.
bool RigKernel::_touches(int p_body) const {
	if (p_body < touch.size() && touch[p_body] >= 0) {
		return touch[p_body] == 1;
	}
	return _touches_world(p_body);
}

bool RigKernel::contact(int p_side) const {
	const Vector<int> &list = p_side == 0 ? contacts_left : contacts_right;
	for (int b : list) {
		if (b >= 0 && b < bodies.size() && _touches(b)) {
			return true;
		}
	}
	return false;
}

float RigKernel::ground(const Vector3 &p_point) const {
	return _ground_from(p_point, 1.0f);
}

// With static_ground, each point's ground by JoltPhysicsServer3D's batched static probe (the same ray
// as _ground_from's); false without that server method, and the caller casts its own.
bool RigKernel::_static_ground_batch(const PackedVector3Array &p_points, float p_up, float *r_heights) const {
	if (ground_data.is_valid() && ground_data->is_enabled()) {
		for (int i = 0; i < p_points.size(); i++) {
			const float h = ground_data->cast_down(p_points[i] + Vector3(0, p_up, 0), p_up + 4.0f);
			r_heights[i] = Math::is_nan(h) ? p_points[i].y - 1.0f : h;
		}
		return true;
	}
	if (!static_ground || bodies.is_empty()) {
		return false;
	}
	// Godot-EX's Jolt server, an engine singleton of its own (PhysicsServer3D's singleton is the
	// thread-safety wrapper, which forwards only the common API).
	static const StringName jolt("JoltPhysicsServer3D");
	static const StringName method("space_cast_static_ground");
	Object *js = Engine::get_singleton()->has_singleton(jolt) ? Engine::get_singleton()->get_singleton_object(jolt) : nullptr;
	if (js == nullptr || !js->has_method(method)) {
		return false;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PackedVector3Array from;
	from.resize(p_points.size());
	for (int i = 0; i < p_points.size(); i++) {
		from.write[i] = p_points[i] + Vector3(0, p_up, 0);
	}
	const PackedFloat32Array h = js->call(method, ps->body_get_space(bodies[pelvis]), from, p_up + 4.0f, ground_mask);
	ERR_FAIL_COND_V(h.size() != p_points.size(), false);
	for (int i = 0; i < h.size(); i++) {
		r_heights[i] = Math::is_nan(h[i]) ? p_points[i].y - 1.0f : h[i];
	}
	return true;
}

// The first hit of a ray from p_up above the point to 4 m below it; without one, 1 m below the point.
float RigKernel::_ground_from(const Vector3 &p_point, float p_up) const {
	ERR_FAIL_COND_V(bodies.is_empty(), p_point.y - 1.0f);
	float h = 0.0f;
	if (_static_ground_batch(PackedVector3Array({ p_point }), p_up, &h)) {
		return h;
	}
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
	const int n = 10 + 3 * action_size + 5 + (p_scan ? get_scan_size() + get_water_size() : 0);
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
		PackedVector3Array points;
		if (!scan_points.is_empty()) {
			for (const Vector2 &q : scan_points) {
				points.push_back(pt.origin + heading.xform(Vector3(q.x, 0, q.y)));
			}
		} else {
			for (int iz = 0; iz < scan_z.size(); iz++) {
				for (int ix = 0; ix < scan_x.size(); ix++) {
					points.push_back(pt.origin + heading.xform(Vector3(scan_x[ix], 0, scan_z[iz])));
				}
			}
		}
		if (_static_ground_batch(points, scan_height, o + k)) {
			for (int i = 0; i < points.size(); i++) {
				o[k++] -= g_com;
			}
		} else {
			for (int i = 0; i < points.size(); i++) {
				o[k++] = _ground_from(points[i], scan_height) - g_com;
			}
		}
		if (!water_points.is_empty()) {
			const int scan_start = k - points.size();
			const bool wet = water_data.is_valid() && water_data->has_water_field();
			float s;
			Vector2 f;
			for (const int i : water_points) {
				ERR_FAIL_INDEX_V(i, points.size(), out);
				const Vector3 &q = points[i];
				o[k++] = wet && water_data->water_sample(q.x, q.z, s, f) ? MAX(s - (o[scan_start + i] + g_com), 0.0f) : 0.0f;
			}
			if (wet && water_data->water_sample(com.x, com.z, s, f) && s > g_com) {
				const Vector3 fl = heading.xform_inv(Vector3(f.x, 0.0f, f.y));
				o[k++] = s - g_com;
				o[k++] = fl.x;
				o[k++] = fl.z;
			} else {
				o[k++] = 0.0f;
				o[k++] = 0.0f;
				o[k++] = 0.0f;
			}
		}
	}
	return out;
}

void RigKernel::set_water(const Ref<GroundData> &p_data, const PackedInt32Array &p_points) {
	water_data = p_data;
	water_points.resize(p_points.size());
	for (int i = 0; i < p_points.size(); i++) {
		water_points.write[i] = p_points[i];
	}
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

// The reward's terms. Each is its script's (ProjectEX's learn/amp_env.gd) on update()'s state, held to it
// by a parity test (tests/kernel_check.gd): the script's float32 vectors kept as vectors, its scalars in
// double.

void RigKernel::set_sensed(const PackedInt32Array &p_bodies) {
	sensed.clear();
	touch.resize(bodies.size());
	touch_friction.resize(bodies.size());
	for (int i = 0; i < bodies.size(); i++) {
		touch.write[i] = -1;
		touch_friction.write[i] = Math::NaN;
	}
	for (int i = 0; i < p_bodies.size(); i++) {
		ERR_CONTINUE(p_bodies[i] < 0 || p_bodies[i] >= bodies.size());
		sensed.push_back(p_bodies[i]);
		touch.write[p_bodies[i]] = 0;
	}
}

bool RigKernel::touches(int p_body) const {
	ERR_FAIL_INDEX_V(p_body, bodies.size(), false);
	return _touches(p_body);
}

bool RigKernel::touches_any(const PackedInt32Array &p_bodies) const {
	for (int i = 0; i < p_bodies.size(); i++) {
		if (touches(p_bodies[i])) {
			return true;
		}
	}
	return false;
}

// The first of p_bodies touching the world: its collider's friction; NaN when none touches.
float RigKernel::contact_friction(const PackedInt32Array &p_bodies) const {
	for (int i = 0; i < p_bodies.size(); i++) {
		const int b = p_bodies[i];
		ERR_CONTINUE(b < 0 || b >= bodies.size());
		const float f = b < touch.size() && touch[b] >= 0 ? touch_friction[b] : _world_friction(b);
		if (!Math::is_nan(f)) {
			return f;
		}
	}
	return Math::NaN;
}

double RigKernel::angular_speed_squared(const PackedInt32Array &p_bodies) const {
	double sum = 0.0;
	for (int i = 0; i < p_bodies.size(); i++) {
		ERR_CONTINUE(p_bodies[i] < 0 || p_bodies[i] >= bodies.size());
		sum += ang_vel[p_bodies[i]].length_squared();
	}
	return sum;
}

// A body's slip: touching the world, its horizontal speed past p_free up to p_cap (ProjectEX's amp_env.gd
// slip_script).
double RigKernel::_slip_body(int p_body, double p_free, double p_cap) const {
	if (p_body < 0 || p_body >= lin_vel.size() || !_touches(p_body)) {
		return 0.0;
	}
	const Vector3 &v = lin_vel[p_body];
	return MIN(MAX(double(Vector2(v.x, v.z).length()) - p_free, 0.0), p_cap);
}

double RigKernel::slip(const PackedInt32Array &p_bodies, double p_free, double p_cap) const {
	double sum = 0.0;
	for (int i = 0; i < p_bodies.size(); i++) {
		ERR_CONTINUE(p_bodies[i] < 0 || p_bodies[i] >= bodies.size());
		sum += _slip_body(p_bodies[i], p_free, p_cap);
	}
	return sum;
}

// p_bodies: the left lower leg and foot, the right's; p_actions: each foot's pitch in the action;
// p_gains: the drive's stiffness and damping, its torque cap, the nominal pitch, the action's scale on it,
// people's peak ankle torque.
void RigKernel::set_ankles(const PackedInt32Array &p_bodies, const PackedInt32Array &p_actions, const PackedFloat64Array &p_gains) {
	ERR_FAIL_COND(p_bodies.size() != 4 || p_actions.size() != 2 || p_gains.size() != 6);
	for (int k = 0; k < 2; k++) {
		ERR_FAIL_INDEX(p_bodies[2 * k], bodies.size());
		ERR_FAIL_INDEX(p_bodies[2 * k + 1], bodies.size());
		ankle_lower[k] = p_bodies[2 * k];
		ankle_foot[k] = p_bodies[2 * k + 1];
		ankle_action[k] = p_actions[k];
	}
	ankle_kp = p_gains[0];
	ankle_kd = p_gains[1];
	ankle_cap = p_gains[2];
	ankle_nominal = p_gains[3];
	ankle_scale = p_gains[4];
	ankle_human = p_gains[5];
}

// Both ankles' pitch torque (the drive's, from the action's target, the angle and the rate) past people's
// peak, each as a share of the rest of its cap; with p_push_only only while it does positive work.
double RigKernel::ankle_excess(const PackedFloat32Array &p_action, bool p_push_only) const {
	ERR_FAIL_COND_V_MSG(ankle_lower[0] < 0, 0.0, "set_ankles() first.");
	double e = 0.0;
	for (int k = 0; k < 2; k++) {
		ERR_FAIL_INDEX_V(ankle_action[k], p_action.size(), 0.0);
		const Basis li = xform[ankle_lower[k]].basis.inverse();
		const double angle = rotation_vector(li * xform[ankle_foot[k]].basis).x;
		const double rate = li.xform(ang_vel[ankle_foot[k]] - ang_vel[ankle_lower[k]]).x;
		const double target = ankle_nominal + ankle_scale * CLAMP(double(p_action[ankle_action[k]]), -1.0, 1.0);
		const double t = CLAMP(ankle_kp * (target - angle) - ankle_kd * rate, -ankle_cap, ankle_cap);
		if (p_push_only && t * rate <= 0.0) {
			continue;
		}
		e += MAX(Math::abs(t) - ankle_human, 0.0) / MAX(ankle_cap - ankle_human, 1.0);
	}
	return e;
}

void RigKernel::set_knees(const PackedInt32Array &p_bodies, const PackedVector3Array &p_offsets) {
	ERR_FAIL_COND(p_bodies.size() != p_offsets.size());
	knee_bodies.clear();
	knee_offsets.clear();
	for (int i = 0; i < p_bodies.size(); i++) {
		ERR_FAIL_INDEX(p_bodies[i], bodies.size());
		knee_bodies.push_back(p_bodies[i]);
		knee_offsets.push_back(p_offsets[i]);
	}
}

// Whether a knee is down: its joint within p_height of the ground under it (ground()), where the terrain's
// slope there is at most p_max_degrees.
bool RigKernel::knee_down(double p_height, double p_max_degrees) const {
	for (int i = 0; i < knee_bodies.size(); i++) {
		const Vector3 k = xform[knee_bodies[i]].xform(knee_offsets[i]);
		const double g = ground(k);
		if (double(k.y) - g >= p_height) {
			continue;
		}
		if (terrain.is_valid()) {
			const Vector3 o = terrain->get_origin();
			const double slope = terrain->fall_line(Vector2(double(k.x) - o.x, double(k.z) - o.z)).length();
			if (Math::rad_to_deg(Math::atan(slope)) > p_max_degrees) {
				continue;
			}
		}
		return true;
	}
	return false;
}

void RigKernel::set_soles(const PackedVector3Array &p_heel, const PackedVector3Array &p_toe, float p_lift, float p_probe, float p_edge_on) {
	heel_corners.clear();
	toe_corners.clear();
	for (int i = 0; i < p_heel.size(); i++) {
		heel_corners.push_back(p_heel[i]);
	}
	for (int i = 0; i < p_toe.size(); i++) {
		toe_corners.push_back(p_toe[i]);
	}
	sole_lift = p_lift;
	sole_probe = p_probe;
	edge_on = p_edge_on;
}

double RigKernel::_terrain_ground(const Vector3 &p_point) const {
	return terrain.is_valid() ? terrain->ground_at(p_point) : 0.0;
}

// Whether a foot stands over a prop's edge: some of its sole's corners over a prop (the ground data's
// first surface down from sole_lift over the corner more than edge_on over the terrain's ground), some not.
bool RigKernel::on_edge(int p_foot, int p_toes) const {
	ERR_FAIL_INDEX_V(p_foot, bodies.size(), false);
	ERR_FAIL_INDEX_V(p_toes, bodies.size(), false);
	return sole_on_edge(xform[p_foot], xform[p_toes]);
}

// on_edge() for a foot and its toes at these transforms.
bool RigKernel::sole_on_edge(const Transform3D &p_foot, const Transform3D &p_toes) const {
	ERR_FAIL_COND_V_MSG(ground_data.is_null(), false, "on_edge() reads the ground data.");
	int on = 0;
	const int n = heel_corners.size() + toe_corners.size();
	for (int i = 0; i < n; i++) {
		const bool heel = i < heel_corners.size();
		const Vector3 p = (heel ? p_foot : p_toes).xform(heel ? heel_corners[i] : toe_corners[i - heel_corners.size()]) + Vector3(0, sole_lift, 0);
		const float surface = ground_data->cast_down(p, sole_probe);
		if (!Math::is_nan(surface) && double(surface) - _terrain_ground(p) > double(edge_on)) {
			on++;
		}
	}
	return on > 0 && on < n;
}

void RigKernel::set_legs(const PackedInt32Array &p_bodies) {
	legs.clear();
	for (int i = 0; i < p_bodies.size(); i++) {
		ERR_FAIL_INDEX(p_bodies[i], bodies.size());
		legs.push_back(p_bodies[i]);
	}
}

// Undergrowth's drag: each leg body under p_height over the terrain's ground (every one without a terrain)
// gets a constant force p_coefficient times its horizontal velocity, the others none. The physics server's
// state now (a snag may have changed a velocity since update()).
void RigKernel::leg_drag(double p_coefficient, double p_height) const {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (const int b : legs) {
		Vector3 f;
		const Transform3D t = ps->body_get_state(bodies[b], PS3DE::BODY_STATE_TRANSFORM);
		if (terrain.is_null() || double(t.origin.y) - _terrain_ground(t.origin) < p_height) {
			const Vector3 v = ps->body_get_state(bodies[b], PS3DE::BODY_STATE_LINEAR_VELOCITY);
			f = Vector3(v.x, 0.0f, v.z) * real_t(p_coefficient);
		}
		ps->body_set_constant_force(bodies[b], f);
	}
}

void RigKernel::clear_leg_drag() const {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (const int b : legs) {
		ps->body_set_constant_force(bodies[b], Vector3());
	}
}

// Every drive's gains and torque cap at k = 1 (amp_env.gd GAINS and cap_of, in ACT_DOFS order).
void RigKernel::set_drive_gains(const PackedFloat64Array &p_kp, const PackedFloat64Array &p_kd, const PackedFloat64Array &p_cap) {
	ERR_FAIL_COND_MSG(p_kp.size() != drives.size() || p_kd.size() != drives.size() || p_cap.size() != drives.size(), "One kp, kd and cap per drive.");
	drive_kp.clear();
	drive_kd.clear();
	drive_cap.clear();
	for (int i = 0; i < drives.size(); i++) {
		drive_kp.push_back(p_kp[i]);
		drive_kd.push_back(p_kd[i]);
		drive_cap.push_back(p_cap[i]);
	}
}

// The joints' mechanical power (W): every drive's PD torque estimate, kp (target - angle) - kd rate within its cap
// times the episode's strength (ankle_excess's estimate, over every driven axis; the targets as write_targets writes
// them), times the joint's rate, its magnitude summed (amp_env.gd _effort_script).
double RigKernel::effort(const PackedFloat32Array &p_action) const {
	ERR_FAIL_COND_V_MSG(drive_kp.size() != drives.size(), 0.0, "set_drive_gains() first.");
	ERR_FAIL_COND_V(p_action.size() != action_size, 0.0);
	double e = 0.0;
	int k = 0;
	for (int i = 0; i < drives.size(); i++) {
		const Drive &d = drives[i];
		const int parent = parents[d.body];
		const Basis pi = xform[parent].basis.inverse();
		const Vector3 rv = rotation_vector(pi * xform[d.body].basis);
		const Vector3 rel = pi.xform(ang_vel[d.body] - ang_vel[parent]);
		Vector3 t = d.nominal;
		for (int axis : d.axes) {
			t[axis] += d.scale[axis] * CLAMP(p_action[k], -1.0f, 1.0f);
			k++;
		}
		t = t.clamp(d.lo, d.hi);
		const double cap = drive_cap[i] * (i < drive_group.size() ? strengths[CLAMP(drive_group[i], 0, 1)] : strength);
		for (int axis : d.axes) {
			const double tau = CLAMP(drive_kp[i] * (double(t[axis]) - double(rv[axis])) - drive_kd[i] * double(rel[axis]), -cap, cap);
			e += Math::abs(tau * double(rel[axis]));
		}
	}
	return e;
}

// The legs' (with the trunk and neck) and the arms' strength k (one value: both); strength becomes the legs'.
void RigKernel::set_strengths(const PackedFloat64Array &p_k) {
	ERR_FAIL_COND(p_k.is_empty());
	strengths[0] = p_k[0];
	strengths[1] = p_k.size() > 1 ? p_k[1] : p_k[0];
	strength = strengths[0];
}

PackedFloat64Array RigKernel::get_strengths() const {
	PackedFloat64Array out;
	out.push_back(strengths[0]);
	out.push_back(strengths[1]);
	return out;
}

// Each drive's limb group (0 the legs, the trunk and the neck; 1 the arms), in drive order: effort()'s caps.
void RigKernel::set_drive_groups(const PackedInt32Array &p_groups) {
	ERR_FAIL_COND_MSG(p_groups.size() != drives.size(), "One group per drive (add_drive's order).");
	drive_group.resize(p_groups.size());
	for (int i = 0; i < p_groups.size(); i++) {
		drive_group.write[i] = CLAMP(p_groups[i], 0, 1);
	}
}

// The critic's balance quantities (ProjectEX amp_env.gd privileged(), run61; the script's _balance_script the reference),
// from the bodies update() read, every body a point mass at its origin as the COM is: the capture point (the COM's ground
// projection plus its horizontal velocity over sqrt(g / h), h the COM's height over the ground under it) against the
// support's centre (the p_support bodies touching the world, or all of them when none does; sensed bodies only), x and z
// in the heading frame; h; the COM's velocity in the heading frame; its angular momentum about the COM (the bodies' about
// it) over the mass times p_height squared, in the heading frame (1/s).
PackedFloat32Array RigKernel::balance(const PackedInt32Array &p_support, double p_height) const {
	PackedFloat32Array out;
	out.resize(9);
	float *o = out.ptrw();
	const double g = rc.gravity;
	const double ground_y = terrain.is_valid() ? terrain->ground_at(com) : double(ground(com));
	const double h = double(com.y) - ground_y;
	const double w0 = Math::sqrt(g / MAX(h, 0.2));
	Vector3 centre;
	int n = 0;
	for (const int b : p_support) {
		if (b >= 0 && b < touch.size() && touch[b] == 1) {
			centre += xform[b].origin;
			n++;
		}
	}
	if (n == 0) {
		for (const int b : p_support) {
			if (b >= 0 && b < xform.size()) {
				centre += xform[b].origin;
				n++;
			}
		}
	}
	centre = n > 0 ? centre / real_t(n) : com;
	const Vector3 cp = com + com_vel * real_t(1.0 / w0);
	const Vector3 d = heading.xform_inv(Vector3(cp.x - centre.x, 0.0f, cp.z - centre.z));
	o[0] = d.x;
	o[1] = d.z;
	o[2] = float(h);
	const Vector3 v = heading.xform_inv(com_vel);
	o[3] = v.x;
	o[4] = v.y;
	o[5] = v.z;
	Vector3 l;
	real_t m = 0.0f;
	for (int i = 0; i < bodies.size(); i++) {
		l += (xform[i].origin - com).cross(lin_vel[i] - com_vel) * masses[i];
		m += masses[i];
	}
	l = heading.xform_inv(l) / MAX(real_t(m * p_height * p_height), real_t(1e-6));
	o[6] = l.x;
	o[7] = l.y;
	o[8] = l.z;
	return out;
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
	ClassDB::bind_method(D_METHOD("set_scan_points", "points"), &RigKernel::set_scan_points);
	ClassDB::bind_method(D_METHOD("get_scan_size"), &RigKernel::get_scan_size);
	ClassDB::bind_method(D_METHOD("set_ground_mask", "mask"), &RigKernel::set_ground_mask);
	ClassDB::bind_method(D_METHOD("set_scan_height", "height"), &RigKernel::set_scan_height);
	ClassDB::bind_method(D_METHOD("set_static_ground", "enabled"), &RigKernel::set_static_ground);
	ClassDB::bind_method(D_METHOD("set_ground_data", "data"), &RigKernel::set_ground_data);
	ClassDB::bind_method(D_METHOD("get_ground_data"), &RigKernel::get_ground_data);
	ClassDB::bind_method(D_METHOD("get_action_size"), &RigKernel::get_action_size);
	ClassDB::bind_method(D_METHOD("set_water", "data", "points"), &RigKernel::set_water);
	ClassDB::bind_method(D_METHOD("get_water_data"), &RigKernel::get_water_data);
	ClassDB::bind_method(D_METHOD("get_water_size"), &RigKernel::get_water_size);
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
	ClassDB::bind_method(D_METHOD("set_terrain", "field"), &RigKernel::set_terrain);
	ClassDB::bind_method(D_METHOD("get_terrain"), &RigKernel::get_terrain);
	ClassDB::bind_method(D_METHOD("set_sensed", "bodies"), &RigKernel::set_sensed);
	ClassDB::bind_method(D_METHOD("touches", "body"), &RigKernel::touches);
	ClassDB::bind_method(D_METHOD("touches_any", "bodies"), &RigKernel::touches_any);
	ClassDB::bind_method(D_METHOD("contact_friction", "bodies"), &RigKernel::contact_friction);
	ClassDB::bind_method(D_METHOD("angular_speed_squared", "bodies"), &RigKernel::angular_speed_squared);
	ClassDB::bind_method(D_METHOD("slip", "bodies", "free", "cap"), &RigKernel::slip);
	ClassDB::bind_method(D_METHOD("set_ankles", "bodies", "actions", "gains"), &RigKernel::set_ankles);
	ClassDB::bind_method(D_METHOD("ankle_excess", "action", "push_only"), &RigKernel::ankle_excess);
	ClassDB::bind_method(D_METHOD("set_knees", "bodies", "offsets"), &RigKernel::set_knees);
	ClassDB::bind_method(D_METHOD("knee_down", "height", "max_degrees"), &RigKernel::knee_down);
	ClassDB::bind_method(D_METHOD("set_soles", "heel", "toe", "lift", "probe", "edge_on"), &RigKernel::set_soles);
	ClassDB::bind_method(D_METHOD("on_edge", "foot", "toes"), &RigKernel::on_edge);
	ClassDB::bind_method(D_METHOD("sole_on_edge", "foot", "toes"), &RigKernel::sole_on_edge);
	ClassDB::bind_method(D_METHOD("set_legs", "bodies"), &RigKernel::set_legs);
	ClassDB::bind_method(D_METHOD("leg_drag", "coefficient", "height"), &RigKernel::leg_drag);
	ClassDB::bind_method(D_METHOD("clear_leg_drag"), &RigKernel::clear_leg_drag);
	ClassDB::bind_method(D_METHOD("set_drive_gains", "kp", "kd", "cap"), &RigKernel::set_drive_gains);
	ClassDB::bind_method(D_METHOD("set_strength", "k"), &RigKernel::set_strength);
	ClassDB::bind_method(D_METHOD("set_strengths", "k"), &RigKernel::set_strengths);
	ClassDB::bind_method(D_METHOD("get_strengths"), &RigKernel::get_strengths);
	ClassDB::bind_method(D_METHOD("set_drive_groups", "groups"), &RigKernel::set_drive_groups);
	ClassDB::bind_method(D_METHOD("set_effort_weight", "weight"), &RigKernel::set_effort_weight);
	ClassDB::bind_method(D_METHOD("get_effort_weight"), &RigKernel::get_effort_weight);
	ClassDB::bind_method(D_METHOD("set_assist_level", "level"), &RigKernel::set_assist_level);
	ClassDB::bind_method(D_METHOD("get_assist_level"), &RigKernel::get_assist_level);
	ClassDB::bind_method(D_METHOD("get_strength"), &RigKernel::get_strength);
	ClassDB::bind_method(D_METHOD("effort", "action"), &RigKernel::effort);
	ClassDB::bind_method(D_METHOD("balance", "support", "height"), &RigKernel::balance);
	ClassDB::bind_method(D_METHOD("set_reward_config", "config"), &RigKernel::set_reward_config);
	ClassDB::bind_method(D_METHOD("reset_terms", "wy_mean", "features", "pelvis_prev", "g_pelvis"), &RigKernel::reset_terms);
	ClassDB::bind_method(D_METHOD("reward_step", "command", "style", "steps", "push_at", "trip_at", "switch_at", "scramble_cap", "action", "prev_action"), &RigKernel::reward_step);
	ClassDB::bind_method(D_METHOD("get_amp_pair"), &RigKernel::get_amp_pair);
	ClassDB::bind_method(D_METHOD("get_step_terms"), &RigKernel::get_step_terms);
	ClassDB::bind_method(D_METHOD("observation_full", "action", "command", "style", "strength"), &RigKernel::observation_full);
	ClassDB::bind_method(D_METHOD("get_path"), &RigKernel::get_path);
	ClassDB::bind_method(D_METHOD("get_asked"), &RigKernel::get_asked);
	ClassDB::bind_method(D_METHOD("get_along"), &RigKernel::get_along);
	ClassDB::bind_method(D_METHOD("get_edges"), &RigKernel::get_edges);
	ClassDB::bind_method(D_METHOD("get_wy_mean"), &RigKernel::get_wy_mean);
	ClassDB::bind_method(D_METHOD("get_ground_mu"), &RigKernel::get_ground_mu);
}
