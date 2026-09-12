/**************************************************************************/
/*  metal_fx.cpp                                                          */
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

#ifdef METAL_ENABLED

#include "metal_fx.h"

#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/rendering_device_driver_metal3.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#include <MetalFX/MetalFX.hpp>

using namespace RendererRD;

#pragma mark - Spatial Scaler

MFXSpatialContext::~MFXSpatialContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXSpatialEffect::MFXSpatialEffect() {
}

MFXSpatialEffect::~MFXSpatialEffect() {
}

void MFXSpatialEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::SpatialScalerBase *scaler = p_userdata->scaler;
	scaler->setFence(obj->external_pass_fence());
	scaler->setColorTexture(src_texture);
	scaler->setOutputTexture(dst_texture);
	MTLFX::SpatialScaler *s = static_cast<MTLFX::SpatialScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

void MFXSpatialEffect::ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) {
	p_render_buffers->ensure_mfx(this);
}

void MFXSpatialEffect::process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_src, RID p_dst) {
	MFXSpatialContext *ctx = p_render_buffers->get_mfx_spatial_context();
	DEV_ASSERT(ctx); // this should have been done by the caller via ensure_context

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_dst)),
			*ctx);
	RD::CallbackResource res[2] = {
		{ .rid = p_src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE }
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXSpatialEffect::callback, userdata, VectorView<RD::CallbackResource>(res, 2));
}

MFXSpatialContext *MFXSpatialEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_SPATIAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::SpatialScalerDescriptor> desc = NS::TransferPtr(MTLFX::SpatialScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));
	desc->setColorProcessingMode(MTLFX::SpatialScalerColorProcessingModeLinear);

	MFXSpatialContext *context = memnew(MFXSpatialContext);
	context->scaler = desc->newSpatialScaler(dev);

	return context;
}

#ifdef METAL_MFXTEMPORAL_ENABLED

#pragma mark - Temporal Scaler

MFXTemporalContext::~MFXTemporalContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXTemporalEffect::MFXTemporalEffect() {}
MFXTemporalEffect::~MFXTemporalEffect() {}

MFXTemporalContext *MFXTemporalEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_TEMPORAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setAutoExposureEnabled(false);

	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	MFXTemporalContext *context = memnew(MFXTemporalContext);
	context->scaler = desc->newTemporalScaler(dev);
	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true); // Godot uses reverse Z per https://github.com/godotengine/godot/pull/88328

	return context;
}

void MFXTemporalEffect::process(RendererRD::MFXTemporalContext *p_ctx, RendererRD::MFXTemporalEffect::Params p_params) {
	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.depth)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.motion)),
			p_params.exposure.is_valid() ? RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.exposure)) : RDD::TextureID(),
			p_params.jitter_offset,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.dst)),
			*p_ctx,
			p_params.reset);
	RD::CallbackResource res[5] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.motion, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
		{ .rid = p_params.exposure, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
	};
	uint32_t res_count = p_params.exposure.is_valid() ? 5 : 4;
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXTemporalEffect::callback, userdata, VectorView<RD::CallbackResource>(res, res_count));
}

void MFXTemporalEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(p_userdata->depth.id);
	MTL::Texture *motion = reinterpret_cast<MTL::Texture *>(p_userdata->motion.id);
	MTL::Texture *exposure = reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id);

	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::TemporalScalerBase *scaler = p_userdata->scaler;
	scaler->setFence(obj->external_pass_fence());
	scaler->setReset(p_userdata->reset);
	scaler->setColorTexture(src_texture);
	scaler->setDepthTexture(depth);
	scaler->setMotionTexture(motion);
	scaler->setExposureTexture(exposure);
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setOutputTexture(dst_texture);
	MTLFX::TemporalScaler *s = static_cast<MTLFX::TemporalScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

#pragma mark - Temporal Denoised Scaler

MFXTemporalDenoisedContext::~MFXTemporalDenoisedContext() {
	if (scaler) {
		scaler->release();
	}
}

MFXTemporalDenoisedEffect::MFXTemporalDenoisedEffect() {}
MFXTemporalDenoisedEffect::~MFXTemporalDenoisedEffect() {}

bool MFXTemporalDenoisedEffect::is_supported() {
	if (__builtin_available(macOS 26.0, iOS 26.0, *)) {
		RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
		return MTLFX::TemporalDenoisedScalerDescriptor::supportsDevice(rdd->get_device());
	}
	return false;
}

