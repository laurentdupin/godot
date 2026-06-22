/**************************************************************************/
/*  html_view.cpp                                                         */
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

#include "html_view.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

static HTMLSurfaceBackendPreference html_view_to_surface_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	return p_backend_preference == HTMLView::BACKEND_CPU ? HTML_SURFACE_BACKEND_CPU : HTML_SURFACE_BACKEND_AUTO;
}

void HTMLView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLView::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLView::get_document);
	ClassDB::bind_method(D_METHOD("set_html", "html"), &HTMLView::set_html);
	ClassDB::bind_method(D_METHOD("get_html"), &HTMLView::get_html);
	ClassDB::bind_method(D_METHOD("set_html_file", "html_file"), &HTMLView::set_html_file);
	ClassDB::bind_method(D_METHOD("get_html_file"), &HTMLView::get_html_file);
	ClassDB::bind_method(D_METHOD("set_input_enabled", "input_enabled"), &HTMLView::set_input_enabled);
	ClassDB::bind_method(D_METHOD("is_input_enabled"), &HTMLView::is_input_enabled);
	ClassDB::bind_method(D_METHOD("set_focus_on_click", "focus_on_click"), &HTMLView::set_focus_on_click);
	ClassDB::bind_method(D_METHOD("is_focus_on_click_enabled"), &HTMLView::is_focus_on_click_enabled);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLView::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLView::get_backend_preference);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLView::get_texture);
	ClassDB::bind_method(D_METHOD("bind_action", "action", "callable"), &HTMLView::bind_action);
	ClassDB::bind_method(D_METHOD("unbind_action", "action"), &HTMLView::unbind_action);
	ClassDB::bind_method(D_METHOD("has_action", "action"), &HTMLView::has_action);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html", PROPERTY_HINT_MULTILINE_TEXT), "set_html", "get_html");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html_file", PROPERTY_HINT_FILE, "*.html,*.htm"), "set_html_file", "get_html_file");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_enabled"), "set_input_enabled", "is_input_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "focus_on_click"), "set_focus_on_click", "is_focus_on_click_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU"), "set_backend_preference", "get_backend_preference");

	ADD_SIGNAL(MethodInfo("action_requested", PropertyInfo(Variant::STRING_NAME, "action"), PropertyInfo(Variant::DICTIONARY, "payload")));
	ADD_SIGNAL(MethodInfo("element_clicked", PropertyInfo(Variant::STRING_NAME, "element_id"), PropertyInfo(Variant::INT, "button")));
	ADD_SIGNAL(MethodInfo("render_error", PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(BACKEND_AUTO);
	BIND_ENUM_CONSTANT(BACKEND_CPU);
}

void HTMLView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			Ref<Texture2D> texture = surface->get_texture();
			if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
				draw_texture_rect(texture, Rect2(Vector2(), get_size()));
			}
		} break;

		case NOTIFICATION_RESIZED: {
			_update_surface_size();
			update_minimum_size();
		} break;
	}
}

void HTMLView::_surface_changed() {
	update_minimum_size();
	queue_redraw();
}

void HTMLView::_ensure_document() {
	if (surface->get_document().is_null()) {
		Ref<HTMLDocument> document;
		document.instantiate();
		surface->set_document(document);
	}
}

void HTMLView::_update_surface_size() {
	Size2 control_size = get_size();
	Size2i target_size = Size2i(control_size.x, control_size.y);
	if (target_size.x <= 0 || target_size.y <= 0) {
		Ref<HTMLDocument> document = surface->get_document();
		target_size = document.is_valid() ? document->get_default_size() : Size2i(512, 512);
	}
	surface->set_size(target_size);
	surface->render_now("HTMLView");
	queue_redraw();
}

void HTMLView::_emit_placeholder_click(const Vector2 &p_position, MouseButton p_button) {
	const StringName element_id = SNAME("placeholder");
	const StringName action = SNAME("default");
	Dictionary payload;
	payload[SNAME("element_id")] = element_id;
	payload[SNAME("position")] = p_position;
	payload[SNAME("button")] = (int)p_button;

	emit_signal(SNAME("element_clicked"), element_id, (int)p_button);
	emit_signal(SNAME("action_requested"), action, payload);
	_call_bound_action(action, payload);
}

void HTMLView::_call_bound_action(const StringName &p_action, const Dictionary &p_payload) {
	const Callable *callable = action_bindings.getptr(p_action);
	if (!callable || !callable->is_valid()) {
		return;
	}

	const Variant args[1] = { p_payload };
	const Variant *argptrs[1] = { &args[0] };
	Variant ret;
	Callable::CallError ce;
	callable->callp(argptrs, 1, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		String message = vformat("HTML action '%s' callable failed with error %d.", String(p_action), ce.error);
		ERR_PRINT(message);
		emit_signal(SNAME("render_error"), message);
	}
}

void HTMLView::set_document(const Ref<HTMLDocument> &p_document) {
	surface->set_document(p_document);
	_update_surface_size();
}

Ref<HTMLDocument> HTMLView::get_document() const {
	return surface->get_document();
}

void HTMLView::set_html(const String &p_html) {
	_ensure_document();
	surface->get_document()->set_html(p_html);
}

String HTMLView::get_html() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_html() : String();
}

void HTMLView::set_html_file(const String &p_html_file) {
	_ensure_document();
	surface->get_document()->set_html_file(p_html_file);
}

String HTMLView::get_html_file() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_html_file() : String();
}

void HTMLView::set_input_enabled(bool p_input_enabled) {
	input_enabled = p_input_enabled;
}

bool HTMLView::is_input_enabled() const {
	return input_enabled;
}

void HTMLView::set_focus_on_click(bool p_focus_on_click) {
	focus_on_click = p_focus_on_click;
}

bool HTMLView::is_focus_on_click_enabled() const {
	return focus_on_click;
}

void HTMLView::set_backend_preference(BackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 2);
	backend_preference = p_backend_preference;
	surface->set_backend_preference(html_view_to_surface_backend_preference(p_backend_preference));
}

HTMLView::BackendPreference HTMLView::get_backend_preference() const {
	return backend_preference;
}

Ref<Texture2D> HTMLView::get_texture() const {
	return surface->get_texture();
}

void HTMLView::bind_action(const StringName &p_action, const Callable &p_callable) {
	if (p_callable.is_valid()) {
		action_bindings[p_action] = p_callable;
	} else {
		action_bindings.erase(p_action);
	}
}

void HTMLView::unbind_action(const StringName &p_action) {
	action_bindings.erase(p_action);
}

bool HTMLView::has_action(const StringName &p_action) const {
	return action_bindings.has(p_action);
}

void HTMLView::gui_input(const Ref<InputEvent> &p_event) {
	if (!input_enabled || p_event.is_null()) {
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (focus_on_click && mb->is_pressed()) {
			grab_focus();
		}
		if (!mb->is_pressed()) {
			_emit_placeholder_click(mb->get_position(), mb->get_button_index());
			accept_event();
		}
	}
}

Size2 HTMLView::get_minimum_size() const {
	Ref<HTMLDocument> document = surface->get_document();
	if (document.is_valid()) {
		return document->get_default_size();
	}
	return Size2();
}

HTMLView::HTMLView() {
	surface.instantiate();
	surface->set_changed_callback(callable_mp(this, &HTMLView::_surface_changed));
	surface->set_placeholder_background(Color(0.08, 0.09, 0.1, 1.0));
	surface->set_backend_preference(html_view_to_surface_backend_preference(backend_preference));
	surface->render_now("HTMLView");
	set_focus_mode(FOCUS_CLICK);
}
