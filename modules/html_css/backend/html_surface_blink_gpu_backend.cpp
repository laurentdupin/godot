/**************************************************************************/
/*  html_surface_blink_gpu_backend.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "html_surface_blink_gpu_backend.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/godot_vulkan.h"
#endif

#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
#include "drivers/d3d12/godot_d3dx12.h"
#include <wrl/client.h>
#endif

static bool html_css_gpu_trace_enabled() {
	return OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_TRACE") == "1";
}

static void html_css_gpu_trace(const String &p_message) {
	if (html_css_gpu_trace_enabled()) {
		print_line(vformat("HTML/CSS GPU trace: %s", p_message));
	}
}

static double html_css_gpu_elapsed_ms(uint64_t p_start_usec) {
	if (OS::get_singleton() == nullptr || p_start_usec == 0) {
		return 0.0;
	}
	return (double)(OS::get_singleton()->get_ticks_usec() - p_start_usec) / 1000.0;
}

static String html_css_gpu_ptr_string(const void *p_ptr) {
	return "0x" + String::num_uint64((uint64_t)p_ptr, 16);
}

static void blink_standalone_backdrop_filter_op_to_html(const blink_standalone_backdrop_filter_op_t &p_op, HTMLBackdropFilterOperation &r_operation) {
	r_operation.type = (HTMLBackdropFilterOperationType)p_op.type;
	r_operation.amount = p_op.amount;
}

static void blink_standalone_gpu_backdrop_effect_to_html(const blink_standalone_gpu_backdrop_effect_t &p_effect, HTMLGPUBackdropEffect &r_effect) {
	r_effect = HTMLGPUBackdropEffect();
	r_effect.id = p_effect.id;
	r_effect.generation = p_effect.generation;
	r_effect.bounds = Rect2(p_effect.bounds.x, p_effect.bounds.y, p_effect.bounds.width, p_effect.bounds.height);
	r_effect.blur_radius_css_px = p_effect.blur_radius_css_px;
	r_effect.border_radius_top_left = p_effect.border_radius_top_left;
	r_effect.border_radius_top_right = p_effect.border_radius_top_right;
	r_effect.border_radius_bottom_right = p_effect.border_radius_bottom_right;
	r_effect.border_radius_bottom_left = p_effect.border_radius_bottom_left;
	r_effect.opacity = p_effect.opacity;
	r_effect.flags = p_effect.flags;
	for (uint32_t i = 0; i < p_effect.filter_op_count && i < BLINK_STANDALONE_MAX_BACKDROP_FILTER_OPS; i++) {
		HTMLBackdropFilterOperation operation;
		blink_standalone_backdrop_filter_op_to_html(p_effect.filter_ops[i], operation);
		r_effect.filter_operations.push_back(operation);
	}
}

String HTMLSurfaceBlinkGPUBackend::_get_backend_name() const {
	switch (requested_backend) {
		case BLINK_STANDALONE_GPU_BACKEND_VULKAN:
			return "Vulkan";
		case BLINK_STANDALONE_GPU_BACKEND_D3D12:
			return "D3D12";
		default:
			return "GPU";
	}
}

String HTMLSurfaceBlinkGPUBackend::_get_unsupported_message() const {
	switch (requested_backend) {
		case BLINK_STANDALONE_GPU_BACKEND_VULKAN:
			return "The Vulkan GPU backend is not available from this Godot rendering driver or external renderer build.";
		case BLINK_STANDALONE_GPU_BACKEND_D3D12:
			return "The D3D12 GPU backend is not available from this Godot rendering driver or external renderer build.";
		default:
			return "No supported Godot rendering-driver GPU target is available.";
	}
}

Size2i HTMLSurfaceBlinkGPUBackend::_get_physical_size() const {
	return Size2i(
			MAX(1, (int)Math::ceil(size.x * device_scale_factor)),
			MAX(1, (int)Math::ceil(size.y * device_scale_factor)));
}

blink_standalone_gpu_backend_t HTMLSurfaceBlinkGPUBackend::_choose_backend() const {
	if (renderer == nullptr) {
		html_css_gpu_trace("choose_backend: renderer is null");
		return BLINK_STANDALONE_GPU_BACKEND_NONE;
	}
	if (requested_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN || requested_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
		const bool supported = _backend_is_supported(requested_backend);
		html_css_gpu_trace(vformat("choose_backend: explicit=%d supported=%s", (int)requested_backend, supported ? "true" : "false"));
		return supported ? requested_backend : BLINK_STANDALONE_GPU_BACKEND_NONE;
	}

	const String rendering_driver = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name().to_lower() : String();
	if (rendering_driver == "d3d12") {
#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
		if (_backend_is_supported(BLINK_STANDALONE_GPU_BACKEND_D3D12)) {
			return BLINK_STANDALONE_GPU_BACKEND_D3D12;
		}
#endif
	} else if (rendering_driver == "vulkan") {
#if defined(VULKAN_ENABLED)
		if (_backend_is_supported(BLINK_STANDALONE_GPU_BACKEND_VULKAN)) {
			return BLINK_STANDALONE_GPU_BACKEND_VULKAN;
		}
#endif
	}
	html_css_gpu_trace(vformat("choose_backend: no GPU backend matches Godot rendering driver '%s'", rendering_driver));
	return BLINK_STANDALONE_GPU_BACKEND_NONE;
}

bool HTMLSurfaceBlinkGPUBackend::_godot_driver_supports_backend(blink_standalone_gpu_backend_t p_backend) const {
	const String rendering_driver = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name().to_lower() : String();
	switch (p_backend) {
		case BLINK_STANDALONE_GPU_BACKEND_VULKAN:
			return rendering_driver == "vulkan";
		case BLINK_STANDALONE_GPU_BACKEND_D3D12:
			return rendering_driver == "d3d12";
		default:
			return false;
	}
}

bool HTMLSurfaceBlinkGPUBackend::_backend_is_supported(blink_standalone_gpu_backend_t p_backend) const {
	if (renderer == nullptr) {
		return false;
	}
	if (!_godot_driver_supports_backend(p_backend)) {
		html_css_gpu_trace(vformat("backend_is_supported: backend=%d incompatible with Godot rendering driver '%s'", (int)p_backend, OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name() : String()));
		return false;
	}
	if (p_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN && !vulkan_device_configured) {
		return true;
	}
	if (p_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12 && !d3d12_device_configured) {
		return true;
	}
	if (p_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12 && target_ready && d3d12_shared_handle != nullptr) {
		return true;
	}
	const uint32_t capabilities = blink_standalone_renderer_gpu_backend_capabilities(renderer, p_backend);
	html_css_gpu_trace(vformat("backend_is_supported: backend=%d capabilities=0x%s vulkan_configured=%s d3d12_configured=%s", (int)p_backend, String::num_uint64(capabilities, 16), vulkan_device_configured ? "true" : "false", d3d12_device_configured ? "true" : "false"));
	if ((capabilities & BLINK_STANDALONE_GPU_CAPABILITY_AVAILABLE) == 0) {
		return false;
	}
	return (capabilities & BLINK_STANDALONE_GPU_CAPABILITY_EXTERNAL_TARGET) != 0;
}

void HTMLSurfaceBlinkGPUBackend::_report_error_once(const String &p_error) {
	terminal_render_failure = true;
	terminal_render_failure_reason = p_error;
	if (error_reported) {
		return;
	}
	error_reported = true;
	ERR_PRINT(vformat("HTML/CSS %s GPU backend failed: %s Explicit GPU requests do not fall back to CPU output.", _get_backend_name(), p_error));
}

void HTMLSurfaceBlinkGPUBackend::_clear_gpu_output() {
	if (!_cancel_async_request()) {
		html_css_gpu_trace("clear_gpu_output: deferred while dedicated command is still pending");
		return;
	}
	pending_output = false;
	frame_metadata = HTMLFrameMetadata();
	_clear_gpu_backdrop_frame();
	_destroy_target();
	clear_to_background();
}

void HTMLSurfaceBlinkGPUBackend::_clear_gpu_backdrop_frame() {
	gpu_backdrop_frame.clear();
}

bool HTMLSurfaceBlinkGPUBackend::_ensure_target(blink_standalone_gpu_backend_t p_backend) {
	const Size2i physical_size = _get_physical_size();
	const uint64_t start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	html_css_gpu_trace(vformat("ensure_target: backend=%d logical_size=%dx%d dsf=%.3f physical_size=%dx%d target_ready=%s active=%d native_size=%dx%d generation=%d pending_output=%s", (int)p_backend, size.x, size.y, device_scale_factor, physical_size.x, physical_size.y, target_ready ? "true" : "false", (int)active_backend, native_target_size.x, native_target_size.y, (int64_t)generation, pending_output ? "true" : "false"));
	if (target_ready && active_backend == p_backend && native_target_size == physical_size) {
		html_css_gpu_trace(vformat("ensure_target: reusing existing target elapsed_ms=%.3f", html_css_gpu_elapsed_ms(start_usec)));
		return true;
	}

	if (!_destroy_target()) {
		html_css_gpu_trace("ensure_target: previous target destruction deferred");
		return false;
	}
	html_css_gpu_trace("ensure_target: previous target destroyed");
	last_native_error = String();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		last_native_error = "RenderingServer is not available.";
		return false;
	}

	if (rs->is_on_render_thread()) {
		html_css_gpu_trace("ensure_target: already on render thread");
		_ensure_target_on_render_thread((int)p_backend);
	} else {
		html_css_gpu_trace("ensure_target: dispatching to render thread");
		rs->call_on_render_thread(callable_mp_static(&HTMLSurfaceBlinkGPUBackend::_ensure_target_on_render_thread_callback).bind((uint64_t)this, (int)p_backend));
		rs->sync();
		html_css_gpu_trace(vformat("ensure_target: render thread sync complete target_ready=%s error='%s' elapsed_ms=%.3f", target_ready ? "true" : "false", last_native_error, html_css_gpu_elapsed_ms(start_usec)));
	}

	html_css_gpu_trace(vformat("ensure_target: complete target_ready=%s active=%d native_size=%dx%d elapsed_ms=%.3f", target_ready ? "true" : "false", (int)active_backend, native_target_size.x, native_target_size.y, html_css_gpu_elapsed_ms(start_usec)));
	return target_ready;
}

bool HTMLSurfaceBlinkGPUBackend::_ensure_texture_imported() {
	if (rs_texture_rid.is_valid()) {
		return true;
	}
	last_native_error = String();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		last_native_error = "RenderingServer is not available.";
		return false;
	}

	if (rs->is_on_render_thread()) {
		_ensure_texture_imported_on_render_thread();
	} else {
		rs->call_on_render_thread(callable_mp_static(&HTMLSurfaceBlinkGPUBackend::_ensure_texture_imported_on_render_thread_callback).bind((uint64_t)this));
		rs->sync();
	}

	return rs_texture_rid.is_valid();
}

bool HTMLSurfaceBlinkGPUBackend::_ensure_backdrop_mask_texture_imported() {
	if (backdrop_mask_rs_texture_rid.is_valid()) {
		return true;
	}
	if (!backdrop_mask_ready) {
		return false;
	}
	last_native_error = String();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		last_native_error = "RenderingServer is not available.";
		return false;
	}

	if (rs->is_on_render_thread()) {
		_ensure_backdrop_mask_texture_imported_on_render_thread();
	} else {
		rs->call_on_render_thread(callable_mp_static(&HTMLSurfaceBlinkGPUBackend::_ensure_backdrop_mask_texture_imported_on_render_thread_callback).bind((uint64_t)this));
		rs->sync();
	}

	return backdrop_mask_rs_texture_rid.is_valid();
}

void HTMLSurfaceBlinkGPUBackend::_detach_texture_import() {
	if (!rs_texture_rid.is_valid()) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);
	if (rs->is_on_render_thread()) {
		_detach_texture_import_on_render_thread();
	} else {
		rs->call_on_render_thread(callable_mp_static(&HTMLSurfaceBlinkGPUBackend::_detach_texture_import_on_render_thread_callback).bind((uint64_t)this));
		rs->sync();
	}
}

bool HTMLSurfaceBlinkGPUBackend::_destroy_target() {
	if (!_cancel_async_request()) {
		html_css_gpu_trace("destroy_target: deferred while dedicated command is still pending");
		return false;
	}
	if (!target_ready && !rs_texture_rid.is_valid() && vk_image == nullptr && d3d12_resource == nullptr) {
		return true;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return false;
	}

	if (rs->is_on_render_thread()) {
		_destroy_target_on_render_thread();
	} else {
		rs->call_on_render_thread(callable_mp_static(&HTMLSurfaceBlinkGPUBackend::_destroy_target_on_render_thread_callback).bind((uint64_t)this));
		rs->sync();
	}
	return !target_ready && !rs_texture_rid.is_valid() && vk_image == nullptr && d3d12_resource == nullptr;
}

#if defined(VULKAN_ENABLED)
static bool find_vulkan_memory_type(VkPhysicalDevice p_physical_device, uint32_t p_type_bits, VkMemoryPropertyFlags p_required_flags, uint32_t &r_memory_type_index) {
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	vkGetPhysicalDeviceMemoryProperties(p_physical_device, &memory_properties);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
		if ((p_type_bits & (1u << i)) == 0) {
			continue;
		}
		if ((memory_properties.memoryTypes[i].propertyFlags & p_required_flags) == p_required_flags) {
			r_memory_type_index = i;
			return true;
		}
	}
	return false;
}
#endif

void HTMLSurfaceBlinkGPUBackend::_ensure_target_on_render_thread(int p_backend) {
	html_css_gpu_trace(vformat("ensure_target_rt: begin backend=%d", p_backend));
	target_ready = false;
	active_backend = BLINK_STANDALONE_GPU_BACKEND_NONE;
	native_target_size = Size2i();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		last_native_error = "RenderingDevice is not available. Use the CPU backend with headless or compatibility renderers.";
		html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
		return;
	}

	const Size2i physical_size = _get_physical_size();
	if (gpu_texture.is_null()) {
		gpu_texture.instantiate();
	}

	if (p_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
#if defined(VULKAN_ENABLED)
		VkInstance instance = (VkInstance)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT);
		VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);
		VkDevice device = (VkDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
		VkQueue queue = (VkQueue)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
		const uint32_t queue_family_index = (uint32_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY);
		if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
			last_native_error = "Godot Vulkan driver did not expose the required instance, physical device, logical device, or queue handle.";
			html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace(vformat("ensure_target_rt: Vulkan handles instance=%s physical=%s device=%s queue=%s family=%d", html_css_gpu_ptr_string(instance), html_css_gpu_ptr_string(physical_device), html_css_gpu_ptr_string(device), html_css_gpu_ptr_string(queue), queue_family_index));

		const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
		const VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkImageCreateInfo image_create_info = {};
		image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_create_info.imageType = VK_IMAGE_TYPE_2D;
		image_create_info.format = format;
		image_create_info.extent.width = (uint32_t)physical_size.x;
		image_create_info.extent.height = (uint32_t)physical_size.y;
		image_create_info.extent.depth = 1;
		image_create_info.mipLevels = 1;
		image_create_info.arrayLayers = 1;
		image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_create_info.usage = image_usage;
		image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkImage image = VK_NULL_HANDLE;
		VkResult result = vkCreateImage(device, &image_create_info, nullptr, &image);
		if (result != VK_SUCCESS) {
			last_native_error = vformat("vkCreateImage failed with VkResult %d.", (int)result);
			html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace(vformat("ensure_target_rt: vkCreateImage image=%s", html_css_gpu_ptr_string(image)));

		VkMemoryRequirements memory_requirements = {};
		vkGetImageMemoryRequirements(device, image, &memory_requirements);
		html_css_gpu_trace(vformat("ensure_target_rt: memory requirements size=%d type_bits=0x%s", (uint64_t)memory_requirements.size, String::num_uint64(memory_requirements.memoryTypeBits, 16)));
		uint32_t memory_type_index = 0;
		if (!find_vulkan_memory_type(physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type_index)) {
			vkDestroyImage(device, image, nullptr);
			last_native_error = "No Vulkan device-local memory type is compatible with the interop image.";
			html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
			return;
		}

		VkMemoryAllocateInfo allocate_info = {};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = memory_requirements.size;
		allocate_info.memoryTypeIndex = memory_type_index;

		VkDeviceMemory memory = VK_NULL_HANDLE;
		result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
		if (result != VK_SUCCESS) {
			vkDestroyImage(device, image, nullptr);
			last_native_error = vformat("vkAllocateMemory failed with VkResult %d.", (int)result);
			html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace(vformat("ensure_target_rt: vkAllocateMemory memory=%s type=%d", html_css_gpu_ptr_string(memory), memory_type_index));

		result = vkBindImageMemory(device, image, memory, 0);
		if (result != VK_SUCCESS) {
			vkDestroyImage(device, image, nullptr);
			vkFreeMemory(device, memory, nullptr);
			last_native_error = vformat("vkBindImageMemory failed with VkResult %d.", (int)result);
			html_css_gpu_trace(vformat("ensure_target_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace("ensure_target_rt: vkBindImageMemory OK");

		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(physical_device, &properties);
		const uint32_t api_version = VK_MAKE_API_VERSION(
				VK_API_VERSION_VARIANT(properties.apiVersion),
				VK_API_VERSION_MAJOR(properties.apiVersion),
				VK_API_VERSION_MINOR(properties.apiVersion),
				0);

		vk_instance = instance;
		vk_physical_device = physical_device;
		vk_device = device;
		vk_queue = queue;
		vk_image = image;
		vk_device_memory = memory;
		vk_queue_family_index = queue_family_index;
		vk_api_version = api_version;
		vk_allocation_size = memory_requirements.size;
		vk_memory_type_index = memory_type_index;
		vk_format = format;
		vk_image_tiling = VK_IMAGE_TILING_OPTIMAL;
		vk_image_usage_flags = image_usage;
		vk_sample_count = VK_SAMPLE_COUNT_1_BIT;
		vk_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		vk_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		active_backend = BLINK_STANDALONE_GPU_BACKEND_VULKAN;
		native_target_size = physical_size;
		target_ready = true;
		VkImage mask_image = VK_NULL_HANDLE;
		VkDeviceMemory mask_memory = VK_NULL_HANDLE;
		result = vkCreateImage(device, &image_create_info, nullptr, &mask_image);
		if (result == VK_SUCCESS) {
			VkMemoryRequirements mask_memory_requirements = {};
			vkGetImageMemoryRequirements(device, mask_image, &mask_memory_requirements);
			uint32_t mask_memory_type_index = 0;
			if (find_vulkan_memory_type(physical_device, mask_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mask_memory_type_index)) {
				VkMemoryAllocateInfo mask_allocate_info = {};
				mask_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				mask_allocate_info.allocationSize = mask_memory_requirements.size;
				mask_allocate_info.memoryTypeIndex = mask_memory_type_index;
				result = vkAllocateMemory(device, &mask_allocate_info, nullptr, &mask_memory);
				if (result == VK_SUCCESS) {
					result = vkBindImageMemory(device, mask_image, mask_memory, 0);
				}
				if (result == VK_SUCCESS) {
					vk_backdrop_mask_image = mask_image;
					vk_backdrop_mask_device_memory = mask_memory;
					vk_backdrop_mask_allocation_size = mask_memory_requirements.size;
					vk_backdrop_mask_memory_type_index = mask_memory_type_index;
					vk_backdrop_mask_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
					vk_backdrop_mask_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					backdrop_mask_ready = true;
				}
			}
		}
		if (!backdrop_mask_ready) {
			if (mask_image != VK_NULL_HANDLE) {
				vkDestroyImage(device, mask_image, nullptr);
			}
			if (mask_memory != VK_NULL_HANDLE) {
				vkFreeMemory(device, mask_memory, nullptr);
			}
			html_css_gpu_trace("ensure_target_rt: Vulkan backdrop mask target unavailable; GPU backdrop blur will be disabled");
		}
		html_css_gpu_trace(vformat("ensure_target_rt: Vulkan target ready api_version=0x%x allocation_size=%d memory_type=%d", vk_api_version, vk_allocation_size, vk_memory_type_index));
		return;
#else
		last_native_error = "This Godot build was not compiled with Vulkan driver support.";
		return;
#endif
	}

	if (p_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
		ID3D12Device *device = (ID3D12Device *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
		ID3D12CommandQueue *command_queue = (ID3D12CommandQueue *)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
		if (device == nullptr || command_queue == nullptr) {
			last_native_error = "Godot D3D12 driver did not expose the required device or command queue handle.";
			return;
		}

		const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D12_HEAP_PROPERTIES heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(
				format,
				(uint64_t)physical_size.x,
				(uint32_t)physical_size.y,
				1,
				1,
				1,
				0,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		html_css_gpu_trace(vformat("ensure_target_rt: D3D12 desc width=%d height=%d mip_levels=%d sample_count=%d flags=0x%s heap_flags=0x%s initial_state=0x%s", (uint64_t)resource_desc.Width, (uint32_t)resource_desc.Height, (uint32_t)resource_desc.MipLevels, (uint32_t)resource_desc.SampleDesc.Count, String::num_uint64(resource_desc.Flags, 16), String::num_uint64(D3D12_HEAP_FLAG_SHARED, 16), String::num_uint64(D3D12_RESOURCE_STATE_COMMON, 16)));
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		HRESULT hr = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_SHARED,
				&resource_desc,
				D3D12_RESOURCE_STATE_COMMON,
				nullptr,
				IID_PPV_ARGS(&resource));
		if (FAILED(hr)) {
			last_native_error = vformat("ID3D12Device::CreateCommittedResource failed with HRESULT 0x%08x.", (uint32_t)hr);
			return;
		}
		html_css_gpu_trace(vformat("ensure_target_rt: D3D12 resource=%s", html_css_gpu_ptr_string(resource.Get())));

		HANDLE shared_handle = nullptr;
		hr = device->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle);
		if (FAILED(hr) || shared_handle == nullptr) {
			last_native_error = vformat("ID3D12Device::CreateSharedHandle failed with HRESULT 0x%08x.", (uint32_t)hr);
			return;
		}
		html_css_gpu_trace(vformat("ensure_target_rt: D3D12 shared_handle=%s", html_css_gpu_ptr_string(shared_handle)));

		resource->AddRef();

		d3d12_device = device;
		d3d12_command_queue = command_queue;
		d3d12_resource = resource.Get();
		d3d12_shared_handle = shared_handle;
		d3d12_format = format;
		d3d12_current_state = D3D12_RESOURCE_STATE_COMMON;
		d3d12_final_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		active_backend = BLINK_STANDALONE_GPU_BACKEND_D3D12;
		native_target_size = physical_size;
		target_ready = true;
		Microsoft::WRL::ComPtr<ID3D12Resource> mask_resource;
		HANDLE mask_shared_handle = nullptr;
		hr = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_SHARED,
				&resource_desc,
				D3D12_RESOURCE_STATE_COMMON,
				nullptr,
				IID_PPV_ARGS(&mask_resource));
		if (SUCCEEDED(hr)) {
			hr = device->CreateSharedHandle(mask_resource.Get(), nullptr, GENERIC_ALL, nullptr, &mask_shared_handle);
		}
		if (SUCCEEDED(hr) && mask_resource.Get() != nullptr && mask_shared_handle != nullptr) {
			mask_resource->AddRef();
			d3d12_backdrop_mask_resource = mask_resource.Get();
			d3d12_backdrop_mask_shared_handle = mask_shared_handle;
			d3d12_backdrop_mask_current_state = D3D12_RESOURCE_STATE_COMMON;
			d3d12_backdrop_mask_final_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			backdrop_mask_ready = true;
			html_css_gpu_trace(vformat("ensure_target_rt: D3D12 backdrop mask resource=%s shared_handle=%s", html_css_gpu_ptr_string(d3d12_backdrop_mask_resource), html_css_gpu_ptr_string(d3d12_backdrop_mask_shared_handle)));
		} else {
			if (mask_shared_handle != nullptr) {
				CloseHandle(mask_shared_handle);
			}
			html_css_gpu_trace(vformat("ensure_target_rt: D3D12 backdrop mask target unavailable hr=0x%08x; GPU backdrop blur will be disabled", (uint32_t)hr));
		}
		html_css_gpu_trace(vformat("ensure_target_rt: D3D12 target ready current_state=0x%s final_state=0x%s", String::num_uint64(d3d12_current_state, 16), String::num_uint64(d3d12_final_state, 16)));
		return;
#else
		last_native_error = "This Godot build was not compiled with D3D12 driver support.";
		return;
#endif
	}

	last_native_error = "No explicit GPU backend was selected.";
}

void HTMLSurfaceBlinkGPUBackend::_ensure_texture_imported_on_render_thread() {
	if (rs_texture_rid.is_valid()) {
		return;
	}
	if (!target_ready) {
		last_native_error = "Cannot import GPU texture before the native target is ready.";
		html_css_gpu_trace(vformat("ensure_texture_imported_rt: %s", last_native_error));
		return;
	}

	if (gpu_texture.is_null()) {
		gpu_texture.instantiate();
	}

	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
		if (d3d12_resource == nullptr) {
			last_native_error = "Cannot import D3D12 texture without a resource.";
			html_css_gpu_trace(vformat("ensure_texture_imported_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace(vformat("ensure_texture_imported_rt: D3D12 import resource=%s size=%dx%d", html_css_gpu_ptr_string(d3d12_resource), native_target_size.x, native_target_size.y));
		rs_texture_rid = RenderingServer::get_singleton()->texture_create_from_native_handle(
				RenderingServerEnums::TEXTURE_TYPE_2D,
				Image::FORMAT_RGBA8,
				(uint64_t)d3d12_resource,
				native_target_size.x,
				native_target_size.y,
				1,
				1);
		if (!rs_texture_rid.is_valid()) {
			last_native_error = "RenderingServer::texture_create_from_native_handle failed for the rendered D3D12 resource.";
			html_css_gpu_trace(vformat("ensure_texture_imported_rt: %s", last_native_error));
			return;
		}
		gpu_texture->set_external_texture(rs_texture_rid, native_target_size, true);
		html_css_gpu_trace("ensure_texture_imported_rt: D3D12 import OK");
#else
		last_native_error = "This Godot build was not compiled with D3D12 driver support.";
#endif
	}

	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
#if defined(VULKAN_ENABLED)
		if (vk_image == nullptr) {
			last_native_error = "Cannot import Vulkan texture without an image.";
			html_css_gpu_trace(vformat("ensure_texture_imported_rt: %s", last_native_error));
			return;
		}
		html_css_gpu_trace(vformat("ensure_texture_imported_rt: Vulkan import image=%s size=%dx%d", html_css_gpu_ptr_string(vk_image), native_target_size.x, native_target_size.y));
		rs_texture_rid = RenderingServer::get_singleton()->texture_create_from_native_handle(
				RenderingServerEnums::TEXTURE_TYPE_2D,
				Image::FORMAT_RGBA8,
				(uint64_t)vk_image,
				native_target_size.x,
				native_target_size.y,
				1,
				1);
		if (!rs_texture_rid.is_valid()) {
			last_native_error = "RenderingServer::texture_create_from_native_handle failed for the rendered Vulkan image.";
			html_css_gpu_trace(vformat("ensure_texture_imported_rt: %s", last_native_error));
			return;
		}
		gpu_texture->set_external_texture(rs_texture_rid, native_target_size, true);
		html_css_gpu_trace("ensure_texture_imported_rt: Vulkan import OK");
#else
		last_native_error = "This Godot build was not compiled with Vulkan driver support.";
#endif
	}
}

void HTMLSurfaceBlinkGPUBackend::_ensure_backdrop_mask_texture_imported_on_render_thread() {
	if (backdrop_mask_rs_texture_rid.is_valid()) {
		return;
	}
	if (!backdrop_mask_ready) {
		last_native_error = "Cannot import GPU backdrop mask before the native mask target is ready.";
		return;
	}

	if (gpu_backdrop_mask_texture.is_null()) {
		gpu_backdrop_mask_texture.instantiate();
	}

	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
		if (d3d12_backdrop_mask_resource == nullptr) {
			last_native_error = "Cannot import D3D12 backdrop mask texture without a resource.";
			return;
		}
		backdrop_mask_rs_texture_rid = RenderingServer::get_singleton()->texture_create_from_native_handle(
				RenderingServerEnums::TEXTURE_TYPE_2D,
				Image::FORMAT_RGBA8,
				(uint64_t)d3d12_backdrop_mask_resource,
				native_target_size.x,
				native_target_size.y,
				1,
				1);
		if (!backdrop_mask_rs_texture_rid.is_valid()) {
			last_native_error = "RenderingServer::texture_create_from_native_handle failed for the D3D12 backdrop mask resource.";
			return;
		}
		gpu_backdrop_mask_texture->set_external_texture(backdrop_mask_rs_texture_rid, native_target_size, true);
#else
		last_native_error = "This Godot build was not compiled with D3D12 driver support.";
#endif
	}

	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
#if defined(VULKAN_ENABLED)
		if (vk_backdrop_mask_image == nullptr) {
			last_native_error = "Cannot import Vulkan backdrop mask texture without an image.";
			return;
		}
		backdrop_mask_rs_texture_rid = RenderingServer::get_singleton()->texture_create_from_native_handle(
				RenderingServerEnums::TEXTURE_TYPE_2D,
				Image::FORMAT_RGBA8,
				(uint64_t)vk_backdrop_mask_image,
				native_target_size.x,
				native_target_size.y,
				1,
				1);
		if (!backdrop_mask_rs_texture_rid.is_valid()) {
			last_native_error = "RenderingServer::texture_create_from_native_handle failed for the Vulkan backdrop mask image.";
			return;
		}
		gpu_backdrop_mask_texture->set_external_texture(backdrop_mask_rs_texture_rid, native_target_size, true);
#else
		last_native_error = "This Godot build was not compiled with Vulkan driver support.";
#endif
	}
}

void HTMLSurfaceBlinkGPUBackend::_detach_texture_import_on_render_thread() {
	html_css_gpu_trace(vformat("detach_texture_import_rt: rid_valid=%s", rs_texture_rid.is_valid() ? "true" : "false"));
	if (gpu_texture.is_valid()) {
		gpu_texture->clear_external_texture();
	}
	if (rs_texture_rid.is_valid()) {
		RenderingServer::get_singleton()->free_rid(rs_texture_rid);
	}
	rs_texture_rid = RID();
	if (gpu_backdrop_mask_texture.is_valid()) {
		gpu_backdrop_mask_texture->clear_external_texture();
	}
	if (backdrop_mask_rs_texture_rid.is_valid()) {
		RenderingServer::get_singleton()->free_rid(backdrop_mask_rs_texture_rid);
	}
	backdrop_mask_rs_texture_rid = RID();
}

void HTMLSurfaceBlinkGPUBackend::_destroy_target_on_render_thread() {
	html_css_gpu_trace(vformat("destroy_target_rt: begin rid_valid=%s vk_image=%s vk_memory=%s d3d12_resource=%s", rs_texture_rid.is_valid() ? "true" : "false", html_css_gpu_ptr_string(vk_image), html_css_gpu_ptr_string(vk_device_memory), html_css_gpu_ptr_string(d3d12_resource)));
	_detach_texture_import_on_render_thread();

#if defined(VULKAN_ENABLED)
	if (vk_device != nullptr) {
		VkDevice device = (VkDevice)vk_device;
		const VkResult wait_result = vkDeviceWaitIdle(device);
		if (wait_result != VK_SUCCESS) {
			html_css_gpu_trace(vformat("destroy_target_rt: vkDeviceWaitIdle failed result=%d", (int)wait_result));
		}
		if (vk_image != nullptr) {
			vkDestroyImage(device, (VkImage)vk_image, nullptr);
		}
		if (vk_device_memory != nullptr) {
			vkFreeMemory(device, (VkDeviceMemory)vk_device_memory, nullptr);
		}
		if (vk_backdrop_mask_image != nullptr) {
			vkDestroyImage(device, (VkImage)vk_backdrop_mask_image, nullptr);
		}
		if (vk_backdrop_mask_device_memory != nullptr) {
			vkFreeMemory(device, (VkDeviceMemory)vk_backdrop_mask_device_memory, nullptr);
		}
	}
#endif
	vk_instance = nullptr;
	vk_physical_device = nullptr;
	vk_device = nullptr;
	vk_queue = nullptr;
	vk_image = nullptr;
	vk_device_memory = nullptr;
	vk_backdrop_mask_image = nullptr;
	vk_backdrop_mask_device_memory = nullptr;
	vk_backdrop_mask_allocation_size = 0;
	vk_backdrop_mask_memory_type_index = 0;
	vk_backdrop_mask_current_layout = 0;
	vk_backdrop_mask_final_layout = 0;

#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
	if (d3d12_shared_handle != nullptr) {
		CloseHandle((HANDLE)d3d12_shared_handle);
	}
	if (d3d12_resource != nullptr) {
		((ID3D12Resource *)d3d12_resource)->Release();
	}
	if (d3d12_backdrop_mask_shared_handle != nullptr) {
		CloseHandle((HANDLE)d3d12_backdrop_mask_shared_handle);
	}
	if (d3d12_backdrop_mask_resource != nullptr) {
		((ID3D12Resource *)d3d12_backdrop_mask_resource)->Release();
	}
#endif
	d3d12_device = nullptr;
	d3d12_command_queue = nullptr;
	d3d12_resource = nullptr;
	d3d12_shared_handle = nullptr;
	d3d12_backdrop_mask_resource = nullptr;
	d3d12_backdrop_mask_shared_handle = nullptr;
	d3d12_backdrop_mask_current_state = 0;
	d3d12_backdrop_mask_final_state = 0;

	active_backend = BLINK_STANDALONE_GPU_BACKEND_NONE;
	native_target_size = Size2i();
	target_ready = false;
	backdrop_mask_ready = false;
	_clear_gpu_backdrop_frame();
	html_css_gpu_trace("destroy_target_rt: complete");
}

void HTMLSurfaceBlinkGPUBackend::_ensure_target_on_render_thread_callback(uint64_t p_backend_ptr, int p_backend) {
	HTMLSurfaceBlinkGPUBackend *backend = reinterpret_cast<HTMLSurfaceBlinkGPUBackend *>(p_backend_ptr);
	if (backend != nullptr) {
		backend->_ensure_target_on_render_thread(p_backend);
	}
}

void HTMLSurfaceBlinkGPUBackend::_ensure_texture_imported_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceBlinkGPUBackend *backend = reinterpret_cast<HTMLSurfaceBlinkGPUBackend *>(p_backend_ptr);
	if (backend != nullptr) {
		backend->_ensure_texture_imported_on_render_thread();
	}
}

void HTMLSurfaceBlinkGPUBackend::_ensure_backdrop_mask_texture_imported_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceBlinkGPUBackend *backend = reinterpret_cast<HTMLSurfaceBlinkGPUBackend *>(p_backend_ptr);
	if (backend != nullptr) {
		backend->_ensure_backdrop_mask_texture_imported_on_render_thread();
	}
}

void HTMLSurfaceBlinkGPUBackend::_detach_texture_import_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceBlinkGPUBackend *backend = reinterpret_cast<HTMLSurfaceBlinkGPUBackend *>(p_backend_ptr);
	if (backend != nullptr) {
		backend->_detach_texture_import_on_render_thread();
	}
}

void HTMLSurfaceBlinkGPUBackend::_destroy_target_on_render_thread_callback(uint64_t p_backend_ptr) {
	HTMLSurfaceBlinkGPUBackend *backend = reinterpret_cast<HTMLSurfaceBlinkGPUBackend *>(p_backend_ptr);
	if (backend != nullptr) {
		backend->_destroy_target_on_render_thread();
	}
}

bool HTMLSurfaceBlinkGPUBackend::_configure_vulkan_device() {
	if (active_backend != BLINK_STANDALONE_GPU_BACKEND_VULKAN || vulkan_device_configured) {
		html_css_gpu_trace(vformat("configure_vulkan_device: skipped active=%d configured=%s", (int)active_backend, vulkan_device_configured ? "true" : "false"));
		return true;
	}
	if (renderer == nullptr || vk_instance == nullptr || vk_physical_device == nullptr || vk_device == nullptr || vk_queue == nullptr) {
		last_native_error = "Vulkan renderer/device handles are not ready for configure_vulkan_external_device.";
		html_css_gpu_trace(vformat("configure_vulkan_device: %s", last_native_error));
		return false;
	}

	blink_standalone_vulkan_external_device_t device = {};
	device.vk_instance = vk_instance;
	device.vk_physical_device = vk_physical_device;
	device.vk_device = vk_device;
	device.vk_queue = vk_queue;
	device.queue_family_index = vk_queue_family_index;
	device.api_version = vk_api_version;

	html_css_gpu_trace(vformat("configure_vulkan_device: enter api_version=0x%s", String::num_uint64(vk_api_version, 16)));
	const blink_standalone_status_code_t status = blink_standalone_renderer_configure_vulkan_external_device(renderer, &device);
	if (status != BLINK_STANDALONE_STATUS_OK) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		last_native_error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("configure_vulkan_external_device failed with status %d.", (int)status);
		html_css_gpu_trace(vformat("configure_vulkan_device: failed status=%d error='%s'", (int)status, last_native_error));
		return false;
	}

	vulkan_device_configured = true;
	html_css_gpu_trace("configure_vulkan_device: OK");
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::_configure_d3d12_device() {
	if (active_backend != BLINK_STANDALONE_GPU_BACKEND_D3D12 || d3d12_device_configured) {
		html_css_gpu_trace(vformat("configure_d3d12_device: skipped active=%d configured=%s", (int)active_backend, d3d12_device_configured ? "true" : "false"));
		return true;
	}
	if (renderer == nullptr || d3d12_device == nullptr) {
		last_native_error = "D3D12 renderer/device handles are not ready for configure_d3d12_external_device.";
		html_css_gpu_trace(vformat("configure_d3d12_device: %s", last_native_error));
		return false;
	}

#if defined(WINDOWS_ENABLED) && defined(D3D12_ENABLED)
	blink_standalone_d3d12_external_device_t device = {};
	device.d3d12_device = d3d12_device;
	device.d3d12_command_queue = d3d12_command_queue;

	html_css_gpu_trace(vformat("configure_d3d12_device: enter device=%s queue=%s", html_css_gpu_ptr_string(d3d12_device), html_css_gpu_ptr_string(d3d12_command_queue)));
	const blink_standalone_status_code_t status = blink_standalone_renderer_configure_d3d12_external_device(renderer, &device);
	if (status != BLINK_STANDALONE_STATUS_OK) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		last_native_error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("configure_d3d12_external_device failed with status %d.", (int)status);
		html_css_gpu_trace(vformat("configure_d3d12_device: failed status=%d error='%s'", (int)status, last_native_error));
		if (status == BLINK_STANDALONE_STATUS_UNSUPPORTED && d3d12_shared_handle != nullptr) {
			d3d12_device_configured = true;
			html_css_gpu_trace("configure_d3d12_device: unsupported external-device adoption; continuing with shared-handle target path");
			return true;
		}
		return false;
	}

	d3d12_device_configured = true;
	html_css_gpu_trace("configure_d3d12_device: OK");
	return true;
#else
	last_native_error = "This Godot build was not compiled with D3D12 driver support.";
	html_css_gpu_trace(vformat("configure_d3d12_device: %s", last_native_error));
	return false;
#endif
}

void HTMLSurfaceBlinkGPUBackend::_fill_common_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.common.backend = active_backend;
	r_target.common.logical_width = size.x;
	r_target.common.logical_height = size.y;
	r_target.common.physical_width = native_target_size.x;
	r_target.common.physical_height = native_target_size.y;
	r_target.common.device_scale_factor = device_scale_factor;
	r_target.common.pixel_format = BLINK_STANDALONE_PIXEL_FORMAT_RGBA8;
	r_target.common.alpha_mode = BLINK_STANDALONE_ALPHA_MODE_PREMULTIPLIED;
	r_target.common.color_space = BLINK_STANDALONE_COLOR_SPACE_SRGB;
	r_target.common.flags = BLINK_STANDALONE_GPU_TARGET_SKIP_IF_CLEAN;
	r_target.common.generation = generation;
}

void HTMLSurfaceBlinkGPUBackend::_fill_vulkan_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.vulkan.vk_image = vk_image;
	r_target.vulkan.vk_device = vk_device;
	r_target.vulkan.vk_physical_device = vk_physical_device;
	r_target.vulkan.vk_device_memory = vk_device_memory;
	r_target.vulkan.vk_format = vk_format;
	r_target.vulkan.width = native_target_size.x;
	r_target.vulkan.height = native_target_size.y;
	r_target.vulkan.current_layout = vk_current_layout;
	r_target.vulkan.required_final_layout = vk_final_layout;
	r_target.vulkan.queue_family_index = vk_queue_family_index;
	r_target.vulkan.allocation_offset = 0;
	r_target.vulkan.allocation_size = vk_allocation_size;
	r_target.vulkan.memory_type_index = vk_memory_type_index;
	r_target.vulkan.image_tiling = vk_image_tiling;
	r_target.vulkan.image_usage_flags = vk_image_usage_flags;
	r_target.vulkan.sample_count = vk_sample_count;
	r_target.vulkan.level_count = 1;
	r_target.vulkan.sharing_mode = 0;
	r_target.vulkan.external_memory_handle_type = 0;
}

void HTMLSurfaceBlinkGPUBackend::_fill_d3d12_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.d3d12.d3d12_device = d3d12_device;
	r_target.d3d12.d3d12_command_queue = d3d12_command_queue;
	r_target.d3d12.d3d12_resource = d3d12_resource;
	r_target.d3d12.shared_handle = d3d12_shared_handle;
	r_target.d3d12.dxgi_format = d3d12_format;
	r_target.d3d12.width = native_target_size.x;
	r_target.d3d12.height = native_target_size.y;
	r_target.d3d12.current_state = d3d12_current_state;
	r_target.d3d12.required_final_state = d3d12_final_state;
}

void HTMLSurfaceBlinkGPUBackend::_fill_common_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.common.backend = active_backend;
	r_target.common.logical_width = size.x;
	r_target.common.logical_height = size.y;
	r_target.common.physical_width = native_target_size.x;
	r_target.common.physical_height = native_target_size.y;
	r_target.common.device_scale_factor = device_scale_factor;
	r_target.common.pixel_format = BLINK_STANDALONE_PIXEL_FORMAT_RGBA8;
	r_target.common.alpha_mode = BLINK_STANDALONE_ALPHA_MODE_PREMULTIPLIED;
	r_target.common.color_space = BLINK_STANDALONE_COLOR_SPACE_SRGB;
	r_target.common.flags = 0;
	r_target.common.generation = backdrop_mask_generation;
}

void HTMLSurfaceBlinkGPUBackend::_fill_vulkan_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.vulkan.vk_image = vk_backdrop_mask_image;
	r_target.vulkan.vk_device = vk_device;
	r_target.vulkan.vk_physical_device = vk_physical_device;
	r_target.vulkan.vk_device_memory = vk_backdrop_mask_device_memory;
	r_target.vulkan.vk_format = vk_format;
	r_target.vulkan.width = native_target_size.x;
	r_target.vulkan.height = native_target_size.y;
	r_target.vulkan.current_layout = vk_backdrop_mask_current_layout;
	r_target.vulkan.required_final_layout = vk_backdrop_mask_final_layout;
	r_target.vulkan.queue_family_index = vk_queue_family_index;
	r_target.vulkan.allocation_offset = 0;
	r_target.vulkan.allocation_size = vk_backdrop_mask_allocation_size;
	r_target.vulkan.memory_type_index = vk_backdrop_mask_memory_type_index;
	r_target.vulkan.image_tiling = vk_image_tiling;
	r_target.vulkan.image_usage_flags = vk_image_usage_flags;
	r_target.vulkan.sample_count = vk_sample_count;
	r_target.vulkan.level_count = 1;
	r_target.vulkan.sharing_mode = 0;
	r_target.vulkan.external_memory_handle_type = 0;
}

void HTMLSurfaceBlinkGPUBackend::_fill_d3d12_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const {
	r_target.d3d12.d3d12_device = d3d12_device;
	r_target.d3d12.d3d12_command_queue = d3d12_command_queue;
	r_target.d3d12.d3d12_resource = d3d12_backdrop_mask_resource;
	r_target.d3d12.shared_handle = d3d12_backdrop_mask_shared_handle;
	r_target.d3d12.dxgi_format = d3d12_format;
	r_target.d3d12.width = native_target_size.x;
	r_target.d3d12.height = native_target_size.y;
	r_target.d3d12.current_state = d3d12_backdrop_mask_current_state;
	r_target.d3d12.required_final_state = d3d12_backdrop_mask_final_state;
}

void HTMLSurfaceBlinkGPUBackend::_read_gpu_backdrop_effects(const blink_standalone_gpu_backdrop_render_result_t &p_result) {
	_clear_gpu_backdrop_frame();
	if (p_result.backdrop_mask_written == 0 || p_result.effect_count == 0 || p_result.mask_encoding != BLINK_STANDALONE_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE) {
		return;
	}
	if (!_ensure_backdrop_mask_texture_imported()) {
		html_css_gpu_trace(vformat("read_gpu_backdrop_effects: mask texture import failed error='%s'", last_native_error));
		return;
	}

	HTMLGPUBackdropFrame frame;
	frame.mask_texture = gpu_backdrop_mask_texture;
	frame.logical_size = size;
	frame.physical_size = native_target_size;
	frame.device_scale_factor = device_scale_factor;
	frame.backend = p_result.backend;
	frame.mask_encoding = HTML_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
	frame.max_effect_id = p_result.max_effect_id;
	frame.frame_generation = p_result.frame_generation;
	frame.main_target_generation = p_result.main_target_generation;
	frame.backdrop_mask_generation = p_result.backdrop_mask_generation;

	const size_t effect_count = blink_standalone_renderer_gpu_backdrop_effect_count(renderer);
	for (size_t i = 0; i < effect_count; i++) {
		blink_standalone_gpu_backdrop_effect_t effect = {};
		if (blink_standalone_renderer_get_gpu_backdrop_effect(renderer, i, &effect) != BLINK_STANDALONE_STATUS_OK) {
			continue;
		}
		HTMLGPUBackdropEffect html_effect;
		blink_standalone_gpu_backdrop_effect_to_html(effect, html_effect);
		if (html_effect.generation != frame.frame_generation || html_effect.id == 0 || html_effect.id > frame.max_effect_id) {
			continue;
		}
		frame.effects.push_back(html_effect);
	}

	if (frame.is_valid()) {
		gpu_backdrop_frame = frame;
		html_css_gpu_trace(vformat("read_gpu_backdrop_effects: effects=%d generation=%d mask_size=%dx%d", gpu_backdrop_frame.effects.size(), (int64_t)gpu_backdrop_frame.frame_generation, gpu_backdrop_frame.physical_size.x, gpu_backdrop_frame.physical_size.y));
	}
}

void HTMLSurfaceBlinkGPUBackend::_read_gpu_backdrop_effects(const blink_standalone_gpu_async_render_result_t &p_result) {
	_clear_gpu_backdrop_frame();
	if (p_result.backdrop_mask_written == 0 || p_result.effect_count == 0 || p_result.mask_encoding != BLINK_STANDALONE_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE) {
		return;
	}
	if (!_ensure_backdrop_mask_texture_imported()) {
		html_css_gpu_trace(vformat("read_gpu_backdrop_effects_async: mask texture import failed error='%s'", last_native_error));
		return;
	}

	HTMLGPUBackdropFrame frame;
	frame.mask_texture = gpu_backdrop_mask_texture;
	frame.logical_size = size;
	frame.physical_size = native_target_size;
	frame.device_scale_factor = device_scale_factor;
	frame.backend = p_result.backend;
	frame.mask_encoding = HTML_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
	frame.max_effect_id = p_result.max_effect_id;
	frame.frame_generation = p_result.frame_generation;
	frame.main_target_generation = p_result.main_target_generation;
	frame.backdrop_mask_generation = p_result.backdrop_mask_generation;

	const size_t effect_count = blink_standalone_renderer_gpu_backdrop_effect_count(renderer);
	for (size_t i = 0; i < effect_count; i++) {
		blink_standalone_gpu_backdrop_effect_t effect = {};
		if (blink_standalone_renderer_get_gpu_backdrop_effect(renderer, i, &effect) != BLINK_STANDALONE_STATUS_OK) {
			continue;
		}
		HTMLGPUBackdropEffect html_effect;
		blink_standalone_gpu_backdrop_effect_to_html(effect, html_effect);
		if (html_effect.generation != frame.frame_generation || html_effect.id == 0 || html_effect.id > frame.max_effect_id) {
			continue;
		}
		frame.effects.push_back(html_effect);
	}

	if (frame.is_valid()) {
		gpu_backdrop_frame = frame;
		html_css_gpu_trace(vformat("read_gpu_backdrop_effects_async: effects=%d generation=%d mask_size=%dx%d", gpu_backdrop_frame.effects.size(), (int64_t)gpu_backdrop_frame.frame_generation, gpu_backdrop_frame.physical_size.x, gpu_backdrop_frame.physical_size.y));
	}
}

bool HTMLSurfaceBlinkGPUBackend::_async_result_is_waiting(const blink_standalone_gpu_async_render_result_t &p_result) const {
	return p_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_SUBMITTED || p_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_PENDING;
}

static bool html_css_dedicated_thread_command_is_terminal(uint32_t p_state) {
	return p_state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_COMPLETED ||
			p_state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_FAILED ||
			p_state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_STALE ||
			p_state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_CANCELLED;
}

bool HTMLSurfaceBlinkGPUBackend::_cancel_async_request() {
	if (renderer != nullptr && dedicated_target_command_id != 0) {
		blink_standalone_dedicated_thread_gpu_frame_result_t result = {};
		const uint64_t command_to_cancel = dedicated_target_command_id;
		const bool command_was_in_flight = dedicated_command_in_flight && dedicated_command_id == command_to_cancel;
		const blink_standalone_status_code_t cancel_status = blink_standalone_renderer_cancel_dedicated_thread_gpu_frame(renderer, command_to_cancel, &result);
		html_css_gpu_trace(vformat("cancel_async_request: dedicated command_id=%d cancel_status=%d result_status=%d state=%d render_state=%d main_written=%d mask_written=%d",
				(int64_t)command_to_cancel,
				(int)cancel_status,
				(int)result.status,
				(int)result.state,
				(int)result.render_result.state,
				result.render_result.main_target_written,
				result.render_result.backdrop_mask_written));

		bool terminal = cancel_status == BLINK_STANDALONE_STATUS_OK && html_css_dedicated_thread_command_is_terminal(result.state);
		for (int i = 0; i < 120 && !terminal; i++) {
			blink_standalone_dedicated_thread_gpu_frame_result_t poll_result = {};
			const blink_standalone_status_code_t poll_status = blink_standalone_renderer_poll_dedicated_thread_gpu_frame(renderer, command_to_cancel, &poll_result);
			html_css_gpu_trace(vformat("cancel_async_request: dedicated drain command_id=%d poll_status=%d result_status=%d state=%d render_state=%d",
					(int64_t)command_to_cancel,
					(int)poll_status,
					(int)poll_result.status,
					(int)poll_result.state,
					(int)poll_result.render_result.state));
			terminal = (poll_status == BLINK_STANDALONE_STATUS_OK || poll_status == BLINK_STANDALONE_STATUS_PENDING) && html_css_dedicated_thread_command_is_terminal(poll_result.state);
			if (!terminal && OS::get_singleton() != nullptr) {
				OS::get_singleton()->delay_usec(1000);
			}
		}
		if (!terminal && command_was_in_flight) {
			html_css_gpu_trace(vformat("cancel_async_request: dedicated command still pending command_id=%d after cancel/drain", (int64_t)command_to_cancel));
			pending_output = true;
			return false;
		}
		dedicated_command_in_flight = false;
		dedicated_command_id = 0;
		dedicated_target_command_id = 0;
		pending_output = false;
	}

	if (!async_in_flight || renderer == nullptr || async_request_id == 0) {
		async_in_flight = false;
		async_request_id = 0;
		return true;
	}

	blink_standalone_gpu_async_render_result_t result = {};
	const blink_standalone_status_code_t status = blink_standalone_renderer_cancel_gpu_frame_async(renderer, async_request_id, &result);
	html_css_gpu_trace(vformat("cancel_async_request: request_id=%d status=%d state=%d result_status=%d", (int64_t)async_request_id, (int)status, (int)result.state, (int)result.status));
	async_in_flight = false;
	async_request_id = 0;
	pending_output = false;
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::_publish_async_result(const blink_standalone_gpu_async_render_result_t &p_result) {
	if (p_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_NO_DEMAND) {
		pending_output = false;
		async_in_flight = false;
		async_request_id = 0;
		return false;
	}
	if (p_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_CANCELLED || p_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_STALE) {
		pending_output = false;
		async_in_flight = false;
		async_request_id = 0;
		_clear_gpu_backdrop_frame();
		return false;
	}
	if (p_result.state != BLINK_STANDALONE_GPU_ASYNC_STATE_COMPLETED || p_result.main_target_written == 0) {
		return false;
	}
	if (p_result.backend != (uint32_t)async_backend ||
			p_result.request_generation != async_request_generation ||
			p_result.main_target_generation != async_main_target_generation ||
			(async_backdrop_requested && p_result.backdrop_mask_written != 0 && p_result.backdrop_mask_generation != async_backdrop_mask_generation) ||
			p_result.physical_width != (uint32_t)async_physical_size.x ||
			p_result.physical_height != (uint32_t)async_physical_size.y ||
			async_logical_size != size ||
			async_physical_size != native_target_size ||
			!Math::is_equal_approx(async_device_scale_factor, device_scale_factor)) {
		html_css_gpu_trace(vformat("publish_async_result: stale/mismatched result state=%d backend=%d/%d request_gen=%d/%d main_gen=%d/%d mask_gen=%d/%d size=%dx%d expected=%dx%d current=%dx%d",
				(int)p_result.state,
				(int)p_result.backend,
				(int)async_backend,
				(int64_t)p_result.request_generation,
				(int64_t)async_request_generation,
				(int64_t)p_result.main_target_generation,
				(int64_t)async_main_target_generation,
				(int64_t)p_result.backdrop_mask_generation,
				(int64_t)async_backdrop_mask_generation,
				(int)p_result.physical_width,
				(int)p_result.physical_height,
				async_physical_size.x,
				async_physical_size.y,
				native_target_size.x,
				native_target_size.y));
		pending_output = false;
		async_in_flight = false;
		async_request_id = 0;
		_clear_gpu_backdrop_frame();
		return false;
	}

	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
		vk_current_layout = vk_final_layout;
		if (p_result.backdrop_mask_written != 0) {
			vk_backdrop_mask_current_layout = vk_backdrop_mask_final_layout;
		}
	}
	if (active_backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
		d3d12_current_state = d3d12_final_state;
		if (p_result.backdrop_mask_written != 0) {
			d3d12_backdrop_mask_current_state = d3d12_backdrop_mask_final_state;
		}
	}

	if (!_ensure_texture_imported()) {
		_clear_gpu_output();
		_report_error_once(last_native_error.is_empty() ? "Could not import the completed GPU texture into Godot." : last_native_error);
		return true;
	}

	pending_output = false;
	async_in_flight = false;
	async_request_id = 0;
	generation = p_result.main_target_generation;
	_read_frame_metadata();
	if (async_backdrop_requested && p_result.backdrop_mask_written != 0) {
		_read_gpu_backdrop_effects(p_result);
	} else {
		_clear_gpu_backdrop_frame();
	}
	html_css_gpu_trace(vformat("publish_async_result: complete request_id=%d generation=%d main_written=%d mask_written=%d effects=%d", (int64_t)p_result.request_id, (int64_t)generation, p_result.main_target_written, p_result.backdrop_mask_written, p_result.effect_count));
	return true;
}

void HTMLSurfaceBlinkGPUBackend::_store_async_expectations(blink_standalone_gpu_backend_t p_backend, const Size2i &p_logical_size, const Size2i &p_physical_size, float p_device_scale_factor, uint64_t p_request_generation, uint64_t p_main_target_generation, uint64_t p_backdrop_mask_generation, bool p_backdrop_requested) {
	async_backdrop_requested = p_backdrop_requested;
	async_request_generation = p_request_generation;
	async_main_target_generation = p_main_target_generation;
	async_backdrop_mask_generation = p_backdrop_mask_generation;
	async_logical_size = p_logical_size;
	async_physical_size = p_physical_size;
	async_device_scale_factor = p_device_scale_factor;
	async_backend = p_backend;
}

bool HTMLSurfaceBlinkGPUBackend::_handle_dedicated_thread_result(const blink_standalone_dedicated_thread_gpu_frame_result_t &p_result) {
	if (p_result.state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_PENDING) {
		pending_output = true;
		dedicated_command_in_flight = p_result.command_id != 0;
		dedicated_command_id = p_result.command_id;
		dedicated_target_command_id = p_result.command_id;
		return false;
	}

	dedicated_command_in_flight = false;
	dedicated_command_id = 0;

	if (p_result.state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_COMPLETED) {
		dedicated_target_command_id = p_result.command_id;
		if (p_result.render_result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_NO_DEMAND || p_result.render_result.main_target_written == 0) {
			pending_output = false;
			html_css_gpu_trace(vformat("dedicated_thread: completed clean/no-write update_needs_output=%d source_state=%d render_state=%d", p_result.update_result.needs_output, (int)p_result.source_result.state, (int)p_result.render_result.state));
			return false;
		}
		async_in_flight = true;
		async_request_id = p_result.render_result.request_id;
		const bool published = _publish_async_result(p_result.render_result);
		html_css_gpu_trace(vformat("dedicated_thread: completed publish=%s command_id=%d render_request_id=%d elapsed_ms=%.3f update_ms=%.3f source_ms=%.3f submit_ms=%.3f poll_ms=%.3f",
				published ? "true" : "false",
				(int64_t)p_result.command_id,
				(int64_t)p_result.render_result.request_id,
				p_result.elapsed_ms,
				p_result.update_ms,
				p_result.source_tick_ms,
				p_result.submit_ms,
				p_result.poll_ms));
		return published;
	}

	if (p_result.state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_STALE) {
		pending_output = true;
		dedicated_target_command_id = 0;
		_clear_gpu_backdrop_frame();
		html_css_gpu_trace(vformat("dedicated_thread: stale command_id=%d", (int64_t)p_result.command_id));
		return false;
	}

	const char *last_error = p_result.error_message != nullptr && p_result.error_message[0] != '\0' ? p_result.error_message : (renderer != nullptr ? blink_standalone_renderer_last_error(renderer) : nullptr);
	const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("dedicated-thread GPU frame failed with status %d state %d.", (int)p_result.status, (int)p_result.state);
	html_css_gpu_trace(vformat("dedicated_thread: error='%s'", error));
	_clear_gpu_output();
	_report_error_once(error);
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::_poll_dedicated_thread_command(bool *r_waiting_for_completion) {
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = dedicated_command_in_flight;
	}
	if (!dedicated_command_in_flight || renderer == nullptr || dedicated_command_id == 0) {
		return false;
	}

	blink_standalone_dedicated_thread_gpu_frame_result_t result = {};
	const uint64_t poll_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const blink_standalone_status_code_t status = blink_standalone_renderer_poll_dedicated_thread_gpu_frame(renderer, dedicated_command_id, &result);
	html_css_gpu_trace(vformat("poll_dedicated_thread_gpu_frame: command_id=%d status=%d result_status=%d state=%d render_state=%d main_written=%d mask_written=%d effects=%d size=%dx%d elapsed_ms=%.3f command_elapsed_ms=%.3f update_ms=%.3f source_ms=%.3f submit_ms=%.3f poll_ms=%.3f",
			(int64_t)dedicated_command_id,
			(int)status,
			(int)result.status,
			(int)result.state,
			(int)result.render_result.state,
			result.render_result.main_target_written,
			result.render_result.backdrop_mask_written,
			result.render_result.effect_count,
			(int)result.render_result.physical_width,
			(int)result.render_result.physical_height,
			html_css_gpu_elapsed_ms(poll_start_usec),
			result.elapsed_ms,
			result.update_ms,
			result.source_tick_ms,
			result.submit_ms,
			result.poll_ms));

	if (status == BLINK_STANDALONE_STATUS_OK || status == BLINK_STANDALONE_STATUS_PENDING) {
		const bool changed = _handle_dedicated_thread_result(result);
		if (r_waiting_for_completion != nullptr) {
			*r_waiting_for_completion = dedicated_command_in_flight;
		}
		return changed;
	}

	const char *last_error = result.error_message != nullptr && result.error_message[0] != '\0' ? result.error_message : blink_standalone_renderer_last_error(renderer);
	const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("poll_dedicated_thread_gpu_frame failed with status %d state %d.", (int)status, (int)result.state);
	html_css_gpu_trace(vformat("poll_dedicated_thread_gpu_frame: error='%s'", error));
	dedicated_command_in_flight = false;
	dedicated_command_id = 0;
	_clear_gpu_output();
	_report_error_once(error);
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = false;
	}
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::_post_dedicated_thread_frame(blink_standalone_gpu_backend_t p_backend, const blink_standalone_external_gpu_target_t &p_target, bool p_backdrop_requested, const blink_standalone_external_gpu_target_t &p_backdrop_mask_target, uint64_t p_render_start_usec) {
	blink_standalone_dedicated_thread_gpu_frame_request_t request = {};
	request.timeline_time_seconds = OS::get_singleton() != nullptr ? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0 : 0.0;
	request.poll_interval_ms = 0;
	request.max_poll_iterations = 512;
	request.source_request.backend = p_backend;
	request.source_request.flags = BLINK_STANDALONE_GPU_ASYNC_SKIP_IF_CLEAN;
	request.source_request.request_generation = p_target.common.generation;
	request.source_request.timeline_time_seconds = request.timeline_time_seconds;
	request.source_request.logical_width = p_target.common.logical_width;
	request.source_request.logical_height = p_target.common.logical_height;
	request.source_request.physical_width = p_target.common.physical_width;
	request.source_request.physical_height = p_target.common.physical_height;
	request.source_request.device_scale_factor = p_target.common.device_scale_factor;
	request.render_request.backend = p_backend;
	request.render_request.flags = BLINK_STANDALONE_GPU_ASYNC_SKIP_IF_CLEAN;
	if (OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_ASYNC_BLOCK") == "1") {
		request.render_request.flags |= BLINK_STANDALONE_GPU_ASYNC_BLOCK_UNTIL_COMPLETE;
	}
	request.render_request.request_generation = p_target.common.generation;
	request.render_request.mask_encoding = BLINK_STANDALONE_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
	request.render_request.main_target = p_target;
	if (p_backdrop_requested) {
		request.render_request.flags |= BLINK_STANDALONE_GPU_ASYNC_BACKDROP_MASK_REQUIRED;
		request.render_request.backdrop_mask_target = p_backdrop_mask_target;
	}

	_store_async_expectations(p_backend, size, native_target_size, device_scale_factor, request.render_request.request_generation, request.render_request.main_target.common.generation, request.render_request.backdrop_mask_target.common.generation, p_backdrop_requested);

	blink_standalone_dedicated_thread_gpu_frame_result_t result = {};
	html_css_gpu_trace(vformat("render_placeholder: post_dedicated_thread_gpu_frame enter backdrop_filter=%s mask_ready=%s", p_backdrop_requested ? "true" : "false", backdrop_mask_ready ? "true" : "false"));
	const blink_standalone_status_code_t status = blink_standalone_renderer_post_dedicated_thread_gpu_frame(renderer, &request, &result);
	html_css_gpu_trace(vformat("render_placeholder: post_dedicated_thread_gpu_frame exit status=%d result_status=%d state=%d command_id=%d render_state=%d main_written=%d mask_written=%d effects=%d elapsed_ms=%.3f command_elapsed_ms=%.3f update_ms=%.3f source_ms=%.3f submit_ms=%.3f poll_ms=%.3f",
			(int)status,
			(int)result.status,
			(int)result.state,
			(int64_t)result.command_id,
			(int)result.render_result.state,
			result.render_result.main_target_written,
			result.render_result.backdrop_mask_written,
			result.render_result.effect_count,
			html_css_gpu_elapsed_ms(p_render_start_usec),
			result.elapsed_ms,
			result.update_ms,
			result.source_tick_ms,
			result.submit_ms,
			result.poll_ms));

	if (status == BLINK_STANDALONE_STATUS_OK || status == BLINK_STANDALONE_STATUS_PENDING) {
		if (result.state == BLINK_STANDALONE_DEDICATED_THREAD_COMMAND_PENDING && result.command_id != 0) {
			dedicated_command_in_flight = true;
			dedicated_command_id = result.command_id;
			dedicated_target_command_id = result.command_id;
			pending_output = true;
			html_css_gpu_trace(vformat("render_placeholder: dedicated command accepted command_id=%d generation=%d", (int64_t)dedicated_command_id, (int64_t)request.render_request.request_generation));
			return true;
		}
		_handle_dedicated_thread_result(result);
		return true;
	}

	const char *last_error = result.error_message != nullptr && result.error_message[0] != '\0' ? result.error_message : blink_standalone_renderer_last_error(renderer);
	const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("post_dedicated_thread_gpu_frame failed with status %d state %d.", (int)status, (int)result.state);
	html_css_gpu_trace(vformat("render_placeholder: post_dedicated_thread_gpu_frame error='%s'", error));
	_clear_gpu_output();
	_report_error_once(error);
	return true;
}

blink_standalone_status_code_t HTMLSurfaceBlinkGPUBackend::_create_renderer(const blink_standalone_renderer_config_t *p_config, blink_standalone_renderer_t **r_renderer) {
	if (OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_DEDICATED_THREAD") == "0") {
		dedicated_thread_renderer = false;
		return HTMLSurfaceExternalCApiBackend::_create_renderer(p_config, r_renderer);
	}
	blink_standalone_status_code_t status = blink_standalone_renderer_create_dedicated_thread(p_config, r_renderer);
	dedicated_thread_renderer = status == BLINK_STANDALONE_STATUS_OK && r_renderer != nullptr && *r_renderer != nullptr;
	if (dedicated_thread_renderer) {
		html_css_gpu_trace("create_renderer: dedicated thread renderer enabled");
		return status;
	}
	html_css_gpu_trace(vformat("create_renderer: dedicated thread renderer unavailable status=%d; using compatibility renderer", (int)status));
	return HTMLSurfaceExternalCApiBackend::_create_renderer(p_config, r_renderer);
}

bool HTMLSurfaceBlinkGPUBackend::_after_renderer_created() {
	error_reported = false;
	vulkan_device_configured = false;
	d3d12_device_configured = false;
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::_prepare_for_input() {
	if (dedicated_command_in_flight && renderer != nullptr && dedicated_command_id != 0) {
		bool waiting = true;
		for (int i = 0; i < 240 && waiting; i++) {
			_poll_dedicated_thread_command(&waiting);
			if (waiting && OS::get_singleton() != nullptr) {
				OS::get_singleton()->delay_usec(1000);
			}
		}
		if (dedicated_command_in_flight) {
			html_css_gpu_trace(vformat("prepare_for_input: dedicated command still pending command_id=%d; deferring forwarded API call", (int64_t)dedicated_command_id));
			return false;
		}
		if (terminal_render_failure) {
			return false;
		}
	}

	return HTMLSurfaceExternalCApiBackend::_prepare_for_input();
}

void HTMLSurfaceBlinkGPUBackend::set_size(const Size2i &p_size) {
	const Size2i old_physical_size = _get_physical_size();
	HTMLSurfaceExternalCApiBackend::set_size(p_size);
	const Size2i new_physical_size = _get_physical_size();
	html_css_gpu_trace(vformat("set_size: logical=%dx%d old_physical=%dx%d new_physical=%dx%d dsf=%.3f target_ready=%s native_size=%dx%d", size.x, size.y, old_physical_size.x, old_physical_size.y, new_physical_size.x, new_physical_size.y, device_scale_factor, target_ready ? "true" : "false", native_target_size.x, native_target_size.y));
	if (target_ready && old_physical_size != new_physical_size) {
		html_css_gpu_trace("set_size: physical size changed, destroying target");
		_destroy_target();
	}
}

void HTMLSurfaceBlinkGPUBackend::set_device_scale_factor(float p_device_scale_factor) {
	const Size2i old_physical_size = _get_physical_size();
	const float old_device_scale_factor = device_scale_factor;
	HTMLSurfaceExternalCApiBackend::set_device_scale_factor(p_device_scale_factor);
	const Size2i new_physical_size = _get_physical_size();
	html_css_gpu_trace(vformat("set_device_scale_factor: old_dsf=%.3f new_dsf=%.3f logical=%dx%d old_physical=%dx%d new_physical=%dx%d target_ready=%s native_size=%dx%d", old_device_scale_factor, device_scale_factor, size.x, size.y, old_physical_size.x, old_physical_size.y, new_physical_size.x, new_physical_size.y, target_ready ? "true" : "false", native_target_size.x, native_target_size.y));
	if (target_ready && old_physical_size != new_physical_size) {
		html_css_gpu_trace("set_device_scale_factor: physical size changed, destroying target");
		_destroy_target();
	}
}

void HTMLSurfaceBlinkGPUBackend::set_backdrop_filter_enabled(bool p_enabled) {
	if (backdrop_filter_enabled == p_enabled) {
		return;
	}
	backdrop_filter_enabled = p_enabled;
	_clear_gpu_backdrop_frame();
}

void HTMLSurfaceBlinkGPUBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	terminal_render_failure = false;
	terminal_render_failure_reason = String();
	const uint64_t render_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	html_css_gpu_trace(vformat("render_placeholder: begin requested=%d logical_size=%dx%d dsf=%.3f physical_size=%dx%d target_ready=%s active=%d native_size=%dx%d generation=%d pending_output=%s", (int)requested_backend, size.x, size.y, device_scale_factor, _get_physical_size().x, _get_physical_size().y, target_ready ? "true" : "false", (int)active_backend, native_target_size.x, native_target_size.y, (int64_t)generation, pending_output ? "true" : "false"));
	if (document.is_null()) {
		html_css_gpu_trace("render_placeholder: document null");
		_clear_gpu_output();
		return;
	}
	if (!_ensure_renderer()) {
		_clear_gpu_output();
		_report_error_once("Could not create the external HTML/CSS renderer.");
		return;
	}

	const blink_standalone_gpu_backend_t backend = _choose_backend();
	html_css_gpu_trace(vformat("render_placeholder: selected backend=%d", (int)backend));
	if (backend == BLINK_STANDALONE_GPU_BACKEND_NONE) {
		_clear_gpu_output();
		_report_error_once(_get_unsupported_message());
		return;
	}
	const uint64_t ensure_target_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	if (!_ensure_target(backend)) {
		if (pending_output && last_native_error.is_empty()) {
			html_css_gpu_trace("render_placeholder: target recreation deferred while a previous GPU command is releasing borrowed resources");
			return;
		}
		_clear_gpu_output();
		_report_error_once(last_native_error.is_empty() ? "Could not create or import the native GPU target." : last_native_error);
		return;
	}
	html_css_gpu_trace(vformat("render_placeholder: ensure_target OK elapsed_ms=%.3f native_size=%dx%d target_ready=%s active=%d", html_css_gpu_elapsed_ms(ensure_target_start_usec), native_target_size.x, native_target_size.y, target_ready ? "true" : "false", (int)active_backend));
	if (backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN && !_configure_vulkan_device()) {
		_clear_gpu_output();
		_report_error_once(last_native_error);
		return;
	}
	if (backend == BLINK_STANDALONE_GPU_BACKEND_D3D12 && !_configure_d3d12_device()) {
		_clear_gpu_output();
		_report_error_once(last_native_error);
		return;
	}
	if (!_backend_is_supported(backend)) {
		_clear_gpu_output();
		_report_error_once(_get_unsupported_message());
		return;
	}
	const uint64_t sync_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	if (!_sync_viewport() || !_sync_document()) {
		html_css_gpu_trace("render_placeholder: sync viewport/document failed");
		_clear_gpu_output();
		return;
	}
	html_css_gpu_trace(vformat("render_placeholder: sync viewport/document OK elapsed_ms=%.3f", html_css_gpu_elapsed_ms(sync_start_usec)));

	if (async_in_flight || dedicated_command_in_flight) {
		bool waiting_for_completion = false;
		const bool poll_changed = poll_pending_output(&waiting_for_completion);
		if (waiting_for_completion || poll_changed) {
			return;
		}
	}

	blink_standalone_external_gpu_target_t target = {};
	_fill_common_target(target);
	target.common.generation = generation + 1;
	if (backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
		_fill_vulkan_target(target);
	} else if (backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
		_fill_d3d12_target(target);
	} else {
		_clear_gpu_output();
		_report_error_once("Selected backend is not a GPU target backend.");
		return;
	}

	html_css_gpu_trace(vformat("render_placeholder: target_metadata backend=%d logical=%dx%d physical=%dx%d dsf=%.3f generation=%d format=%d vk_image=%s vk_memory=%s vk_layout=0x%s vk_usage=0x%s d3d12_resource=%s d3d12_handle=%s d3d12_state=0x%s", (int)target.common.backend, (int)target.common.logical_width, (int)target.common.logical_height, (int)target.common.physical_width, (int)target.common.physical_height, target.common.device_scale_factor, (int64_t)target.common.generation, (int)target.common.pixel_format, html_css_gpu_ptr_string(vk_image), html_css_gpu_ptr_string(vk_device_memory), String::num_uint64(vk_current_layout, 16), String::num_uint64(vk_image_usage_flags, 16), html_css_gpu_ptr_string(d3d12_resource), html_css_gpu_ptr_string(d3d12_shared_handle), String::num_uint64(d3d12_current_state, 16)));

	const bool use_dedicated_product_path = dedicated_thread_renderer && (OS::get_singleton() == nullptr || OS::get_singleton()->get_environment("HTML_CSS_GPU_COOPERATIVE_TICK") != "1");
	if (!use_dedicated_product_path) {
	blink_standalone_gpu_source_frame_tick_request_t source_tick_request = {};
	source_tick_request.backend = backend;
	source_tick_request.flags = BLINK_STANDALONE_GPU_ASYNC_SKIP_IF_CLEAN;
	source_tick_request.request_generation = target.common.generation;
	source_tick_request.timeline_time_seconds = OS::get_singleton() != nullptr ? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0 : 0.0;
	source_tick_request.logical_width = target.common.logical_width;
	source_tick_request.logical_height = target.common.logical_height;
	source_tick_request.physical_width = target.common.physical_width;
	source_tick_request.physical_height = target.common.physical_height;
	source_tick_request.device_scale_factor = target.common.device_scale_factor;

	blink_standalone_gpu_source_frame_tick_result_t source_tick_result = {};
	const uint64_t source_tick_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	html_css_gpu_trace("render_placeholder: tick_gpu_source_frame_async enter");
	const blink_standalone_status_code_t source_tick_status = blink_standalone_renderer_tick_gpu_source_frame_async(renderer, &source_tick_request, &source_tick_result);
	html_css_gpu_trace(vformat("render_placeholder: tick_gpu_source_frame_async exit status=%d result_status=%d state=%d needs_output=%d frame_advanced=%d skipped=%d full_damage=%d damage_rects=%d source_generation=%d size=%dx%d physical=%dx%d dsf=%.3f elapsed_ms=%.3f api_elapsed_ms=%.3f",
			(int)source_tick_status,
			(int)source_tick_result.status,
			(int)source_tick_result.state,
			source_tick_result.needs_output,
			source_tick_result.frame_advanced,
			source_tick_result.frame_skipped_due_to_no_demand,
			source_tick_result.full_frame_damage,
			source_tick_result.damage_rect_count,
			(int64_t)source_tick_result.source_frame_generation,
			(int)source_tick_result.logical_width,
			(int)source_tick_result.logical_height,
			(int)source_tick_result.physical_width,
			(int)source_tick_result.physical_height,
			source_tick_result.device_scale_factor,
			html_css_gpu_elapsed_ms(source_tick_start_usec),
			source_tick_result.elapsed_ms));
	if (source_tick_status == BLINK_STANDALONE_STATUS_OK && source_tick_result.state == BLINK_STANDALONE_GPU_SOURCE_FRAME_STATE_NO_DEMAND) {
		pending_output = false;
		html_css_gpu_trace("render_placeholder: source_tick clean_skip");
		return;
	}
	if ((source_tick_status == BLINK_STANDALONE_STATUS_OK || source_tick_status == BLINK_STANDALONE_STATUS_PENDING) &&
			source_tick_result.state == BLINK_STANDALONE_GPU_SOURCE_FRAME_STATE_PENDING) {
		pending_output = true;
		html_css_gpu_trace("render_placeholder: source_tick pending; keeping previous GPU output");
		return;
	}
	if (source_tick_status == BLINK_STANDALONE_STATUS_OK && source_tick_result.state == BLINK_STANDALONE_GPU_SOURCE_FRAME_STATE_STALE) {
		pending_output = true;
		_clear_gpu_backdrop_frame();
		html_css_gpu_trace("render_placeholder: source_tick stale; will retry");
		return;
	}
	if (source_tick_status != BLINK_STANDALONE_STATUS_OK || source_tick_result.state != BLINK_STANDALONE_GPU_SOURCE_FRAME_STATE_SOURCE_READY) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("tick_gpu_source_frame_async failed with status %d state %d.", (int)source_tick_status, (int)source_tick_result.state);
		html_css_gpu_trace(vformat("render_placeholder: tick_gpu_source_frame_async error='%s'", error));
		_clear_gpu_output();
		_report_error_once(error);
		return;
	}
	if (source_tick_result.backend != (uint32_t)backend ||
			source_tick_result.request_generation != target.common.generation ||
			source_tick_result.logical_width != target.common.logical_width ||
			source_tick_result.logical_height != target.common.logical_height ||
			source_tick_result.physical_width != target.common.physical_width ||
			source_tick_result.physical_height != target.common.physical_height ||
			!Math::is_equal_approx(source_tick_result.device_scale_factor, target.common.device_scale_factor)) {
		html_css_gpu_trace(vformat("render_placeholder: source_tick mismatch backend=%d/%d request_gen=%d/%d size=%dx%d/%dx%d physical=%dx%d/%dx%d dsf=%.3f/%.3f",
				(int)source_tick_result.backend,
				(int)backend,
				(int64_t)source_tick_result.request_generation,
				(int64_t)target.common.generation,
				(int)source_tick_result.logical_width,
				(int)source_tick_result.logical_height,
				(int)target.common.logical_width,
				(int)target.common.logical_height,
				(int)source_tick_result.physical_width,
				(int)source_tick_result.physical_height,
				(int)target.common.physical_width,
				(int)target.common.physical_height,
				source_tick_result.device_scale_factor,
				target.common.device_scale_factor));
		pending_output = true;
		_clear_gpu_backdrop_frame();
		return;
	}
	}

	const uint64_t gpu_render_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;

	blink_standalone_gpu_async_render_request_t async_request = {};
	async_request.backend = backend;
	async_request.flags = BLINK_STANDALONE_GPU_ASYNC_SKIP_IF_CLEAN;
	if (OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_ASYNC_BLOCK") == "1") {
		async_request.flags |= BLINK_STANDALONE_GPU_ASYNC_BLOCK_UNTIL_COMPLETE;
	}
	async_request.request_generation = target.common.generation;
	async_request.mask_encoding = BLINK_STANDALONE_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
	async_request.main_target = target;
	if (backdrop_filter_enabled && backdrop_mask_ready) {
		backdrop_mask_generation = target.common.generation + 1;
		_fill_common_backdrop_mask_target(async_request.backdrop_mask_target);
		if (backend == BLINK_STANDALONE_GPU_BACKEND_VULKAN) {
			_fill_vulkan_backdrop_mask_target(async_request.backdrop_mask_target);
		} else if (backend == BLINK_STANDALONE_GPU_BACKEND_D3D12) {
			_fill_d3d12_backdrop_mask_target(async_request.backdrop_mask_target);
		}
	}

	if (use_dedicated_product_path) {
		_post_dedicated_thread_frame(backend, target, backdrop_filter_enabled && backdrop_mask_ready, async_request.backdrop_mask_target, gpu_render_start_usec);
		return;
	}

	blink_standalone_gpu_async_render_result_t result = {};
	html_css_gpu_trace(vformat("render_placeholder: submit_gpu_frame_async enter backdrop_filter=%s mask_ready=%s", backdrop_filter_enabled ? "true" : "false", backdrop_mask_ready ? "true" : "false"));
	const blink_standalone_status_code_t status = blink_standalone_renderer_submit_gpu_frame_async(renderer, &async_request, &result);
	html_css_gpu_trace(vformat("render_placeholder: submit_gpu_frame_async exit status=%d state=%d request_id=%d main_written=%d mask_written=%d effects=%d needs_output=%d frame_advanced=%d skipped=%d full_damage=%d elapsed_ms=%.3f", (int)status, (int)result.state, (int64_t)result.request_id, result.main_target_written, result.backdrop_mask_written, result.effect_count, result.needs_output, result.frame_advanced, result.frame_skipped_due_to_no_demand, result.full_frame_damage, html_css_gpu_elapsed_ms(gpu_render_start_usec)));
	if ((status == BLINK_STANDALONE_STATUS_OK || status == BLINK_STANDALONE_STATUS_PENDING) && _async_result_is_waiting(result) && result.request_id != 0) {
		async_in_flight = true;
		pending_output = true;
		async_backdrop_requested = backdrop_filter_enabled && backdrop_mask_ready;
		async_request_id = result.request_id;
		async_request_generation = async_request.request_generation;
		async_main_target_generation = async_request.main_target.common.generation;
		async_backdrop_mask_generation = async_request.backdrop_mask_target.common.generation;
		async_logical_size = size;
		async_physical_size = native_target_size;
		async_device_scale_factor = device_scale_factor;
		async_backend = backend;
		html_css_gpu_trace(vformat("render_placeholder: async request accepted request_id=%d generation=%d", (int64_t)async_request_id, (int64_t)async_request_generation));
		return;
	}
	if ((status == BLINK_STANDALONE_STATUS_OK || status == BLINK_STANDALONE_STATUS_PENDING) && _async_result_is_waiting(result) && result.request_id == 0) {
		async_in_flight = false;
		async_request_id = 0;
		pending_output = true;
		html_css_gpu_trace("render_placeholder: async source frame pending without accepted request");
		return;
	}
	if (status == BLINK_STANDALONE_STATUS_OK && result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_NO_DEMAND) {
		pending_output = false;
		html_css_gpu_trace("render_placeholder: async clean_skip");
		return;
	}
	if (status == BLINK_STANDALONE_STATUS_OK && result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_COMPLETED) {
		async_in_flight = true;
		pending_output = true;
		async_backdrop_requested = backdrop_filter_enabled && backdrop_mask_ready;
		async_request_id = result.request_id;
		async_request_generation = async_request.request_generation;
		async_main_target_generation = async_request.main_target.common.generation;
		async_backdrop_mask_generation = async_request.backdrop_mask_target.common.generation;
		async_logical_size = size;
		async_physical_size = native_target_size;
		async_device_scale_factor = device_scale_factor;
		async_backend = backend;
		const bool published = _publish_async_result(result);
		html_css_gpu_trace(vformat("render_placeholder: async immediate complete published=%s elapsed_ms=%.3f generation=%d", published ? "true" : "false", html_css_gpu_elapsed_ms(render_start_usec), (int64_t)generation));
		return;
	}
	if (status != BLINK_STANDALONE_STATUS_OK) {
		const char *last_error = blink_standalone_renderer_last_error(renderer);
		const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("submit_gpu_frame_async failed with status %d state %d.", (int)status, (int)result.state);
		if (result.request_id == 0 && result.main_target_written == 0 &&
				(error.contains("GPU source frame is not ready") || error.contains("size does not match the external target"))) {
			async_in_flight = false;
			async_request_id = 0;
			pending_output = true;
			html_css_gpu_trace(vformat("render_placeholder: async startup/resize source pending; will retry error='%s'", error));
			return;
		}
		html_css_gpu_trace(vformat("render_placeholder: submit_gpu_frame_async error='%s'", error));
		_clear_gpu_output();
		_report_error_once(error);
		return;
	}
	html_css_gpu_trace(vformat("render_placeholder: async submit returned unhandled state=%d status=%d", (int)result.state, (int)status));
}

bool HTMLSurfaceBlinkGPUBackend::poll_pending_output(bool *r_waiting_for_completion) {
	if (dedicated_command_in_flight) {
		return _poll_dedicated_thread_command(r_waiting_for_completion);
	}
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = async_in_flight;
	}
	if (!async_in_flight || renderer == nullptr || async_request_id == 0) {
		return false;
	}

	blink_standalone_gpu_async_render_result_t result = {};
	const uint64_t poll_start_usec = html_css_gpu_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const blink_standalone_status_code_t status = blink_standalone_renderer_poll_gpu_frame_async(renderer, async_request_id, &result);
	html_css_gpu_trace(vformat("poll_pending_output: request_id=%d status=%d result_status=%d state=%d main_written=%d mask_written=%d effects=%d size=%dx%d elapsed_ms=%.3f",
			(int64_t)async_request_id,
			(int)status,
			(int)result.status,
			(int)result.state,
			result.main_target_written,
			result.backdrop_mask_written,
			result.effect_count,
			(int)result.physical_width,
			(int)result.physical_height,
			html_css_gpu_elapsed_ms(poll_start_usec)));

	if ((status == BLINK_STANDALONE_STATUS_OK || status == BLINK_STANDALONE_STATUS_PENDING) && _async_result_is_waiting(result)) {
		pending_output = true;
		if (r_waiting_for_completion != nullptr) {
			*r_waiting_for_completion = true;
		}
		return false;
	}

	if (status == BLINK_STANDALONE_STATUS_OK && result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_COMPLETED) {
		const bool published = _publish_async_result(result);
		if (r_waiting_for_completion != nullptr) {
			*r_waiting_for_completion = false;
		}
		return published;
	}

	if (status == BLINK_STANDALONE_STATUS_OK &&
			(result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_CANCELLED ||
					result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_STALE ||
					result.state == BLINK_STANDALONE_GPU_ASYNC_STATE_NO_DEMAND)) {
		_publish_async_result(result);
		if (r_waiting_for_completion != nullptr) {
			*r_waiting_for_completion = false;
		}
		return false;
	}

	const char *last_error = blink_standalone_renderer_last_error(renderer);
	const String error = last_error != nullptr && last_error[0] != '\0' ? String::utf8(last_error) : vformat("poll_gpu_frame_async failed with status %d state %d.", (int)status, (int)result.state);
	html_css_gpu_trace(vformat("poll_pending_output: error='%s'", error));
	_clear_gpu_output();
	_report_error_once(error);
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = false;
	}
	return true;
}

bool HTMLSurfaceBlinkGPUBackend::has_pending_output() const {
	return pending_output;
}

bool HTMLSurfaceBlinkGPUBackend::has_terminal_render_failure() const {
	return terminal_render_failure;
}

String HTMLSurfaceBlinkGPUBackend::get_terminal_render_failure_reason() const {
	return terminal_render_failure_reason;
}

void HTMLSurfaceBlinkGPUBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	HTMLSurfaceExternalCApiBackend::get_frame_metadata(r_metadata);

	// GPU backends need a mask/table path for backdrop-filter. Do not feed
	// CPU rectangle metadata into the GPU compositor path.
	r_metadata.backdrop_filter_regions.clear();
}

void HTMLSurfaceBlinkGPUBackend::get_gpu_backdrop_frame(HTMLGPUBackdropFrame &r_frame) const {
	if (!gpu_backdrop_frame.is_valid() ||
			gpu_backdrop_frame.backend != (uint32_t)active_backend ||
			gpu_backdrop_frame.logical_size != size ||
			gpu_backdrop_frame.physical_size != native_target_size ||
			gpu_backdrop_frame.main_target_generation != generation ||
			gpu_backdrop_frame.backdrop_mask_generation != backdrop_mask_generation ||
			!Math::is_equal_approx(gpu_backdrop_frame.device_scale_factor, device_scale_factor)) {
		r_frame.clear();
		return;
	}
	r_frame = gpu_backdrop_frame;
}

Ref<Texture2D> HTMLSurfaceBlinkGPUBackend::get_texture() const {
	if (target_ready && gpu_texture.is_valid()) {
		return gpu_texture;
	}
	return Ref<Texture2D>();
}

Ref<HTMLTexture2D> HTMLSurfaceBlinkGPUBackend::get_html_texture() const {
	if (target_ready && gpu_texture.is_valid()) {
		return gpu_texture;
	}
	return Ref<HTMLTexture2D>();
}

HTMLSurfaceBlinkGPUBackend::HTMLSurfaceBlinkGPUBackend(blink_standalone_gpu_backend_t p_requested_backend) {
	requested_backend = p_requested_backend;
}

HTMLSurfaceBlinkGPUBackend::~HTMLSurfaceBlinkGPUBackend() {
	_destroy_target();
}
