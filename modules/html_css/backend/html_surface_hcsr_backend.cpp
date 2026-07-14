/**************************************************************************/
/*  html_surface_hcsr_backend.cpp                                        */
/**************************************************************************/

#include "html_surface_hcsr_backend.h"

#include "hcsr_performance_monitor.h"
#include "../bridge/html_asset_provider.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
#include <Metal/Metal.hpp>
#endif

static String hcsr_color_to_css_rgba(const Color &p_color) {
	return vformat("rgba(%d, %d, %d, %.6f)",
			Math::round(p_color.r * 255.0f),
			Math::round(p_color.g * 255.0f),
			Math::round(p_color.b * 255.0f),
			p_color.a);
}

static String hcsr_inject_document_style(const String &p_html, const Color &p_background, const String &p_css) {
	String style = "<style data-godot-hcsr=\"true\">html { background-color: " + hcsr_color_to_css_rgba(p_background) + "; }\n";
	style += p_css.replace("</style", "<\\/style");
	style += "\n</style>";
	const String lower_html = p_html.to_lower();
	const int head_end = lower_html.find("</head>");
	if (head_end >= 0) {
		return p_html.insert(head_end, style);
	}
	const int root_end = p_html.find(">");
	return root_end >= 0 ? p_html.insert(root_end + 1, style) : p_html;
}

static String hcsr_globalize_res_urls(const String &p_source) {
	String project_root = ProjectSettings::get_singleton()->globalize_path("res://").replace("\\", "/");
	if (!project_root.ends_with("/")) {
		project_root += "/";
	}
	return p_source.replace("res://", project_root);
}

static String hcsr_get_tag_attribute(const String &p_tag, const String &p_attribute) {
	const String lower_tag = p_tag.to_lower();
	const String attribute = p_attribute.to_lower();
	int cursor = 0;
	while ((cursor = lower_tag.find(attribute, cursor)) >= 0) {
		const bool starts_token = cursor == 0 || lower_tag[cursor - 1] <= 0x20;
		int value_start = cursor + attribute.length();
		while (value_start < lower_tag.length() && lower_tag[value_start] <= 0x20) {
			value_start++;
		}
		if (!starts_token || value_start >= lower_tag.length() || lower_tag[value_start] != '=') {
			cursor += attribute.length();
			continue;
		}
		value_start++;
		while (value_start < p_tag.length() && p_tag[value_start] <= 0x20) {
			value_start++;
		}
		if (value_start >= p_tag.length()) {
			return String();
		}
		const char32_t quote = p_tag[value_start];
		if (quote == '\'' || quote == '"') {
			const int value_end = p_tag.find_char(quote, value_start + 1);
			return value_end >= 0 ? p_tag.substr(value_start + 1, value_end - value_start - 1) : String();
		}
		int value_end = value_start;
		while (value_end < p_tag.length() && p_tag[value_end] > 0x20 && p_tag[value_end] != '>') {
			value_end++;
		}
		return p_tag.substr(value_start, value_end - value_start);
	}
	return String();
}

static void hcsr_inline_packed_stylesheets(const Ref<HTMLDocument> &p_document, String &r_html, String &r_css) {
	int cursor = 0;
	while (true) {
		const String lower_html = r_html.to_lower();
		const int link_start = lower_html.find("<link", cursor);
		if (link_start < 0) {
			return;
		}
		const int link_end = lower_html.find(">", link_start + 5);
		if (link_end < 0) {
			return;
		}
		const String link_tag = r_html.substr(link_start, link_end - link_start + 1);
		const String rel = hcsr_get_tag_attribute(link_tag, "rel");
		const String href = hcsr_get_tag_attribute(link_tag, "href");
		bool is_stylesheet = false;
		for (const String &token : rel.split(" ", false)) {
			if (token.to_lower() == "stylesheet") {
				is_stylesheet = true;
				break;
			}
		}
		if (!is_stylesheet || href.is_empty()) {
			cursor = link_end + 1;
			continue;
		}
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(p_document, href, asset, &error) != OK) {
			ERR_PRINT(error);
			cursor = link_end + 1;
			continue;
		}
		r_css += "\n" + String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size()) + "\n";
		r_html = r_html.erase(link_start, link_end - link_start + 1);
		cursor = link_start;
	}
}

