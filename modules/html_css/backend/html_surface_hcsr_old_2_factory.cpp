/**************************************************************************/
/*  html_surface_hcsr_old_2_factory.cpp                                   */
/**************************************************************************/

#include "html_surface_backend_factory.h"

#include "html_surface_hcsr_old_2_backend.h"
#include "html_surface_unsupported_backend.h"

#include "core/os/os.h"

static bool html_surface_hcsr_old_2_can_use_d3d12() {
	return OS::get_singleton() != nullptr
			&& OS::get_singleton()->get_current_rendering_driver_name().to_lower() == "d3d12";
}

static bool html_surface_hcsr_old_2_preference_uses_runtime(HTMLSurfaceBackendPreference p_preference) {
	return html_surface_hcsr_old_2_can_use_d3d12()
			&& (p_preference == HTML_SURFACE_BACKEND_AUTO
					|| p_preference == HTML_SURFACE_BACKEND_GPU_AUTO
					|| p_preference == HTML_SURFACE_BACKEND_D3D12);
}

HTMLSurfaceBackend *html_surface_backend_create(HTMLSurfaceBackendPreference p_preference) {
	if (p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) {
		return html_surface_hcsr_old_2_can_use_d3d12()
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSROld2Backend)
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS hcsr_old_2 requires Godot's D3D12 rendering driver."));
	}
	if (p_preference == HTML_SURFACE_BACKEND_D3D12) {
		return html_surface_hcsr_old_2_can_use_d3d12()
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSROld2Backend)
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS hcsr_old_2 D3D12 backend requested, but Godot is not running its D3D12 driver."));
	}
	if (p_preference == HTML_SURFACE_BACKEND_CPU) {
		return memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS hcsr_old_2 has no CPU presentation backend."));
	}
	return memnew(HTMLSurfaceUnsupportedBackend("The requested GPU backend is not implemented by hcsr_old_2."));
}

HTMLSurfaceBackend *html_surface_backend_create_auto_fallback(const String &p_reason) {
	(void)p_reason;
	return nullptr;
}

bool html_surface_backend_can_reuse(
		HTMLSurfaceBackendPreference p_current,
		HTMLSurfaceBackendPreference p_requested,
		const HTMLSurfaceBackend *p_backend) {
	return p_backend != nullptr
			&& !p_backend->has_terminal_render_failure()
			&& html_surface_hcsr_old_2_preference_uses_runtime(p_current)
			&& html_surface_hcsr_old_2_preference_uses_runtime(p_requested);
}

HTMLSurfaceBackendPreference html_surface_backend_default_preference() {
	return HTML_SURFACE_BACKEND_GPU_AUTO;
}

bool html_surface_backend_should_defer_activation(
		HTMLSurfaceBackendPreference p_preference,
		bool p_has_current_viewport_size) {
	(void)p_preference;
	(void)p_has_current_viewport_size;
	return false;
}
