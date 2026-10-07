/**************************************************************************/
/*  terrain_field.cpp                                                     */
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


#include "terrain_field.h"

#include "core/object/class_db.h"
#include "modules/noise/noise.h"

void TerrainField::set_macro(const PackedFloat32Array &p_heights, double p_half, double p_step, const Vector2 &p_up, double p_cross) {
	macro.resize(p_heights.size());
	for (int i = 0; i < p_heights.size(); i++) {
		macro[i] = p_heights[i];
	}
	macro_half = p_half;
	macro_step = p_step;
	up = p_up;
	cross = p_cross;
}

void TerrainField::set_hills(const Ref<Noise> &p_noise, double p_amplitude) {
	hills = p_noise;
	hill_amp = p_amplitude;
}

void TerrainField::set_bumps(const Ref<Noise> &p_noise, double p_amplitude) {
	bumps = p_noise;
	bump_amp = p_amplitude;
}

void TerrainField::set_water_shape(double p_bank, double p_levee, double p_levee_top) {
	bank = p_bank;
	levee = p_levee;
	levee_top = p_levee_top;
}

void TerrainField::add_pool(const Vector2 &p_center, double p_radius, double p_surface, double p_depth) {
	Water w;
	w.pool = true;
	w.c = p_center;
	w.radius = p_radius;
	w.surface = p_surface;
	w.depth = p_depth;
	waters.push_back(w);
}

void TerrainField::add_stream(const Vector2 &p_point, const Vector2 &p_direction, double p_width, double p_depth, double p_amplitude, double p_wavelength, double p_t0, double p_step, const PackedFloat32Array &p_profile) {
	ERR_FAIL_COND_MSG(p_profile.size() < 2, "A stream needs its surface's profile.");
	Water w;
	w.c = p_point;
	w.d = p_direction;
	w.width = p_width;
	w.depth = p_depth;
	w.a = p_amplitude;
	w.lambda = p_wavelength;
	w.t0 = p_t0;
	w.step = p_step;
	w.profile.resize(p_profile.size());
	for (int i = 0; i < p_profile.size(); i++) {
		w.profile[i] = p_profile[i];
	}
	waters.push_back(w);
}

void TerrainField::add_wall(const Vector2 &p_center, double p_yaw, double p_length) {
	Wall w;
	w.c = p_center;
	w.dir = Vector2::from_angle(p_yaw);
	w.length = p_length;
	walls.push_back(w);
}

// terrain.gd wall_distance.
double TerrainField::wall_distance(const Vector2 &p_point) const {
	double d = Math::INF;
	for (const Wall &w : walls) {
		const Vector2 rel = p_point - w.c;
		const double along = CLAMP(double(rel.dot(w.dir)), -0.5 * w.length, 0.5 * w.length);
		d = MIN(d, double((rel - w.dir * along).length()) - 0.15);
	}
	return d;
}

// terrain.gd _raw (its float32 vectors kept where the script has them, its scalars in double).
double TerrainField::_raw(double p_x, double p_z) const {
	double h = plane.x * p_x + plane.y * p_z;
	if (!macro.is_empty()) {
		const double s = up.x * p_x + up.y * p_z;
		const double f = CLAMP((s + macro_half) / macro_step, 0.0, double(macro.size()) - 1.001);
		const int i = int(f);
		h += Math::lerp(double(macro[i]), double(macro[i + 1]), f - i) + cross * (up.x * p_z - up.y * p_x);
	}
	if (hills.is_valid()) {
		h += hill_amp * hills->get_noise_2d(p_x, p_z);
	}
	if (bumps.is_valid()) {
		h += bump_amp * bumps->get_noise_2d(p_x, p_z);
	}
	return h;
}

// terrain.gd _carve: the water's bowl inside, the bank's blend and the levee outside.
double TerrainField::_carve(const Water &p_w, double p_x, double p_z, double p_h) const {
	const Vector2 p(p_x, p_z);
	double e;
	double inside;
	double surf;
	if (p_w.pool) {
		const double r = p.distance_to(p_w.c);
		e = r - p_w.radius;
		inside = 1.0 - Math::pow(r / p_w.radius, 2.0);
		surf = p_w.surface;
	} else {
		const Vector2 q = p - p_w.c;
		const double t = q.dot(p_w.d);
		const double k = Math::TAU / p_w.lambda;
		const double u = q.dot(p_w.d.orthogonal()) - p_w.a * Math::sin(k * t);
		e = Math::abs(u) - 0.5 * p_w.width;
		inside = 1.0 - Math::pow(2.0 * u / p_w.width, 2.0);
		if (e > bank + 4.0) {
			return p_h;
		}
		const double f = CLAMP((t - p_w.t0) / p_w.step, 0.0, double(p_w.profile.size()) - 1.001);
		const int i = int(f);
		surf = Math::lerp(double(p_w.profile[i]), double(p_w.profile[i + 1]), f - i);
	}
	if (e > bank + 4.0) {
		return p_h;
	}
	if (e <= 0.0) {
		return p_w.pool ? MIN(p_h, surf - p_w.depth * inside) : surf - p_w.depth * inside;
	}
	const double lv = surf + levee * MIN(e / 0.3, 1.0) - 0.5 * MAX(e - levee_top, 0.0);
	return MAX(lv, Math::lerp(surf, p_h, Math::smoothstep(0.0, bank, e)));
}

