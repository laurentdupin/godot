/**************************************************************************/
/*  html_surface_backend_factory.h                                        */
/**************************************************************************/

#pragma once

#include "html_surface_backend.h"

enum HTMLSurfaceBackendPreference {
	HTML_SURFACE_BACKEND_AUTO,
	HTML_SURFACE_BACKEND_CPU,
	HTML_SURFACE_BACKEND_GPU_AUTO,
	HTML_SURFACE_BACKEND_VULKAN,
	HTML_SURFACE_BACKEND_D3D12,
	HTML_SURFACE_BACKEND_METAL,
};

HTMLSurfaceBackend *html_surface_backend_create(HTMLSurfaceBackendPreference p_preference);
HTMLSurfaceBackend *html_surface_backend_create_auto_fallback(const String &p_reason);
bool html_surface_backend_can_reuse(
		HTMLSurfaceBackendPreference p_current,
		HTMLSurfaceBackendPreference p_requested,
		const HTMLSurfaceBackend *p_backend);
HTMLSurfaceBackendPreference html_surface_backend_default_preference();
bool html_surface_backend_should_defer_activation(
		HTMLSurfaceBackendPreference p_preference,
		bool p_has_current_viewport_size);
