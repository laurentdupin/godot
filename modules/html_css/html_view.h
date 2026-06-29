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
#include "html_render_surface.h"

#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "scene/gui/control.h"

class ColorRect;
class Shader;
class ShaderMaterial;
class Viewport;

class HTMLView : public Control {
	GDCLASS(HTMLView, Control);

public:
	enum BackendPreference {
		BACKEND_AUTO,
		BACKEND_CPU,
		BACKEND_GPU_AUTO,
		BACKEND_VULKAN,
		BACKEND_D3D12,
	};

	enum ViewportSizeMode {
		VIEWPORT_SIZE_CONTROL,
		VIEWPORT_SIZE_SCREEN_PIXELS,
		VIEWPORT_SIZE_FIXED,
	};

private:
	Ref<HTMLRenderSurface> surface;
	HashMap<StringName, Callable> action_bindings;
	bool input_enabled = true;
	bool focus_on_click = true;
	BackendPreference backend_preference = BACKEND_AUTO;
	ViewportSizeMode viewport_size_mode = VIEWPORT_SIZE_SCREEN_PIXELS;
	Size2i fixed_viewport_size;
	bool use_document_minimum_size = false;
	bool backdrop_filter_enabled = false;
	StringName accept_action = "ui_accept";
	StringName focus_next_action = "ui_focus_next";
	StringName focus_previous_action = "ui_focus_prev";
	StringName text_submit_action = "ui_text_submit";
	StringName text_backspace_action = "ui_text_backspace";
	StringName text_delete_action = "ui_text_delete";
	bool pointer_press_active = false;
	MouseButton pointer_press_button = MouseButton::NONE;
	HTMLElementHit pointer_press_hit;
	bool frame_render_pending = false;
	int frame_render_delay = 0;
	ColorRect *backdrop_filter_rect = nullptr;
	Viewport *viewport_size_changed_viewport = nullptr;
	Ref<Shader> backdrop_filter_shader;
	Ref<ShaderMaterial> backdrop_filter_material;

	bool _has_current_viewport_size() const;
	bool _should_defer_backend_activation() const;
	void _apply_surface_backend_preference();
	void _surface_changed();
	void _connect_viewport_size_changed();
	void _disconnect_viewport_size_changed();
	void _viewport_size_changed();
	void _ensure_document();
	void _ensure_backdrop_filter_canvas();
	void _update_backdrop_filter_canvas();
	Vector2 _get_screen_pixel_scale() const;
	Size2i _get_target_viewport_size() const;
	float _get_target_device_scale_factor() const;
	void _update_surface_size(bool p_force_render = true);
	Vector2 _local_to_html_position(const Vector2 &p_position) const;
	int _modifiers_from_event(const Ref<InputEvent> &p_event, MouseButton p_button = MouseButton::NONE, bool p_button_pressed = false) const;
	HTMLSurfaceMouseButton _to_html_mouse_button(MouseButton p_button) const;
	HTMLSurfaceInputKey _to_html_input_key(Key p_key) const;
	bool _hit_test(const Vector2 &p_html_position, HTMLElementHit &r_hit) const;
	bool _same_activation_target(const HTMLElementHit &p_pressed, const HTMLElementHit &p_released) const;
	void _emit_activation(const HTMLElementHit &p_hit, const Vector2 &p_html_position, MouseButton p_button);
	bool _send_action_key_event(const Ref<InputEvent> &p_event);
	bool _send_key_event(const Ref<InputEventKey> &p_event);
	void _call_bound_action(const StringName &p_action, const Dictionary &p_payload);
	void _queue_frame_render();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	void set_html(const String &p_html);
	String get_html() const;

	void set_css(const String &p_css);
	String get_css() const;

	void set_html_file(const String &p_html_file);
	String get_html_file() const;

	void set_css_files(const PackedStringArray &p_css_files);
	PackedStringArray get_css_files() const;
	void add_css_file(const String &p_css_file);
	void remove_css_file(const String &p_css_file);
	void clear_css_files();

	void set_input_enabled(bool p_input_enabled);
	bool is_input_enabled() const;

	void set_focus_on_click(bool p_focus_on_click);
	bool is_focus_on_click_enabled() const;

	void set_accept_action(const StringName &p_action);
	StringName get_accept_action() const;

	void set_focus_next_action(const StringName &p_action);
	StringName get_focus_next_action() const;

	void set_focus_previous_action(const StringName &p_action);
	StringName get_focus_previous_action() const;

	void set_text_submit_action(const StringName &p_action);
	StringName get_text_submit_action() const;

	void set_text_backspace_action(const StringName &p_action);
	StringName get_text_backspace_action() const;

	void set_text_delete_action(const StringName &p_action);
	StringName get_text_delete_action() const;

	void set_backend_preference(BackendPreference p_backend_preference);
	BackendPreference get_backend_preference() const;

	void set_viewport_size_mode(ViewportSizeMode p_viewport_size_mode);
	ViewportSizeMode get_viewport_size_mode() const;

	void set_fixed_viewport_size(const Size2i &p_fixed_viewport_size);
	Size2i get_fixed_viewport_size() const;

	void set_use_document_minimum_size(bool p_use_document_minimum_size);
	bool is_using_document_minimum_size() const;

	void set_backdrop_filter_enabled(bool p_backdrop_filter_enabled);
	bool is_backdrop_filter_enabled() const;
	Array get_backdrop_filter_regions() const;

	Ref<Texture2D> get_texture() const;
	Vector2 local_to_html_position(const Vector2 &p_position) const;
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

	void bind_action(const StringName &p_action, const Callable &p_callable);
	void unbind_action(const StringName &p_action);
	bool has_action(const StringName &p_action) const;

	void gui_input(const Ref<InputEvent> &p_event) override;
	Size2 get_minimum_size() const override;

	HTMLView();
};

VARIANT_ENUM_CAST(HTMLView::BackendPreference);
VARIANT_ENUM_CAST(HTMLView::ViewportSizeMode);
