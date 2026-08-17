/**************************************************************************/
/*  html_render_surface.cpp                                               */
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

#include "html_render_surface.h"

#ifdef HTML_CSS_USE_HCSR
#include "backend/html_surface_hcsr_backend.h"
#elif defined(HTML_CSS_USE_HCSR_RUNTIME)
#include "backend/html_surface_hcsr_runtime_backend.h"
#endif
#include "backend/html_surface_cpu_backend.h"
#include "backend/html_surface_unsupported_backend.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"

static bool html_surface_auto_can_use_gpu_backend() {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
	const String rendering_driver = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name().to_lower() : String();
	return rendering_driver == "d3d12";
#elif defined(HTML_CSS_USE_HCSR)
	const String rendering_driver = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name().to_lower() : String();
	return rendering_driver == "d3d12" || rendering_driver == "vulkan" || rendering_driver == "metal";
#else
	return false;
#endif
}

#ifdef HTML_CSS_USE_HCSR
static hcsr_render_backend_t html_surface_hcsr_backend_for_driver(const String &p_rendering_driver) {
	if (p_rendering_driver == "vulkan") {
		return HCSR_RENDER_BACKEND_VULKAN;
	}
	if (p_rendering_driver == "metal") {
		return HCSR_RENDER_BACKEND_METAL;
	}
	return HCSR_RENDER_BACKEND_D3D12;
}

static int html_surface_hcsr_resolved_backend(HTMLSurfaceBackendPreference p_preference) {
	const String rendering_driver = OS::get_singleton() != nullptr
			? OS::get_singleton()->get_current_rendering_driver_name().to_lower()
			: String();
	if (p_preference == HTML_SURFACE_BACKEND_CPU) {
		return HCSR_RENDER_BACKEND_CPU;
	}
	if (p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) {
		if (rendering_driver == "d3d12" || rendering_driver == "vulkan" || rendering_driver == "metal") {
			return html_surface_hcsr_backend_for_driver(rendering_driver);
		}
		return p_preference == HTML_SURFACE_BACKEND_AUTO ? HCSR_RENDER_BACKEND_CPU : -1;
	}
	if (p_preference == HTML_SURFACE_BACKEND_D3D12 && rendering_driver == "d3d12") {
		return HCSR_RENDER_BACKEND_D3D12;
	}
	if (p_preference == HTML_SURFACE_BACKEND_VULKAN && rendering_driver == "vulkan") {
		return HCSR_RENDER_BACKEND_VULKAN;
	}
	if (p_preference == HTML_SURFACE_BACKEND_METAL && rendering_driver == "metal") {
		return HCSR_RENDER_BACKEND_METAL;
	}
	return -1;
}
#endif

static void html_surface_warn_auto_cpu_fallback(const String &p_reason) {
	WARN_PRINT_ONCE(vformat("HTML/CSS Auto backend is using the CPU/raw renderer instead of a GPU target: %s", p_reason));
}

void HTMLRenderSurface::_ensure_backend() {
	if (backend != nullptr) {
		return;
	}

	if (backend_preference == HTML_SURFACE_BACKEND_AUTO) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = html_surface_auto_can_use_gpu_backend()
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRRuntimeBackend)
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement runtime currently requires Godot's D3D12 rendering driver."));
#elif defined(HTML_CSS_USE_HCSR)
		if (html_surface_auto_can_use_gpu_backend()) {
			const String rendering_driver = OS::get_singleton()->get_current_rendering_driver_name().to_lower();
			backend = memnew(HTMLSurfaceHCSRBackend(html_surface_hcsr_backend_for_driver(rendering_driver)));
		} else {
			html_surface_warn_auto_cpu_fallback("HCSR host-device GPU mode requires Godot's Vulkan, D3D12, or Metal rendering driver");
			backend = memnew(HTMLSurfaceHCSRBackend);
		}
#else
		html_surface_warn_auto_cpu_fallback("no external HTML/CSS renderer provider is compiled in");
		backend = memnew(HTMLSurfaceCPUBackend);
