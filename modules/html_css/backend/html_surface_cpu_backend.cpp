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

Ref<Texture2D> HTMLSurfaceCPUBackend::get_texture() const {
	return texture;
}

Ref<HTMLTexture2D> HTMLSurfaceCPUBackend::get_html_texture() const {
	return texture;
}

HTMLSurfaceCPUBackend::HTMLSurfaceCPUBackend() {
	texture.instantiate();
}
