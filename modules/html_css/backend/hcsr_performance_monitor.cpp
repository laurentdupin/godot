/**************************************************************************/
/*  hcsr_performance_monitor.cpp                                          */
/**************************************************************************/

#include "hcsr_performance_monitor.h"

#include "core/debugger/engine_debugger.h"
#include "core/object/callable_mp.h"
#include "main/performance.h"

Mutex HCSRPerformanceMonitor::mutex;
HashMap<uint64_t, hcsr_performance_profile_t> HCSRPerformanceMonitor::profiles;
HashMap<uint64_t, HCSRPerformanceMonitor::IntegrationCounters> HCSRPerformanceMonitor::integration_counters;
Vector<hcsr_performance_profile_t> HCSRPerformanceMonitor::pending_profiler_profiles;
Vector<double> HCSRPerformanceMonitor::pending_input_to_visible_milliseconds;
Vector<double> HCSRPerformanceMonitor::pending_input_to_composed_milliseconds;
double HCSRPerformanceMonitor::latest_input_to_visible_milliseconds = 0.0;
double HCSRPerformanceMonitor::latest_input_to_composed_milliseconds = 0.0;

struct HCSRMonitorDefinition {
	const char *name;
	HCSRPerformanceMonitor::Monitor monitor;
	Performance::MonitorType type;
};

