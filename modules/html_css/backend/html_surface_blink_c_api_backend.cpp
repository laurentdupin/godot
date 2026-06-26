/**************************************************************************/
/*  html_surface_blink_c_api_backend.cpp                                  */
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

#include "html_surface_blink_c_api_backend.h"

#include "../bridge/html_asset_provider.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"

static Rect2i blink_standalone_rect_to_rect2i(const blink_standalone_rect_t &p_rect) {
	const int x = Math::floor(p_rect.x);
	const int y = Math::floor(p_rect.y);
	const int right = Math::ceil(p_rect.x + p_rect.width);
	const int bottom = Math::ceil(p_rect.y + p_rect.height);
	return Rect2i(x, y, MAX(0, right - x), MAX(0, bottom - y));
}

static String blink_standalone_string_to_godot(const char *p_text) {
	return p_text != nullptr ? String::utf8(p_text) : String();
}

static HTMLFramePixelFormat blink_standalone_pixel_format_to_frame_format(blink_standalone_pixel_format_t p_format) {
	return p_format == BLINK_STANDALONE_PIXEL_FORMAT_BGRA8 ? HTML_FRAME_PIXEL_FORMAT_BGRA8 : HTML_FRAME_PIXEL_FORMAT_RGBA8;
}

static blink_standalone_mouse_button_t html_surface_mouse_button_to_blink_standalone(HTMLSurfaceMouseButton p_button) {
	switch (p_button) {
		case HTML_SURFACE_MOUSE_BUTTON_LEFT:
			return BLINK_STANDALONE_MOUSE_BUTTON_LEFT;
		case HTML_SURFACE_MOUSE_BUTTON_MIDDLE:
			return BLINK_STANDALONE_MOUSE_BUTTON_MIDDLE;
		case HTML_SURFACE_MOUSE_BUTTON_RIGHT:
			return BLINK_STANDALONE_MOUSE_BUTTON_RIGHT;
		default:
			return BLINK_STANDALONE_MOUSE_BUTTON_NONE;
	}
}

static blink_standalone_key_t html_surface_input_key_to_blink_standalone(HTMLSurfaceInputKey p_key) {
	switch (p_key) {
		case HTML_SURFACE_INPUT_KEY_BACKSPACE:
			return BLINK_STANDALONE_KEY_BACKSPACE;
		case HTML_SURFACE_INPUT_KEY_TAB:
			return BLINK_STANDALONE_KEY_TAB;
		case HTML_SURFACE_INPUT_KEY_ENTER:
			return BLINK_STANDALONE_KEY_ENTER;
		case HTML_SURFACE_INPUT_KEY_DELETE:
			return BLINK_STANDALONE_KEY_DELETE;
		default:
			return BLINK_STANDALONE_KEY_UNKNOWN;
	}
}

static int color_channel_to_byte(float p_channel) {
	return CLAMP(Math::round(p_channel * 255.0f), 0, 255);
}

static String color_to_css_rgba(const Color &p_color) {
	return vformat("rgba(%d, %d, %d, %s)",
			color_channel_to_byte(p_color.r),
			color_channel_to_byte(p_color.g),
			color_channel_to_byte(p_color.b),
			String::num_real(CLAMP(p_color.a, 0.0f, 1.0f), false));
}

static String escape_style_element_text(const String &p_css) {
	return p_css.replace("</style", "<\\/style");
}

static String build_document_style_block(const Color &p_background_color, const String &p_css) {
	String block;
	block += "<style data-godot-background=\"true\">:where(html), :where(body) { background-color: " + color_to_css_rgba(p_background_color) + "; }</style>\n";
	if (!p_css.strip_edges().is_empty()) {
		block += "<style data-godot-css=\"true\">\n";
		block += escape_style_element_text(p_css);
		block += "\n</style>\n";
	}
	return block;
}

