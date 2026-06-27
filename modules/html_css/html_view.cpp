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

#include "bridge/html_activation_engine.h"

#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

static HTMLSurfaceBackendPreference html_view_to_surface_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	return p_backend_preference == HTMLView::BACKEND_CPU ? HTML_SURFACE_BACKEND_CPU : HTML_SURFACE_BACKEND_AUTO;
}

static Dictionary html_form_control_state_to_dictionary(const HTMLFormControlState &p_state) {
	Dictionary state;
	state[SNAME("element_id")] = p_state.element_id;
	state[SNAME("tag_name")] = p_state.tag_name;
	state[SNAME("value")] = p_state.value;
	state[SNAME("checked")] = p_state.checked;
	state[SNAME("focused")] = p_state.focused;
	state[SNAME("selection_offsets_present")] = p_state.selection_offsets_present;
	state[SNAME("selection_start")] = p_state.selection_start;
	state[SNAME("selection_end")] = p_state.selection_end;
	return state;
}

void HTMLView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLView::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLView::get_document);
	ClassDB::bind_method(D_METHOD("set_html", "html"), &HTMLView::set_html);
	ClassDB::bind_method(D_METHOD("get_html"), &HTMLView::get_html);
	ClassDB::bind_method(D_METHOD("set_css", "css"), &HTMLView::set_css);
	ClassDB::bind_method(D_METHOD("get_css"), &HTMLView::get_css);
	ClassDB::bind_method(D_METHOD("set_html_file", "html_file"), &HTMLView::set_html_file);
	ClassDB::bind_method(D_METHOD("get_html_file"), &HTMLView::get_html_file);
	ClassDB::bind_method(D_METHOD("set_css_files", "css_files"), &HTMLView::set_css_files);
	ClassDB::bind_method(D_METHOD("get_css_files"), &HTMLView::get_css_files);
	ClassDB::bind_method(D_METHOD("add_css_file", "css_file"), &HTMLView::add_css_file);
	ClassDB::bind_method(D_METHOD("remove_css_file", "css_file"), &HTMLView::remove_css_file);
	ClassDB::bind_method(D_METHOD("clear_css_files"), &HTMLView::clear_css_files);
	ClassDB::bind_method(D_METHOD("set_input_enabled", "input_enabled"), &HTMLView::set_input_enabled);
	ClassDB::bind_method(D_METHOD("is_input_enabled"), &HTMLView::is_input_enabled);
	ClassDB::bind_method(D_METHOD("set_focus_on_click", "focus_on_click"), &HTMLView::set_focus_on_click);
	ClassDB::bind_method(D_METHOD("is_focus_on_click_enabled"), &HTMLView::is_focus_on_click_enabled);
	ClassDB::bind_method(D_METHOD("set_accept_action", "action"), &HTMLView::set_accept_action);
	ClassDB::bind_method(D_METHOD("get_accept_action"), &HTMLView::get_accept_action);
	ClassDB::bind_method(D_METHOD("set_focus_next_action", "action"), &HTMLView::set_focus_next_action);
	ClassDB::bind_method(D_METHOD("get_focus_next_action"), &HTMLView::get_focus_next_action);
	ClassDB::bind_method(D_METHOD("set_focus_previous_action", "action"), &HTMLView::set_focus_previous_action);
	ClassDB::bind_method(D_METHOD("get_focus_previous_action"), &HTMLView::get_focus_previous_action);
	ClassDB::bind_method(D_METHOD("set_text_submit_action", "action"), &HTMLView::set_text_submit_action);
	ClassDB::bind_method(D_METHOD("get_text_submit_action"), &HTMLView::get_text_submit_action);
	ClassDB::bind_method(D_METHOD("set_text_backspace_action", "action"), &HTMLView::set_text_backspace_action);
	ClassDB::bind_method(D_METHOD("get_text_backspace_action"), &HTMLView::get_text_backspace_action);
	ClassDB::bind_method(D_METHOD("set_text_delete_action", "action"), &HTMLView::set_text_delete_action);
	ClassDB::bind_method(D_METHOD("get_text_delete_action"), &HTMLView::get_text_delete_action);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLView::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLView::get_backend_preference);
	ClassDB::bind_method(D_METHOD("set_viewport_size_mode", "viewport_size_mode"), &HTMLView::set_viewport_size_mode);
	ClassDB::bind_method(D_METHOD("get_viewport_size_mode"), &HTMLView::get_viewport_size_mode);
	ClassDB::bind_method(D_METHOD("set_fixed_viewport_size", "fixed_viewport_size"), &HTMLView::set_fixed_viewport_size);
	ClassDB::bind_method(D_METHOD("get_fixed_viewport_size"), &HTMLView::get_fixed_viewport_size);
	ClassDB::bind_method(D_METHOD("set_use_document_minimum_size", "use_document_minimum_size"), &HTMLView::set_use_document_minimum_size);
	ClassDB::bind_method(D_METHOD("is_using_document_minimum_size"), &HTMLView::is_using_document_minimum_size);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLView::get_texture);
	ClassDB::bind_method(D_METHOD("local_to_html_position", "position"), &HTMLView::local_to_html_position);
	ClassDB::bind_method(D_METHOD("set_element_text", "id", "text"), &HTMLView::set_element_text);
	ClassDB::bind_method(D_METHOD("set_element_inner_html", "id", "html_fragment"), &HTMLView::set_element_inner_html);
	ClassDB::bind_method(D_METHOD("set_element_attribute", "id", "name", "value"), &HTMLView::set_element_attribute);
	ClassDB::bind_method(D_METHOD("remove_element_attribute", "id", "name"), &HTMLView::remove_element_attribute);
	ClassDB::bind_method(D_METHOD("set_element_style", "id", "css_text"), &HTMLView::set_element_style);
	ClassDB::bind_method(D_METHOD("replace_stylesheet_text", "style_id", "css_text"), &HTMLView::replace_stylesheet_text);
	ClassDB::bind_method(D_METHOD("set_form_control_value", "id", "value"), &HTMLView::set_form_control_value);
	ClassDB::bind_method(D_METHOD("set_form_control_checked", "id", "checked"), &HTMLView::set_form_control_checked);
	ClassDB::bind_method(D_METHOD("focus_element", "id"), &HTMLView::focus_element);
	ClassDB::bind_method(D_METHOD("blur_focused_element"), &HTMLView::blur_focused_element);
	ClassDB::bind_method(D_METHOD("set_text_selection", "id", "start", "end"), &HTMLView::set_text_selection);
	ClassDB::bind_method(D_METHOD("get_form_control_state", "id"), &HTMLView::get_form_control_state);
	ClassDB::bind_method(D_METHOD("bind_action", "action", "callable"), &HTMLView::bind_action);
	ClassDB::bind_method(D_METHOD("unbind_action", "action"), &HTMLView::unbind_action);
	ClassDB::bind_method(D_METHOD("has_action", "action"), &HTMLView::has_action);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html", PROPERTY_HINT_MULTILINE_TEXT), "set_html", "get_html");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "css", PROPERTY_HINT_MULTILINE_TEXT), "set_css", "get_css");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html_file", PROPERTY_HINT_FILE, "*.html,*.htm"), "set_html_file", "get_html_file");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "css_files"), "set_css_files", "get_css_files");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_enabled"), "set_input_enabled", "is_input_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "focus_on_click"), "set_focus_on_click", "is_focus_on_click_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "accept_action"), "set_accept_action", "get_accept_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "focus_next_action"), "set_focus_next_action", "get_focus_next_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "focus_previous_action"), "set_focus_previous_action", "get_focus_previous_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_submit_action"), "set_text_submit_action", "get_text_submit_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_backspace_action"), "set_text_backspace_action", "get_text_backspace_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_delete_action"), "set_text_delete_action", "get_text_delete_action");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU"), "set_backend_preference", "get_backend_preference");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "viewport_size_mode", PROPERTY_HINT_ENUM, "Control Size,Screen Pixels,Fixed"), "set_viewport_size_mode", "get_viewport_size_mode");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "fixed_viewport_size", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:px"), "set_fixed_viewport_size", "get_fixed_viewport_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_document_minimum_size"), "set_use_document_minimum_size", "is_using_document_minimum_size");

	ADD_SIGNAL(MethodInfo("action_requested", PropertyInfo(Variant::STRING_NAME, "action"), PropertyInfo(Variant::DICTIONARY, "payload")));
	ADD_SIGNAL(MethodInfo("element_clicked", PropertyInfo(Variant::STRING_NAME, "element_id"), PropertyInfo(Variant::INT, "button")));
	ADD_SIGNAL(MethodInfo("render_error", PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(BACKEND_AUTO);
	BIND_ENUM_CONSTANT(BACKEND_CPU);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_CONTROL);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_SCREEN_PIXELS);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_FIXED);
}

