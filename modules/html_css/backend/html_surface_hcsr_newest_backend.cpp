/**************************************************************************/
/*  html_surface_hcsr_newest_backend.cpp                                  */
/**************************************************************************/

#include "html_surface_hcsr_newest_backend.h"

#include "../bridge/html_asset_provider.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/regex.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_driver.h"
#include "servers/rendering/rendering_server.h"

struct HTMLSurfaceHCSRNewestBackend::State {
	mutable Mutex mutex;
	HTMLSurfaceHCSRNewestRenderer renderer = HTML_SURFACE_HCSR_NEWEST_CPU;
	hcsr_runtime_t runtime = 0;
	hcsr_source_t source = 0;
	hcsr_scene_t scene = 0;
	hcsr_draw_packet_t pending_packet = 0;
	hcsr_presenter_t presenter = 0;
	Ref<HTMLTexture2D> texture;
	Ref<HTMLDocument> document;
	Size2i logical_size = Size2i(512, 512);
	Size2i physical_size = Size2i(512, 512);
	float device_scale = 1.0f;
	Color background = Color(0, 0, 0, 1);
	Color placeholder = Color(0, 0, 0, 1);
	RID rd_texture;
	RID canvas_texture;
	HTMLFrameMetadata metadata;
	bool document_dirty = true;
	bool render_pending = false;
	void *pending_gpu_submission = nullptr;
	bool presentation_changed = false;
	bool needs_another_frame = false;
	bool gpu_texture_initialized = false;
	uint64_t queued_generation = 0;
	uint64_t active_generation = 0;
	bool terminal = false;
	bool closing = false;
	String terminal_reason;
};

namespace {
template <typename T>
static void initialize_abi(T &r_value) {
	memset(&r_value, 0, sizeof(T));
	r_value.struct_size = sizeof(T);
}

static void set_terminal(HTMLSurfaceHCSRNewestBackend::State *p_state, const String &p_reason) {
	p_state->terminal = true;
	p_state->terminal_reason = p_reason;
	ERR_PRINT(p_reason);
}

static hcsr_utf8_t utf8_view(const CharString &p_value) {
	hcsr_utf8_t result = {};
	result.data = p_value.get_data();
	result.length = p_value.length();
	return result;
}

static String html_attribute(const String &p_attributes, const String &p_name) {
	const String pattern = "(?i)(?:^|\\s)" + p_name + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'=<>`]+))";
	Ref<RegEx> expression = RegEx::create_from_string(pattern, false);
	if (expression.is_null()) return String();
	Ref<RegExMatch> match = expression->search(p_attributes);
	if (match.is_null()) return String();
	for (int group = 1; group <= 3; group++) {
		if (match->get_start(group) >= 0) return match->get_string(group);
	}
	return String();
}

static Error append_document_stylesheets(const Ref<HTMLDocument> &p_document, const String &p_html,
		Vector<CharString> &r_stylesheets, String &r_error) {
	Ref<RegEx> sources = RegEx::create_from_string(
			"(?is)<style\\b[^>]*>(.*?)</style\\s*>|<link\\b([^>]*)>", false);
	if (sources.is_null()) return ERR_BUG;
	const TypedArray<RegExMatch> matches = sources->search_all(p_html);
	for (int index = 0; index < matches.size(); index++) {
		const Ref<RegExMatch> match = matches[index];
		if (match->get_start(1) >= 0) {
			r_stylesheets.push_back(match->get_string(1).utf8());
			continue;
		}
		const String attributes = match->get_string(2);
		const PackedStringArray relations = html_attribute(attributes, "rel").split(" ", false);
		bool is_stylesheet = false;
		for (const String &relation : relations) {
			if (relation.nocasecmp_to("stylesheet") == 0) {
				is_stylesheet = true;
				break;
			}
		}
		const String href = html_attribute(attributes, "href");
		if (!is_stylesheet || href.is_empty()) continue;
		HTMLAssetResource asset;
		if (HTMLGodotAssetProvider::load_asset(p_document, href, asset, &r_error) != OK) return ERR_CANT_OPEN;
		r_stylesheets.push_back(String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size()).utf8());
	}
	return OK;
}

static String scene_error(HTMLSurfaceHCSRNewestBackend::State *p_state, const String &p_fallback) {
	const size_t required = hcsr_last_error(p_state->runtime, nullptr, 0);
	if (required <= 1) {
		return p_fallback;
	}
	Vector<char> bytes;
	bytes.resize(required);
	hcsr_last_error(p_state->runtime, bytes.ptrw(), bytes.size());
	const String message = String::utf8(bytes.ptr());
	return message.is_empty() ? p_fallback : message;
}

static uint32_t input_modifiers(int p_modifiers) {
	uint32_t result = 0;
	if (p_modifiers & HTML_SURFACE_INPUT_MODIFIER_SHIFT) result |= HCSR_INPUT_SHIFT;
	if (p_modifiers & HTML_SURFACE_INPUT_MODIFIER_CONTROL) result |= HCSR_INPUT_CONTROL;
	if (p_modifiers & HTML_SURFACE_INPUT_MODIFIER_ALT) result |= HCSR_INPUT_ALT;
	if (p_modifiers & HTML_SURFACE_INPUT_MODIFIER_META) result |= HCSR_INPUT_META;
	return result;
}

