/**************************************************************************/
/*  jolt_friction_material.h                                              */
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

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>

// A part of a shape with its own friction (Godot-EX: a height map's cells, JoltPhysicsServer3D's
// height_map_shape_set_cell_frictions): the space's friction combine takes it in place of the body's.
class JoltFrictionMaterial final : public JPH::PhysicsMaterial {
public:
	JPH_DECLARE_RTTI_VIRTUAL(JPH_NO_EXPORT, JoltFrictionMaterial)

	float friction = 1.0f; // negative: the body's

	JoltFrictionMaterial() = default;
	explicit JoltFrictionMaterial(float p_friction) :
			friction(p_friction) {}

	// The friction where a body is touched: its sub-shape's material's if that is a JoltFrictionMaterial
	// (static bodies only: the terrain's; a dynamic body's shapes have no such materials), else the body's.
	static float friction_of(const JPH::Body &p_body, const JPH::SubShapeID &p_sub_shape_id);
};