void HTMLView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (frame_render_pending) {
				set_process_internal(true);
			}
			_update_surface_size();
		} break;

		case NOTIFICATION_DRAW: {
			Ref<Texture2D> texture = surface->get_texture();
			if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
				draw_texture_rect(texture, Rect2(Vector2(), get_size()));
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!frame_render_pending) {
				set_process_internal(false);
				break;
			}

			if (frame_render_delay > 0) {
				frame_render_delay--;
				break;
			}

			frame_render_pending = false;
			set_process_internal(false);
			surface->render_now("HTMLView");
		} break;

		case NOTIFICATION_RESIZED: {
			_update_surface_size();
			update_minimum_size();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_surface_size(false);
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

Vector2 HTMLView::_get_screen_pixel_scale() const {
	if (!is_inside_tree()) {
		return Vector2(1, 1);
	}

	const Vector2 scale = (get_viewport_transform() * get_global_transform()).get_scale().abs();
	float scale_x = scale.x;
	float scale_y = scale.y;
	if (!Math::is_finite(scale_x) || scale_x <= 0.0f) {
		scale_x = 1.0f;
	}
	if (!Math::is_finite(scale_y) || scale_y <= 0.0f) {
		scale_y = 1.0f;
	}
	return Vector2(CLAMP(scale_x, 0.01f, 8.0f), CLAMP(scale_y, 0.01f, 8.0f));
}

Size2i HTMLView::_get_target_viewport_size() const {
	const Size2 control_size = get_size();
	Size2i target_size;

	switch (viewport_size_mode) {
		case VIEWPORT_SIZE_SCREEN_PIXELS: {
			const Vector2 screen_scale = _get_screen_pixel_scale();
			target_size = Size2i(Math::ceil(control_size.x * screen_scale.x), Math::ceil(control_size.y * screen_scale.y));
		} break;

		case VIEWPORT_SIZE_FIXED: {
			target_size = fixed_viewport_size;
			if (target_size.x <= 0 || target_size.y <= 0) {
				Ref<HTMLDocument> document = surface->get_document();
				target_size = document.is_valid() ? document->get_default_size() : Size2i(512, 512);
			}
		} break;

		case VIEWPORT_SIZE_CONTROL:
		default: {
			target_size = Size2i(Math::ceil(control_size.x), Math::ceil(control_size.y));
		} break;
	}

	if (target_size.x <= 0 || target_size.y <= 0) {
		Ref<HTMLDocument> document = surface->get_document();
		target_size = document.is_valid() ? document->get_default_size() : Size2i(512, 512);
	}
	return Size2i(MAX(1, target_size.x), MAX(1, target_size.y));
}

float HTMLView::_get_target_device_scale_factor() const {
	if (viewport_size_mode != VIEWPORT_SIZE_CONTROL) {
		return 1.0f;
	}

	const Vector2 scale = _get_screen_pixel_scale();
	return CLAMP(MAX(scale.x, scale.y), 1.0f, 8.0f);
}

void HTMLView::_update_surface_size(bool p_force_render) {
	if (!surface->set_viewport(_get_target_viewport_size(), _get_target_device_scale_factor()) && p_force_render) {
		surface->render_now("HTMLView");
	}
	queue_redraw();
}

Vector2 HTMLView::_local_to_html_position(const Vector2 &p_position) const {
	const Size2 control_size = get_size();
	const Size2i html_size = surface->get_size();
	if (control_size.x <= 0.0 || control_size.y <= 0.0 || html_size.x <= 0 || html_size.y <= 0) {
		return p_position;
	}

	return Vector2(
			p_position.x * (double)html_size.x / control_size.x,
			p_position.y * (double)html_size.y / control_size.y);
}

int HTMLView::_modifiers_from_event(const Ref<InputEvent> &p_event, MouseButton p_button, bool p_button_pressed) const {
	int modifiers = 0;

	Ref<InputEventWithModifiers> modifiers_event = p_event;
	if (modifiers_event.is_valid()) {
		if (modifiers_event->is_shift_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_SHIFT;
		}
		if (modifiers_event->is_ctrl_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_CONTROL;
		}
		if (modifiers_event->is_alt_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_ALT;
		}
		if (modifiers_event->is_meta_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_META;
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		const BitField<MouseButtonMask> button_mask = motion->get_button_mask();
		if (button_mask.has_flag(MouseButtonMask::LEFT)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_LEFT_BUTTON_DOWN;
		}
		if (button_mask.has_flag(MouseButtonMask::MIDDLE)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_MIDDLE_BUTTON_DOWN;
		}
		if (button_mask.has_flag(MouseButtonMask::RIGHT)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_RIGHT_BUTTON_DOWN;
		}
	}

	if (p_button_pressed) {
		switch (p_button) {
			case MouseButton::LEFT:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_LEFT_BUTTON_DOWN;
				break;
			case MouseButton::MIDDLE:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_MIDDLE_BUTTON_DOWN;
				break;
			case MouseButton::RIGHT:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_RIGHT_BUTTON_DOWN;
				break;
			default:
				break;
		}
	}

	return modifiers;
}

HTMLSurfaceMouseButton HTMLView::_to_html_mouse_button(MouseButton p_button) const {
	switch (p_button) {
		case MouseButton::LEFT:
			return HTML_SURFACE_MOUSE_BUTTON_LEFT;
		case MouseButton::MIDDLE:
			return HTML_SURFACE_MOUSE_BUTTON_MIDDLE;
		case MouseButton::RIGHT:
			return HTML_SURFACE_MOUSE_BUTTON_RIGHT;
		default:
			return HTML_SURFACE_MOUSE_BUTTON_NONE;
	}
}

HTMLSurfaceInputKey HTMLView::_to_html_input_key(Key p_key) const {
	switch (p_key) {
		case Key::BACKSPACE:
			return HTML_SURFACE_INPUT_KEY_BACKSPACE;
		case Key::TAB:
			return HTML_SURFACE_INPUT_KEY_TAB;
		case Key::ENTER:
		case Key::KP_ENTER:
			return HTML_SURFACE_INPUT_KEY_ENTER;
		case Key::KEY_DELETE:
			return HTML_SURFACE_INPUT_KEY_DELETE;
		default:
			return HTML_SURFACE_INPUT_KEY_UNKNOWN;
	}
}

bool HTMLView::_hit_test(const Vector2 &p_html_position, HTMLElementHit &r_hit) const {
	return surface->hit_test(Point2(p_html_position.x, p_html_position.y), r_hit);
}

bool HTMLView::_same_activation_target(const HTMLElementHit &p_pressed, const HTMLElementHit &p_released) const {
	if (p_pressed.disabled || p_released.disabled) {
		return false;
	}

	if (p_pressed.element_id != StringName() || p_released.element_id != StringName()) {
		return p_pressed.element_id == p_released.element_id;
	}

	const String pressed_action = p_pressed.get_attribute(SNAME("data-godot-action"));
	const String released_action = p_released.get_attribute(SNAME("data-godot-action"));
	if (!pressed_action.is_empty() || !released_action.is_empty()) {
		return pressed_action == released_action && p_pressed.bounds == p_released.bounds;
	}

	return p_pressed.tag_name == p_released.tag_name && p_pressed.bounds == p_released.bounds;
}

void HTMLView::_emit_activation(const HTMLElementHit &p_hit, const Vector2 &p_html_position, MouseButton p_button) {
	if (p_hit.disabled) {
		return;
	}

	const Point2i document_position(Math::floor(p_html_position.x), Math::floor(p_html_position.y));
	HTMLActionActivation activation = HTMLActivationEngine::activate(p_hit, document_position, p_button);
	if (!activation.has_action) {
		return;
	}

	emit_signal(SNAME("element_clicked"), activation.element_id, (int)p_button);
	emit_signal(SNAME("action_requested"), activation.action, activation.payload);
	_call_bound_action(activation.action, activation.payload);
}

bool HTMLView::_send_action_key_event(const Ref<InputEvent> &p_event) {
	struct ActionKeyMapping {
		StringName action;
		HTMLSurfaceInputKey key = HTML_SURFACE_INPUT_KEY_UNKNOWN;
		int extra_modifiers = 0;
	};

	const int base_modifiers = _modifiers_from_event(p_event);
	const ActionKeyMapping mappings[] = {
		{ focus_previous_action, HTML_SURFACE_INPUT_KEY_TAB, HTML_SURFACE_INPUT_MODIFIER_SHIFT },
		{ focus_next_action, HTML_SURFACE_INPUT_KEY_TAB, 0 },
		{ text_backspace_action, HTML_SURFACE_INPUT_KEY_BACKSPACE, 0 },
		{ text_delete_action, HTML_SURFACE_INPUT_KEY_DELETE, 0 },
		{ text_submit_action, HTML_SURFACE_INPUT_KEY_ENTER, 0 },
		{ accept_action, HTML_SURFACE_INPUT_KEY_ENTER, 0 },
	};

	for (const ActionKeyMapping &mapping : mappings) {
		if (mapping.action == StringName() || mapping.key == HTML_SURFACE_INPUT_KEY_UNKNOWN) {
			continue;
		}

		const int modifiers = base_modifiers | mapping.extra_modifiers;
		if (p_event->is_action_pressed(mapping.action, false, true)) {
			return surface->key_down(mapping.key, modifiers) == OK;
		}
		if (p_event->is_action_released(mapping.action, true)) {
			return surface->key_up(mapping.key, modifiers) == OK;
		}
	}

	return false;
}

bool HTMLView::_send_key_event(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null()) {
		return false;
	}

	bool handled = false;
	const HTMLSurfaceInputKey key = _to_html_input_key(p_event->get_keycode());
	if (key != HTML_SURFACE_INPUT_KEY_UNKNOWN) {
		if (p_event->is_pressed()) {
			handled = surface->key_down(key, _modifiers_from_event(p_event)) == OK;
		} else {
			handled = surface->key_up(key, _modifiers_from_event(p_event)) == OK;
		}
	}

	if (p_event->is_pressed() && !p_event->is_echo() && !p_event->is_ctrl_pressed() && !p_event->is_alt_pressed() && !p_event->is_meta_pressed() && p_event->get_unicode() >= 32) {
		handled = surface->text_input(String::chr(p_event->get_unicode())) == OK || handled;
	}

	return handled;
}

void HTMLView::_call_bound_action(const StringName &p_action, const Dictionary &p_payload) {
	const Callable *callable_ptr = action_bindings.getptr(p_action);
	if (!callable_ptr || !callable_ptr->is_valid()) {
		return;
	}

	const Callable callable = *callable_ptr;
	const Variant args[1] = { p_payload };
	const Variant *argptrs[1] = { &args[0] };
	Variant ret;
	Callable::CallError ce;
	callable.callp(argptrs, 1, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		String message = vformat("HTML action '%s' callable failed with error %d.", String(p_action), ce.error);
		ERR_PRINT(message);
		emit_signal(SNAME("render_error"), message);
	}
}

void HTMLView::_queue_frame_render() {
	if (frame_render_pending) {
		return;
	}

	frame_render_pending = true;
	frame_render_delay = 1;
	if (is_inside_tree()) {
		set_process_internal(true);
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

void HTMLView::set_css(const String &p_css) {
	_ensure_document();
	surface->get_document()->set_css(p_css);
}

String HTMLView::get_css() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_css() : String();
}

void HTMLView::set_html_file(const String &p_html_file) {
	_ensure_document();
	surface->get_document()->set_html_file(p_html_file);
}

String HTMLView::get_html_file() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_html_file() : String();
}

void HTMLView::set_css_files(const PackedStringArray &p_css_files) {
	_ensure_document();
	surface->get_document()->set_css_files(p_css_files);
}

PackedStringArray HTMLView::get_css_files() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_css_files() : PackedStringArray();
}

