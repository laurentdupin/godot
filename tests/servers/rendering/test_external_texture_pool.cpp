/**************************************************************************/
/*  test_external_texture_pool.cpp                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_external_texture_pool)

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#ifdef D3D12_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace TestExternalTexturePool {

#ifdef D3D12_ENABLED
TEST_CASE("[RenderingDevice][D3D12] External producer texture pool uses nonblocking three-slot timeline handoff") {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return;
	}
	RenderingDevice *rd = rendering_server->get_rendering_device();
	if (rd == nullptr || !rd->get_device_api_name().contains("D3D12")) {
		return;
	}

	ID3D12Device *device = reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
	REQUIRE(device != nullptr);

	RID pool = rd->external_texture_pool_create();
	REQUIRE(pool.is_valid());
	Microsoft::WRL::ComPtr<ID3D12Resource> resources[3];
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
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT,
				uint64_t(resources[slot].Get()),
				uint64_t(producer_handles[slot]),
				64,
				64,
				1,
				1,
				1,
				RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL);
		REQUIRE(added_slot == slot);
		Dictionary status = rd->external_texture_pool_get_slot_status(pool, slot);
		HANDLE release_handle = reinterpret_cast<HANDLE>(uint64_t(status["release_timeline"]));
		REQUIRE(release_handle != nullptr);
		REQUIRE(SUCCEEDED(device->OpenSharedHandle(release_handle, IID_PPV_ARGS(release_fences[slot].GetAddressOf()))));
	}

	uint64_t generation = 1;
	REQUIRE(SUCCEEDED(producer_fences[0]->Signal(1)));
	REQUIRE(rd->external_texture_pool_publish(pool, 0, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
	RID first_completed = rd->external_texture_pool_acquire_latest(pool);
	REQUIRE(first_completed.is_valid());
	generation++;
	REQUIRE(rd->external_texture_pool_publish(pool, 1, 1, generation, RenderingDevice::EXTERNAL_TEXTURE_STATE_GENERAL) == OK);
	CHECK(rd->external_texture_pool_acquire_latest(pool) == first_completed);
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
}
#endif

} // namespace TestExternalTexturePool