MFXTemporalDenoisedContext *MFXTemporalDenoisedEffect::create_context(CreateParams p_params) const {
	if (!is_supported()) {
		return nullptr;
	}

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalDenoisedScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalDenoisedScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);
	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setDiffuseAlbedoTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.albedo_format));
	desc->setSpecularAlbedoTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.albedo_format));
	desc->setNormalTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.normal_format));
	desc->setRoughnessTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.roughness_format));
	desc->setSpecularHitDistanceTextureEnabled(true);
	desc->setSpecularHitDistanceTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.hit_distance_format));
	desc->setDenoiseStrengthMaskTextureEnabled(true);
	desc->setDenoiseStrengthMaskTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.strength_mask_format));
	desc->setTransparencyOverlayTextureEnabled(false);
	desc->setReactiveMaskTextureEnabled(false);
	desc->setAutoExposureEnabled(false);
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	MTLFX::TemporalDenoisedScaler *scaler = desc->newTemporalDenoisedScaler(dev);
	if (scaler == nullptr) {
		ERR_PRINT("MetalFX: the temporal denoised scaler could not be created for these formats.");
		return nullptr;
	}
	MFXTemporalDenoisedContext *context = memnew(MFXTemporalDenoisedContext);
	context->scaler = scaler;
	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true); // Godot uses reverse Z per https://github.com/godotengine/godot/pull/88328

	return context;
}

static RDD::TextureID _mfx_texture(RID p_rid) {
	return p_rid.is_valid() ? RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_rid)) : RDD::TextureID();
}

void MFXTemporalDenoisedEffect::process(MFXTemporalDenoisedContext *p_ctx, Params p_params) {
	CallbackArgs *userdata = args_allocator.alloc();
	userdata->owner = this;
	userdata->scaler = p_ctx->scaler;
	userdata->src = _mfx_texture(p_params.src);
	userdata->depth = _mfx_texture(p_params.depth);
	userdata->motion = _mfx_texture(p_params.motion);
	userdata->exposure = _mfx_texture(p_params.exposure);
	userdata->diffuse_albedo = _mfx_texture(p_params.diffuse_albedo);
	userdata->specular_albedo = _mfx_texture(p_params.specular_albedo);
	userdata->normal = _mfx_texture(p_params.normal);
	userdata->roughness = _mfx_texture(p_params.roughness);
	userdata->hit_distance = _mfx_texture(p_params.hit_distance);
	userdata->strength_mask = _mfx_texture(p_params.strength_mask);
	userdata->dst = _mfx_texture(p_params.dst);
	userdata->jitter_offset = p_params.jitter_offset;
	userdata->reset = p_params.reset;
	// Column-major, as simd::float4x4 and Godot's Projection both are.
	Projection world_to_view(p_params.world_to_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			userdata->world_to_view[col * 4 + row] = world_to_view.columns[col][row];
			userdata->view_to_clip[col * 4 + row] = p_params.view_to_clip.columns[col][row];
		}
	}

	RD::CallbackResource res[12] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.motion, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.diffuse_albedo, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.specular_albedo, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.normal, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.roughness, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.hit_distance, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.strength_mask, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
		{ .rid = p_params.exposure, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
	};
	uint32_t res_count = p_params.exposure.is_valid() ? 11 : 10;
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXTemporalDenoisedEffect::callback, userdata, VectorView<RD::CallbackResource>(res, res_count));
}

void MFXTemporalDenoisedEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTLFX::TemporalDenoisedScalerBase *scaler = p_userdata->scaler;
	scaler->setFence(obj->external_pass_fence());
	scaler->setShouldResetHistory(p_userdata->reset);
	scaler->setColorTexture(reinterpret_cast<MTL::Texture *>(p_userdata->src.id));
	scaler->setDepthTexture(reinterpret_cast<MTL::Texture *>(p_userdata->depth.id));
	scaler->setMotionTexture(reinterpret_cast<MTL::Texture *>(p_userdata->motion.id));
	scaler->setExposureTexture(reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id));
	scaler->setDiffuseAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->diffuse_albedo.id));
	scaler->setSpecularAlbedoTexture(reinterpret_cast<MTL::Texture *>(p_userdata->specular_albedo.id));
	scaler->setNormalTexture(reinterpret_cast<MTL::Texture *>(p_userdata->normal.id));
	scaler->setRoughnessTexture(reinterpret_cast<MTL::Texture *>(p_userdata->roughness.id));
	scaler->setSpecularHitDistanceTexture(reinterpret_cast<MTL::Texture *>(p_userdata->hit_distance.id));
	scaler->setDenoiseStrengthMaskTexture(reinterpret_cast<MTL::Texture *>(p_userdata->strength_mask.id));
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	simd::float4x4 w2v, v2c;
	for (int col = 0; col < 4; col++) {
		w2v.columns[col] = simd::float4{ p_userdata->world_to_view[col * 4 + 0], p_userdata->world_to_view[col * 4 + 1], p_userdata->world_to_view[col * 4 + 2], p_userdata->world_to_view[col * 4 + 3] };
		v2c.columns[col] = simd::float4{ p_userdata->view_to_clip[col * 4 + 0], p_userdata->view_to_clip[col * 4 + 1], p_userdata->view_to_clip[col * 4 + 2], p_userdata->view_to_clip[col * 4 + 3] };
	}
	scaler->setWorldToViewMatrix(w2v);
	scaler->setViewToClipMatrix(v2c);
	scaler->setOutputTexture(reinterpret_cast<MTL::Texture *>(p_userdata->dst.id));
	MTLFX::TemporalDenoisedScaler *s = static_cast<MTLFX::TemporalDenoisedScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

#endif

#endif
