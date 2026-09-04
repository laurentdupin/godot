/**************************************************************************/
/*  hcsr_newest_performance_monitor.h                                    */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"

#include "hcsr_scene.h"

class HCSRNewestPerformanceMonitor {
public:
	struct Sample {
		hcsr_scene_profile_t scene = {};
		double record_and_submit_seconds = 0.0;
		double input_to_visible_seconds = 0.0;
	};

	enum Monitor {
		MONITOR_FRAME_TIME,
		MONITOR_CORE_TIME,
		MONITOR_STYLE_TIME,
		MONITOR_LAYOUT_TIME,
		MONITOR_HIT_TEST_TIME,
		MONITOR_INTERACTION_TIME,
		MONITOR_DISPLAY_LIST_TIME,
		MONITOR_PHYSICAL_COMPILATION_TIME,
		MONITOR_RECORD_AND_SUBMIT_TIME,
		MONITOR_GPU_SUBMIT_TIME,
		MONITOR_INPUT_TO_VISIBLE_TIME,
		MONITOR_MANAGED_ALLOCATION,
		MONITOR_STYLE_ALLOCATED_BYTES,
		MONITOR_LAYOUT_ALLOCATED_BYTES,
		MONITOR_DISPLAY_LIST_ALLOCATED_BYTES,
		MONITOR_VISITED_PAINT_CHUNKS,
		MONITOR_CHANGED_PAINT_CHUNKS,
		MONITOR_PAINT_CHUNKS_REBUILT,
		MONITOR_SEMANTIC_SNAPSHOTS_REUSED,
		MONITOR_SEMANTIC_SNAPSHOTS_RECREATED,
		MONITOR_LAYOUT_NODE_CALLS,
		MONITOR_DISTINCT_LAYOUT_NODES,
		MONITOR_EQUIVALENT_LAYOUT_REENTRIES,
	};

private:
	static Mutex mutex;
	static HashMap<uint64_t, Sample> samples;
	static double _read_monitor(int p_monitor);

public:
	static void initialize();
	static void finalize();
	static void update_scene(uint64_t p_instance_id, const hcsr_scene_profile_t &p_profile);
	static void update_presentation(uint64_t p_instance_id, double p_record_and_submit_seconds, double p_input_to_visible_seconds);
	static void remove(uint64_t p_instance_id);
};
