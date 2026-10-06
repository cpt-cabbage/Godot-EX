/**************************************************************************/
/*  ground_data.cpp                                                       */
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


#include "ground_data.h"

#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "core/templates/pair.h"

void GroundData::set_height_field(const Transform3D &p_xform, int p_width, int p_depth, const PackedFloat32Array &p_heights) {
	ERR_FAIL_COND_MSG(p_width < 2 || p_depth < 2 || p_heights.size() != p_width * p_depth, "The height field must be at least 2 x 2, its heights width x depth.");
	field_width = p_width;
	field_depth = p_depth;
	field.resize(p_heights.size());
	for (int i = 0; i < p_heights.size(); i++) {
		field[i] = p_heights[i];
	}
	has_field = true;
	set_height_field_transform(p_xform);
}

void GroundData::set_height_field_transform(const Transform3D &p_xform) {
	const Basis &b = p_xform.basis;
	ERR_FAIL_COND_MSG(!Math::is_zero_approx(b.rows[0][1]) || !Math::is_zero_approx(b.rows[0][2]) || !Math::is_zero_approx(b.rows[1][0]) || !Math::is_zero_approx(b.rows[1][2]) || !Math::is_zero_approx(b.rows[2][0]) || !Math::is_zero_approx(b.rows[2][1]),
			"The height field's transform must be a scale and a translation (no rotation).");
	field_xform = p_xform;
}

void GroundData::clear_props() {
	props.clear();
	buckets.clear();
}

void GroundData::_add_prop(const Prop &p_prop) {
	const int index = props.size();
	props.push_back(p_prop);
	const AABB &a = p_prop.aabb;
	const int x0 = int(Math::floor(a.position.x / BUCKET));
	const int x1 = int(Math::floor((a.position.x + a.size.x) / BUCKET));
	const int z0 = int(Math::floor(a.position.z / BUCKET));
	const int z1 = int(Math::floor((a.position.z + a.size.z) / BUCKET));
	for (int x = x0; x <= x1; x++) {
		for (int z = z0; z <= z1; z++) {
			buckets[_bucket_key(x, z)].push_back(index);
		}
	}
}

void GroundData::add_box(const Transform3D &p_xform, const Vector3 &p_half_extents) {
	Prop p;
	p.kind = Prop::BOX;
	p.xform = p_xform;
	p.inverse = p_xform.affine_inverse();
	p.size = p_half_extents;
	p.aabb = p_xform.xform(AABB(-p_half_extents, 2.0f * p_half_extents));
	_add_prop(p);
}

void GroundData::add_cylinder(const Transform3D &p_xform, float p_radius, float p_height) {
	Prop p;
	p.kind = Prop::CYLINDER;
	p.xform = p_xform;
	p.inverse = p_xform.affine_inverse();
	p.size = Vector3(p_radius, 0.5f * p_height, 0.0f);
	p.aabb = p_xform.xform(AABB(Vector3(-p_radius, -0.5f * p_height, -p_radius), Vector3(2.0f * p_radius, p_height, 2.0f * p_radius)));
	_add_prop(p);
}

// The height field under p_from: its local frame is the shape's (vertices at (x - (w - 1) / 2, h, z -
// (d - 1) / 2)), scaled and placed by field_xform; each cell split along (x + 1, z)-(x, z + 1).
bool GroundData::_field_hit(const Vector3 &p_from, float p_length, float &r_y) const {
	if (!has_field) {
		return false;
	}
	const Vector3 s = field_xform.basis.get_scale_abs();
	const Vector3 &o = field_xform.origin;
	const float fx = (p_from.x - o.x) / s.x + 0.5f * (field_width - 1);
	const float fz = (p_from.z - o.z) / s.z + 0.5f * (field_depth - 1);
	if (fx < 0.0f || fz < 0.0f || fx > field_width - 1 || fz > field_depth - 1) {
		return false;
	}
	const int ix = MIN(int(fx), field_width - 2);
	const int iz = MIN(int(fz), field_depth - 2);
	const float a = fx - ix;
	const float b = fz - iz;
	const float h00 = field[iz * field_width + ix];
	const float h10 = field[iz * field_width + ix + 1];
	const float h01 = field[(iz + 1) * field_width + ix];
	const float h11 = field[(iz + 1) * field_width + ix + 1];
	float h;
	if (a + b <= 1.0f) {
		h = h00 + a * (h10 - h00) + b * (h01 - h00);
	} else {
		h = h11 + (1.0f - a) * (h01 - h11) + (1.0f - b) * (h10 - h11);
	}
	if (Math::is_nan(h)) {
		return false; // a hole
	}
	const float y = o.y + field_xform.basis.rows[1][1] * h;
	if (y > p_from.y || y < p_from.y - p_length) {
		return false; // above the ray's start (it starts under the surface, which it does not cross) or past its end
	}
	r_y = y;
	return true;
}

