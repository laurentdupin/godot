/**************************************************************************/
/*  html_render_target.cpp                                                */
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

#include "html_render_target.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

static HTMLSurfaceBackendPreference html_render_target_to_surface_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	switch (p_backend_preference) {
		case HTMLView::BACKEND_CPU:
			return HTML_SURFACE_BACKEND_CPU;
		case HTMLView::BACKEND_GPU_AUTO:
			return HTML_SURFACE_BACKEND_GPU_AUTO;
		case HTMLView::BACKEND_VULKAN:
			return HTML_SURFACE_BACKEND_VULKAN;
		case HTMLView::BACKEND_D3D12:
			return HTML_SURFACE_BACKEND_D3D12;
		case HTMLView::BACKEND_AUTO:
		default:
			return HTML_SURFACE_BACKEND_AUTO;
	}
}

static Dictionary html_render_target_form_control_state_to_dictionary(const HTMLFormControlState &p_state) {
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

static Dictionary html_render_target_backdrop_filter_region_to_dictionary(const HTMLBackdropFilterRegion &p_region) {
	Dictionary region;
	region[SNAME("element_id")] = p_region.element_id;
	region[SNAME("bounds")] = p_region.bounds;
	region[SNAME("blur_radius_css_px")] = p_region.blur_radius_css_px;
	region[SNAME("border_radius_top_left")] = p_region.border_radius_top_left;
	region[SNAME("border_radius_top_right")] = p_region.border_radius_top_right;
	region[SNAME("border_radius_bottom_right")] = p_region.border_radius_bottom_right;
	region[SNAME("border_radius_bottom_left")] = p_region.border_radius_bottom_left;
	region[SNAME("opacity")] = p_region.opacity;
	region[SNAME("flags")] = (int64_t)p_region.flags;
	region[SNAME("supported")] = !p_region.has_unsupported_flags();
	Array operations;
	for (const HTMLBackdropFilterOperation &operation : p_region.filter_operations) {
		Dictionary operation_data;
		operation_data[SNAME("type")] = operation.type;
		operation_data[SNAME("amount")] = operation.amount;
		operations.push_back(operation_data);
	}
	region[SNAME("filter_operations")] = operations;
	return region;
}

void HTMLRenderTarget::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLRenderTarget::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLRenderTarget::get_document);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &HTMLRenderTarget::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &HTMLRenderTarget::get_size);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLRenderTarget::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLRenderTarget::get_backend_preference);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLRenderTarget::get_texture);
	ClassDB::bind_method(D_METHOD("get_image"), &HTMLRenderTarget::get_image);
	ClassDB::bind_method(D_METHOD("get_backdrop_filter_regions"), &HTMLRenderTarget::get_backdrop_filter_regions);
	ClassDB::bind_method(D_METHOD("render_now"), &HTMLRenderTarget::render_now);
	ClassDB::bind_method(D_METHOD("set_element_text", "id", "text"), &HTMLRenderTarget::set_element_text);
	ClassDB::bind_method(D_METHOD("set_element_inner_html", "id", "html_fragment"), &HTMLRenderTarget::set_element_inner_html);
	ClassDB::bind_method(D_METHOD("set_body_inner_html", "html_fragment"), &HTMLRenderTarget::set_body_inner_html);
	ClassDB::bind_method(D_METHOD("set_element_attribute", "id", "name", "value"), &HTMLRenderTarget::set_element_attribute);
	ClassDB::bind_method(D_METHOD("remove_element_attribute", "id", "name"), &HTMLRenderTarget::remove_element_attribute);
	ClassDB::bind_method(D_METHOD("set_element_style", "id", "css_text"), &HTMLRenderTarget::set_element_style);
	ClassDB::bind_method(D_METHOD("replace_stylesheet_text", "style_id", "css_text"), &HTMLRenderTarget::replace_stylesheet_text);
	ClassDB::bind_method(D_METHOD("set_form_control_value", "id", "value"), &HTMLRenderTarget::set_form_control_value);
	ClassDB::bind_method(D_METHOD("set_form_control_checked", "id", "checked"), &HTMLRenderTarget::set_form_control_checked);
	ClassDB::bind_method(D_METHOD("focus_element", "id"), &HTMLRenderTarget::focus_element);
	ClassDB::bind_method(D_METHOD("blur_focused_element"), &HTMLRenderTarget::blur_focused_element);
	ClassDB::bind_method(D_METHOD("set_text_selection", "id", "start", "end"), &HTMLRenderTarget::set_text_selection);
	ClassDB::bind_method(D_METHOD("get_form_control_state", "id"), &HTMLRenderTarget::get_form_control_state);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:px"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU,GPU Auto,Vulkan,D3D12"), "set_backend_preference", "get_backend_preference");

	ADD_SIGNAL(MethodInfo("rendered"));
	ADD_SIGNAL(MethodInfo("texture_changed"));
}

