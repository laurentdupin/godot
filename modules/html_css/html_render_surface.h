/**************************************************************************/
/*  html_render_surface.h                                                 */
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

#include "backend/html_surface_backend.h"
#include "bridge/html_asset_provider.h"
#include "html_document.h"

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

enum HTMLSurfaceBackendPreference {
	HTML_SURFACE_BACKEND_AUTO,
	HTML_SURFACE_BACKEND_CPU,
	HTML_SURFACE_BACKEND_GPU_AUTO,
	HTML_SURFACE_BACKEND_VULKAN,
	HTML_SURFACE_BACKEND_D3D12,
};

class HTMLRenderSurface : public RefCounted {
	Ref<HTMLDocument> document;
	HTMLSurfaceBackend *backend = nullptr;
	Size2i size = Size2i(512, 512);
	float device_scale_factor = 1.0f;
	Color placeholder_background = Color(0.08, 0.09, 0.1, 1.0);
	String marker = "HTML";
	HTMLFrameMetadata frame_metadata;
	HTMLSurfaceBackendPreference backend_preference = HTML_SURFACE_BACKEND_AUTO;
	Callable changed_callback;

	void _ensure_backend();
	void _sync_backend_state();
	void _document_changed();
	void _notify_changed() const;

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	bool set_size(const Size2i &p_size);
	Size2i get_size() const;
	bool set_device_scale_factor(float p_device_scale_factor);
	float get_device_scale_factor() const;
	bool set_viewport(const Size2i &p_size, float p_device_scale_factor, bool p_render = true);

	void set_placeholder_background(const Color &p_color);

	void set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference);
	HTMLSurfaceBackendPreference get_backend_preference() const;

	void set_changed_callback(const Callable &p_callback);
	Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame = nullptr);
	void render_now(const String &p_marker);
	bool has_pending_output() const;
	Error submit_cpu_frame(const HTMLCPUFrame &p_frame, const HTMLFrameMetadata &p_metadata = HTMLFrameMetadata());
	const HTMLFrameMetadata &get_frame_metadata() const;
	const Vector<HTMLBackdropFilterRegion> &get_backdrop_filter_regions() const;
	const HTMLElementHit *find_hit_at(const Point2i &p_position) const;
	bool hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const;
	Error mouse_move(const Point2 &p_position, int p_modifiers);
	Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count);
	Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count);
	Error wheel(const Point2 &p_position, const Vector2 &p_delta);
	Error key_down(HTMLSurfaceInputKey p_key, int p_modifiers);
	Error key_up(HTMLSurfaceInputKey p_key, int p_modifiers);
	Error text_input(const String &p_text);
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
	bool get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state);
	bool is_document_source_valid() const;
	Error load_asset(const String &p_uri, HTMLAssetResource &r_asset, String *r_error = nullptr) const;
	Ref<Texture2D> get_texture() const;
	Ref<HTMLTexture2D> get_html_texture() const;

	HTMLRenderSurface();
	~HTMLRenderSurface();
};