static uint32_t map_key(HTMLSurfaceInputKey p_key) {
	switch (p_key) {
		case HTML_SURFACE_INPUT_KEY_BACKSPACE: return HCSR_KEY_BACKSPACE;
		case HTML_SURFACE_INPUT_KEY_TAB: return HCSR_KEY_TAB;
		case HTML_SURFACE_INPUT_KEY_ENTER: return HCSR_KEY_ENTER;
		case HTML_SURFACE_INPUT_KEY_DELETE: return HCSR_KEY_DELETE;
		default: return HCSR_KEY_NONE;
	}
}

static void release_gpu_target(HTMLSurfaceHCSRNewestBackend::State *p_state, RenderingServer *p_server, RenderingDevice *p_device) {
	if (p_state->canvas_texture.is_valid() && p_server != nullptr) {
		p_server->free_rid(p_state->canvas_texture);
	}
	if (p_state->rd_texture.is_valid() && p_device != nullptr) {
		p_device->free_rid(p_state->rd_texture);
	}
	p_state->canvas_texture = RID();
	p_state->rd_texture = RID();
	p_state->gpu_texture_initialized = false;
}

static bool ensure_gpu_target(HTMLSurfaceHCSRNewestBackend::State *p_state, RenderingServer *p_server, RenderingDevice *p_device, const Size2i &p_physical_size) {
	if (p_state->rd_texture.is_valid()) {
		const RenderingDevice::TextureFormat existing = p_device->texture_get_format(p_state->rd_texture);
		if ((int)existing.width == p_physical_size.x && (int)existing.height == p_physical_size.y) {
			return true;
		}
		release_gpu_target(p_state, p_server, p_device);
	}
	RenderingDevice::TextureFormat format;
	format.format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	format.width = p_physical_size.x;
	format.height = p_physical_size.y;
	format.usage_bits = RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
	format.shareable_formats.push_back(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	format.shareable_formats.push_back(RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB);
	p_state->rd_texture = p_device->texture_create(format, RenderingDevice::TextureView());
	if (!p_state->rd_texture.is_valid()) {
		return false;
	}
	p_state->canvas_texture = p_server->texture_rd_create(p_state->rd_texture);
	if (!p_state->canvas_texture.is_valid()) {
		release_gpu_target(p_state, p_server, p_device);
		return false;
	}
	p_state->texture->set_external_texture(p_state->canvas_texture, p_physical_size, true);
	return true;
}

static bool ensure_presenter(HTMLSurfaceHCSRNewestBackend::State *p_state, RenderingDevice *p_device) {
	if (p_state->presenter != 0) return true;
	hcsr_presenter_desc_t common;
	initialize_abi(common);
	common.maximum_frames_in_flight = 3;
	if (p_state->renderer == HTML_SURFACE_HCSR_NEWEST_D3D12) {
		hcsr_d3d12_engine_desc_t backend;
		initialize_abi(backend);
		backend.render_target_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		backend.sample_count = 1;
		backend.device = (ID3D12Device *)p_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
		if (backend.device == nullptr) return false;
		const hcsr_result_t result = hcsr_d3d12_create_engine(&common, &backend, &p_state->presenter);
		if (result != HCSR_OK) ERR_PRINT(vformat("hcsr_newest D3D12 presenter creation failed with result %d.", (int)result));
		return result == HCSR_OK;
	}
	if (p_state->renderer == HTML_SURFACE_HCSR_NEWEST_VULKAN) {
		hcsr_vulkan_engine_desc_t backend;
		initialize_abi(backend);
		backend.graphics_queue_family = (uint32_t)p_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY);
		backend.render_target_format = VK_FORMAT_R8G8B8A8_UNORM;
		backend.samples = VK_SAMPLE_COUNT_1_BIT;
		backend.physical_device = (VkPhysicalDevice)p_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);
		backend.device = (VkDevice)p_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
		if (backend.physical_device == VK_NULL_HANDLE || backend.device == VK_NULL_HANDLE) return false;
		const hcsr_result_t result = hcsr_vulkan_create_engine(&common, &backend, &p_state->presenter);
		if (result != HCSR_OK) ERR_PRINT(vformat("hcsr_newest Vulkan presenter creation failed with result %d.", (int)result));
		return result == HCSR_OK;
	}
	return true;
}

