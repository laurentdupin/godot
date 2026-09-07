#pragma once

#include "../bridge/html_frame_types.h"
#include "hcsr_scene.h"

// Scene coverage is supplied by HCSR; this adapter only rasterizes the host mask.
class HCSRNewestBackdrop {
	Vector<uint8_t> previous;
	Size2i previous_size;
	HTMLGPUBackdropFrame frame;

public:
	const HTMLGPUBackdropFrame &update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical);
};