static String inject_document_styles(const String &p_html, const Color &p_background_color, const String &p_css) {
	const String style = build_document_style_block(p_background_color, p_css);
	const String lower_html = p_html.to_lower();
	const int head_close = lower_html.find("</head>");
	if (head_close >= 0) {
		return p_html.substr(0, head_close) + "\n" + style + p_html.substr(head_close);
	}

	const int head_start = lower_html.find("<head");
	if (head_start >= 0) {
		const int head_end = p_html.find(">", head_start);
		if (head_end >= 0) {
			return p_html.substr(0, head_end + 1) + "\n" + style + p_html.substr(head_end + 1);
		}
	}

	if (lower_html.begins_with("<!doctype")) {
		const int doctype_end = p_html.find(">");
		if (doctype_end >= 0) {
			return p_html.substr(0, doctype_end + 1) + "\n" + style + p_html.substr(doctype_end + 1);
		}
	}

	return style + p_html;
}

static bool is_godot_local_path(const String &p_path) {
	return p_path.begins_with("res://") || p_path.begins_with("user://");
}

static String to_external_resource_path(const String &p_path) {
	if (p_path.is_empty()) {
		return String();
	}

	if (is_godot_local_path(p_path)) {
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		if (project_settings == nullptr) {
			return String();
		}
		return project_settings->globalize_path(p_path).simplify_path();
	}

	return p_path.simplify_path();
}

static void blink_standalone_hit_to_element_hit(const blink_standalone_hit_metadata_t &p_hit, HTMLElementHit &r_hit) {
	r_hit = HTMLElementHit();
	r_hit.element_id = StringName(blink_standalone_string_to_godot(p_hit.element_id));
	r_hit.tag_name = StringName(blink_standalone_string_to_godot(p_hit.tag_name));
	r_hit.bounds = blink_standalone_rect_to_rect2i(p_hit.bounds);
	r_hit.disabled = p_hit.disabled != 0;
	r_hit.editable = p_hit.editable != 0;
	r_hit.checked = p_hit.checked != 0;
	r_hit.focused = p_hit.focused != 0;

	const String action = blink_standalone_string_to_godot(p_hit.data_godot_action);
	if (!action.is_empty()) {
		HTMLElementAttribute attribute;
		attribute.name = SNAME("data-godot-action");
		attribute.value = action;
		r_hit.attributes.push_back(attribute);
	}
}

static Error blink_standalone_status_to_error(blink_standalone_status_code_t p_status) {
	return p_status == BLINK_STANDALONE_STATUS_OK ? OK : FAILED;
}

static void blink_standalone_form_control_to_state(const blink_standalone_form_control_state_t &p_state, HTMLFormControlState &r_state) {
	r_state = HTMLFormControlState();
	r_state.element_id = StringName(blink_standalone_string_to_godot(p_state.element_id));
	r_state.tag_name = StringName(blink_standalone_string_to_godot(p_state.tag_name));
	r_state.value = blink_standalone_string_to_godot(p_state.value);
	r_state.checked = p_state.checked != 0;
	r_state.focused = p_state.focused != 0;
	r_state.selection_offsets_present = p_state.selection_offsets_present != 0;
	r_state.selection_start = p_state.selection_start;
	r_state.selection_end = p_state.selection_end;
}

bool HTMLSurfaceExternalCApiBackend::_ensure_renderer() {
	if (renderer != nullptr) {
		return true;
	}

	blink_standalone_renderer_config_t config = {};
	config.width = size.x;
	config.height = size.y;
	config.device_scale_factor = device_scale_factor;
	config.no_script_profile = 1;

	blink_standalone_status_code_t status = blink_standalone_renderer_create(&config, &renderer);
	if (status != BLINK_STANDALONE_STATUS_OK || renderer == nullptr) {
		ERR_PRINT("Could not create the external HTML/CSS renderer.");
		return false;
	}

	document_dirty = true;
	viewport_dirty = false;
	return true;
}