static bool record_gpu(HTMLSurfaceHCSRNewestBackend::State *p_state, HTMLSurfaceHCSRNewestRenderer p_renderer,
		const hcsr_draw_packet_view_t &p_packet, const Size2i &p_physical_size, const Color &p_background,
		uint64_t p_command_buffer, uint64_t p_target, uint64_t p_target_view) {
	hcsr_frame_desc_t frame;
	initialize_abi(frame);
	frame.frame_slot = p_packet.scene_generation % 3;
	frame.frame_id = p_packet.scene_generation;
	frame.flags = HCSR_FRAME_CLEAR_TARGET;
	frame.clear_r = p_background.r;
	frame.clear_g = p_background.g;
	frame.clear_b = p_background.b;
	frame.clear_a = p_background.a;
	if (p_renderer == HTML_SURFACE_HCSR_NEWEST_D3D12) {
		hcsr_d3d12_record_target_t target;
		initialize_abi(target);
		target.width = p_physical_size.x;
		target.height = p_physical_size.y;
		target.command_list = (ID3D12GraphicsCommandList *)p_command_buffer;
		target.render_target = (ID3D12Resource *)p_target;
		if (target.command_list == nullptr || target.render_target == nullptr
				|| hcsr_d3d12_record(p_state->presenter, &p_packet, &frame, &target) != HCSR_OK) return false;
	} else {
		hcsr_vulkan_record_target_t target;
		initialize_abi(target);
		target.width = p_physical_size.x;
		target.height = p_physical_size.y;
		target.command_buffer = (VkCommandBuffer)p_command_buffer;
		target.image = (VkImage)p_target;
		target.image_view = (VkImageView)p_target_view;
		if (target.command_buffer == VK_NULL_HANDLE || target.image == VK_NULL_HANDLE || target.image_view == VK_NULL_HANDLE
				|| hcsr_vulkan_record(p_state->presenter, &p_packet, &frame, &target) != HCSR_OK) return false;
	}
	p_state->gpu_texture_initialized = true;
	return true;
}

static void render_cpu(HTMLSurfaceHCSRNewestBackend::State *p_state, const hcsr_draw_packet_view_t &p_packet, const Size2i &p_physical_size, const Color &p_background) {
	Ref<Image> image = Image::create_empty(p_physical_size.x, p_physical_size.y, false, Image::FORMAT_RGBA8);
	image->fill(p_background);
	const float scale_x = p_physical_size.x / MAX(1.0f, p_packet.viewport_width);
	const float scale_y = p_physical_size.y / MAX(1.0f, p_packet.viewport_height);
	for (size_t index = 0; index < p_packet.draw_item_count; index++) {
		const hcsr_draw_item_t &draw = p_packet.draw_items[index];
		if (draw.material_index >= p_packet.material_count) continue;
		const hcsr_material_t &material = p_packet.materials[draw.material_index];
		if (material.kind != HCSR_MATERIAL_AREA_GRAYSCALE
				|| material.payload_offset + sizeof(hcsr_area_grayscale_material_t) > p_packet.material_payload_size) continue;
		const hcsr_area_grayscale_material_t *area = (const hcsr_area_grayscale_material_t *)(p_packet.material_payload + material.payload_offset);
		const Rect2i bounds(
				Math::floor(draw.bounds.x * scale_x), Math::floor(draw.bounds.y * scale_y),
				Math::ceil(draw.bounds.width * scale_x), Math::ceil(draw.bounds.height * scale_y));
		image->fill_rect(bounds.intersection(Rect2i(Point2i(), p_physical_size)),
				Color(area->luminance, area->luminance, area->luminance, area->opacity));
	}
	p_state->texture->update_from_image(image);
}

struct HCSRNewestGpuSubmission {
	HTMLSurfaceHCSRNewestBackend::State *state = nullptr;
	hcsr_draw_packet_t packet = 0;
	HTMLSurfaceHCSRNewestRenderer renderer = HTML_SURFACE_HCSR_NEWEST_CPU;
	Size2i physical_size;
	Color background;
	uint64_t target = 0;
	uint64_t target_view = 0;
};

static void complete_gpu_submission(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_command_buffer, void *p_userdata) {
	HCSRNewestGpuSubmission *submission = (HCSRNewestGpuSubmission *)p_userdata;
	hcsr_draw_packet_view_t packet;
	initialize_abi(packet);
	bool rendered = hcsr_draw_packet_get_view(submission->packet, &packet) == HCSR_OK;
	const uint64_t generation = rendered ? packet.scene_generation : 0;
	const Size2i logical_size = rendered
			? Size2i(Math::ceil(packet.viewport_width), Math::ceil(packet.viewport_height))
			: Size2i();
	if (rendered) {
		const uint64_t native_command_buffer = p_driver->get_resource_native_handle(
				RenderingDeviceDriver::DRIVER_RESOURCE_COMMAND_BUFFER, p_command_buffer);
		rendered = native_command_buffer != 0 && record_gpu(submission->state, submission->renderer,
				packet, submission->physical_size, submission->background, native_command_buffer,
				submission->target, submission->target_view);
	}
	hcsr_draw_packet_destroy(submission->packet);
	{
		MutexLock lock(submission->state->mutex);
		if (submission->state->pending_gpu_submission == submission) submission->state->pending_gpu_submission = nullptr;
		if (submission->state->pending_packet == submission->packet) submission->state->pending_packet = 0;
		submission->state->render_pending = false;
		if (rendered && !submission->state->closing) {
			submission->state->presentation_changed = true;
			submission->state->metadata.generation = generation;
			submission->state->metadata.logical_size = logical_size;
			submission->state->metadata.physical_size = submission->physical_size;
			submission->state->active_generation = generation;
		} else if (!submission->state->closing) {
			set_terminal(submission->state, "hcsr_newest could not record the scene packet into Godot's rendering graph.");
		}
	}
	memdelete(submission);
}
} // namespace

