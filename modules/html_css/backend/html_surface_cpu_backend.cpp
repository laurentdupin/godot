/**************************************************************************/
/*  html_surface_cpu_backend.cpp                                          */
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

#include "html_surface_cpu_backend.h"

void HTMLSurfaceCPUBackend::set_size(const Size2i &p_size) {
	size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
}

void HTMLSurfaceCPUBackend::set_transparent_background(bool p_transparent_background) {
	transparent_background = p_transparent_background;
}

void HTMLSurfaceCPUBackend::set_placeholder_background(const Color &p_color) {
	placeholder_background = p_color;
}

void HTMLSurfaceCPUBackend::render_placeholder(const String &p_marker) {
	Color background = placeholder_background;
	if (transparent_background) {
		background.a = 0.0;
	}
	texture->update_placeholder(size, background, p_marker);
}

Error HTMLSurfaceCPUBackend::submit_cpu_frame(const HTMLCPUFrame &p_frame) {
	ERR_FAIL_COND_V_MSG(p_frame.size.x <= 0 || p_frame.size.y <= 0, ERR_INVALID_PARAMETER, "HTML CPU frame size must be positive.");
	ERR_FAIL_COND_V_MSG(p_frame.pixel_format != HTML_FRAME_PIXEL_FORMAT_RGBA8 && p_frame.pixel_format != HTML_FRAME_PIXEL_FORMAT_BGRA8, ERR_INVALID_PARAMETER, "HTML CPU frame pixel format is not supported.");

	const int row_bytes = p_frame.size.x * 4;
	ERR_FAIL_COND_V_MSG(p_frame.stride < row_bytes, ERR_INVALID_PARAMETER, "HTML CPU frame stride is smaller than the row size.");

	const int64_t required_size = int64_t(p_frame.stride) * int64_t(p_frame.size.y - 1) + row_bytes;
	ERR_FAIL_COND_V_MSG(p_frame.pixels.size() < required_size, ERR_INVALID_DATA, "HTML CPU frame pixel buffer is shorter than the declared size.");

	Vector<uint8_t> rgba;
	rgba.resize(row_bytes * p_frame.size.y);
	uint8_t *write = rgba.ptrw();
	const uint8_t *read = p_frame.pixels.ptr();

	for (int y = 0; y < p_frame.size.y; y++) {
		const uint8_t *src_row = read + int64_t(p_frame.stride) * y;
		uint8_t *dst_row = write + int64_t(row_bytes) * y;
		if (p_frame.pixel_format == HTML_FRAME_PIXEL_FORMAT_RGBA8) {
			memcpy(dst_row, src_row, row_bytes);
		} else {
			for (int x = 0; x < p_frame.size.x; x++) {
				const uint8_t *src = src_row + x * 4;
				uint8_t *dst = dst_row + x * 4;
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = src[3];
			}
		}
	}

	Ref<Image> image = Image::create_from_data(p_frame.size.x, p_frame.size.y, false, Image::FORMAT_RGBA8, rgba);
	ERR_FAIL_COND_V_MSG(image.is_null() || image->is_empty(), ERR_CANT_CREATE, "Could not create an Image from the HTML CPU frame.");

	size = p_frame.size;
	texture->update_from_image(image);
	return OK;
}

Ref<Texture2D> HTMLSurfaceCPUBackend::get_texture() const {
	return texture;
}

Ref<HTMLTexture2D> HTMLSurfaceCPUBackend::get_html_texture() const {
	return texture;
}

HTMLSurfaceCPUBackend::HTMLSurfaceCPUBackend() {
	texture.instantiate();
}
