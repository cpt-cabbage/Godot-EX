/**************************************************************************/
/*  terrain_field.h                                                       */
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

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

class Noise;

// A training terrain's ground as its analytic function (ProjectEX's learn/terrain.gd: a plane, a
// mountain's climb profile, hills and bumps from noise, the water's beds carved), at a level's share of
// its hard end. The reward reads it many times a step (the ground under bodies, the grade along a way,
// the slope); in script that dominated a training server's reward. It is the script's function, not the
// height field's triangles (GroundData's): the same values to float precision, held by a parity test.
// Heights are relative to the terrain's origin (its centre), x and z too; ground_at() takes world points.
class TerrainField : public RefCounted {
	GDCLASS(TerrainField, RefCounted);

	struct Water {
		bool pool = false;
		Vector2 c; // a pool's centre, a point on a stream's line
		Vector2 d; // a stream's direction
		double width = 0.0;
		double radius = 0.0;
		double depth = 0.0;
		double surface = 0.0; // a pool's
		double a = 0.0; // a stream's meander amplitude
		double lambda = 1.0; // and wavelength
		double t0 = 0.0; // a stream's profile: its start along the line, its step, the surface
		double step = 1.0;
		LocalVector<float> profile;
	};

	Vector2 plane;
	LocalVector<float> macro;
	double macro_half = 0.0;
	double macro_step = 1.0;
	Vector2 up;
	double cross = 0.0;
	Ref<Noise> hills;
	double hill_amp = 0.0;
	Ref<Noise> bumps;
	double bump_amp = 0.0;
	double raw0 = 0.0;
	double bank = 1.5;
	double levee = 0.15;
	double levee_top = 2.0;
	LocalVector<Water> waters;
	double scale = 1.0;
	Vector3 origin;

	double _raw(double p_x, double p_z) const;
	double _carve(const Water &p_w, double p_x, double p_z, double p_h) const;

protected:
	static void _bind_methods();

public:
	void set_plane(const Vector2 &p_grade) { plane = p_grade; }
	void set_macro(const PackedFloat32Array &p_heights, double p_half, double p_step, const Vector2 &p_up, double p_cross);
	void set_hills(const Ref<Noise> &p_noise, double p_amplitude);
	void set_bumps(const Ref<Noise> &p_noise, double p_amplitude);
	void set_raw0(double p_raw0) { raw0 = p_raw0; }
	void set_water_shape(double p_bank, double p_levee, double p_levee_top);
	void add_pool(const Vector2 &p_center, double p_radius, double p_surface, double p_depth);
	void add_stream(const Vector2 &p_point, const Vector2 &p_direction, double p_width, double p_depth, double p_amplitude, double p_wavelength, double p_t0, double p_step, const PackedFloat32Array &p_profile);
	void clear_waters() { waters.clear(); }
	void set_scale(double p_scale) { scale = p_scale; }
	double get_scale() const { return scale; }
	void set_origin(const Vector3 &p_origin) { origin = p_origin; }
	Vector3 get_origin() const { return origin; }

	double hard(double p_x, double p_z) const; // at the hard end
	double height(double p_x, double p_z) const { return scale * hard(p_x, p_z); }
	double ground_at(const Vector3 &p_point) const { return origin.y + height(p_point.x - origin.x, p_point.z - origin.z); }
	// The rise per metre along the unit direction p_dir at p_point over +-0.5 m.
	double grade(const Vector2 &p_point, const Vector2 &p_dir) const;
	// The uphill gradient (rise per metre in x and z) at p_point over +-1 m, and its length.
	Vector2 fall_line(const Vector2 &p_point) const;
	double slope(const Vector2 &p_point) const { return fall_line(p_point).length(); }
	PackedFloat32Array heights(const PackedVector2Array &p_points) const;
};
