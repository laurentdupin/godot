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

#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void HTMLSurfaceCPUBackend::clear_to_background() {
	Ref<Image> image = Image::create_empty(size.x, size.y, false, Image::FORMAT_RGBA8);
	image->fill(background_color);
	texture->update_from_image(image);
}

void HTMLSurfaceCPUBackend::set_size(const Size2i &p_size) {
	size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
}

void HTMLSurfaceCPUBackend::set_document(const Ref<HTMLDocument> &p_document) {
}

void HTMLSurfaceCPUBackend::set_transparent_background(bool p_transparent_background) {
	transparent_background = p_transparent_background;
}

void HTMLSurfaceCPUBackend::set_background_color(const Color &p_background_color) {
	background_color = p_background_color;
}

void HTMLSurfaceCPUBackend::set_placeholder_background(const Color &p_color) {
	placeholder_background = p_color;
}

void HTMLSurfaceCPUBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	clear_to_background();
}

void HTMLSurfaceCPUBackend::convert_bgra_to_rgba(const uint8_t *p_source, int p_source_stride, uint8_t *p_destination, int p_width, int p_height) {
	const int destination_stride = p_width * 4;
	for (int y = 0; y < p_height; y++) {
		const uint8_t *source_row = p_source + int64_t(p_source_stride) * y;
		uint8_t *destination_row = p_destination + int64_t(destination_stride) * y;
		for (int x = 0; x < p_width; x++) {
			uint32_t bgra;
			memcpy(&bgra, source_row + x * 4, sizeof(bgra));
			const uint32_t rgba_pixel = (bgra & 0xff00ff00U)
					| ((bgra & 0x00ff0000U) >> 16)
					| ((bgra & 0x000000ffU) << 16);
			memcpy(destination_row + x * 4, &rgba_pixel, sizeof(rgba_pixel));
		}
	}
}

Error HTMLSurfaceCPUBackend::submit_cpu_frame(const HTMLCPUFrame &p_frame) {
	return submit_cpu_frame_data(
			p_frame.size,
			p_frame.stride,
			p_frame.pixel_format,
			p_frame.pixels.ptr(),
			p_frame.pixels.size());
}

Error HTMLSurfaceCPUBackend::submit_cpu_frame_data(const Size2i &p_size, int p_stride, HTMLFramePixelFormat p_pixel_format, const uint8_t *p_pixels, int64_t p_pixel_count, bool p_alpha_known, bool p_has_alpha) {
	ERR_FAIL_COND_V_MSG(p_size.x <= 0 || p_size.y <= 0, ERR_INVALID_PARAMETER, "HTML CPU frame size must be positive.");
	ERR_FAIL_COND_V_MSG(p_pixel_format != HTML_FRAME_PIXEL_FORMAT_RGBA8 && p_pixel_format != HTML_FRAME_PIXEL_FORMAT_BGRA8, ERR_INVALID_PARAMETER, "HTML CPU frame pixel format is not supported.");

	const int row_bytes = p_size.x * 4;
	ERR_FAIL_COND_V_MSG(p_stride < row_bytes, ERR_INVALID_PARAMETER, "HTML CPU frame stride is smaller than the row size.");

	const int64_t required_size = int64_t(p_stride) * int64_t(p_size.y - 1) + row_bytes;
	ERR_FAIL_NULL_V_MSG(p_pixels, ERR_INVALID_DATA, "HTML CPU frame pixel buffer is null.");
	ERR_FAIL_COND_V_MSG(p_pixel_count < required_size, ERR_INVALID_DATA, "HTML CPU frame pixel buffer is shorter than the declared size.");

	const uint64_t conversion_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	Vector<uint8_t> rgba;
	rgba.resize(row_bytes * p_size.y);
	uint8_t *write = rgba.ptrw();

	if (p_pixel_format == HTML_FRAME_PIXEL_FORMAT_RGBA8) {
		for (int y = 0; y < p_size.y; y++) {
			const uint8_t *src_row = p_pixels + int64_t(p_stride) * y;
			uint8_t *dst_row = write + int64_t(row_bytes) * y;
			memcpy(dst_row, src_row, row_bytes);
		}
	} else {
		convert_bgra_to_rgba(p_pixels, p_stride, write, p_size.x, p_size.y);
	}

	Ref<Image> image = Image::create_from_data(p_size.x, p_size.y, false, Image::FORMAT_RGBA8, rgba);
	ERR_FAIL_COND_V_MSG(image.is_null() || image->is_empty(), ERR_CANT_CREATE, "Could not create an Image from the HTML CPU frame.");
	cpu_frame_conversion_milliseconds = conversion_start_usec != 0
			? (OS::get_singleton()->get_ticks_usec() - conversion_start_usec) / 1000.0
			: 0.0;

	size = p_size;
	const uint64_t upload_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	if (p_alpha_known) {
		texture->update_from_image(image, p_has_alpha);
	} else {
		texture->update_from_image(image);
	}
	cpu_frame_upload_milliseconds = upload_start_usec != 0
			? (OS::get_singleton()->get_ticks_usec() - upload_start_usec) / 1000.0
			: 0.0;
	return OK;
}

void HTMLSurfaceCPUBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	r_metadata = HTMLFrameMetadata();
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

HTMLSurfaceCPUBackend::~HTMLSurfaceCPUBackend() {
	if (texture.is_valid()) {
		texture.unref();
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr && !rendering_server->is_on_render_thread()) {
		rendering_server->sync();
	}
}
