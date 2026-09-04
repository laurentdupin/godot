/**************************************************************************/
/*  hcsr_newest_performance_monitor.cpp                                  */
/**************************************************************************/

#include "hcsr_newest_performance_monitor.h"

#include "core/object/callable_mp.h"
#include "main/performance.h"

Mutex HCSRNewestPerformanceMonitor::mutex;
HashMap<uint64_t, HCSRNewestPerformanceMonitor::Sample> HCSRNewestPerformanceMonitor::samples;

struct HCSRNewestMonitorDefinition {
	const char *name;
	HCSRNewestPerformanceMonitor::Monitor monitor;
	Performance::MonitorType type;
};

void HCSRNewestPerformanceMonitor::initialize() {
	Performance *performance = Performance::get_singleton();
	if (performance == nullptr) return;
	static const HCSRNewestMonitorDefinition definitions[] = {
		{ "HCSR/Frame Time", MONITOR_FRAME_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Core Pipeline Time", MONITOR_CORE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Style Time", MONITOR_STYLE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Layout Time", MONITOR_LAYOUT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Hit Test Time", MONITOR_HIT_TEST_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Interaction Time", MONITOR_INTERACTION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Display List Time", MONITOR_DISPLAY_LIST_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Physical Compilation Time", MONITOR_PHYSICAL_COMPILATION_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Record And Submit Time", MONITOR_RECORD_AND_SUBMIT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/GPU Submit Time", MONITOR_GPU_SUBMIT_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Input To Visible Time", MONITOR_INPUT_TO_VISIBLE_TIME, Performance::MONITOR_TYPE_TIME },
		{ "HCSR/Managed Allocation", MONITOR_MANAGED_ALLOCATION, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Style Allocated Bytes", MONITOR_STYLE_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Layout Allocated Bytes", MONITOR_LAYOUT_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Display List Allocated Bytes", MONITOR_DISPLAY_LIST_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Visited Paint Chunks", MONITOR_VISITED_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Changed Paint Chunks", MONITOR_CHANGED_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks Rebuilt", MONITOR_PAINT_CHUNKS_REBUILT, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Snapshots Reused", MONITOR_SEMANTIC_SNAPSHOTS_REUSED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Semantic Snapshots Recreated", MONITOR_SEMANTIC_SNAPSHOTS_RECREATED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Layout Node Calls", MONITOR_LAYOUT_NODE_CALLS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Distinct Layout Nodes", MONITOR_DISTINCT_LAYOUT_NODES, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Equivalent Layout Reentries", MONITOR_EQUIVALENT_LAYOUT_REENTRIES, Performance::MONITOR_TYPE_QUANTITY },
	};
	for (const HCSRNewestMonitorDefinition &definition : definitions) {
		const StringName name(definition.name);
		if (!performance->has_custom_monitor(name)) {
			performance->add_custom_monitor(name, callable_mp_static(&HCSRNewestPerformanceMonitor::_read_monitor).bind((int)definition.monitor), Vector<Variant>(), definition.type);
		}
	}
}

void HCSRNewestPerformanceMonitor::finalize() {
	Performance *performance = Performance::get_singleton();
	if (performance != nullptr) {
		static const char *names[] = {
			"HCSR/Frame Time", "HCSR/Core Pipeline Time", "HCSR/Style Time", "HCSR/Layout Time",
			"HCSR/Hit Test Time", "HCSR/Interaction Time", "HCSR/Display List Time",
			"HCSR/Physical Compilation Time", "HCSR/Record And Submit Time", "HCSR/GPU Submit Time",
			"HCSR/Input To Visible Time", "HCSR/Managed Allocation", "HCSR/Style Allocated Bytes",
			"HCSR/Layout Allocated Bytes", "HCSR/Display List Allocated Bytes", "HCSR/Visited Paint Chunks",
			"HCSR/Changed Paint Chunks", "HCSR/Paint Chunks Rebuilt", "HCSR/Semantic Snapshots Reused",
			"HCSR/Semantic Snapshots Recreated", "HCSR/Layout Node Calls", "HCSR/Distinct Layout Nodes",
			"HCSR/Equivalent Layout Reentries",
		};
		for (const char *name : names) {
			const StringName monitor(name);
			if (performance->has_custom_monitor(monitor)) performance->remove_custom_monitor(monitor);
		}
	}
	MutexLock lock(mutex);
	samples.clear();
}

void HCSRNewestPerformanceMonitor::update_scene(uint64_t p_instance_id, const hcsr_scene_profile_t &p_profile) {
	MutexLock lock(mutex);
	Sample &sample = samples[p_instance_id];
	sample.scene = p_profile;
}

void HCSRNewestPerformanceMonitor::update_presentation(uint64_t p_instance_id, double p_record_and_submit_seconds, double p_input_to_visible_seconds) {
	MutexLock lock(mutex);
	Sample &sample = samples[p_instance_id];
	sample.record_and_submit_seconds = p_record_and_submit_seconds;
	sample.input_to_visible_seconds = p_input_to_visible_seconds;
}

void HCSRNewestPerformanceMonitor::remove(uint64_t p_instance_id) {
	MutexLock lock(mutex);
	samples.erase(p_instance_id);
}

double HCSRNewestPerformanceMonitor::_read_monitor(int p_monitor) {
	MutexLock lock(mutex);
	double value = 0.0;
	for (const KeyValue<uint64_t, Sample> &entry : samples) {
		const Sample &sample = entry.value;
		switch ((Monitor)p_monitor) {
			case MONITOR_FRAME_TIME: value += sample.scene.total_seconds + sample.record_and_submit_seconds; break;
			case MONITOR_CORE_TIME: value += sample.scene.total_seconds; break;
			case MONITOR_STYLE_TIME: value += sample.scene.style_seconds; break;
			case MONITOR_LAYOUT_TIME: value += sample.scene.layout_seconds; break;
			case MONITOR_INTERACTION_TIME: value += sample.scene.input_seconds; break;
			case MONITOR_DISPLAY_LIST_TIME: value += sample.scene.packet_seconds; break;
			case MONITOR_RECORD_AND_SUBMIT_TIME: value += sample.record_and_submit_seconds; break;
			case MONITOR_INPUT_TO_VISIBLE_TIME: value += sample.input_to_visible_seconds; break;
			case MONITOR_MANAGED_ALLOCATION: value += sample.scene.allocated_bytes; break;
			case MONITOR_LAYOUT_NODE_CALLS: value += sample.scene.layout_node_calls; break;
			case MONITOR_DISTINCT_LAYOUT_NODES: value += sample.scene.distinct_layout_nodes; break;
			case MONITOR_EQUIVALENT_LAYOUT_REENTRIES: value += sample.scene.equivalent_constraint_reentries; break;
			default: break; // Unsupported retained-renderer counters are truthfully zero.
		}
	}
	return value;
}
