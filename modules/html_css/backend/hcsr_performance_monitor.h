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
		MONITOR_SEMANTIC_PREPARATION_TIME,
		MONITOR_SEMANTIC_SNAPSHOT_VALIDATION_TIME,
		MONITOR_SEMANTIC_SNAPSHOTS_REUSED,
		MONITOR_SEMANTIC_SNAPSHOTS_RECREATED,
		MONITOR_TRANSLATED_PACKETS,
		MONITOR_TRANSLATED_COMMANDS,
		MONITOR_TRANSLATION_ALLOCATED_BYTES,
		MONITOR_PHYSICAL_COMPILATION_TIME,
		MONITOR_PHYSICAL_COMPILATION_ALLOCATED_BYTES,
		MONITOR_PHYSICAL_PROJECTION_TIME,
		MONITOR_PAINTER_ORDER_PLANNING_TIME,
		MONITOR_PHYSICAL_INSTANCE_EXPANSION_TIME,
		MONITOR_RECORD_AND_SUBMIT_TIME,
		MONITOR_PAINTER_ORDER_INTERSECTION_TESTS,
		MONITOR_PAINTER_ORDER_FLUSHES,
		MONITOR_TEXTURE_RESOURCE_CREATES,
		MONITOR_TEXTURE_RESOURCE_FREES,
		MONITOR_PRESENTATION_LOCK_BUSY,
		MONITOR_CAPACITY_PROBE_CANCELLATIONS,
	};
	struct IntegrationCounters {
		uint64_t texture_resource_creates = 0;
		uint64_t texture_resource_frees = 0;
		uint64_t presentation_lock_busy = 0;
		uint64_t capacity_probe_cancellations = 0;
	};

private:
	static Mutex mutex;
	static HashMap<uint64_t, hcsr_performance_profile_t> profiles;
	static HashMap<uint64_t, IntegrationCounters> integration_counters;
	static Vector<hcsr_performance_profile_t> pending_profiler_profiles;
	static Vector<double> pending_input_to_visible_milliseconds;
	static double latest_input_to_visible_milliseconds;
	static double _read_monitor(int p_monitor);

public:
	static void initialize();
	static void finalize();
	static void update(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile);
	static void update_integration(uint64_t p_instance_id, const IntegrationCounters &p_counters);
	static void record_input_to_visible(double p_milliseconds);
	static void publish_frame_data();
	static void remove(uint64_t p_instance_id);
};