#endif
	} else if (backend_preference == HTML_SURFACE_BACKEND_GPU_AUTO) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = html_surface_auto_can_use_gpu_backend()
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRRuntimeBackend)
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement GPU runtime currently requires Godot's D3D12 rendering driver."));
#elif defined(HTML_CSS_USE_HCSR)
		if (html_surface_auto_can_use_gpu_backend()) {
			const String rendering_driver = OS::get_singleton()->get_current_rendering_driver_name().to_lower();
			backend = memnew(HTMLSurfaceHCSRBackend(html_surface_hcsr_backend_for_driver(rendering_driver)));
		} else {
			backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS GPU backend requested, but HCSR host-device mode requires Godot's Vulkan, D3D12, or Metal rendering driver. Explicit GPU requests do not fall back to CPU output."));
		}
#else
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS GPU backend requested, but HCSR is not compiled in. Explicit GPU requests do not fall back to CPU output."));
#endif
	} else if (backend_preference == HTML_SURFACE_BACKEND_VULKAN) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement Vulkan presentation is not implemented; no legacy fallback is permitted."));
#elif defined(HTML_CSS_USE_HCSR)
		backend = OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name().to_lower() == "vulkan"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_VULKAN))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Vulkan backend requested, but Godot is not running its Vulkan rendering driver."));
#else
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Vulkan GPU backend requested, but HCSR is not compiled in. Explicit GPU requests do not fall back to CPU output."));
#endif
	} else if (backend_preference == HTML_SURFACE_BACKEND_D3D12) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name().to_lower() == "d3d12"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRRuntimeBackend)
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement D3D12 runtime requested, but Godot is not running its D3D12 driver."));
#elif defined(HTML_CSS_USE_HCSR)
		backend = OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name().to_lower() == "d3d12"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_D3D12))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS D3D12 backend requested, but Godot is not running its D3D12 rendering driver."));
#else
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS D3D12 GPU backend requested, but HCSR is not compiled in. Explicit GPU requests do not fall back to CPU output."));
#endif
	} else if (backend_preference == HTML_SURFACE_BACKEND_METAL) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement Metal presentation is not implemented; no legacy fallback is permitted."));
#elif defined(HTML_CSS_USE_HCSR)
		backend = OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name().to_lower() == "metal"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_METAL))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Metal backend requested, but Godot is not running its Metal rendering driver."));
#else
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Metal GPU backend requires the HCSR renderer provider. Explicit GPU requests do not fall back to CPU output."));
#endif
	} else if (backend_preference == HTML_SURFACE_BACKEND_CPU) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
		backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement CPU presentation is not implemented; no legacy fallback is permitted."));
#elif defined(HTML_CSS_USE_HCSR)
		backend = memnew(HTMLSurfaceHCSRBackend);
#else
		backend = memnew(HTMLSurfaceCPUBackend);
#endif
	}

	if (backend != nullptr) {
		_sync_backend_state();
		return;
	}

#ifdef HTML_CSS_USE_HCSR_RUNTIME
	backend = memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS replacement runtime could not resolve a supported backend."));
#else
	backend = memnew(HTMLSurfaceCPUBackend);
#endif
	_sync_backend_state();
}

void HTMLRenderSurface::_sync_backend_state() {
	ERR_FAIL_NULL(backend);
	Color background_color = document.is_valid() ? document->get_background_color() : Color(0, 0, 0, 0);
	backend->set_size(size);
	backend->set_device_scale_factor(device_scale_factor);
	backend->set_physical_size(physical_size);
	backend->set_document(document);
	backend->set_transparent_background(background_color.a < 1.0);
	backend->set_background_color(background_color);
	backend->set_placeholder_background(placeholder_background);
	backend->set_backdrop_filter_enabled(backdrop_filter_enabled);
	_sync_backend_presentation_outputs();
}

void HTMLRenderSurface::_sync_backend_presentation_outputs() {
	ERR_FAIL_NULL(backend);
	for (KeyValue<uint64_t, PresentationOutputBinding> &entry : presentation_outputs) {
		if (entry.value.backend_output_id != 0) {
			continue;
		}
		entry.value.backend_output_id = backend->create_presentation_output(
				entry.value.size,
				entry.value.mipmaps);
	}
}

