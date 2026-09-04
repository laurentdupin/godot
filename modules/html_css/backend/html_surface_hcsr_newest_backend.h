/**************************************************************************/
/*  html_surface_hcsr_newest_backend.h                                    */
/**************************************************************************/

#pragma once

#include "html_surface_backend.h"

#include "core/os/mutex.h"

#include "hcsr_render_d3d12.h"
#include "hcsr_render_vulkan.h"

enum HTMLSurfaceHCSRNewestRenderer {
	HTML_SURFACE_HCSR_NEWEST_CPU,
	HTML_SURFACE_HCSR_NEWEST_D3D12,
	HTML_SURFACE_HCSR_NEWEST_VULKAN,
};

class HTMLSurfaceHCSRNewestBackend : public HTMLSurfaceBackend {
public:
	struct State;
	static void _render_on_render_thread(uint64_t p_state_pointer);
	static void _cancel_gpu_submission_on_render_thread(uint64_t p_state_pointer);

private:
	State *state = nullptr;
	Ref<HTMLTexture2D> texture;
	Ref<HTMLDocument> document;

	Error _rebuild_scene();
	Error _queue_input(const hcsr_input_event_t &p_event, const CharString &p_payload = CharString());
	Error _apply_mutation(const hcsr_mutation_t &p_mutation);

public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_physical_size(const Size2i &p_physical_size) override;
	virtual void set_viewport_configuration(const Size2i &p_size, float p_device_scale_factor, const Size2i &p_physical_size) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void set_transparent_background(bool p_transparent_background) override;
	virtual void set_background_color(const Color &p_background_color) override;
	virtual void set_placeholder_background(const Color &p_color) override;
	virtual Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool poll_pending_output(bool *r_waiting_for_completion = nullptr) override;
	virtual bool has_pending_output() const override;
	virtual bool has_pending_frame_request() const override;
	virtual uint64_t get_last_queued_frame_generation() const override;
	virtual uint64_t get_active_frame_generation() const override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual Error submit_cpu_frame(const HTMLCPUFrame &p_frame) override;
	virtual Error mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed) override;
	virtual Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error pointer_cancel(const Point2 &p_position, int p_pointer_id) override;
	virtual Error notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) override;
	virtual Error begin_scrollbar_interaction(const Point2 &p_position, double p_event_time_seconds, bool &r_consumed) override;
	virtual Error update_scrollbar_interaction(const Point2 &p_position, bool &r_consumed) override;
	virtual Error end_scrollbar_interaction(bool &r_consumed) override;
	virtual Error wheel(const Point2 &p_position, const Vector2 &p_delta) override;
	virtual Error key_down(HTMLSurfaceInputKey p_key, int p_modifiers) override;
	virtual Error key_up(HTMLSurfaceInputKey p_key, int p_modifiers) override;
	virtual Error text_input(const String &p_text) override;
	virtual Error apply_element_mutations(const Array &p_mutations) override;
	virtual Error set_element_text(const StringName &p_id, const String &p_text) override;
	virtual Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment) override;
	virtual Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) override;
	virtual Error remove_element_attribute(const StringName &p_id, const StringName &p_name) override;
	virtual Error set_element_style(const StringName &p_id, const String &p_css_text) override;
	virtual Error set_form_control_value(const StringName &p_id, const String &p_value) override;
	virtual Error set_form_control_checked(const StringName &p_id, bool p_checked) override;
	virtual bool get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) override;
	virtual bool hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual Ref<Texture2D> get_texture() const override;
	virtual Ref<HTMLTexture2D> get_html_texture() const override;

	HTMLSurfaceHCSRNewestBackend(HTMLSurfaceHCSRNewestRenderer p_renderer);
	~HTMLSurfaceHCSRNewestBackend();
};
