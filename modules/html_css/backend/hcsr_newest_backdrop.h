#pragma once

#include "../bridge/html_frame_types.h"
#include "hcsr_scene.h"

#include "scene/resources/texture_rd.h"
#include "servers/rendering/rendering_device.h"

// Scene coverage is supplied by HCSR; this adapter only rasterizes the host mask.
class HCSRNewestBackdrop {
	Vector<uint8_t> previous;
	Size2i previous_size;
	HTMLGPUBackdropFrame frame;
	Vector<uint8_t> geometry_signature;
	RID shader, pipeline, uniform, target;
	RID buffers[4];
	uint32_t capacities[4] = {};
	Ref<Texture2DRD> gpu_texture;
	uint64_t geometry_uploaded_bytes = 0, redraws = 0;
	bool draw_gpu(const hcsr_backdrop_view_t &view, const hcsr_draw_packet_view_t &packet, const Size2i &logical, const Size2i &physical);
	void release_gpu();

public:
	~HCSRNewestBackdrop();
	Dictionary get_statistics() const;
	const HTMLGPUBackdropFrame &update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical);
};
