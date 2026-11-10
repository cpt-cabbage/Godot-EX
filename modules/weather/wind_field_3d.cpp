/**************************************************************************/
/*  wind_field_3d.cpp                                                     */
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

#include "wind_field_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "modules/tile_terrain/tile_terrain_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_globals.h"

// Value noise, -1 to 1, smooth.
static _FORCE_INLINE_ float hash3(int x, int y, int z) {
	uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(z) * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float((h ^ (h >> 16)) & 0xFFFFFFu) * (2.0f / 16777215.0f) - 1.0f;
}

static float noise3(float x, float y, float z) {
	const int ix = int(Math::floor(x)), iy = int(Math::floor(y)), iz = int(Math::floor(z));
	float fx = x - ix, fy = y - iy, fz = z - iz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float a = Math::lerp(hash3(ix, iy, iz), hash3(ix + 1, iy, iz), fx);
	const float b = Math::lerp(hash3(ix, iy + 1, iz), hash3(ix + 1, iy + 1, iz), fx);
	const float c = Math::lerp(hash3(ix, iy, iz + 1), hash3(ix + 1, iy, iz + 1), fx);
	const float d = Math::lerp(hash3(ix, iy + 1, iz + 1), hash3(ix + 1, iy + 1, iz + 1), fx);
	return Math::lerp(Math::lerp(a, b, fy), Math::lerp(c, d, fy), fz);
}