void HTMLSurfaceHCSRNewestBackend::_render_on_render_thread(uint64_t p_state_pointer) {
	State *state = (State *)(uintptr_t)p_state_pointer;
	hcsr_draw_packet_t packet_handle = 0;
	HTMLSurfaceHCSRNewestRenderer renderer = HTML_SURFACE_HCSR_NEWEST_CPU;
	Size2i physical_size;
	Color background;
	{
		MutexLock lock(state->mutex);
		if (state->pending_packet == 0) {
			state->render_pending = false;
			return;
		}
		packet_handle = state->pending_packet;
		renderer = state->renderer;
		physical_size = state->physical_size;
		background = state->background;
	}
	hcsr_draw_packet_view_t packet;
	initialize_abi(packet);
	bool rendered = hcsr_draw_packet_get_view(packet_handle, &packet) == HCSR_OK;
	const uint64_t rendered_generation = rendered ? packet.scene_generation : 0;
	const Size2i rendered_logical_size = rendered
			? Size2i(Math::ceil(packet.viewport_width), Math::ceil(packet.viewport_height))
			: Size2i();
	if (rendered && renderer != HTML_SURFACE_HCSR_NEWEST_CPU) {
		RenderingServer *server = RenderingServer::get_singleton();
		RenderingDevice *device = server != nullptr ? server->get_rendering_device() : nullptr;
		rendered = server != nullptr && device != nullptr && ensure_gpu_target(state, server, device, physical_size)
				&& ensure_presenter(state, device);
		if (rendered) {
			HCSRNewestGpuSubmission *submission = memnew(HCSRNewestGpuSubmission);
			submission->state = state;
			submission->packet = packet_handle;
			submission->renderer = renderer;
			submission->physical_size = physical_size;
			submission->background = background;
			submission->target = device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, state->rd_texture);
			submission->target_view = device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, state->rd_texture);
			RenderingDevice::CallbackResource resource;
			resource.rid = state->rd_texture;
			resource.usage = RenderingDevice::CALLBACK_RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE;
			const Error callback_error = device->driver_callback_add_next_frame(complete_gpu_submission, submission, VectorView(&resource, 1));
			if (callback_error == OK) {
				MutexLock lock(state->mutex);
				state->pending_gpu_submission = submission;
				return;
			}
			memdelete(submission);
			rendered = false;
		}
	} else if (rendered) {
		render_cpu(state, packet, physical_size, background);
	}
	hcsr_draw_packet_destroy(packet_handle);
	{
		MutexLock lock(state->mutex);
		if (state->pending_packet == packet_handle) {
			state->pending_packet = 0;
		}
		state->render_pending = false;
		if (rendered && !state->closing) {
			state->presentation_changed = true;
			state->metadata.generation = rendered_generation;
			state->metadata.logical_size = rendered_logical_size;
			state->metadata.physical_size = physical_size;
			state->active_generation = rendered_generation;
		} else if (!state->closing) {
			set_terminal(state, "hcsr_newest could not record the scene packet into Godot's rendering device.");
		}
	}
}

void HTMLSurfaceHCSRNewestBackend::_cancel_gpu_submission_on_render_thread(uint64_t p_state_pointer) {
	State *state = (State *)(uintptr_t)p_state_pointer;
	HCSRNewestGpuSubmission *submission = nullptr;
	{
		MutexLock lock(state->mutex);
		submission = (HCSRNewestGpuSubmission *)state->pending_gpu_submission;
	}
	if (submission == nullptr) return;
	RenderingServer *server = RenderingServer::get_singleton();
	RenderingDevice *device = server != nullptr ? server->get_rendering_device() : nullptr;
	if (device == nullptr || !device->driver_callback_cancel_next_frame(complete_gpu_submission, submission)) return;
	{
		MutexLock lock(state->mutex);
		if (state->pending_gpu_submission == submission) state->pending_gpu_submission = nullptr;
		if (state->pending_packet == submission->packet) state->pending_packet = 0;
		state->render_pending = false;
	}
	hcsr_draw_packet_destroy(submission->packet);
	memdelete(submission);
}