bool HTMLSurfaceHCSRBackend::_ensure_renderer() {
	if (renderer != nullptr) {
		return true;
	}

	hcsr_renderer_config_t config = {};
	config.struct_size = sizeof(config);
	config.width = MAX(1, size.x);
	config.height = MAX(1, size.y);
	config.device_scale = device_scale_factor;
	config.pixel_format = HCSR_PIXEL_FORMAT_BGRA8;
	config.transparent_background = transparent_background ? 1 : 0;
	config.render_backend = render_backend;
	const hcsr_status_t status = hcsr_renderer_create(&config, &renderer);
	if (status != HCSR_STATUS_OK || renderer == nullptr) {
		terminal_failure = true;
		terminal_failure_reason = "HCSR could not create its static renderer instance.";
		ERR_PRINT(terminal_failure_reason);
		return false;
	}

#ifdef DEBUG_ENABLED
	if (hcsr_renderer_set_performance_profiling_enabled(renderer, 1) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected performance profiling configuration");
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
		return false;
	}
#endif

	viewport_dirty = true;
	document_dirty = true;
	if (hcsr_renderer_set_backdrop_metadata_enabled(renderer, backdrop_filter_enabled ? 1 : 0) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected backdrop metadata configuration");
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
		return false;
	}
	bool configured = true;
	switch (render_backend) {
		case HCSR_RENDER_BACKEND_D3D12:
			configured = _configure_d3d12_device();
			break;
		case HCSR_RENDER_BACKEND_VULKAN:
			configured = _configure_vulkan_device();
			break;
		case HCSR_RENDER_BACKEND_METAL:
			configured = _configure_metal_device();
			break;
		case HCSR_RENDER_BACKEND_CPU:
			break;
	}
	if (!configured) {
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
		return false;
	}
	return true;
}

void HTMLSurfaceHCSRBackend::_configure_d3d12_device_on_render_thread() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (renderer == nullptr || rendering_device == nullptr) {
		terminal_failure_reason = "Godot did not expose a RenderingDevice for HCSR D3D12.";
		return;
	}

	hcsr_d3d12_device_t device = {};
	device.struct_size = sizeof(device);
	device.device = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	device.command_queue = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	if (device.device == nullptr || device.command_queue == nullptr) {
		terminal_failure_reason = "Godot's active renderer did not expose D3D12 device and command-queue handles.";
		return;
	}

	if (hcsr_renderer_set_d3d12_device(renderer, &device) != HCSR_STATUS_OK) {
		_record_error("HCSR could not borrow Godot's D3D12 device");
		return;
	}
	gpu_device_configured = true;
}

void HTMLSurfaceHCSRBackend::_configure_d3d12_device_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_configure_d3d12_device_on_render_thread();
	}
}

bool HTMLSurfaceHCSRBackend::_configure_d3d12_device() {
	if (gpu_device_configured) {
		return true;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR D3D12 configuration.";
		return false;
	}
	if (rendering_server->is_on_render_thread()) {
		_configure_d3d12_device_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_configure_d3d12_device_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
	return gpu_device_configured;
}

void HTMLSurfaceHCSRBackend::_configure_vulkan_device_on_render_thread() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (renderer == nullptr || rendering_device == nullptr) {
		terminal_failure_reason = "Godot did not expose a RenderingDevice for HCSR Vulkan.";
		return;
	}

	hcsr_vulkan_device_t device = {};
	device.struct_size = sizeof(device);
	device.instance = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT);
	device.physical_device = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	device.device = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	device.queue = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	device.queue_family_index = (uint32_t)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY);
	if (device.physical_device == nullptr || device.device == nullptr || device.queue == nullptr) {
		terminal_failure_reason = "Godot's active renderer did not expose Vulkan device and queue handles.";
		return;
	}

	if (hcsr_renderer_set_vulkan_device(renderer, &device) != HCSR_STATUS_OK) {
		_record_error("HCSR could not borrow Godot's Vulkan device");
		return;
	}
	gpu_device_configured = true;
}

void HTMLSurfaceHCSRBackend::_configure_vulkan_device_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_configure_vulkan_device_on_render_thread();
	}
}

