/**************************************************************************/
/*  html_surface_hcsr_old_factory.cpp                                     */
/**************************************************************************/

#include "html_surface_backend_factory.h"

#include "html_surface_hcsr_backend.h"
#include "html_surface_unsupported_backend.h"

#include "core/error/error_macros.h"
#include "core/os/os.h"

static String html_surface_hcsr_old_rendering_driver() {
	return OS::get_singleton() != nullptr
			? OS::get_singleton()->get_current_rendering_driver_name().to_lower()
			: String();
}

static bool html_surface_hcsr_old_can_use_gpu() {
	const String rendering_driver = html_surface_hcsr_old_rendering_driver();
	return rendering_driver == "d3d12" || rendering_driver == "vulkan" || rendering_driver == "metal";
}

static hcsr_render_backend_t html_surface_hcsr_old_backend_for_driver(const String &p_rendering_driver) {
	if (p_rendering_driver == "vulkan") {
		return HCSR_RENDER_BACKEND_VULKAN;
	}
	if (p_rendering_driver == "metal") {
		return HCSR_RENDER_BACKEND_METAL;
	}
	return HCSR_RENDER_BACKEND_D3D12;
}

static int html_surface_hcsr_old_resolved_backend(HTMLSurfaceBackendPreference p_preference) {
	const String rendering_driver = html_surface_hcsr_old_rendering_driver();
	if (p_preference == HTML_SURFACE_BACKEND_CPU) {
		return HCSR_RENDER_BACKEND_CPU;
	}
	if (p_preference == HTML_SURFACE_BACKEND_AUTO || p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) {
		if (html_surface_hcsr_old_can_use_gpu()) {
			return html_surface_hcsr_old_backend_for_driver(rendering_driver);
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

HTMLSurfaceBackend *html_surface_backend_create(HTMLSurfaceBackendPreference p_preference) {
	const String rendering_driver = html_surface_hcsr_old_rendering_driver();
	if (p_preference == HTML_SURFACE_BACKEND_AUTO) {
		if (html_surface_hcsr_old_can_use_gpu()) {
			return memnew(HTMLSurfaceHCSRBackend(html_surface_hcsr_old_backend_for_driver(rendering_driver)));
		}
		WARN_PRINT_ONCE("HTML/CSS Auto backend is using old HCSR CPU output because the active driver is not D3D12, Vulkan, or Metal.");
		return memnew(HTMLSurfaceHCSRBackend);
	}
	if (p_preference == HTML_SURFACE_BACKEND_GPU_AUTO) {
		return html_surface_hcsr_old_can_use_gpu()
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(html_surface_hcsr_old_backend_for_driver(rendering_driver)))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS GPU backend requested, but old HCSR requires Godot's Vulkan, D3D12, or Metal rendering driver."));
	}
	if (p_preference == HTML_SURFACE_BACKEND_VULKAN) {
		return rendering_driver == "vulkan"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_VULKAN))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Vulkan backend requested, but Godot is not running its Vulkan rendering driver."));
	}
	if (p_preference == HTML_SURFACE_BACKEND_D3D12) {
		return rendering_driver == "d3d12"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_D3D12))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS D3D12 backend requested, but Godot is not running its D3D12 rendering driver."));
	}
	if (p_preference == HTML_SURFACE_BACKEND_METAL) {
		return rendering_driver == "metal"
				? (HTMLSurfaceBackend *)memnew(HTMLSurfaceHCSRBackend(HCSR_RENDER_BACKEND_METAL))
				: (HTMLSurfaceBackend *)memnew(HTMLSurfaceUnsupportedBackend("HTML/CSS Metal backend requested, but Godot is not running its Metal rendering driver."));
	}
	return memnew(HTMLSurfaceHCSRBackend);
}

HTMLSurfaceBackend *html_surface_backend_create_auto_fallback(const String &p_reason) {
	WARN_PRINT_ONCE(vformat("HTML/CSS old HCSR GPU output failed; using its CPU backend: %s", p_reason));
	return memnew(HTMLSurfaceHCSRBackend);
}

bool html_surface_backend_can_reuse(
		HTMLSurfaceBackendPreference p_current,
		HTMLSurfaceBackendPreference p_requested,
		const HTMLSurfaceBackend *p_backend) {
	if (p_backend == nullptr || p_backend->has_terminal_render_failure()) {
		return false;
	}
	const int current_backend = html_surface_hcsr_old_resolved_backend(p_current);
	return current_backend >= 0 && current_backend == html_surface_hcsr_old_resolved_backend(p_requested);
}

HTMLSurfaceBackendPreference html_surface_backend_default_preference() {
	return HTML_SURFACE_BACKEND_CPU;
}

bool html_surface_backend_should_defer_activation(
		HTMLSurfaceBackendPreference p_preference,
		bool p_has_current_viewport_size) {
	return p_preference != HTML_SURFACE_BACKEND_CPU && !p_has_current_viewport_size;
}