// A downward ray against one prop, in its local frame. A ray starting inside does not hit it.
bool GroundData::_prop_hit(const Prop &p_prop, const Vector3 &p_from, float p_length, float &r_y) {
	const Vector3 o = p_prop.inverse.xform(p_from);
	const Vector3 d = p_prop.inverse.basis.xform(Vector3(0.0f, -p_length, 0.0f)); // t in [0, 1]
	float t_hit = Math::INF;
	if (p_prop.kind == Prop::BOX) {
		const Vector3 &e = p_prop.size;
		if (Math::abs(o.x) <= e.x && Math::abs(o.y) <= e.y && Math::abs(o.z) <= e.z) {
			return false;
		}
		float t0 = 0.0f;
		float t1 = 1.0f;
		for (int i = 0; i < 3; i++) {
			if (Math::abs(d[i]) < 1e-9f) {
				if (o[i] < -e[i] || o[i] > e[i]) {
					return false;
				}
				continue;
			}
			float ta = (-e[i] - o[i]) / d[i];
			float tb = (e[i] - o[i]) / d[i];
			if (ta > tb) {
				SWAP(ta, tb);
			}
			t0 = MAX(t0, ta);
			t1 = MIN(t1, tb);
			if (t0 > t1) {
				return false;
			}
		}
		t_hit = t0;
	} else {
		const float r = p_prop.size.x;
		const float hh = p_prop.size.y;
		const float c = o.x * o.x + o.z * o.z - r * r;
		if (c <= 0.0f && Math::abs(o.y) <= hh) {
			return false;
		}
		// The side: |o.xz + t d.xz| = r, the hit within the height.
		const float qa = d.x * d.x + d.z * d.z;
		if (qa > 1e-12f) {
			const float qb = 2.0f * (o.x * d.x + o.z * d.z);
			const float disc = qb * qb - 4.0f * qa * c;
			if (disc >= 0.0f) {
				const float t = (-qb - Math::sqrt(disc)) / (2.0f * qa);
				if (t >= 0.0f && t <= 1.0f && Math::abs(o.y + t * d.y) <= hh) {
					t_hit = MIN(t_hit, t);
				}
			}
		}
		// The caps.
		if (Math::abs(d.y) > 1e-9f) {
			for (const float cap : { hh, -hh }) {
				const float t = (cap - o.y) / d.y;
				if (t >= 0.0f && t <= 1.0f) {
					const float x = o.x + t * d.x;
					const float z = o.z + t * d.z;
					if (x * x + z * z <= r * r) {
						t_hit = MIN(t_hit, t);
					}
				}
			}
		}
	}
	if (t_hit > 1.0f) {
		return false;
	}
	r_y = p_from.y - t_hit * p_length;
	return true;
}

float GroundData::cast_down(const Vector3 &p_from, float p_length) const {
	float best = -Math::INF;
	float y;
	if (_field_hit(p_from, p_length, y)) {
		best = y;
	}
	const LocalVector<int> *cell = buckets.getptr(_bucket_key(int(Math::floor(p_from.x / BUCKET)), int(Math::floor(p_from.z / BUCKET))));
	if (cell != nullptr) {
		for (const int i : *cell) {
			if (_prop_hit(props[i], p_from, p_length, y) && y > best) {
				best = y;
			}
		}
	}
	return best == -Math::INF ? Math::NaN : best;
}

PackedFloat32Array GroundData::cast_down_batch(const PackedVector3Array &p_from, float p_length) const {
	PackedFloat32Array out;
	out.resize(p_from.size());
	float *o = out.ptrw();
	for (int i = 0; i < p_from.size(); i++) {
		o[i] = cast_down(p_from[i], p_length);
	}
	return out;
}

