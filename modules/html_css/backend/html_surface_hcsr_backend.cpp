/**************************************************************************/
/*  html_surface_hcsr_backend.cpp                                        */
/**************************************************************************/

#include "html_surface_hcsr_backend.h"

#include "hcsr_performance_monitor.h"
#include "../bridge/html_asset_provider.h"

#include "core/config/project_settings.h"
#include "core/config/engine.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
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

static const uint64_t HCSR_REQUIRED_GODOT_GPU_CAPABILITIES =
		HCSR_GPU_CAPABILITY_ASYNC_COMPLETION_POLLING |
		HCSR_GPU_CAPABILITY_IMMUTABLE_GENERATION_METADATA |
		HCSR_GPU_CAPABILITY_DEVICE_IDENTITY |
		HCSR_GPU_CAPABILITY_OBSERVABLE_BATCH_CANCELLATION |
		HCSR_GPU_CAPABILITY_SUBMISSION_COMPLETION_TOKENS |
		HCSR_GPU_CAPABILITY_EXPLICIT_CONSUMER_RELEASE;

static String hcsr_inject_document_style(const String &p_html, const Color &p_background, const String &p_css) {
	String style = "<style data-godot-hcsr=\"true\">html { background-color: " + hcsr_color_to_css_rgba(p_background) + "; }\n";
	style += p_css.replace("</style", "<\\/style");
	style += "\n</style>";
	const String lower_html = p_html.to_lower();
	const int head_end = lower_html.find("</head>");
	if (head_end >= 0) {
		return p_html.insert(head_end, style);
	}
	const int html_start = lower_html.find("<html");
	const int html_open_end = html_start >= 0 ? p_html.find(">", html_start + 5) : -1;
	return html_open_end >= 0 ? p_html.insert(html_open_end + 1, "<head>" + style + "</head>") : p_html;
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
	input_state_cache.reset();

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
	gpu_device_configured = _validate_gpu_capabilities();
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
	gpu_device_configured = _validate_gpu_capabilities();
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
	gpu_device_configured = _validate_gpu_capabilities();
}