bool HTMLSurfaceHCSRBackend::_configure_vulkan_device() {
	if (gpu_device_configured) {
		return true;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR Vulkan configuration.";
		return false;
	}
	if (rendering_server->is_on_render_thread()) {
		_configure_vulkan_device_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_configure_vulkan_device_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
	return gpu_device_configured;
}

void HTMLSurfaceHCSRBackend::_configure_metal_device_on_render_thread() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (renderer == nullptr || rendering_device == nullptr) {
		terminal_failure_reason = "Godot did not expose a RenderingDevice for HCSR Metal.";
		return;
	}

	hcsr_metal_device_t device = {};
	device.struct_size = sizeof(device);
	device.device = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	device.command_queue = (void *)rendering_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	if (device.device == nullptr || device.command_queue == nullptr) {
		terminal_failure_reason = "Godot's Metal driver did not expose its MTLDevice and MTLCommandQueue.";
		return;
	}

	if (hcsr_renderer_set_metal_device(renderer, &device) != HCSR_STATUS_OK) {
		_record_error("HCSR could not borrow Godot's Metal device");
		return;
	}
	gpu_device_configured = true;
}

void HTMLSurfaceHCSRBackend::_configure_metal_device_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_configure_metal_device_on_render_thread();
	}
}

bool HTMLSurfaceHCSRBackend::_configure_metal_device() {
	if (gpu_device_configured) {
		return true;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR Metal configuration.";
		return false;
	}
	if (rendering_server->is_on_render_thread()) {
		_configure_metal_device_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_configure_metal_device_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
	return gpu_device_configured;
}

void HTMLSurfaceHCSRBackend::_record_error(const String &p_context) {
	const char *last_error = renderer != nullptr ? hcsr_renderer_last_error(renderer) : nullptr;
	terminal_failure_reason = p_context;
	if (last_error != nullptr && last_error[0] != '\0') {
		terminal_failure_reason += ": " + String::utf8(last_error);
	}
	terminal_failure = true;
	if (terminal_failure_reason != last_reported_error) {
		last_reported_error = terminal_failure_reason;
		ERR_PRINT(terminal_failure_reason);
	}
}

bool HTMLSurfaceHCSRBackend::_sync_viewport() {
	if (!viewport_dirty) {
		return true;
	}
	if (!_ensure_renderer()) {
		return false;
	}
	if (hcsr_renderer_set_viewport(renderer, MAX(1, size.x), MAX(1, size.y)) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot viewport");
		return false;
	}
	viewport_dirty = false;
	return true;
}

bool HTMLSurfaceHCSRBackend::_load_document_source(String &r_html, String &r_document_path, String &r_asset_root) const {
	if (document.is_null() || !document->is_source_valid()) {
		return false;
	}

	String html = document->get_html();
	String document_path = document->get_html_file();
	if (html.is_empty() && !document_path.is_empty()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, document_path, asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		html = String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
	}
	if (html.strip_edges().is_empty()) {
		return false;
	}

	String resource_root = document->get_resource_root();
	if (resource_root.is_empty()) {
		resource_root = "res://";
	}
	r_asset_root = ProjectSettings::get_singleton()->globalize_path(resource_root.begins_with("res://") ? String("res://") : resource_root);

	String css;
	if (r_asset_root.is_empty()) {
		hcsr_inline_packed_stylesheets(document, html, css);
	}
	for (const String &css_file : document->get_css_files()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, css_file, asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		css += "\n" + String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size()) + "\n";
	}
	css += document->get_css();
	r_html = hcsr_globalize_res_urls(hcsr_inject_document_style(html, document->get_background_color(), css));
	if (document_path.is_empty()) {
		document_path = resource_root.path_join("hcsr_document.html");
	}
	r_document_path = ProjectSettings::get_singleton()->globalize_path(document_path);
	if (r_document_path.is_empty()) {
		r_document_path = "/hcsr/" + document_path.get_file();
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_load_document_package(PackedByteArray &r_package) const {
	if (document.is_null()) {
		return false;
	}
	String package_path = document->get_package_file();
#if !defined(TOOLS_ENABLED) && !defined(DEBUG_ENABLED)
	if (package_path.is_empty() && !document->get_html_file().is_empty()) {
		package_path = document->get_html_file().get_basename() + ".hcsrpkg";
	}
#endif
	if (package_path.is_empty()) {
		return false;
	}
	HTMLAssetResource asset;
	String error;
	if (HTMLGodotAssetProvider::load_asset(document, package_path, asset, &error) != OK) {
		ERR_PRINT(error);
		return false;
	}
	r_package = asset.bytes;
	return !r_package.is_empty();
}

bool HTMLSurfaceHCSRBackend::_sync_document() {
	if (!document_dirty) {
		return true;
	}
	if (!_ensure_renderer()) {
		return false;
	}
	PackedByteArray package;
	if (_load_document_package(package)) {
		if (hcsr_renderer_set_document_package(renderer, package.ptr(), package.size()) != HCSR_STATUS_OK) {
			_record_error("HCSR rejected the compiled Godot HTML package");
			return false;
		}
		document_dirty = false;
		terminal_failure = false;
		terminal_failure_reason = String();
		return true;
	}
	String html;
	String document_path;
	String asset_root;
	if (!_load_document_source(html, document_path, asset_root)) {
		return false;
	}
	CharString root_utf8 = asset_root.utf8();
	CharString path_utf8 = document_path.utf8();
	CharString html_utf8 = html.utf8();
	if (!asset_root.is_empty() && hcsr_renderer_set_asset_root(renderer, root_utf8.ptr()) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot asset root '" + asset_root + "'");
		return false;
	}
	if (hcsr_renderer_set_document(renderer, path_utf8.ptr(), html_utf8.ptr()) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot HTMLDocument at '" + document_path + "'");
		return false;
	}
	document_dirty = false;
	terminal_failure = false;
	terminal_failure_reason = String();
	return true;
}

Error HTMLSurfaceHCSRBackend::_set_input() {
	if (!_ensure_renderer()) {
		return ERR_CANT_CREATE;
	}
	return hcsr_renderer_set_input(
			renderer,
			Math::floor(pointer_position.x),
			Math::floor(pointer_position.y),
			primary_button_pressed ? 1 : 0,
			scroll_offset.x,
			scroll_offset.y) == HCSR_STATUS_OK
			? OK
			: FAILED;
}

bool HTMLSurfaceHCSRBackend::_clamp_scroll_offset_to_content(bool &r_changed) {
	r_changed = false;
	int32_t content_width = 0;
	int32_t content_height = 0;
	if (hcsr_renderer_get_content_size(renderer, &content_width, &content_height) != HCSR_STATUS_OK) {
		_record_error("HCSR could not report the document content size");
		return false;
	}

	const Vector2i clamped_offset(
			CLAMP(scroll_offset.x, 0, MAX(0, content_width - size.x)),
			CLAMP(scroll_offset.y, 0, MAX(0, content_height - size.y)));
	if (clamped_offset == scroll_offset) {
		return true;
	}
	scroll_offset = clamped_offset;
	r_changed = true;
	return _set_input() == OK;
}

Error HTMLSurfaceHCSRBackend::_apply_dom_mutation(hcsr_dom_mutation_operation_kind_t p_operation, hcsr_dom_mutation_target_kind_t p_target_kind, const String &p_target, const String &p_name, const String &p_value, hcsr_dom_mutation_content_kind_t p_content_kind) {
	if (!_sync_document()) {
		return ERR_UNAVAILABLE;
	}

	const CharString target_utf8 = p_target.utf8();
	const CharString name_utf8 = p_name.utf8();
	const String normalized_value = p_content_kind == HCSR_DOM_MUTATION_CONTENT_HTML ? hcsr_globalize_res_urls(p_value) : p_value;
	const CharString value_utf8 = normalized_value.utf8();
	hcsr_dom_mutation_t mutation = {};
	mutation.struct_size = sizeof(mutation);
	mutation.operation = p_operation;
	mutation.target_kind = p_target_kind;
	mutation.match_policy = HCSR_DOM_MUTATION_REQUIRE_MATCH;
	mutation.content_kind = p_content_kind;
	mutation.target_utf8 = p_target.is_empty() ? nullptr : target_utf8.ptr();
	mutation.name_utf8 = p_name.is_empty() ? nullptr : name_utf8.ptr();
	mutation.value_utf8 = normalized_value.is_empty() ? "" : value_utf8.ptr();
	mutation.child_index = -1;

	uint8_t changed = 0;
	if (hcsr_renderer_apply_dom_mutations(renderer, &mutation, 1, &changed) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected a Godot DOM mutation");
		return FAILED;
	}
	return OK;
}

void HTMLSurfaceHCSRBackend::_ensure_gpu_texture_imported_on_render_thread() {
	if (gpu_texture_rid.is_valid() || native_gpu_texture == nullptr || native_gpu_size.x <= 0 || native_gpu_size.y <= 0) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR GPU texture import.";
		return;
	}
#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
	MTL::Texture *retained_metal_texture = nullptr;
	if (render_backend == HCSR_RENDER_BACKEND_METAL) {
		// Godot's Metal extension-texture path adopts one reference. Preserve HCSR's
		// ownership by transferring a separate retain to the imported RID.
		retained_metal_texture = reinterpret_cast<MTL::Texture *>(native_gpu_texture);
		retained_metal_texture->retain();
	}
#endif
	gpu_texture_rid = rendering_server->texture_create_from_native_handle(
			RenderingServerEnums::TEXTURE_TYPE_2D,
			Image::FORMAT_RGBA8,
			(uint64_t)native_gpu_texture,
			native_gpu_size.x,
			native_gpu_size.y,
			1,
			1);
	if (!gpu_texture_rid.is_valid()) {
#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
		if (retained_metal_texture != nullptr) {
			retained_metal_texture->release();
		}
#endif
		terminal_failure_reason = "Godot could not import HCSR's host-device GPU texture.";
		return;
	}
	if (gpu_texture.is_null()) {
		gpu_texture.instantiate();
	}
	gpu_texture->set_external_texture(gpu_texture_rid, native_gpu_size, true);
}

void HTMLSurfaceHCSRBackend::_detach_gpu_texture_import_on_render_thread() {
	if (gpu_texture.is_valid()) {
		gpu_texture->clear_external_texture();
	}
	if (gpu_texture_rid.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free_rid(gpu_texture_rid);
	}
	gpu_texture_rid = RID();
}

void HTMLSurfaceHCSRBackend::_detach_gpu_texture_import_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_detach_gpu_texture_import_on_render_thread();
	}
}

