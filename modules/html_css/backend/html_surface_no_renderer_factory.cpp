/**************************************************************************/
/*  html_surface_no_renderer_factory.cpp                                  */
/**************************************************************************/

#include "html_surface_backend_factory.h"

#include "html_surface_cpu_backend.h"
#include "html_surface_unsupported_backend.h"

#include "core/error/error_macros.h"

HTMLSurfaceBackend *html_surface_backend_create(HTMLSurfaceBackendPreference p_preference) {
	if (p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_CPU) {
		if (p_preference == HTML_SURFACE_BACKEND_AUTO) {
			WARN_PRINT_ONCE("HTML/CSS Auto backend is using the raw CPU frame receiver because no renderer provider is compiled in.");
		}
		return memnew(HTMLSurfaceCPUBackend);
	}
	return memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS GPU backend requested, but no renderer provider is compiled in."));
}

HTMLSurfaceBackend *html_surface_backend_create_auto_fallback(const String &p_reason) {
	(void)p_reason;
	return memnew(HTMLSurfaceCPUBackend);
}

bool html_surface_backend_can_reuse(
		HTMLSurfaceBackendPreference p_current,
		HTMLSurfaceBackendPreference p_requested,
		const HTMLSurfaceBackend *p_backend) {
	if (p_backend == nullptr || p_backend->has_terminal_render_failure()) {
		return false;
	}
	const bool current_cpu = p_current == HTML_SURFACE_BACKEND_AUTO || p_current == HTML_SURFACE_BACKEND_CPU;
	const bool requested_cpu = p_requested == HTML_SURFACE_BACKEND_AUTO || p_requested == HTML_SURFACE_BACKEND_CPU;
	return current_cpu && requested_cpu;
}

HTMLSurfaceBackendPreference html_surface_backend_default_preference() {
	return HTML_SURFACE_BACKEND_CPU;
}

bool html_surface_backend_should_defer_activation(
		HTMLSurfaceBackendPreference p_preference,
		bool p_has_current_viewport_size) {
	(void)p_preference;
	(void)p_has_current_viewport_size;
	return false;
}
