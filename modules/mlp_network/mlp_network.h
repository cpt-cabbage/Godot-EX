/**************************************************************************/
/*  mlp_network.h                                                         */
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

#include "core/object/ref_counted.h"
#include "core/variant/type_info.h"

// A small dense network evaluated on the CPU: an optional input normalisation, then fully connected
// layers, each with its activation. Meant for control policies queried at tens of hertz (a few
// hundred thousand multiply-adds), where a scripting language is too slow and a tensor runtime is
// more than needed.
class MLPNetwork : public RefCounted {
	GDCLASS(MLPNetwork, RefCounted);

public:
	enum Activation {
		ACTIVATION_NONE,
		ACTIVATION_RELU,
		ACTIVATION_ELU,
		ACTIVATION_TANH,
	};

private:
	struct Layer {
		int inputs = 0;
		int outputs = 0;
		Vector<float> weights; // Row-major: outputs rows of inputs.
		Vector<float> bias;
		Activation activation = ACTIVATION_NONE;
	};

	Vector<Layer> layers;
	Vector<float> input_mean;
	Vector<float> input_inv_std;
	float input_clip = 0.0f;
	Vector<float> buffer_a;
	Vector<float> buffer_b;

protected:
	static void _bind_methods();

public:
	void add_layer(const PackedFloat32Array &p_weights, const PackedFloat32Array &p_bias, int p_inputs, Activation p_activation);
	void set_input_normalization(const PackedFloat32Array &p_mean, const PackedFloat32Array &p_inv_std, float p_clip);
	void clear();
	int get_layer_count() const { return layers.size(); }
	int get_input_size() const;
	int get_output_size() const;
	PackedFloat32Array forward(const PackedFloat32Array &p_input);
};

VARIANT_ENUM_CAST(MLPNetwork::Activation);
