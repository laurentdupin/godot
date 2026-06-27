/**************************************************************************/
/*  html_render_target.h                                                  */
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

#include "html_document.h"
#include "html_render_surface.h"
#include "html_view.h"

#include "scene/main/node.h"

class HTMLRenderTarget : public Node {
	GDCLASS(HTMLRenderTarget, Node);

	Ref<HTMLRenderSurface> surface;
	Size2i size = Size2i(512, 512);
	HTMLView::BackendPreference backend_preference = HTMLView::BACKEND_AUTO;

	void _surface_changed();

protected:
	static void _bind_methods();

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	void set_size(const Size2i &p_size);
	Size2i get_size() const;

	void set_backend_preference(HTMLView::BackendPreference p_backend_preference);
	HTMLView::BackendPreference get_backend_preference() const;

	Ref<Texture2D> get_texture() const;
	Ref<Image> get_image() const;
	void render_now();
	Error set_element_text(const StringName &p_id, const String &p_text);
	Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment);
	Error set_body_inner_html(const String &p_html_fragment);
	Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value);
	Error remove_element_attribute(const StringName &p_id, const StringName &p_name);
	Error set_element_style(const StringName &p_id, const String &p_css_text);
	Error replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text);
	Error set_form_control_value(const StringName &p_id, const String &p_value);
	Error set_form_control_checked(const StringName &p_id, bool p_checked);
	Error focus_element(const StringName &p_id);
	Error blur_focused_element();
	Error set_text_selection(const StringName &p_id, int p_start, int p_end);
	Dictionary get_form_control_state(const StringName &p_id);

	HTMLRenderTarget();
};
