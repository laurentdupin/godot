/**************************************************************************/
/*  html_surface_blink_c_api_backend.h                                    */
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

#include "html_surface_cpu_backend.h"

#include "html_css_renderer/renderer_c_api.h"

#include <memory>
#include <vector>

class HTMLSurfaceExternalCApiBackend : public HTMLSurfaceCPUBackend {
	struct ResourceProviderPayload {
		CharString mime_type;
		CharString cache_key;
		Vector<uint8_t> bytes;
	};

	blink_standalone_renderer_t *renderer = nullptr;
	Ref<HTMLDocument> document;
	HTMLFrameMetadata frame_metadata;
	std::vector<std::unique_ptr<ResourceProviderPayload>> resource_provider_payloads;
	bool document_dirty = true;
	bool viewport_dirty = true;
	float device_scale_factor = 1.0f;

	bool _ensure_renderer();
	bool _install_resource_provider();
	bool _sync_document();
	bool _sync_viewport();
	bool _prepare_for_input();
	bool _copy_latest_output();
	void _read_frame_metadata();
	String _load_document_html() const;
	bool _load_document_css(String &r_css) const;
	String _get_document_resource_root() const;
	String _get_document_base_path() const;
	blink_standalone_resource_status_t _load_resource(const blink_standalone_resource_request_t *p_request, blink_standalone_resource_response_t *r_response);
	void _release_resource(blink_standalone_resource_response_t *p_response);
	Error _status_to_error(blink_standalone_status_code_t p_status, const char *p_operation) const;
	void _clear_output();

	static blink_standalone_resource_status_t _load_resource_callback(void *p_user_data, const blink_standalone_resource_request_t *p_request, blink_standalone_resource_response_t *r_response);
	static void _release_resource_callback(void *p_user_data, blink_standalone_resource_response_t *p_response);

public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual Error mouse_move(const Point2 &p_position, int p_modifiers) override;
	virtual Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error wheel(const Point2 &p_position, const Vector2 &p_delta) override;
	virtual Error key_down(HTMLSurfaceInputKey p_key, int p_modifiers) override;
	virtual Error key_up(HTMLSurfaceInputKey p_key, int p_modifiers) override;
	virtual Error text_input(const String &p_text) override;
	virtual Error set_element_text(const StringName &p_id, const String &p_text) override;
	virtual Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment) override;
	virtual Error set_body_inner_html(const String &p_html_fragment) override;
	virtual Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) override;
	virtual Error remove_element_attribute(const StringName &p_id, const StringName &p_name) override;
	virtual Error set_element_style(const StringName &p_id, const String &p_css_text) override;
	virtual Error replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) override;
	virtual Error set_form_control_value(const StringName &p_id, const String &p_value) override;
	virtual Error set_form_control_checked(const StringName &p_id, bool p_checked) override;
	virtual Error focus_element(const StringName &p_id) override;
	virtual Error blur_focused_element() override;
	virtual Error set_text_selection(const StringName &p_id, int p_start, int p_end) override;
	virtual bool get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) override;
	virtual bool hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const override;

	~HTMLSurfaceExternalCApiBackend();
};
