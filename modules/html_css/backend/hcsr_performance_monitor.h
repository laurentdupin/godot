/**************************************************************************/
/*  hcsr_performance_monitor.h                                            */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"

#include "hcsr_renderer.h"

class HCSRPerformanceMonitor {
public:
	enum Monitor {
		MONITOR_FRAME_TIME,
		MONITOR_CORE_TIME,
		MONITOR_PARSE_TIME,
		MONITOR_STYLE_TIME,
		MONITOR_LAYOUT_TIME,
		MONITOR_HIT_TEST_TIME,
		MONITOR_INTERACTION_TIME,
		MONITOR_DISPLAY_LIST_TIME,
		MONITOR_RASTER_TIME,
		MONITOR_GPU_SUBMIT_TIME,
		MONITOR_ALLOCATED_BYTES,
		MONITOR_DISPLAY_COMMANDS,
		MONITOR_PAINT_CHUNKS,
		MONITOR_PAINT_CHUNKS_REUSED,
		MONITOR_PAINT_CHUNKS_REBUILT,
		MONITOR_UPDATED_PIXELS,
	};

private:
	static Mutex mutex;
	static HashMap<uint64_t, hcsr_performance_profile_t> profiles;
	static double _read_monitor(int p_monitor);

public:
	static void initialize();
	static void finalize();
	static void update(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile);
	static void remove(uint64_t p_instance_id);
};
