/**************************************************************************/
/*  water_forces.cpp                                                      */
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

#include "water_forces.h"

#include "core/object/class_db.h"
#include "servers/physics_3d/direct_states/physics_direct_body_state_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

void WaterForces::_add(const RID &p_body, Kind p_kind, float p_volume, const Vector3 &p_center, const Vector3 &p_size, int p_axis) {
	ERR_FAIL_COND_MSG(p_volume <= 0.0f, "A body's volume must be positive.");
	ERR_FAIL_INDEX(p_axis, 3);
	Body b;
	b.rid = p_body;
	b.kind = p_kind;
	b.volume = p_volume;
	b.center = p_center;
	b.size = p_size;
	b.axis = p_axis;
	bodies.push_back(b);
}

void WaterForces::add_box(const RID &p_body, float p_volume, const Vector3 &p_center, const Vector3 &p_half_extents) {
	_add(p_body, BOX, p_volume, p_center, p_half_extents, p_half_extents.max_axis_index());
}

void WaterForces::add_capsule(const RID &p_body, float p_volume, const Vector3 &p_center, int p_axis, float p_radius, float p_height) {
	_add(p_body, CAPSULE, p_volume, p_center, Vector3(p_radius, MAX(0.5f * p_height - p_radius, 0.0f), 0.0f), p_axis);
}

void WaterForces::add_sphere(const RID &p_body, float p_volume, const Vector3 &p_center, float p_radius) {
	_add(p_body, SPHERE, p_volume, p_center, Vector3(p_radius, 0.0f, 0.0f), 1);
}

