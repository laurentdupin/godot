/**************************************************************************/
/*  html_surface_hcsr_backend.h                                          */
/**************************************************************************/

#pragma once

#include "html_surface_cpu_backend.h"

#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"

#include "hcsr_renderer.h"

class HTMLSurfaceHCSRBackend : public HTMLSurfaceCPUBackend {
	hcsr_renderer_t *renderer = nullptr;
	hcsr_render_backend_t render_backend = HCSR_RENDER_BACKEND_CPU;
	Ref<HTMLDocument> document;
	Ref<HTMLTexture2D> gpu_texture;
	HTMLFrameMetadata frame_metadata;
	RID gpu_texture_rid;
	HashMap<uint64_t, RID> gpu_texture_import_cache;
	void *native_gpu_texture = nullptr;
	uint64_t native_gpu_generation = 0;
	Size2i native_gpu_size;
	float device_scale_factor = 1.0f;
	double timeline_time_seconds = 0.0;
	Point2 pointer_position;
	Vector2i scroll_offset;
	bool primary_button_pressed = false;
	bool document_dirty = true;
	bool viewport_dirty = true;
	bool terminal_failure = false;
	bool gpu_device_configured = false;
	bool gpu_render_succeeded = false;
	hcsr_gpu_frame_packet_t *deferred_gpu_packet = nullptr;
	SafeFlag gpu_frame_pending;
	SafeFlag gpu_submission_deferred;
	SafeFlag gpu_submission_retry_pending;
	SafeFlag gpu_follow_up_frame_requested;
	SafeFlag gpu_presentation_poll_pending;
	SafeFlag gpu_completed_presentation_available;
	SafeFlag gpu_presentation_work_pending;
	bool backdrop_filter_enabled = false;
	String terminal_failure_reason;
	String last_reported_error;

	bool _ensure_renderer();
	bool _sync_viewport();
	bool _sync_document();
	bool _load_document_source(String &r_html, String &r_document_path, String &r_asset_root) const;
	bool _load_document_package(PackedByteArray &r_package) const;
	bool _render_frame();
	bool _uses_async_gpu_presentation() const;
	void _read_backdrop_filter_regions();
	bool _configure_d3d12_device();
	void _configure_d3d12_device_on_render_thread();
	bool _configure_vulkan_device();
	void _configure_vulkan_device_on_render_thread();
	bool _configure_metal_device();
	void _configure_metal_device_on_render_thread();
	bool _render_gpu_frame();
	void _render_gpu_frame_on_render_thread(hcsr_gpu_frame_packet_t *p_packet);
	void _retry_deferred_gpu_frame_on_render_thread();
	void _schedule_deferred_gpu_submission();
	void _poll_gpu_presentation_on_render_thread();
	bool _accept_gpu_frame_on_render_thread(const hcsr_gpu_frame_t &p_output);
	void _ensure_gpu_texture_imported_on_render_thread();
	void _detach_gpu_texture_import();
	void _detach_gpu_texture_import_on_render_thread();
	void _destroy_renderer_on_render_thread();
	static void _configure_d3d12_device_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _configure_vulkan_device_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _configure_metal_device_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _render_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr, uint64_t p_packet_ptr);
	static void _retry_deferred_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _poll_gpu_presentation_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _detach_gpu_texture_import_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _destroy_renderer_on_render_thread_callback(uint64_t p_backend_ptr);
	void _record_error(const String &p_context);
	void _update_performance_profile();
	Error _set_input();
	bool _clamp_scroll_offset_to_content(bool &r_changed);
	Error _apply_dom_mutation(hcsr_dom_mutation_operation_kind_t p_operation, hcsr_dom_mutation_target_kind_t p_target_kind, const String &p_target, const String &p_name, const String &p_value, hcsr_dom_mutation_content_kind_t p_content_kind = HCSR_DOM_MUTATION_CONTENT_TEXT);

public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void set_background_color(const Color &p_background_color) override;
	virtual void set_backdrop_filter_enabled(bool p_enabled) override;
	virtual Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool poll_pending_output(bool *r_waiting_for_completion = nullptr) override;
	virtual bool has_pending_output() const override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual Error mouse_move(const Point2 &p_position, int p_modifiers) override;
	virtual Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error pointer_cancel(const Point2 &p_position, int p_pointer_id) override;
	virtual Error notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) override;
	virtual bool poll_pointer_event(HTMLPointerEvent &r_event) override;
	virtual Error wheel(const Point2 &p_position, const Vector2 &p_delta) override;
	virtual bool hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const override;
	virtual Error set_element_text(const StringName &p_id, const String &p_text) override;
	virtual Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment) override;
	virtual Error set_body_inner_html(const String &p_html_fragment) override;
	virtual Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) override;
	virtual Error remove_element_attribute(const StringName &p_id, const StringName &p_name) override;
	virtual Error set_element_style(const StringName &p_id, const String &p_css_text) override;
	virtual Error replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) override;
	virtual Error set_form_control_value(const StringName &p_id, const String &p_value) override;
	virtual Error set_form_control_checked(const StringName &p_id, bool p_checked) override;
	virtual bool get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual Ref<Texture2D> get_texture() const override;
	virtual Ref<HTMLTexture2D> get_html_texture() const override;

	HTMLSurfaceHCSRBackend(hcsr_render_backend_t p_render_backend = HCSR_RENDER_BACKEND_CPU);
	~HTMLSurfaceHCSRBackend();
};
