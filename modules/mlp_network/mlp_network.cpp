/**************************************************************************/
/*  mlp_network.cpp                                                       */
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

#include "mlp_network.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"

void MLPNetwork::add_layer(const PackedFloat32Array &p_weights, const PackedFloat32Array &p_bias, int p_inputs, Activation p_activation) {
	ERR_FAIL_COND_MSG(p_inputs <= 0, "A layer needs at least one input.");
	ERR_FAIL_COND_MSG(p_bias.is_empty(), "A layer needs at least one output.");
	ERR_FAIL_COND_MSG(p_weights.size() != p_inputs * p_bias.size(), vformat("Expected %d weights (%d outputs x %d inputs), got %d.", p_inputs * p_bias.size(), p_bias.size(), p_inputs, p_weights.size()));
	ERR_FAIL_COND_MSG(!layers.is_empty() && layers[layers.size() - 1].outputs != p_inputs, vformat("The previous layer has %d outputs, this one %d inputs.", layers[layers.size() - 1].outputs, p_inputs));
	Layer layer;
	layer.inputs = p_inputs;
	layer.outputs = p_bias.size();
	layer.weights = p_weights;
	layer.bias = p_bias;
	layer.activation = p_activation;
	layers.push_back(layer);
}

void MLPNetwork::set_input_normalization(const PackedFloat32Array &p_mean, const PackedFloat32Array &p_inv_std, float p_clip) {
	ERR_FAIL_COND_MSG(p_mean.size() != p_inv_std.size(), "The mean and inverse standard deviation differ in size.");
	input_mean = p_mean;
	input_inv_std = p_inv_std;
	input_clip = p_clip;
}

void MLPNetwork::clear() {
	layers.clear();
	input_mean.clear();
	input_inv_std.clear();
	input_clip = 0.0f;
}

int MLPNetwork::get_input_size() const {
	return layers.is_empty() ? 0 : layers[0].inputs;
}

int MLPNetwork::get_output_size() const {
	return layers.is_empty() ? 0 : layers[layers.size() - 1].outputs;
}

PackedFloat32Array MLPNetwork::forward(const PackedFloat32Array &p_input) {
	ERR_FAIL_COND_V_MSG(layers.is_empty(), PackedFloat32Array(), "The network has no layers.");
	ERR_FAIL_COND_V_MSG(p_input.size() != layers[0].inputs, PackedFloat32Array(), vformat("Expected %d inputs, got %d.", layers[0].inputs, p_input.size()));

	buffer_a = p_input;
	if (!input_mean.is_empty()) {
		ERR_FAIL_COND_V_MSG(input_mean.size() != p_input.size(), PackedFloat32Array(), "The input normalisation does not match the input size.");
		float *x = buffer_a.ptrw();
		const float *mean = input_mean.ptr();
		const float *inv_std = input_inv_std.ptr();
		for (int i = 0; i < buffer_a.size(); i++) {
			x[i] = (x[i] - mean[i]) * inv_std[i];
			if (input_clip > 0.0f) {
				x[i] = CLAMP(x[i], -input_clip, input_clip);
			}
		}
	}

	for (const Layer &layer : layers) {
		buffer_b.resize(layer.outputs);
		const float *in = buffer_a.ptr();
		const float *w = layer.weights.ptr();
		const float *b = layer.bias.ptr();
		float *out = buffer_b.ptrw();
		for (int o = 0; o < layer.outputs; o++) {
			const float *row = w + (int64_t)o * layer.inputs;
			float sum = b[o];
			for (int i = 0; i < layer.inputs; i++) {
				sum += row[i] * in[i];
			}
			switch (layer.activation) {
				case ACTIVATION_RELU:
					sum = MAX(sum, 0.0f);
					break;
				case ACTIVATION_ELU:
					sum = sum > 0.0f ? sum : Math::exp(sum) - 1.0f;
					break;
				case ACTIVATION_TANH:
					sum = Math::tanh(sum);
					break;
				case ACTIVATION_NONE:
					break;
			}
			out[o] = sum;
		}
		SWAP(buffer_a, buffer_b);
	}

	PackedFloat32Array result;
	result.resize(buffer_a.size());
	memcpy(result.ptrw(), buffer_a.ptr(), buffer_a.size() * sizeof(float));
	return result;
}

void MLPNetwork::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_layer", "weights", "bias", "inputs", "activation"), &MLPNetwork::add_layer);
	ClassDB::bind_method(D_METHOD("set_input_normalization", "mean", "inv_std", "clip"), &MLPNetwork::set_input_normalization);
	ClassDB::bind_method(D_METHOD("clear"), &MLPNetwork::clear);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &MLPNetwork::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_input_size"), &MLPNetwork::get_input_size);
	ClassDB::bind_method(D_METHOD("get_output_size"), &MLPNetwork::get_output_size);
	ClassDB::bind_method(D_METHOD("forward", "input"), &MLPNetwork::forward);

	BIND_ENUM_CONSTANT(ACTIVATION_NONE);
	BIND_ENUM_CONSTANT(ACTIVATION_RELU);
	BIND_ENUM_CONSTANT(ACTIVATION_ELU);
	BIND_ENUM_CONSTANT(ACTIVATION_TANH);
}