void HCSRPerformanceMonitor::initialize() {
	Performance *performance = Performance::get_singleton();
	if (performance == nullptr) {
		return;
	}

	static const HCSRMonitorDefinition definitions[] = {
		{ "HCSR/Frame Time", MONITOR_FRAME_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Core Pipeline Time", MONITOR_CORE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Parse Time", MONITOR_PARSE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Style Time", MONITOR_STYLE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Layout Time", MONITOR_LAYOUT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Hit Test Time", MONITOR_HIT_TEST_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Interaction Time", MONITOR_INTERACTION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Display List Time", MONITOR_DISPLAY_LIST_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Raster Time", MONITOR_RASTER_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/GPU Submit Time", MONITOR_GPU_SUBMIT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Presentation Checkpoint Capture Time", MONITOR_PRESENTATION_CHECKPOINT_CAPTURE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Presentation Checkpoint Restore Time", MONITOR_PRESENTATION_CHECKPOINT_RESTORE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Managed Allocation", MONITOR_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Display Commands", MONITOR_DISPLAY_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks", MONITOR_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks Reused", MONITOR_PAINT_CHUNKS_REUSED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks Rebuilt", MONITOR_PAINT_CHUNKS_REBUILT, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Updated Pixels", MONITOR_UPDATED_PIXELS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Input To Visible Time", MONITOR_INPUT_TO_VISIBLE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Input To Composed Time", MONITOR_INPUT_TO_COMPOSED_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Resolved Updates", MONITOR_RESOLVED_UPDATES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Draw Batches", MONITOR_DRAW_BATCHES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Clear Operations", MONITOR_CLEAR_OPERATIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Copy Operations", MONITOR_COPY_OPERATIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Resolved Copied Bytes", MONITOR_RESOLVED_COPIED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Executed Display Commands", MONITOR_EXECUTED_DISPLAY_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Executed Glyphs", MONITOR_EXECUTED_GLYPHS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/GPU Dispatches", MONITOR_GPU_DISPATCHES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Preparation Time", MONITOR_SEMANTIC_PREPARATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Semantic Snapshot Validation Time", MONITOR_SEMANTIC_SNAPSHOT_VALIDATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Semantic Snapshots Reused", MONITOR_SEMANTIC_SNAPSHOTS_REUSED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Snapshots Recreated", MONITOR_SEMANTIC_SNAPSHOTS_RECREATED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Translated Packets", MONITOR_TRANSLATED_PACKETS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Translated Commands", MONITOR_TRANSLATED_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Translation Allocated Bytes", MONITOR_TRANSLATION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Physical Compilation Time", MONITOR_PHYSICAL_COMPILATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Physical Compilation Allocated Bytes", MONITOR_PHYSICAL_COMPILATION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Physical Projection Time", MONITOR_PHYSICAL_PROJECTION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Painter Order Planning Time", MONITOR_PAINTER_ORDER_PLANNING_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Physical Instance Expansion Time", MONITOR_PHYSICAL_INSTANCE_EXPANSION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Record And Submit Time", MONITOR_RECORD_AND_SUBMIT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Painter Order Intersection Tests", MONITOR_PAINTER_ORDER_INTERSECTION_TESTS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Painter Order Flushes", MONITOR_PAINTER_ORDER_FLUSHES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime Allocated Bytes", MONITOR_RUNTIME_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Runtime Allocation Rate MBps", MONITOR_RUNTIME_ALLOCATION_RATE, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime GC Pause Time", MONITOR_RUNTIME_GC_PAUSE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Runtime GC Pause Observed Collections", MONITOR_RUNTIME_GC_PAUSE_OBSERVED_COLLECTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime GC Pause Unobserved Collections", MONITOR_RUNTIME_GC_PAUSE_UNOBSERVED_COLLECTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime Gen0 Collections", MONITOR_RUNTIME_GEN0_COLLECTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime Gen1 Collections", MONITOR_RUNTIME_GEN1_COLLECTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime Gen2 Collections", MONITOR_RUNTIME_GEN2_COLLECTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Runtime Managed Heap Bytes", MONITOR_RUNTIME_MANAGED_HEAP_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Runtime Managed Committed Bytes", MONITOR_RUNTIME_MANAGED_COMMITTED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Runtime LOH Bytes", MONITOR_RUNTIME_LOH_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Runtime LOH Fragmentation Bytes", MONITOR_RUNTIME_LOH_FRAGMENTATION_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Input State Application Time", MONITOR_INPUT_STATE_APPLICATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Snapshot Publication Time", MONITOR_SNAPSHOT_PUBLICATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Completion Retirement Time", MONITOR_COMPLETION_RETIREMENT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Owner Lock Wait Time", MONITOR_OWNER_LOCK_WAIT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Owner Lock Contentions", MONITOR_OWNER_LOCK_CONTENTIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Style Allocated Bytes", MONITOR_STYLE_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Layout Allocated Bytes", MONITOR_LAYOUT_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Layout Tree Allocated Bytes", MONITOR_LAYOUT_TREE_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Text Shaping Allocated Bytes", MONITOR_LAYOUT_TEXT_SHAPING_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Hit Test Allocated Bytes", MONITOR_HIT_TEST_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Display List Allocated Bytes", MONITOR_DISPLAY_LIST_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Paint Preparation Allocated Bytes", MONITOR_PAINT_PREPARATION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Paint Traversal Allocated Bytes", MONITOR_PAINT_TRAVERSAL_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Paint Scene Allocated Bytes", MONITOR_PAINT_SCENE_CONSTRUCTION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Paint Candidate Index Allocated Bytes", MONITOR_PAINT_CANDIDATE_INDEX_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Complete Frame Materialization Allocated Bytes", MONITOR_COMPLETE_FRAME_MATERIALIZATION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Complete Sequence Allocated Bytes", MONITOR_COMPLETE_SEQUENCE_CONSTRUCTION_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Semantic Snapshot Allocated Bytes", MONITOR_SEMANTIC_SNAPSHOT_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Raster Allocated Bytes", MONITOR_RASTER_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Authoritative Paint Chunks", MONITOR_AUTHORITATIVE_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Visited Paint Chunks", MONITOR_VISITED_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Changed Paint Chunks", MONITOR_CHANGED_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Immutable Command References", MONITOR_IMMUTABLE_COMMAND_REFERENCES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Flat Command References", MONITOR_FLAT_COMMAND_REFERENCES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Segments", MONITOR_SEMANTIC_SEGMENTS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/New Scope Commands", MONITOR_NEW_SCOPE_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/New Scrollbar Commands", MONITOR_NEW_SCROLLBAR_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Snapshot Entries Written", MONITOR_SNAPSHOT_ENTRIES_WRITTEN, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Retained Transform Layer Hits", MONITOR_RETAINED_TRANSFORM_LAYER_HITS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Retained Transform Layer Rasters", MONITOR_RETAINED_TRANSFORM_LAYER_RASTERS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Texture Resource Creates", MONITOR_TEXTURE_RESOURCE_CREATES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Texture Resource Frees", MONITOR_TEXTURE_RESOURCE_FREES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Presentation Lock Busy", MONITOR_PRESENTATION_LOCK_BUSY, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Capacity Probe Cancellations", MONITOR_CAPACITY_PROBE_CANCELLATIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/CPU Primary Publication Time", MONITOR_CPU_PRIMARY_PUBLICATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/CPU Secondary Publication Time", MONITOR_CPU_SECONDARY_PUBLICATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/CPU Primary Conversion Time", MONITOR_CPU_PRIMARY_CONVERSION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/CPU Primary Upload Time", MONITOR_CPU_PRIMARY_UPLOAD_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/CPU Secondary Conversion Time", MONITOR_CPU_SECONDARY_CONVERSION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/CPU Secondary Upload Time", MONITOR_CPU_SECONDARY_UPLOAD_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Managed Export Boundary Overhead Time", MONITOR_MANAGED_EXPORT_BOUNDARY_OVERHEAD_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Semantic Worker Mailbox Delay Time", MONITOR_SEMANTIC_WORKER_MAILBOX_DELAY_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Semantic Worker Supersessions", MONITOR_SEMANTIC_WORKER_SUPERSESSIONS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Worker Host Call Time", MONITOR_SEMANTIC_WORKER_HOST_CALL_TIME, Performance::MONITOR_TYPE_TIME },
	};

	for (const HCSRMonitorDefinition &definition : definitions) {
		const StringName name(definition.name);
		if (!performance->has_custom_monitor(name)) {
			performance->add_custom_monitor(name, callable_mp_static(&HCSRPerformanceMonitor::_read_monitor).bind((int)definition.monitor), Vector<Variant>(), definition.type);
		}
	}
}

