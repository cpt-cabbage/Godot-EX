/**************************************************************************/
/*  knob_log.cpp                                                          */
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

#include "knob_log.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"

#include <atomic>

// A measurement harness sets experiment knobs (GODOT_* environment variables)
// and needs to know that each one was read: a mistyped name, or a knob whose
// code path never ran, otherwise measures as a neutral arm (a sharpness knob
// that never reached a walk was judged that way, rt_lab r10). The harness sets
// GODOT_KNOB_LOG=1 and fails a run whose log lacks a knob it set. Off, a read
// costs a prefix test and an atomic load.
void knob_log_note(const String &p_var) {
	// Whether the log is on is decided at the first GODOT_* read, through the
	// same get_environment that calls this (the deciding thread skips its own
	// nested call; another thread waits for the answer).
	static std::atomic<int> state{ 0 }; // 0 undecided, 1 deciding, 2 off, 3 on.
	static thread_local bool deciding = false;
	if (deciding || !p_var.begins_with("GODOT_")) {
		return;
	}
	int s = state.load(std::memory_order_acquire);
	if (s < 2) {
		int expected = 0;
		if (state.compare_exchange_strong(expected, 1)) {
			deciding = true;
			const bool on = OS::get_singleton()->get_environment("GODOT_KNOB_LOG") == "1";
			deciding = false;
			state.store(on ? 3 : 2, std::memory_order_release);
		}
		while ((s = state.load(std::memory_order_acquire)) == 1) {
		}
	}
	if (s != 3) {
		return;
	}
	// Each name once, by its hash: no container to allocate or to tear down at
	// exit, and some knobs are read every frame.
	static BinaryMutex mutex;
	static uint32_t seen[1024];
	static int seen_count = 0;
	const uint32_t h = p_var.hash();
	MutexLock lock(mutex);
	for (int i = 0; i < seen_count; i++) {
		if (seen[i] == h) {
			return;
		}
	}
	if (seen_count < 1024) {
		seen[seen_count++] = h;
	}
	print_line("KNOB READ " + p_var);
}
