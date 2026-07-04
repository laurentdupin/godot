/**************************************************************************/
/*  html_surface_blink_gpu_backend.h                                      */
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

#pragma once

#include "html_surface_blink_c_api_backend.h"

#include "html_css_renderer/renderer_c_api.h"

class HTMLSurfaceBlinkGPUBackend : public HTMLSurfaceExternalCApiBackend {
	blink_standalone_gpu_backend_t requested_backend = BLINK_STANDALONE_GPU_BACKEND_NONE;
	blink_standalone_gpu_backend_t active_backend = BLINK_STANDALONE_GPU_BACKEND_NONE;
	Size2i native_target_size;
	Ref<HTMLTexture2D> gpu_texture;
	Ref<HTMLTexture2D> gpu_backdrop_mask_texture;
	RID rs_texture_rid;
	RID backdrop_mask_rs_texture_rid;
	HTMLGPUBackdropFrame gpu_backdrop_frame;
	String last_native_error;
	String terminal_render_failure_reason;
	uint64_t generation = 0;
	uint64_t backdrop_mask_generation = 0;
	uint64_t async_request_generation = 0;
	uint64_t async_request_id = 0;
	uint64_t async_main_target_generation = 0;
	uint64_t async_backdrop_mask_generation = 0;
	uint64_t dedicated_command_id = 0;
	uint64_t dedicated_target_command_id = 0;
	Size2i async_logical_size;
	Size2i async_physical_size;
	float async_device_scale_factor = 1.0f;
	blink_standalone_gpu_backend_t async_backend = BLINK_STANDALONE_GPU_BACKEND_NONE;
	bool error_reported = false;
	bool target_ready = false;
	bool backdrop_mask_ready = false;
	bool pending_output = false;
	bool async_in_flight = false;
	bool dedicated_thread_renderer = false;
	bool dedicated_command_in_flight = false;
	bool async_backdrop_requested = false;
	bool terminal_render_failure = false;
	bool vulkan_device_configured = false;
	bool d3d12_device_configured = false;
	bool backdrop_filter_enabled = false;

	void *vk_instance = nullptr;
	void *vk_physical_device = nullptr;
	void *vk_device = nullptr;
	void *vk_queue = nullptr;
	void *vk_image = nullptr;
	void *vk_device_memory = nullptr;
	uint32_t vk_queue_family_index = 0;
	uint32_t vk_api_version = 0;
	uint64_t vk_allocation_size = 0;
	uint32_t vk_memory_type_index = 0;
	uint32_t vk_format = 0;
	uint32_t vk_image_tiling = 0;
	uint32_t vk_image_usage_flags = 0;
	uint32_t vk_sample_count = 0;
	uint32_t vk_current_layout = 0;
	uint32_t vk_final_layout = 0;
	void *vk_backdrop_mask_image = nullptr;
	void *vk_backdrop_mask_device_memory = nullptr;
	uint64_t vk_backdrop_mask_allocation_size = 0;
	uint32_t vk_backdrop_mask_memory_type_index = 0;
	uint32_t vk_backdrop_mask_current_layout = 0;
	uint32_t vk_backdrop_mask_final_layout = 0;

	void *d3d12_device = nullptr;
	void *d3d12_command_queue = nullptr;
	void *d3d12_resource = nullptr;
	void *d3d12_shared_handle = nullptr;
	uint32_t d3d12_format = 0;
	uint32_t d3d12_current_state = 0;
	uint32_t d3d12_final_state = 0;
	void *d3d12_backdrop_mask_resource = nullptr;
	void *d3d12_backdrop_mask_shared_handle = nullptr;
	uint32_t d3d12_backdrop_mask_current_state = 0;
	uint32_t d3d12_backdrop_mask_final_state = 0;