String HTMLSurfaceExternalCApiBackend::_load_document_html() const {
	if (document.is_null()) {
		return String();
	}

	String css;
	if (!_load_document_css(css)) {
		return String();
	}

	String html = document->get_html();
	if (html.is_empty()) {
		const String html_file = document->get_html_file();
		if (html_file.is_empty()) {
			return String();
		}

		HTMLAssetResource asset;
		String error;
		Error err = HTMLGodotAssetProvider::load_asset(document, html_file, asset, &error);
		if (err != OK) {
			ERR_PRINT(error);
			return String();
		}
		if (asset.bytes.is_empty()) {
			return String();
		}

		html = String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
	}

	return inject_document_styles(html, document->get_background_color(), css);
}

bool HTMLSurfaceExternalCApiBackend::_load_document_css(String &r_css) const {
	r_css = String();
	if (document.is_null()) {
		return true;
	}

	const PackedStringArray css_files = document->get_css_files();
	for (const String &css_file : css_files) {
		HTMLAssetResource asset;
		String error;
		Error err = HTMLGodotAssetProvider::load_asset(document, css_file, asset, &error);
		if (err != OK) {
			ERR_PRINT(error);
			return false;
		}
		if (asset.bytes.is_empty()) {
			continue;
		}

		r_css += vformat("\n/* %s */\n", css_file);
		r_css += String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
		r_css += "\n";
	}

	const String inline_css = document->get_css();
	if (!inline_css.is_empty()) {
		r_css += "\n/* inline HTMLDocument.css */\n";
		r_css += inline_css;
		r_css += "\n";
	}

	return true;
}

String HTMLSurfaceExternalCApiBackend::_get_document_resource_root() const {
	if (document.is_null()) {
		return String();
	}

	return to_external_resource_path(document->get_resource_root());
}

String HTMLSurfaceExternalCApiBackend::_get_document_base_path() const {
	if (document.is_valid()) {
		const String html_file = document->get_html_file();
		if (!html_file.is_empty() && is_godot_local_path(html_file)) {
			return to_external_resource_path(html_file.get_base_dir());
		}

		const String resource_root = document->get_resource_root();
		if (!resource_root.is_empty()) {
			return to_external_resource_path(resource_root);
		}
	}

	return String();
}

bool HTMLSurfaceExternalCApiBackend::_sync_document() {
	if (!document_dirty) {
		return true;
	}
	if (!_ensure_renderer()) {
		return false;
	}

	if (document.is_valid() && !document->is_source_valid()) {
		PackedStringArray errors = document->get_source_errors();
		for (const String &error : errors) {
			ERR_PRINT(vformat("External HTML/CSS renderer skipped invalid document source: %s", error));
		}
		return false;
	}

	const String html = _load_document_html();
	if (html.strip_edges().is_empty()) {
		document_dirty = false;
		return false;
	}

	const String resource_root = _get_document_resource_root();
	const String base_path = _get_document_base_path();

	CharString html_utf8 = html.utf8();
	CharString root_utf8 = resource_root.utf8();
	CharString base_path_utf8 = base_path.utf8();

	blink_standalone_status_code_t status = blink_standalone_renderer_set_document_html(renderer, html_utf8.ptr(), root_utf8.ptr(), base_path_utf8.ptr());
	if (status != BLINK_STANDALONE_STATUS_OK) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		if (last_error != nullptr) {
			ERR_PRINT(vformat("External HTML/CSS renderer rejected the document: %s", String::utf8(last_error)));
		}
		return false;
	}

	document_dirty = false;
	return true;
}

bool HTMLSurfaceExternalCApiBackend::_sync_viewport() {
	if (!viewport_dirty) {
		return true;
	}
	if (!_ensure_renderer()) {
		return false;
	}

	blink_standalone_status_code_t status = blink_standalone_renderer_set_viewport(renderer, size.x, size.y, device_scale_factor);
	if (status != BLINK_STANDALONE_STATUS_OK) {
		ERR_PRINT("External HTML/CSS renderer rejected the viewport.");
		return false;
	}

	viewport_dirty = false;
	return true;
}

