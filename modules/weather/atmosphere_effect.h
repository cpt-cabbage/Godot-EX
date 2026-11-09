/**************************************************************************/
/*  atmosphere_effect.h                                                   */
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

// The atmosphere (Godot-EX, 2026-11-09; atmosphere.glsl, Hillaire 2020): a compositor effect that, each frame after the
// sky, computes the lookup tables (transmittance and multiple scattering when the air's parameters change; the sky view
// and the aerial-perspective volume every frame, from the camera) and lays the aerial perspective over the scene by its
// depth. The sky shader reads get_sky_view_texture() and get_transmittance_texture() (a frame late: the sky draws first)
// with sky_view_uv's mapping. The light (`light_direction`, `light_illuminance`: the sun's, or the moon's at night) is
// TimeOfDay3D's to set. Physical defaults (Earth's, per km); the art's knobs scale them: `rayleigh_density` (the blue),
// `mie_density` (haze; the weather's mist raises it), `mie_anisotropy`, `ground_albedo`, `aerial_perspective_scale` (how
// far the air seems, a multiplier on distance), `multiscatter`. Altitudes: world y plus `altitude_offset` m.
// transmittance() gives the same air's transmittance on the CPU, for the sun's and the moon's colour.
// The art's colour (2026-11-09): `mie_tint` colours what the haze scatters (dust or smoke glows orange at sunset), the
// rest of it absorbed; `absorption_color` at `absorption_density` (per km at the ground, thinning as the haze) is other
// matter that absorbs (smoke, dust), taking those colours out of the sunlight and the sky; `sky_tint` and `sky_luminance`
// scale the sky's and the aerial perspective's light (not the discs) and need no new tables, so SkyLook (sky_look.h)
// drives them by the sun's height every frame.
//
// The terrain's shadow in the air (2026-11-09): set_terrain_heights() gives the terrain's height texture; a map turned
// to the light (SHADOW_N a side over the terrain's diagonal) holds the height below which the ground hides the light,
// rebuilt when the light moves. The aerial volume (AERIAL_W x AERIAL_H x AERIAL_Z, marched once down each column)
// shadows its single scattering by it, so the mountains' shadows darken the haze and the light between them falls in
// shafts; the sky's pixels lose the shadowed share of the light the volume holds (crepuscular rays). `air_shadows`
// turns it off; `shadow_softness` (m) is the shadow edge's width in height.
//
// The air volume (air_volume.glsl): froxels (FOG_W x FOG_H x FOG_Z, finer near) out to `fog_distance` holding ground
// haze (`ground_haze_density` per m at the ground, thinning over `ground_haze_height`) and valley mist (`mist_density`
// below the world height `mist_top`, thinning over `mist_falloff` above it), broken by wind-carried noise; lit by
// `light_color` (TimeOfDay3D sets it: the light's colour and energy) through the terrain's shadow map and, within
// `fog_rt_distance`, ray-traced visibility with alpha-tested leaves (shafts through the trees), and by the sky (its
// table's mean, times `fog_sky_light`). Each frame's point in a froxel is jittered and blended into the reprojected
// last frame's (`fog_history`). Off with `fog_enabled`.
//
// The clouds (clouds.glsl, atmosphere_clouds.cpp): a layer `cloud_base` m above sea level (it may lie below the peaks),
// as thick as its `cloud_type` makes it (layered, heaped, towering; `cloud_thickness_scale`), `cloud_cover` of the sky,
// carried by `cloud_wind`; shaped by noise at `cloud_scale` and `cloud_detail_scale` m, their cover by a weather map at
// `cloud_weather_scale`. Marched at `cloud_resolution` of the screen out to `cloud_reach` m, blended with the last
// frame (`cloud_history`); laid over the scene behind the haze in front of them. Their shadow map (`cloud_shadows`, a
// `cloud_shadow_size` m square round the camera) shades the haze and the air volume; get_cloud_panorama_texture() is the
// clouds over the whole sky for the sky shader's light probe. WeatherDirector sets the cover, base, type and wind. `sky_recolor` (0 to 1) replaces that light's own colour by its luminance
// times the tint, so a tint can make a blue sky orange rather than brown; `sky_tint_height` (radians of the view's
// elevation) is how far up from the horizon the tint and recolour reach, fading as exp(-elevation / height): 10 is the
// whole sky, 0.3 a band low down with the zenith left as it was.