bool HTMLSurfaceHCSRBackend::_validate_gpu_capabilities() {
	gpu_capabilities = {};
	gpu_capabilities.struct_size = sizeof(gpu_capabilities);
	if (hcsr_renderer_get_gpu_capabilities(renderer, &gpu_capabilities) != HCSR_STATUS_OK) {
		_record_error("HCSR could not report its configured GPU capabilities");
		return false;
	}
	if (gpu_capabilities.render_backend != render_backend) {
		terminal_failure_reason = vformat("HCSR configured GPU backend %d but reported backend %d.", (int)render_backend, (int)gpu_capabilities.render_backend);
	} else if ((gpu_capabilities.flags & HCSR_REQUIRED_GODOT_GPU_CAPABILITIES) != HCSR_REQUIRED_GODOT_GPU_CAPABILITIES) {
		terminal_failure_reason = vformat("HCSR's configured GPU presenter is missing required Godot capabilities (reported=0x%s, required=0x%s).", String::num_uint64(gpu_capabilities.flags, 16), String::num_uint64(HCSR_REQUIRED_GODOT_GPU_CAPABILITIES, 16));
	} else if (gpu_capabilities.synchronization_mode != HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
		terminal_failure_reason = "HCSR's configured GPU presenter does not order producer work on Godot's engine queue.";
	} else if (gpu_capabilities.resource_retirement_mode != HCSR_GPU_RESOURCE_RETIREMENT_ENGINE_QUEUE_ORDERED) {
		terminal_failure_reason = "HCSR's configured GPU presenter cannot retire presentation resources on Godot's engine queue.";
	} else if (gpu_capabilities.maximum_prepared_packet_count == 0 || gpu_capabilities.presentation_pool_size < 2) {
		terminal_failure_reason = "HCSR's configured GPU presenter reported invalid asynchronous queue or presentation-pool limits.";
	} else {
		return true;
	}

	terminal_failure = true;
	if (terminal_failure_reason != last_reported_error) {
		last_reported_error = terminal_failure_reason;
		ERR_PRINT(terminal_failure_reason);
	}
	return false;
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
	const int physical_width = MAX(1, physical_size.x);
	const int physical_height = MAX(1, physical_size.y);
	if (hcsr_renderer_set_viewport_metrics(renderer, MAX(1, size.x), MAX(1, size.y), device_scale_factor, physical_width, physical_height) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot viewport metrics");
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
		pending_document_commits.clear();
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
	pending_document_commits.clear();
	terminal_failure = false;
	terminal_failure_reason = String();
	return true;
}

Error HTMLSurfaceHCSRBackend::_set_input() {
	if (!_ensure_renderer()) {
		return ERR_CANT_CREATE;
	}
	if (!input_state_cache.needs_synchronization(pointer_position, primary_button_pressed, scroll_offset)) {
		return OK;
	}
	const hcsr_status_t status = hcsr_renderer_set_input_css(
			renderer,
			pointer_position.x,
			pointer_position.y,
			primary_button_pressed ? 1 : 0,
			scroll_offset.x,
			scroll_offset.y);
	if (status != HCSR_STATUS_OK) {
		return FAILED;
	}
	input_state_cache.mark_synchronized(pointer_position, primary_button_pressed, scroll_offset);
	return OK;
}

bool HTMLSurfaceHCSRBackend::_clamp_scroll_offset_to_content(bool &r_changed, int p_content_width, int p_content_height) {
	r_changed = false;
	int32_t content_width = p_content_width;
	int32_t content_height = p_content_height;
	if (content_width < 0 || content_height < 0) {
		if (hcsr_renderer_get_content_size(renderer, &content_width, &content_height) != HCSR_STATUS_OK) {
			_record_error("HCSR could not report the document content size");
			return false;
		}
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

bool HTMLSurfaceHCSRBackend::_read_gpu_packet_metadata(hcsr_gpu_frame_packet_t *p_packet, PreparedGPUFrameMetadata &r_metadata, uint64_t &r_generation) {
	r_metadata = PreparedGPUFrameMetadata();
	r_generation = 0;
	hcsr_gpu_frame_packet_metadata_t source = {};
	source.struct_size = sizeof(source);
	if (hcsr_renderer_get_gpu_frame_packet_metadata(p_packet, &source) != HCSR_STATUS_OK || source.frame_generation == 0) {
		_record_error("HCSR could not provide immutable GPU packet metadata");
		return false;
	}

	r_generation = source.frame_generation;
	if (source.css_viewport_width <= 0 || source.css_viewport_height <= 0 || source.physical_width <= 0 || source.physical_height <= 0 || !Math::is_finite(source.device_scale) || source.device_scale <= 0.0) {
		_record_error("HCSR returned invalid immutable GPU packet viewport metadata");
		return false;
	}
	r_metadata.css_viewport_size = Size2i(source.css_viewport_width, source.css_viewport_height);
	r_metadata.physical_size = Size2i(source.physical_width, source.physical_height);
	r_metadata.device_scale_factor = source.device_scale;
	r_metadata.frame_metadata.logical_size = r_metadata.css_viewport_size;
	r_metadata.frame_metadata.physical_size = r_metadata.physical_size;
	r_metadata.frame_metadata.device_scale_factor = r_metadata.device_scale_factor;
	r_metadata.frame_metadata.generation = source.frame_generation;
	r_metadata.frame_metadata.host_frame_number = source.host_frame_number;
	r_metadata.frame_metadata.timeline_time_seconds = source.timeline_time_seconds;
	r_metadata.viewport_revision = viewport_revision.get();
	r_metadata.semantic_state_revision = semantic_state_revision.get();
	r_metadata.frame_request_revision = frame_request_revision.get();
	if (r_metadata.css_viewport_size != size
			|| r_metadata.physical_size != physical_size
			|| !Math::is_equal_approx(r_metadata.device_scale_factor, device_scale_factor)) {
		// Live resize can supersede a semantic packet while it is being prepared.
		// The packet is safely abandoned by the caller; retain an explicit request
		// for the current viewport instead of waiting for unrelated input.
		gpu_follow_up_frame_requested.set();
		return false;
	}
	r_metadata.content_width = source.content_width;
	r_metadata.content_height = source.content_height;
	r_metadata.scroll_offset = Point2(source.scroll_x, source.scroll_y);
	for (uint32_t region_index = 0; region_index < source.backdrop_filter_region_count; region_index++) {
		hcsr_backdrop_filter_region_t region_source = {};
		region_source.struct_size = sizeof(region_source);
		if (hcsr_renderer_get_gpu_frame_packet_backdrop_filter_region(p_packet, region_index, &region_source) != HCSR_STATUS_OK
				|| region_source.frame_generation != source.frame_generation
				|| region_source.right <= region_source.left
				|| region_source.bottom <= region_source.top) {
			_record_error("HCSR returned inconsistent GPU packet backdrop metadata");
			return false;
		}

		HTMLBackdropFilterRegion region;
		region.bounds = Rect2(region_source.left, region_source.top, region_source.right - region_source.left, region_source.bottom - region_source.top);
		region.blur_radius_css_px = region_source.blur_radius_css_px;
		region.border_radius_top_left = region_source.border_radius_top_left;
		region.border_radius_top_right = region_source.border_radius_top_right;
		region.border_radius_bottom_right = region_source.border_radius_bottom_right;
		region.border_radius_bottom_left = region_source.border_radius_bottom_left;
		region.opacity = region_source.opacity;
		region.flags = region_source.flags;
		const uint32_t operation_count = MIN(region_source.filter_operation_count, (uint32_t)HCSR_MAX_BACKDROP_FILTER_OPERATIONS);
		for (uint32_t operation_index = 0; operation_index < operation_count; operation_index++) {
			const int32_t operation_type = region_source.filter_operation_types[operation_index];
			if (operation_type < HTML_BACKDROP_FILTER_OPERATION_BLUR || operation_type > HTML_BACKDROP_FILTER_OPERATION_OPACITY) {
				region.flags |= HTML_BACKDROP_FILTER_UNSUPPORTED_FILTER_OP;
				continue;
			}
			HTMLBackdropFilterOperation operation;
			operation.type = (HTMLBackdropFilterOperationType)operation_type;
			operation.amount = region_source.filter_operation_amounts[operation_index];
			region.filter_operations.push_back(operation);
		}
		r_metadata.frame_metadata.backdrop_filter_regions.push_back(region);
	}
	if (hcsr_gpu_frame_packet_retain_hit_test_snapshot(p_packet, &r_metadata.hit_test_snapshot) != HCSR_STATUS_OK || r_metadata.hit_test_snapshot == nullptr) {
		_release_gpu_packet_metadata(r_metadata);
		_record_error("HCSR could not provide the GPU packet hit-test snapshot");
		return false;
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_set_host_frame_context() {
	hcsr_host_frame_context_t context = {};
	context.struct_size = sizeof(context);
	// HCSR reserves zero for hosts without frame identity. Godot's process-frame
	// counter begins at zero, so publish it as a one-based opaque identity.
	context.host_frame_number = Engine::get_singleton() != nullptr ? Engine::get_singleton()->get_process_frames() + 1 : 0;
	context.timeline_time_seconds = timeline_time_seconds;
	if (hcsr_renderer_set_host_frame_context(renderer, &context) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot host-frame context");
		return false;
	}
	return true;
}

#ifdef DEBUG_ENABLED
bool HTMLSurfaceHCSRBackend::_diagnostic_is_capacity_blocked() {
	if (diagnostic_capacity_block_after_submissions <= 0 || diagnostic_capacity_block_frames <= 0) {
		return false;
	}
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	const int requested_arm = project_settings != nullptr ? int(project_settings->get_setting(
			"rendering/html_css/hcsr/testing/capacity_block_arm",
			0)) : 0;
	if (requested_arm <= 0) {
		return false;
	}
	if (requested_arm != diagnostic_capacity_block_arm) {
		diagnostic_capacity_block_arm = requested_arm;
		diagnostic_capacity_block_submission_baseline = diagnostic_successful_submissions;
		diagnostic_capacity_block_until_frame = 0;
		diagnostic_capacity_block_triggered = false;
	}
	const uint64_t host_frame = Engine::get_singleton() != nullptr ? Engine::get_singleton()->get_process_frames() + 1 : 0;
	if (!diagnostic_capacity_block_triggered
			&& diagnostic_successful_submissions - diagnostic_capacity_block_submission_baseline >= diagnostic_capacity_block_after_submissions) {
		diagnostic_capacity_block_triggered = true;
		diagnostic_capacity_block_until_frame = host_frame + diagnostic_capacity_block_frames;
	}
	return diagnostic_capacity_block_triggered && host_frame < diagnostic_capacity_block_until_frame;
}

void HTMLSurfaceHCSRBackend::_diagnostic_note_successful_submission() {
	diagnostic_successful_submissions++;
}
#endif

void HTMLSurfaceHCSRBackend::_release_gpu_packet_metadata(PreparedGPUFrameMetadata &r_metadata) {
	if (r_metadata.hit_test_snapshot != nullptr) {
		hcsr_hit_test_snapshot_release(r_metadata.hit_test_snapshot);
		r_metadata.hit_test_snapshot = nullptr;
	}
}

void HTMLSurfaceHCSRBackend::_stage_gpu_packet_metadata(uint64_t p_generation, PreparedGPUFrameMetadata &r_metadata) {
	hcsr_hit_test_snapshot_t *replaced_snapshot = nullptr;
	{
		MutexLock lock(prepared_gpu_frame_metadata_mutex);
		PreparedGPUFrameMetadata *existing = prepared_gpu_frame_metadata.getptr(p_generation);
		if (existing != nullptr) {
			replaced_snapshot = existing->hit_test_snapshot;
		}
		prepared_gpu_frame_metadata.insert(p_generation, r_metadata);
		r_metadata.hit_test_snapshot = nullptr;
	}
	if (replaced_snapshot != nullptr) {
		hcsr_hit_test_snapshot_release(replaced_snapshot);
	}
}

bool HTMLSurfaceHCSRBackend::_take_gpu_packet_metadata(uint64_t p_generation, PreparedGPUFrameMetadata &r_metadata) {
	Vector<hcsr_hit_test_snapshot_t *> retired_snapshots;
	{
		MutexLock lock(prepared_gpu_frame_metadata_mutex);
		const PreparedGPUFrameMetadata *metadata = prepared_gpu_frame_metadata.getptr(p_generation);
		if (metadata == nullptr) {
			return false;
		}
		r_metadata = *metadata;
		Vector<uint64_t> retired_generations;
		for (const KeyValue<uint64_t, PreparedGPUFrameMetadata> &entry : prepared_gpu_frame_metadata) {
			if (entry.key <= p_generation) {
				retired_generations.push_back(entry.key);
				if (entry.key != p_generation && entry.value.hit_test_snapshot != nullptr) {
					retired_snapshots.push_back(entry.value.hit_test_snapshot);
				}
			}
		}
		for (uint64_t generation : retired_generations) {
			prepared_gpu_frame_metadata.erase(generation);
		}
	}
	for (hcsr_hit_test_snapshot_t *snapshot : retired_snapshots) {
		hcsr_hit_test_snapshot_release(snapshot);
	}
	return true;
}

void HTMLSurfaceHCSRBackend::_discard_gpu_packet_metadata(uint64_t p_generation) {
	hcsr_hit_test_snapshot_t *snapshot = nullptr;
	{
		MutexLock lock(prepared_gpu_frame_metadata_mutex);
		PreparedGPUFrameMetadata *metadata = prepared_gpu_frame_metadata.getptr(p_generation);
		if (metadata != nullptr) {
			snapshot = metadata->hit_test_snapshot;
			prepared_gpu_frame_metadata.erase(p_generation);
		}
	}
	if (snapshot != nullptr) {
		hcsr_hit_test_snapshot_release(snapshot);
	}
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
		_record_error("HCSR could not apply a Godot DOM mutation");
		return FAILED;
	}
	if (changed != 0) {
		semantic_state_revision.increment();
	}
	return OK;
}

void HTMLSurfaceHCSRBackend::_retire_document_commits() {
	for (int commit_index = pending_document_commits.size() - 1; commit_index >= 0; commit_index--) {
		hcsr_document_commit_status_t status = {};
		status.struct_size = sizeof(status);
		if (hcsr_renderer_get_document_commit_status(renderer, pending_document_commits[commit_index], &status) != HCSR_STATUS_OK
				|| status.state == HCSR_DOCUMENT_COMMIT_PENDING) {
			continue;
		}

		pending_document_commits.remove_at(commit_index);
		if (status.state != HCSR_DOCUMENT_COMMIT_FAILED) {
			continue;
		}

		String failure = "HCSR rejected an asynchronous Godot DOM mutation";
		const char *last_error = hcsr_renderer_last_error(renderer);
		if (last_error != nullptr && last_error[0] != '\0') {
			failure += ": " + String::utf8(last_error);
		}
		if (failure != last_reported_error) {
			last_reported_error = failure;
			ERR_PRINT(failure);
		}
	}
}

void HTMLSurfaceHCSRBackend::_release_gpu_resource_after_retirement_callback(uint64_t p_renderer_ptr, uint64_t p_native_texture, uint64_t p_resource_generation, uint64_t p_frame_generation, uint64_t p_submission_token) {
	const hcsr_status_t status = hcsr_renderer_release_gpu_presentation_resource(
			(hcsr_renderer_t *)p_renderer_ptr,
			(void *)p_native_texture,
			p_resource_generation,
			p_frame_generation,
			p_submission_token);
	if (status != HCSR_STATUS_OK && status != HCSR_STATUS_INVALID_ARGUMENT) {
		ERR_PRINT("HCSR could not retire a Godot-consumed GPU presentation resource.");
	}
}

void HTMLSurfaceHCSRBackend::_release_presentation_output_resource_after_retirement_callback(uint64_t p_renderer_ptr, uint64_t p_output_ptr, uint64_t p_native_texture, uint64_t p_resource_generation, uint64_t p_frame_generation, uint64_t p_submission_token, uint64_t p_render_backend, uint64_t p_width, uint64_t p_height) {
	hcsr_gpu_frame_t frame = {};
	frame.struct_size = sizeof(frame);
	frame.render_backend = (hcsr_render_backend_t)p_render_backend;
	frame.native_texture = (void *)p_native_texture;
	frame.width = (int32_t)p_width;
	frame.height = (int32_t)p_height;
	frame.resource_generation = p_resource_generation;
	frame.frame_generation = p_frame_generation;
	frame.submission_token = p_submission_token;
	frame.texture_format = HCSR_GPU_TEXTURE_FORMAT_RGBA8_UNORM;
	frame.producer_completed = 1;
	frame.premultiplied_alpha = 0;
	const hcsr_status_t status = hcsr_renderer_release_presentation_output_resource(
			(hcsr_renderer_t *)p_renderer_ptr,
			(hcsr_presentation_output_t *)p_output_ptr,
			&frame);
	if (status != HCSR_STATUS_OK && status != HCSR_STATUS_INVALID_ARGUMENT) {
		ERR_PRINT("HCSR could not retire a Godot-consumed secondary presentation resource.");
	}
}

void HTMLSurfaceHCSRBackend::_destroy_presentation_output_after_retirement_callback(uint64_t p_renderer_ptr, uint64_t p_output_ptr) {
	if (p_renderer_ptr != 0 && p_output_ptr != 0) {
		hcsr_renderer_destroy_presentation_output((hcsr_renderer_t *)p_renderer_ptr, (hcsr_presentation_output_t *)p_output_ptr);
	}
}

void HTMLSurfaceHCSRBackend::_destroy_presentation_output_state_on_render_thread_callback(uint64_t p_backend_ptr, uint64_t p_state_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	PresentationOutputState *state = (PresentationOutputState *)p_state_ptr;
	if (backend != nullptr && state != nullptr) {
		backend->_destroy_presentation_output_state_on_render_thread(state);
	}
}

void HTMLSurfaceHCSRBackend::_destroy_renderer_after_retirement_callback(uint64_t p_renderer_ptr) {
	if (p_renderer_ptr != 0) {
		hcsr_renderer_destroy((hcsr_renderer_t *)p_renderer_ptr);
	}
}

void HTMLSurfaceHCSRBackend::_publish_external_texture_state_on_render_thread(RID p_texture) {
	if (render_backend != HCSR_RENDER_BACKEND_VULKAN || !p_texture.is_valid()) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (rendering_device == nullptr) {
		return;
	}
	const RID rd_texture = rendering_server->texture_get_rd_texture(p_texture, false);
	if (rd_texture.is_valid()) {
		rendering_device->external_texture_set_state(rd_texture, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL);
	}
}

void HTMLSurfaceHCSRBackend::_defer_gpu_resource_release_on_render_thread(const hcsr_gpu_frame_t &p_frame) {
	if (renderer == nullptr || p_frame.native_texture == nullptr || p_frame.resource_generation == 0 || p_frame.frame_generation == 0 || p_frame.submission_token == 0) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (rendering_device == nullptr) {
		return;
	}
	const Callable release_callback = callable_mp_static(&HTMLSurfaceHCSRBackend::_release_gpu_resource_after_retirement_callback).bind(
			(uint64_t)renderer,
			(uint64_t)p_frame.native_texture,
			p_frame.resource_generation,
			p_frame.frame_generation,
			p_frame.submission_token);
	const RID *cached = gpu_texture_import_cache.getptr((uint64_t)p_frame.native_texture);
	const RID imported = cached != nullptr ? *cached : ((uint64_t)native_gpu_texture == (uint64_t)p_frame.native_texture ? gpu_texture_rid : RID());
	const RID rd_texture = imported.is_valid() ? rendering_server->texture_get_rd_texture(imported, false) : RID();
	if (render_backend == HCSR_RENDER_BACKEND_VULKAN && rd_texture.is_valid()) {
		rendering_device->external_texture_defer_release(rd_texture, release_callback);
	} else {
		rendering_device->external_resource_defer_release(release_callback);
	}
}

void HTMLSurfaceHCSRBackend::_defer_presentation_output_resource_release_on_render_thread(hcsr_presentation_output_t *p_output, const hcsr_gpu_frame_t &p_frame, RID p_imported_texture) {
	if (renderer == nullptr || p_output == nullptr || p_frame.native_texture == nullptr || p_frame.submission_token == 0) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (rendering_device == nullptr) {
		return;
	}
	const Callable release_callback = callable_mp_static(&HTMLSurfaceHCSRBackend::_release_presentation_output_resource_after_retirement_callback).bind(
			(uint64_t)renderer,
			(uint64_t)p_output,
			(uint64_t)p_frame.native_texture,
			p_frame.resource_generation,
			p_frame.frame_generation,
			p_frame.submission_token,
			(uint64_t)p_frame.render_backend,
			(uint64_t)p_frame.width,
			(uint64_t)p_frame.height);
	const RID rd_texture = p_imported_texture.is_valid() ? rendering_server->texture_get_rd_texture(p_imported_texture, false) : RID();
	if (render_backend == HCSR_RENDER_BACKEND_VULKAN && rd_texture.is_valid()) {
		rendering_device->external_texture_defer_release(rd_texture, release_callback);
	} else {
		rendering_device->external_resource_defer_release(release_callback);
	}
}

void HTMLSurfaceHCSRBackend::_detach_presentation_output_on_render_thread(PresentationOutputState *p_state) {
	ERR_FAIL_NULL(p_state);
	if (p_state->texture.is_valid()) {
		p_state->texture->clear_external_texture();
	}
	if (p_state->active_frame.native_texture != nullptr) {
		const RID *active_imported = p_state->import_cache.getptr((uint64_t)p_state->active_frame.native_texture);
		_defer_presentation_output_resource_release_on_render_thread(
				p_state->output,
				p_state->active_frame,
				active_imported != nullptr ? *active_imported : p_state->texture_rid);
		p_state->active_frame = {};
	}
	for (const KeyValue<uint64_t, hcsr_gpu_frame_t> &entry : p_state->queued_frames) {
		_defer_presentation_output_resource_release_on_render_thread(p_state->output, entry.value);
	}
	p_state->queued_frames.clear();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr) {
		if (p_state->mipmapped_texture_rid.is_valid()) {
			rendering_server->free_rid(p_state->mipmapped_texture_rid);
			integration_counters.texture_resource_frees++;
		}
		for (const KeyValue<uint64_t, RID> &entry : p_state->import_cache) {
			if (entry.value.is_valid()) {
				rendering_server->free_rid(entry.value);
				integration_counters.texture_resource_frees++;
			}
		}
		if (p_state->texture_rid.is_valid() && !p_state->import_cache.has((uint64_t)p_state->native_texture)) {
			rendering_server->free_rid(p_state->texture_rid);
			integration_counters.texture_resource_frees++;
		}
	}
	_publish_integration_counters();
	p_state->import_cache.clear();
	p_state->submitted_generations.clear();
	p_state->texture_rid = RID();
	p_state->mipmapped_texture_rid = RID();
	p_state->native_texture = nullptr;
	p_state->native_generation = 0;
	p_state->native_size = Size2i();
}

void HTMLSurfaceHCSRBackend::_defer_presentation_output_destroy_on_render_thread(hcsr_presentation_output_t *p_output) {
	if (renderer == nullptr || p_output == nullptr) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
	if (rendering_device != nullptr) {
		rendering_device->external_resource_defer_release(callable_mp_static(&HTMLSurfaceHCSRBackend::_destroy_presentation_output_after_retirement_callback).bind((uint64_t)renderer, (uint64_t)p_output));
	} else {
		hcsr_renderer_destroy_presentation_output(renderer, p_output);
	}
}

void HTMLSurfaceHCSRBackend::_destroy_presentation_output_state_on_render_thread(PresentationOutputState *p_state) {
	ERR_FAIL_NULL(p_state);
	_detach_presentation_output_on_render_thread(p_state);
	if (p_state->texture.is_valid()) {
		p_state->texture.unref();
	}
	_defer_presentation_output_destroy_on_render_thread(p_state->output);
	memdelete(p_state);
}

bool HTMLSurfaceHCSRBackend::_ensure_presentation_outputs_on_render_thread() {
	bool topology_changed = false;
	MutexLock lock(presentation_outputs_mutex);
	for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
		PresentationOutputState *state = entry.value;
		if (state == nullptr) {
			continue;
		}
		if (state->output != nullptr && state->resize_pending) {
			_detach_presentation_output_on_render_thread(state);
			hcsr_presentation_output_t *retired_output = state->output;
			state->output = nullptr;
			state->resize_pending = false;
			_defer_presentation_output_destroy_on_render_thread(retired_output);
			topology_changed = true;
		}
		if (state->output != nullptr) {
			continue;
		}
		if (hcsr_renderer_create_presentation_output(renderer, state->requested_size.x, state->requested_size.y, &state->output) != HCSR_STATUS_OK || state->output == nullptr) {
			_record_error("HCSR could not create a secondary presentation output");
			return false;
		}
		topology_changed = true;
	}
	return !topology_changed;
}

void HTMLSurfaceHCSRBackend::_sync_presentation_output_topology_on_render_thread() {
	presentation_output_topology_sync_pending.clear();
	if (renderer == nullptr || render_backend == HCSR_RENDER_BACKEND_CPU) {
		return;
	}
	_ensure_presentation_outputs_on_render_thread();
	bool topology_synchronized = true;
	{
		MutexLock lock(presentation_outputs_mutex);
		for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
			const PresentationOutputState *state = entry.value;
			if (state != nullptr && (state->output == nullptr || state->resize_pending)) {
				topology_synchronized = false;
				break;
			}
		}
	}
	if (topology_synchronized) {
		presentation_output_topology_sync_required.clear();
	}
	gpu_follow_up_frame_requested.set();
}

void HTMLSurfaceHCSRBackend::_sync_presentation_output_topology_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_sync_presentation_output_topology_on_render_thread();
	}
}

