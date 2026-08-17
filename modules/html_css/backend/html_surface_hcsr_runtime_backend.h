/**************************************************************************/
/*  html_surface_hcsr_runtime_backend.h                                   */
/**************************************************************************/

#pragma once

#include "html_surface_backend.h"

#include "core/os/mutex.h"

#include "hcsr_runtime.h"

class HTMLSurfaceHCSRRuntimeBackend : public HTMLSurfaceBackend {
	// Render-thread helpers are public only so the static callback/free-function
	// implementation can use the opaque state without granting it to callers.
public:
	struct RuntimeState;
	static void _step_on_render_thread_callback(uint64_t p_state_ptr);
	static void _activate_frame_cutoff_on_render_thread_callback(uint64_t p_state_ptr);
	static void _destroy_state_on_render_thread_callback(uint64_t p_state_ptr);

private:
	RuntimeState *state = nullptr;
	Ref<HTMLTexture2D> texture;
	Ref<HTMLDocument> document;

	void _schedule_work();
	void _queue_document_snapshot();
	Error _queue_mutation(int p_kind, const StringName &p_id, const StringName &p_name, const String &p_value);
public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_physical_size(const Size2i &p_physical_size) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void set_transparent_background(bool p_transparent_background) override;
	virtual void set_background_color(const Color &p_background_color) override;
	virtual void set_placeholder_background(const Color &p_color) override;
	virtual Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool poll_pending_output(bool *r_waiting_for_completion = nullptr) override;
	virtual HTMLPendingOutputState consume_pending_output_state() override;
	virtual bool has_pending_output() const override;
	virtual bool has_pending_frame_request() const override;
	virtual uint64_t get_last_queued_frame_generation() const override;
	virtual uint64_t get_active_frame_generation() const override;
	virtual bool uses_generation_bound_input() const override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual Error submit_cpu_frame(const HTMLCPUFrame &p_frame) override;
	virtual Error apply_element_mutations(const Array &p_mutations) override;
	virtual Error set_element_text(const StringName &p_id, const String &p_text) override;
	virtual Error set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) override;
	virtual Error set_element_style(const StringName &p_id, const String &p_css_text) override;
	virtual Error set_element_inner_html(const StringName &p_id, const String &p_html_fragment) override;
	virtual Error set_form_control_checked(const StringName &p_id, bool p_checked) override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual Ref<Texture2D> get_texture() const override;
	virtual Ref<HTMLTexture2D> get_html_texture() const override;

	HTMLSurfaceHCSRRuntimeBackend();
	~HTMLSurfaceHCSRRuntimeBackend();
};
