/**************************************************************************/
/*  html_desktop_overlay.h                                                */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "html_view_output.h"

class HTMLDesktopOverlay : public RefCounted {
	GDCLASS(HTMLDesktopOverlay, RefCounted);

	using GetAbiVersionFn = uint32_t (*)();
	using CreateD3D12Fn = uint64_t (*)(int32_t, int32_t, int32_t, int32_t, uint64_t, uint64_t);
	using DestroyFn = int32_t (*)(uint64_t);
	using UpdateD3D12Fn = int32_t (*)(uint64_t, uint64_t, uint64_t, int32_t, int32_t, uint32_t);
	using SetRectFn = int32_t (*)(uint64_t, int32_t, int32_t, int32_t, int32_t);
	using SetVisibleFn = int32_t (*)(uint64_t, int32_t);
	using GetLastErrorFn = uint32_t (*)();

	void *library = nullptr;
	uint64_t overlay = 0;
	uint64_t device = 0;
	uint64_t queue = 0;
	uint64_t scheduled_generation = 0;
	uint64_t presented_generation = 0;
	uint32_t last_native_error = 0;
	GetAbiVersionFn get_abi_version = nullptr;
	CreateD3D12Fn create_d3d12 = nullptr;
	DestroyFn destroy = nullptr;
	UpdateD3D12Fn update_d3d12 = nullptr;
	SetRectFn set_rect_native = nullptr;
	SetVisibleFn set_visible_native = nullptr;
	GetLastErrorFn get_last_error = nullptr;

	static void _capture_device_on_render_thread(uint64_t p_self);
	static void _present_on_render_thread(uint64_t p_self, RID p_texture,
			uint64_t p_generation, Size2i p_size);
	void _unload_library();

protected:
	static void _bind_methods();

public:
	Error create(const Rect2i &p_rect, const String &p_library_path);
	Error present(const Ref<HTMLViewOutput> &p_output);
	Error set_rect(const Rect2i &p_rect);
	Error set_visible(bool p_visible);
	void close();
	bool is_open() const;
	uint32_t get_last_native_error() const;
	uint64_t get_presented_generation() const;

	~HTMLDesktopOverlay();
};
