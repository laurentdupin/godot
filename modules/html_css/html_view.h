/**************************************************************************/
/*  html_view.h                                                           */
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
#include "html_texture.h"

#include "core/templates/hash_map.h"
#include "scene/gui/control.h"

class HTMLView : public Control {
	GDCLASS(HTMLView, Control);

public:
	enum BackendPreference {
		BACKEND_AUTO,
		BACKEND_CPU,
	};

private:
	Ref<HTMLDocument> document;
	Ref<HTMLTexture2D> texture;
	HashMap<StringName, Callable> action_bindings;
	bool input_enabled = true;
	bool focus_on_click = true;
	BackendPreference backend_preference = BACKEND_AUTO;

	void _document_changed();
	void _ensure_texture();
	void _update_placeholder();
	void _emit_placeholder_click(const Vector2 &p_position, MouseButton p_button);
	void _call_bound_action(const StringName &p_action, const Dictionary &p_payload);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	void set_html(const String &p_html);
	String get_html() const;

	void set_html_file(const String &p_html_file);
	String get_html_file() const;

	void set_input_enabled(bool p_input_enabled);
	bool is_input_enabled() const;

	void set_focus_on_click(bool p_focus_on_click);
	bool is_focus_on_click_enabled() const;

	void set_backend_preference(BackendPreference p_backend_preference);
	BackendPreference get_backend_preference() const;

	Ref<Texture2D> get_texture() const;

	void bind_action(const StringName &p_action, const Callable &p_callable);
	void unbind_action(const StringName &p_action);
	bool has_action(const StringName &p_action) const;

	void gui_input(const Ref<InputEvent> &p_event) override;
	Size2 get_minimum_size() const override;

	HTMLView();
};

VARIANT_ENUM_CAST(HTMLView::BackendPreference);
