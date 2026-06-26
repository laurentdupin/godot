/**************************************************************************/
/*  html_frame_types.h                                                    */
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

#include "core/math/rect2i.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

enum HTMLFramePixelFormat {
	HTML_FRAME_PIXEL_FORMAT_RGBA8,
	HTML_FRAME_PIXEL_FORMAT_BGRA8,
};

struct HTMLFrameDamage {
	Vector<Rect2i> rects;
	bool full_frame = true;
};

struct HTMLCPUFrame {
	Size2i size;
	int stride = 0;
	HTMLFramePixelFormat pixel_format = HTML_FRAME_PIXEL_FORMAT_RGBA8;
	bool premultiplied_alpha = true;
	Vector<uint8_t> pixels;
	HTMLFrameDamage damage;

	bool is_valid() const {
		if (size.x <= 0 || size.y <= 0) {
			return false;
		}
		if (pixel_format != HTML_FRAME_PIXEL_FORMAT_RGBA8 && pixel_format != HTML_FRAME_PIXEL_FORMAT_BGRA8) {
			return false;
		}
		const int minimum_stride = size.x * 4;
		if (stride < minimum_stride) {
			return false;
		}
		const int64_t required_size = int64_t(stride) * int64_t(size.y - 1) + minimum_stride;
		return pixels.size() >= required_size;
	}
};

struct HTMLElementAttribute {
	StringName name;
	String value;
};

struct HTMLElementHit {
	StringName element_id;
	StringName tag_name;
	Rect2i bounds;
	bool disabled = false;
	bool editable = false;
	bool checked = false;
	bool focused = false;
	Vector<HTMLElementAttribute> attributes;

	String get_attribute(const StringName &p_name) const {
		for (const HTMLElementAttribute &attribute : attributes) {
			if (attribute.name == p_name) {
				return attribute.value;
			}
		}
		return String();
	}

	bool has_attribute(const StringName &p_name) const {
		for (const HTMLElementAttribute &attribute : attributes) {
			if (attribute.name == p_name) {
				return true;
			}
		}
		return false;
	}
};

struct HTMLFormControlState {
	StringName element_id;
	StringName tag_name;
	String value;
	bool checked = false;
	bool focused = false;
	bool selection_offsets_present = false;
	uint32_t selection_start = 0;
	uint32_t selection_end = 0;
};

struct HTMLFrameMetadata {
	Vector<HTMLElementHit> hits;

	const HTMLElementHit *find_hit_at(const Point2i &p_position) const {
		for (int i = hits.size() - 1; i >= 0; i--) {
			if (hits[i].bounds.has_point(p_position)) {
				return &hits[i];
			}
		}
		return nullptr;
	}

	const HTMLElementHit *find_hit_by_id(const StringName &p_id) const {
		for (const HTMLElementHit &hit : hits) {
			if (hit.element_id == p_id) {
				return &hit;
			}
		}
		return nullptr;
	}
};
