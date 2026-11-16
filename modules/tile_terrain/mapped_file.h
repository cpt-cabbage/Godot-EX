/**************************************************************************/
/*  mapped_file.h                                                         */
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

// A file's bytes as memory the operating system pages in where they are read and may drop again (Godot-EX; ProjectEX's
// valley, 2026-11-16): the terrain's heights (0.5 GB) and the ground cover's weight maps (66 MB each) were read whole
// into the process and held for the run, though a frame reads a few megabytes of them round the viewer. Mapped, only
// the pages touched are in memory, they are the file cache's and not the process's own, and several users of one file
// share them (MappedFile::open_shared). Where a file cannot be mapped (inside a pack, or the call fails) it is read
// whole as before, and in the editor always: a mapped file cannot be written over while it is held, and the editor
// stays open while a project's tools write its data again. A running game does hold them: close it before an import.

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

class MappedFile : public RefCounted {
	GDSOFTCLASS(MappedFile, RefCounted);

	const uint8_t *data = nullptr;
	uint64_t size = 0;
	bool mapped = false;
	String path;
#ifdef WINDOWS_ENABLED
	void *file_handle = nullptr;
	void *map_handle = nullptr;
#else
	int fd = -1;
#endif
	LocalVector<uint8_t> whole; // the fallback: the file read whole

public:
	Error open(const String &p_path);
	void close();
	const uint8_t *ptr() const { return data; }
	uint64_t get_size() const { return size; }
	bool is_mapped() const { return mapped; }

	// The one mapping of p_path, shared by everyone who holds it; null if the file cannot be read.
	static Ref<MappedFile> open_shared(const String &p_path);

	~MappedFile();
};