Error HTMLSurfaceHCSRNewestBackend::_rebuild_scene() {
	ERR_FAIL_NULL_V(state, ERR_UNAVAILABLE);
	if (document.is_null() || !document->is_source_valid()) return ERR_INVALID_DATA;
	String html = document->get_html();
	if (html.is_empty() && !document->get_html_file().is_empty()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, document->get_html_file(), asset, &error) != OK) {
			set_terminal(state, error);
			return ERR_CANT_OPEN;
		}
		html = String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size());
	}
	if (html.strip_edges().is_empty()) return ERR_INVALID_DATA;
	Vector<CharString> stylesheet_bytes;
	String stylesheet_error;
	if (append_document_stylesheets(document, html, stylesheet_bytes, stylesheet_error) != OK) {
		set_terminal(state, stylesheet_error.is_empty() ? "hcsr_newest could not resolve a linked stylesheet." : stylesheet_error);
		return ERR_CANT_OPEN;
	}
	for (const String &path : document->get_css_files()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, path, asset, &error) != OK) {
			set_terminal(state, error);
			return ERR_CANT_OPEN;
		}
		stylesheet_bytes.push_back(String::utf8((const char *)asset.bytes.ptr(), asset.bytes.size()).utf8());
	}
	if (!document->get_css().is_empty()) stylesheet_bytes.push_back(document->get_css().utf8());
	Vector<hcsr_utf8_t> stylesheets;
	stylesheets.resize(stylesheet_bytes.size());
	for (int index = 0; index < stylesheet_bytes.size(); index++) stylesheets.write[index] = utf8_view(stylesheet_bytes[index]);
	CharString html_bytes = html.utf8();
	String resource_base_path = document->get_resource_root();
	if (resource_base_path.is_empty() && !document->get_html_file().is_empty()) {
		resource_base_path = document->get_html_file().get_base_dir();
	}
	CharString resource_base = resource_base_path.utf8();
	hcsr_source_desc_t source_desc;
	initialize_abi(source_desc);
	source_desc.html = utf8_view(html_bytes);
	source_desc.style_sheets = stylesheets.ptr();
	source_desc.style_sheet_count = stylesheets.size();
	source_desc.resource_base = utf8_view(resource_base);
	if (state->scene != 0) hcsr_scene_destroy(state->scene);
	if (state->source != 0) hcsr_source_destroy(state->source);
	state->scene = 0;
	state->source = 0;
	if (hcsr_source_create(state->runtime, &source_desc, &state->source) != HCSR_OK) {
		set_terminal(state, scene_error(state, "hcsr_newest could not create the document source."));
		return ERR_INVALID_DATA;
	}
	hcsr_scene_desc_t scene_desc;
	initialize_abi(scene_desc);
	scene_desc.viewport_width = state->logical_size.x;
	scene_desc.viewport_height = state->logical_size.y;
	scene_desc.device_scale = state->device_scale;
	if (hcsr_scene_create(state->source, &scene_desc, &state->scene) != HCSR_OK) {
		set_terminal(state, scene_error(state, "hcsr_newest could not create the scene."));
		return ERR_CANT_CREATE;
	}
	state->document_dirty = false;
	state->metadata.logical_size = state->logical_size;
	state->metadata.physical_size = state->physical_size;
	state->metadata.device_scale_factor = state->device_scale;
	return OK;
}

void HTMLSurfaceHCSRNewestBackend::mark_document_dirty() {
	MutexLock lock(state->mutex);
	state->document_dirty = true;
}

void HTMLSurfaceHCSRNewestBackend::set_size(const Size2i &p_size) {
	set_viewport_configuration(p_size, state->device_scale, Size2i());
}

void HTMLSurfaceHCSRNewestBackend::set_device_scale_factor(float p_device_scale_factor) {
	set_viewport_configuration(state->logical_size, p_device_scale_factor, Size2i());
}

void HTMLSurfaceHCSRNewestBackend::set_physical_size(const Size2i &p_physical_size) {
	set_viewport_configuration(state->logical_size, state->device_scale, p_physical_size);
}

void HTMLSurfaceHCSRNewestBackend::set_viewport_configuration(const Size2i &p_size, float p_device_scale_factor, const Size2i &p_physical_size) {
	MutexLock lock(state->mutex);
	const Size2i logical_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	const float device_scale = MAX(0.01f, p_device_scale_factor);
	const Size2i physical_size = p_physical_size.x > 0 && p_physical_size.y > 0
			? p_physical_size
			: Size2i(MAX(1, Math::ceil(logical_size.x * device_scale)), MAX(1, Math::ceil(logical_size.y * device_scale)));
	if (state->logical_size != logical_size || state->physical_size != physical_size || !Math::is_equal_approx(state->device_scale, device_scale)) {
		state->logical_size = logical_size;
		state->physical_size = physical_size;
		state->device_scale = device_scale;
		state->needs_another_frame = true;
	}
}

void HTMLSurfaceHCSRNewestBackend::set_document(const Ref<HTMLDocument> &p_document) {
	document = p_document;
	MutexLock lock(state->mutex);
	if (state->document == p_document) return;
	state->document = p_document;
	state->document_dirty = true;
	state->needs_another_frame = true;
}

void HTMLSurfaceHCSRNewestBackend::set_transparent_background(bool p_transparent_background) {
	MutexLock lock(state->mutex);
	state->background.a = p_transparent_background ? 0.0f : 1.0f;
}

void HTMLSurfaceHCSRNewestBackend::set_background_color(const Color &p_background_color) {
	MutexLock lock(state->mutex);
	state->background = p_background_color;
}

void HTMLSurfaceHCSRNewestBackend::set_placeholder_background(const Color &p_color) {
	MutexLock lock(state->mutex);
	state->placeholder = p_color;
}

