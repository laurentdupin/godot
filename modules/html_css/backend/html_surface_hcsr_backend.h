/**************************************************************************/
/*  html_surface_hcsr_backend.h                                          */
/**************************************************************************/

#pragma once

#include "html_surface_cpu_backend.h"

#include "hcsr_renderer.h"

class HTMLSurfaceHCSRBackend : public HTMLSurfaceCPUBackend {
	hcsr_renderer_t *renderer = nullptr;
	Ref<HTMLDocument> document;
	float device_scale_factor = 1.0f;
	double timeline_time_seconds = 0.0;
	Point2 pointer_position;
	Vector2i scroll_offset;
	bool primary_button_pressed = false;
	bool document_dirty = true;
	bool viewport_dirty = true;
	bool terminal_failure = false;
	String terminal_failure_reason;

	bool _ensure_renderer();
	bool _sync_viewport();
	bool _sync_document();
	bool _load_document_source(String &r_html, String &r_document_path, String &r_asset_root) const;
	bool _render_frame();
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

	HTMLSurfaceHCSRBackend();
	~HTMLSurfaceHCSRBackend();
};
