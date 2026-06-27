/**************************************************************************/
/*  html_render_surface.cpp                                               */
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

#include "html_render_surface.h"

#ifdef HTML_CSS_USE_BLINK_C_API
#include "backend/html_surface_blink_c_api_backend.h"
#endif
#include "backend/html_surface_cpu_backend.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"

void HTMLRenderSurface::_ensure_backend() {
	if (backend != nullptr) {
		return;
	}

#ifdef HTML_CSS_USE_BLINK_C_API
	if (backend_preference == HTML_SURFACE_BACKEND_AUTO) {
		backend = memnew(HTMLSurfaceExternalCApiBackend);
	} else
#endif
	{
		backend = memnew(HTMLSurfaceCPUBackend);
	}
	_sync_backend_state();
}

void HTMLRenderSurface::_sync_backend_state() {
	ERR_FAIL_NULL(backend);
	Color background_color = document.is_valid() ? document->get_background_color() : Color(0, 0, 0, 0);
	backend->set_size(size);
	backend->set_device_scale_factor(device_scale_factor);
	backend->set_document(document);
	backend->set_transparent_background(background_color.a < 1.0);
	backend->set_background_color(background_color);
	backend->set_placeholder_background(placeholder_background);
}

void HTMLRenderSurface::_document_changed() {
	if (backend != nullptr) {
		backend->mark_document_dirty();
	}
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::_notify_changed() const {
	if (!changed_callback.is_valid()) {
		return;
	}

	Variant ret;
	Callable::CallError ce;
	changed_callback.callp(nullptr, 0, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("HTML render surface changed callback failed with error %d.", ce.error));
	}
}

void HTMLRenderSurface::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	document = p_document;
	if (document.is_valid()) {
		document->connect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	_sync_backend_state();
	render_now(marker);
}

Ref<HTMLDocument> HTMLRenderSurface::get_document() const {
	return document;
}

bool HTMLRenderSurface::set_size(const Size2i &p_size) {
	return set_viewport(p_size, device_scale_factor);
}

Size2i HTMLRenderSurface::get_size() const {
	return size;
}

bool HTMLRenderSurface::set_device_scale_factor(float p_device_scale_factor) {
	return set_viewport(size, p_device_scale_factor);
}

float HTMLRenderSurface::get_device_scale_factor() const {
	return device_scale_factor;
}

bool HTMLRenderSurface::set_viewport(const Size2i &p_size, float p_device_scale_factor) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	float new_device_scale_factor = p_device_scale_factor;
	if (!Math::is_finite(new_device_scale_factor) || new_device_scale_factor <= 0.0f) {
		new_device_scale_factor = 1.0f;
	}
	new_device_scale_factor = CLAMP(new_device_scale_factor, 0.01f, 8.0f);
	if (size == new_size && Math::is_equal_approx(device_scale_factor, new_device_scale_factor)) {
		return false;
	}
	size = new_size;
	device_scale_factor = new_device_scale_factor;
	render_now(marker);
	return true;
}

void HTMLRenderSurface::set_placeholder_background(const Color &p_color) {
	if (placeholder_background == p_color) {
		return;
	}
	placeholder_background = p_color;
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 2);
	if (backend_preference == p_backend_preference) {
		return;
	}
	backend_preference = p_backend_preference;
	if (backend != nullptr) {
		memdelete(backend);
		backend = nullptr;
	}
	render_now(marker);
}

HTMLSurfaceBackendPreference HTMLRenderSurface::get_backend_preference() const {
	return backend_preference;
}

void HTMLRenderSurface::set_changed_callback(const Callable &p_callback) {
	changed_callback = p_callback;
}

void HTMLRenderSurface::render_now(const String &p_marker) {
	marker = p_marker;
	_ensure_backend();
	_sync_backend_state();
	backend->render_placeholder(marker);
	backend->get_frame_metadata(frame_metadata);
	_notify_changed();
}

Error HTMLRenderSurface::submit_cpu_frame(const HTMLCPUFrame &p_frame, const HTMLFrameMetadata &p_metadata) {
	_ensure_backend();
	Error err = backend->submit_cpu_frame(p_frame);
	ERR_FAIL_COND_V(err != OK, err);

	frame_metadata = p_metadata;
	_notify_changed();
	return OK;
}