void HTMLRenderSurface::_detach_backend_presentation_outputs() {
	for (KeyValue<uint64_t, PresentationOutputBinding> &entry : presentation_outputs) {
		entry.value.backend_output_id = 0;
	}
}

bool HTMLRenderSurface::_fallback_auto_gpu_to_cpu(const String &p_reason) {
#ifdef HTML_CSS_USE_HCSR_RUNTIME
	(void)p_reason;
	return false;
#else
	if (backend_preference != HTML_SURFACE_BACKEND_AUTO || backend == nullptr || !backend->has_terminal_render_failure()) {
		return false;
	}

#ifdef HTML_CSS_USE_HCSR
	HTMLSurfaceBackend *fallback_backend = memnew(HTMLSurfaceHCSRBackend);
#else
	HTMLSurfaceBackend *fallback_backend = memnew(HTMLSurfaceCPUBackend);
#endif
	_detach_backend_presentation_outputs();
	memdelete(backend);
	backend = fallback_backend;
	_reset_frame_state_notifications();
	_sync_backend_state();
	html_surface_warn_auto_cpu_fallback(p_reason.is_empty() ? String("GPU target rendering failed.") : p_reason);
	return true;
#endif
}

void HTMLRenderSurface::_document_changed() {
	if (backend != nullptr) {
		backend->mark_document_dirty();
	}
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::_notify_changed() const {
	if (!changed_callback.is_valid()) {
		return;
	}

	Variant ret;
	Callable::CallError ce;
	changed_callback.callp(nullptr, 0, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("HTML render surface changed callback failed with error %d.", ce.error));
	}
}

void HTMLRenderSurface::_notify_frame_state_changes() {
	ERR_FAIL_NULL(backend);
	const uint64_t queued_generation = backend->get_last_queued_frame_generation();
	const uint64_t active_generation = backend->get_active_frame_generation();
	if (queued_generation != 0 && queued_generation != notified_queued_frame_generation) {
		notified_queued_frame_generation = queued_generation;
		if (frame_queued_callback.is_valid()) {
			const Variant generation = queued_generation;
			const Variant *arguments[] = { &generation };
			Variant ret;
			Callable::CallError ce;
			frame_queued_callback.callp(arguments, 1, ret, ce);
			if (ce.error != Callable::CallError::CALL_OK) {
				ERR_PRINT(vformat("HTML render surface frame-queued callback failed with error %d.", ce.error));
			}
		}
	}
	if (active_generation != 0 && active_generation != notified_active_frame_generation) {
		notified_active_frame_generation = active_generation;
		if (frame_activated_callback.is_valid()) {
			const Variant generation = active_generation;
			const Variant *arguments[] = { &generation };
			Variant ret;
			Callable::CallError ce;
			frame_activated_callback.callp(arguments, 1, ret, ce);
			if (ce.error != Callable::CallError::CALL_OK) {
				ERR_PRINT(vformat("HTML render surface frame-activated callback failed with error %d.", ce.error));
			}
		}
	}
}

void HTMLRenderSurface::_reset_frame_state_notifications() {
	notified_queued_frame_generation = 0;
	notified_active_frame_generation = 0;
}