// Once, at the module's start (register_types.cpp), so shaders that read them compile; a node entering the tree later
// finds them there.
void WindField3D::register_globals() {
	static bool registered = false;
	RenderingServer *rs = RS::get_singleton();
	if (registered || rs == nullptr) {
		return;
	}
	registered = true;
	rs->global_shader_parameter_add("wind_field", RSE::GLOBAL_VAR_TYPE_SAMPLER3D, RID());
	rs->global_shader_parameter_add("wind_field_origin", RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rs->global_shader_parameter_add("wind_field_size", RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(1, 1, 1));
	rs->global_shader_parameter_add("wind_global", RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
	rs->global_shader_parameter_add("wind_advect", RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
}

void WindField3D::set_extent(const Vector3 &p_extent) {
	extent = p_extent.max(Vector3(1, 1, 1));
}

void WindField3D::set_resolution(const Vector3i &p_res) {
	resolution = p_res.clamp(Vector3i(2, 2, 2), Vector3i(256, 128, 256));
	texture.unref();
	slices.clear();
}

float WindField3D::_ground_at(float p_x, float p_z) const {
	const TileTerrain3D *t = Object::cast_to<TileTerrain3D>(terrain_object);
	if (t == nullptr) {
		return 0.0f;
	}
	const float h = t->get_height(p_x, p_z);
	return Math::is_nan(h) ? 0.0f : h;
}

// The global wind at a point whose ground is p_ground: shear with the height above it, veer, gusts travelling with the
// wind, turbulence in its direction. r_turb, the turbulence there (0-1).
Vector3 WindField3D::_global(const Vector3 &p, float p_ground, float &r_turb) const {
	const float h = p.y - p_ground;
	if (h < -1.0f) {
		r_turb = 0.0f;
		return Vector3(); // inside the ground
	}
	const float hh = MAX(h, 0.3f);
	const float s = speed * Math::pow(hh / 10.0f, shear_exponent);
	const float ang = Math::deg_to_rad(heading + veer * hh / 1000.0f);
	const Vector3 dir(Math::cos(ang), 0.0f, Math::sin(ang));
	// Gusts: a noise carried along with the wind, large and slow; turbulence: a small fast one.
	const Vector3 carried = p - Vector3(float(advect_x), 0.0f, float(advect_z));
	const float g = noise3(carried.x / gust_scale, carried.y / (gust_scale * 0.5f), carried.z / gust_scale) * 0.7f +
			noise3(carried.x / (gust_scale * 0.4f), float(time) * 0.1f, carried.z / (gust_scale * 0.4f)) * 0.3f;
	const float n1 = noise3(carried.x / turbulence_scale, carried.y / turbulence_scale + float(time) * 0.7f, carried.z / turbulence_scale);
	const float n2 = noise3(carried.z / turbulence_scale + 31.7f, carried.y / turbulence_scale, carried.x / turbulence_scale + float(time) * 0.7f);
	r_turb = CLAMP(turbulence * (0.5f + 0.5f * Math::abs(n1)), 0.0f, 1.0f);
	const float turn = turbulence * n1 * 0.6f;
	const Vector3 d2(Math::cos(ang + turn), turbulence * n2 * 0.25f, Math::sin(ang + turn));
	const float near_ground = CLAMP(h + 1.0f, 0.0f, 1.0f);
	return d2 * s * MAX(0.0f, 1.0f + gust_strength * g) * near_ground;
}

Vector3 WindField3D::get_wind(const Vector3 &p_world) const {
	if (terrain_object == nullptr && !terrain.is_empty() && is_inside_tree()) {
		const_cast<WindField3D *>(this)->terrain_object = get_node_or_null(terrain);
	}
	float turb;
	const Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	Vector3 w = _global(local, _ground_at(local.x, local.z), turb);
	LocalVector<WindZone3D::Snapshot> zs;
	WindZone3D::snapshot_all(zs);
	for (const WindZone3D::Snapshot &z : zs) {
		w += z.sample(p_world, noise3(p_world.x * 0.2f, float(time) * 1.3f, p_world.z * 0.2f));
	}
	return w;
}

float WindField3D::get_gust(const Vector3 &p_world) const {
	const Vector3 p = get_global_transform().affine_inverse().xform(p_world);
	const Vector3 carried = p - Vector3(float(advect_x), 0.0f, float(advect_z));
	const float g = noise3(carried.x / gust_scale, carried.y / (gust_scale * 0.5f), carried.z / gust_scale) * 0.7f +
			noise3(carried.x / (gust_scale * 0.4f), float(time) * 0.1f, carried.z / (gust_scale * 0.4f)) * 0.3f;
	return MAX(0.0f, 1.0f + gust_strength * g);
}

Vector3 WindField3D::get_wind_at_height(float p_height) const {
	const float hh = MAX(p_height, 0.3f);
	const float s = speed * Math::pow(hh / 10.0f, shear_exponent);
	const float ang = Math::deg_to_rad(heading + veer * hh / 1000.0f);
	return Vector3(Math::cos(ang), 0.0f, Math::sin(ang)) * s;
}

void WindField3D::_bake_slice(uint32_t p_z, void *p_unused) {
	const Vector3 cell = extent / Vector3(resolution);
	const int nx = resolution.x, ny = resolution.y;
	uint16_t *out = (uint16_t *)slices.write[p_z]->ptrw();
	const float z = origin.z + (p_z + 0.5f) * cell.z;
	for (int iy = 0; iy < ny; iy++) {
		const float y = origin.y + (iy + 0.5f) * cell.y;
		for (int ix = 0; ix < nx; ix++) {
			const float x = origin.x + (ix + 0.5f) * cell.x;
			const Vector3 p(x, y, z);
			float turb;
			Vector3 w = _global(p, ground[p_z * nx + ix], turb);
			for (const WindZone3D::Snapshot &zs : snaps) {
				w += zs.sample(p, noise3(x * 0.2f, float(time) * 1.3f, z * 0.2f));
			}
			uint16_t *o = out + (int64_t(iy) * nx + ix) * 4;
			o[0] = Math::make_half_float(w.x);
			o[1] = Math::make_half_float(w.y);
			o[2] = Math::make_half_float(w.z);
			o[3] = Math::make_half_float(turb);
		}
	}
}

void WindField3D::_bake(const Vector3 &p_center) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	terrain_object = terrain.is_empty() ? nullptr : get_node_or_null(terrain);
	const Vector3 cell = extent / Vector3(resolution);
	const Vector3 c = get_global_transform().affine_inverse().xform(p_center);
	origin = ((c - extent * 0.5f) / cell).floor() * cell;
	if (texture.is_null() || slices.size() != resolution.z) {
		slices.clear();
		for (int z = 0; z < resolution.z; z++) {
			slices.push_back(Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_RGBAH));
		}
		texture.instantiate();
		texture->create(Image::FORMAT_RGBAH, resolution.x, resolution.y, resolution.z, false, slices);
	}
	ground.resize(resolution.x * resolution.z);
	for (int iz = 0; iz < resolution.z; iz++) {
		for (int ix = 0; ix < resolution.x; ix++) {
			ground[iz * resolution.x + ix] = _ground_at(origin.x + (ix + 0.5f) * cell.x, origin.z + (iz + 0.5f) * cell.z);
		}
	}
	WindZone3D::snapshot_all(snaps);
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &WindField3D::_bake_slice, (void *)nullptr, resolution.z, -1, true, "WindField3D bake");
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	texture->update(slices);
	if (publish) {
		RenderingServer *rs = RS::get_singleton();
		const Transform3D xf = get_global_transform();
		rs->global_shader_parameter_set("wind_field", texture->get_rid());
		rs->global_shader_parameter_set("wind_field_origin", xf.xform(origin));
		rs->global_shader_parameter_set("wind_field_size", extent);
		const Vector3 w10 = get_wind_at_height(10.0f);
		rs->global_shader_parameter_set("wind_global", Vector4(w10.x, w10.z, gust_strength, float(time)));
		// The shader TIME of the frame about to be drawn: the renderer's clock as the last frame left it, and this
		// frame's step.
		double shader_time = RSG::rasterizer != nullptr ? RSG::rasterizer->get_total_time() + last_step : 0.0;
		const double roll = GLOBAL_GET("rendering/limits/time/time_rollover_secs");
		if (roll > 0.0) {
			shader_time = Math::fmod(shader_time, roll);
		}
		rs->global_shader_parameter_set("wind_advect", Vector4(float(advect_x), float(advect_z), float(shader_time), gust_scale));
	}
	last_bake_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

void WindField3D::bake_now(const Vector3 &p_center) {
	_bake(p_center);
}

void WindField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			register_globals();
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			last_step = get_process_delta_time();
			time += last_step;
			{
				const float ang = Math::deg_to_rad(heading);
				const double go = MAX(speed, 0.5f) * last_step;
				// Kept under 2^20 m, where a float still places the pattern to a few centimetres.
				advect_x = Math::fmod(advect_x + Math::cos(ang) * go, 1048576.0);
				advect_z = Math::fmod(advect_z + Math::sin(ang) * go, 1048576.0);
			}
			Vector3 p = focus_position;
			if (follow_camera) {
				const Camera3D *cam = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
				if (cam != nullptr) {
					p = cam->get_global_position();
				}
			}
			_bake(p);
		} break;
	}
}