Error HTMLSurfaceHCSRNewestBackend::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	MutexLock lock(state->mutex);
	if (r_needs_output != nullptr) *r_needs_output = false;
	if (r_needs_begin_frame != nullptr) *r_needs_begin_frame = state->needs_another_frame;
	if (state->terminal) return ERR_CANT_CREATE;
	if (state->render_pending) return OK;
	if (state->document_dirty) {
		const Error rebuild = _rebuild_scene();
		if (rebuild != OK) return rebuild;
	}
	if (state->scene == 0) return ERR_UNCONFIGURED;
	hcsr_step_desc_t step;
	initialize_abi(step);
	step.flags = HCSR_STEP_BUILD_PACKET;
	step.packet_format = HCSR_DRAW_PACKET_FORMAT_1;
	step.paint_mode = HCSR_PAINT_MODE_AREA_GRAYSCALE;
	step.time_seconds = p_timeline_time_seconds;
	step.viewport_width = state->logical_size.x;
	step.viewport_height = state->logical_size.y;
	step.device_scale = state->device_scale;
	hcsr_step_result_t result;
	initialize_abi(result);
	if (hcsr_scene_step(state->scene, &step, &result, &state->pending_packet) != HCSR_OK || state->pending_packet == 0) {
		set_terminal(state, scene_error(state, "hcsr_newest could not advance the scene."));
		return ERR_INVALID_DATA;
	}
	state->needs_another_frame = (result.flags & HCSR_STEP_RESULT_NEEDS_ANOTHER_STEP) != 0;
	state->metadata.timeline_time_seconds = p_timeline_time_seconds;
	state->metadata.host_frame_number++;
	state->queued_generation = result.scene_generation;
	state->render_pending = true;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRNewestBackend::_render_on_render_thread).bind((uint64_t)(uintptr_t)state));
	if (r_needs_begin_frame != nullptr) *r_needs_begin_frame = state->needs_another_frame;
	return OK;
}

void HTMLSurfaceHCSRNewestBackend::render_placeholder(const String &p_marker) {
	bool can_render = false;
	bool show_placeholder = false;
	Size2i placeholder_size;
	Color placeholder_color;
	{
		MutexLock lock(state->mutex);
		can_render = !state->closing && !state->terminal && state->document.is_valid() && state->document->is_source_valid();
		show_placeholder = state->active_generation == 0 && !state->render_pending;
		placeholder_size = state->physical_size;
		placeholder_color = state->placeholder;
	}
	if (show_placeholder) {
		texture->update_placeholder(placeholder_size, placeholder_color, p_marker);
	}
	if (can_render) {
		bool needs_output = false;
		bool needs_begin_frame = false;
		const double timeline_time_seconds = OS::get_singleton() != nullptr
				? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0
				: 0.0;
		update_compositor(timeline_time_seconds, &needs_output, &needs_begin_frame);
	}
}

bool HTMLSurfaceHCSRNewestBackend::poll_pending_output(bool *r_waiting_for_completion) {
	MutexLock lock(state->mutex);
	if (r_waiting_for_completion != nullptr) *r_waiting_for_completion = state->render_pending;
	const bool changed = state->presentation_changed;
	state->presentation_changed = false;
	return changed;
}

bool HTMLSurfaceHCSRNewestBackend::has_pending_output() const { MutexLock lock(state->mutex); return state->render_pending || state->presentation_changed; }
bool HTMLSurfaceHCSRNewestBackend::has_pending_frame_request() const { MutexLock lock(state->mutex); return state->needs_another_frame; }
uint64_t HTMLSurfaceHCSRNewestBackend::get_last_queued_frame_generation() const { MutexLock lock(state->mutex); return state->queued_generation; }
uint64_t HTMLSurfaceHCSRNewestBackend::get_active_frame_generation() const { MutexLock lock(state->mutex); return state->active_generation; }
bool HTMLSurfaceHCSRNewestBackend::has_terminal_render_failure() const { MutexLock lock(state->mutex); return state->terminal; }
String HTMLSurfaceHCSRNewestBackend::get_terminal_render_failure_reason() const { MutexLock lock(state->mutex); return state->terminal_reason; }
Error HTMLSurfaceHCSRNewestBackend::submit_cpu_frame(const HTMLCPUFrame &p_frame) { (void)p_frame; return ERR_UNAVAILABLE; }

Error HTMLSurfaceHCSRNewestBackend::_queue_input(const hcsr_input_event_t &p_event, const CharString &p_payload) {
	MutexLock lock(state->mutex);
	if (state->scene == 0) return ERR_UNCONFIGURED;
	const Error result = hcsr_scene_enqueue_input(state->scene, &p_event, 1, p_payload.get_data(), p_payload.length()) == HCSR_OK ? OK : ERR_INVALID_DATA;
	if (result == OK) state->needs_another_frame = true;
	return result;
}