#include "scene/resources/compositor.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/renderer_rd/shader_rd.h"

#include "air_volume.glsl.gen.h"
#include "clouds.glsl.gen.h"
#include "atmosphere.glsl.gen.h"

class AtmosphereEffect : public CompositorEffect {
	GDCLASS(AtmosphereEffect, CompositorEffect);

	// The air, per km (Hillaire 2020's Earth).
	Vector3 rayleigh_scattering = Vector3(5.802e-3, 13.558e-3, 33.1e-3);
	float rayleigh_scale_height = 8.0;
	float mie_scattering = 3.996e-3;
	float mie_extinction = 4.44e-3;
	float mie_scale_height = 1.2;
	Vector3 ozone_absorption = Vector3(0.650e-3, 1.881e-3, 0.085e-3);
	float planet_radius = 6360.0;
	float atmosphere_height = 100.0;

	// The art's knobs.
	float rayleigh_density = 1.0;
	float mie_density = 1.0;
	float mie_anisotropy = 0.8;
	float ground_albedo = 0.3;
	float multiscatter = 1.0;
	float aerial_perspective_scale = 1.0;
	float aerial_distance = 32.0; // km the volume reaches
	float altitude_offset = 1400.0;
	bool apply_aerial = true;
	Color mie_tint = Color(1, 1, 1);
	Color absorption_color = Color(0.25, 0.55, 1.0);
	float absorption_density = 0.0;
	Color sky_tint = Color(1, 1, 1);
	float sky_luminance = 1.0;
	float sky_recolor = 0.0;
	float sky_tint_height = 10.0;

	Vector3 light_direction = Vector3(0, 1, 0);
	float light_illuminance = 1.0;

	// GPU.
	// The terrain's shadow in the air.
	Ref<Texture2D> terrain_heights;
	Rect2 terrain_rect;
	float terrain_height_min = 0.0;
	float terrain_height_range = 1.0;
	bool air_shadows = true;
	float shadow_softness = 20.0;
	Vector3 shadow_light; // the light the map was made for
	bool shadow_dirty = true;

	// The air volume.
	bool fog_enabled = true;
	float ground_haze_density = 0.0003;
	float ground_haze_height = 40.0;
	float mist_density = 0.0;
	float mist_top = 0.0;
	float mist_falloff = 60.0;
	float fog_noise = 0.5;
	float fog_noise_scale = 30.0;
	Vector3 fog_wind;
	Color fog_albedo = Color(0.95, 0.95, 0.95);
	float fog_anisotropy = 0.6;
	float fog_sky_light = 1.0;
	float fog_distance = 800.0;
	float fog_rt_distance = 400.0;
	float fog_history = 0.9;
	Color light_color = Color(1, 1, 1);
	AirVolumeShaderRD fog_shader;
	RID fog_version;
	RID fog_pipelines[3];
	RID fog_media[2];
	RID fog_light;
	RID fog_ubo;
	int fog_frame = 0;
	bool fog_rt_supported = false;
	// The clouds.
	bool clouds_enabled = true;
	float cloud_cover = 0.3;
	float cloud_base = 2600.0;
	float cloud_type = 0.5;
	float cloud_thickness_scale = 1.0;
	float cloud_density = 0.04;
	float cloud_scale = 3000.0;
	float cloud_detail_scale = 280.0;
	float cloud_weather_scale = 14000.0;
	float cloud_erosion = 0.35;
	Vector3 cloud_wind = Vector3(6, 0, 3);
	float cloud_anisotropy = 0.8;
	float cloud_sky_light = 1.0;
	float cloud_multiscatter = 1.0;
	int cloud_steps = 64;
	float cloud_light_step = 40.0;
	float cloud_reach = 100000.0; // past 40 km a low layer's end showed as a band of clear sky at the horizon
	float cloud_resolution = 0.5;
	float cloud_history = 0.85;
	bool cloud_shadows = true;
	float cloud_shadow_size = 40000.0;
	CloudsShaderRD cloud_shader;
	RID cloud_version;
	RID cloud_pipelines[5];
	RID sampler_repeat;
	RID noise_base_rd, noise_detail_rd;
	RID clouds_rd[2], cloud_depth_rd[2];
	RID cloud_current, cloud_current_depth;
	RID cloud_panorama_rd, cloud_shadow_rd;
	Ref<Texture2DRD> cloud_panorama_texture;
	RID cloud_ubo;
	bool cloud_noise_ready = false;
	int cloud_noise_slice = -1; // the next base noise slice to make; -1 the detail volume first
	Size2i cloud_size;
	int cloud_frame = 0;
	uint64_t cloud_last_ticks = 0;
	Vector3 cloud_offset;
	Vector2 cloud_shadow_origin;
	Projection cloud_view_proj, cloud_prev_view_proj;
	Vector3 cloud_cam, cloud_prev_cam;
	bool cloud_prev_valid = false;
	void _clouds_ensure(const Size2i &p_full);
	void _clouds_free();
	bool _clouds_prepare(const Transform3D &p_cam, const Projection &p_inv_rot, const Projection &p_view_proj, const Size2i &p_full);
	void _clouds_dispatch(int64_t p_list, RID p_depth, bool p_march);