void WindField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_wind", "position"), &WindField3D::get_wind);
	ClassDB::bind_method(D_METHOD("get_wind_at_height", "height"), &WindField3D::get_wind_at_height);
	ClassDB::bind_method(D_METHOD("get_texture"), &WindField3D::get_texture);
	ClassDB::bind_method(D_METHOD("get_field_origin"), &WindField3D::get_field_origin);
	ClassDB::bind_method(D_METHOD("get_time"), &WindField3D::get_time);
	ClassDB::bind_method(D_METHOD("get_advect"), &WindField3D::get_advect);
	ClassDB::bind_method(D_METHOD("get_gust", "position"), &WindField3D::get_gust);
	ClassDB::bind_method(D_METHOD("bake_now", "center"), &WindField3D::bake_now);
	ClassDB::bind_method(D_METHOD("get_last_bake_ms"), &WindField3D::get_last_bake_ms);
	ClassDB::bind_method(D_METHOD("set_heading", "degrees"), &WindField3D::set_heading);
	ClassDB::bind_method(D_METHOD("get_heading"), &WindField3D::get_heading);
	ClassDB::bind_method(D_METHOD("set_speed", "speed"), &WindField3D::set_speed);
	ClassDB::bind_method(D_METHOD("get_speed"), &WindField3D::get_speed);
	ClassDB::bind_method(D_METHOD("set_shear_exponent", "exponent"), &WindField3D::set_shear_exponent);
	ClassDB::bind_method(D_METHOD("get_shear_exponent"), &WindField3D::get_shear_exponent);
	ClassDB::bind_method(D_METHOD("set_veer", "degrees"), &WindField3D::set_veer);
	ClassDB::bind_method(D_METHOD("get_veer"), &WindField3D::get_veer);
	ClassDB::bind_method(D_METHOD("set_gust_strength", "strength"), &WindField3D::set_gust_strength);
	ClassDB::bind_method(D_METHOD("get_gust_strength"), &WindField3D::get_gust_strength);
	ClassDB::bind_method(D_METHOD("set_gust_scale", "metres"), &WindField3D::set_gust_scale);
	ClassDB::bind_method(D_METHOD("get_gust_scale"), &WindField3D::get_gust_scale);
	ClassDB::bind_method(D_METHOD("set_turbulence", "turbulence"), &WindField3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &WindField3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_turbulence_scale", "metres"), &WindField3D::set_turbulence_scale);
	ClassDB::bind_method(D_METHOD("get_turbulence_scale"), &WindField3D::get_turbulence_scale);
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &WindField3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &WindField3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &WindField3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &WindField3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_terrain", "terrain"), &WindField3D::set_terrain);
	ClassDB::bind_method(D_METHOD("get_terrain"), &WindField3D::get_terrain);
	ClassDB::bind_method(D_METHOD("set_follow_camera", "follow"), &WindField3D::set_follow_camera);
	ClassDB::bind_method(D_METHOD("get_follow_camera"), &WindField3D::get_follow_camera);
	ClassDB::bind_method(D_METHOD("set_focus_position", "position"), &WindField3D::set_focus_position);
	ClassDB::bind_method(D_METHOD("get_focus_position"), &WindField3D::get_focus_position);
	ClassDB::bind_method(D_METHOD("set_publish", "publish"), &WindField3D::set_publish);
	ClassDB::bind_method(D_METHOD("get_publish"), &WindField3D::get_publish);

	ADD_GROUP("Global Wind", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heading", PROPERTY_HINT_RANGE, "-360,360,0.1,suffix:deg"), "set_heading", "get_heading");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:m/s"), "set_speed", "get_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shear_exponent", PROPERTY_HINT_RANGE, "0,0.6,0.01"), "set_shear_exponent", "get_shear_exponent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "veer", PROPERTY_HINT_RANGE, "-90,90,0.1,suffix:deg/km"), "set_veer", "get_veer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust_strength", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_gust_strength", "get_gust_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust_scale", PROPERTY_HINT_RANGE, "1,1000,1,suffix:m"), "set_gust_scale", "get_gust_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence_scale", PROPERTY_HINT_RANGE, "0.5,100,0.1,suffix:m"), "set_turbulence_scale", "get_turbulence_scale");
	ADD_GROUP("Field", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "extent", PROPERTY_HINT_NONE, "suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "resolution"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain"), "set_terrain", "get_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_camera"), "set_follow_camera", "get_follow_camera");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "focus_position", PROPERTY_HINT_NONE, "suffix:m"), "set_focus_position", "get_focus_position");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish"), "set_publish", "get_publish");
}

WindField3D::WindField3D() {
}