void HTMLRenderSurface::set_document(const Ref<HTMLDocument> &p_document) {
	if (document == p_document) {
		return;
	}
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	document = p_document;
	if (document.is_valid()) {
		document->connect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	_sync_backend_state();
	render_now(marker);
}

Ref<HTMLDocument> HTMLRenderSurface::get_document() const {
	return document;
}

bool HTMLRenderSurface::set_size(const Size2i &p_size) {
	return set_viewport(p_size, device_scale_factor);
}

Size2i HTMLRenderSurface::get_size() const {
	return size;
}

bool HTMLRenderSurface::set_device_scale_factor(float p_device_scale_factor) {
	return set_viewport(size, p_device_scale_factor);
}

float HTMLRenderSurface::get_device_scale_factor() const {
	return device_scale_factor;
}

bool HTMLRenderSurface::set_viewport(const Size2i &p_size, float p_device_scale_factor, bool p_render) {
	const Size2i derived_physical_size(
			MAX(1, (int)Math::round(p_size.x * p_device_scale_factor)),
			MAX(1, (int)Math::round(p_size.y * p_device_scale_factor)));
	return set_viewport(p_size, p_device_scale_factor, derived_physical_size, p_render);
}

bool HTMLRenderSurface::set_viewport(const Size2i &p_size, float p_device_scale_factor, const Size2i &p_physical_size, bool p_render) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	Size2i new_physical_size = Size2i(MAX(1, p_physical_size.x), MAX(1, p_physical_size.y));
	float new_device_scale_factor = p_device_scale_factor;
	if (!Math::is_finite(new_device_scale_factor) || new_device_scale_factor <= 0.0f) {
		new_device_scale_factor = 1.0f;
	}
	new_device_scale_factor = CLAMP(new_device_scale_factor, 0.01f, 8.0f);
	if (size == new_size && physical_size == new_physical_size && Math::is_equal_approx(device_scale_factor, new_device_scale_factor)) {
		return false;
	}
	size = new_size;
	physical_size = new_physical_size;
	device_scale_factor = new_device_scale_factor;
	gpu_backdrop_frame.clear();
	if (backend != nullptr) {
		_sync_backend_state();
	}
	if (p_render) {
		render_now(marker);
	}
	return true;
}

void HTMLRenderSurface::set_placeholder_background(const Color &p_color) {
	if (placeholder_background == p_color) {
		return;
	}
	placeholder_background = p_color;
	_sync_backend_state();
	render_now(marker);
}

void HTMLRenderSurface::set_backdrop_filter_enabled(bool p_enabled) {
	if (backdrop_filter_enabled == p_enabled) {
		return;
	}
	backdrop_filter_enabled = p_enabled;
	gpu_backdrop_frame.clear();
	if (backend != nullptr) {
		_sync_backend_state();
	}
	render_now(marker);
}

bool HTMLRenderSurface::is_backdrop_filter_enabled() const {
	return backdrop_filter_enabled;
}

void HTMLRenderSurface::set_backend_preference(HTMLSurfaceBackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 6);
	if (backend_preference == p_backend_preference) {
		return;
	}
#ifdef HTML_CSS_USE_HCSR
	const int current_resolved_backend = html_surface_hcsr_resolved_backend(backend_preference);
	if (backend != nullptr
			&& !backend->has_terminal_render_failure()
			&& current_resolved_backend >= 0
			&& current_resolved_backend == html_surface_hcsr_resolved_backend(p_backend_preference)) {
		backend_preference = p_backend_preference;
		return;
	}
#endif
	backend_preference = p_backend_preference;
	if (backend != nullptr) {
		_detach_backend_presentation_outputs();
		memdelete(backend);
		backend = nullptr;
		_reset_frame_state_notifications();
	}
	render_now(marker);
}

HTMLSurfaceBackendPreference HTMLRenderSurface::get_backend_preference() const {
	return backend_preference;
}

void HTMLRenderSurface::set_changed_callback(const Callable &p_callback) {
	changed_callback = p_callback;
}

void HTMLRenderSurface::set_frame_queued_callback(const Callable &p_callback) {
	frame_queued_callback = p_callback;
}

void HTMLRenderSurface::set_frame_activated_callback(const Callable &p_callback) {
	frame_activated_callback = p_callback;
}

uint64_t HTMLRenderSurface::get_last_queued_frame_generation() const {
	return backend != nullptr ? backend->get_last_queued_frame_generation() : 0;
}

uint64_t HTMLRenderSurface::get_active_frame_generation() const {
	return backend != nullptr ? backend->get_active_frame_generation() : 0;
}

bool HTMLRenderSurface::uses_generation_bound_input() const {
	return backend != nullptr && backend->uses_generation_bound_input();
}

Error HTMLRenderSurface::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	_ensure_backend();
	bool needs_output = true;
	bool needs_begin_frame = false;
	Error err = backend->update_compositor(p_timeline_time_seconds, &needs_output, &needs_begin_frame);
	ERR_FAIL_COND_V(err != OK, err);

	backend->get_frame_metadata(frame_metadata);
	backend->get_gpu_backdrop_frame(gpu_backdrop_frame);
	if (r_needs_output != nullptr) {
		*r_needs_output = needs_output;
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = needs_begin_frame;
	}
	return OK;
}

