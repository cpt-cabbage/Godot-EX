/**************************************************************************/
/*  jolt_height_map_shape_3d.h                                            */
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

#include "jolt_shape_3d.h"
#include "../jolt_project_settings.h"

class JoltHeightMapShape3D final : public JoltShape3D {
	AABB aabb;

#ifdef REAL_T_IS_DOUBLE
	PackedFloat64Array heights;
#else
	PackedFloat32Array heights;
#endif

	int width = 0;
	int depth = 0;

	// Godot-EX: a friction per cell (height_map_shape_set_cell_frictions): each of the (width - 1) x (depth - 1)
	// cells' index (row-major, z then x, as the heights) into the frictions; empty, the body's friction everywhere.
	// The height field only (a non-square map builds as a mesh and keeps the body's).
	PackedByteArray cell_materials;
	PackedFloat32Array material_frictions;

	virtual JPH::ShapeRefC _build() const override;
	JPH::ShapeRefC _build_height_field() const;
	JPH::ShapeRefC _build_mesh() const;

	AABB _calculate_aabb() const;

public:
	virtual ShapeType get_type() const override { return ShapeType::SHAPE_HEIGHTMAP; }
	virtual bool is_convex() const override { return false; }
	// Godot-EX: the project setting (physics/jolt_physics_3d/collisions/height_map_back_face_collision; the training ground
	// is one-sided, env_server.gd refuses it on) where upstream has true; before upstream 62cd25492d it was the
	// double-sided decorator with the same flag.
	virtual bool has_back_face_collision() const override { return JoltProjectSettings::height_map_back_face_collision; }

	virtual Variant get_data() const override;
	virtual void set_data(const Variant &p_data) override;

	void set_cell_frictions(const PackedByteArray &p_cells, const PackedFloat32Array &p_frictions);
	PackedByteArray get_cell_materials() const { return cell_materials; }
	PackedFloat32Array get_material_frictions() const { return material_frictions; }

	virtual float get_margin() const override { return 0.0f; }
	virtual void set_margin(float p_margin) override {}

	virtual AABB get_aabb() const override { return aabb; }

	String to_string() const;
};
