#pragma once

#include "../bridge/html_frame_types.h"
#include "hcsr_scene.h"
#include "hcsr_newest_scene_renderer.h"

#include "scene/resources/texture_rd.h"
#include "servers/rendering/rendering_device.h"

// Scene coverage is supplied by HCSR; this adapter submits cached atlas surfaces into the host mask.
class HCSRNewestBackdrop {
	Vector<uint8_t> previous;
	Size2i previous_size;
	HTMLGPUBackdropFrame frame;

	RID target;
	Ref<Texture2DRD> gpu_texture;
	uint64_t redraws = 0;
    uint32_t surface_instances=0;
	bool draw_gpu(const hcsr_backdrop_view_t &view, const hcsr_draw_packet_view_t &packet, const Size2i &logical, const Size2i &physical, HCSRNewestSceneRenderer &renderer);
	void release_gpu();

public:
	~HCSRNewestBackdrop();
    void release() { release_gpu(); }
	Dictionary get_statistics() const;
	const HTMLGPUBackdropFrame &update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical, HCSRNewestSceneRenderer &renderer);
};
