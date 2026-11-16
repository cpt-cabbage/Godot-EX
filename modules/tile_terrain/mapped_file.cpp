/**************************************************************************/
/*  mapped_file.cpp                                                       */
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

#include "mapped_file.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"

#ifdef WINDOWS_ENABLED
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static Mutex mapped_files_mutex;
static HashMap<String, MappedFile *> mapped_files;

Error MappedFile::open(const String &p_path) {
	close();
	const String global = ProjectSettings::get_singleton()->globalize_path(p_path);
	// Not in the editor: a mapped file cannot be written over (Windows refuses to truncate one), and the editor stays
	// open with the scene in it while a project's tools write its data again. There the file is read whole, as it was.
	const bool map_it = !Engine::get_singleton()->is_editor_hint();
#ifdef WINDOWS_ENABLED
	HANDLE fh = map_it ? CreateFileW((LPCWSTR)global.utf16().get_data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr) : INVALID_HANDLE_VALUE;
	if (fh != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER length;
		if (GetFileSizeEx(fh, &length) && length.QuadPart > 0) {
			HANDLE mh = CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (mh != nullptr) {
				void *view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
				if (view != nullptr) {
					data = (const uint8_t *)view;
					size = uint64_t(length.QuadPart);
					file_handle = fh;
					map_handle = mh;
					mapped = true;
					path = p_path;
					return OK;
				}
				CloseHandle(mh);
			}
		}
		CloseHandle(fh);
	}
#else
	const int f = map_it ? ::open(global.utf8().get_data(), O_RDONLY) : -1;
	if (f >= 0) {
		struct stat st;
		if (fstat(f, &st) == 0 && st.st_size > 0) {
			void *view = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, f, 0);
			if (view != MAP_FAILED) {
				data = (const uint8_t *)view;
				size = uint64_t(st.st_size);
				fd = f;
				mapped = true;
				path = p_path;
				return OK;
			}
		}
		::close(f);
	}
#endif
	// Not a file of the file system's (a pack's), or it would not map: whole, as it was.
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}
	whole.resize(uint32_t(f->get_length()));
	f->get_buffer(whole.ptr(), whole.size());
	data = whole.ptr();
	size = whole.size();
	path = p_path;
	return OK;
}

void MappedFile::close() {
	if (mapped) {
#ifdef WINDOWS_ENABLED
		UnmapViewOfFile((LPCVOID)data);
		CloseHandle((HANDLE)map_handle);
		CloseHandle((HANDLE)file_handle);
		map_handle = nullptr;
		file_handle = nullptr;
#else
		munmap((void *)data, size_t(size));
		::close(fd);
		fd = -1;
#endif
	}
	whole.reset();
	data = nullptr;
	size = 0;
	mapped = false;
}

Ref<MappedFile> MappedFile::open_shared(const String &p_path) {
	MutexLock lock(mapped_files_mutex);
	MappedFile **held = mapped_files.getptr(p_path);
	if (held != nullptr) {
		// (it leaves the table in its destructor, under this mutex: one found here still has a holder, unless its last
		// reference is going at this moment, which init_ref below tells)
		Ref<MappedFile> ref;
		ref = Ref<MappedFile>(*held);
		if (ref.is_valid()) {
			return ref;
		}
	}
	Ref<MappedFile> file;
	file.instantiate();
	if (file->open(p_path) != OK) {
		return Ref<MappedFile>();
	}
	mapped_files[p_path] = file.ptr();
	return file;
}

MappedFile::~MappedFile() {
	{
		MutexLock lock(mapped_files_mutex);
		MappedFile **held = mapped_files.getptr(path);
		if (held != nullptr && *held == this) {
			mapped_files.erase(path);
		}
	}
	close();
}
