/**************************************************************************/
/*  html_surface_hcsr_newest_factory.cpp                                  */
/**************************************************************************/

#include "html_surface_backend_factory.h"

#include "html_surface_hcsr_newest_backend.h"
#include "html_surface_unsupported_backend.h"

#include "core/os/os.h"

static String newest_driver() {
	return OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name().to_lower() : String();
}

static int newest_resolve(HTMLSurfaceBackendPreference p_preference) {
	const String driver = newest_driver();
	if (p_preference == HTML_SURFACE_BACKEND_CPU) {
		return HTML_SURFACE_HCSR_NEWEST_CPU;
	}
	if ((p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) && driver == "d3d12") {
		return HTML_SURFACE_HCSR_NEWEST_D3D12;
	}
	if ((p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) && driver == "vulkan") {
		return HTML_SURFACE_HCSR_NEWEST_VULKAN;
	}
	if (p_preference == HTML_SURFACE_BACKEND_D3D12 && driver == "d3d12") {
		return HTML_SURFACE_HCSR_NEWEST_D3D12;
	}
	if (p_preference == HTML_SURFACE_BACKEND_VULKAN && driver == "vulkan") {
		return HTML_SURFACE_HCSR_NEWEST_VULKAN;
	}
	return p_preference == HTML_SURFACE_BACKEND_AUTO ? HTML_SURFACE_HCSR_NEWEST_CPU : -1;
}

HTMLSurfaceBackend *html_surface_backend_create(HTMLSurfaceBackendPreference p_preference) {
	const int renderer = newest_resolve(p_preference);
	if (renderer >= 0) {
		return memnew(HTMLSurfaceHCSRNewestBackend((HTMLSurfaceHCSRNewestRenderer)renderer));
	}
	return memnew(HTMLSurfaceUnsupportedBackend("The requested hcsr_newest renderer does not match Godot's active rendering driver."));
}

HTMLSurfaceBackend *html_surface_backend_create_auto_fallback(const String &p_reason) {
	WARN_PRINT_ONCE(vformat("HTML/CSS hcsr_newest GPU output failed; using its CPU renderer: %s", p_reason));
	return memnew(HTMLSurfaceHCSRNewestBackend(HTML_SURFACE_HCSR_NEWEST_CPU));
}

bool html_surface_backend_can_reuse(HTMLSurfaceBackendPreference p_current, HTMLSurfaceBackendPreference p_requested, const HTMLSurfaceBackend *p_backend) {
	return p_backend != nullptr && !p_backend->has_terminal_render_failure() && newest_resolve(p_current) == newest_resolve(p_requested);
}

HTMLSurfaceBackendPreference html_surface_backend_default_preference() {
	return HTML_SURFACE_BACKEND_GPU_AUTO;
}

bool html_surface_backend_should_defer_activation(HTMLSurfaceBackendPreference p_preference, bool p_has_current_viewport_size) {
	return p_preference != HTML_SURFACE_BACKEND_CPU && !p_has_current_viewport_size;
}