void HTMLSurfaceHCSRBackend::_schedule_presentation_output_topology_sync() {
	if (render_backend == HCSR_RENDER_BACKEND_CPU || !presentation_output_topology_sync_required.is_set() || presentation_output_topology_sync_pending.is_set()) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return;
	}
	presentation_output_topology_sync_pending.set();
	if (rendering_server->is_on_render_thread()) {
		_sync_presentation_output_topology_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_sync_presentation_output_topology_on_render_thread_callback).bind((uint64_t)this));
	}
}

bool HTMLSurfaceHCSRBackend::_activate_presentation_output_on_render_thread(PresentationOutputState *p_state, const hcsr_gpu_frame_t &p_frame, bool p_engine_ordered) {
	ERR_FAIL_NULL_V(p_state, false);
	if (p_frame.native_texture == nullptr || p_frame.width <= 0 || p_frame.height <= 0 || p_frame.render_backend != render_backend || p_frame.texture_format != HCSR_GPU_TEXTURE_FORMAT_RGBA8_UNORM || p_frame.premultiplied_alpha != 0 || (p_frame.producer_completed == 0) != p_engine_ordered || p_frame.frame_generation <= p_state->active_generation) {
		_record_error("HCSR returned an invalid secondary presentation frame");
		return false;
	}
	const Size2i frame_size(p_frame.width, p_frame.height);
	if (frame_size != p_state->requested_size) {
		_defer_presentation_output_resource_release_on_render_thread(p_state->output, p_frame);
		return false;
	}
	if (p_state->texture.is_null()) {
		p_state->texture.instantiate();
	}
	if (p_state->native_texture != p_frame.native_texture || p_state->native_generation != p_frame.resource_generation || p_state->native_size != frame_size) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(rendering_server, false);
		const uint64_t native_handle = (uint64_t)p_frame.native_texture;
		const RID *cached = p_state->import_cache.getptr(native_handle);
		RID imported = cached != nullptr ? *cached : RID();
		if (!imported.is_valid()) {
#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
			MTL::Texture *retained_metal_texture = nullptr;
			if (render_backend == HCSR_RENDER_BACKEND_METAL) {
				retained_metal_texture = reinterpret_cast<MTL::Texture *>(p_frame.native_texture);
				retained_metal_texture->retain();
			}
#endif
			imported = rendering_server->texture_create_from_native_handle(
					RenderingServerEnums::TEXTURE_TYPE_2D,
					Image::FORMAT_RGBA8,
					native_handle,
					frame_size.x,
					frame_size.y,
					1,
					1,
					RenderingServerEnums::TEXTURE_LAYERED_2D_ARRAY,
					true);
			if (!imported.is_valid()) {
#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
				if (retained_metal_texture != nullptr) {
					retained_metal_texture->release();
				}
#endif
				_defer_presentation_output_resource_release_on_render_thread(p_state->output, p_frame);
				_record_error("Godot could not import an HCSR secondary presentation texture");
				return false;
			}
			integration_counters.texture_resource_creates++;
			_publish_integration_counters();
			p_state->import_cache.insert(native_handle, imported);
		}
		p_state->texture_rid = imported;
		p_state->native_texture = p_frame.native_texture;
		p_state->native_generation = p_frame.resource_generation;
		p_state->native_size = frame_size;
	}
	_publish_external_texture_state_on_render_thread(p_state->texture_rid);
	RID published_texture = p_state->texture_rid;
	if (p_state->mipmaps) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(rendering_server, false);
		if (!p_state->mipmapped_texture_rid.is_valid()) {
			p_state->mipmapped_texture_rid = rendering_server->texture_drawable_create(
					frame_size.x,
					frame_size.y,
					RenderingServerEnums::TEXTURE_DRAWABLE_FORMAT_RGBA8_SRGB,
					Color(0, 0, 0, 0),
					true);
			ERR_FAIL_COND_V_MSG(!p_state->mipmapped_texture_rid.is_valid(), false, "Godot could not create an HCSR mipmapped output texture.");
			integration_counters.texture_resource_creates++;
			_publish_integration_counters();
		}
		rendering_server->texture_drawable_copy_level_zero(p_state->texture_rid, p_state->mipmapped_texture_rid);
		rendering_server->texture_drawable_generate_mipmaps(p_state->mipmapped_texture_rid, true);
		published_texture = p_state->mipmapped_texture_rid;
	}
	p_state->texture->set_external_texture(published_texture, frame_size, true);
	if (p_state->active_frame.native_texture != nullptr) {
		const RID *active_imported = p_state->import_cache.getptr((uint64_t)p_state->active_frame.native_texture);
		_defer_presentation_output_resource_release_on_render_thread(
				p_state->output,
				p_state->active_frame,
				active_imported != nullptr ? *active_imported : RID());
	}
	p_state->active_frame = p_frame;
	p_state->active_generation = p_frame.frame_generation;
	gpu_presentation_changed.set();
	return true;
}

void HTMLSurfaceHCSRBackend::_poll_presentation_outputs_on_render_thread() {
	MutexLock lock(presentation_outputs_mutex);
	for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
		PresentationOutputState *state = entry.value;
		if (state == nullptr || state->output == nullptr) {
			continue;
		}
		hcsr_gpu_frame_t frame = {};
		frame.struct_size = sizeof(frame);
		uint8_t updated = 0;
		uint8_t pending = 0;
		if (hcsr_renderer_poll_presentation_output(renderer, state->output, &frame, &updated, &pending) != HCSR_STATUS_OK) {
			_record_error("HCSR could not poll a secondary presentation output");
			continue;
		}
		if (updated != 0) {
			if (gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
				const uint64_t *submitted_generation = state->submitted_generations.getptr(frame.submission_token);
				if (submitted_generation == nullptr || *submitted_generation != frame.frame_generation) {
					_record_error("HCSR completed an uncorrelated secondary engine-ordered resource");
				} else {
					Vector<uint64_t> retired_tokens;
					for (const KeyValue<uint64_t, uint64_t> &submitted : state->submitted_generations) {
						if (submitted.key <= frame.submission_token) {
							retired_tokens.push_back(submitted.key);
						}
					}
					for (uint64_t token : retired_tokens) {
						state->submitted_generations.erase(token);
					}
				}
			} else {
				_activate_presentation_output_on_render_thread(state, frame);
			}
		}
		if (pending != 0) {
			gpu_presentation_work_pending.set();
		}
	}
}

void HTMLSurfaceHCSRBackend::_poll_cpu_presentation_outputs() {
	MutexLock lock(presentation_outputs_mutex);
	for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
		PresentationOutputState *state = entry.value;
		if (state == nullptr || state->output == nullptr) {
			continue;
		}
		hcsr_frame_t output = {};
		output.struct_size = sizeof(output);
		uint8_t updated = 0;
		if (hcsr_renderer_poll_cpu_presentation_output(renderer, state->output, &output, &updated) != HCSR_STATUS_OK) {
			_record_error("HCSR could not poll a CPU secondary presentation output");
			continue;
		}
		if (updated == 0) {
			continue;
		}
		if (output.pixels == nullptr || output.width <= 0 || output.height <= 0 || output.stride < output.width * 4 || Size2i(output.width, output.height) != state->requested_size || output.generation < state->active_generation) {
			_record_error("HCSR returned an invalid CPU secondary presentation frame");
			continue;
		}

		const Size2i output_size(output.width, output.height);
		const int row_bytes = output_size.x * 4;
		const uint64_t conversion_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		Vector<uint8_t> rgba;
		rgba.resize(row_bytes * output_size.y);
		convert_bgra_to_rgba(output.pixels, output.stride, rgba.ptrw(), output_size.x, output_size.y);
		Ref<Image> image = Image::create_from_data(output_size.x, output_size.y, false, Image::FORMAT_RGBA8, rgba);
		if (image.is_null() || image->is_empty()) {
			_record_error("Godot could not create an image for a CPU secondary presentation output");
			continue;
		}
		integration_counters.cpu_secondary_conversion_milliseconds = conversion_start_usec != 0
				? (OS::get_singleton()->get_ticks_usec() - conversion_start_usec) / 1000.0
				: 0.0;
		const uint64_t upload_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		state->texture->update_from_image(image, transparent_background);
		if (state->mipmaps) {
			RenderingServer *rendering_server = RenderingServer::get_singleton();
			ERR_CONTINUE_MSG(rendering_server == nullptr, "RenderingServer is unavailable for an HCSR CPU mipmapped output.");
			if (!state->mipmapped_texture_rid.is_valid()) {
				state->mipmapped_texture_rid = rendering_server->texture_drawable_create(
						output_size.x,
						output_size.y,
						RenderingServerEnums::TEXTURE_DRAWABLE_FORMAT_RGBA8_SRGB,
						Color(0, 0, 0, 0),
						true);
				if (state->mipmapped_texture_rid.is_valid()) {
					integration_counters.texture_resource_creates++;
					_publish_integration_counters();
				}
			}
			ERR_CONTINUE_MSG(!state->mipmapped_texture_rid.is_valid(), "Godot could not create an HCSR CPU mipmapped output texture.");
			rendering_server->texture_drawable_copy_level_zero(state->texture->get_rid(), state->mipmapped_texture_rid);
			rendering_server->texture_drawable_generate_mipmaps(state->mipmapped_texture_rid, true);
			state->texture->set_external_texture(state->mipmapped_texture_rid, output_size, true);
		}
		state->active_generation = output.generation;
		state->native_size = output_size;
		integration_counters.cpu_secondary_upload_milliseconds = upload_start_usec != 0
				? (OS::get_singleton()->get_ticks_usec() - upload_start_usec) / 1000.0
				: 0.0;
	}
}

void HTMLSurfaceHCSRBackend::_ensure_gpu_texture_imported_on_render_thread() {
	if (gpu_texture_rid.is_valid() || native_gpu_texture == nullptr || native_gpu_size.x <= 0 || native_gpu_size.y <= 0) {
		return;
	}
	const uint64_t native_handle = (uint64_t)native_gpu_texture;
	if (_uses_presentation_texture_import_cache()) {
		const RID *cached_rid = gpu_texture_import_cache.getptr(native_handle);
		if (cached_rid != nullptr && cached_rid->is_valid()) {
			gpu_texture_rid = *cached_rid;
			if (gpu_texture.is_null()) {
				gpu_texture.instantiate();
			}
			gpu_texture->set_external_texture(gpu_texture_rid, native_gpu_size, true);
			return;
		}
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
			1,
			RenderingServerEnums::TEXTURE_LAYERED_2D_ARRAY,
			true);
	if (!gpu_texture_rid.is_valid()) {
#if defined(MACOS_ENABLED) && defined(METAL_ENABLED)
		if (retained_metal_texture != nullptr) {
			retained_metal_texture->release();
		}
#endif
		terminal_failure_reason = "Godot could not import HCSR's host-device GPU texture.";
		return;
	}
	integration_counters.texture_resource_creates++;
	_publish_integration_counters();
	if (gpu_texture.is_null()) {
		gpu_texture.instantiate();
	}
	if (_uses_presentation_texture_import_cache()) {
		gpu_texture_import_cache.insert(native_handle, gpu_texture_rid);
	}
	gpu_texture->set_external_texture(gpu_texture_rid, native_gpu_size, true);
}

void HTMLSurfaceHCSRBackend::_detach_gpu_texture_import_on_render_thread() {
	if (gpu_texture.is_valid()) {
		gpu_texture->clear_external_texture();
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr) {
		for (const KeyValue<uint64_t, RID> &entry : gpu_texture_import_cache) {
			if (entry.value.is_valid()) {
				rendering_server->free_rid(entry.value);
				integration_counters.texture_resource_frees++;
			}
		}
		if (gpu_texture_rid.is_valid() && !gpu_texture_import_cache.has((uint64_t)native_gpu_texture)) {
			rendering_server->free_rid(gpu_texture_rid);
			integration_counters.texture_resource_frees++;
		}
	}
	_publish_integration_counters();
	gpu_texture_import_cache.clear();
	gpu_texture_rid = RID();
}

bool HTMLSurfaceHCSRBackend::_record_submitted_gpu_frame_on_render_thread(const hcsr_gpu_frame_t &p_output) {
	// Queue-ordered submission transfers producer ownership and makes the
	// resource safe for later Godot consumers on the same queue. Completion is
	// observed separately for producer retirement.
	if (p_output.native_texture == nullptr || p_output.width <= 0 || p_output.height <= 0 || p_output.render_backend != render_backend || p_output.texture_format != HCSR_GPU_TEXTURE_FORMAT_RGBA8_UNORM || p_output.premultiplied_alpha != 0 || p_output.frame_generation == 0 || p_output.submission_token == 0 || p_output.producer_completed != 0) {
		if (p_output.frame_generation != 0) {
			_discard_gpu_packet_metadata(p_output.frame_generation);
		}
		_record_error("HCSR returned an invalid queued Godot GPU frame");
		return false;
	}
	{
		MutexLock metadata_lock(prepared_gpu_frame_metadata_mutex);
		if (!prepared_gpu_frame_metadata.has(p_output.frame_generation)) {
			_defer_gpu_resource_release_on_render_thread(p_output);
			_record_error("HCSR returned a GPU texture without matching prepared frame metadata");
			return false;
		}
	}
	MutexLock lock(frame_metadata_mutex);
	if (p_output.frame_generation <= last_queued_frame_generation || p_output.submission_token <= latest_submitted_gpu_submission_token || submitted_gpu_frames.has(p_output.submission_token)) {
		_discard_gpu_packet_metadata(p_output.frame_generation);
		_defer_gpu_resource_release_on_render_thread(p_output);
		_record_error("HCSR returned a non-monotonic queued Godot GPU frame");
		return false;
	}
	last_queued_frame_generation = p_output.frame_generation;
	latest_submitted_gpu_submission_token = p_output.submission_token;
	submitted_gpu_frame_generations.insert(p_output.submission_token, p_output.frame_generation);
	submitted_gpu_frames.insert(p_output.submission_token, p_output);
	return true;
}