void HTMLView::add_css_file(const String &p_css_file) {
	_ensure_document();
	surface->get_document()->add_css_file(p_css_file);
}

void HTMLView::remove_css_file(const String &p_css_file) {
	_ensure_document();
	surface->get_document()->remove_css_file(p_css_file);
}

void HTMLView::clear_css_files() {
	_ensure_document();
	surface->get_document()->clear_css_files();
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

void HTMLView::set_accept_action(const StringName &p_action) {
	accept_action = p_action;
}

StringName HTMLView::get_accept_action() const {
	return accept_action;
}

void HTMLView::set_focus_next_action(const StringName &p_action) {
	focus_next_action = p_action;
}

StringName HTMLView::get_focus_next_action() const {
	return focus_next_action;
}

void HTMLView::set_focus_previous_action(const StringName &p_action) {
	focus_previous_action = p_action;
}

StringName HTMLView::get_focus_previous_action() const {
	return focus_previous_action;
}

void HTMLView::set_text_submit_action(const StringName &p_action) {
	text_submit_action = p_action;
}

StringName HTMLView::get_text_submit_action() const {
	return text_submit_action;
}

void HTMLView::set_text_backspace_action(const StringName &p_action) {
	text_backspace_action = p_action;
}

StringName HTMLView::get_text_backspace_action() const {
	return text_backspace_action;
}

void HTMLView::set_text_delete_action(const StringName &p_action) {
	text_delete_action = p_action;
}

StringName HTMLView::get_text_delete_action() const {
	return text_delete_action;
}

void HTMLView::set_backend_preference(BackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 2);
	backend_preference = p_backend_preference;
	surface->set_backend_preference(html_view_to_surface_backend_preference(p_backend_preference));
}

