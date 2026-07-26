/**************************************************************************/
/*  test_html_surface_cpu_backend.h                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "../backend/html_surface_cpu_backend.h"

#include "tests/test_macros.h"

namespace TestHTMLSurfaceCPUBackend {

class ConversionAccess : public HTMLSurfaceCPUBackend {
public:
	static void convert(const uint8_t *p_source, int p_source_stride, uint8_t *p_destination, int p_width, int p_height) {
		convert_bgra_to_rgba(p_source, p_source_stride, p_destination, p_width, p_height);
	}
};

TEST_CASE("[HTMLCSS] BGRA conversion preserves odd-width strided pixels") {
	constexpr int width = 5;
	constexpr int height = 2;
	constexpr int source_stride = 24;
	const uint8_t source[source_stride * height] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 201, 202, 203, 204,
		21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 205, 206, 207, 208,
	};
	const uint8_t expected[width * height * 4] = {
		3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12, 15, 14, 13, 16, 19, 18, 17, 20,
		23, 22, 21, 24, 27, 26, 25, 28, 31, 30, 29, 32, 35, 34, 33, 36, 39, 38, 37, 40,
	};
	uint8_t converted[width * height * 4] = {};

	ConversionAccess::convert(source, source_stride, converted, width, height);

	CHECK(memcmp(converted, expected, sizeof(expected)) == 0);
}

} // namespace TestHTMLSurfaceCPUBackend