bool HTMLSurfaceHCSRBackend::_activate_engine_ordered_gpu_frame_on_render_thread(const hcsr_gpu_frame_t &p_output) {
	ERR_FAIL_COND_V(gpu_capabilities.synchronization_mode != HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED, false);

	PreparedGPUFrameMetadata prepared_metadata;
	if (!_take_gpu_packet_metadata(p_output.frame_generation, prepared_metadata)) {
		_record_error("HCSR returned an engine-ordered GPU texture without matching prepared frame metadata");
		return false;
	}
	if (prepared_metadata.viewport_revision != viewport_revision.get()
			|| prepared_metadata.physical_size != Size2i(p_output.width, p_output.height)) {
		_release_gpu_packet_metadata(prepared_metadata);
		gpu_follow_up_frame_requested.set();
		return false;
	}

	if (native_gpu_texture != p_output.native_texture
			|| native_gpu_generation != p_output.resource_generation
			|| native_gpu_size != Size2i(p_output.width, p_output.height)) {
		void *previous_native_texture = native_gpu_texture;
		const uint64_t previous_native_generation = native_gpu_generation;
		const Size2i previous_native_size = native_gpu_size;
		const RID previous_texture_rid = gpu_texture_rid;
		native_gpu_texture = p_output.native_texture;
		native_gpu_generation = p_output.resource_generation;
		native_gpu_size = Size2i(p_output.width, p_output.height);
		gpu_texture_rid = RID();
		_ensure_gpu_texture_imported_on_render_thread();
		if (!gpu_texture_rid.is_valid()) {
			native_gpu_texture = previous_native_texture;
			native_gpu_generation = previous_native_generation;
			native_gpu_size = previous_native_size;
			gpu_texture_rid = previous_texture_rid;
		} else if (previous_texture_rid.is_valid()
				&& previous_texture_rid != gpu_texture_rid
				&& !_uses_presentation_texture_import_cache()) {
			RenderingServer *rendering_server = RenderingServer::get_singleton();
			if (rendering_server != nullptr) {
				rendering_server->free_rid(previous_texture_rid);
				integration_counters.texture_resource_frees++;
				_publish_integration_counters();
			}
		}
	} else {
		_ensure_gpu_texture_imported_on_render_thread();
	}
	if (!gpu_texture_rid.is_valid()) {
		_release_gpu_packet_metadata(prepared_metadata);
		return false;
	}
	_publish_external_texture_state_on_render_thread(gpu_texture_rid);

	hcsr_hit_test_snapshot_t *retired_snapshot = nullptr;
	hcsr_gpu_frame_t retired_frame = {};
	{
		MutexLock lock(frame_metadata_mutex);
		if (p_output.frame_generation <= active_gpu_frame_generation) {
			_release_gpu_packet_metadata(prepared_metadata);
			_record_error("HCSR returned a non-monotonic engine-ordered Godot GPU frame");
			return false;
		}
		frame_metadata = prepared_metadata.frame_metadata;
		active_gpu_frame_generation = p_output.frame_generation;
		retired_frame = active_gpu_frame;
		active_gpu_frame = p_output;
		retired_snapshot = active_hit_test_snapshot;
		active_hit_test_snapshot = prepared_metadata.hit_test_snapshot;
		prepared_metadata.hit_test_snapshot = nullptr;
	}
	if (retired_snapshot != nullptr) {
		hcsr_hit_test_snapshot_release(retired_snapshot);
	}
	if (retired_frame.native_texture != nullptr) {
		_defer_gpu_resource_release_on_render_thread(retired_frame);
	}
	gpu_presentation_changed.set();
	return true;
}

bool HTMLSurfaceHCSRBackend::_queue_engine_ordered_output_group_on_render_thread(const hcsr_gpu_frame_t &p_primary_output) {
	ERR_FAIL_COND_V(gpu_capabilities.synchronization_mode != HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED, false);

	struct QueuedPresentationFrame {
		PresentationOutputState *state = nullptr;
		hcsr_gpu_frame_t frame = {};
	};
	Vector<QueuedPresentationFrame> queued_outputs;
	{
		MutexLock lock(presentation_outputs_mutex);
		queued_outputs.reserve(presentation_outputs.size());
		auto release_acquired_outputs = [&]() {
			for (const QueuedPresentationFrame &queued : queued_outputs) {
				const RID *imported = queued.state->import_cache.getptr((uint64_t)queued.frame.native_texture);
				_defer_presentation_output_resource_release_on_render_thread(
						queued.state->output,
						queued.frame,
						imported != nullptr ? *imported : RID());
			}
		};
		for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
			PresentationOutputState *state = entry.value;
			if (state == nullptr || state->output == nullptr) {
				continue;
			}
			QueuedPresentationFrame queued;
			queued.state = state;
			queued.frame.struct_size = sizeof(queued.frame);
			uint8_t available = 0;
			if (hcsr_renderer_acquire_queued_presentation_output(renderer, state->output, p_primary_output.frame_generation, &queued.frame, &available) != HCSR_STATUS_OK || available == 0) {
				release_acquired_outputs();
				_record_error("HCSR did not queue every synchronized presentation output for the primary frame generation");
				return false;
			}
			if (queued.frame.frame_generation != p_primary_output.frame_generation
					|| queued.frame.render_backend != p_primary_output.render_backend
					|| queued.frame.texture_format != p_primary_output.texture_format
					|| queued.frame.premultiplied_alpha != p_primary_output.premultiplied_alpha
					|| queued.frame.producer_completed != 0
					|| Size2i(queued.frame.width, queued.frame.height) != state->requested_size) {
				const RID *imported = state->import_cache.getptr((uint64_t)queued.frame.native_texture);
				_defer_presentation_output_resource_release_on_render_thread(
						state->output,
						queued.frame,
						imported != nullptr ? *imported : RID());
				release_acquired_outputs();
				_record_error("HCSR queued an inconsistent synchronized presentation output");
				return false;
			}
			queued_outputs.push_back(queued);
		}

		for (QueuedPresentationFrame &queued : queued_outputs) {
			if (queued.state->queued_frames.has(p_primary_output.frame_generation)) {
				release_acquired_outputs();
				_record_error("HCSR queued a duplicate synchronized secondary presentation generation");
				return false;
			}
		}
		for (QueuedPresentationFrame &queued : queued_outputs) {
			queued.state->queued_frames.insert(p_primary_output.frame_generation, queued.frame);
			queued.state->submitted_generations.insert(queued.frame.submission_token, queued.frame.frame_generation);
		}
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_activate_completed_engine_ordered_output_group_on_render_thread(const hcsr_gpu_frame_t &p_primary_output, const Vector<uint64_t> &p_superseded_generations) {
	ERR_FAIL_COND_V(gpu_capabilities.synchronization_mode != HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED, false);

	{
		MutexLock lock(presentation_outputs_mutex);
		for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
			PresentationOutputState *state = entry.value;
			if (state == nullptr || state->output == nullptr) {
				continue;
			}
			for (uint64_t generation : p_superseded_generations) {
				hcsr_gpu_frame_t *superseded = state->queued_frames.getptr(generation);
				if (superseded != nullptr) {
					_defer_presentation_output_resource_release_on_render_thread(state->output, *superseded);
					state->queued_frames.erase(generation);
				}
			}
			hcsr_gpu_frame_t *completed = state->queued_frames.getptr(p_primary_output.frame_generation);
			if (completed == nullptr) {
				continue;
			}
			const hcsr_gpu_frame_t completed_frame = *completed;
			state->queued_frames.erase(p_primary_output.frame_generation);
			if (!_activate_presentation_output_on_render_thread(state, completed_frame, true)) {
				_record_error("Godot could not activate a completed synchronized secondary presentation output");
				return false;
			}
		}
	}

	// This render-thread callback is the publication transaction. Texture proxies
	// are switched on the render thread, then one atomic generation is released to
	// game-thread observers only after the complete group is active. Without the
	// final publication point, a capacity-retry callback can be sampled between a
	// secondary proxy update and the primary proxy update.
	if (!_activate_engine_ordered_gpu_frame_on_render_thread(p_primary_output)) {
		return false;
	}
	synchronized_active_generation.set(p_primary_output.frame_generation);
	return true;
}

bool HTMLSurfaceHCSRBackend::_activate_completed_gpu_frame_on_render_thread(const hcsr_gpu_frame_t &p_output) {
	if (p_output.native_texture == nullptr || p_output.width <= 0 || p_output.height <= 0 || p_output.render_backend != render_backend || p_output.texture_format != HCSR_GPU_TEXTURE_FORMAT_RGBA8_UNORM || p_output.premultiplied_alpha != 0 || p_output.frame_generation == 0 || p_output.submission_token == 0 || p_output.producer_completed == 0) {
		_record_error("HCSR returned an invalid completed Godot GPU frame");
		return false;
	}

	hcsr_gpu_frame_t completed_frame = {};
	Vector<hcsr_gpu_frame_t> superseded_frames;
	Vector<uint64_t> superseded_generations;
	{
		MutexLock lock(frame_metadata_mutex);
		const uint64_t *submitted_generation = submitted_gpu_frame_generations.getptr(p_output.submission_token);
		const hcsr_gpu_frame_t *submitted_frame = submitted_gpu_frames.getptr(p_output.submission_token);
		if (submitted_generation == nullptr || submitted_frame == nullptr || *submitted_generation != p_output.frame_generation || p_output.submission_token <= completed_gpu_submission_token || p_output.submission_token > latest_submitted_gpu_submission_token) {
			_record_error("HCSR returned an uncorrelated Godot GPU completion");
			return false;
		}
		completed_frame = *submitted_frame;
		for (const KeyValue<uint64_t, hcsr_gpu_frame_t> &entry : submitted_gpu_frames) {
			if (entry.key < p_output.submission_token) {
				superseded_frames.push_back(entry.value);
				superseded_generations.push_back(entry.value.frame_generation);
			}
		}
		for (const hcsr_gpu_frame_t &frame : superseded_frames) {
			submitted_gpu_frames.erase(frame.submission_token);
			submitted_gpu_frame_generations.erase(frame.submission_token);
		}
		submitted_gpu_frames.erase(p_output.submission_token);
		submitted_gpu_frame_generations.erase(p_output.submission_token);
		completed_gpu_submission_token = p_output.submission_token;
	}
	for (uint64_t generation : superseded_generations) {
		_discard_gpu_packet_metadata(generation);
	}
	for (const hcsr_gpu_frame_t &frame : superseded_frames) {
		_defer_gpu_resource_release_on_render_thread(frame);
	}
	if (gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
		return _activate_completed_engine_ordered_output_group_on_render_thread(completed_frame, superseded_generations);
	}

	PreparedGPUFrameMetadata prepared_metadata;
	if (!_take_gpu_packet_metadata(completed_frame.frame_generation, prepared_metadata)) {
		_defer_gpu_resource_release_on_render_thread(completed_frame);
		_record_error("HCSR returned a GPU texture without matching prepared frame metadata");
		return false;
	}
	if (prepared_metadata.viewport_revision != viewport_revision.get()
			|| prepared_metadata.physical_size != Size2i(completed_frame.width, completed_frame.height)) {
		_release_gpu_packet_metadata(prepared_metadata);
		_defer_gpu_resource_release_on_render_thread(completed_frame);
		gpu_follow_up_frame_requested.set();
		return false;
	}
	{
		MutexLock lock(frame_metadata_mutex);
		if (completed_frame.frame_generation <= active_gpu_frame_generation) {
			_release_gpu_packet_metadata(prepared_metadata);
			_defer_gpu_resource_release_on_render_thread(completed_frame);
			_record_error("HCSR returned a non-monotonic completed Godot GPU frame");
			return false;
		}
	}
	if (native_gpu_texture != completed_frame.native_texture || native_gpu_generation != completed_frame.resource_generation || native_gpu_size != Size2i(completed_frame.width, completed_frame.height)) {
		void *previous_native_texture = native_gpu_texture;
		const uint64_t previous_native_generation = native_gpu_generation;
		const Size2i previous_native_size = native_gpu_size;
		const RID previous_texture_rid = gpu_texture_rid;
		const bool size_changed = previous_native_size != Size2i(completed_frame.width, completed_frame.height);
		native_gpu_texture = completed_frame.native_texture;
		native_gpu_generation = completed_frame.resource_generation;
		native_gpu_size = Size2i(completed_frame.width, completed_frame.height);
		gpu_texture_rid = RID();
		if (size_changed) {
			gpu_texture_import_cache.erase((uint64_t)native_gpu_texture);
		}
		_ensure_gpu_texture_imported_on_render_thread();
		if (!gpu_texture_rid.is_valid()) {
			native_gpu_texture = previous_native_texture;
			native_gpu_generation = previous_native_generation;
			native_gpu_size = previous_native_size;
			gpu_texture_rid = previous_texture_rid;
			_release_gpu_packet_metadata(prepared_metadata);
			_defer_gpu_resource_release_on_render_thread(completed_frame);
			return false;
		}
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		if (rendering_server != nullptr) {
			if (size_changed) {
				Vector<uint64_t> obsolete_handles;
				for (const KeyValue<uint64_t, RID> &entry : gpu_texture_import_cache) {
					if (entry.key != (uint64_t)native_gpu_texture) {
						rendering_server->free_rid(entry.value);
						integration_counters.texture_resource_frees++;
						obsolete_handles.push_back(entry.key);
					}
				}
				for (uint64_t handle : obsolete_handles) {
					gpu_texture_import_cache.erase(handle);
				}
			}
			if (previous_texture_rid.is_valid() && previous_texture_rid != gpu_texture_rid
					&& (!_uses_presentation_texture_import_cache() || previous_native_texture == native_gpu_texture)) {
				rendering_server->free_rid(previous_texture_rid);
				integration_counters.texture_resource_frees++;
			}
			_publish_integration_counters();
		}
	} else {
		_ensure_gpu_texture_imported_on_render_thread();
	}
	if (!gpu_texture_rid.is_valid()) {
		_release_gpu_packet_metadata(prepared_metadata);
		_defer_gpu_resource_release_on_render_thread(completed_frame);
		return false;
	}
	_publish_external_texture_state_on_render_thread(gpu_texture_rid);
	if (active_gpu_frame.native_texture != nullptr) {
		_defer_gpu_resource_release_on_render_thread(active_gpu_frame);
	}
	hcsr_hit_test_snapshot_t *retired_snapshot = nullptr;
	{
		MutexLock lock(frame_metadata_mutex);
		frame_metadata = prepared_metadata.frame_metadata;
		active_gpu_frame_generation = completed_frame.frame_generation;
		active_gpu_frame = completed_frame;
		retired_snapshot = active_hit_test_snapshot;
		active_hit_test_snapshot = prepared_metadata.hit_test_snapshot;
		prepared_metadata.hit_test_snapshot = nullptr;
	}
	if (retired_snapshot != nullptr) {
		hcsr_hit_test_snapshot_release(retired_snapshot);
	}
	gpu_presentation_changed.set();
	return true;
}