void HTMLSurfaceHCSRBackend::_detach_gpu_texture_import() {
	if (!gpu_texture_rid.is_valid()) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		gpu_texture_rid = RID();
		return;
	}
	if (rendering_server->is_on_render_thread()) {
		_detach_gpu_texture_import_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_detach_gpu_texture_import_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
}

void HTMLSurfaceHCSRBackend::_destroy_renderer_on_render_thread() {
	if (renderer != nullptr) {
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
	}
}

void HTMLSurfaceHCSRBackend::_destroy_renderer_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_destroy_renderer_on_render_thread();
	}
}

void HTMLSurfaceHCSRBackend::_render_gpu_frame_on_render_thread(hcsr_gpu_frame_packet_t *p_packet) {
	gpu_render_succeeded = false;
	if (p_packet == nullptr) {
		terminal_failure_reason = "HCSR received an invalid prepared Godot GPU frame.";
		return;
	}
	if (renderer == nullptr) {
		hcsr_renderer_release_gpu_frame_packet(p_packet);
		return;
	}
	if (gpu_texture_rid.is_valid() && native_gpu_size != size) {
		// Keep presenting the previous texture while the resized logical frame is
		// prepared. Release it only here, immediately before the replacement native
		// resource is created and imported in this same render-thread callback.
		_detach_gpu_texture_import_on_render_thread();
		native_gpu_texture = nullptr;
		native_gpu_generation = 0;
		native_gpu_size = Size2i();
	}
	hcsr_gpu_frame_t output = {};
	output.struct_size = sizeof(output);
	if (hcsr_renderer_submit_gpu_frame(renderer, p_packet, &output) != HCSR_STATUS_OK) {
		_record_error("HCSR could not submit the prepared Godot GPU frame");
		return;
	}
	_update_performance_profile();
	if (output.native_texture == nullptr || output.width <= 0 || output.height <= 0 || output.render_backend != render_backend) {
		_record_error("HCSR returned an invalid Godot GPU frame");
		return;
	}
	if (native_gpu_texture != output.native_texture || native_gpu_generation != output.resource_generation || native_gpu_size != Size2i(output.width, output.height)) {
		_detach_gpu_texture_import_on_render_thread();
		native_gpu_texture = output.native_texture;
		native_gpu_generation = output.resource_generation;
		native_gpu_size = Size2i(output.width, output.height);
	}
	_ensure_gpu_texture_imported_on_render_thread();
	gpu_render_succeeded = gpu_texture_rid.is_valid();
}