HTMLView::BackendPreference HTMLView::get_backend_preference() const {
	return backend_preference;
}

void HTMLView::set_viewport_size_mode(ViewportSizeMode p_viewport_size_mode) {
	ERR_FAIL_INDEX((int)p_viewport_size_mode, 3);
	if (viewport_size_mode == p_viewport_size_mode) {
		return;
	}
	viewport_size_mode = p_viewport_size_mode;
	_update_surface_size();
	update_minimum_size();
}

HTMLView::ViewportSizeMode HTMLView::get_viewport_size_mode() const {
	return viewport_size_mode;
}

void HTMLView::set_fixed_viewport_size(const Size2i &p_fixed_viewport_size) {
	const Size2i new_size = Size2i(MAX(0, p_fixed_viewport_size.x), MAX(0, p_fixed_viewport_size.y));
	if (fixed_viewport_size == new_size) {
		return;
	}
	fixed_viewport_size = new_size;
	_update_surface_size();
	update_minimum_size();
}

Size2i HTMLView::get_fixed_viewport_size() const {
	return fixed_viewport_size;
}

void HTMLView::set_use_document_minimum_size(bool p_use_document_minimum_size) {
	if (use_document_minimum_size == p_use_document_minimum_size) {
		return;
	}
	use_document_minimum_size = p_use_document_minimum_size;
	update_minimum_size();
}