bool HTMLSurfaceExternalCApiBackend::_prepare_for_input() {
	return _ensure_renderer() && _sync_viewport() && _sync_document();
}

Error HTMLSurfaceExternalCApiBackend::_status_to_error(blink_standalone_status_code_t p_status, const char *p_operation) const {
	if (p_status == BLINK_STANDALONE_STATUS_OK) {
		return OK;
	}

	if (p_status == BLINK_STANDALONE_STATUS_NO_SCRIPT_REJECTED) {
		return ERR_INVALID_DATA;
	}

	const char *last_error = renderer != nullptr ? blink_standalone_renderer_last_error(renderer) : nullptr;
	if (last_error != nullptr && last_error[0] != '\0') {
		ERR_PRINT(vformat("External HTML/CSS renderer %s failed: %s", p_operation, String::utf8(last_error)));
	} else {
		ERR_PRINT(vformat("External HTML/CSS renderer %s failed with status %d.", p_operation, (int)p_status));
	}

	switch (p_status) {
		case BLINK_STANDALONE_STATUS_INVALID_ARGUMENT:
			return ERR_INVALID_PARAMETER;
		case BLINK_STANDALONE_STATUS_INITIALIZATION_FAILED:
			return ERR_CANT_CREATE;
		case BLINK_STANDALONE_STATUS_RENDER_FAILED:
		default:
			return FAILED;
	}
}

bool HTMLSurfaceExternalCApiBackend::_copy_latest_output() {
	blink_standalone_frame_output_t output = {};
	blink_standalone_status_code_t status = blink_standalone_renderer_get_latest_output(renderer, &output);
	if (status != BLINK_STANDALONE_STATUS_OK) {
		return false;
	}

	if (output.pixels == nullptr || output.pixel_count == 0 || output.width <= 0 || output.height <= 0 || output.stride <= 0 || output.pixel_format == BLINK_STANDALONE_PIXEL_FORMAT_NONE) {
		blink_standalone_renderer_release_latest_output(renderer);
		return false;
	}

	HTMLCPUFrame frame;
	frame.size = Size2i(output.width, output.height);
	frame.stride = output.stride;
	frame.pixel_format = blink_standalone_pixel_format_to_frame_format(output.pixel_format);
	frame.premultiplied_alpha = output.premultiplied_alpha != 0;
	frame.pixels.resize(output.pixel_count);
	const size_t expected_pixel_count = (size_t)output.stride * (size_t)output.height;
	if (output.pixel_count < expected_pixel_count) {
		blink_standalone_renderer_release_latest_output(renderer);
		return false;
	}

	uint8_t *frame_pixels = frame.pixels.ptrw();
	for (int y = 0; y < output.height; y++) {
		const uint8_t *src_row = output.pixels + (int64_t)output.stride * (output.height - 1 - y);
		uint8_t *dst_row = frame_pixels + (int64_t)output.stride * y;
		memcpy(dst_row, src_row, output.stride);
	}
	frame.damage.full_frame = output.dirty_rect_count == 0;
	for (size_t i = 0; i < output.dirty_rect_count; i++) {
		frame.damage.rects.push_back(blink_standalone_rect_to_rect2i(output.dirty_rects[i]));
	}

	blink_standalone_renderer_release_latest_output(renderer);
	return submit_cpu_frame(frame) == OK;
}

void HTMLSurfaceExternalCApiBackend::_read_frame_metadata() {
	frame_metadata = HTMLFrameMetadata();

	const size_t count = blink_standalone_renderer_hit_metadata_count(renderer);
	for (size_t i = 0; i < count; i++) {
		blink_standalone_hit_metadata_t hit = {};
		if (blink_standalone_renderer_get_hit_metadata(renderer, i, &hit) != BLINK_STANDALONE_STATUS_OK) {
			continue;
		}

		HTMLElementHit element_hit;
		blink_standalone_hit_to_element_hit(hit, element_hit);
		frame_metadata.hits.push_back(element_hit);
	}
}