void GroundData::set_water_field(const Transform3D &p_xform, int p_width, int p_depth, const PackedFloat32Array &p_surface, const PackedVector2Array &p_flow) {
	ERR_FAIL_COND_MSG(p_width < 2 || p_depth < 2 || p_surface.size() != p_width * p_depth || p_flow.size() != p_width * p_depth, "The water field must be at least 2 x 2, its surface and flow width x depth.");
	water_width = p_width;
	water_depth = p_depth;
	water_surface.resize(p_surface.size());
	water_flow.resize(p_flow.size());
	for (int i = 0; i < p_surface.size(); i++) {
		water_surface[i] = p_surface[i];
		water_flow[i] = p_flow[i];
	}
	has_water = true;
	set_water_transform(p_xform, water_flow_scale);
}

void GroundData::set_water_transform(const Transform3D &p_xform, float p_flow_scale) {
	const Basis &b = p_xform.basis;
	ERR_FAIL_COND_MSG(!Math::is_zero_approx(b.rows[0][1]) || !Math::is_zero_approx(b.rows[0][2]) || !Math::is_zero_approx(b.rows[1][0]) || !Math::is_zero_approx(b.rows[1][2]) || !Math::is_zero_approx(b.rows[2][0]) || !Math::is_zero_approx(b.rows[2][1]),
			"The water field's transform must be a scale and a translation (no rotation).");
	water_xform = p_xform;
	water_flow_scale = p_flow_scale;
}

void GroundData::clear_water() {
	has_water = false;
	water_surface.clear();
	water_flow.clear();
}

bool GroundData::water_sample(float p_x, float p_z, float &r_surface, Vector2 &r_flow) const {
	r_surface = Math::NaN;
	r_flow = Vector2();
	if (!has_water || !enabled) {
		return false;
	}
	const Basis &b = water_xform.basis;
	const Vector3 &o = water_xform.origin;
	const float fx = (p_x - o.x) / b.rows[0][0] + 0.5f * (water_width - 1);
	const float fz = (p_z - o.z) / b.rows[2][2] + 0.5f * (water_depth - 1);
	if (fx < 0.0f || fz < 0.0f || fx > water_width - 1 || fz > water_depth - 1) {
		return false;
	}
	const int ix = MIN(int(fx), water_width - 2);
	const int iz = MIN(int(fz), water_depth - 2);
	const float a = fx - ix;
	const float c = fz - iz;
	const int idx[4] = { iz * water_width + ix, iz * water_width + ix + 1, (iz + 1) * water_width + ix, (iz + 1) * water_width + ix + 1 };
	const float wt[4] = { (1.0f - a) * (1.0f - c), a * (1.0f - c), (1.0f - a) * c, a * c };
	float h = 0.0f;
	Vector2 f;
	for (int i = 0; i < 4; i++) {
		const float s = water_surface[idx[i]];
		if (Math::is_nan(s)) {
			return false; // a cell with a dry corner is dry: the field reaches past the shore
		}
		h += wt[i] * s;
		f += wt[i] * water_flow[idx[i]];
	}
	r_surface = o.y + b.rows[1][1] * h;
	r_flow = water_flow_scale * f;
	return true;
}

Vector3 GroundData::water_at(const Vector3 &p_point) const {
	float s;
	Vector2 f;
	water_sample(p_point.x, p_point.z, s, f);
	return Vector3(s, f.x, f.y);
}

float GroundData::water_top_near(const Vector3 &p_point, float p_radius) const {
	if (!has_water || !enabled) {
		return -Math::INF;
	}
	const Basis &b = water_xform.basis;
	const Vector3 &o = water_xform.origin;
	const int x0 = MAX(int(Math::floor((p_point.x - p_radius - o.x) / b.rows[0][0] + 0.5f * (water_width - 1))), 0);
	const int x1 = MIN(int(Math::ceil((p_point.x + p_radius - o.x) / b.rows[0][0] + 0.5f * (water_width - 1))), water_width - 1);
	const int z0 = MAX(int(Math::floor((p_point.z - p_radius - o.z) / b.rows[2][2] + 0.5f * (water_depth - 1))), 0);
	const int z1 = MIN(int(Math::ceil((p_point.z + p_radius - o.z) / b.rows[2][2] + 0.5f * (water_depth - 1))), water_depth - 1);
	float top = -Math::INF;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float s = water_surface[z * water_width + x];
			if (!Math::is_nan(s)) {
				top = MAX(top, o.y + b.rows[1][1] * s);
			}
		}
	}
	return top;
}