void HTMLRenderSurface::render_now(const String &p_marker) {
	marker = p_marker;
	_ensure_backend();
	_sync_backend_state();
	backend->render_placeholder(marker);
	if (_fallback_auto_gpu_to_cpu(backend->get_terminal_render_failure_reason())) {
		bool fallback_needs_output = true;
		backend->update_compositor(0.0, &fallback_needs_output, nullptr);
		backend->render_placeholder(marker);
	}
	backend->get_frame_metadata(frame_metadata);
	backend->get_gpu_backdrop_frame(gpu_backdrop_frame);
	_notify_changed();
	_notify_frame_state_changes();
}

bool HTMLRenderSurface::poll_pending_output(bool *r_waiting_for_completion) {
	_ensure_backend();
	const bool changed = backend->poll_pending_output(r_waiting_for_completion);
	if (changed) {
		backend->get_frame_metadata(frame_metadata);
		backend->get_gpu_backdrop_frame(gpu_backdrop_frame);
		_notify_changed();
	}
	_notify_frame_state_changes();
	return changed;
}

HTMLPendingOutputState HTMLRenderSurface::consume_pending_output_state() {
	_ensure_backend();
	HTMLPendingOutputState state = backend->consume_pending_output_state();
	if (state.presentation_changed) {
		backend->get_frame_metadata(frame_metadata);
		backend->get_gpu_backdrop_frame(gpu_backdrop_frame);
		_notify_changed();
	}
	_notify_frame_state_changes();
	return state;
}

void HTMLRenderSurface::schedule_retirement_service() {
	_ensure_backend();
	backend->schedule_retirement_service();
}

bool HTMLRenderSurface::has_pending_output() const {
	return backend != nullptr && backend->has_pending_output();
}

bool HTMLRenderSurface::has_pending_frame_request() const {
	return backend != nullptr && backend->has_pending_frame_request();
}

bool HTMLRenderSurface::has_terminal_render_failure() const {
	return backend != nullptr && backend->has_terminal_render_failure();
}

String HTMLRenderSurface::get_terminal_render_failure_reason() const {
	return backend != nullptr ? backend->get_terminal_render_failure_reason() : String();
}

bool HTMLRenderSurface::is_begin_frame_requested() const {
	return backend != nullptr && backend->is_begin_frame_requested();
}

Error HTMLRenderSurface::submit_cpu_frame(const HTMLCPUFrame &p_frame, const HTMLFrameMetadata &p_metadata) {
	_ensure_backend();
	Error err = backend->submit_cpu_frame(p_frame);
	ERR_FAIL_COND_V(err != OK, err);

	frame_metadata = p_metadata;
	gpu_backdrop_frame.clear();
	_notify_changed();
	return OK;
}

const HTMLFrameMetadata &HTMLRenderSurface::get_frame_metadata() const {
	return frame_metadata;
}

const HTMLGPUBackdropFrame &HTMLRenderSurface::get_gpu_backdrop_frame() const {
	return gpu_backdrop_frame;
}

const Vector<HTMLBackdropFilterRegion> &HTMLRenderSurface::get_backdrop_filter_regions() const {
	return frame_metadata.backdrop_filter_regions;
}

const HTMLElementHit *HTMLRenderSurface::find_hit_at(const Point2i &p_position) const {
	return frame_metadata.find_hit_at(p_position);
}

bool HTMLRenderSurface::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	if (backend != nullptr && backend->hit_test(p_position, r_hit)) {
		return true;
	}

	const HTMLElementHit *hit = frame_metadata.find_hit_at(Point2i(Math::floor(p_position.x), Math::floor(p_position.y)));
	if (hit == nullptr) {
		return false;
	}

	r_hit = *hit;
	return true;
}

Error HTMLRenderSurface::mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed) {
	_ensure_backend();
	return backend->mouse_move(p_position, p_modifiers, r_visual_state_changed);
}