void HTMLSurfaceExternalCApiBackend::_clear_output() {
	frame_metadata = HTMLFrameMetadata();
	clear_to_background();
}

void HTMLSurfaceExternalCApiBackend::mark_document_dirty() {
	document_dirty = true;
	frame_metadata = HTMLFrameMetadata();
}

void HTMLSurfaceExternalCApiBackend::set_size(const Size2i &p_size) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	HTMLSurfaceCPUBackend::set_size(new_size);
	// Viewport changes must preserve the live document, focus, form, scroll, and
	// pointer state. Only sync the renderer viewport; do not re-submit HTML.
	viewport_dirty = true;
}

void HTMLSurfaceExternalCApiBackend::set_device_scale_factor(float p_device_scale_factor) {
	float new_device_scale_factor = p_device_scale_factor;
	if (!Math::is_finite(new_device_scale_factor) || new_device_scale_factor <= 0.0f) {
		new_device_scale_factor = 1.0f;
	}
	new_device_scale_factor = CLAMP(new_device_scale_factor, 0.01f, 8.0f);
	if (Math::is_equal_approx(device_scale_factor, new_device_scale_factor)) {
		return;
	}

	device_scale_factor = new_device_scale_factor;
	viewport_dirty = true;
}

void HTMLSurfaceExternalCApiBackend::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	document = p_document;
	document_dirty = true;
	frame_metadata = HTMLFrameMetadata();
}

void HTMLSurfaceExternalCApiBackend::render_placeholder(const String &p_marker) {
	if (document.is_null()) {
		_clear_output();
		return;
	}

	if (!_ensure_renderer() || !_sync_viewport() || !_sync_document()) {
		_clear_output();
		return;
	}

	if (blink_standalone_renderer_advance_frame(renderer, 0.0) != BLINK_STANDALONE_STATUS_OK) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		if (last_error != nullptr && last_error[0] != '\0') {
			ERR_PRINT(vformat("External HTML/CSS renderer could not produce a frame: %s", String::utf8(last_error)));
		} else {
			ERR_PRINT("External HTML/CSS renderer could not produce a frame.");
		}
		_clear_output();
		return;
	}

	if (!_copy_latest_output()) {
		ERR_PRINT("External HTML/CSS renderer produced no usable frame output.");
		_clear_output();
		return;
	}

	_read_frame_metadata();
}

void HTMLSurfaceExternalCApiBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	r_metadata = frame_metadata;
}

Error HTMLSurfaceExternalCApiBackend::mouse_move(const Point2 &p_position, int p_modifiers) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_mouse_move(renderer, p_position.x, p_position.y, p_modifiers));
}

Error HTMLSurfaceExternalCApiBackend::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	const blink_standalone_mouse_button_t button = html_surface_mouse_button_to_blink_standalone(p_button);
	if (button == BLINK_STANDALONE_MOUSE_BUTTON_NONE) {
		return ERR_INVALID_PARAMETER;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_mouse_down(renderer, p_position.x, p_position.y, button, p_modifiers, p_click_count));
}

Error HTMLSurfaceExternalCApiBackend::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	const blink_standalone_mouse_button_t button = html_surface_mouse_button_to_blink_standalone(p_button);
	if (button == BLINK_STANDALONE_MOUSE_BUTTON_NONE) {
		return ERR_INVALID_PARAMETER;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_mouse_up(renderer, p_position.x, p_position.y, button, p_modifiers, p_click_count));
}

Error HTMLSurfaceExternalCApiBackend::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_wheel(renderer, p_position.x, p_position.y, p_delta.x, p_delta.y));
}

Error HTMLSurfaceExternalCApiBackend::key_down(HTMLSurfaceInputKey p_key, int p_modifiers) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	const blink_standalone_key_t key = html_surface_input_key_to_blink_standalone(p_key);
	if (key == BLINK_STANDALONE_KEY_UNKNOWN) {
		return ERR_INVALID_PARAMETER;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_key_down(renderer, key, p_modifiers));
}

