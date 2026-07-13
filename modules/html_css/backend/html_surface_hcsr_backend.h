/**************************************************************************/
/*  html_surface_hcsr_backend.h                                          */
/**************************************************************************/

#pragma once

#include "html_surface_cpu_backend.h"

#include "hcsr_renderer.h"

class HTMLSurfaceHCSRBackend : public HTMLSurfaceCPUBackend {
	hcsr_renderer_t *renderer = nullptr;
	hcsr_render_backend_t render_backend = HCSR_RENDER_BACKEND_CPU;
	Ref<HTMLDocument> document;
	Ref<HTMLTexture2D> gpu_texture;
	RID gpu_texture_rid;
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
	String terminal_failure_reason;

	bool _ensure_renderer();
	bool _sync_viewport();
	bool _sync_document();
	bool _load_document_source(String &r_html, String &r_document_path, String &r_asset_root) const;
	bool _render_frame();
	bool _configure_d3d12_device();
	void _configure_d3d12_device_on_render_thread();
	bool _configure_vulkan_device();
	void _configure_vulkan_device_on_render_thread();
	bool _render_gpu_frame();
	void _render_gpu_frame_on_render_thread();
	void _ensure_gpu_texture_imported_on_render_thread();
	void _detach_gpu_texture_import();
	void _detach_gpu_texture_import_on_render_thread();
	void _destroy_renderer_on_render_thread();
	static void _configure_d3d12_device_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _configure_vulkan_device_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _render_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _detach_gpu_texture_import_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _destroy_renderer_on_render_thread_callback(uint64_t p_backend_ptr);
	void _record_error(const String &p_context);
	Error _set_input();

public:
	virtual void mark_document_dirty() override;
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_document(const Ref<HTMLDocument> &p_document) override;
	virtual void set_background_color(const Color &p_background_color) override;
	virtual Error update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual Error mouse_move(const Point2 &p_position, int p_modifiers) override;
	virtual Error mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) override;
	virtual Error wheel(const Point2 &p_position, const Vector2 &p_delta) override;
	virtual Ref<Texture2D> get_texture() const override;
	virtual Ref<HTMLTexture2D> get_html_texture() const override;

	HTMLSurfaceHCSRBackend(hcsr_render_backend_t p_render_backend = HCSR_RENDER_BACKEND_CPU);
	~HTMLSurfaceHCSRBackend();
};