float WaterForces::apply(const Ref<GroundData> &p_water) {
	submerged = 0.0f;
	if (p_water.is_null() || !p_water->has_water_field() || bodies.is_empty()) {
		return 0.0f;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const Transform3D first = ps->body_get_state(bodies[0].rid, PS3DE::BODY_STATE_TRANSFORM);
	if (p_water->water_top_near(first.origin, reach) < first.origin.y - reach) {
		return 0.0f;
	}
	float total = 0.0f;
	float wet = 0.0f;
	for (const Body &b : bodies) {
		total += b.volume;
		PhysicsDirectBodyState3D *state = ps->body_get_direct_state(b.rid);
		if (state == nullptr) {
			continue;
		}
		const Transform3D xf = state->get_transform();
		const Basis &r = xf.basis;
		const Vector3 c = xf.xform(b.center);
		float surface;
		Vector2 flow;
		if (!p_water->water_sample(c.x, c.z, surface, flow)) {
			continue;
		}
		// The shape's vertical half extent and its long axis's half segment.
		const Vector3 a = r.get_column(b.axis).normalized();
		float hy = 0.0f;
		float half_seg = 0.0f;
		switch (b.kind) {
			case BOX:
				for (int i = 0; i < 3; i++) {
					hy += Math::abs(r.get_column(i).normalized().y) * b.size[i];
				}
				half_seg = b.size[b.axis];
				break;
			case CAPSULE:
				hy = Math::abs(a.y) * b.size.y + b.size.x;
				half_seg = b.size.y;
				break;
			case SPHERE:
				hy = b.size.x;
				break;
		}
		const float bottom = c.y - hy;
		const float share = CLAMP((surface - bottom) / MAX(2.0f * hy, 1e-4f), 0.0f, 1.0f);
		if (share <= 0.0f) {
			continue;
		}
		wet += share * b.volume;
		// The submerged part's centre: along the long axis, the part of the segment under the surface
		// (all of it when both ends are, or neither: then a level body's surface cuts its thickness).
		Vector3 cb = c;
		if (share < 1.0f && half_seg > 0.0f) {
			const Vector3 p0 = c - a * half_seg;
			const Vector3 p1 = c + a * half_seg;
			const bool under0 = p0.y < surface;
			const bool under1 = p1.y < surface;
			if (under0 != under1) {
				const float t = (surface - p0.y) / (p1.y - p0.y);
				const Vector3 cross = p0 + t * (p1 - p0);
				cb = 0.5f * ((under0 ? p0 : p1) + cross);
			}
		}
		cb.y = 0.5f * (bottom + MIN(surface, c.y + hy));
		const Vector3 com = xf.origin + state->get_center_of_mass();
		const Vector3 w = state->get_angular_velocity();
		const Vector3 u = state->get_linear_velocity() + w.cross(cb - com) - Vector3(flow.x, 0.0f, flow.y);
		Vector3 force(0.0f, density * gravity * b.volume * share, 0.0f);
		const float speed = u.length();
		if (speed > 1e-4f) {
			const Vector3 d = u / speed;
			float area = 0.0f;
			switch (b.kind) {
				case BOX:
					for (int i = 0; i < 3; i++) {
						const int j = (i + 1) % 3;
						const int k = (i + 2) % 3;
						area += Math::abs(r.get_column(i).normalized().dot(d)) * 4.0f * b.size[j] * b.size[k];
					}
					break;
				case CAPSULE: {
					const float along = Math::abs(a.dot(d));
					area = 4.0f * b.size.x * b.size.y * Math::sqrt(MAX(1.0f - along * along, 0.0f)) + float(Math::PI) * b.size.x * b.size.x;
				} break;
				case SPHERE:
					area = float(Math::PI) * b.size.x * b.size.x;
					break;
			}
			// At most what stops u within the tick (a light, flat body, a hand, would overshoot: explicit).
			const float stop = speed / MAX(state->get_inverse_mass() * state->get_step(), 1e-6f);
			force -= MIN(0.5f * density * drag * area * share * speed * speed, stop) * d;
		}
		state->apply_force(force, cb - xf.origin);
		// The spin drag across the long axis.
		if (half_seg > 0.0f) {
			const Vector3 spin = w - a * a.dot(w);
			const float rate = spin.length();
			if (rate > 1e-4f) {
				const float length = 2.0f * half_seg + (b.kind == CAPSULE ? 2.0f * b.size.x : 0.0f);
				float width = 2.0f * b.size.x;
				if (b.kind == BOX) {
					width = b.size[(b.axis + 1) % 3] + b.size[(b.axis + 2) % 3];
				}
				const float l2 = length * length;
				Vector3 torque = -spin_drag * density * width * l2 * l2 / 64.0f * share * rate * spin;
				const float change = (state->get_inverse_inertia_tensor().xform(torque) * state->get_step()).length();
				if (change > rate) {
					torque *= rate / change; // at most what stops the spin within the tick
				}
				state->apply_torque(torque);
			}
		}
	}
	submerged = total > 0.0f ? wet / total : 0.0f;
	return submerged;
}

void WaterForces::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &WaterForces::clear);
	ClassDB::bind_method(D_METHOD("add_box", "body", "volume", "center", "half_extents"), &WaterForces::add_box);
	ClassDB::bind_method(D_METHOD("add_capsule", "body", "volume", "center", "axis", "radius", "height"), &WaterForces::add_capsule);
	ClassDB::bind_method(D_METHOD("add_sphere", "body", "volume", "center", "radius"), &WaterForces::add_sphere);
	ClassDB::bind_method(D_METHOD("get_body_count"), &WaterForces::get_body_count);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &WaterForces::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &WaterForces::get_density);
	ClassDB::bind_method(D_METHOD("set_gravity", "gravity"), &WaterForces::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &WaterForces::get_gravity);
	ClassDB::bind_method(D_METHOD("set_drag", "drag"), &WaterForces::set_drag);
	ClassDB::bind_method(D_METHOD("get_drag"), &WaterForces::get_drag);
	ClassDB::bind_method(D_METHOD("set_spin_drag", "drag"), &WaterForces::set_spin_drag);
	ClassDB::bind_method(D_METHOD("get_spin_drag"), &WaterForces::get_spin_drag);
	ClassDB::bind_method(D_METHOD("set_reach", "reach"), &WaterForces::set_reach);
	ClassDB::bind_method(D_METHOD("get_reach"), &WaterForces::get_reach);
	ClassDB::bind_method(D_METHOD("apply", "water"), &WaterForces::apply);
	ClassDB::bind_method(D_METHOD("get_submerged"), &WaterForces::get_submerged);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag"), "set_drag", "get_drag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spin_drag"), "set_spin_drag", "get_spin_drag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reach"), "set_reach", "get_reach");
}