void HTMLRenderTarget::_surface_changed() {
	emit_signal(SNAME("texture_changed"));
}

void HTMLRenderTarget::set_document(const Ref<HTMLDocument> &p_document) {
	surface->set_document(p_document);
}

Ref<HTMLDocument> HTMLRenderTarget::get_document() const {
	return surface->get_document();
}

void HTMLRenderTarget::set_size(const Size2i &p_size) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	size = new_size;
	surface->set_size(size);
}

Size2i HTMLRenderTarget::get_size() const {
	return size;
}

void HTMLRenderTarget::set_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 5);
	backend_preference = p_backend_preference;
	surface->set_backend_preference(html_render_target_to_surface_backend_preference(p_backend_preference));
}

HTMLView::BackendPreference HTMLRenderTarget::get_backend_preference() const {
	return backend_preference;
}

Ref<Texture2D> HTMLRenderTarget::get_texture() const {
	return surface->get_texture();
}

Ref<Image> HTMLRenderTarget::get_image() const {
	Ref<HTMLTexture2D> texture = surface->get_html_texture();
	return texture.is_valid() ? texture->get_latest_image() : Ref<Image>();
}

Array HTMLRenderTarget::get_backdrop_filter_regions() const {
	Array regions;
	for (const HTMLBackdropFilterRegion &region : surface->get_backdrop_filter_regions()) {
		regions.push_back(html_render_target_backdrop_filter_region_to_dictionary(region));
	}
	return regions;
}

void HTMLRenderTarget::render_now() {
	surface->render_now("HTMLRenderTarget");
	emit_signal(SNAME("rendered"));
}

Error HTMLRenderTarget::set_element_text(const StringName &p_id, const String &p_text) {
	Error err = surface->set_element_text(p_id, p_text);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	Error err = surface->set_element_inner_html(p_id, p_html_fragment);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_body_inner_html(const String &p_html_fragment) {
	Error err = surface->set_body_inner_html(p_html_fragment);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	Error err = surface->set_element_attribute(p_id, p_name, p_value);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	Error err = surface->remove_element_attribute(p_id, p_name);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_element_style(const StringName &p_id, const String &p_css_text) {
	Error err = surface->set_element_style(p_id, p_css_text);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	Error err = surface->replace_stylesheet_text(p_style_id, p_css_text);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_form_control_value(const StringName &p_id, const String &p_value) {
	Error err = surface->set_form_control_value(p_id, p_value);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_form_control_checked(const StringName &p_id, bool p_checked) {
	Error err = surface->set_form_control_checked(p_id, p_checked);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::focus_element(const StringName &p_id) {
	Error err = surface->focus_element(p_id);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::blur_focused_element() {
	Error err = surface->blur_focused_element();
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Error HTMLRenderTarget::set_text_selection(const StringName &p_id, int p_start, int p_end) {
	Error err = surface->set_text_selection(p_id, p_start, p_end);
	if (err == OK) {
		emit_signal(SNAME("rendered"));
	}
	return err;
}

Dictionary HTMLRenderTarget::get_form_control_state(const StringName &p_id) {
	HTMLFormControlState state;
	if (!surface->get_form_control_state(p_id, state)) {
		return Dictionary();
	}
	return html_render_target_form_control_state_to_dictionary(state);
}

HTMLRenderTarget::HTMLRenderTarget() {
	surface.instantiate();
	surface->set_changed_callback(callable_mp(this, &HTMLRenderTarget::_surface_changed));
	surface->set_placeholder_background(Color(0.06, 0.07, 0.08, 1.0));
	surface->set_size(size);
	surface->set_backend_preference(html_render_target_to_surface_backend_preference(backend_preference));
	surface->render_now("HTMLRenderTarget");
}