bool HTMLSurfaceHCSRBackend::_uses_async_gpu_presentation() const {
	return gpu_device_configured
			&& (gpu_capabilities.flags & HCSR_GPU_CAPABILITY_ASYNC_COMPLETION_POLLING) != 0;
}

bool HTMLSurfaceHCSRBackend::_uses_presentation_texture_import_cache() const {
	return render_backend == HCSR_RENDER_BACKEND_D3D12
			|| render_backend == HCSR_RENDER_BACKEND_VULKAN
			|| render_backend == HCSR_RENDER_BACKEND_METAL;
}

void HTMLSurfaceHCSRBackend::_poll_gpu_presentation_on_render_thread() {
	gpu_presentation_poll_pending.clear();
	if (renderer == nullptr || !_uses_async_gpu_presentation()) {
		gpu_presentation_work_pending.clear();
		return;
	}
	hcsr_gpu_frame_t output = {};
	output.struct_size = sizeof(output);
	uint8_t updated = 0;
	uint8_t pending = 0;
	uint8_t lock_busy = 0;
	if (hcsr_renderer_try_poll_gpu_presentation(renderer, &output, &updated, &pending, &lock_busy) != HCSR_STATUS_OK) {
		_record_error("HCSR could not poll the completed GPU presentation");
		return;
	}
	if (lock_busy != 0) {
		integration_counters.presentation_lock_busy++;
		_publish_integration_counters();
		gpu_presentation_work_pending.set();
		return;
	}
	if (pending != 0) {
		gpu_presentation_work_pending.set();
	} else {
		gpu_presentation_work_pending.clear();
	}
	if (updated != 0) {
		_activate_completed_gpu_frame_on_render_thread(output);
	}
	_poll_presentation_outputs_on_render_thread();
	_update_latest_performance_profile();
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
	if (deferred_gpu_packet != nullptr) {
		_abandon_gpu_frame_packet(deferred_gpu_packet);
		deferred_gpu_packet = nullptr;
	}
	gpu_submission_deferred.clear();
	gpu_submission_lock_deferred.clear();
	gpu_submission_retry_pending.clear();
	gpu_frame_pending.clear();
	gpu_presentation_changed.clear();
	semantic_worker_pending = false;
	semantic_worker_last_requested_revision = 0;
	Vector<PresentationOutputState *> outputs_to_destroy;
	{
		MutexLock lock(presentation_outputs_mutex);
		for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
			outputs_to_destroy.push_back(entry.value);
		}
		presentation_outputs.clear();
	}
	for (PresentationOutputState *state : outputs_to_destroy) {
		_destroy_presentation_output_state_on_render_thread(state);
	}
	Vector<hcsr_hit_test_snapshot_t *> staged_snapshots;
	{
		MutexLock lock(prepared_gpu_frame_metadata_mutex);
		for (const KeyValue<uint64_t, PreparedGPUFrameMetadata> &entry : prepared_gpu_frame_metadata) {
			if (entry.value.hit_test_snapshot != nullptr) {
				staged_snapshots.push_back(entry.value.hit_test_snapshot);
			}
		}
		prepared_gpu_frame_metadata.clear();
	}
	for (hcsr_hit_test_snapshot_t *snapshot : staged_snapshots) {
		hcsr_hit_test_snapshot_release(snapshot);
	}
	if (active_gpu_frame.native_texture != nullptr) {
		_defer_gpu_resource_release_on_render_thread(active_gpu_frame);
		active_gpu_frame = {};
	}
	Vector<hcsr_gpu_frame_t> submitted_frames;
	{
		MutexLock lock(frame_metadata_mutex);
		for (const KeyValue<uint64_t, hcsr_gpu_frame_t> &entry : submitted_gpu_frames) {
			submitted_frames.push_back(entry.value);
		}
		submitted_gpu_frames.clear();
		if (active_hit_test_snapshot != nullptr) {
			hcsr_hit_test_snapshot_release(active_hit_test_snapshot);
			active_hit_test_snapshot = nullptr;
		}
		active_gpu_frame_generation = 0;
		synchronized_active_generation.set(0);
		last_queued_frame_generation = 0;
		latest_submitted_gpu_submission_token = 0;
		completed_gpu_submission_token = 0;
		submitted_gpu_frame_generations.clear();
	}
	for (const hcsr_gpu_frame_t &frame : submitted_frames) {
		_defer_gpu_resource_release_on_render_thread(frame);
	}
	gpu_device_configured = false;
	gpu_capabilities = {};
	if (renderer != nullptr) {
		hcsr_renderer_t *renderer_to_destroy = renderer;
		renderer = nullptr;
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		RenderingDevice *rendering_device = rendering_server != nullptr ? rendering_server->get_rendering_device() : nullptr;
		if (render_backend != HCSR_RENDER_BACKEND_CPU && rendering_device != nullptr) {
			rendering_device->external_resource_defer_release(callable_mp_static(&HTMLSurfaceHCSRBackend::_destroy_renderer_after_retirement_callback).bind((uint64_t)renderer_to_destroy));
		} else {
			hcsr_renderer_destroy(renderer_to_destroy);
		}
	}
}

void HTMLSurfaceHCSRBackend::_abandon_gpu_frame_packet(hcsr_gpu_frame_packet_t *p_packet) {
	if (p_packet == nullptr) {
		return;
	}
	if (renderer == nullptr) {
		hcsr_renderer_release_gpu_frame_packet(p_packet);
		return;
	}
	uint32_t canceled_packet_count = 0;
	if (hcsr_renderer_cancel_gpu_frame_packet(renderer, p_packet, &canceled_packet_count) != HCSR_STATUS_OK || canceled_packet_count == 0) {
		hcsr_renderer_release_gpu_frame_packet(p_packet);
		_record_error("HCSR could not roll back an abandoned prepared GPU frame");
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
	if (!_ensure_presentation_outputs_on_render_thread()) {
		_abandon_gpu_frame_packet(p_packet);
		gpu_follow_up_frame_requested.set();
		return;
	}
	uint64_t packet_generation = 0;
	if (hcsr_renderer_gpu_frame_packet_generation(p_packet, &packet_generation) != HCSR_STATUS_OK || packet_generation == 0) {
		_abandon_gpu_frame_packet(p_packet);
		_record_error("HCSR could not identify the prepared Godot GPU frame");
		return;
	}
	uint8_t submission_ready = 0;
	uint8_t readiness_lock_busy = 0;
#ifdef DEBUG_ENABLED
	if (_diagnostic_is_capacity_blocked()) {
		deferred_gpu_packet = p_packet;
		gpu_submission_deferred.set();
		gpu_submission_lock_deferred.clear();
		gpu_render_succeeded = gpu_texture_rid.is_valid();
		return;
	}
#endif
	// Submission readiness is part of the immutable packet handoff: retain the
	// packet unchanged until the borrowed host queue releases a frame slot.
	if (hcsr_renderer_try_can_submit_gpu_frame(renderer, p_packet, &submission_ready, &readiness_lock_busy) != HCSR_STATUS_OK) {
		_abandon_gpu_frame_packet(p_packet);
		_discard_gpu_packet_metadata(packet_generation);
		_record_error("HCSR could not query Godot GPU submission readiness");
		return;
	}
	if (readiness_lock_busy != 0 || submission_ready == 0) {
		if (readiness_lock_busy != 0) {
			integration_counters.presentation_lock_busy++;
			_publish_integration_counters();
		}
		deferred_gpu_packet = p_packet;
		gpu_submission_deferred.set();
		if (readiness_lock_busy != 0) {
			gpu_submission_lock_deferred.set();
		} else {
			gpu_submission_lock_deferred.clear();
		}
		gpu_render_succeeded = gpu_texture_rid.is_valid();
		return;
	}
	hcsr_gpu_frame_t output = {};
	output.struct_size = sizeof(output);
	uint8_t submitted = 0;
	uint8_t submission_lock_busy = 0;
	if (hcsr_renderer_try_submit_gpu_frame(renderer, p_packet, &output, &submitted, &submission_lock_busy) != HCSR_STATUS_OK) {
		_discard_gpu_packet_metadata(packet_generation);
		_record_error("HCSR could not submit the prepared Godot GPU frame");
		return;
	}
	if (submission_lock_busy != 0 || submitted == 0) {
		if (submission_lock_busy != 0) {
			integration_counters.presentation_lock_busy++;
			_publish_integration_counters();
		}
		deferred_gpu_packet = p_packet;
		gpu_submission_deferred.set();
		gpu_submission_lock_deferred.set();
		gpu_render_succeeded = gpu_texture_rid.is_valid();
		return;
	}
#ifdef DEBUG_ENABLED
	_diagnostic_note_successful_submission();
#endif
	deferred_gpu_packet = nullptr;
	gpu_submission_deferred.clear();
	gpu_submission_lock_deferred.clear();
	gpu_render_succeeded = _record_submitted_gpu_frame_on_render_thread(output);
	if (gpu_render_succeeded) {
		// HCSR's top-level timing profile is final at native submission. Completion
		// polling only appends lifecycle diagnostics kept in custom monitors.
		_complete_performance_profile(output.frame_generation);
	} else {
		_update_latest_performance_profile();
	}
	if (gpu_render_succeeded
			&& gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
		gpu_render_succeeded = _queue_engine_ordered_output_group_on_render_thread(output);
	}
	if (_uses_async_gpu_presentation()) {
		gpu_presentation_work_pending.set();
	}
}

void HTMLSurfaceHCSRBackend::_render_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr, uint64_t p_packet_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	hcsr_gpu_frame_packet_t *packet = (hcsr_gpu_frame_packet_t *)p_packet_ptr;
	if (backend != nullptr) {
		backend->_render_gpu_frame_on_render_thread(packet);
		if (!backend->gpu_submission_deferred.is_set()) {
			backend->gpu_frame_pending.clear();
		}
	} else if (packet != nullptr) {
		hcsr_renderer_release_gpu_frame_packet(packet);
	}
}

