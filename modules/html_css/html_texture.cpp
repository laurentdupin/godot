/**************************************************************************/
/*  html_texture.cpp                                                      */
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

#include "html_texture.h"

#include "core/object/class_db.h"

void HTMLTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_placeholder", "size", "background", "marker"), &HTMLTexture2D::update_placeholder, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_latest_image"), &HTMLTexture2D::get_latest_image);
}

void HTMLTexture2D::update_placeholder(const Size2i &p_size, const Color &p_background, const String &p_marker) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	Ref<Image> image = Image::create_empty(new_size.x, new_size.y, false, Image::FORMAT_RGBA8);
	image->fill(p_background);

	// The marker is accepted so the API can stay stable when the real renderer
	// starts labeling debug frames; this placeholder does not rasterize text.
	(void)p_marker;
	update_from_image(image);
}

void HTMLTexture2D::update_from_image(const Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image.is_null());
	ERR_FAIL_COND(p_image->is_empty());

	latest_image = p_image;

	if (texture.is_null()) {
		texture.instantiate();
	}

	if (size == Size2i(p_image->get_width(), p_image->get_height()) && texture->get_rid().is_valid() && texture->get_format() == p_image->get_format()) {
		texture->update(p_image);
	} else {
		texture->set_image(p_image);
	}

	size = Size2i(p_image->get_width(), p_image->get_height());
	alpha = p_image->detect_alpha() != Image::ALPHA_NONE;
	emit_changed();
}

int HTMLTexture2D::get_width() const {
	return size.x;
}

int HTMLTexture2D::get_height() const {
	return size.y;
}

RID HTMLTexture2D::get_rid() const {
	if (texture.is_null()) {
		return RID();
	}
	return texture->get_rid();
}

bool HTMLTexture2D::has_alpha() const {
	return alpha;
}

Ref<Image> HTMLTexture2D::get_image() const {
	return latest_image;
}

Ref<Image> HTMLTexture2D::get_latest_image() const {
	return latest_image;
}

void HTMLTexture2D::draw(RID p_canvas_item, const Point2 &p_pos, const Color &p_modulate, bool p_transpose) const {
	if (texture.is_valid()) {
		texture->draw(p_canvas_item, p_pos, p_modulate, p_transpose);
	}
}

void HTMLTexture2D::draw_rect(RID p_canvas_item, const Rect2 &p_rect, bool p_tile, const Color &p_modulate, bool p_transpose) const {
	if (texture.is_valid()) {
		texture->draw_rect(p_canvas_item, p_rect, p_tile, p_modulate, p_transpose);
	}
}

void HTMLTexture2D::draw_rect_region(RID p_canvas_item, const Rect2 &p_rect, const Rect2 &p_src_rect, const Color &p_modulate, bool p_transpose, bool p_clip_uv) const {
	if (texture.is_valid()) {
		texture->draw_rect_region(p_canvas_item, p_rect, p_src_rect, p_modulate, p_transpose, p_clip_uv);
	}
}

bool HTMLTexture2D::is_pixel_opaque(int p_x, int p_y) const {
	if (texture.is_null()) {
		return false;
	}
	return texture->is_pixel_opaque(p_x, p_y);
}

HTMLTexture2D::HTMLTexture2D() {
	texture.instantiate();
}
