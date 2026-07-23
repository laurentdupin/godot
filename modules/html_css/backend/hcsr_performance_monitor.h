/**************************************************************************/
/*  hcsr_performance_monitor.h                                            */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
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
		MONITOR_RETAINED_CHECKPOINT_CAPTURE_TIME,
		MONITOR_RETAINED_CHECKPOINT_RESTORE_TIME,
		MONITOR_ALLOCATED_BYTES,
		MONITOR_DISPLAY_COMMANDS,
		MONITOR_PAINT_CHUNKS,
		MONITOR_PAINT_CHUNKS_REUSED,
		MONITOR_PAINT_CHUNKS_REBUILT,
		MONITOR_UPDATED_PIXELS,
		MONITOR_INPUT_TO_VISIBLE_TIME,
		MONITOR_RESOLVED_UPDATES,
		MONITOR_DRAW_BATCHES,
		MONITOR_CLEAR_OPERATIONS,
		MONITOR_COPY_OPERATIONS,
		MONITOR_RESOLVED_COPIED_BYTES,
		MONITOR_EXECUTED_DISPLAY_COMMANDS,
		MONITOR_EXECUTED_GLYPHS,
		MONITOR_GPU_DISPATCHES,
	};

private:
	static Mutex mutex;
	static HashMap<uint64_t, hcsr_performance_profile_t> profiles;
	static Vector<hcsr_performance_profile_t> pending_profiler_profiles;
	static Vector<double> pending_input_to_visible_milliseconds;
	static double latest_input_to_visible_milliseconds;
	static double _read_monitor(int p_monitor);

public:
	static void initialize();
	static void finalize();
	static void update(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile);
	static void record_input_to_visible(double p_milliseconds);
	static void publish_frame_data();
	static void remove(uint64_t p_instance_id);
};