void HTMLSurfaceHCSRBackend::_render_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr, uint64_t p_packet_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	hcsr_gpu_frame_packet_t *packet = (hcsr_gpu_frame_packet_t *)p_packet_ptr;
	if (backend != nullptr) {
		backend->_render_gpu_frame_on_render_thread(packet);
	} else if (packet != nullptr) {
		hcsr_renderer_release_gpu_frame_packet(packet);
	}
}

bool HTMLSurfaceHCSRBackend::_render_gpu_frame() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR GPU rendering.";
		return false;
	}
	hcsr_gpu_frame_packet_t *packet = nullptr;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (hcsr_renderer_prepare_gpu_frame(renderer, timeline_time_seconds, &packet) != HCSR_STATUS_OK || packet == nullptr) {
			_record_error("HCSR could not prepare the Godot GPU frame");
			return false;
		}
		bool scroll_offset_changed = false;
		if (!_clamp_scroll_offset_to_content(scroll_offset_changed)) {
			hcsr_renderer_release_gpu_frame_packet(packet);
			return false;
		}
		if (attempt > 0 || !scroll_offset_changed) {
			break;
		}
		hcsr_renderer_release_gpu_frame_packet(packet);
		packet = nullptr;
	}
	if (rendering_server->is_on_render_thread()) {
		_render_gpu_frame_on_render_thread(packet);
		return gpu_render_succeeded;
	} else {
		const bool needs_texture_publish = !gpu_texture_rid.is_valid() || native_gpu_size != size;
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_render_gpu_frame_on_render_thread_callback).bind((uint64_t)this, (uint64_t)packet));
		if (needs_texture_publish) {
			// Initial creation and resize publish the replacement texture atomically.
			// Steady-state frames remain ordered without synchronizing the game thread.
			rendering_server->sync();
			return gpu_render_succeeded;
		}
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_render_frame() {
	if (!_sync_viewport() || !_sync_document() || _set_input() != OK) {
		return false;
	}
	if (render_backend != HCSR_RENDER_BACKEND_CPU) {
		const bool rendered = _render_gpu_frame();
		if (rendered) {
			_read_backdrop_filter_regions();
		}
		return rendered;
	}
	hcsr_frame_t output = {};
	for (int attempt = 0; attempt < 2; attempt++) {
		output = {};
		output.struct_size = sizeof(output);
		if (hcsr_renderer_render_frame(renderer, timeline_time_seconds, &output) != HCSR_STATUS_OK) {
			_record_error("HCSR could not render the Godot frame");
			return false;
		}
		_update_performance_profile();
		bool scroll_offset_changed = false;
		if (!_clamp_scroll_offset_to_content(scroll_offset_changed)) {
			hcsr_renderer_release_frame(renderer, &output);
			return false;
		}
		if (attempt > 0 || !scroll_offset_changed) {
			break;
		}
		hcsr_renderer_release_frame(renderer, &output);
	}
	if (output.pixels == nullptr || output.width <= 0 || output.height <= 0 || output.stride < output.width * 4) {
		hcsr_renderer_release_frame(renderer, &output);
		_record_error("HCSR returned an invalid frame");
		return false;
	}

	HTMLCPUFrame frame;
	frame.size = Size2i(output.width, output.height);
	frame.stride = output.stride;
	frame.pixel_format = HTML_FRAME_PIXEL_FORMAT_BGRA8;
	frame.premultiplied_alpha = output.premultiplied_alpha != 0;
	frame.damage.full_frame = true;
	frame.pixels.resize(output.stride * output.height);
	memcpy(frame.pixels.ptrw(), output.pixels, frame.pixels.size());
	hcsr_renderer_release_frame(renderer, &output);
	const bool rendered = submit_cpu_frame(frame) == OK;
	if (rendered) {
		_read_backdrop_filter_regions();
	}
	return rendered;
}

