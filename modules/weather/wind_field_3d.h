/**************************************************************************/
/*  wind_field_3d.h                                                       */
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

// The wind (Godot-EX, 2026-11-09; design/reports/sky_weather_plan_2026-11-09.md in ProjectEX): one field that everything
// moving in the wind reads. The global wind blows toward `heading` (degrees from +X toward +Z: from north toward east in
// the valley's frame) at `speed` m/s 10 m above the ground, stronger higher up by the power law (speed x (h / 10 m) ^
// `shear_exponent`), turning by `veer` degrees a kilometre up; gusts (`gust_strength`, `gust_scale` m across) travel with
// it, and turbulence (`turbulence`, `turbulence_scale`) shakes its direction. Every WindZone3D adds its own.
//
// Each frame it is baked into a 3D texture (`extent` m, `resolution` cells; RGBA half: velocity in m/s and the
// turbulence) centred on the camera and snapped to its cells, set as the global shader parameters `wind_field`,
// `wind_field_origin`, `wind_field_size`, `wind_global` (the global wind 10 m up, x and z; its gust strength; the
// field's clock) and `wind_advect` (how far the wind has carried the gusts, x and z; the shader TIME that is true at;
// the speed it moves on at), so any shader samples it, or works the same gusts out for itself (the noise is integer
// hashed: a shader's copy gives the same values) at its own TIME, which gives its motion vectors the frame before. get_wind() gives the same wind anywhere, for scripts and physics. The ground
// for the shear is `terrain`'s (a TileTerrain3D, by its get_height), else y = 0.

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "wind_zone_3d.h"

class WindField3D : public Node3D {
	GDCLASS(WindField3D, Node3D);

	float heading = 30.0;
	float speed = 5.0;
	float shear_exponent = 0.14;
	float veer = 20.0;
	float gust_strength = 0.4;
	float gust_scale = 60.0;
	float turbulence = 0.25;
	float turbulence_scale = 8.0;
	Vector3 extent = Vector3(256, 192, 256);
	Vector3i resolution = Vector3i(64, 24, 64);
	NodePath terrain;
	bool follow_camera = true;
	Vector3 focus_position;
	bool publish = true;

	double time = 0.0;
	// How far the wind has carried its gusts and turbulence (m, x and z): the wind summed over time, so a change of
	// speed or heading changes where the pattern goes next, not where it is (time x speed x direction swept the whole
	// pattern across the valley whenever the weather turned the wind).
	double advect_x = 0.0;
	double advect_z = 0.0;
	double last_step = 0.0;
	Vector3 origin;
	Ref<ImageTexture3D> texture;
	Vector<Ref<Image>> slices;
	LocalVector<float> ground; // per column, for the bake
	LocalVector<WindZone3D::Snapshot> snaps;
	Object *terrain_object = nullptr;
	double last_bake_ms = 0.0;

	float _ground_at(float p_x, float p_z) const;
	Vector3 _global(const Vector3 &p_world, float p_ground, float &r_turb) const;
	void _bake_slice(uint32_t p_z, void *p_unused);
	void _bake(const Vector3 &p_center);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static void register_globals();

	// The wind at a world position (m/s): the global wind and every zone, as baked.
	Vector3 get_wind(const Vector3 &p_world) const;
	// The global wind at a height above the ground (m), with no gusts: for clouds.
	Vector3 get_wind_at_height(float p_height) const;
	Ref<Texture3D> get_texture() const { return texture; }
	Vector3 get_field_origin() const { return origin; }
	double get_time() const { return time; }
	Vector2 get_advect() const { return Vector2(advect_x, advect_z); }
	// The gusts' factor on the global wind at a world position (0 and up, 1 without gusts).
	float get_gust(const Vector3 &p_world) const;
	void bake_now(const Vector3 &p_center);
	double get_last_bake_ms() const { return last_bake_ms; }

	void set_heading(float p_deg) { heading = p_deg; }
	float get_heading() const { return heading; }
	void set_speed(float p_speed) { speed = MAX(p_speed, 0.0f); }
	float get_speed() const { return speed; }
	void set_shear_exponent(float p_e) { shear_exponent = CLAMP(p_e, 0.0f, 0.6f); }
	float get_shear_exponent() const { return shear_exponent; }
	void set_veer(float p_deg) { veer = p_deg; }
	float get_veer() const { return veer; }
	void set_gust_strength(float p_s) { gust_strength = CLAMP(p_s, 0.0f, 2.0f); }
	float get_gust_strength() const { return gust_strength; }
	void set_gust_scale(float p_m) { gust_scale = MAX(p_m, 1.0f); }
	float get_gust_scale() const { return gust_scale; }
	void set_turbulence(float p_t) { turbulence = CLAMP(p_t, 0.0f, 2.0f); }
	float get_turbulence() const { return turbulence; }
	void set_turbulence_scale(float p_m) { turbulence_scale = MAX(p_m, 0.5f); }
	float get_turbulence_scale() const { return turbulence_scale; }
	void set_extent(const Vector3 &p_extent);
	Vector3 get_extent() const { return extent; }
	void set_resolution(const Vector3i &p_res);
	Vector3i get_resolution() const { return resolution; }
	void set_terrain(const NodePath &p_path) { terrain = p_path; }
	NodePath get_terrain() const { return terrain; }
	void set_follow_camera(bool p_follow) { follow_camera = p_follow; }
	bool get_follow_camera() const { return follow_camera; }
	void set_focus_position(const Vector3 &p_pos) { focus_position = p_pos; }
	Vector3 get_focus_position() const { return focus_position; }
	void set_publish(bool p_publish) { publish = p_publish; }
	bool get_publish() const { return publish; }

	WindField3D();
};