	Projection prev_view_proj;
	Vector3 prev_cam;
	bool prev_valid = false;

	bool initialized = false;
	bool tables_dirty = true;
	AtmosphereShaderRD shader;
	RID shader_version;
	RID pipelines[6];
	RID sampler;
	RID sampler_mip;
	RID shadow_rd, sky_shadow_rd;
	RID transmittance_rd, multiscatter_rd, sky_view_rd, aerial_rd;
	RID camera_ubo;
	Ref<Texture2DRD> transmittance_texture;
	Ref<Texture2DRD> sky_view_texture;
	float camera_altitude = 0.0; // m, the last frame's

	struct Push {
		float rayleigh[4];
		float mie_scattering[4];
		float mie_extinction[4];
		float ozone[4];
		float radii[4];
		float light[4];
		float misc[4];
		float tint[4];
	};
	Vector3 _mie_scattering_rgb() const;
	Vector3 _extinction_rgb() const; // the haze's and the other absorption's, per km at the ground
	Push _push(float p_camera_altitude_m, int p_width, int p_height) const;
	void _ensure();
	void _free();
	void _dirty() { tables_dirty = true; }

protected:
	virtual void _render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) override;
	static void _bind_methods();

public:
	static constexpr int TRANSMITTANCE_W = 256, TRANSMITTANCE_H = 64;
	static constexpr int MULTISCATTER_N = 32;
	static constexpr int SKY_VIEW_W = 192, SKY_VIEW_H = 108;
	static constexpr int AERIAL_W = 160, AERIAL_H = 90, AERIAL_Z = 64;
	static constexpr int SHADOW_N = 2048;
	static constexpr int CAMERA_FLOATS = 48; // atmosphere.glsl's Camera block
	static constexpr int CLOUD_FLOATS = 32 + 4 * 12; // clouds.glsl's Clouds block
	static constexpr int CLOUD_NOISE_SLICES = 8; // base noise slices made a frame
	static constexpr int CLOUD_NOISE_BASE_N = 128, CLOUD_NOISE_DETAIL_N = 32;
	static constexpr int CLOUD_PANORAMA_W = 256, CLOUD_PANORAMA_H = 128;
	static constexpr int CLOUD_SHADOW_N = 512;
	static constexpr int FOG_W = 160, FOG_H = 90, FOG_Z = 64;
	static constexpr int FOG_FLOATS = 32 + 4 * 17; // air_volume.glsl's Fog block

	Ref<Texture2D> get_transmittance_texture();
	Ref<Texture2D> get_sky_view_texture();
	// The air's transmittance from an altitude (m, world y plus the offset) toward a direction, on the CPU.
	Vector3 transmittance(float p_altitude_m, const Vector3 &p_direction) const;
	float get_camera_altitude() const { return camera_altitude; }

#define PROP(type, m)                                          \
	void set_##m(type p_v) { \
		if (m != p_v) { \
			m = p_v; \
			_dirty(); \
		} \
	}                                                            \
	type get_##m() const { return m; }
	PROP(Vector3, rayleigh_scattering)
	PROP(float, rayleigh_scale_height)
	PROP(float, mie_scattering)
	PROP(float, mie_extinction)
	PROP(float, mie_scale_height)
	PROP(Vector3, ozone_absorption)
	PROP(float, planet_radius)
	PROP(float, atmosphere_height)
	PROP(float, rayleigh_density)
	PROP(float, mie_density)
	PROP(float, mie_anisotropy)
	PROP(float, ground_albedo)
	PROP(float, multiscatter)
	PROP(Color, mie_tint)
	PROP(Color, absorption_color)
	PROP(float, absorption_density)