const HTMLFrameMetadata &HTMLRenderSurface::get_frame_metadata() const {
	return frame_metadata;
}

const HTMLElementHit *HTMLRenderSurface::find_hit_at(const Point2i &p_position) const {
	return frame_metadata.find_hit_at(p_position);
}

bool HTMLRenderSurface::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	const HTMLElementHit *hit = frame_metadata.find_hit_at(Point2i(Math::floor(p_position.x), Math::floor(p_position.y)));
	if (hit == nullptr) {
		return false;
	}

	r_hit = *hit;
	return true;
}

Error HTMLRenderSurface::mouse_move(const Point2 &p_position, int p_modifiers) {
	_ensure_backend();
	return backend->mouse_move(p_position, p_modifiers);
}

Error HTMLRenderSurface::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	_ensure_backend();
	return backend->mouse_down(p_position, p_button, p_modifiers, p_click_count);
}

Error HTMLRenderSurface::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	_ensure_backend();
	return backend->mouse_up(p_position, p_button, p_modifiers, p_click_count);
}

Error HTMLRenderSurface::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	_ensure_backend();
	return backend->wheel(p_position, p_delta);
}

Error HTMLRenderSurface::key_down(HTMLSurfaceInputKey p_key, int p_modifiers) {
	_ensure_backend();
	return backend->key_down(p_key, p_modifiers);
}

Error HTMLRenderSurface::key_up(HTMLSurfaceInputKey p_key, int p_modifiers) {
	_ensure_backend();
	return backend->key_up(p_key, p_modifiers);
}

Error HTMLRenderSurface::text_input(const String &p_text) {
	_ensure_backend();
	return backend->text_input(p_text);
}

Error HTMLRenderSurface::set_element_text(const StringName &p_id, const String &p_text) {
	_ensure_backend();
	Error err = backend->set_element_text(p_id, p_text);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	_ensure_backend();
	Error err = backend->set_element_inner_html(p_id, p_html_fragment);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	_ensure_backend();
	Error err = backend->set_element_attribute(p_id, p_name, p_value);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	_ensure_backend();
	Error err = backend->remove_element_attribute(p_id, p_name);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_element_style(const StringName &p_id, const String &p_css_text) {
	_ensure_backend();
	Error err = backend->set_element_style(p_id, p_css_text);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	_ensure_backend();
	Error err = backend->replace_stylesheet_text(p_style_id, p_css_text);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_form_control_value(const StringName &p_id, const String &p_value) {
	_ensure_backend();
	Error err = backend->set_form_control_value(p_id, p_value);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_form_control_checked(const StringName &p_id, bool p_checked) {
	_ensure_backend();
	Error err = backend->set_form_control_checked(p_id, p_checked);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::focus_element(const StringName &p_id) {
	_ensure_backend();
	Error err = backend->focus_element(p_id);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::blur_focused_element() {
	_ensure_backend();
	Error err = backend->blur_focused_element();
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

Error HTMLRenderSurface::set_text_selection(const StringName &p_id, int p_start, int p_end) {
	_ensure_backend();
	Error err = backend->set_text_selection(p_id, p_start, p_end);
	if (err != OK) {
		return err;
	}
	render_now(marker);
	return OK;
}

bool HTMLRenderSurface::get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) {
	_ensure_backend();
	return backend->get_form_control_state(p_id, r_state);
}

bool HTMLRenderSurface::is_document_source_valid() const {
	return document.is_null() || document->is_source_valid();
}

Error HTMLRenderSurface::load_asset(const String &p_uri, HTMLAssetResource &r_asset, String *r_error) const {
	return HTMLGodotAssetProvider::load_asset(document, p_uri, r_asset, r_error);
}

Ref<Texture2D> HTMLRenderSurface::get_texture() const {
	return backend != nullptr ? backend->get_texture() : Ref<Texture2D>();
}

Ref<HTMLTexture2D> HTMLRenderSurface::get_html_texture() const {
	return backend != nullptr ? backend->get_html_texture() : Ref<HTMLTexture2D>();
}

HTMLRenderSurface::HTMLRenderSurface() {
	_ensure_backend();
	render_now(marker);
}

HTMLRenderSurface::~HTMLRenderSurface() {
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	if (backend != nullptr) {
		memdelete(backend);
	}
}