PackedFloat32Array GroundData::nearest_trunks(const Transform3D &p_xform, int p_count, float p_range) const {
	PackedFloat32Array out;
	out.resize(4 * p_count);
	float *o = out.ptrw();
	for (int i = 0; i < out.size(); i++) {
		o[i] = 0.0f;
	}
	// Candidates: the props in the buckets the range covers, each once.
	const Vector3 c = p_xform.origin;
	const int x0 = int(Math::floor((c.x - p_range) / BUCKET));
	const int x1 = int(Math::floor((c.x + p_range) / BUCKET));
	const int z0 = int(Math::floor((c.z - p_range) / BUCKET));
	const int z1 = int(Math::floor((c.z + p_range) / BUCKET));
	LocalVector<Pair<float, int>> found;
	HashSet<int> seen;
	for (int x = x0; x <= x1; x++) {
		for (int z = z0; z <= z1; z++) {
			const LocalVector<int> *cell = buckets.getptr(_bucket_key(x, z));
			if (cell == nullptr) {
				continue;
			}
			for (const int i : *cell) {
				const Prop &p = props[i];
				if (p.kind != Prop::CYLINDER || 2.0f * p.size.y < TRUNK_HEIGHT || seen.has(i)) {
					continue;
				}
				const Vector3 axis = p.xform.basis.get_column(1).normalized();
				if (Math::abs(axis.y) < 0.866f) {
					continue; // lying or leaning past 30 deg: a log, which the scan shows
				}
				seen.insert(i);
				const Vector3 d = p.xform.origin - c;
				const float dist = Vector2(d.x, d.z).length() - p.size.x; // to its surface
				if (dist <= p_range) {
					found.push_back(Pair<float, int>(dist, i));
				}
			}
		}
	}
	found.sort_custom<PairSort<float, int>>();
	const Transform3D inv = p_xform.affine_inverse();
	for (int k = 0; k < MIN(p_count, (int)found.size()); k++) {
		const Prop &p = props[found[k].second];
		const Vector3 l = inv.xform(p.xform.origin);
		o[4 * k] = l.x;
		o[4 * k + 1] = l.z;
		o[4 * k + 2] = p.size.x;
		o[4 * k + 3] = 1.0f;
	}
	return out;
}

void GroundData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("nearest_trunks", "xform", "count", "range"), &GroundData::nearest_trunks);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &GroundData::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &GroundData::is_enabled);
	ClassDB::bind_method(D_METHOD("set_height_field", "xform", "width", "depth", "heights"), &GroundData::set_height_field);
	ClassDB::bind_method(D_METHOD("set_height_field_transform", "xform"), &GroundData::set_height_field_transform);
	ClassDB::bind_method(D_METHOD("clear_props"), &GroundData::clear_props);
	ClassDB::bind_method(D_METHOD("add_box", "xform", "half_extents"), &GroundData::add_box);
	ClassDB::bind_method(D_METHOD("add_cylinder", "xform", "radius", "height"), &GroundData::add_cylinder);
	ClassDB::bind_method(D_METHOD("get_prop_count"), &GroundData::get_prop_count);
	ClassDB::bind_method(D_METHOD("cast_down", "from", "length"), &GroundData::cast_down);
	ClassDB::bind_method(D_METHOD("set_water_field", "xform", "width", "depth", "surface", "flow"), &GroundData::set_water_field);
	ClassDB::bind_method(D_METHOD("set_water_transform", "xform", "flow_scale"), &GroundData::set_water_transform);
	ClassDB::bind_method(D_METHOD("clear_water"), &GroundData::clear_water);
	ClassDB::bind_method(D_METHOD("has_water_field"), &GroundData::has_water_field);
	ClassDB::bind_method(D_METHOD("water_at", "point"), &GroundData::water_at);
	ClassDB::bind_method(D_METHOD("water_top_near", "point", "radius"), &GroundData::water_top_near);
	ClassDB::bind_method(D_METHOD("cast_down_batch", "from", "length"), &GroundData::cast_down_batch);
}