#undef PROP
	void set_sky_tint(const Color &p_c) { sky_tint = p_c; }
	Color get_sky_tint() const { return sky_tint; }
	void set_sky_luminance(float p_v) { sky_luminance = MAX(p_v, 0.0f); }
	float get_sky_luminance() const { return sky_luminance; }
	void set_sky_recolor(float p_v) { sky_recolor = CLAMP(p_v, 0.0f, 1.0f); }
	float get_sky_recolor() const { return sky_recolor; }
	void set_sky_tint_height(float p_v) { sky_tint_height = MAX(p_v, 0.01f); }
	float get_sky_tint_height() const { return sky_tint_height; }
	void set_aerial_perspective_scale(float p_v) { aerial_perspective_scale = MAX(p_v, 0.0f); }
	float get_aerial_perspective_scale() const { return aerial_perspective_scale; }
	void set_aerial_distance(float p_v) { aerial_distance = MAX(p_v, 1.0f); }
	float get_aerial_distance() const { return aerial_distance; }
	void set_altitude_offset(float p_v) { altitude_offset = p_v; }
	float get_altitude_offset() const { return altitude_offset; }
	// The terrain's heights (a texture of height_min + height_range * value over `rect`, world x and z) for its shadow in
	// the air; a null texture turns it off.
	void set_terrain_heights(const Ref<Texture2D> &p_heights, const Rect2 &p_rect, float p_height_min, float p_height_range);
	void set_air_shadows(bool p_v) { air_shadows = p_v; }
	bool get_air_shadows() const { return air_shadows; }
	void set_shadow_softness(float p_v) { shadow_softness = MAX(p_v, 0.01f); }
	float get_shadow_softness() const { return shadow_softness; }
#define FOGP(type, m)                        \
	void set_##m(type p_v) { m = p_v; }         \
	type get_##m() const { return m; }
	FOGP(bool, fog_enabled)
	FOGP(float, ground_haze_density)
	FOGP(float, ground_haze_height)
	FOGP(float, mist_density)
	FOGP(float, mist_top)
	FOGP(float, mist_falloff)
	FOGP(float, fog_noise)
	FOGP(float, fog_noise_scale)
	FOGP(Vector3, fog_wind)
	FOGP(Color, fog_albedo)
	FOGP(float, fog_anisotropy)
	FOGP(float, fog_sky_light)
	FOGP(float, fog_distance)
	FOGP(float, fog_rt_distance)
	FOGP(float, fog_history)
	FOGP(Color, light_color)
	FOGP(bool, clouds_enabled)
	FOGP(float, cloud_cover)
	FOGP(float, cloud_base)
	FOGP(float, cloud_type)
	FOGP(float, cloud_thickness_scale)
	FOGP(float, cloud_density)
	FOGP(float, cloud_scale)
	FOGP(float, cloud_detail_scale)
	FOGP(float, cloud_weather_scale)
	FOGP(float, cloud_erosion)
	FOGP(Vector3, cloud_wind)
	FOGP(float, cloud_anisotropy)
	FOGP(float, cloud_sky_light)
	FOGP(float, cloud_multiscatter)
	FOGP(int, cloud_steps)
	FOGP(float, cloud_light_step)
	FOGP(float, cloud_reach)
	FOGP(float, cloud_history)
	FOGP(bool, cloud_shadows)
	FOGP(float, cloud_shadow_size)
#undef FOGP
	void set_cloud_resolution(float p_v) { cloud_resolution = CLAMP(p_v, 0.125f, 1.0f); }
	float get_cloud_resolution() const { return cloud_resolution; }
	// The layer's thickness (m) its type gives it.
	float get_cloud_thickness() const;
	// The clouds over the whole sky round the camera (equirectangular: rgb their light, a the transmittance).
	Ref<Texture2D> get_cloud_panorama_texture();
	void set_apply_aerial(bool p_v) { apply_aerial = p_v; }
	bool get_apply_aerial() const { return apply_aerial; }
	void set_light_direction(const Vector3 &p_dir) { light_direction = p_dir.normalized(); }
	Vector3 get_light_direction() const { return light_direction; }
	void set_light_illuminance(float p_e) { light_illuminance = MAX(p_e, 0.0f); }
	float get_light_illuminance() const { return light_illuminance; }

	AtmosphereEffect();
	~AtmosphereEffect();
};
