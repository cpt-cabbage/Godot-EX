/**************************************************************************/
/*  jolt_friction_material.cpp                                            */
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

#include "jolt_friction_material.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

JPH_IMPLEMENT_RTTI_VIRTUAL(JoltFrictionMaterial) {
	JPH_ADD_BASE_CLASS(JoltFrictionMaterial, JPH::PhysicsMaterial)
}

float JoltFrictionMaterial::friction_of(const JPH::Body &p_body, const JPH::SubShapeID &p_sub_shape_id) {
	if (p_body.IsStatic()) {
		const JPH::PhysicsMaterial *material = p_body.GetShape()->GetMaterial(p_sub_shape_id);
		if (material != nullptr && material->GetRTTI() == JPH_RTTI(JoltFrictionMaterial)) {
			return static_cast<const JoltFrictionMaterial *>(material)->friction;
		}
	}
	return p_body.GetFriction();
}
