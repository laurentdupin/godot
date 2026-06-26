/**************************************************************************/
/*  html_surface_backend.h                                                */
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

#include "../bridge/html_frame_types.h"
#include "../html_document.h"
#include "../html_texture.h"

enum HTMLSurfaceMouseButton {
	HTML_SURFACE_MOUSE_BUTTON_NONE,
	HTML_SURFACE_MOUSE_BUTTON_LEFT,
	HTML_SURFACE_MOUSE_BUTTON_MIDDLE,
	HTML_SURFACE_MOUSE_BUTTON_RIGHT,
};

enum HTMLSurfaceInputKey {
	HTML_SURFACE_INPUT_KEY_UNKNOWN,
	HTML_SURFACE_INPUT_KEY_BACKSPACE,
	HTML_SURFACE_INPUT_KEY_TAB,
	HTML_SURFACE_INPUT_KEY_ENTER,
	HTML_SURFACE_INPUT_KEY_DELETE,
};

enum HTMLSurfaceInputModifier {
	HTML_SURFACE_INPUT_MODIFIER_SHIFT = 1 << 0,
	HTML_SURFACE_INPUT_MODIFIER_CONTROL = 1 << 1,
	HTML_SURFACE_INPUT_MODIFIER_ALT = 1 << 2,
	HTML_SURFACE_INPUT_MODIFIER_META = 1 << 3,
	HTML_SURFACE_INPUT_MODIFIER_LEFT_BUTTON_DOWN = 1 << 6,
	HTML_SURFACE_INPUT_MODIFIER_MIDDLE_BUTTON_DOWN = 1 << 7,
	HTML_SURFACE_INPUT_MODIFIER_RIGHT_BUTTON_DOWN = 1 << 8,
};

class HTMLSurfaceBackend {
public:
	virtual ~HTMLSurfaceBackend() {}

	virtual void mark_document_dirty() {}
	virtual void set_size(const Size2i &p_size) = 0;
	virtual void set_device_scale_factor(float p_device_scale_factor) {}
	virtual void set_document(const Ref<HTMLDocument> &p_document) = 0;
	virtual void set_transparent_background(bool p_transparent_background) = 0;
	virtual void set_background_color(const Color &p_background_color) = 0;
	virtual void set_placeholder_background(const Color &p_color) = 0;
	virtual void render_placeholder(const String &p_marker) = 0;
	virtual Error submit_cpu_frame(const HTMLCPUFrame &p_frame) = 0;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const = 0;
	virtual Error mouse_move(const Point2 &, int) { return ERR_UNAVAILABLE; }
	virtual Error mouse_down(const Point2 &, HTMLSurfaceMouseButton, int, int) { return ERR_UNAVAILABLE; }
	virtual Error mouse_up(const Point2 &, HTMLSurfaceMouseButton, int, int) { return ERR_UNAVAILABLE; }
	virtual Error wheel(const Point2 &, const Vector2 &) { return ERR_UNAVAILABLE; }
	virtual Error key_down(HTMLSurfaceInputKey, int) { return ERR_UNAVAILABLE; }
	virtual Error key_up(HTMLSurfaceInputKey, int) { return ERR_UNAVAILABLE; }
	virtual Error text_input(const String &) { return ERR_UNAVAILABLE; }
	virtual Error set_element_text(const StringName &, const String &) { return ERR_UNAVAILABLE; }
	virtual Error set_element_attribute(const StringName &, const StringName &, const String &) { return ERR_UNAVAILABLE; }
	virtual Error remove_element_attribute(const StringName &, const StringName &) { return ERR_UNAVAILABLE; }
	virtual Error set_element_style(const StringName &, const String &) { return ERR_UNAVAILABLE; }
	virtual Error replace_stylesheet_text(const StringName &, const String &) { return ERR_UNAVAILABLE; }
	virtual Error set_form_control_value(const StringName &, const String &) { return ERR_UNAVAILABLE; }
	virtual Error set_form_control_checked(const StringName &, bool) { return ERR_UNAVAILABLE; }
	virtual Error focus_element(const StringName &) { return ERR_UNAVAILABLE; }
	virtual Error blur_focused_element() { return ERR_UNAVAILABLE; }
	virtual Error set_text_selection(const StringName &, int, int) { return ERR_UNAVAILABLE; }
	virtual bool get_form_control_state(const StringName &, HTMLFormControlState &) { return false; }
	virtual bool hit_test(const Point2 &, HTMLElementHit &) const { return false; }
	virtual Ref<Texture2D> get_texture() const = 0;
	virtual Ref<HTMLTexture2D> get_html_texture() const = 0;
};