void HCSRPerformanceMonitor::finalize() {
	Performance *performance = Performance::get_singleton();
	if (performance != nullptr) {
		static const char *monitor_names[] = {
			"HCSR/Frame Time",
			"HCSR/Core Pipeline Time",
			"HCSR/Parse Time",
			"HCSR/Style Time",
			"HCSR/Layout Time",
			"HCSR/Hit Test Time",
			"HCSR/Interaction Time",
			"HCSR/Display List Time",
			"HCSR/Raster Time",
			"HCSR/GPU Submit Time",
			"HCSR/Retained Checkpoint Capture Time",
			"HCSR/Retained Checkpoint Restore Time",
			"HCSR/Managed Allocation",
			"HCSR/Display Commands",
			"HCSR/Paint Chunks",
			"HCSR/Paint Chunks Reused",
			"HCSR/Paint Chunks Rebuilt",
			"HCSR/Updated Pixels",
			"HCSR/Input To Visible Time",
			"HCSR/Input To Composed Time",
			"HCSR/Resolved Updates",
			"HCSR/Draw Batches",
			"HCSR/Clear Operations",
			"HCSR/Copy Operations",
			"HCSR/Resolved Copied Bytes",
			"HCSR/Executed Display Commands",
			"HCSR/Executed Glyphs",
			"HCSR/GPU Dispatches",
			"HCSR/Semantic Preparation Time",
			"HCSR/Semantic Snapshot Validation Time",
			"HCSR/Semantic Snapshots Reused",
			"HCSR/Semantic Snapshots Recreated",
			"HCSR/Translated Packets",
			"HCSR/Translated Commands",
			"HCSR/Translation Allocated Bytes",
			"HCSR/Physical Compilation Time",
			"HCSR/Physical Compilation Allocated Bytes",
			"HCSR/Physical Projection Time",
			"HCSR/Painter Order Planning Time",
			"HCSR/Physical Instance Expansion Time",
			"HCSR/Record And Submit Time",
			"HCSR/Painter Order Intersection Tests",
			"HCSR/Painter Order Flushes",
			"HCSR/Runtime Allocated Bytes",
			"HCSR/Runtime Allocation Rate MBps",
			"HCSR/Runtime GC Pause Time",
			"HCSR/Runtime GC Pause Observed Collections",
			"HCSR/Runtime GC Pause Unobserved Collections",
			"HCSR/Runtime Gen0 Collections",
			"HCSR/Runtime Gen1 Collections",
			"HCSR/Runtime Gen2 Collections",
			"HCSR/Runtime Managed Heap Bytes",
			"HCSR/Runtime Managed Committed Bytes",
			"HCSR/Runtime LOH Bytes",
			"HCSR/Runtime LOH Fragmentation Bytes",
			"HCSR/Input State Application Time",
			"HCSR/Snapshot Publication Time",
			"HCSR/Completion Retirement Time",
			"HCSR/Owner Lock Wait Time",
			"HCSR/Owner Lock Contentions",
			"HCSR/Style Allocated Bytes",
			"HCSR/Layout Allocated Bytes",
			"HCSR/Layout Tree Allocated Bytes",
			"HCSR/Text Shaping Allocated Bytes",
			"HCSR/Hit Test Allocated Bytes",
			"HCSR/Display List Allocated Bytes",
			"HCSR/Paint Preparation Allocated Bytes",
			"HCSR/Paint Traversal Allocated Bytes",
			"HCSR/Paint Scene Allocated Bytes",
			"HCSR/Paint Candidate Index Allocated Bytes",
			"HCSR/Complete Frame Materialization Allocated Bytes",
			"HCSR/Complete Sequence Allocated Bytes",
			"HCSR/Semantic Snapshot Allocated Bytes",
			"HCSR/Raster Allocated Bytes",
			"HCSR/Authoritative Paint Chunks",
			"HCSR/Visited Paint Chunks",
			"HCSR/Changed Paint Chunks",
			"HCSR/Immutable Command References",
			"HCSR/Flat Command References",
			"HCSR/Semantic Segments",
			"HCSR/New Scope Commands",
			"HCSR/New Scrollbar Commands",
			"HCSR/Snapshot Entries Written",
			"HCSR/Retained Transform Layer Hits",
			"HCSR/Retained Transform Layer Rasters",
			"HCSR/Texture Resource Creates",
			"HCSR/Texture Resource Frees",
			"HCSR/Presentation Lock Busy",
			"HCSR/Capacity Probe Cancellations",
			"HCSR/CPU Primary Publication Time",
			"HCSR/CPU Secondary Publication Time",
			"HCSR/CPU Primary Conversion Time",
			"HCSR/CPU Primary Upload Time",
			"HCSR/CPU Secondary Conversion Time",
			"HCSR/CPU Secondary Upload Time",
			"HCSR/Managed Export Boundary Overhead Time",
			"HCSR/Semantic Worker Mailbox Delay Time",
			"HCSR/Semantic Worker Supersessions",
			"HCSR/Semantic Worker Host Call Time",
		};
		for (const char *monitor_name : monitor_names) {
			const StringName name(monitor_name);
			if (performance->has_custom_monitor(name)) {
				performance->remove_custom_monitor(name);
			}
		}
	}

	MutexLock lock(mutex);
	profiles.clear();
	integration_counters.clear();
	pending_profiler_profiles.clear();
	pending_input_to_visible_milliseconds.clear();
	pending_input_to_composed_milliseconds.clear();
	latest_input_to_visible_milliseconds = 0.0;
	latest_input_to_composed_milliseconds = 0.0;
}

void HCSRPerformanceMonitor::record_input_to_visible(double p_milliseconds) {
	MutexLock lock(mutex);
	latest_input_to_visible_milliseconds = p_milliseconds;
	pending_input_to_visible_milliseconds.push_back(p_milliseconds);
}

void HCSRPerformanceMonitor::record_input_to_composed(double p_milliseconds) {
	MutexLock lock(mutex);
	latest_input_to_composed_milliseconds = p_milliseconds;
	pending_input_to_composed_milliseconds.push_back(p_milliseconds);
}

void HCSRPerformanceMonitor::update(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile) {
	{
		MutexLock lock(mutex);
		profiles.insert(p_instance_id, p_profile);
		pending_profiler_profiles.push_back(p_profile);
	}
}

void HCSRPerformanceMonitor::update_integration(uint64_t p_instance_id, const IntegrationCounters &p_counters) {
	MutexLock lock(mutex);
	integration_counters.insert(p_instance_id, p_counters);
}