Error HTMLRenderSurface::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	_ensure_backend();
	return backend->mouse_down(p_position, p_button, p_modifiers, p_click_count);
}

Error HTMLRenderSurface::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	_ensure_backend();
	return backend->mouse_up(p_position, p_button, p_modifiers, p_click_count);
}

Error HTMLRenderSurface::pointer_cancel(const Point2 &p_position, int p_pointer_id) {
	_ensure_backend();
	return backend->pointer_cancel(p_position, p_pointer_id);
}

Error HTMLRenderSurface::notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) {
	_ensure_backend();
	return backend->notify_pointer_leave(p_position, p_cancel_pressed_interaction, p_pointer_id);
}

Error HTMLRenderSurface::begin_scrollbar_interaction(const Point2 &p_position, double p_event_time_seconds, bool &r_consumed) {
	_ensure_backend();
	return backend->begin_scrollbar_interaction(p_position, p_event_time_seconds, r_consumed);
}

Error HTMLRenderSurface::update_scrollbar_interaction(const Point2 &p_position, bool &r_consumed) {
	_ensure_backend();
	return backend->update_scrollbar_interaction(p_position, r_consumed);
}

Error HTMLRenderSurface::end_scrollbar_interaction(bool &r_consumed) {
	_ensure_backend();
	return backend->end_scrollbar_interaction(r_consumed);
}

bool HTMLRenderSurface::poll_pointer_event(HTMLPointerEvent &r_event) {
	_ensure_backend();
	return backend->poll_pointer_event(r_event);
}

Error HTMLRenderSurface::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	_ensure_backend();
	return backend->wheel(p_position, p_delta);
}

Error HTMLRenderSurface::key_down(HTMLSurfaceInputKey p_key, int p_modifiers) {
	_ensure_backend();
	return backend->key_down(p_key, p_modifiers);
}

Error HTMLRenderSurface::key_up(HTMLSurfaceInputKey p_key, int p_modifiers) {
	_ensure_backend();
	return backend->key_up(p_key, p_modifiers);
}

Error HTMLRenderSurface::text_input(const String &p_text) {
	_ensure_backend();
	return backend->text_input(p_text);
}

Error HTMLRenderSurface::set_element_text(const StringName &p_id, const String &p_text) {
	_ensure_backend();
	return backend->set_element_text(p_id, p_text);
}

Error HTMLRenderSurface::apply_element_mutations(const Array &p_mutations) {
	if (p_mutations.is_empty()) {
		return OK;
	}
	_ensure_backend();
	return backend->apply_element_mutations(p_mutations);
}

Error HTMLRenderSurface::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	_ensure_backend();
	return backend->set_element_inner_html(p_id, p_html_fragment);
}

Error HTMLRenderSurface::set_body_inner_html(const String &p_html_fragment) {
	_ensure_backend();
	return backend->set_body_inner_html(p_html_fragment);
}

Error HTMLRenderSurface::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	_ensure_backend();
	return backend->set_element_attribute(p_id, p_name, p_value);
}

Error HTMLRenderSurface::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	_ensure_backend();
	return backend->remove_element_attribute(p_id, p_name);
}

Error HTMLRenderSurface::set_element_style(const StringName &p_id, const String &p_css_text) {
	_ensure_backend();
	return backend->set_element_style(p_id, p_css_text);
}

Error HTMLRenderSurface::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	_ensure_backend();
	return backend->replace_stylesheet_text(p_style_id, p_css_text);
}

Error HTMLRenderSurface::scroll_element_into_view(const StringName &p_id, const StringName &p_block_alignment) {
	if (p_id.is_empty() || p_block_alignment != StringName("start")) {
		return ERR_INVALID_PARAMETER;
	}
	_ensure_backend();
	return backend->scroll_element_into_view(p_id, p_block_alignment);
}

Error HTMLRenderSurface::set_form_control_value(const StringName &p_id, const String &p_value) {
	_ensure_backend();
	return backend->set_form_control_value(p_id, p_value);
}

