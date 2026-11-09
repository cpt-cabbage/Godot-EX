/**************************************************************************/
/*  wind_zone_3d.cpp                                                      */
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

#include "wind_zone_3d.h"

#include "core/object/class_db.h"

LocalVector<WindZone3D *> WindZone3D::zones;

void WindZone3D::snapshot_all(LocalVector<Snapshot> &r_out) {
	r_out.clear();
	for (WindZone3D *z : zones) {
		if (!z->enabled || !z->is_inside_tree() || !z->is_visible_in_tree()) {
			continue;
		}
		Snapshot s;
		s.xf = z->get_global_transform();
		s.inv = s.xf.affine_inverse();
		s.mode = z->mode;
		s.strength = z->strength;
		s.radius = z->radius;
		s.height = z->height;
		s.falloff = z->falloff;
		s.turbulence = z->turbulence;
		s.downward = z->downward;
		s.aabb = s.xf.xform(AABB(Vector3(-s.radius, -s.height * 0.5f, -s.radius), Vector3(s.radius * 2.0f, s.height, s.radius * 2.0f)));
		r_out.push_back(s);
	}
}

// p_noise in [-1, 1]: the field's turbulence there, scaled by the zone's own.
Vector3 WindZone3D::Snapshot::sample(const Vector3 &p_world, float p_noise) const {
	if (!aabb.has_point(p_world)) {
		return Vector3();
	}
	const Vector3 l = inv.xform(p_world);
	const float r = Math::sqrt(l.x * l.x + l.z * l.z);
	const float hy = Math::abs(l.y) * 2.0f / height;
	const float rr = r / radius;
	if (rr >= 1.0f || hy >= 1.0f) {
		return Vector3();
	}
	auto fade = [this](float x) {
		const float edge = 1.0f - falloff;
		if (x <= edge) {
			return 1.0f;
		}
		const float t = (x - edge) / MAX(falloff, 1e-4f);
		return 1.0f - t * t * (3.0f - 2.0f * t);
	};
	const float w = fade(rr) * fade(hy) * strength * (1.0f + turbulence * p_noise);
	Vector3 local;
	switch (mode) {
		case MODE_DIRECTIONAL: {
			local = Vector3(0, 0, -1);
		} break;
		case MODE_OUTWARD: {
			const Vector3 out = r > 1e-3f ? Vector3(l.x / r, 0, l.z / r) : Vector3();
			const float axis = 1.0f - rr; // near the axis the air comes down, farther out it spreads
			local = out * (1.0f - downward * axis) + Vector3(0, -downward * axis, 0);
		} break;
		case MODE_SWIRL: {
			local = r > 1e-3f ? Vector3(-l.z / r, 0, l.x / r) : Vector3();
		} break;
	}
	return xf.basis.xform(local) * w;
}

void WindZone3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			zones.push_back(this);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			zones.erase(this);
		} break;
	}
}

void WindZone3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &WindZone3D::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &WindZone3D::get_mode);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &WindZone3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &WindZone3D::get_strength);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &WindZone3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &WindZone3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_height", "height"), &WindZone3D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &WindZone3D::get_height);
	ClassDB::bind_method(D_METHOD("set_falloff", "falloff"), &WindZone3D::set_falloff);
	ClassDB::bind_method(D_METHOD("get_falloff"), &WindZone3D::get_falloff);
	ClassDB::bind_method(D_METHOD("set_turbulence", "turbulence"), &WindZone3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &WindZone3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_downward", "downward"), &WindZone3D::set_downward);
	ClassDB::bind_method(D_METHOD("get_downward"), &WindZone3D::get_downward);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &WindZone3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &WindZone3D::is_enabled);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Directional,Outward,Swirl"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:m/s"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.1,2000,0.1,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.1,2000,0.1,or_greater,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "falloff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_falloff", "get_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "downward", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_downward", "get_downward");

	BIND_ENUM_CONSTANT(MODE_DIRECTIONAL);
	BIND_ENUM_CONSTANT(MODE_OUTWARD);
	BIND_ENUM_CONSTANT(MODE_SWIRL);
}