void HTMLSurfaceHCSRBackend::_read_backdrop_filter_regions() {
	frame_metadata.backdrop_filter_regions.clear();
	if (!backdrop_filter_enabled || renderer == nullptr) {
		return;
	}

	const uint32_t region_count = hcsr_renderer_backdrop_filter_region_count(renderer);
	for (uint32_t region_index = 0; region_index < region_count; region_index++) {
		hcsr_backdrop_filter_region_t source = {};
		source.struct_size = sizeof(source);
		if (hcsr_renderer_get_backdrop_filter_region(renderer, region_index, &source) != HCSR_STATUS_OK || source.right <= source.left || source.bottom <= source.top) {
			continue;
		}

		HTMLBackdropFilterRegion region;
		region.bounds = Rect2(source.left, source.top, source.right - source.left, source.bottom - source.top);
		region.blur_radius_css_px = source.blur_radius_css_px;
		region.border_radius_top_left = source.border_radius_top_left;
		region.border_radius_top_right = source.border_radius_top_right;
		region.border_radius_bottom_right = source.border_radius_bottom_right;
		region.border_radius_bottom_left = source.border_radius_bottom_left;
		region.opacity = source.opacity;
		region.flags = source.flags;
		const uint32_t operation_count = MIN(source.filter_operation_count, (uint32_t)HCSR_MAX_BACKDROP_FILTER_OPERATIONS);
		for (uint32_t operation_index = 0; operation_index < operation_count; operation_index++) {
			const int32_t operation_type = source.filter_operation_types[operation_index];
			if (operation_type < HTML_BACKDROP_FILTER_OPERATION_BLUR || operation_type > HTML_BACKDROP_FILTER_OPERATION_OPACITY) {
				region.flags |= HTML_BACKDROP_FILTER_UNSUPPORTED_FILTER_OP;
				continue;
			}
			HTMLBackdropFilterOperation operation;
			operation.type = (HTMLBackdropFilterOperationType)operation_type;
			operation.amount = source.filter_operation_amounts[operation_index];
			region.filter_operations.push_back(operation);
		}
		frame_metadata.backdrop_filter_regions.push_back(region);
	}
}

