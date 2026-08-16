/**************************************************************************/
/*  hcsr_performance_monitor.h                                            */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#include "hcsr_renderer.h"

class HCSRPerformanceProfileStore {
public:
	struct GenerationSample {
		uint64_t instance_id = 0;
		uint64_t generation = 0;
		hcsr_performance_profile_t profile = {};
	};

private:
	// Custom monitors read the latest snapshot, while the Servers profiler
	// consumes each successfully completed generation at most once.
	HashMap<uint64_t, hcsr_performance_profile_t> latest_profiles;
	HashMap<uint64_t, uint64_t> completed_generations;
	Vector<GenerationSample> pending_generation_samples;

public:
	void update_latest(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile);
	bool complete_generation(uint64_t p_instance_id, uint64_t p_generation, const hcsr_performance_profile_t &p_profile);
	const HashMap<uint64_t, hcsr_performance_profile_t> &get_latest_profiles() const;
	Vector<GenerationSample> take_pending_generation_samples();
	void discard_pending_generation_samples();
	void remove(uint64_t p_instance_id);
	void clear();
};

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
		MONITOR_PRESENTATION_CHECKPOINT_CAPTURE_TIME,
		MONITOR_PRESENTATION_CHECKPOINT_RESTORE_TIME,
		MONITOR_ALLOCATED_BYTES,
		MONITOR_DISPLAY_COMMANDS,
		MONITOR_PAINT_CHUNKS,
		MONITOR_PAINT_CHUNKS_REUSED,
		MONITOR_PAINT_CHUNKS_REBUILT,
		MONITOR_UPDATED_PIXELS,
		MONITOR_INPUT_TO_VISIBLE_TIME,
		MONITOR_INPUT_TO_COMPOSED_TIME,
		MONITOR_DRAW_BATCHES,
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
		MONITOR_RUNTIME_ALLOCATED_BYTES,
		MONITOR_RUNTIME_ALLOCATION_RATE,
		MONITOR_RUNTIME_GC_PAUSE_TIME,
		MONITOR_RUNTIME_GC_PAUSE_OBSERVED_COLLECTIONS,
		MONITOR_RUNTIME_GC_PAUSE_UNOBSERVED_COLLECTIONS,
		MONITOR_RUNTIME_GEN0_COLLECTIONS,
		MONITOR_RUNTIME_GEN1_COLLECTIONS,
		MONITOR_RUNTIME_GEN2_COLLECTIONS,
		MONITOR_RUNTIME_MANAGED_HEAP_BYTES,
		MONITOR_RUNTIME_MANAGED_COMMITTED_BYTES,
		MONITOR_RUNTIME_LOH_BYTES,
		MONITOR_RUNTIME_LOH_FRAGMENTATION_BYTES,
		MONITOR_INPUT_STATE_APPLICATION_TIME,
		MONITOR_SNAPSHOT_PUBLICATION_TIME,
		MONITOR_COMPLETION_RETIREMENT_TIME,
		MONITOR_OWNER_LOCK_WAIT_TIME,
		MONITOR_OWNER_LOCK_CONTENTIONS,
		MONITOR_STYLE_ALLOCATED_BYTES,
		MONITOR_LAYOUT_ALLOCATED_BYTES,
		MONITOR_LAYOUT_TREE_ALLOCATED_BYTES,
		MONITOR_LAYOUT_TEXT_SHAPING_ALLOCATED_BYTES,
		MONITOR_HIT_TEST_ALLOCATED_BYTES,
		MONITOR_DISPLAY_LIST_ALLOCATED_BYTES,
		MONITOR_PAINT_PREPARATION_ALLOCATED_BYTES,
		MONITOR_PAINT_TRAVERSAL_ALLOCATED_BYTES,
		MONITOR_PAINT_SCENE_CONSTRUCTION_ALLOCATED_BYTES,
		MONITOR_PAINT_CANDIDATE_INDEX_ALLOCATED_BYTES,
		MONITOR_COMPLETE_FRAME_MATERIALIZATION_ALLOCATED_BYTES,
		MONITOR_COMPLETE_SEQUENCE_CONSTRUCTION_ALLOCATED_BYTES,
		MONITOR_SEMANTIC_SNAPSHOT_ALLOCATED_BYTES,
		MONITOR_RASTER_ALLOCATED_BYTES,
		MONITOR_AUTHORITATIVE_PAINT_CHUNKS,
		MONITOR_VISITED_PAINT_CHUNKS,
		MONITOR_CHANGED_PAINT_CHUNKS,
		MONITOR_IMMUTABLE_COMMAND_REFERENCES,
		MONITOR_FLAT_COMMAND_REFERENCES,
		MONITOR_SEMANTIC_SEGMENTS,
		MONITOR_NEW_SCOPE_COMMANDS,
		MONITOR_NEW_SCROLLBAR_COMMANDS,
		MONITOR_SNAPSHOT_ENTRIES_WRITTEN,
		MONITOR_RETAINED_TRANSFORM_LAYER_HITS,
		MONITOR_RETAINED_TRANSFORM_LAYER_RASTERS,
		MONITOR_TEXTURE_RESOURCE_CREATES,
		MONITOR_TEXTURE_RESOURCE_FREES,
		MONITOR_PRESENTATION_LOCK_BUSY,
		MONITOR_CAPACITY_PROBE_CANCELLATIONS,
		MONITOR_CPU_PRIMARY_PUBLICATION_TIME,
		MONITOR_CPU_SECONDARY_PUBLICATION_TIME,
		MONITOR_CPU_PRIMARY_CONVERSION_TIME,
		MONITOR_CPU_PRIMARY_UPLOAD_TIME,
		MONITOR_CPU_SECONDARY_CONVERSION_TIME,
		MONITOR_CPU_SECONDARY_UPLOAD_TIME,
		MONITOR_MANAGED_EXPORT_BOUNDARY_OVERHEAD_TIME,
		MONITOR_SEMANTIC_WORKER_MAILBOX_DELAY_TIME,
		MONITOR_SEMANTIC_WORKER_SUPERSESSIONS,
		MONITOR_SEMANTIC_WORKER_HOST_CALL_TIME,
		MONITOR_RUNTIME_SESSION_STEP_TIME,
		MONITOR_RUNTIME_PRESENTATION_SLICE_TIME,
		MONITOR_RUNTIME_SESSION_WORK_UNITS,
		MONITOR_RUNTIME_CHANGED_TILE_BYTES,
		MONITOR_RUNTIME_TEXTURE_UPLOAD_BYTES,
		MONITOR_RUNTIME_RETIRING_SESSIONS,
	};
	struct IntegrationCounters {
		uint64_t texture_resource_creates = 0;
		uint64_t texture_resource_frees = 0;
		uint64_t presentation_lock_busy = 0;
		uint64_t capacity_probe_cancellations = 0;
		double cpu_primary_publication_milliseconds = 0.0;
		double cpu_secondary_publication_milliseconds = 0.0;
		double cpu_primary_conversion_milliseconds = 0.0;
		double cpu_primary_upload_milliseconds = 0.0;
		double cpu_secondary_conversion_milliseconds = 0.0;
		double cpu_secondary_upload_milliseconds = 0.0;
		double managed_export_boundary_overhead_milliseconds = 0.0;
		double semantic_worker_mailbox_delay_milliseconds = 0.0;
		uint64_t semantic_worker_supersessions = 0;
		double semantic_worker_host_call_milliseconds = 0.0;
		double runtime_session_step_milliseconds = 0.0;
		double runtime_presentation_slice_milliseconds = 0.0;
		uint64_t runtime_session_work_units = 0;
		uint64_t runtime_changed_tile_bytes = 0;
		uint64_t runtime_texture_upload_bytes = 0;
		uint64_t runtime_retiring_sessions = 0;
	};

private:
	static Mutex mutex;
	static HCSRPerformanceProfileStore profile_store;
	static HashMap<uint64_t, IntegrationCounters> integration_counters;
	static double latest_input_to_visible_milliseconds;
	static double latest_input_to_composed_milliseconds;
	static double _read_monitor(int p_monitor);

public:
	static void initialize();
	static void finalize();
	static void update_latest(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile);
	static void complete_generation(uint64_t p_instance_id, uint64_t p_generation, const hcsr_performance_profile_t &p_profile);
	static void update_integration(uint64_t p_instance_id, const IntegrationCounters &p_counters);
	static void record_input_to_visible(double p_milliseconds);
	static void record_input_to_composed(double p_milliseconds);
	static Array build_profiler_frame_data(const Vector<HCSRPerformanceProfileStore::GenerationSample> &p_samples);
	static void publish_frame_data();
	static void remove(uint64_t p_instance_id);
};
