/**************************************************************************/
/*  system_font_data.cpp                                                   */
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

#include "system_font_data.h"

#include "core/io/file_access.h"
#include "core/os/os.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#elif defined(UNIX_ENABLED)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

void TextServerSystemFontData::open(const String &p_path) {
	ERR_FAIL_COND(mapped != nullptr || !copied.is_empty());
	// Diagnostic A/B switch, read once. Other fonts and packed assets retain
	// their existing FileAccess path. Mapping failures also use that path.
	static const bool use_mapping = OS::get_singleton()->get_environment("GODOT_SYSTEM_FONT_DATA_MODE") != "copy";
	if (use_mapping && p_path.is_absolute_path() && !p_path.contains("://")) {
#ifdef WINDOWS_ENABLED
		HANDLE file = CreateFileW((LPCWSTR)p_path.utf16().get_data(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER length;
			if (GetFileSizeEx(file, &length) && length.QuadPart > 0 && uint64_t(length.QuadPart) <= SIZE_MAX) {
				HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				if (mapping) {
					mapped = static_cast<const uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
					if (mapped) {
						mapped_size = uint64_t(length.QuadPart);
					}
					CloseHandle(mapping);
				}
			}
			CloseHandle(file);
		}
#elif defined(UNIX_ENABLED)
		const int file = ::open(p_path.utf8().get_data(), O_RDONLY | O_CLOEXEC);
		if (file >= 0) {
			struct stat info;
			if (fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && uint64_t(info.st_size) <= SIZE_MAX) {
				void *view = mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
				if (view != MAP_FAILED) {
					mapped = static_cast<const uint8_t *>(view);
					mapped_size = uint64_t(info.st_size);
				}
			}
			close(file);
		}
#endif
	}
	if (!mapped) {
		copied = FileAccess::get_file_as_bytes(p_path);
	}
}

TextServerSystemFontData::~TextServerSystemFontData() {
	if (!mapped) {
		return;
	}
#ifdef WINDOWS_ENABLED
	UnmapViewOfFile(mapped);
#elif defined(UNIX_ENABLED)
	munmap(const_cast<uint8_t *>(mapped), size_t(mapped_size));
#endif
}
