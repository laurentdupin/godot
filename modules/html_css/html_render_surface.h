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
#include "backend/html_surface_backend_factory.h"
#include "bridge/html_asset_provider.h"
#include "html_document.h"

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

class HTMLRenderSurface : public RefCounted {
	Ref<HTMLDocument> document;
	HTMLSurfaceBackend *backend = nullptr;
	Size2i size = Size2i(512, 512);
	Size2i physical_size = Size2i(512, 512);
	float device_scale_factor = 1.0f;
	Color placeholder_background = Color(0.08, 0.09, 0.1, 1.0);
	bool backdrop_filter_enabled = false;
	String marker = "HTML";
	HTMLFrameMetadata frame_metadata;
	HTMLGPUBackdropFrame gpu_backdrop_frame;
	HTMLSurfaceBackendPreference backend_preference = HTML_SURFACE_BACKEND_AUTO;
	Callable changed_callback;
	Callable frame_queued_callback;
	Callable frame_activated_callback;
	uint64_t notified_queued_frame_generation = 0;
	uint64_t notified_active_frame_generation = 0;
	struct PresentationOutputBinding {
		Size2i size;
		bool mipmaps = false;
		uint64_t backend_output_id = 0;
	};
	HashMap<uint64_t, PresentationOutputBinding> presentation_outputs;
	uint64_t next_presentation_output_id = 1;

	void _ensure_backend();
	void _sync_backend_state();
	void _sync_backend_presentation_outputs();
	void _detach_backend_presentation_outputs();
	bool _fallback_auto_gpu_to_cpu(const String &p_reason);
	void _document_changed();
	void _notify_changed() const;
	void _notify_frame_state_changes();
	void _reset_frame_state_notifications();

public:
	void set_document(const Ref<HTMLDocument> &p_document);
	Ref<HTMLDocument> get_document() const;

	bool set_size(const Size2i &p_size);
	Size2i get_size() const;
	bool set_device_scale_factor(float p_device_scale_factor);
	float get_device_scale_factor() const;
	bool set_viewport(const Size2i &p_size, float p_device_scale_factor, const Size2i &p_physical_size, bool p_render = true);
	bool set_viewport(const Size2i &p_size, float p_device_scale_factor, bool p_render = true);

	void set_placeholder_background(const Color &p_color);
	void set_backdrop_filter_enabled(bool p_enabled);
	bool is_backdrop_filter_enabled() const;

	void set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference);
	HTMLSurfaceBackendPreference get_backend_preference() const;

	void set_changed_callback(const Callable &p_callback);
	void set_frame_queued_callback(const Callable &p_callback);
	void set_frame_activated_callback(const Callable &p_callback);
	uint64_t get_last_queued_frame_generation() const;
	uint64_t get_active_frame_generation() const;
	bool uses_generation_bound_input() const;
	Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame = nullptr);
	void render_now(const String &p_marker);
	bool poll_pending_output(bool *r_waiting_for_completion = nullptr);
	HTMLPendingOutputState consume_pending_output_state();
	void schedule_retirement_service();
	bool has_pending_output() const;
	bool has_pending_frame_request() const;
	bool has_terminal_render_failure() const;
	String get_terminal_render_failure_reason() const;
	bool is_begin_frame_requested() const;
	Error submit_cpu_frame(const HTMLCPUFrame &p_frame, const HTMLFrameMetadata &p_metadata = HTMLFrameMetadata());
	const HTMLFrameMetadata &get_frame_metadata() const;
	const HTMLGPUBackdropFrame &get_gpu_backdrop_frame() const;
	const Vector<HTMLBackdropFilterRegion> &get_backdrop_filter_regions() const;
	const HTMLElementHit *find_hit_at(const Point2i &p_position) const;
	bool hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const;
	Error mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed);
	Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count);
	Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count);
	void begin_host_input_transaction();
	void end_host_input_transaction();
	Error pointer_cancel(const Point2 &p_position, int p_pointer_id = 1);
	Error notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction = true, int p_pointer_id = 1);
	bool is_pointer_cancel_deferred() const;
	Error begin_scrollbar_interaction(const Point2 &p_position, double p_event_time_seconds, bool &r_consumed);
	Error update_scrollbar_interaction(const Point2 &p_position, bool &r_consumed);
	Error end_scrollbar_interaction(bool &r_consumed);
	bool poll_pointer_event(HTMLPointerEvent &r_event);
	Error wheel(const Point2 &p_position, const Vector2 &p_delta);
	Error key_down(HTMLSurfaceInputKey p_key, int p_modifiers);
	Error key_up(HTMLSurfaceInputKey p_key, int p_modifiers);
	Error text_input(const String &p_text);
	Error set_element_text(const StringName &p_id, const String &p_text);
	Error apply_element_mutations(const Array &p_mutations);
	Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment);
	Error set_body_inner_html(const String &p_html_fragment);
	Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value);
	Error remove_element_attribute(const StringName &p_id, const StringName &p_name);
	Error set_element_style(const StringName &p_id, const String &p_css_text);
	Error replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text);
	Error scroll_element_into_view(const StringName &p_id, const StringName &p_block_alignment);
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
	uint64_t create_presentation_output(const Size2i &p_size, bool p_mipmaps);
	Error resize_presentation_output(uint64_t p_output_id, const Size2i &p_size);
	void destroy_presentation_output(uint64_t p_output_id);
	Ref<Texture2D> get_presentation_output_texture(uint64_t p_output_id) const;
	uint64_t get_presentation_output_generation(uint64_t p_output_id) const;

	HTMLRenderSurface();
	~HTMLRenderSurface();
};
