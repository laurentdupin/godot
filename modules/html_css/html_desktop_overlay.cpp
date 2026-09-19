/**************************************************************************/
/*  html_desktop_overlay.cpp                                              */
/**************************************************************************/

#include "html_desktop_overlay.h"
#include "html_texture.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

namespace {
constexpr uint32_t required_overlay_abi = 5;
// HCSR newest finishes a non-mipmapped secondary output as a color attachment.
// The overlay pass temporarily transitions it to shader-resource and restores
// this state, so Godot's render-graph tracking remains correct.
constexpr uint32_t d3d12_html_output_state = 0x4;

Point2i to_native_desktop_position(const Point2i &p_position) {
#ifdef WINDOWS_ENABLED
	DisplayServer *display = DisplayServer::get_singleton();
	if (display != nullptr) {
		// DisplayServerWindows exposes coordinates normalized by the virtual
		// desktop origin. Win32 HWND APIs use the native desktop where the
		// primary monitor begins at (0, 0).
		return p_position - display->screen_get_position(display->get_primary_screen());
	}
#endif
	return p_position;
}
}

void HTMLDesktopOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "rect", "library_path"), &HTMLDesktopOverlay::create,
			DEFVAL("res://Addons/OverlayWindows/build/Release/OverlayWindows.dll"));
	ClassDB::bind_method(D_METHOD("present", "output"), &HTMLDesktopOverlay::present);
	ClassDB::bind_method(D_METHOD("set_rect", "rect"), &HTMLDesktopOverlay::set_rect);
	ClassDB::bind_method(D_METHOD("set_visible", "visible"), &HTMLDesktopOverlay::set_visible);
	ClassDB::bind_method(D_METHOD("close"), &HTMLDesktopOverlay::close);
	ClassDB::bind_method(D_METHOD("is_open"), &HTMLDesktopOverlay::is_open);
	ClassDB::bind_method(D_METHOD("get_last_native_error"), &HTMLDesktopOverlay::get_last_native_error);
	ClassDB::bind_method(D_METHOD("get_presented_generation"), &HTMLDesktopOverlay::get_presented_generation);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "open", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "is_open");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "last_native_error", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "get_last_native_error");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "presented_generation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY), "", "get_presented_generation");
}

void HTMLDesktopOverlay::_capture_device_on_render_thread(uint64_t p_self) {
	HTMLDesktopOverlay *self = reinterpret_cast<HTMLDesktopOverlay *>(p_self);
	RenderingServer *server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = server != nullptr ? server->get_rendering_device() : nullptr;
	if (rendering_device == nullptr) {
		return;
	}
	self->device = rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	self->queue = rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
}

