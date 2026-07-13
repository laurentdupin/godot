/**************************************************************************/
/*  html_surface_hcsr_backend.cpp                                        */
/**************************************************************************/

#include "html_surface_hcsr_backend.h"

#include "../bridge/html_asset_provider.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"

static String hcsr_color_to_css_rgba(const Color &p_color) {
	return vformat("rgba(%d, %d, %d, %.6f)",
			Math::round(p_color.r * 255.0f),
			Math::round(p_color.g * 255.0f),
			Math::round(p_color.b * 255.0f),
			p_color.a);
}

static String hcsr_inject_document_style(const String &p_html, const Color &p_background, const String &p_css) {
	String style = "<style data-godot-hcsr=\"true\">:where(html), :where(body) { background-color: " + hcsr_color_to_css_rgba(p_background) + "; }\n";
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
	const hcsr_status_t status = hcsr_renderer_create(&config, &renderer);
	if (status != HCSR_STATUS_OK || renderer == nullptr) {
		terminal_failure = true;
		terminal_failure_reason = "HCSR could not create its static renderer instance.";
		ERR_PRINT(terminal_failure_reason);
		return false;
	}

	viewport_dirty = true;
	document_dirty = true;
	return true;
}

void HTMLSurfaceHCSRBackend::_record_error(const String &p_context) {
	const char *last_error = renderer != nullptr ? hcsr_renderer_last_error(renderer) : nullptr;
	terminal_failure_reason = p_context;
	if (last_error != nullptr && last_error[0] != '\0') {
		terminal_failure_reason += ": " + String::utf8(last_error);
	}
	terminal_failure = true;
	ERR_PRINT(terminal_failure_reason);
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

	String css;
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
	r_html = hcsr_inject_document_style(html, document->get_background_color(), css);
	String resource_root = document->get_resource_root();
	if (resource_root.is_empty()) {
		resource_root = "res://";
	}
	r_asset_root = ProjectSettings::get_singleton()->globalize_path(resource_root);
	if (document_path.is_empty()) {
		document_path = resource_root.path_join("hcsr_document.html");
	}
	r_document_path = ProjectSettings::get_singleton()->globalize_path(document_path);
	return true;
}

bool HTMLSurfaceHCSRBackend::_sync_document() {
	if (!document_dirty) {
		return true;
	}
	if (!_ensure_renderer()) {
		return false;
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
	if (hcsr_renderer_set_asset_root(renderer, root_utf8.ptr()) != HCSR_STATUS_OK ||
			hcsr_renderer_set_document(renderer, path_utf8.ptr(), html_utf8.ptr()) != HCSR_STATUS_OK) {
		_record_error("HCSR rejected the Godot HTMLDocument");
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

bool HTMLSurfaceHCSRBackend::_render_frame() {
	if (!_sync_viewport() || !_sync_document() || _set_input() != OK) {
		return false;
	}
	hcsr_frame_t output = {};
	output.struct_size = sizeof(output);
	if (hcsr_renderer_render_frame(renderer, timeline_time_seconds, &output) != HCSR_STATUS_OK) {
		_record_error("HCSR could not render the Godot frame");
		return false;
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
	return submit_cpu_frame(frame) == OK;
}

void HTMLSurfaceHCSRBackend::mark_document_dirty() {
	document_dirty = true;
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
	}
}

void HTMLSurfaceHCSRBackend::set_background_color(const Color &p_background_color) {
	if (background_color != p_background_color) {
		HTMLSurfaceCPUBackend::set_background_color(p_background_color);
		document_dirty = true;
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
	scroll_offset.x = MAX(0, scroll_offset.x - Math::round(p_delta.x * 40.0f));
	scroll_offset.y = MAX(0, scroll_offset.y - Math::round(p_delta.y * 40.0f));
	return _set_input();
}

HTMLSurfaceHCSRBackend::HTMLSurfaceHCSRBackend() {
}

HTMLSurfaceHCSRBackend::~HTMLSurfaceHCSRBackend() {
	if (renderer != nullptr) {
		hcsr_renderer_destroy(renderer);
		renderer = nullptr;
	}
}