double TerrainField::hard(double p_x, double p_z) const {
	double h = _raw(p_x, p_z) - raw0;
	for (const Water &w : waters) {
		h = _carve(w, p_x, p_z, h);
	}
	return h;
}

double TerrainField::grade(const Vector2 &p_point, const Vector2 &p_dir) const {
	return height(p_point.x + 0.5 * p_dir.x, p_point.y + 0.5 * p_dir.y) - height(p_point.x - 0.5 * p_dir.x, p_point.y - 0.5 * p_dir.y);
}

Vector2 TerrainField::fall_line(const Vector2 &p_point) const {
	const double x = p_point.x;
	const double z = p_point.y;
	return 0.5 * Vector2(height(x + 1.0, z) - height(x - 1.0, z), height(x, z + 1.0) - height(x, z - 1.0));
}

PackedFloat32Array TerrainField::heights(const PackedVector2Array &p_points) const {
	PackedFloat32Array out;
	out.resize(p_points.size());
	float *o = out.ptrw();
	for (int i = 0; i < p_points.size(); i++) {
		o[i] = height(p_points[i].x, p_points[i].y);
	}
	return out;
}

void TerrainField::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_plane", "grade"), &TerrainField::set_plane);
	ClassDB::bind_method(D_METHOD("set_macro", "heights", "half", "step", "up", "cross"), &TerrainField::set_macro);
	ClassDB::bind_method(D_METHOD("set_hills", "noise", "amplitude"), &TerrainField::set_hills);
	ClassDB::bind_method(D_METHOD("set_bumps", "noise", "amplitude"), &TerrainField::set_bumps);
	ClassDB::bind_method(D_METHOD("set_raw0", "raw0"), &TerrainField::set_raw0);
	ClassDB::bind_method(D_METHOD("set_water_shape", "bank", "levee", "levee_top"), &TerrainField::set_water_shape);
	ClassDB::bind_method(D_METHOD("add_pool", "center", "radius", "surface", "depth"), &TerrainField::add_pool);
	ClassDB::bind_method(D_METHOD("add_stream", "point", "direction", "width", "depth", "amplitude", "wavelength", "t0", "step", "profile"), &TerrainField::add_stream);
	ClassDB::bind_method(D_METHOD("clear_waters"), &TerrainField::clear_waters);
	ClassDB::bind_method(D_METHOD("add_wall", "center", "yaw", "length"), &TerrainField::add_wall);
	ClassDB::bind_method(D_METHOD("clear_walls"), &TerrainField::clear_walls);
	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &TerrainField::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &TerrainField::get_bounds);
	ClassDB::bind_method(D_METHOD("wall_distance", "point"), &TerrainField::wall_distance);
	ClassDB::bind_method(D_METHOD("set_scale", "scale"), &TerrainField::set_scale);
	ClassDB::bind_method(D_METHOD("get_scale"), &TerrainField::get_scale);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &TerrainField::set_origin);
	ClassDB::bind_method(D_METHOD("get_origin"), &TerrainField::get_origin);
	ClassDB::bind_method(D_METHOD("hard", "x", "z"), &TerrainField::hard);
	ClassDB::bind_method(D_METHOD("height", "x", "z"), &TerrainField::height);
	ClassDB::bind_method(D_METHOD("ground_at", "point"), &TerrainField::ground_at);
	ClassDB::bind_method(D_METHOD("grade", "point", "direction"), &TerrainField::grade);
	ClassDB::bind_method(D_METHOD("fall_line", "point"), &TerrainField::fall_line);
	ClassDB::bind_method(D_METHOD("slope", "point"), &TerrainField::slope);
	ClassDB::bind_method(D_METHOD("heights", "points"), &TerrainField::heights);
}
