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

#include "core/math/math_funcs.h"

static Rect2i hcsr_rect_to_rect2i(const hcsr_rect_t &p_rect) {
	const int x = Math::floor(p_rect.x);
	const int y = Math::floor(p_rect.y);
	const int right = Math::ceil(p_rect.x + p_rect.width);
	const int bottom = Math::ceil(p_rect.y + p_rect.height);
	return Rect2i(x, y, MAX(0, right - x), MAX(0, bottom - y));
}

static String hcsr_string_to_godot(const char *p_text) {
	return p_text != nullptr ? String::utf8(p_text) : String();
}

static HTMLFramePixelFormat hcsr_pixel_format_to_frame_format(hcsr_pixel_format_t p_format) {
	return p_format == HCSR_PIXEL_FORMAT_BGRA8 ? HTML_FRAME_PIXEL_FORMAT_BGRA8 : HTML_FRAME_PIXEL_FORMAT_RGBA8;
}

bool HTMLSurfaceExternalCApiBackend::_ensure_renderer() {
	if (renderer != nullptr) {
		return true;
	}

	hcsr_renderer_config_t config = {};
	config.width = size.x;
	config.height = size.y;
	config.device_scale_factor = 1.0f;
	config.no_script_profile = 1;

	hcsr_status_code_t status = hcsr_renderer_create(&config, &renderer);
	if (status != HCSR_STATUS_OK || renderer == nullptr) {
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

	const String inline_html = document->get_html();
	if (!inline_html.is_empty()) {
		return inline_html;
	}

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

	return String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
}

String HTMLSurfaceExternalCApiBackend::_get_document_base_path() const {
	if (document.is_valid()) {
		const String html_file = document->get_html_file();
		if (!html_file.is_empty() && (html_file.begins_with("res://") || html_file.begins_with("user://"))) {
			return html_file.get_base_dir();
		}

		const String resource_root = document->get_resource_root();
		if (!resource_root.is_empty()) {
			return resource_root;
		}
	}

	return "res://";
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
	const String resource_root = document.is_valid() ? document->get_resource_root() : String("res://");
	const String base_path = _get_document_base_path();

	CharString html_utf8 = html.utf8();
	CharString root_utf8 = resource_root.utf8();
	CharString base_path_utf8 = base_path.utf8();

	hcsr_status_code_t status = hcsr_renderer_set_document_html(renderer, html_utf8.ptr(), root_utf8.ptr(), base_path_utf8.ptr());
	if (status != HCSR_STATUS_OK) {
		const char *last_error = hcsr_renderer_last_error(renderer);
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

	hcsr_status_code_t status = hcsr_renderer_set_viewport(renderer, size.x, size.y, 1.0f);
	if (status != HCSR_STATUS_OK) {
		ERR_PRINT("External HTML/CSS renderer rejected the viewport.");
		return false;
	}

	viewport_dirty = false;
	return true;
}

bool HTMLSurfaceExternalCApiBackend::_copy_latest_output() {
	hcsr_frame_output_t output = {};
	hcsr_status_code_t status = hcsr_renderer_get_latest_output(renderer, &output);
	if (status != HCSR_STATUS_OK) {
		return false;
	}

	if (output.pixels == nullptr || output.pixel_count == 0 || output.width <= 0 || output.height <= 0 || output.stride <= 0 || output.pixel_format == HCSR_PIXEL_FORMAT_NONE) {
		hcsr_renderer_release_latest_output(renderer);
		return false;
	}

	HTMLCPUFrame frame;
	frame.size = Size2i(output.width, output.height);
	frame.stride = output.stride;
	frame.pixel_format = hcsr_pixel_format_to_frame_format(output.pixel_format);
	frame.premultiplied_alpha = output.premultiplied_alpha != 0;
	frame.pixels.resize(output.pixel_count);
	memcpy(frame.pixels.ptrw(), output.pixels, output.pixel_count);
	frame.damage.full_frame = output.dirty_rect_count == 0;
	for (size_t i = 0; i < output.dirty_rect_count; i++) {
		frame.damage.rects.push_back(hcsr_rect_to_rect2i(output.dirty_rects[i]));
	}

	hcsr_renderer_release_latest_output(renderer);
	return submit_cpu_frame(frame) == OK;
}

void HTMLSurfaceExternalCApiBackend::_read_frame_metadata() {
	frame_metadata = HTMLFrameMetadata();

	const size_t count = hcsr_renderer_hit_metadata_count(renderer);
	for (size_t i = 0; i < count; i++) {
		hcsr_hit_metadata_t hit = {};
		if (hcsr_renderer_get_hit_metadata(renderer, i, &hit) != HCSR_STATUS_OK) {
			continue;
		}

		HTMLElementHit element_hit;
		element_hit.element_id = StringName(hcsr_string_to_godot(hit.element_id));
		element_hit.tag_name = StringName(hcsr_string_to_godot(hit.tag_name));
		element_hit.bounds = hcsr_rect_to_rect2i(hit.bounds);
		element_hit.disabled = hit.disabled != 0;
		element_hit.editable = hit.editable != 0;
		element_hit.checked = hit.checked != 0;
		element_hit.focused = hit.focused != 0;

		const String action = hcsr_string_to_godot(hit.data_godot_action);
		if (!action.is_empty()) {
			HTMLElementAttribute attribute;
			attribute.name = SNAME("data-godot-action");
			attribute.value = action;
			element_hit.attributes.push_back(attribute);
		}

		frame_metadata.hits.push_back(element_hit);
	}
}

void HTMLSurfaceExternalCApiBackend::_clear_output() {
	frame_metadata = HTMLFrameMetadata();
	clear_to_transparent();
}

void HTMLSurfaceExternalCApiBackend::set_size(const Size2i &p_size) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	HTMLSurfaceCPUBackend::set_size(new_size);
	if (size == new_size) {
		return;
	}
	size = new_size;
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
	if (!_ensure_renderer() || !_sync_viewport() || !_sync_document()) {
		_clear_output();
		return;
	}

	if (hcsr_renderer_advance_frame(renderer, 0.0) != HCSR_STATUS_OK) {
		ERR_PRINT("External HTML/CSS renderer could not produce a frame.");
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

HTMLSurfaceExternalCApiBackend::~HTMLSurfaceExternalCApiBackend() {
	if (renderer != nullptr) {
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
	}
}