void HTMLSurfaceHCSRBackend::_retry_deferred_gpu_frame_on_render_thread() {
	gpu_submission_retry_pending.clear();
	if (deferred_gpu_packet == nullptr) {
		gpu_submission_deferred.clear();
		gpu_frame_pending.clear();
		return;
	}

	uint8_t submission_ready = 0;
	uint8_t readiness_lock_busy = 0;
#ifdef DEBUG_ENABLED
	if (_diagnostic_is_capacity_blocked()) {
		return;
	}
#endif
	if (hcsr_renderer_try_can_submit_gpu_frame(renderer, deferred_gpu_packet, &submission_ready, &readiness_lock_busy) != HCSR_STATUS_OK) {
		hcsr_gpu_frame_packet_t *packet = deferred_gpu_packet;
		deferred_gpu_packet = nullptr;
		gpu_submission_deferred.clear();
		gpu_submission_lock_deferred.clear();
		gpu_frame_pending.clear();
		_abandon_gpu_frame_packet(packet);
		_record_error("HCSR could not query deferred Godot GPU submission readiness");
		return;
	}
	if (readiness_lock_busy != 0 || submission_ready == 0) {
		if (readiness_lock_busy != 0) {
			integration_counters.presentation_lock_busy++;
			_publish_integration_counters();
		}
		return;
	}

	uint64_t packet_generation = 0;
	(void)hcsr_renderer_gpu_frame_packet_generation(deferred_gpu_packet, &packet_generation);
	bool semantic_state_is_current = false;
	{
		MutexLock metadata_lock(prepared_gpu_frame_metadata_mutex);
		const PreparedGPUFrameMetadata *metadata = prepared_gpu_frame_metadata.getptr(packet_generation);
		semantic_state_is_current = metadata != nullptr
				&& metadata->semantic_state_revision == semantic_state_revision.get()
				&& metadata->frame_request_revision == frame_request_revision.get()
				&& metadata->viewport_revision == viewport_revision.get();
	}
	if (semantic_state_is_current) {
		hcsr_gpu_frame_t output = {};
		output.struct_size = sizeof(output);
		uint8_t submitted = 0;
		uint8_t submission_lock_busy = 0;
		if (hcsr_renderer_try_submit_gpu_frame(renderer, deferred_gpu_packet, &output, &submitted, &submission_lock_busy) != HCSR_STATUS_OK) {
			hcsr_gpu_frame_packet_t *packet = deferred_gpu_packet;
			deferred_gpu_packet = nullptr;
			gpu_submission_deferred.clear();
			gpu_submission_lock_deferred.clear();
			gpu_frame_pending.clear();
			_abandon_gpu_frame_packet(packet);
			_discard_gpu_packet_metadata(packet_generation);
			_record_error("HCSR could not retry the prepared Godot GPU frame");
			return;
		}
		if (submission_lock_busy != 0 || submitted == 0) {
			if (submission_lock_busy != 0) {
				integration_counters.presentation_lock_busy++;
				_publish_integration_counters();
			}
			return;
		}

#ifdef DEBUG_ENABLED
		_diagnostic_note_successful_submission();
#endif
		deferred_gpu_packet = nullptr;
		gpu_submission_deferred.clear();
		gpu_submission_lock_deferred.clear();
		gpu_frame_pending.clear();
		gpu_render_succeeded = _record_submitted_gpu_frame_on_render_thread(output);
		if (gpu_render_succeeded) {
			// HCSR's top-level timing profile is final at native submission. Completion
			// polling only appends lifecycle diagnostics kept in custom monitors.
			_complete_performance_profile(output.frame_generation);
		} else {
			_update_latest_performance_profile();
		}
		if (gpu_render_succeeded
				&& gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
			gpu_render_succeeded = _queue_engine_ordered_output_group_on_render_thread(output);
		}
		if (_uses_async_gpu_presentation()) {
			gpu_presentation_work_pending.set();
		}
		return;
	}

	hcsr_gpu_frame_packet_t *superseded_packet = deferred_gpu_packet;
	deferred_gpu_packet = nullptr;
	gpu_submission_deferred.clear();
	gpu_submission_lock_deferred.clear();
	gpu_frame_pending.clear();
	_abandon_gpu_frame_packet(superseded_packet);
	integration_counters.capacity_probe_cancellations++;
	_publish_integration_counters();
	if (packet_generation != 0) {
		_discard_gpu_packet_metadata(packet_generation);
	}
	gpu_follow_up_frame_requested.set();
}

void HTMLSurfaceHCSRBackend::_retry_deferred_gpu_frame_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_retry_deferred_gpu_frame_on_render_thread();
	}
}

void HTMLSurfaceHCSRBackend::_schedule_deferred_gpu_submission() {
	if (!gpu_submission_deferred.is_set() || gpu_submission_retry_pending.is_set()) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return;
	}
	gpu_submission_retry_pending.set();
	if (rendering_server->is_on_render_thread()) {
		_retry_deferred_gpu_frame_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_retry_deferred_gpu_frame_on_render_thread_callback).bind((uint64_t)this));
	}
}

void HTMLSurfaceHCSRBackend::_poll_gpu_presentation_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceHCSRBackend *backend = (HTMLSurfaceHCSRBackend *)p_backend_ptr;
	if (backend != nullptr) {
		backend->_poll_gpu_presentation_on_render_thread();
	}
}

bool HTMLSurfaceHCSRBackend::_render_gpu_frame() {
	frame_request_revision.increment();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		terminal_failure_reason = "RenderingServer is unavailable for HCSR GPU rendering.";
		return false;
	}
	_schedule_presentation_output_topology_sync();
	if (gpu_frame_pending.is_set()) {
		_schedule_deferred_gpu_submission();
		// Keep presenting the previous completed texture. Preparing another retained
		// delta before the render thread submits the outstanding packet would make
		// the logical retained state run ahead of the GPU presenter. Remember that
		// the game-thread request still needs a replacement packet once the queued
		// packet completes; otherwise a mutation made from an input callback can be
		// left showing the prior generation until another input event arrives.
		gpu_follow_up_frame_requested.set();
		return true;
	}
	if (semantic_worker_enabled) {
		return _request_semantic_worker_frame();
	}
	gpu_follow_up_frame_requested.clear();
	hcsr_gpu_frame_packet_t *packet = nullptr;
	PreparedGPUFrameMetadata packet_metadata;
	uint64_t packet_generation = 0;
	for (int attempt = 0; attempt < 2; attempt++) {
		packet = nullptr;
		const uint64_t managed_export_call_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		if (hcsr_renderer_prepare_gpu_frame(renderer, timeline_time_seconds, &packet) != HCSR_STATUS_OK) {
			_record_error("HCSR could not prepare the Godot GPU frame");
			return false;
		}
		_record_managed_export_boundary_overhead(managed_export_call_start_usec);
		if (!_update_frame_schedule()) {
			if (packet != nullptr) {
				_abandon_gpu_frame_packet(packet);
			}
			return false;
		}
		if (packet == nullptr) {
			// A successful null packet is an explicit idle frame. Keep the last
			// completed texture active and do not enqueue redundant GPU work.
			_retire_document_commits();
			return true;
		}
		if (!_read_gpu_packet_metadata(packet, packet_metadata, packet_generation)) {
			_abandon_gpu_frame_packet(packet);
			return false;
		}
		_retire_document_commits();
		bool scroll_offset_changed = false;
		if (!_clamp_scroll_offset_to_content(scroll_offset_changed, packet_metadata.content_width, packet_metadata.content_height)) {
			_release_gpu_packet_metadata(packet_metadata);
			_abandon_gpu_frame_packet(packet);
			return false;
		}
		if (!scroll_offset_changed) {
			break;
		}

		// A prepared packet is an immutable snapshot of its render inputs. Never
		// submit a packet after clamping changed the host scroll state: doing so
		// presents one frame translated by an offset the document cannot reach.
		_release_gpu_packet_metadata(packet_metadata);
		_abandon_gpu_frame_packet(packet);
		packet = nullptr;
		if (attempt > 0) {
			_record_error("HCSR document scroll state did not stabilize while preparing a GPU frame");
			return false;
		}
	}
	ERR_FAIL_NULL_V(packet, false);
	return _queue_prepared_gpu_packet(packet, packet_metadata, packet_generation);
}

bool HTMLSurfaceHCSRBackend::_request_semantic_worker_frame() {
	gpu_follow_up_frame_requested.clear();
	const uint64_t revision = ++semantic_worker_next_revision;
	const uint64_t host_frame = Engine::get_singleton() != nullptr ? Engine::get_singleton()->get_process_frames() + 1 : 0;
	const uint64_t call_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const hcsr_status_t request_status = hcsr_renderer_request_semantic_worker_frame(
				renderer,
				revision,
				host_frame,
				timeline_time_seconds);
	integration_counters.semantic_worker_host_call_milliseconds = call_start_usec != 0
			? double(OS::get_singleton()->get_ticks_usec() - call_start_usec) / 1000.0
			: 0.0;
	_publish_integration_counters();
	if (request_status != HCSR_STATUS_OK) {
		_record_error("HCSR could not request a semantic worker frame");
		return false;
	}
	semantic_worker_last_requested_revision = revision;
	semantic_worker_pending = true;
	return true;
}

void HTMLSurfaceHCSRBackend::_poll_semantic_worker_frame() {
	if (!semantic_worker_enabled || !semantic_worker_pending || renderer == nullptr) {
		return;
	}

	hcsr_semantic_worker_poll_state_t state = HCSR_SEMANTIC_WORKER_NONE;
	uint64_t revision = 0;
	double mailbox_delay_milliseconds = 0.0;
	uint64_t superseded_revision_count = 0;
	hcsr_gpu_frame_packet_t *packet = nullptr;
	const uint64_t call_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const hcsr_status_t poll_status = hcsr_renderer_poll_semantic_worker_frame(
				renderer,
				&state,
				&revision,
				&mailbox_delay_milliseconds,
				&superseded_revision_count,
				&packet);
	integration_counters.semantic_worker_host_call_milliseconds = call_start_usec != 0
			? double(OS::get_singleton()->get_ticks_usec() - call_start_usec) / 1000.0
			: 0.0;
	if (poll_status != HCSR_STATUS_OK) {
		_publish_integration_counters();
		_record_error("HCSR could not poll the semantic worker");
		semantic_worker_pending = false;
		return;
	}
	integration_counters.semantic_worker_supersessions = superseded_revision_count;
	if (state == HCSR_SEMANTIC_WORKER_NONE || state == HCSR_SEMANTIC_WORKER_PENDING) {
		_publish_integration_counters();
		return;
	}

	semantic_worker_pending = false;
	integration_counters.semantic_worker_mailbox_delay_milliseconds = mailbox_delay_milliseconds;
	_publish_integration_counters();
	if (state == HCSR_SEMANTIC_WORKER_FAILED) {
		_record_error("HCSR semantic worker preparation failed");
		return;
	}
	if (revision != semantic_worker_last_requested_revision) {
		if (packet != nullptr) {
			_abandon_gpu_frame_packet(packet);
		}
		_record_error("HCSR semantic worker published an obsolete host revision");
		return;
	}
	if (!_update_frame_schedule()) {
		if (packet != nullptr) {
			_abandon_gpu_frame_packet(packet);
		}
		return;
	}
	_retire_document_commits();
	_update_latest_performance_profile();
	if (state == HCSR_SEMANTIC_WORKER_NO_VISUAL_OUTPUT) {
		return;
	}
	if (state != HCSR_SEMANTIC_WORKER_PREPARED || packet == nullptr) {
		_record_error("HCSR semantic worker returned an invalid completion");
		return;
	}

	PreparedGPUFrameMetadata packet_metadata;
	uint64_t packet_generation = 0;
	if (!_read_gpu_packet_metadata(packet, packet_metadata, packet_generation)) {
		_abandon_gpu_frame_packet(packet);
		return;
	}
	const Vector2i published_scroll_offset(
			Math::round(packet_metadata.scroll_offset.x),
			Math::round(packet_metadata.scroll_offset.y));
	const Vector2i clamped_scroll_offset(
			CLAMP(published_scroll_offset.x, 0, MAX(0, packet_metadata.content_width - size.x)),
			CLAMP(published_scroll_offset.y, 0, MAX(0, packet_metadata.content_height - size.y)));
	scroll_offset = clamped_scroll_offset;
	if (published_scroll_offset != clamped_scroll_offset) {
		_release_gpu_packet_metadata(packet_metadata);
		_abandon_gpu_frame_packet(packet);
		if (_set_input() == OK) {
			_request_semantic_worker_frame();
		}
		return;
	}
	_queue_prepared_gpu_packet(packet, packet_metadata, packet_generation);
}