Error HTMLSurfaceExternalCApiBackend::key_up(HTMLSurfaceInputKey p_key, int p_modifiers) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	const blink_standalone_key_t key = html_surface_input_key_to_blink_standalone(p_key);
	if (key == BLINK_STANDALONE_KEY_UNKNOWN) {
		return ERR_INVALID_PARAMETER;
	}

	return blink_standalone_status_to_error(blink_standalone_renderer_key_up(renderer, key, p_modifiers));
}

Error HTMLSurfaceExternalCApiBackend::text_input(const String &p_text) {
	if (p_text.is_empty()) {
		return OK;
	}
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	const CharString text_utf8 = p_text.utf8();
	return blink_standalone_status_to_error(blink_standalone_renderer_text_input(renderer, text_utf8.ptr()));
}

Error HTMLSurfaceExternalCApiBackend::set_element_text(const StringName &p_id, const String &p_text) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	CharString id_utf8 = String(p_id).utf8();
	CharString text_utf8 = p_text.utf8();
	return _status_to_error(blink_standalone_renderer_set_element_text(renderer, id_utf8.ptr(), text_utf8.ptr()), "set_element_text");
}

Error HTMLSurfaceExternalCApiBackend::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	CharString id_utf8 = String(p_id).utf8();
	CharString name_utf8 = String(p_name).utf8();
	CharString value_utf8 = p_value.utf8();
	return _status_to_error(blink_standalone_renderer_set_element_attribute(renderer, id_utf8.ptr(), name_utf8.ptr(), value_utf8.ptr()), "set_element_attribute");
}

Error HTMLSurfaceExternalCApiBackend::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	CharString id_utf8 = String(p_id).utf8();
	CharString name_utf8 = String(p_name).utf8();
	return _status_to_error(blink_standalone_renderer_remove_element_attribute(renderer, id_utf8.ptr(), name_utf8.ptr()), "remove_element_attribute");
}

Error HTMLSurfaceExternalCApiBackend::set_element_style(const StringName &p_id, const String &p_css_text) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	CharString id_utf8 = String(p_id).utf8();
	CharString css_utf8 = p_css_text.utf8();
	return _status_to_error(blink_standalone_renderer_set_element_style(renderer, id_utf8.ptr(), css_utf8.ptr()), "set_element_style");
}

Error HTMLSurfaceExternalCApiBackend::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	if (!_prepare_for_input()) {
		return ERR_UNAVAILABLE;
	}

	CharString id_utf8 = String(p_style_id).utf8();
	CharString css_utf8 = p_css_text.utf8();
	return _status_to_error(blink_standalone_renderer_replace_stylesheet_text(renderer, id_utf8.ptr(), css_utf8.ptr()), "replace_stylesheet_text");
}

bool HTMLSurfaceExternalCApiBackend::get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) {
	if (!_prepare_for_input()) {
		return false;
	}

	CharString id_utf8 = String(p_id).utf8();
	blink_standalone_form_control_state_t state = {};
	if (blink_standalone_renderer_get_form_control_state_by_id(renderer, id_utf8.ptr(), &state) != BLINK_STANDALONE_STATUS_OK) {
		return false;
	}

	blink_standalone_form_control_to_state(state, r_state);
	return true;
}

bool HTMLSurfaceExternalCApiBackend::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	if (renderer == nullptr) {
		return false;
	}

	blink_standalone_hit_metadata_t hit = {};
	if (blink_standalone_renderer_hit_test(renderer, p_position.x, p_position.y, &hit) != BLINK_STANDALONE_STATUS_OK) {
		return false;
	}

	blink_standalone_hit_to_element_hit(hit, r_hit);
	return true;
}

HTMLSurfaceExternalCApiBackend::~HTMLSurfaceExternalCApiBackend() {
	if (renderer != nullptr) {
		blink_standalone_renderer_destroy(renderer);
		renderer = nullptr;
	}
}