Error HTMLSurfaceHCSRNewestBackend::mouse_move(const Point2 &p_position, int p_modifiers, bool &r_visual_state_changed) {
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_POINTER_MOVE; event.x = p_position.x; event.y = p_position.y; event.code = 1; event.modifiers = input_modifiers(p_modifiers);
	r_visual_state_changed = true;
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::mouse_down(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_button; (void)p_click_count;
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_POINTER_BUTTON; event.flags = HCSR_INPUT_PRESSED; event.x = p_position.x; event.y = p_position.y; event.code = 1; event.modifiers = input_modifiers(p_modifiers);
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::mouse_up(const Point2 &p_position, HTMLSurfaceMouseButton p_button, int p_modifiers, int p_click_count) {
	(void)p_button; (void)p_click_count;
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_POINTER_BUTTON; event.flags = HCSR_INPUT_RELEASED; event.x = p_position.x; event.y = p_position.y; event.code = 1; event.modifiers = input_modifiers(p_modifiers);
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::pointer_cancel(const Point2 &p_position, int p_pointer_id) {
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_POINTER_BUTTON; event.flags = HCSR_INPUT_CANCELED; event.x = p_position.x; event.y = p_position.y; event.code = p_pointer_id;
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::notify_pointer_leave(const Point2 &p_position, bool p_cancel_pressed_interaction, int p_pointer_id) {
	return p_cancel_pressed_interaction ? pointer_cancel(p_position, p_pointer_id) : OK;
}

Error HTMLSurfaceHCSRNewestBackend::wheel(const Point2 &p_position, const Vector2 &p_delta) {
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_WHEEL; event.x = p_position.x; event.y = p_position.y; event.delta_x = p_delta.x; event.delta_y = p_delta.y;
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::key_down(HTMLSurfaceInputKey p_key, int p_modifiers) {
	const uint32_t key = map_key(p_key);
	if (key == HCSR_KEY_NONE) return ERR_UNAVAILABLE;
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_KEY; event.code = key; event.flags = HCSR_INPUT_PRESSED; event.modifiers = input_modifiers(p_modifiers);
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::key_up(HTMLSurfaceInputKey p_key, int p_modifiers) {
	const uint32_t key = map_key(p_key);
	if (key == HCSR_KEY_NONE) return ERR_UNAVAILABLE;
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_KEY; event.code = key; event.flags = HCSR_INPUT_RELEASED; event.modifiers = input_modifiers(p_modifiers);
	return _queue_input(event);
}

Error HTMLSurfaceHCSRNewestBackend::text_input(const String &p_text) {
	const CharString payload = p_text.utf8();
	hcsr_input_event_t event;
	initialize_abi(event);
	event.kind = HCSR_INPUT_TEXT; event.payload_length = payload.length();
	return _queue_input(event, payload);
}

Error HTMLSurfaceHCSRNewestBackend::_apply_mutation(const hcsr_mutation_t &p_mutation) {
	MutexLock lock(state->mutex);
	if (state->scene == 0) return ERR_UNCONFIGURED;
	const Error result = hcsr_scene_apply_mutations(state->scene, &p_mutation, 1) == HCSR_OK ? OK : ERR_INVALID_DATA;
	if (result == OK) state->needs_another_frame = true;
	return result;
}

Error HTMLSurfaceHCSRNewestBackend::set_element_text(const StringName &p_id, const String &p_text) {
	const CharString id = String(p_id).utf8(); const CharString value = p_text.utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_SET_TEXT; mutation.element_id = utf8_view(id); mutation.value = utf8_view(value);
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	const CharString id = String(p_id).utf8(); const CharString name = String(p_name).utf8(); const CharString value = p_value.utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_SET_ATTRIBUTE; mutation.element_id = utf8_view(id); mutation.name = utf8_view(name); mutation.value = utf8_view(value);
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	const CharString id = String(p_id).utf8(); const CharString name = String(p_name).utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_REMOVE_ATTRIBUTE; mutation.element_id = utf8_view(id); mutation.name = utf8_view(name);
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::set_element_style(const StringName &p_id, const String &p_css_text) {
	const CharString id = String(p_id).utf8(); const CharString value = p_css_text.utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_SET_STYLE; mutation.element_id = utf8_view(id); mutation.value = utf8_view(value);
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::set_form_control_value(const StringName &p_id, const String &p_value) {
	const CharString id = String(p_id).utf8(); const CharString value = p_value.utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_SET_FORM_VALUE; mutation.element_id = utf8_view(id); mutation.value = utf8_view(value);
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::set_form_control_checked(const StringName &p_id, bool p_checked) {
	const CharString id = String(p_id).utf8();
	hcsr_mutation_t mutation; initialize_abi(mutation); mutation.kind = HCSR_MUTATION_SET_CHECKED; mutation.element_id = utf8_view(id); mutation.integer_value = p_checked ? 1 : 0;
	return _apply_mutation(mutation);
}

Error HTMLSurfaceHCSRNewestBackend::apply_element_mutations(const Array &p_mutations) {
	if (p_mutations.is_empty()) return OK;
	Vector<CharString> ids;
	Vector<CharString> names;
	Vector<CharString> values;
	Vector<hcsr_mutation_t> mutations;
	ids.resize(p_mutations.size());
	names.resize(p_mutations.size());
	values.resize(p_mutations.size());
	mutations.resize(p_mutations.size());
	for (int index = 0; index < p_mutations.size(); index++) {
		const Variant &entry = p_mutations[index];
		if (entry.get_type() != Variant::DICTIONARY) return ERR_INVALID_PARAMETER;
		const Dictionary value = entry;
		const String operation = value.get("operation", String());
		ids.write[index] = String(value.get("id", StringName())).utf8();
		names.write[index] = String(value.get("name", StringName())).utf8();
		values.write[index] = String(value.get("value", String())).utf8();
		hcsr_mutation_t &mutation = mutations.write[index];
		initialize_abi(mutation);
		mutation.element_id = utf8_view(ids[index]);
		mutation.name = utf8_view(names[index]);
		mutation.value = utf8_view(values[index]);
		if (operation == "set_text") mutation.kind = HCSR_MUTATION_SET_TEXT;
		else if (operation == "set_attribute") mutation.kind = HCSR_MUTATION_SET_ATTRIBUTE;
		else if (operation == "remove_attribute") mutation.kind = HCSR_MUTATION_REMOVE_ATTRIBUTE;
		else if (operation == "set_style") mutation.kind = HCSR_MUTATION_SET_STYLE;
		else if (operation == "set_value") mutation.kind = HCSR_MUTATION_SET_FORM_VALUE;
		else if (operation == "set_checked") {
			mutation.kind = HCSR_MUTATION_SET_CHECKED;
			mutation.integer_value = value.get("value", false) ? 1 : 0;
		} else return ERR_UNAVAILABLE;
	}
	MutexLock lock(state->mutex);
	if (state->scene == 0) return ERR_UNCONFIGURED;
	const Error result = hcsr_scene_apply_mutations(state->scene, mutations.ptr(), mutations.size()) == HCSR_OK ? OK : ERR_INVALID_DATA;
	if (result == OK) state->needs_another_frame = true;
	return result;
}

bool HTMLSurfaceHCSRNewestBackend::hit_test(const Point2 &p_position, HTMLElementHit &r_hit) const {
	MutexLock lock(state->mutex);
	if (state->scene == 0) return false;
	hcsr_hit_t hit; initialize_abi(hit);
	if (hcsr_scene_hit_test(state->scene, p_position.x, p_position.y, &hit) != HCSR_OK || hit.object_id == 0) return false;
	Vector<char> id; id.resize(MAX((size_t)1, hit.element_id_bytes)); size_t required = 0;
	if (hcsr_scene_copy_element_id(state->scene, hit.object_id, id.ptrw(), id.size(), &required) != HCSR_OK) return false;
	r_hit = HTMLElementHit();
	r_hit.element_id = StringName(String::utf8(id.ptr()));
	r_hit.bounds = Rect2i(Math::floor(hit.bounds.x), Math::floor(hit.bounds.y), Math::ceil(hit.bounds.width), Math::ceil(hit.bounds.height));
	return true;
}

void HTMLSurfaceHCSRNewestBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const { MutexLock lock(state->mutex); r_metadata = state->metadata; }
Ref<Texture2D> HTMLSurfaceHCSRNewestBackend::get_texture() const { return texture; }
Ref<HTMLTexture2D> HTMLSurfaceHCSRNewestBackend::get_html_texture() const { return texture; }

HTMLSurfaceHCSRNewestBackend::HTMLSurfaceHCSRNewestBackend(HTMLSurfaceHCSRNewestRenderer p_renderer) {
	texture.instantiate();
	state = memnew(State);
	state->renderer = p_renderer;
	state->texture = texture;
	if (hcsr_scene_abi_version() != HCSR_SCENE_ABI_VERSION_1 || hcsr_runtime_create(&state->runtime) != HCSR_OK) {
		set_terminal(state, "hcsr_newest scene ABI initialization failed.");
	}
}

HTMLSurfaceHCSRNewestBackend::~HTMLSurfaceHCSRNewestBackend() {
	if (state == nullptr) return;
	{
		MutexLock lock(state->mutex);
		state->closing = true;
	}
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr) {
		if (!server->is_on_render_thread()) server->sync();
		server->call_on_render_thread(callable_mp_static(&HTMLSurfaceHCSRNewestBackend::_cancel_gpu_submission_on_render_thread).bind((uint64_t)(uintptr_t)state));
		if (!server->is_on_render_thread()) {
			server->sync();
		}
	}
	RenderingDevice *device = server != nullptr ? server->get_rendering_device() : nullptr;
	{
		MutexLock lock(state->mutex);
		if (state->pending_packet != 0) hcsr_draw_packet_destroy(state->pending_packet);
		if (state->presenter != 0) {
			if (state->renderer == HTML_SURFACE_HCSR_NEWEST_D3D12) hcsr_d3d12_destroy(state->presenter);
			else if (state->renderer == HTML_SURFACE_HCSR_NEWEST_VULKAN) hcsr_vulkan_destroy(state->presenter);
		}
		release_gpu_target(state, server, device);
		if (state->scene != 0) hcsr_scene_destroy(state->scene);
		if (state->source != 0) hcsr_source_destroy(state->source);
		if (state->runtime != 0) hcsr_runtime_destroy(state->runtime);
		state->texture.unref();
	}
	memdelete(state);
	state = nullptr;
	document.unref();
	texture.unref();
}