void HTMLSurfaceHCSRBackend::_update_performance_profile() {
#ifdef DEBUG_ENABLED
	if (renderer == nullptr) {
		return;
	}
	hcsr_performance_profile_t profile = {};
	profile.struct_size = sizeof(profile);
	if (hcsr_renderer_get_performance_profile(renderer, &profile) == HCSR_STATUS_OK) {
		HCSRPerformanceMonitor::update((uint64_t)this, profile);
	}
#endif
}

void HTMLSurfaceHCSRBackend::mark_document_dirty() {
	document_dirty = true;
	last_reported_error = String();
}

void HTMLSurfaceHCSRBackend::set_size(const Size2i &p_size) {
	const Size2i new_size(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	HTMLSurfaceCPUBackend::set_size(new_size);
	viewport_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_device_scale_factor(float p_device_scale_factor) {
	const float new_scale = CLAMP(Math::is_finite(p_device_scale_factor) && p_device_scale_factor > 0.0f ? p_device_scale_factor : 1.0f, 0.01f, 8.0f);
	if (!Math::is_equal_approx(device_scale_factor, new_scale)) {
		device_scale_factor = new_scale;
		viewport_dirty = true;
	}
}

void HTMLSurfaceHCSRBackend::set_document(const Ref<HTMLDocument> &p_document) {
	if (document != p_document) {
		document = p_document;
		document_dirty = true;
		last_reported_error = String();
	}
}

void HTMLSurfaceHCSRBackend::set_background_color(const Color &p_background_color) {
	if (background_color != p_background_color) {
		HTMLSurfaceCPUBackend::set_background_color(p_background_color);
		document_dirty = true;
	}
}

void HTMLSurfaceHCSRBackend::set_backdrop_filter_enabled(bool p_enabled) {
	if (backdrop_filter_enabled == p_enabled) {
		return;
	}
	backdrop_filter_enabled = p_enabled;
	frame_metadata.backdrop_filter_regions.clear();
	if (renderer != nullptr && hcsr_renderer_set_backdrop_metadata_enabled(renderer, p_enabled ? 1 : 0) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected backdrop metadata configuration");
	}
}

Error HTMLSurfaceHCSRBackend::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	timeline_time_seconds = MAX(0.0, p_timeline_time_seconds);
	if (r_needs_output != nullptr) {
		*r_needs_output = true;
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = false;
	}
	return document.is_valid() && _ensure_renderer() ? OK : ERR_UNAVAILABLE;
}

void HTMLSurfaceHCSRBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	if (!_render_frame()) {
		clear_to_background();
	}
}

bool HTMLSurfaceHCSRBackend::has_terminal_render_failure() const {
	return terminal_failure;
}

String HTMLSurfaceHCSRBackend::get_terminal_render_failure_reason() const {
	return terminal_failure_reason;
}

Error HTMLSurfaceHCSRBackend::mouse_move(const Point2 &p_position, int p_modifiers) {
	(void)p_modifiers;
	pointer_position = p_position;
	return _set_input();
}

Error HTMLSurfaceHCSRBackend::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	pointer_position = p_position;
	if (p_button == HTML_SURFACE_MOUSE_BUTTON_LEFT) {
		primary_button_pressed = true;
	}
	return _set_input();
}

