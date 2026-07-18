/**************************************************************************/
/*  test_external_texture_pool.cpp                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tests/test_macros.h"

#include "core/os/os.h"

TEST_FORCE_LINK(test_external_texture_pool)

#include "servers/rendering/rendering_device.h"
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#include <d3d12.h>
#include <wrl/client.h>
#endif

#if defined(VULKAN_ENABLED) && defined(WINDOWS_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#include <vulkan/vulkan.h>
#endif

namespace TestExternalTexturePool {

#ifdef D3D12_ENABLED
TEST_CASE("[RenderingDevice][D3D12] External producer texture pool uses nonblocking three-slot timeline handoff") {
	RenderingContextDriverD3D12 *context = memnew(RenderingContextDriverD3D12);
	REQUIRE(context->initialize() == OK);
	RenderingDevice *rd = memnew(RenderingDevice);
	REQUIRE(rd->initialize(context) == OK);
	REQUIRE(rd->get_device_api_name().contains("D3D12"));
	Dictionary device_identity = rd->get_external_device_identity();
	REQUIRE(device_identity.has("adapter_luid"));

	ID3D12Device *device = reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
	REQUIRE(device != nullptr);

	RID pool = rd->external_texture_pool_create();
	REQUIRE(pool.is_valid());
	Microsoft::WRL::ComPtr<ID3D12Resource> resources[3];
	ID3D12Resource *resource_ptrs[3] = {};
	Microsoft::WRL::ComPtr<ID3D12Fence> producer_fences[3];
	Microsoft::WRL::ComPtr<ID3D12Fence> release_fences[3];
	HANDLE producer_handles[3] = {};

	D3D12_HEAP_PROPERTIES heap_properties = {};
	heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC texture_description = {};
	texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texture_description.Width = 64;
	texture_description.Height = 64;
	texture_description.DepthOrArraySize = 1;
	texture_description.MipLevels = 1;
	texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texture_description.SampleDesc.Count = 1;
	texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texture_description.Flags = D3D12_RESOURCE_FLAG_NONE;

	for (int32_t slot = 0; slot < 3; slot++) {
		REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &texture_description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resources[slot].GetAddressOf()))));
		REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(producer_fences[slot].GetAddressOf()))));
		REQUIRE(SUCCEEDED(device->CreateSharedHandle(producer_fences[slot].Get(), nullptr, GENERIC_ALL, nullptr, &producer_handles[slot])));
		const int32_t added_slot = rd->external_texture_pool_add_slot(
				pool,
				RenderingDevice::TEXTURE_TYPE_2D,
				RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
				RenderingDevice::TEXTURE_SAMPLES_1,
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT,
				uint64_t(resources[slot].Get()),
				uint64_t(producer_handles[slot]),
				64,
				64,
				1,
				1,
				1,
				RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL);
		REQUIRE(added_slot == slot);
		resource_ptrs[slot] = resources[slot].Get();
		resources[slot].Reset(); // The imported root must retain its own COM reference.
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		HANDLE release_handle = reinterpret_cast<HANDLE>(uint64_t(status["release_timeline"]));
		REQUIRE(release_handle != nullptr);
		REQUIRE(SUCCEEDED(device->OpenSharedHandle(release_handle, IID_PPV_ARGS(release_fences[slot].GetAddressOf()))));
	}

	Microsoft::WRL::ComPtr<ID3D12CommandQueue> producer_queue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> producer_allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> producer_commands;
	Microsoft::WRL::ComPtr<ID3D12Resource> producer_upload;
	D3D12_COMMAND_QUEUE_DESC producer_queue_description = {};
	producer_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	REQUIRE(SUCCEEDED(device->CreateCommandQueue(&producer_queue_description, IID_PPV_ARGS(producer_queue.GetAddressOf()))));
	REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(producer_allocator.GetAddressOf()))));
	REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, producer_allocator.Get(), nullptr, IID_PPV_ARGS(producer_commands.GetAddressOf()))));
	D3D12_HEAP_PROPERTIES upload_heap_properties = {};
	upload_heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC upload_description = {};
	upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	upload_description.Width = 64 * 64 * 4;
	upload_description.Height = 1;
	upload_description.DepthOrArraySize = 1;
	upload_description.MipLevels = 1;
	upload_description.SampleDesc.Count = 1;
	upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	REQUIRE(SUCCEEDED(device->CreateCommittedResource(&upload_heap_properties, D3D12_HEAP_FLAG_NONE, &upload_description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(producer_upload.GetAddressOf()))));
	uint8_t *upload_bytes = nullptr;
	REQUIRE(SUCCEEDED(producer_upload->Map(0, nullptr, reinterpret_cast<void **>(&upload_bytes))));
	for (uint32_t pixel = 0; pixel < 64 * 64; pixel++) {
		upload_bytes[pixel * 4 + 0] = 0x21;
		upload_bytes[pixel * 4 + 1] = 0x43;
		upload_bytes[pixel * 4 + 2] = 0x65;
		upload_bytes[pixel * 4 + 3] = 0xff;
	}
	producer_upload->Unmap(0, nullptr);
	D3D12_RESOURCE_BARRIER to_copy = {};
	to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy.Transition.pResource = resource_ptrs[0];
	to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	producer_commands->ResourceBarrier(1, &to_copy);
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = producer_upload.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	source.PlacedFootprint.Footprint.Width = 64;
	source.PlacedFootprint.Footprint.Height = 64;
	source.PlacedFootprint.Footprint.Depth = 1;
	source.PlacedFootprint.Footprint.RowPitch = 64 * 4;
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = resource_ptrs[0];
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	producer_commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	D3D12_RESOURCE_BARRIER to_common = to_copy;
	to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	producer_commands->ResourceBarrier(1, &to_common);
	REQUIRE(SUCCEEDED(producer_commands->Close()));
	ID3D12CommandList *producer_command_lists[] = { producer_commands.Get() };
	producer_queue->ExecuteCommandLists(1, producer_command_lists);
	REQUIRE(SUCCEEDED(producer_queue->Signal(producer_fences[0].Get(), 1)));

	uint64_t generation = 1;
	REQUIRE(rd->external_texture_pool_publish(pool, 0, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
	RID first_completed;
	for (uint32_t poll = 0; poll < 1000 && !first_completed.is_valid(); poll++) {
		first_completed = rd->external_texture_pool_acquire_latest(pool);
		if (!first_completed.is_valid()) {
			OS::get_singleton()->delay_usec(100);
		}
	}
	REQUIRE(first_completed.is_valid());
	// Texture2DRD/RenderingServer create both a same-format shared view and an
	// sRGB reinterpretation before exposing an RD texture to CanvasItem or 3D.
	// Imported roots must support that ordinary sampled-texture path even though
	// their native allocation remains producer-owned.
	RenderingDevice::TextureView identity_view;
	RID identity_shared = rd->texture_create_shared(identity_view, first_completed);
	REQUIRE(identity_shared.is_valid());
	RenderingDevice::TextureView srgb_view;
	srgb_view.format_override = RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB;
	RID srgb_shared = rd->texture_create_shared(srgb_view, first_completed);
	REQUIRE(srgb_shared.is_valid());
	rd->free_rid(srgb_shared);
	rd->free_rid(identity_shared);
	Vector<uint8_t> acquired_pixels = rd->texture_get_data(first_completed, 0);
	REQUIRE(acquired_pixels.size() == 64 * 64 * 4);
	CHECK(acquired_pixels[0] == 0x21);
	CHECK(acquired_pixels[1] == 0x43);
	CHECK(acquired_pixels[2] == 0x65);
	CHECK(acquired_pixels[3] == 0xff);
	generation++;
	REQUIRE(rd->external_texture_pool_publish(pool, 1, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
	const uint64_t pending_poll_start = OS::get_singleton()->get_ticks_usec();
	for (uint32_t poll = 0; poll < 256; poll++) {
		CHECK(rd->external_texture_pool_acquire_latest(pool) == first_completed);
	}
	CHECK(OS::get_singleton()->get_ticks_usec() - pending_poll_start < 1000000);
	CHECK(bool(rd->external_texture_pool_get_slot_status(pool, 1)["pending"]));
	REQUIRE(SUCCEEDED(producer_fences[1]->Signal(1)));
	RID second_completed = rd->external_texture_pool_acquire_latest(pool);
	CHECK(second_completed.is_valid());
	CHECK(second_completed != first_completed);

	for (uint64_t iteration = 1; iteration <= 96; iteration++) {
		const int32_t slot = int32_t((iteration - 1) % 3);
		Dictionary before = rd->external_texture_pool_get_slot_status(pool, slot);
		if (!bool(before["available"])) {
			rd->submit();
			rd->sync();
			before = rd->external_texture_pool_get_slot_status(pool, slot);
		}
		REQUIRE(bool(before["available"]));
		const uint64_t producer_value = iteration + 1;
		REQUIRE(SUCCEEDED(producer_fences[slot]->Signal(producer_value)));
		generation++;
		REQUIRE(rd->external_texture_pool_publish(pool, slot, producer_value, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
		RID current = rd->external_texture_pool_acquire_latest(pool);
		REQUIRE(current.is_valid());
		Dictionary adopted = rd->external_texture_pool_get_slot_status(pool, slot);
		REQUIRE(bool(adopted["current"]));
		REQUIRE(uint64_t(adopted["generation"]) == generation);
	}
	rd->submit();
	rd->sync();
	rd->external_texture_pool_acquire_latest(pool);
	int32_t abandoned_slot = -1;
	for (int32_t slot = 0; slot < 3; slot++) {
		if (bool(rd->external_texture_pool_get_slot_status(pool, slot)["available"])) {
			abandoned_slot = slot;
			break;
		}
	}
	REQUIRE(abandoned_slot >= 0);
	generation++;
	REQUIRE(rd->external_texture_pool_publish(pool, abandoned_slot, 1000, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
	REQUIRE(rd->external_texture_pool_abandon_pending(pool, abandoned_slot) == OK);

	rd->external_texture_pool_stop(pool);
	rd->submit();
	rd->sync();
	for (int32_t slot = 0; slot < 3; slot++) {
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		CHECK(bool(status["release_complete"]));
		CHECK(release_fences[slot]->GetCompletedValue() >= uint64_t(status["release_value"]));
		CloseHandle(producer_handles[slot]);
	}
	rd->free_rid(pool);
	rd->submit();
	rd->sync();
	producer_upload.Reset();
	producer_commands.Reset();
	producer_allocator.Reset();
	producer_queue.Reset();
	for (int32_t slot = 0; slot < 3; slot++) {
		producer_fences[slot].Reset();
		release_fences[slot].Reset();
	}
	memdelete(rd);
	memdelete(context);
}
#endif

#if defined(VULKAN_ENABLED) && defined(WINDOWS_ENABLED)
TEST_CASE("[RenderingDevice][Vulkan] External producer texture pool uses nonblocking three-slot timeline handoff") {
	RenderingContextDriverVulkan *context = memnew(RenderingContextDriverVulkan);
	REQUIRE(context->initialize() == OK);
	RenderingDevice *rd = memnew(RenderingDevice);
	REQUIRE(rd->initialize(context) == OK);
	REQUIRE(rd->get_device_api_name().contains("Vulkan"));
	Dictionary device_identity = rd->get_external_device_identity();
	REQUIRE(device_identity.has("adapter_luid"));

	VkDevice device = reinterpret_cast<VkDevice>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
	VkPhysicalDevice physical_device = reinterpret_cast<VkPhysicalDevice>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE));
	REQUIRE(device != VK_NULL_HANDLE);
	REQUIRE(physical_device != VK_NULL_HANDLE);

	VkPhysicalDeviceMemoryProperties memory_properties = {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	RID pool = rd->external_texture_pool_create();
	REQUIRE(pool.is_valid());
	VkImage images[3] = {};
	VkDeviceMemory memories[3] = {};
	VkSemaphore producer_timelines[3] = {};
	VkSemaphore release_timelines[3] = {};
	HANDLE producer_handles[3] = {};

	for (int32_t slot = 0; slot < 3; slot++) {
		VkImageCreateInfo image_info = {};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
		image_info.extent = { 64, 64, 1 };
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		REQUIRE(vkCreateImage(device, &image_info, nullptr, &images[slot]) == VK_SUCCESS);

		VkMemoryRequirements memory_requirements = {};
		vkGetImageMemoryRequirements(device, images[slot], &memory_requirements);
		uint32_t memory_type = UINT32_MAX;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
			if ((memory_requirements.memoryTypeBits & (1U << i)) != 0 && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
				memory_type = i;
				break;
			}
		}
		REQUIRE(memory_type != UINT32_MAX);
		VkMemoryAllocateInfo allocation_info = {};
		allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation_info.allocationSize = memory_requirements.size;
		allocation_info.memoryTypeIndex = memory_type;
		REQUIRE(vkAllocateMemory(device, &allocation_info, nullptr, &memories[slot]) == VK_SUCCESS);
		REQUIRE(vkBindImageMemory(device, images[slot], memories[slot], 0) == VK_SUCCESS);

		VkExportSemaphoreCreateInfo export_info = {};
		export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
		export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkSemaphoreTypeCreateInfo timeline_info = {};
		timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		timeline_info.pNext = &export_info;
		timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphore_info = {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = &timeline_info;
		REQUIRE(vkCreateSemaphore(device, &semaphore_info, nullptr, &producer_timelines[slot]) == VK_SUCCESS);
		VkSemaphoreGetWin32HandleInfoKHR producer_handle_info = {};
		producer_handle_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
		producer_handle_info.semaphore = producer_timelines[slot];
		producer_handle_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		REQUIRE(vkGetSemaphoreWin32HandleKHR(device, &producer_handle_info, &producer_handles[slot]) == VK_SUCCESS);

		REQUIRE(rd->external_texture_pool_add_slot(pool, RenderingDevice::TEXTURE_TYPE_2D, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, RenderingDevice::TEXTURE_SAMPLES_1, RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT, uint64_t(images[slot]), uint64_t(producer_handles[slot]), 64, 64, 1, 1, 1, RenderingDevice::EXTERNAL_TEXTURE_STATE_UNDEFINED) == slot);
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		HANDLE release_handle = reinterpret_cast<HANDLE>(uint64_t(status["release_timeline"]));
		REQUIRE(release_handle != nullptr);
		VkSemaphoreTypeCreateInfo release_timeline_info = {};
		release_timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		release_timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo release_semaphore_info = {};
		release_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		release_semaphore_info.pNext = &release_timeline_info;
		REQUIRE(vkCreateSemaphore(device, &release_semaphore_info, nullptr, &release_timelines[slot]) == VK_SUCCESS);
		VkImportSemaphoreWin32HandleInfoKHR import_info = {};
		import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
		import_info.semaphore = release_timelines[slot];
		import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		import_info.handle = release_handle;
		REQUIRE(vkImportSemaphoreWin32HandleKHR(device, &import_info) == VK_SUCCESS);
	}

	uint64_t generation = 1;
	VkSemaphoreSignalInfo first_signal = {};
	first_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
	first_signal.semaphore = producer_timelines[0];
	first_signal.value = 1;
	REQUIRE(vkSignalSemaphore(device, &first_signal) == VK_SUCCESS);
	REQUIRE(rd->external_texture_pool_publish(pool, 0, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_UNDEFINED) == OK);
	RID first_completed = rd->external_texture_pool_acquire_latest(pool);
	REQUIRE(first_completed.is_valid());
	RenderingDevice::TextureView identity_view;
	RID identity_shared = rd->texture_create_shared(identity_view, first_completed);
	REQUIRE(identity_shared.is_valid());
	RenderingDevice::TextureView srgb_view;
	srgb_view.format_override = RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB;
	RID srgb_shared = rd->texture_create_shared(srgb_view, first_completed);
	REQUIRE(srgb_shared.is_valid());
	rd->free_rid(srgb_shared);
	rd->free_rid(identity_shared);
	generation++;
	REQUIRE(rd->external_texture_pool_publish(pool, 1, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_UNDEFINED) == OK);
	const uint64_t pending_poll_start = OS::get_singleton()->get_ticks_usec();
	for (uint32_t poll = 0; poll < 256; poll++) {
		CHECK(rd->external_texture_pool_acquire_latest(pool) == first_completed);
	}
	CHECK(OS::get_singleton()->get_ticks_usec() - pending_poll_start < 1000000);
	VkSemaphoreSignalInfo second_signal = first_signal;
	second_signal.semaphore = producer_timelines[1];
	REQUIRE(vkSignalSemaphore(device, &second_signal) == VK_SUCCESS);
	REQUIRE(rd->external_texture_pool_acquire_latest(pool).is_valid());

	for (uint64_t iteration = 1; iteration <= 96; iteration++) {
		const int32_t slot = int32_t((iteration - 1) % 3);
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		for (uint32_t release_poll = 0; release_poll < 16 && !bool(status["available"]); release_poll++) {
			rd->submit();
			rd->sync();
			rd->external_texture_pool_acquire_latest(pool);
			status = rd->external_texture_pool_get_slot_status(pool, slot);
		}
		REQUIRE(bool(status["available"]));
		VkSemaphoreSignalInfo signal_info = {};
		signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
		signal_info.semaphore = producer_timelines[slot];
		const uint64_t producer_value = iteration + 1;
		signal_info.value = producer_value;
		REQUIRE(vkSignalSemaphore(device, &signal_info) == VK_SUCCESS);
		generation++;
		REQUIRE(rd->external_texture_pool_publish(pool, slot, producer_value, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_UNDEFINED) == OK);
		REQUIRE(rd->external_texture_pool_acquire_latest(pool).is_valid());
	}
	rd->submit();
	rd->sync();
	rd->external_texture_pool_acquire_latest(pool);
	int32_t abandoned_slot = -1;
	for (int32_t slot = 0; slot < 3; slot++) {
		if (bool(rd->external_texture_pool_get_slot_status(pool, slot)["available"])) {
			abandoned_slot = slot;
			break;
		}
	}
	REQUIRE(abandoned_slot >= 0);
	generation++;
	REQUIRE(rd->external_texture_pool_publish(pool, abandoned_slot, 1000, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_UNDEFINED) == OK);
	REQUIRE(rd->external_texture_pool_abandon_pending(pool, abandoned_slot) == OK);

	rd->external_texture_pool_stop(pool);
	rd->submit();
	rd->sync();
	for (int32_t slot = 0; slot < 3; slot++) {
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		uint64_t completed_value = 0;
		REQUIRE(vkGetSemaphoreCounterValue(device, release_timelines[slot], &completed_value) == VK_SUCCESS);
		CHECK(completed_value >= uint64_t(status["release_value"]));
	}
	rd->free_rid(pool);
	rd->submit();
	rd->sync();
	for (int32_t slot = 0; slot < 3; slot++) {
		vkDestroySemaphore(device, release_timelines[slot], nullptr);
		vkDestroySemaphore(device, producer_timelines[slot], nullptr);
		CloseHandle(producer_handles[slot]);
		vkDestroyImage(device, images[slot], nullptr);
		vkFreeMemory(device, memories[slot], nullptr);
	}
	memdelete(rd);
	memdelete(context);
}
#endif

} // namespace TestExternalTexturePool