bool HTMLView::is_using_document_minimum_size() const {
	return use_document_minimum_size;
}

Ref<Texture2D> HTMLView::get_texture() const {
	return surface->get_texture();
}

Vector2 HTMLView::local_to_html_position(const Vector2 &p_position) const {
	return _local_to_html_position(p_position);
}

Error HTMLView::set_element_text(const StringName &p_id, const String &p_text) {
	return surface->set_element_text(p_id, p_text);
}

Error HTMLView::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	return surface->set_element_inner_html(p_id, p_html_fragment);
}

Error HTMLView::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	return surface->set_element_attribute(p_id, p_name, p_value);
}

Error HTMLView::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	return surface->remove_element_attribute(p_id, p_name);
}

Error HTMLView::set_element_style(const StringName &p_id, const String &p_css_text) {
	return surface->set_element_style(p_id, p_css_text);
}

Error HTMLView::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	return surface->replace_stylesheet_text(p_style_id, p_css_text);
}

Error HTMLView::set_form_control_value(const StringName &p_id, const String &p_value) {
	return surface->set_form_control_value(p_id, p_value);
}

Error HTMLView::set_form_control_checked(const StringName &p_id, bool p_checked) {
	return surface->set_form_control_checked(p_id, p_checked);
}

Error HTMLView::focus_element(const StringName &p_id) {
	return surface->focus_element(p_id);
}