bool HTMLSurfaceHCSRBackend::_queue_prepared_gpu_packet(hcsr_gpu_frame_packet_t *packet, PreparedGPUFrameMetadata &packet_metadata, uint64_t packet_generation) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		_release_gpu_packet_metadata(packet_metadata);
		_abandon_gpu_frame_packet(packet);
		terminal_failure_reason = "RenderingServer is unavailable for HCSR GPU rendering.";
		return false;
	}
	_stage_gpu_packet_metadata(packet_generation, packet_metadata);
	if (rendering_server->is_on_render_thread()) {
		gpu_frame_pending.set();
		_render_gpu_frame_on_render_thread(packet);
		if (!gpu_submission_deferred.is_set()) {
			gpu_frame_pending.clear();
		}
		return gpu_render_succeeded;
	} else {
		gpu_frame_pending.set();
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_render_gpu_frame_on_render_thread_callback).bind((uint64_t)this, (uint64_t)packet));
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_render_frame() {
	if (semantic_worker_enabled
			&& semantic_worker_pending
			&& (viewport_dirty || document_dirty)) {
		gpu_follow_up_frame_requested.set();
		return true;
	}
	if (!_sync_viewport()
			|| !_sync_document()
			|| _set_input() != OK
			|| (!semantic_worker_enabled && !_set_host_frame_context())) {
		return false;
	}
	if (render_backend != HCSR_RENDER_BACKEND_CPU) {
		const bool rendered = _render_gpu_frame();
		return rendered;
	}
	_ensure_presentation_outputs_on_render_thread();
	hcsr_frame_t output = {};
	for (int attempt = 0; attempt < 2; attempt++) {
		output = {};
		output.struct_size = sizeof(output);
		const uint64_t managed_export_call_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		if (hcsr_renderer_render_frame(renderer, timeline_time_seconds, &output) != HCSR_STATUS_OK) {
			_record_error("HCSR could not render the Godot frame");
			return false;
		}
		_record_managed_export_boundary_overhead(managed_export_call_start_usec);
		if (!_update_frame_schedule()) {
			hcsr_renderer_release_frame(renderer, &output);
			return false;
		}
		_retire_document_commits();
		_update_latest_performance_profile();
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

	const uint64_t frame_generation = output.generation;
	const uint64_t primary_publication_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const bool rendered = submit_cpu_frame_data(
			Size2i(output.width, output.height),
			output.stride,
			HTML_FRAME_PIXEL_FORMAT_BGRA8,
			output.pixels,
			int64_t(output.stride) * output.height,
			true,
			transparent_background) == OK;
	integration_counters.cpu_primary_publication_milliseconds = primary_publication_start_usec != 0
			? (OS::get_singleton()->get_ticks_usec() - primary_publication_start_usec) / 1000.0
			: 0.0;
	integration_counters.cpu_primary_conversion_milliseconds = cpu_frame_conversion_milliseconds;
	integration_counters.cpu_primary_upload_milliseconds = cpu_frame_upload_milliseconds;
	if (rendered) {
		_complete_performance_profile(frame_generation);
	}
	hcsr_renderer_release_frame(renderer, &output);
	if (rendered) {
		const uint64_t secondary_publication_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		_poll_cpu_presentation_outputs();
		integration_counters.cpu_secondary_publication_milliseconds = secondary_publication_start_usec != 0
				? (OS::get_singleton()->get_ticks_usec() - secondary_publication_start_usec) / 1000.0
				: 0.0;
		_publish_integration_counters();
		{
			MutexLock lock(frame_metadata_mutex);
			last_queued_frame_generation = frame_generation;
			active_gpu_frame_generation = frame_generation;
		}
		_read_backdrop_filter_regions();
	}
	return rendered;
}

void HTMLSurfaceHCSRBackend::_read_backdrop_filter_regions() {
	HTMLFrameMetadata next_metadata;
	next_metadata.logical_size = size;
	next_metadata.physical_size = physical_size;
	next_metadata.device_scale_factor = device_scale_factor;
	next_metadata.generation = active_gpu_frame_generation;
	if (renderer != nullptr) {
		hcsr_frame_evidence_t evidence = {};
		evidence.struct_size = sizeof(evidence);
		if (hcsr_renderer_get_frame_evidence(renderer, &evidence) == HCSR_STATUS_OK
				&& evidence.logical_frame_generation == next_metadata.generation) {
			next_metadata.host_frame_number = evidence.host_frame_number;
			next_metadata.timeline_time_seconds = evidence.timeline_time_seconds;
		}
	}
	if (!backdrop_filter_enabled || renderer == nullptr) {
		MutexLock lock(frame_metadata_mutex);
		frame_metadata = next_metadata;
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
		next_metadata.backdrop_filter_regions.push_back(region);
	}
	MutexLock lock(frame_metadata_mutex);
	frame_metadata = next_metadata;
}

void HTMLSurfaceHCSRBackend::_update_latest_performance_profile() {
#ifdef DEBUG_ENABLED
	if (renderer == nullptr) {
		return;
	}
	hcsr_performance_profile_t profile = {};
	profile.struct_size = sizeof(profile);
	if (hcsr_renderer_get_performance_profile(renderer, &profile) == HCSR_STATUS_OK) {
		HCSRPerformanceMonitor::update_latest((uint64_t)this, profile);
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		if (rendering_server == nullptr || !rendering_server->is_on_render_thread()) {
			HCSRPerformanceMonitor::publish_frame_data();
		}
	}
#endif
}

void HTMLSurfaceHCSRBackend::_complete_performance_profile(uint64_t p_generation) {
#ifdef DEBUG_ENABLED
	if (renderer == nullptr || p_generation == 0) {
		return;
	}
	hcsr_performance_profile_t profile = {};
	profile.struct_size = sizeof(profile);
	if (hcsr_renderer_get_performance_profile(renderer, &profile) == HCSR_STATUS_OK) {
		HCSRPerformanceMonitor::complete_generation((uint64_t)this, p_generation, profile);
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		if (rendering_server == nullptr || !rendering_server->is_on_render_thread()) {
			HCSRPerformanceMonitor::publish_frame_data();
		}
	}
#endif
}

void HTMLSurfaceHCSRBackend::_record_managed_export_boundary_overhead(uint64_t p_call_start_usec) {
#ifdef DEBUG_ENABLED
	if (renderer == nullptr || p_call_start_usec == 0 || OS::get_singleton() == nullptr) {
		return;
	}
	hcsr_performance_profile_t profile = {};
	profile.struct_size = sizeof(profile);
	if (hcsr_renderer_get_performance_profile(renderer, &profile) != HCSR_STATUS_OK) {
		return;
	}
	const double host_call_milliseconds = double(OS::get_singleton()->get_ticks_usec() - p_call_start_usec) / 1000.0;
	integration_counters.managed_export_boundary_overhead_milliseconds =
			MAX(0.0, host_call_milliseconds - profile.native_total_milliseconds);
	_publish_integration_counters();
#endif
}

void HTMLSurfaceHCSRBackend::_publish_integration_counters() {
#ifdef DEBUG_ENABLED
	HCSRPerformanceMonitor::update_integration((uint64_t)this, integration_counters);
#endif
}

bool HTMLSurfaceHCSRBackend::_update_frame_schedule() {
	if (renderer == nullptr) {
		begin_frame_requested = false;
		next_begin_frame_time_seconds = 0.0;
		return false;
	}

	hcsr_frame_schedule_t schedule = {};
	schedule.struct_size = sizeof(schedule);
	if (hcsr_renderer_get_frame_schedule(renderer, &schedule) != HCSR_STATUS_OK) {
		begin_frame_requested = false;
		next_begin_frame_time_seconds = 0.0;
		_record_error("HCSR could not query frame scheduling metadata");
		return false;
	}

	begin_frame_requested = schedule.needs_begin_frame != 0;
	const double delay_seconds = Math::is_finite(schedule.suggested_next_frame_delay_seconds)
			? MAX(0.0, schedule.suggested_next_frame_delay_seconds)
			: 0.0;
	next_begin_frame_time_seconds = begin_frame_requested ? timeline_time_seconds + delay_seconds : 0.0;
	return true;
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
	viewport_revision.increment();
	HTMLSurfaceCPUBackend::set_size(new_size);
	viewport_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_device_scale_factor(float p_device_scale_factor) {
	const float new_scale = CLAMP(Math::is_finite(p_device_scale_factor) && p_device_scale_factor > 0.0f ? p_device_scale_factor : 1.0f, 0.01f, 8.0f);
	if (!Math::is_equal_approx(device_scale_factor, new_scale)) {
		viewport_revision.increment();
		device_scale_factor = new_scale;
		viewport_dirty = true;
	}
}

void HTMLSurfaceHCSRBackend::set_physical_size(const Size2i &p_physical_size) {
	const Size2i new_physical_size(MAX(1, p_physical_size.x), MAX(1, p_physical_size.y));
	if (physical_size != new_physical_size) {
		viewport_revision.increment();
		physical_size = new_physical_size;
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
	{
		MutexLock lock(frame_metadata_mutex);
		frame_metadata.backdrop_filter_regions.clear();
	}
	if (renderer != nullptr && hcsr_renderer_set_backdrop_metadata_enabled(renderer, p_enabled ? 1 : 0) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected backdrop metadata configuration");
	}
}

Error HTMLSurfaceHCSRBackend::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	timeline_time_seconds = MAX(0.0, p_timeline_time_seconds);
	const bool scheduled_frame_due = begin_frame_requested && timeline_time_seconds + 0.000001 >= next_begin_frame_time_seconds;
	const bool state_sync_required = viewport_dirty || document_dirty || gpu_follow_up_frame_requested.is_set();
	if (r_needs_output != nullptr) {
		*r_needs_output = state_sync_required || !begin_frame_requested || scheduled_frame_due;
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = !state_sync_required && begin_frame_requested && !scheduled_frame_due;
	}
	return document.is_valid() && _ensure_renderer() ? OK : ERR_UNAVAILABLE;
}

void HTMLSurfaceHCSRBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	if (!_render_frame()) {
		clear_to_background();
	}
}

bool HTMLSurfaceHCSRBackend::poll_pending_output(bool *r_waiting_for_completion) {
	const HTMLPendingOutputState state = consume_pending_output_state();
	schedule_retirement_service();
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = state.producer_blocked;
	}
	return state.presentation_changed;
}

HTMLPendingOutputState HTMLSurfaceHCSRBackend::consume_pending_output_state() {
	_poll_semantic_worker_frame();
	HCSRPerformanceMonitor::publish_frame_data();
	HTMLPendingOutputState state;
	const bool follow_up_requested = gpu_follow_up_frame_requested.is_set();
	state.presentation_changed = gpu_presentation_changed.clear_if_set();
	state.producer_blocked = (follow_up_requested && gpu_frame_pending.is_set())
			|| gpu_submission_deferred.is_set()
			|| gpu_submission_retry_pending.is_set();
	state.retirement_pending = gpu_presentation_work_pending.is_set()
			|| gpu_presentation_poll_pending.is_set();
	return state;
}

void HTMLSurfaceHCSRBackend::schedule_retirement_service() {
	if (_uses_async_gpu_presentation()
			&& gpu_presentation_work_pending.is_set()
			&& !gpu_presentation_poll_pending.is_set()) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		if (rendering_server != nullptr) {
			gpu_presentation_poll_pending.set();
			if (rendering_server->is_on_render_thread()) {
				_poll_gpu_presentation_on_render_thread();
			} else {
				rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_poll_gpu_presentation_on_render_thread_callback).bind((uint64_t)this));
			}
		}
	}
	_schedule_deferred_gpu_submission();
}

bool HTMLSurfaceHCSRBackend::has_pending_output() const {
	return semantic_worker_pending
			|| gpu_follow_up_frame_requested.is_set()
			|| gpu_frame_pending.is_set()
			|| gpu_submission_deferred.is_set()
			|| gpu_submission_retry_pending.is_set()
			|| gpu_presentation_work_pending.is_set()
			|| gpu_presentation_poll_pending.is_set()
			|| gpu_presentation_changed.is_set();
}

bool HTMLSurfaceHCSRBackend::has_pending_frame_request() const {
	return viewport_dirty || document_dirty || gpu_follow_up_frame_requested.is_set();
}

uint64_t HTMLSurfaceHCSRBackend::get_last_queued_frame_generation() const {
	MutexLock lock(frame_metadata_mutex);
	return last_queued_frame_generation;
}

uint64_t HTMLSurfaceHCSRBackend::get_active_frame_generation() const {
	if (gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
		uint64_t generation = synchronized_active_generation.get();
		MutexLock output_lock(presentation_outputs_mutex);
		for (const KeyValue<uint64_t, PresentationOutputState *> &entry : presentation_outputs) {
			if (entry.value != nullptr) {
				generation = MIN(generation, entry.value->active_generation);
			}
		}
		return generation;
	}
	MutexLock lock(frame_metadata_mutex);
	return active_gpu_frame_generation;
}

bool HTMLSurfaceHCSRBackend::uses_generation_bound_input() const {
	return _uses_async_gpu_presentation();
}

bool HTMLSurfaceHCSRBackend::is_begin_frame_requested() const {
	return begin_frame_requested;
}

bool HTMLSurfaceHCSRBackend::has_terminal_render_failure() const {
	return terminal_failure;
}

String HTMLSurfaceHCSRBackend::get_terminal_render_failure_reason() const {
	return terminal_failure_reason;
}