void HCSRPerformanceMonitor::publish_frame_data() {
	if (!EngineDebugger::is_profiling("servers")) {
		MutexLock lock(mutex);
		pending_profiler_profiles.clear();
		pending_input_to_visible_milliseconds.clear();
		pending_input_to_composed_milliseconds.clear();
		return;
	}

	Vector<hcsr_performance_profile_t> frame_profiles;
	Vector<double> frame_input_to_visible_milliseconds;
	Vector<double> frame_input_to_composed_milliseconds;
	{
		MutexLock lock(mutex);
		frame_profiles = pending_profiler_profiles;
		pending_profiler_profiles.clear();
		frame_input_to_visible_milliseconds = pending_input_to_visible_milliseconds;
		pending_input_to_visible_milliseconds.clear();
		frame_input_to_composed_milliseconds = pending_input_to_composed_milliseconds;
		pending_input_to_composed_milliseconds.clear();
	}
	if (frame_profiles.is_empty() && frame_input_to_visible_milliseconds.is_empty() && frame_input_to_composed_milliseconds.is_empty()) {
		return;
	}

	hcsr_performance_profile_t frame_profile = {};
	for (const hcsr_performance_profile_t &profile : frame_profiles) {
		frame_profile.native_total_milliseconds += profile.native_total_milliseconds;
		frame_profile.core_total_milliseconds += profile.core_total_milliseconds;
		frame_profile.parse_milliseconds += profile.parse_milliseconds;
		frame_profile.style_milliseconds += profile.style_milliseconds;
		frame_profile.layout_milliseconds += profile.layout_milliseconds;
		frame_profile.hit_test_milliseconds += profile.hit_test_milliseconds;
		frame_profile.interaction_milliseconds += profile.interaction_milliseconds;
		frame_profile.display_list_milliseconds += profile.display_list_milliseconds;
		frame_profile.raster_milliseconds += profile.raster_milliseconds;
		frame_profile.gpu_submit_milliseconds += profile.gpu_submit_milliseconds;
		frame_profile.presentation_checkpoint_capture_milliseconds += profile.presentation_checkpoint_capture_milliseconds;
		frame_profile.presentation_checkpoint_restore_milliseconds += profile.presentation_checkpoint_restore_milliseconds;
		frame_profile.semantic_preparation_milliseconds += profile.semantic_preparation_milliseconds;
		frame_profile.semantic_snapshot_validation_milliseconds += profile.semantic_snapshot_validation_milliseconds;
		frame_profile.physical_compilation_milliseconds += profile.physical_compilation_milliseconds;
		frame_profile.physical_projection_milliseconds += profile.physical_projection_milliseconds;
		frame_profile.painter_order_planning_milliseconds += profile.painter_order_planning_milliseconds;
		frame_profile.physical_instance_expansion_milliseconds += profile.physical_instance_expansion_milliseconds;
		frame_profile.record_and_submit_milliseconds += profile.record_and_submit_milliseconds;
		frame_profile.input_state_application_milliseconds += profile.input_state_application_milliseconds;
		frame_profile.snapshot_publication_milliseconds += profile.snapshot_publication_milliseconds;
		frame_profile.completion_retirement_milliseconds += profile.completion_retirement_milliseconds;
		frame_profile.owner_lock_wait_milliseconds += profile.owner_lock_wait_milliseconds;
		frame_profile.style_allocated_bytes += profile.style_allocated_bytes;
		frame_profile.layout_allocated_bytes += profile.layout_allocated_bytes;
		frame_profile.layout_tree_allocated_bytes += profile.layout_tree_allocated_bytes;
		frame_profile.layout_text_shaping_allocated_bytes += profile.layout_text_shaping_allocated_bytes;
		frame_profile.hit_test_allocated_bytes += profile.hit_test_allocated_bytes;
		frame_profile.display_list_allocated_bytes += profile.display_list_allocated_bytes;
		frame_profile.paint_preparation_allocated_bytes += profile.paint_preparation_allocated_bytes;
		frame_profile.paint_traversal_allocated_bytes += profile.paint_traversal_allocated_bytes;
		frame_profile.paint_scene_construction_allocated_bytes += profile.paint_scene_construction_allocated_bytes;
		frame_profile.paint_candidate_index_allocated_bytes += profile.paint_candidate_index_allocated_bytes;
		frame_profile.complete_frame_materialization_allocated_bytes += profile.complete_frame_materialization_allocated_bytes;
		frame_profile.complete_sequence_construction_allocated_bytes += profile.complete_sequence_construction_allocated_bytes;
		frame_profile.semantic_snapshot_allocated_bytes += profile.semantic_snapshot_allocated_bytes;
		frame_profile.raster_allocated_bytes += profile.raster_allocated_bytes;
		frame_profile.authoritative_paint_chunk_count += profile.authoritative_paint_chunk_count;
		frame_profile.visited_paint_chunk_count += profile.visited_paint_chunk_count;
		frame_profile.changed_paint_chunk_identity_count += profile.changed_paint_chunk_identity_count;
		frame_profile.immutable_command_reference_count += profile.immutable_command_reference_count;
		frame_profile.flat_command_reference_count += profile.flat_command_reference_count;
		frame_profile.semantic_segment_count += profile.semantic_segment_count;
		frame_profile.new_scope_command_count += profile.new_scope_command_count;
		frame_profile.new_scrollbar_command_count += profile.new_scrollbar_command_count;
		frame_profile.snapshot_array_entry_write_count += profile.snapshot_array_entry_write_count;
		frame_profile.retained_transform_layer_hit_count += profile.retained_transform_layer_hit_count;
		frame_profile.retained_transform_layer_raster_count += profile.retained_transform_layer_raster_count;
	}

	const double measured_core_stage_milliseconds = frame_profile.parse_milliseconds
			+ frame_profile.style_milliseconds
			+ frame_profile.layout_milliseconds
			+ frame_profile.hit_test_milliseconds
			+ frame_profile.interaction_milliseconds
			+ frame_profile.display_list_milliseconds
			+ frame_profile.raster_milliseconds;
	const double unclassified_core_milliseconds = MAX(0.0, frame_profile.core_total_milliseconds - measured_core_stage_milliseconds);
	const double unclassified_native_milliseconds = MAX(0.0,
			frame_profile.native_total_milliseconds
					- frame_profile.core_total_milliseconds
					- frame_profile.gpu_submit_milliseconds
					- frame_profile.presentation_checkpoint_capture_milliseconds
					- frame_profile.presentation_checkpoint_restore_milliseconds);
	Array values;
	values.push_back("hcsr");
	values.push_back("parse");
	values.push_back(frame_profile.parse_milliseconds / 1000.0);
	values.push_back("style");
	values.push_back(frame_profile.style_milliseconds / 1000.0);
	values.push_back("layout");
	values.push_back(frame_profile.layout_milliseconds / 1000.0);
	values.push_back("hit_test");
	values.push_back(frame_profile.hit_test_milliseconds / 1000.0);
	values.push_back("interaction");
	values.push_back(frame_profile.interaction_milliseconds / 1000.0);
	values.push_back("display_list");
	values.push_back(frame_profile.display_list_milliseconds / 1000.0);
	values.push_back("raster");
	values.push_back(frame_profile.raster_milliseconds / 1000.0);
	values.push_back("core_unclassified");
	values.push_back(unclassified_core_milliseconds / 1000.0);
	values.push_back("gpu_submit");
	values.push_back(frame_profile.gpu_submit_milliseconds / 1000.0);
	values.push_back("presentation_checkpoint_capture");
	values.push_back(frame_profile.presentation_checkpoint_capture_milliseconds / 1000.0);
	values.push_back("presentation_checkpoint_restore");
	values.push_back(frame_profile.presentation_checkpoint_restore_milliseconds / 1000.0);
	values.push_back("native_unclassified");
	values.push_back(unclassified_native_milliseconds / 1000.0);
	values.push_back("semantic_preparation");
	values.push_back(frame_profile.semantic_preparation_milliseconds / 1000.0);
	values.push_back("semantic_snapshot_validation");
	values.push_back(frame_profile.semantic_snapshot_validation_milliseconds / 1000.0);
	values.push_back("physical_compilation");
	values.push_back(frame_profile.physical_compilation_milliseconds / 1000.0);
	values.push_back("physical_projection");
	values.push_back(frame_profile.physical_projection_milliseconds / 1000.0);
	values.push_back("painter_order_planning");
	values.push_back(frame_profile.painter_order_planning_milliseconds / 1000.0);
	values.push_back("physical_instance_expansion");
	values.push_back(frame_profile.physical_instance_expansion_milliseconds / 1000.0);
	values.push_back("record_and_submit");
	values.push_back(frame_profile.record_and_submit_milliseconds / 1000.0);
	values.push_back("input_state_application");
	values.push_back(frame_profile.input_state_application_milliseconds / 1000.0);
	values.push_back("snapshot_publication");
	values.push_back(frame_profile.snapshot_publication_milliseconds / 1000.0);
	values.push_back("completion_retirement");
	values.push_back(frame_profile.completion_retirement_milliseconds / 1000.0);
	values.push_back("owner_lock_wait");
	values.push_back(frame_profile.owner_lock_wait_milliseconds / 1000.0);
	values.push_back("style_allocated_bytes");
	values.push_back(frame_profile.style_allocated_bytes);
	values.push_back("layout_allocated_bytes");
	values.push_back(frame_profile.layout_allocated_bytes);
	values.push_back("layout_tree_allocated_bytes");
	values.push_back(frame_profile.layout_tree_allocated_bytes);
	values.push_back("text_shaping_allocated_bytes");
	values.push_back(frame_profile.layout_text_shaping_allocated_bytes);
	values.push_back("hit_test_allocated_bytes");
	values.push_back(frame_profile.hit_test_allocated_bytes);
	values.push_back("display_list_allocated_bytes");
	values.push_back(frame_profile.display_list_allocated_bytes);
	values.push_back("paint_preparation_allocated_bytes");
	values.push_back(frame_profile.paint_preparation_allocated_bytes);
	values.push_back("paint_traversal_allocated_bytes");
	values.push_back(frame_profile.paint_traversal_allocated_bytes);
	values.push_back("paint_scene_allocated_bytes");
	values.push_back(frame_profile.paint_scene_construction_allocated_bytes);
	values.push_back("paint_candidate_index_allocated_bytes");
	values.push_back(frame_profile.paint_candidate_index_allocated_bytes);
	values.push_back("complete_frame_materialization_allocated_bytes");
	values.push_back(frame_profile.complete_frame_materialization_allocated_bytes);
	values.push_back("complete_sequence_allocated_bytes");
	values.push_back(frame_profile.complete_sequence_construction_allocated_bytes);
	values.push_back("semantic_snapshot_allocated_bytes");
	values.push_back(frame_profile.semantic_snapshot_allocated_bytes);
	values.push_back("raster_allocated_bytes");
	values.push_back(frame_profile.raster_allocated_bytes);
	values.push_back("authoritative_paint_chunks");
	values.push_back(frame_profile.authoritative_paint_chunk_count);
	values.push_back("visited_paint_chunks");
	values.push_back(frame_profile.visited_paint_chunk_count);
	values.push_back("changed_paint_chunks");
	values.push_back(frame_profile.changed_paint_chunk_identity_count);
	values.push_back("immutable_command_references");
	values.push_back(frame_profile.immutable_command_reference_count);
	values.push_back("flat_command_references");
	values.push_back(frame_profile.flat_command_reference_count);
	values.push_back("semantic_segments");
	values.push_back(frame_profile.semantic_segment_count);
	values.push_back("new_scope_commands");
	values.push_back(frame_profile.new_scope_command_count);
	values.push_back("new_scrollbar_commands");
	values.push_back(frame_profile.new_scrollbar_command_count);
	values.push_back("snapshot_entries_written");
	values.push_back(frame_profile.snapshot_array_entry_write_count);
	values.push_back("retained_transform_layer_hits");
	values.push_back(frame_profile.retained_transform_layer_hit_count);
	values.push_back("retained_transform_layer_rasters");
	values.push_back(frame_profile.retained_transform_layer_raster_count);
	double input_to_visible_milliseconds = 0.0;
	for (double sample : frame_input_to_visible_milliseconds) {
		input_to_visible_milliseconds = MAX(input_to_visible_milliseconds, sample);
	}
	values.push_back("input_to_visible");
	values.push_back(input_to_visible_milliseconds / 1000.0);
	double input_to_composed_milliseconds = 0.0;
	for (double sample : frame_input_to_composed_milliseconds) {
		input_to_composed_milliseconds = MAX(input_to_composed_milliseconds, sample);
	}
	values.push_back("input_to_composed");
	values.push_back(input_to_composed_milliseconds / 1000.0);
	EngineDebugger::profiler_add_frame_data("servers", values);
}