Error HTMLView::blur_focused_element() {
	return surface->blur_focused_element();
}

Error HTMLView::set_text_selection(const StringName &p_id, int p_start, int p_end) {
	return surface->set_text_selection(p_id, p_start, p_end);
}

Dictionary HTMLView::get_form_control_state(const StringName &p_id) {
	HTMLFormControlState state;
	if (!surface->get_form_control_state(p_id, state)) {
		return Dictionary();
	}
	return html_form_control_state_to_dictionary(state);
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

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 html_position = _local_to_html_position(mm->get_position());
		if (surface->mouse_move(html_position, _modifiers_from_event(mm)) == OK) {
			_queue_frame_render();
		}
		accept_event();
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const Vector2 html_position = _local_to_html_position(mb->get_position());
		const MouseButton button_index = mb->get_button_index();

		Vector2 wheel_delta;
		const float wheel_step = 48.0f * mb->get_factor();
		switch (button_index) {
			case MouseButton::WHEEL_UP:
				wheel_delta.y = -wheel_step;
				break;
			case MouseButton::WHEEL_DOWN:
				wheel_delta.y = wheel_step;
				break;
			case MouseButton::WHEEL_LEFT:
				wheel_delta.x = -wheel_step;
				break;
			case MouseButton::WHEEL_RIGHT:
				wheel_delta.x = wheel_step;
				break;
			default:
				break;
		}

		if (!wheel_delta.is_zero_approx()) {
			if (mb->is_pressed()) {
				if (surface->wheel(html_position, wheel_delta) == OK) {
					_queue_frame_render();
				}
				accept_event();
			}
			return;
		}

		const HTMLSurfaceMouseButton html_button = _to_html_mouse_button(button_index);
		if (html_button == HTML_SURFACE_MOUSE_BUTTON_NONE) {
			return;
		}

		if (focus_on_click && mb->is_pressed() && button_index == MouseButton::LEFT) {
			grab_focus();
		}

		if (mb->is_pressed()) {
			pointer_press_active = false;
			pointer_press_button = button_index;
			surface->mouse_down(html_position, html_button, _modifiers_from_event(mb, button_index, true), mb->is_double_click() ? 2 : 1);
			if (_hit_test(html_position, pointer_press_hit)) {
				pointer_press_active = true;
			}
			_queue_frame_render();
			accept_event();
			return;
		}

		surface->mouse_up(html_position, html_button, _modifiers_from_event(mb, button_index, false), mb->is_double_click() ? 2 : 1);
		HTMLElementHit release_hit;
		const bool has_release_hit = _hit_test(html_position, release_hit);
		if (button_index == MouseButton::LEFT && pointer_press_active && pointer_press_button == button_index && has_release_hit && _same_activation_target(pointer_press_hit, release_hit)) {
			_emit_activation(release_hit, html_position, button_index);
		}
		pointer_press_active = false;
		_queue_frame_render();
		accept_event();
		return;
	}

	if (_send_action_key_event(p_event)) {
		_queue_frame_render();
		accept_event();
		return;
	}

	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && _send_key_event(key_event)) {
		_queue_frame_render();
		accept_event();
		return;
	}
}

Size2 HTMLView::get_minimum_size() const {
	if (use_document_minimum_size) {
		Ref<HTMLDocument> document = surface->get_document();
		if (document.is_valid()) {
			return document->get_default_size();
		}
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
	set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
	set_notify_transform(true);
}