Error HTMLSurfaceHCSRBackend::mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed) {
	(void)p_modifiers;
	const double event_time_seconds = OS::get_singleton() != nullptr ? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0 : 0.0;
	r_visual_state_changed = false;
	pointer_position = p_position;
	if (!_ensure_renderer()) {
		return ERR_CANT_CREATE;
	}
	const uint32_t buttons = primary_button_pressed ? 1U : 0U;
	uint32_t damage_flags = HCSR_POINTER_DAMAGE_NONE;
	const hcsr_status_t status = hcsr_renderer_dispatch_pointer_move_ex4(
			renderer,
			p_position.x,
			p_position.y,
			primary_button_pressed ? 1 : 0,
			scroll_offset.x,
			scroll_offset.y,
			HCSR_POINTER_KIND_MOUSE,
			buttons,
			1,
			event_time_seconds,
			&damage_flags);
	if (status == HCSR_STATUS_OK) {
		input_state_cache.mark_synchronized(pointer_position, primary_button_pressed, scroll_offset);
	}
	r_visual_state_changed = (damage_flags & HCSR_POINTER_DAMAGE_VISUAL) != 0;
	if (r_visual_state_changed) {
		semantic_state_revision.increment();
	}
	return status == HCSR_STATUS_OK ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error HTMLSurfaceHCSRBackend::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	pointer_position = p_position;
	if (p_button == HTML_SURFACE_MOUSE_BUTTON_LEFT) {
		primary_button_pressed = true;
	}
	const Error input_error = _set_input();
	if (input_error != OK) {
		return input_error;
	}
	const bool dispatched = hcsr_renderer_dispatch_pointer_down(renderer, p_position.x, p_position.y, (hcsr_pointer_button_t)p_button, 1) == HCSR_STATUS_OK;
	if (dispatched) {
		semantic_state_revision.increment();
	}
	return dispatched ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error HTMLSurfaceHCSRBackend::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_modifiers;
	(void)p_click_count;
	pointer_position = p_position;
	if (p_button == HTML_SURFACE_MOUSE_BUTTON_LEFT) {
		primary_button_pressed = false;
	}
	const Error input_error = _set_input();
	if (input_error != OK) {
		return input_error;
	}
	const bool dispatched = hcsr_renderer_dispatch_pointer_up(renderer, p_position.x, p_position.y, (hcsr_pointer_button_t)p_button, 1) == HCSR_STATUS_OK;
	if (dispatched) {
		semantic_state_revision.increment();
	}
	return dispatched ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error HTMLSurfaceHCSRBackend::pointer_cancel(const Point2 &p_position, int p_pointer_id) {
	pointer_position = p_position;
	primary_button_pressed = false;
	const Error input_error = _set_input();
	if (input_error != OK) {
		return input_error;
	}
	const bool dispatched = hcsr_renderer_dispatch_pointer_cancel(renderer, p_position.x, p_position.y, p_pointer_id) == HCSR_STATUS_OK;
	if (dispatched) {
		semantic_state_revision.increment();
	}
	return dispatched ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error HTMLSurfaceHCSRBackend::notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) {
	pointer_position = p_position;
	if (p_cancel_pressed_interaction) {
		primary_button_pressed = false;
	}
	const Error input_error = _set_input();
	if (input_error != OK) {
		return input_error;
	}
	const bool dispatched = hcsr_renderer_notify_pointer_leave(renderer, p_position.x, p_position.y, p_cancel_pressed_interaction ? 1 : 0, p_pointer_id) == HCSR_STATUS_OK;
	if (dispatched) {
		semantic_state_revision.increment();
	}
	return dispatched ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error HTMLSurfaceHCSRBackend::begin_scrollbar_interaction(const Point2 &p_position, double p_event_time_seconds, bool &r_consumed) {
	pointer_position = p_position;
	const Error input_error = _set_input();
	if (input_error != OK) {
		r_consumed = false;
		return input_error;
	}

	uint8_t consumed = 0;
	if (hcsr_renderer_begin_scrollbar_interaction(renderer, p_position.x, p_position.y, p_event_time_seconds, &consumed) != HCSR_STATUS_OK) {
		r_consumed = false;
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	r_consumed = consumed != 0;
	if (r_consumed) {
		semantic_state_revision.increment();
	}
	return OK;
}

Error HTMLSurfaceHCSRBackend::update_scrollbar_interaction(const Point2 &p_position, bool &r_consumed) {
	pointer_position = p_position;
	uint8_t consumed = 0;
	if (hcsr_renderer_update_scrollbar_interaction(renderer, p_position.x, p_position.y, &consumed) != HCSR_STATUS_OK) {
		r_consumed = false;
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	r_consumed = consumed != 0;
	if (r_consumed) {
		semantic_state_revision.increment();
	}
	return OK;
}

Error HTMLSurfaceHCSRBackend::end_scrollbar_interaction(bool &r_consumed) {
	uint8_t consumed = 0;
	if (hcsr_renderer_end_scrollbar_interaction(renderer, &consumed) != HCSR_STATUS_OK) {
		r_consumed = false;
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	r_consumed = consumed != 0;
	if (r_consumed) {
		semantic_state_revision.increment();
	}
	return OK;
}

bool HTMLSurfaceHCSRBackend::poll_pointer_event(HTMLPointerEvent &r_event) {
	if (renderer == nullptr || hcsr_renderer_pointer_event_count(renderer) == 0) {
		return false;
	}

	hcsr_pointer_event_t source = {};
	source.struct_size = sizeof(source);
	if (hcsr_renderer_poll_pointer_event(renderer, &source) != HCSR_STATUS_OK || source.struct_size == 0) {
		return false;
	}

	r_event = HTMLPointerEvent();
	r_event.sequence = source.sequence;
	r_event.type = (HTMLPointerEventType)source.type;
	r_event.target.element_id = StringName(String::utf8(source.element_id_utf8));
	r_event.target.tag_name = StringName(String::utf8(source.tag_name_utf8));
	r_event.target.bounds = Rect2i(source.left, source.top, MAX(0, source.right - source.left), MAX(0, source.bottom - source.top));
	r_event.target.disabled = source.disabled != 0;
	r_event.action_element_id = StringName(String::utf8(source.action_element_id_utf8));
	r_event.action = String::utf8(source.action_utf8);
	if (!r_event.action.is_empty()) {
		HTMLElementAttribute attribute;
		attribute.name = SNAME("data-godot-action");
		attribute.value = r_event.action;
		r_event.target.attributes.push_back(attribute);
	}
	r_event.document_position = Point2(source.document_x, source.document_y);
	r_event.button = source.button;
	r_event.buttons = source.buttons;
	r_event.pointer_id = source.pointer_id;
	r_event.bubbles = source.bubbles != 0;
	r_event.default_prevented = source.default_prevented != 0;
	r_event.state_changed = source.state_changed != 0;
	return true;
}

Error HTMLSurfaceHCSRBackend::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	pointer_position = p_position;
	const Error input_error = _set_input();
	if (input_error != OK) {
		return input_error;
	}

	uint8_t consumed = 0;
	const double event_time_seconds = OS::get_singleton() != nullptr ? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0 : 0.0;
	if (hcsr_renderer_dispatch_scroll(renderer, p_position.x, p_position.y, p_delta.x, p_delta.y,
			HCSR_SCROLL_GRANULARITY_PRECISE_PIXEL, HCSR_SCROLL_SOURCE_MOUSE_WHEEL,
			event_time_seconds, &consumed) != HCSR_STATUS_OK) {
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if (consumed != 0) {
		// Nested scrolling is committed through HCSR's scroll property tree: the
		// content transform advances while its scrollport clip remains anchored.
		// Mirroring the delta into the document offset would apply it twice.
		semantic_state_revision.increment();
		return OK;
	}

	// HTMLView supplies a signed pixel delta. Positive values move the viewport
	// toward increasing document coordinates. Only fall back to document scrolling
	// when no hovered scrollable ancestor could consume the delta.
	const Point2 previous_scroll_offset = scroll_offset;
	scroll_offset.x = MAX(0, scroll_offset.x + Math::round(p_delta.x));
	scroll_offset.y = MAX(0, scroll_offset.y + Math::round(p_delta.y));
	bool scroll_offset_changed = false;
	if (!_clamp_scroll_offset_to_content(scroll_offset_changed)) {
		return FAILED;
	}
	if (scroll_offset != previous_scroll_offset) {
		semantic_state_revision.increment();
	}
	return OK;
}

bool HTMLSurfaceHCSRBackend::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	if (renderer == nullptr) {
		return false;
	}

	hcsr_element_hit_t source = {};
	source.struct_size = sizeof(source);
	if (render_backend == HCSR_RENDER_BACKEND_CPU) {
		if (hcsr_renderer_hit_test(renderer, Math::floor(p_position.x), Math::floor(p_position.y), &source) != HCSR_STATUS_OK || source.element_key_utf8[0] == '\0') {
			return false;
		}
	} else {
		MutexLock lock(frame_metadata_mutex);
		if (active_hit_test_snapshot == nullptr
				|| hcsr_hit_test_snapshot_hit_test(active_hit_test_snapshot, Math::floor(p_position.x), Math::floor(p_position.y), &source) != HCSR_STATUS_OK
				|| source.element_key_utf8[0] == '\0') {
			return false;
		}
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

Error HTMLSurfaceHCSRBackend::scroll_element_into_view(const StringName &p_id, const StringName &p_block_alignment) {
	if (!_sync_document()) {
		return ERR_UNAVAILABLE;
	}
	const CharString id_utf8 = String(p_id).utf8();
	const CharString alignment_utf8 = String(p_block_alignment).utf8();
	if (hcsr_renderer_scroll_element_into_view(renderer, id_utf8.ptr(), alignment_utf8.ptr()) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected an element scroll-into-view request");
		return ERR_INVALID_PARAMETER;
	}
	semantic_state_revision.increment();
	return OK;
}

Error HTMLSurfaceHCSRBackend::set_form_control_value(const StringName &p_id, const String &p_value) {
	if (!_sync_document()) {
		return ERR_UNAVAILABLE;
	}
	const CharString id_utf8 = String(p_id).utf8();
	const CharString value_utf8 = p_value.utf8();
	if (hcsr_renderer_set_form_control_value(renderer, id_utf8.ptr(), value_utf8.ptr()) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected a form-control value update");
		return ERR_INVALID_PARAMETER;
	}
	semantic_state_revision.increment();
	return OK;
}

Error HTMLSurfaceHCSRBackend::set_form_control_checked(const StringName &p_id, bool p_checked) {
	if (!_sync_document()) {
		return ERR_UNAVAILABLE;
	}
	const CharString id_utf8 = String(p_id).utf8();
	if (hcsr_renderer_set_form_control_checked(renderer, id_utf8.ptr(), p_checked ? 1 : 0) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected a form-control checked update");
		return ERR_INVALID_PARAMETER;
	}
	semantic_state_revision.increment();
	return OK;
}

bool HTMLSurfaceHCSRBackend::get_form_control_state(const StringName &p_id, HTMLFormControlState &r_state) {
	if (!_sync_document()) {
		return false;
	}
	const CharString id_utf8 = String(p_id).utf8();
	hcsr_form_control_state_t source = {};
	source.struct_size = sizeof(source);
	if (hcsr_renderer_get_form_control_state(renderer, id_utf8.ptr(), &source) != HCSR_STATUS_OK) {
		return false;
	}
	r_state = HTMLFormControlState();
	r_state.element_id = StringName(String::utf8(source.element_id_utf8));
	r_state.tag_name = StringName(String::utf8(source.tag_name_utf8));
	r_state.value = String::utf8(source.value_utf8);
	r_state.checked = source.checked != 0;
	r_state.selected = source.selected != 0;
	r_state.selected_index = source.selected_index;
	r_state.focused = source.focused != 0;
	return true;
}

void HTMLSurfaceHCSRBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	MutexLock lock(frame_metadata_mutex);
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

uint64_t HTMLSurfaceHCSRBackend::create_presentation_output(const Size2i &p_size, bool p_mipmaps) {
	if (p_size.x <= 0 || p_size.y <= 0) {
		return 0;
	}
	PresentationOutputState *state = memnew(PresentationOutputState);
	state->requested_size = p_size;
	state->mipmaps = p_mipmaps;
	state->texture.instantiate();
	uint64_t output_id = 0;
	{
		MutexLock lock(presentation_outputs_mutex);
		output_id = next_presentation_output_id++;
		presentation_outputs.insert(output_id, state);
	}
	if (render_backend == HCSR_RENDER_BACKEND_CPU) {
		begin_frame_requested = true;
	} else {
		presentation_output_topology_sync_required.set();
		gpu_follow_up_frame_requested.set();
	}
	_schedule_presentation_output_topology_sync();
	return output_id;
}

Error HTMLSurfaceHCSRBackend::resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, ERR_INVALID_PARAMETER);
	PresentationOutputState *state = nullptr;
	{
		MutexLock lock(presentation_outputs_mutex);
		PresentationOutputState *const *found = presentation_outputs.getptr(p_output_id);
		ERR_FAIL_NULL_V(found, ERR_DOES_NOT_EXIST);
		state = *found;
		if (state->requested_size == p_size) {
			return OK;
		}
		state->requested_size = p_size;
		state->resize_pending = true;
	}
	if (render_backend == HCSR_RENDER_BACKEND_CPU) {
		begin_frame_requested = true;
	} else {
		presentation_output_topology_sync_required.set();
		gpu_follow_up_frame_requested.set();
		_schedule_presentation_output_topology_sync();
	}
	return OK;
}

void HTMLSurfaceHCSRBackend::destroy_presentation_output(uint64_t p_output_id) {
	PresentationOutputState *state = nullptr;
	{
		MutexLock lock(presentation_outputs_mutex);
		PresentationOutputState *const *found = presentation_outputs.getptr(p_output_id);
		if (found == nullptr) {
			return;
		}
		state = *found;
		presentation_outputs.erase(p_output_id);
	}
	if (renderer != nullptr && state->output != nullptr
			&& hcsr_renderer_detach_presentation_output(renderer, state->output) != HCSR_STATUS_OK) {
		_record_error("HCSR could not detach a secondary presentation output from synchronized topology");
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr || rendering_server->is_on_render_thread()) {
		_destroy_presentation_output_state_on_render_thread(state);
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_destroy_presentation_output_state_on_render_thread_callback).bind((uint64_t)this, (uint64_t)state));
	}
}

Ref<Texture2D> HTMLSurfaceHCSRBackend::get_presentation_output_texture(uint64_t p_output_id) const {
	MutexLock lock(presentation_outputs_mutex);
	PresentationOutputState *const *found = presentation_outputs.getptr(p_output_id);
	return found != nullptr ? Ref<Texture2D>((*found)->texture) : Ref<Texture2D>();
}

uint64_t HTMLSurfaceHCSRBackend::get_presentation_output_generation(uint64_t p_output_id) const {
	MutexLock lock(presentation_outputs_mutex);
	PresentationOutputState *const *found = presentation_outputs.getptr(p_output_id);
	if (found == nullptr) {
		return 0;
	}
	if (gpu_capabilities.synchronization_mode == HCSR_GPU_SYNCHRONIZATION_ENGINE_QUEUE_ORDERED) {
		return MIN((*found)->active_generation, synchronized_active_generation.get());
	}
	return (*found)->active_generation;
}

HTMLSurfaceHCSRBackend::HTMLSurfaceHCSRBackend(hcsr_render_backend_t p_render_backend) :
		render_backend(p_render_backend) {
	semantic_worker_enabled = render_backend != HCSR_RENDER_BACKEND_CPU
			&& OS::get_singleton() != nullptr
			&& OS::get_singleton()->get_environment("HCSR_SEMANTIC_WORKER") == "1";
#ifdef DEBUG_ENABLED
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings != nullptr) {
		diagnostic_capacity_block_after_submissions = MAX(0, int(project_settings->get_setting(
				"rendering/html_css/hcsr/testing/capacity_block_after_submissions",
				0)));
		diagnostic_capacity_block_frames = MAX(0, int(project_settings->get_setting(
				"rendering/html_css/hcsr/testing/capacity_block_frames",
				0)));
	}
#endif
}

HTMLSurfaceHCSRBackend::~HTMLSurfaceHCSRBackend() {
	_detach_gpu_texture_import();
	if (gpu_texture.is_valid()) {
		gpu_texture.unref();
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (render_backend == HCSR_RENDER_BACKEND_CPU || rendering_server == nullptr || rendering_server->is_on_render_thread()) {
		_destroy_renderer_on_render_thread();
	} else {
		rendering_server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRBackend::_destroy_renderer_on_render_thread_callback).bind((uint64_t)this));
		rendering_server->sync();
	}
#ifdef DEBUG_ENABLED
	HCSRPerformanceMonitor::remove((uint64_t)this);
#endif
}
