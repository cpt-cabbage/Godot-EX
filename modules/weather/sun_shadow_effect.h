/**************************************************************************/
/*  sun_shadow_effect.h                                                   */
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

// The sun's shadow from the terrain and the clouds for a renderer without ray-traced shadows (Godot-EX, 2026-11-10;
// sun_shadow.glsl): a compositor effect after the depth prepass that writes a screen mask (the render buffers'
// rb_sun_shadow / mask) the scene shader multiplies the first directional light by where there is no ray-traced shadow
// mask, from `atmosphere`'s terrain shadow map and clouds' shadow map (last frame's). With ray-traced sun shadows on, it does nothing: they own the mask (and
// take the clouds' shadow themselves).

#include "atmosphere_effect.h"
#include "scene/resources/compositor.h"

#include "sun_shadow.glsl.gen.h"

class SunShadowEffect : public CompositorEffect {
	GDCLASS(SunShadowEffect, CompositorEffect);

	Ref<AtmosphereEffect> atmosphere;
	SunShadowShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID sampler;
	RID ubo;
	bool initialized = false;

	void _ensure();

protected:
	virtual void _render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) override;
	static void _bind_methods();

public:
	void set_atmosphere(const Ref<AtmosphereEffect> &p_a) { atmosphere = p_a; }
	Ref<AtmosphereEffect> get_atmosphere() const { return atmosphere; }

	SunShadowEffect();
	~SunShadowEffect();
};
