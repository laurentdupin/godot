/**************************************************************************/
/*  hcsr_performance_monitor.cpp                                          */
/**************************************************************************/

#include "hcsr_performance_monitor.h"

#include "core/object/callable_mp.h"
#include "main/performance.h"

Mutex HCSRPerformanceMonitor::mutex;
HashMap<uint64_t, hcsr_performance_profile_t> HCSRPerformanceMonitor::profiles;

struct HCSRMonitorDefinition {
	const char *name;
	HCSRPerformanceMonitor::Monitor monitor;
	Performance::MonitorType type;
};

void HCSRPerformanceMonitor::initialize() {
	Performance *performance = Performance::get_singleton();
	ERR_FAIL_NULL(performance);

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
		{ "HCSR/Managed Allocation", MONITOR_ALLOCATED_BYTES, Performance::MONITOR_TYPE_MEMORY },
		{ "HCSR/Display Commands", MONITOR_DISPLAY_COMMANDS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks", MONITOR_PAINT_CHUNKS, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks Reused", MONITOR_PAINT_CHUNKS_REUSED, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Paint Chunks Rebuilt", MONITOR_PAINT_CHUNKS_REBUILT, Performance::MONITOR_TYPE_QUANTITY },
		{ "HCSR/Updated Pixels", MONITOR_UPDATED_PIXELS, Performance::MONITOR_TYPE_QUANTITY },
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
			"HCSR/Managed Allocation",
			"HCSR/Display Commands",
			"HCSR/Paint Chunks",
			"HCSR/Paint Chunks Reused",
			"HCSR/Paint Chunks Rebuilt",
			"HCSR/Updated Pixels",
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
}

void HCSRPerformanceMonitor::update(uint64_t p_instance_id, const hcsr_performance_profile_t &p_profile) {
	MutexLock lock(mutex);
	profiles.insert(p_instance_id, p_profile);
}

void HCSRPerformanceMonitor::remove(uint64_t p_instance_id) {
	MutexLock lock(mutex);
	profiles.erase(p_instance_id);
}

double HCSRPerformanceMonitor::_read_monitor(int p_monitor) {
	double value = 0.0;
	MutexLock lock(mutex);
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
		}
	}
	return value;
}