void HTMLDesktopOverlay::_present_on_render_thread(uint64_t p_self, RID p_texture,
		uint64_t p_generation, Size2i p_size) {
	HTMLDesktopOverlay *self = reinterpret_cast<HTMLDesktopOverlay *>(p_self);
	if (self->overlay == 0 || self->update_d3d12 == nullptr) {
		return;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	const uint64_t native_texture = server != nullptr ? server->texture_get_native_handle(p_texture) : 0;
	if (native_texture == 0 || self->update_d3d12(self->overlay, native_texture,
			p_generation, p_size.x, p_size.y, d3d12_html_output_state) == 0) {
		self->last_native_error = self->get_last_error != nullptr ? self->get_last_error() : 0;
		// The source can be temporarily unavailable while HTML swaps a target.
		// Allow the same generation to be queued again on the following frame.
		self->scheduled_generation = 0;
	} else {
		self->last_native_error = 0;
		self->presented_generation = p_generation;
	}
}

Error HTMLDesktopOverlay::create(const Rect2i &p_rect, const String &p_library_path) {
	ERR_FAIL_COND_V_MSG(overlay != 0, ERR_ALREADY_IN_USE, "The desktop overlay is already open.");
	ERR_FAIL_COND_V_MSG(p_rect.size.x <= 0 || p_rect.size.y <= 0, ERR_INVALID_PARAMETER, "Overlay dimensions must be positive.");
	ERR_FAIL_COND_V_MSG(RenderingServer::get_singleton() == nullptr, ERR_UNAVAILABLE, "RenderingServer is unavailable.");

	const String library_path = ProjectSettings::get_singleton()->globalize_path(p_library_path);
	Error error = OS::get_singleton()->open_dynamic_library(library_path, library);
	if (error != OK) {
		library = nullptr;
		return error;
	}
#define LOAD_OVERLAY_SYMBOL(member, symbol) \
	if (OS::get_singleton()->get_dynamic_library_symbol_handle(library, symbol, reinterpret_cast<void *&>(member)) != OK) { \
		_unload_library(); \
		return ERR_CANT_RESOLVE; \
	}
	LOAD_OVERLAY_SYMBOL(get_abi_version, "OverlayWindows_GetAbiVersion");
	LOAD_OVERLAY_SYMBOL(create_d3d12, "OverlayWindows_CreateOverlayWindowD3D12");
	LOAD_OVERLAY_SYMBOL(destroy, "OverlayWindows_DestroyOverlayWindow");
	LOAD_OVERLAY_SYMBOL(update_d3d12, "OverlayWindows_UpdateOverlayD3D12TexturePremultiplied");
	LOAD_OVERLAY_SYMBOL(set_rect_native, "OverlayWindows_SetOverlayRect");
	LOAD_OVERLAY_SYMBOL(set_visible_native, "OverlayWindows_SetOverlayVisible");
	LOAD_OVERLAY_SYMBOL(get_last_error, "OverlayWindows_GetLastError");
#undef LOAD_OVERLAY_SYMBOL
	if (get_abi_version() < required_overlay_abi) {
		_unload_library();
		return ERR_UNAVAILABLE;
	}

	RenderingServer *server = RenderingServer::get_singleton();
	server->call_on_render_thread(callable_mp_static(&HTMLDesktopOverlay::_capture_device_on_render_thread).bind((uint64_t)this));
	server->sync();
	if (device == 0 || queue == 0) {
		_unload_library();
		return ERR_UNAVAILABLE;
	}
	const Point2i native_position = to_native_desktop_position(p_rect.position);
	overlay = create_d3d12(native_position.x, native_position.y,
			p_rect.size.x, p_rect.size.y, device, queue);
	if (overlay == 0) {
		last_native_error = get_last_error();
		_unload_library();
		return ERR_CANT_CREATE;
	}
	return OK;
}

Error HTMLDesktopOverlay::present(const Ref<HTMLViewOutput> &p_output) {
	ERR_FAIL_COND_V_MSG(overlay == 0, ERR_UNCONFIGURED, "Create the desktop overlay first.");
	ERR_FAIL_COND_V_MSG(p_output.is_null() || !p_output->is_valid(), ERR_INVALID_PARAMETER, "A valid HTMLViewOutput is required.");
	ERR_FAIL_COND_V_MSG(p_output->has_mipmaps(), ERR_UNAVAILABLE, "Desktop overlay presentation currently requires a non-mipmapped HTMLViewOutput.");
	const uint64_t generation = p_output->get_generation();
	if (generation == 0 || generation == scheduled_generation) {
		return OK;
	}
	Ref<Texture2D> texture = p_output->get_texture();
	ERR_FAIL_COND_V_MSG(texture.is_null(), ERR_UNAVAILABLE, "The HTML output does not have a GPU texture yet.");
	HTMLTexture2D *html_texture = Object::cast_to<HTMLTexture2D>(texture.ptr());
	ERR_FAIL_NULL_V_MSG(html_texture, ERR_UNAVAILABLE, "The HTML output texture is not an HTMLTexture2D.");
	const RID external_texture = html_texture->get_external_texture_rid();
	ERR_FAIL_COND_V_MSG(!external_texture.is_valid(), ERR_UNAVAILABLE, "The HTML output does not have an external GPU texture yet.");
	scheduled_generation = generation;
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp_static(&HTMLDesktopOverlay::_present_on_render_thread)
					.bind((uint64_t)this, external_texture, generation, p_output->get_size()));
	return OK;
}

Error HTMLDesktopOverlay::set_rect(const Rect2i &p_rect) {
	ERR_FAIL_COND_V_MSG(overlay == 0, ERR_UNCONFIGURED, "Create the desktop overlay first.");
	ERR_FAIL_COND_V_MSG(p_rect.size.x <= 0 || p_rect.size.y <= 0, ERR_INVALID_PARAMETER, "Overlay dimensions must be positive.");
	const Point2i native_position = to_native_desktop_position(p_rect.position);
	if (set_rect_native(overlay, native_position.x, native_position.y,
			p_rect.size.x, p_rect.size.y) == 0) {
		last_native_error = get_last_error();
		return FAILED;
	}
	return OK;
}

Error HTMLDesktopOverlay::set_visible(bool p_visible) {
	ERR_FAIL_COND_V_MSG(overlay == 0, ERR_UNCONFIGURED, "Create the desktop overlay first.");
	if (set_visible_native(overlay, p_visible ? 1 : 0) == 0) {
		last_native_error = get_last_error();
		return FAILED;
	}
	return OK;
}

void HTMLDesktopOverlay::close() {
	if (overlay != 0) {
		RenderingServer *server = RenderingServer::get_singleton();
		if (server != nullptr) {
			server->sync();
		}
		if (destroy != nullptr && destroy(overlay) == 0) {
			last_native_error = get_last_error != nullptr ? get_last_error() : 0;
		}
		overlay = 0;
	}
	_unload_library();
	device = 0;
	queue = 0;
	scheduled_generation = 0;
	presented_generation = 0;
}

void HTMLDesktopOverlay::_unload_library() {
	if (library != nullptr) {
		OS::get_singleton()->close_dynamic_library(library);
		library = nullptr;
	}
	get_abi_version = nullptr;
	create_d3d12 = nullptr;
	destroy = nullptr;
	update_d3d12 = nullptr;
	set_rect_native = nullptr;
	set_visible_native = nullptr;
	get_last_error = nullptr;
}

bool HTMLDesktopOverlay::is_open() const {
	return overlay != 0;
}

uint32_t HTMLDesktopOverlay::get_last_native_error() const {
	return last_native_error;
}

uint64_t HTMLDesktopOverlay::get_presented_generation() const {
	return presented_generation;
}

HTMLDesktopOverlay::~HTMLDesktopOverlay() {
	close();
}
