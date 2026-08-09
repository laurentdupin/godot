/**************************************************************************/
/*  test_hcsr_performance_monitor.h                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef HTML_CSS_HCSR_TESTS_ENABLED

#include "../backend/hcsr_performance_monitor.h"

#include "tests/test_macros.h"

namespace TestHCSRPerformanceMonitor {

TEST_CASE("[HTMLCSS][HCSR][Profiler] staged snapshots publish once per generation as time-only data") {
	constexpr uint64_t instance_id = 123;
	constexpr uint64_t generation = 7;
	HCSRPerformanceProfileStore store;

	hcsr_performance_profile_t semantic_profile = {};
	semantic_profile.struct_size = sizeof(semantic_profile);
	semantic_profile.native_total_milliseconds = 15.06;
	semantic_profile.core_total_milliseconds = 14.06;
	semantic_profile.layout_milliseconds = 14.06;
	semantic_profile.style_allocated_bytes = 238840;
	store.update_latest(instance_id, semantic_profile);
	CHECK(store.take_pending_generation_samples().is_empty());

	hcsr_performance_profile_t submission_profile = semantic_profile;
	submission_profile.native_total_milliseconds = 35.47;
	submission_profile.gpu_submit_milliseconds = 18.41;
	submission_profile.presentation_checkpoint_capture_milliseconds = 1.0;
	submission_profile.presentation_checkpoint_restore_milliseconds = 0.5;
	submission_profile.record_and_submit_milliseconds = 17.88;
	submission_profile.draw_batch_count = 4321;
	CHECK(store.complete_generation(instance_id, generation, submission_profile));

	hcsr_performance_profile_t completion_profile = submission_profile;
	completion_profile.completion_retirement_milliseconds = 0.25;
	store.update_latest(instance_id, completion_profile);
	CHECK_FALSE(store.complete_generation(instance_id, generation, completion_profile));

	Vector<HCSRPerformanceProfileStore::GenerationSample> samples = store.take_pending_generation_samples();
	REQUIRE(samples.size() == 1);
	CHECK(samples[0].instance_id == instance_id);
	CHECK(samples[0].generation == generation);
	CHECK(samples[0].profile.completion_retirement_milliseconds == 0.0);
	CHECK(samples[0].profile.record_and_submit_milliseconds == doctest::Approx(17.88));
	CHECK(store.take_pending_generation_samples().is_empty());

	const hcsr_performance_profile_t *latest_profile = store.get_latest_profiles().getptr(instance_id);
	REQUIRE(latest_profile != nullptr);
	CHECK(latest_profile->completion_retirement_milliseconds == doctest::Approx(0.25));

	struct ExpectedValue {
		const char *name;
		double seconds;
	};
	static const ExpectedValue expected_values[] = {
		{ "parse", 0.0 },
		{ "style", 0.0 },
		{ "layout", 0.01406 },
		{ "hit_test", 0.0 },
		{ "interaction", 0.0 },
		{ "display_list", 0.0 },
		{ "raster", 0.0 },
		{ "core_unclassified", 0.0 },
		{ "gpu_submit", 0.01841 },
		{ "presentation_checkpoint_capture", 0.001 },
		{ "presentation_checkpoint_restore", 0.0005 },
		{ "native_unclassified", 0.0015 },
	};
	constexpr int expected_value_count = sizeof(expected_values) / sizeof(expected_values[0]);

	const Array payload = HCSRPerformanceMonitor::build_profiler_frame_data(samples);
	REQUIRE(payload.size() == 1 + expected_value_count * 2);
	CHECK(payload[0].get_type() == Variant::STRING);
	CHECK(String(payload[0]) == "hcsr");
	double category_total_seconds = 0.0;
	for (int index = 0; index < expected_value_count; index++) {
		const int payload_index = 1 + index * 2;
		CHECK(payload[payload_index].get_type() == Variant::STRING);
		CHECK(String(payload[payload_index]) == expected_values[index].name);
		CHECK(payload[payload_index + 1].get_type() == Variant::FLOAT);
		const double seconds = payload[payload_index + 1];
		CHECK(seconds == doctest::Approx(expected_values[index].seconds));
		category_total_seconds += seconds;
	}
	CHECK(category_total_seconds == doctest::Approx(submission_profile.native_total_milliseconds / 1000.0));
}

TEST_CASE("[HTMLCSS][HCSR][Profiler] removing an instance purges its unpublished generations") {
	constexpr uint64_t removed_instance_id = 456;
	constexpr uint64_t retained_instance_id = 789;
	HCSRPerformanceProfileStore store;
	hcsr_performance_profile_t profile = {};
	profile.struct_size = sizeof(profile);
	profile.native_total_milliseconds = 1.0;

	CHECK(store.complete_generation(removed_instance_id, 3, profile));
	CHECK(store.complete_generation(retained_instance_id, 4, profile));
	store.remove(removed_instance_id);

	Vector<HCSRPerformanceProfileStore::GenerationSample> samples = store.take_pending_generation_samples();
	REQUIRE(samples.size() == 1);
	CHECK(samples[0].instance_id == retained_instance_id);
	CHECK(store.get_latest_profiles().getptr(removed_instance_id) == nullptr);
	CHECK(store.complete_generation(removed_instance_id, 1, profile));
}

} // namespace TestHCSRPerformanceMonitor

#endif // HTML_CSS_HCSR_TESTS_ENABLED