	String _get_backend_name() const;
	String _get_unsupported_message() const;
	Size2i _get_physical_size() const;
	blink_standalone_gpu_backend_t _choose_backend() const;
	bool _godot_driver_supports_backend(blink_standalone_gpu_backend_t p_backend) const;
	bool _backend_is_supported(blink_standalone_gpu_backend_t p_backend) const;
	void _report_error_once(const String &p_error);
	void _clear_gpu_output();
	void _clear_gpu_backdrop_frame();
	bool _ensure_target(blink_standalone_gpu_backend_t p_backend);
	bool _ensure_texture_imported();
	bool _ensure_backdrop_mask_texture_imported();
	void _detach_texture_import();
	bool _destroy_target();
	void _ensure_texture_imported_on_render_thread();
	void _ensure_backdrop_mask_texture_imported_on_render_thread();
	void _ensure_target_on_render_thread(int p_backend);
	void _detach_texture_import_on_render_thread();
	void _destroy_target_on_render_thread();
	static void _ensure_texture_imported_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _ensure_backdrop_mask_texture_imported_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _ensure_target_on_render_thread_callback(uint64_t p_backend_ptr, int p_backend);
	static void _detach_texture_import_on_render_thread_callback(uint64_t p_backend_ptr);
	static void _destroy_target_on_render_thread_callback(uint64_t p_backend_ptr);
	bool _configure_vulkan_device();
	bool _configure_d3d12_device();
	void _fill_common_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _fill_vulkan_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _fill_d3d12_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _fill_common_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _fill_vulkan_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _fill_d3d12_backdrop_mask_target(blink_standalone_external_gpu_target_t &r_target) const;
	void _read_gpu_backdrop_effects(const blink_standalone_gpu_backdrop_render_result_t &p_result);
	void _read_gpu_backdrop_effects(const blink_standalone_gpu_async_render_result_t &p_result);
	bool _cancel_async_request();
	bool _async_result_is_waiting(const blink_standalone_gpu_async_render_result_t &p_result) const;
	bool _publish_async_result(const blink_standalone_gpu_async_render_result_t &p_result);
	void _store_async_expectations(blink_standalone_gpu_backend_t p_backend, const Size2i &p_logical_size, const Size2i &p_physical_size, float p_device_scale_factor, uint64_t p_request_generation, uint64_t p_main_target_generation, uint64_t p_backdrop_mask_generation, bool p_backdrop_requested);
	bool _post_dedicated_thread_frame(blink_standalone_gpu_backend_t p_backend, const blink_standalone_external_gpu_target_t &p_target, bool p_backdrop_requested, const blink_standalone_external_gpu_target_t &p_backdrop_mask_target, uint64_t p_render_start_usec);
	bool _handle_dedicated_thread_result(const blink_standalone_dedicated_thread_gpu_frame_result_t &p_result);
	bool _poll_dedicated_thread_command(bool *r_waiting_for_completion);

	virtual blink_standalone_status_code_t _create_renderer(const blink_standalone_renderer_config_t *p_config, blink_standalone_renderer_t **r_renderer) override;
	virtual bool _after_renderer_created() override;
	virtual bool _prepare_for_input() override;

public:
	virtual void set_size(const Size2i &p_size) override;
	virtual void set_device_scale_factor(float p_device_scale_factor) override;
	virtual void set_backdrop_filter_enabled(bool p_enabled) override;
	virtual void render_placeholder(const String &p_marker) override;
	virtual bool poll_pending_output(bool *r_waiting_for_completion = nullptr) override;
	virtual bool has_pending_output() const override;
	virtual bool has_terminal_render_failure() const override;
	virtual String get_terminal_render_failure_reason() const override;
	virtual void get_frame_metadata(HTMLFrameMetadata &r_metadata) const override;
	virtual void get_gpu_backdrop_frame(HTMLGPUBackdropFrame &r_frame) const override;
	virtual Ref<Texture2D> get_texture() const override;
	virtual Ref<HTMLTexture2D> get_html_texture() const override;

	HTMLSurfaceBlinkGPUBackend(blink_standalone_gpu_backend_t p_requested_backend);
	~HTMLSurfaceBlinkGPUBackend();
};