Error HTMLSurfaceHCSRBackend::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	pointer_position = p_position;
	if (p_button == HTML_SURFACE_MOUSE_BUTTON_LEFT) {
		primary_button_pressed = false;
	}
	return _set_input();
}

Error HTMLSurfaceHCSRBackend::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	pointer_position = p_position;
	// HTMLView supplies a signed pixel delta. Positive values move the viewport
	// toward increasing document coordinates.
	scroll_offset.x = MAX(0, scroll_offset.x + Math::round(p_delta.x));
	scroll_offset.y = MAX(0, scroll_offset.y + Math::round(p_delta.y));
	return _set_input();
}

bool HTMLSurfaceHCSRBackend::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	if (renderer == nullptr) {
		return false;
	}

	hcsr_element_hit_t source = {};
	source.struct_size = sizeof(source);
	if (hcsr_renderer_hit_test(renderer, Math::floor(p_position.x), Math::floor(p_position.y), &source) != HCSR_STATUS_OK || source.element_key_utf8[0] == '\0') {
		return false;
	}

	r_hit = HTMLElementHit();
	r_hit.element_id = StringName(String::utf8(source.element_id_utf8));
	r_hit.tag_name = StringName(String::utf8(source.tag_name_utf8));
	r_hit.bounds = Rect2i(source.left, source.top, MAX(0, source.right - source.left), MAX(0, source.bottom - source.top));
	r_hit.disabled = source.disabled != 0;
	const String action = String::utf8(source.action_utf8);
	if (!action.is_empty()) {
		HTMLElementAttribute attribute;
		attribute.name = SNAME("data-godot-action");
		attribute.value = action;
		r_hit.attributes.push_back(attribute);
	}
	return true;
}

Error HTMLSurfaceHCSRBackend::set_element_text(const StringName &p_id, const String &p_text) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_TEXT, HCSR_DOM_TARGET_ID, String(p_id), String(), p_text);
}

Error HTMLSurfaceHCSRBackend::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_INNER_CONTENT, HCSR_DOM_TARGET_ID, String(p_id), String(), p_html_fragment, HCSR_DOM_MUTATION_CONTENT_HTML);
}

Error HTMLSurfaceHCSRBackend::set_body_inner_html(const String &p_html_fragment) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_INNER_CONTENT, HCSR_DOM_TARGET_ROOT_BODY, String(), String(), p_html_fragment, HCSR_DOM_MUTATION_CONTENT_HTML);
}

Error HTMLSurfaceHCSRBackend::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_ATTRIBUTE, HCSR_DOM_TARGET_ID, String(p_id), String(p_name), p_value);
}

Error HTMLSurfaceHCSRBackend::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_REMOVE_ATTRIBUTE, HCSR_DOM_TARGET_ID, String(p_id), String(p_name), String());
}

Error HTMLSurfaceHCSRBackend::set_element_style(const StringName &p_id, const String &p_css_text) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_ATTRIBUTE, HCSR_DOM_TARGET_ID, String(p_id), "style", p_css_text);
}

Error HTMLSurfaceHCSRBackend::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	return _apply_dom_mutation(HCSR_DOM_MUTATION_SET_TEXT, HCSR_DOM_TARGET_ID, String(p_style_id), String(), p_css_text);
}

void HTMLSurfaceHCSRBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	r_metadata = frame_metadata;
}

Ref<Texture2D> HTMLSurfaceHCSRBackend::get_texture() const {
	if (render_backend == HCSR_RENDER_BACKEND_CPU) {
		return HTMLSurfaceCPUBackend::get_texture();
	}
	return gpu_texture;
}

Ref<HTMLTexture2D> HTMLSurfaceHCSRBackend::get_html_texture() const {
	return render_backend == HCSR_RENDER_BACKEND_CPU ? HTMLSurfaceCPUBackend::get_html_texture() : gpu_texture;
}

HTMLSurfaceHCSRBackend::HTMLSurfaceHCSRBackend(hcsr_render_backend_t p_render_backend) :
		render_backend(p_render_backend) {
}

HTMLSurfaceHCSRBackend::~HTMLSurfaceHCSRBackend() {
#ifdef DEBUG_ENABLED
	HCSRPerformanceMonitor::remove((uint64_t)this);
#endif
	_detach_gpu_texture_import();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (render_backend == HCSR_RENDER_BACKEND_CPU || rendering_server == nullptr || rendering_server->is_on_render_thread()) {
		_destroy_renderer_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_destroy_renderer_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
}