Error HTMLRenderSurface::set_form_control_checked(const StringName &p_id, bool p_checked) {
	_ensure_backend();
	return backend->set_form_control_checked(p_id, p_checked);
}

Error HTMLRenderSurface::focus_element(const StringName &p_id) {
	_ensure_backend();
	return backend->focus_element(p_id);
}

Error HTMLRenderSurface::blur_focused_element() {
	_ensure_backend();
	return backend->blur_focused_element();
}

Error HTMLRenderSurface::set_text_selection(const StringName &p_id, int p_start, int p_end) {
	_ensure_backend();
	return backend->set_text_selection(p_id, p_start, p_end);
}

bool HTMLRenderSurface::get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) {
	_ensure_backend();
	return backend->get_form_control_state(p_id, r_state);
}

bool HTMLRenderSurface::is_document_source_valid() const {
	return document.is_null() || document->is_source_valid();
}

Error HTMLRenderSurface::load_asset(const String &p_uri, HTMLAssetResource &r_asset, String *r_error) const {
	return HTMLGodotAssetProvider::load_asset(document, p_uri, r_asset, r_error);
}

Ref<Texture2D> HTMLRenderSurface::get_texture() const {
	return backend != nullptr ? backend->get_texture() : Ref<Texture2D>();
}

Ref<HTMLTexture2D> HTMLRenderSurface::get_html_texture() const {
	return backend != nullptr ? backend->get_html_texture() : Ref<HTMLTexture2D>();
}

uint64_t HTMLRenderSurface::create_presentation_output(const Size2i &p_size, bool p_mipmaps) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, 0);
	const uint64_t output_id = next_presentation_output_id++;
	PresentationOutputBinding binding;
	binding.size = p_size;
	binding.mipmaps = p_mipmaps;
	if (backend != nullptr) {
		binding.backend_output_id = backend->create_presentation_output(
				p_size,
				p_mipmaps);
	}
	presentation_outputs.insert(output_id, binding);
	render_now(marker);
	return output_id;
}

Error HTMLRenderSurface::resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, ERR_INVALID_PARAMETER);
	PresentationOutputBinding *binding = presentation_outputs.getptr(p_output_id);
	ERR_FAIL_NULL_V(binding, ERR_DOES_NOT_EXIST);
	if (binding->size == p_size) {
		return OK;
	}
	binding->size = p_size;
	if (backend != nullptr && binding->backend_output_id != 0) {
		const Error error = backend->resize_presentation_output(
				binding->backend_output_id,
				p_size);
		if (error != OK) {
			return error;
		}
	}
	render_now(marker);
	return OK;
}

void HTMLRenderSurface::destroy_presentation_output(uint64_t p_output_id) {
	PresentationOutputBinding *binding = presentation_outputs.getptr(p_output_id);
	if (binding == nullptr) {
		return;
	}
	if (backend != nullptr && binding->backend_output_id != 0) {
		backend->destroy_presentation_output(binding->backend_output_id);
	}
	presentation_outputs.erase(p_output_id);
}

Ref<Texture2D> HTMLRenderSurface::get_presentation_output_texture(uint64_t p_output_id) const {
	const PresentationOutputBinding *binding = presentation_outputs.getptr(p_output_id);
	return backend != nullptr && binding != nullptr && binding->backend_output_id != 0
			? backend->get_presentation_output_texture(binding->backend_output_id)
			: Ref<Texture2D>();
}

uint64_t HTMLRenderSurface::get_presentation_output_generation(uint64_t p_output_id) const {
	const PresentationOutputBinding *binding = presentation_outputs.getptr(p_output_id);
	return backend != nullptr && binding != nullptr && binding->backend_output_id != 0
			? backend->get_presentation_output_generation(binding->backend_output_id)
			: 0;
}

HTMLRenderSurface::HTMLRenderSurface() {
	_ensure_backend();
	render_now(marker);
}

HTMLRenderSurface::~HTMLRenderSurface() {
	if (document.is_valid()) {
		document->disconnect_changed(callable_mp(this, &HTMLRenderSurface::_document_changed));
	}
	if (backend != nullptr) {
		_detach_backend_presentation_outputs();
		memdelete(backend);
	}
}