void HCSRPerformanceMonitor::remove(uint64_t p_instance_id) {
	MutexLock lock(mutex);
	profiles.erase(p_instance_id);
	integration_counters.erase(p_instance_id);
}

double HCSRPerformanceMonitor::_read_monitor(int p_monitor) {
	double value = 0.0;
	MutexLock lock(mutex);
	if (p_monitor >= MONITOR_TEXTURE_RESOURCE_CREATES) {
		for (const KeyValue<uint64_t, IntegrationCounters> &entry : integration_counters) {
			switch ((Monitor)p_monitor) {
				case MONITOR_TEXTURE_RESOURCE_CREATES:
					value += entry.value.texture_resource_creates;
					break;
				case MONITOR_TEXTURE_RESOURCE_FREES:
					value += entry.value.texture_resource_frees;
					break;
				case MONITOR_PRESENTATION_LOCK_BUSY:
					value += entry.value.presentation_lock_busy;
					break;
				case MONITOR_CAPACITY_PROBE_CANCELLATIONS:
					value += entry.value.capacity_probe_cancellations;
					break;
				case MONITOR_CPU_PRIMARY_PUBLICATION_TIME:
					value += entry.value.cpu_primary_publication_milliseconds / 1000.0;
					break;
				case MONITOR_CPU_SECONDARY_PUBLICATION_TIME:
					value += entry.value.cpu_secondary_publication_milliseconds / 1000.0;
					break;
				case MONITOR_CPU_PRIMARY_CONVERSION_TIME:
					value += entry.value.cpu_primary_conversion_milliseconds / 1000.0;
					break;
				case MONITOR_CPU_PRIMARY_UPLOAD_TIME:
					value += entry.value.cpu_primary_upload_milliseconds / 1000.0;
					break;
				case MONITOR_CPU_SECONDARY_CONVERSION_TIME:
					value += entry.value.cpu_secondary_conversion_milliseconds / 1000.0;
					break;
				case MONITOR_CPU_SECONDARY_UPLOAD_TIME:
					value += entry.value.cpu_secondary_upload_milliseconds / 1000.0;
					break;
				case MONITOR_MANAGED_EXPORT_BOUNDARY_OVERHEAD_TIME:
					value += entry.value.managed_export_boundary_overhead_milliseconds / 1000.0;
					break;
				case MONITOR_SEMANTIC_WORKER_MAILBOX_DELAY_TIME:
					value += entry.value.semantic_worker_mailbox_delay_milliseconds / 1000.0;
					break;
				case MONITOR_SEMANTIC_WORKER_SUPERSESSIONS:
					value += entry.value.semantic_worker_supersessions;
					break;
				case MONITOR_SEMANTIC_WORKER_HOST_CALL_TIME:
					value += entry.value.semantic_worker_host_call_milliseconds / 1000.0;
					break;
				default:
					break;
			}
		}
		return value;
	}
	for (const KeyValue<uint64_t, hcsr_performance_profile_t> &entry : profiles) {
		const hcsr_performance_profile_t &profile = entry.value;
		switch ((Monitor)p_monitor) {
			case MONITOR_FRAME_TIME:
				value += profile.native_total_milliseconds / 1000.0;
				break;
			case MONITOR_CORE_TIME:
				value += profile.core_total_milliseconds / 1000.0;
				break;
			case MONITOR_PARSE_TIME:
				value += profile.parse_milliseconds / 1000.0;
				break;
			case MONITOR_STYLE_TIME:
				value += profile.style_milliseconds / 1000.0;
				break;
			case MONITOR_LAYOUT_TIME:
				value += profile.layout_milliseconds / 1000.0;
				break;
			case MONITOR_HIT_TEST_TIME:
				value += profile.hit_test_milliseconds / 1000.0;
				break;
			case MONITOR_INTERACTION_TIME:
				value += profile.interaction_milliseconds / 1000.0;
				break;
			case MONITOR_DISPLAY_LIST_TIME:
				value += profile.display_list_milliseconds / 1000.0;
				break;
			case MONITOR_RASTER_TIME:
				value += profile.raster_milliseconds / 1000.0;
				break;
			case MONITOR_GPU_SUBMIT_TIME:
				value += profile.gpu_submit_milliseconds / 1000.0;
				break;
			case MONITOR_PRESENTATION_CHECKPOINT_CAPTURE_TIME:
				value += profile.presentation_checkpoint_capture_milliseconds / 1000.0;
				break;
			case MONITOR_PRESENTATION_CHECKPOINT_RESTORE_TIME:
				value += profile.presentation_checkpoint_restore_milliseconds / 1000.0;
				break;
			case MONITOR_ALLOCATED_BYTES:
				value += profile.allocated_bytes;
				break;
			case MONITOR_DISPLAY_COMMANDS:
				value += profile.display_command_count;
				break;
			case MONITOR_PAINT_CHUNKS:
				value += profile.paint_chunk_count;
				break;
			case MONITOR_PAINT_CHUNKS_REUSED:
				value += profile.paint_chunk_reused_count;
				break;
			case MONITOR_PAINT_CHUNKS_REBUILT:
				value += profile.paint_chunk_rebuilt_count;
				break;
			case MONITOR_UPDATED_PIXELS:
				value += profile.surface_updated_pixel_area;
				break;
			case MONITOR_INPUT_TO_VISIBLE_TIME:
				value = latest_input_to_visible_milliseconds / 1000.0;
				break;
			case MONITOR_INPUT_TO_COMPOSED_TIME:
				value = latest_input_to_composed_milliseconds / 1000.0;
				break;
			case MONITOR_RESOLVED_UPDATES:
				value += profile.resolved_update_count;
				break;
			case MONITOR_DRAW_BATCHES:
				value += profile.draw_batch_count;
				break;
			case MONITOR_CLEAR_OPERATIONS:
				value += profile.clear_operation_count;
				break;
			case MONITOR_COPY_OPERATIONS:
				value += profile.copy_operation_count;
				break;
			case MONITOR_RESOLVED_COPIED_BYTES:
				value += profile.resolved_copied_bytes;
				break;
			case MONITOR_EXECUTED_DISPLAY_COMMANDS:
				value += profile.executed_display_command_count;
				break;
			case MONITOR_EXECUTED_GLYPHS:
				value += profile.executed_glyph_count;
				break;
			case MONITOR_GPU_DISPATCHES:
				value += profile.gpu_dispatch_count;
				break;
			case MONITOR_SEMANTIC_PREPARATION_TIME:
				value += profile.semantic_preparation_milliseconds / 1000.0;
				break;
			case MONITOR_SEMANTIC_SNAPSHOT_VALIDATION_TIME:
				value += profile.semantic_snapshot_validation_milliseconds / 1000.0;
				break;
			case MONITOR_SEMANTIC_SNAPSHOTS_REUSED:
				value += profile.semantic_snapshot_reused_count;
				break;
			case MONITOR_SEMANTIC_SNAPSHOTS_RECREATED:
				value += profile.semantic_snapshot_recreated_count;
				break;
			case MONITOR_TRANSLATED_PACKETS:
				value += profile.translated_packet_count;
				break;
			case MONITOR_TRANSLATED_COMMANDS:
				value += profile.translated_command_count;
				break;
			case MONITOR_TRANSLATION_ALLOCATED_BYTES:
				value += profile.translation_allocated_bytes;
				break;
			case MONITOR_PHYSICAL_COMPILATION_TIME:
				value += profile.physical_compilation_milliseconds / 1000.0;
				break;
			case MONITOR_PHYSICAL_COMPILATION_ALLOCATED_BYTES:
				value += profile.physical_compilation_allocated_bytes;
				break;
			case MONITOR_PHYSICAL_PROJECTION_TIME:
				value += profile.physical_projection_milliseconds / 1000.0;
				break;
			case MONITOR_PAINTER_ORDER_PLANNING_TIME:
				value += profile.painter_order_planning_milliseconds / 1000.0;
				break;
			case MONITOR_PHYSICAL_INSTANCE_EXPANSION_TIME:
				value += profile.physical_instance_expansion_milliseconds / 1000.0;
				break;
			case MONITOR_RECORD_AND_SUBMIT_TIME:
				value += profile.record_and_submit_milliseconds / 1000.0;
				break;
			case MONITOR_PAINTER_ORDER_INTERSECTION_TESTS:
				value += profile.painter_order_intersection_test_count;
				break;
			case MONITOR_PAINTER_ORDER_FLUSHES:
				value += profile.painter_order_flush_count;
				break;
			case MONITOR_RUNTIME_ALLOCATED_BYTES:
				value += profile.runtime_allocated_bytes;
				break;
			case MONITOR_RUNTIME_ALLOCATION_RATE:
				value += profile.runtime_allocation_rate_megabytes_per_second;
				break;
			case MONITOR_RUNTIME_GC_PAUSE_TIME:
				value += profile.runtime_gc_pause_milliseconds / 1000.0;
				break;
			case MONITOR_RUNTIME_GC_PAUSE_OBSERVED_COLLECTIONS:
				value += profile.runtime_gc_pause_observed_collection_count;
				break;
			case MONITOR_RUNTIME_GC_PAUSE_UNOBSERVED_COLLECTIONS:
				value += profile.runtime_gc_pause_unobserved_collection_count;
				break;
			case MONITOR_RUNTIME_GEN0_COLLECTIONS:
				value += profile.runtime_gen0_collection_count;
				break;
			case MONITOR_RUNTIME_GEN1_COLLECTIONS:
				value += profile.runtime_gen1_collection_count;
				break;
			case MONITOR_RUNTIME_GEN2_COLLECTIONS:
				value += profile.runtime_gen2_collection_count;
				break;
			case MONITOR_RUNTIME_MANAGED_HEAP_BYTES:
				value += profile.runtime_managed_heap_bytes;
				break;
			case MONITOR_RUNTIME_MANAGED_COMMITTED_BYTES:
				value += profile.runtime_managed_committed_bytes;
				break;
			case MONITOR_RUNTIME_LOH_BYTES:
				value += profile.runtime_large_object_heap_bytes;
				break;
			case MONITOR_RUNTIME_LOH_FRAGMENTATION_BYTES:
				value += profile.runtime_large_object_heap_fragmentation_bytes;
				break;
			case MONITOR_INPUT_STATE_APPLICATION_TIME:
				value += profile.input_state_application_milliseconds / 1000.0;
				break;
			case MONITOR_SNAPSHOT_PUBLICATION_TIME:
				value += profile.snapshot_publication_milliseconds / 1000.0;
				break;
			case MONITOR_COMPLETION_RETIREMENT_TIME:
				value += profile.completion_retirement_milliseconds / 1000.0;
				break;
			case MONITOR_OWNER_LOCK_WAIT_TIME:
				value += profile.owner_lock_wait_milliseconds / 1000.0;
				break;
			case MONITOR_OWNER_LOCK_CONTENTIONS:
				value += profile.owner_lock_contention_count;
				break;
			case MONITOR_STYLE_ALLOCATED_BYTES:
				value += profile.style_allocated_bytes;
				break;
			case MONITOR_LAYOUT_ALLOCATED_BYTES:
				value += profile.layout_allocated_bytes;
				break;
			case MONITOR_LAYOUT_TREE_ALLOCATED_BYTES:
				value += profile.layout_tree_allocated_bytes;
				break;
			case MONITOR_LAYOUT_TEXT_SHAPING_ALLOCATED_BYTES:
				value += profile.layout_text_shaping_allocated_bytes;
				break;
			case MONITOR_HIT_TEST_ALLOCATED_BYTES:
				value += profile.hit_test_allocated_bytes;
				break;
			case MONITOR_DISPLAY_LIST_ALLOCATED_BYTES:
				value += profile.display_list_allocated_bytes;
				break;
			case MONITOR_PAINT_PREPARATION_ALLOCATED_BYTES:
				value += profile.paint_preparation_allocated_bytes;
				break;
			case MONITOR_PAINT_TRAVERSAL_ALLOCATED_BYTES:
				value += profile.paint_traversal_allocated_bytes;
				break;
			case MONITOR_PAINT_SCENE_CONSTRUCTION_ALLOCATED_BYTES:
				value += profile.paint_scene_construction_allocated_bytes;
				break;
			case MONITOR_PAINT_CANDIDATE_INDEX_ALLOCATED_BYTES:
				value += profile.paint_candidate_index_allocated_bytes;
				break;
			case MONITOR_COMPLETE_FRAME_MATERIALIZATION_ALLOCATED_BYTES:
				value += profile.complete_frame_materialization_allocated_bytes;
				break;
			case MONITOR_COMPLETE_SEQUENCE_CONSTRUCTION_ALLOCATED_BYTES:
				value += profile.complete_sequence_construction_allocated_bytes;
				break;
			case MONITOR_SEMANTIC_SNAPSHOT_ALLOCATED_BYTES:
				value += profile.semantic_snapshot_allocated_bytes;
				break;
			case MONITOR_RASTER_ALLOCATED_BYTES:
				value += profile.raster_allocated_bytes;
				break;
			case MONITOR_AUTHORITATIVE_PAINT_CHUNKS:
				value += profile.authoritative_paint_chunk_count;
				break;
			case MONITOR_VISITED_PAINT_CHUNKS:
				value += profile.visited_paint_chunk_count;
				break;
			case MONITOR_CHANGED_PAINT_CHUNKS:
				value += profile.changed_paint_chunk_identity_count;
				break;
			case MONITOR_IMMUTABLE_COMMAND_REFERENCES:
				value += profile.immutable_command_reference_count;
				break;
			case MONITOR_FLAT_COMMAND_REFERENCES:
				value += profile.flat_command_reference_count;
				break;
			case MONITOR_SEMANTIC_SEGMENTS:
				value += profile.semantic_segment_count;
				break;
			case MONITOR_NEW_SCOPE_COMMANDS:
				value += profile.new_scope_command_count;
				break;
			case MONITOR_NEW_SCROLLBAR_COMMANDS:
				value += profile.new_scrollbar_command_count;
				break;
			case MONITOR_SNAPSHOT_ENTRIES_WRITTEN:
				value += profile.snapshot_array_entry_write_count;
				break;
			case MONITOR_RETAINED_TRANSFORM_LAYER_HITS:
				value += profile.retained_transform_layer_hit_count;
				break;
			case MONITOR_RETAINED_TRANSFORM_LAYER_RASTERS:
				value += profile.retained_transform_layer_raster_count;
				break;
		}
	}
	return value;
}
