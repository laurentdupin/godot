/**************************************************************************/
/*  html_surface_hcsr_backend.cpp                                        */
/**************************************************************************/

#include "html_surface_hcsr_backend.h"

#include "../bridge/html_asset_provider.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

namespace {

constexpr uint64_t HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC = 1000;
constexpr uint64_t HCSR_RUNTIME_RETIREMENT_BUDGET_USEC = 500;
constexpr int32_t HCSR_RUNTIME_TILE_SIZE = 64;

static String copy_report_text(hcsr_runtime_compilation_report_t *p_report, int p_index, bool p_message, int p_byte_count) {
	if (p_byte_count <= 1) {
		return String();
	}
	Vector<char> bytes;
	bytes.resize(p_byte_count);
	if (hcsr_runtime_compilation_report_copy_diagnostic_text(
			p_report,
			p_index,
			p_message ? 1 : 0,
			bytes.ptrw(),
			bytes.size()) != HCSR_RUNTIME_OK) {
		return String();
	}
	return String::utf8(bytes.ptr(), bytes.size() - 1);
}

} // namespace

void HTMLSurfaceHCSRBackend::_set_terminal_failure(const String &p_message) {
	terminal_failure = true;
	terminal_failure_reason = p_message;
	ERR_PRINT(p_message);
}

bool HTMLSurfaceHCSRBackend::_load_document_source(String &r_html, String &r_css) const {
	if (document.is_null() || !document->is_source_valid()) {
		return false;
	}

	r_html = document->get_html();
	if (r_html.is_empty() && !document->get_html_file().is_empty()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, document->get_html_file(), asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		r_html = String::utf8(reinterpret_cast<const char *>(asset.bytes.ptr()), asset.bytes.size());
	}
	if (r_html.strip_edges().is_empty()) {
		return false;
	}

	r_css = String();
	for (const String &css_file : document->get_css_files()) {
		HTMLAssetResource asset;
		String error;
		if (HTMLGodotAssetProvider::load_asset(document, css_file, asset, &error) != OK) {
			ERR_PRINT(error);
			return false;
		}
		r_css += "\n" + String::utf8(reinterpret_cast<const char *>(asset.bytes.ptr()), asset.bytes.size()) + "\n";
	}
	r_css += document->get_css();
	return true;
}

void HTMLSurfaceHCSRBackend::_report_compilation(hcsr_runtime_compilation_report_t *p_report) {
	if (p_report == nullptr) {
		return;
	}

	hcsr_runtime_compilation_report_info_t report_info;
	_initialize_abi(report_info);
	if (hcsr_runtime_compilation_report_get_info(p_report, &report_info) != HCSR_RUNTIME_OK) {
		ERR_PRINT("HCSR RuntimeSession compilation report could not be inspected.");
		return;
	}

	for (int index = 0; index < report_info.diagnostic_count; index++) {
		hcsr_runtime_compilation_diagnostic_info_t diagnostic;
		_initialize_abi(diagnostic);
		if (hcsr_runtime_compilation_report_get_diagnostic(p_report, index, &diagnostic) != HCSR_RUNTIME_OK) {
			continue;
		}
		const String code = copy_report_text(p_report, index, false, diagnostic.code_utf8_bytes);
		const String message = copy_report_text(p_report, index, true, diagnostic.message_utf8_bytes);
		const String formatted = vformat("HCSR %s at %d:%d: %s", code, diagnostic.line, diagnostic.column, message);
		if (diagnostic.severity == HCSR_RUNTIME_DIAGNOSTIC_ERROR) {
			ERR_PRINT(formatted);
		} else {
			WARN_PRINT(formatted);
		}
	}

	for (int index = 0; index < report_info.resource_intent_count; index++) {
		hcsr_runtime_compilation_resource_info_t resource;
		_initialize_abi(resource);
		if (hcsr_runtime_compilation_report_get_resource(p_report, index, &resource) != HCSR_RUNTIME_OK || resource.reference_utf8_bytes <= 1) {
			continue;
		}
		Vector<char> reference;
		reference.resize(resource.reference_utf8_bytes);
		if (hcsr_runtime_compilation_report_copy_resource_reference(p_report, index, reference.ptrw(), reference.size()) == HCSR_RUNTIME_OK) {
			WARN_PRINT(vformat(
					"HCSR RuntimeSession resource intent kind %d for '%s' is unresolved by the CPU integration slice.",
					(int)resource.kind,
					String::utf8(reference.ptr(), reference.size() - 1)));
		}
	}
}

bool HTMLSurfaceHCSRBackend::_compile_document() {
	if (document.is_null()) {
		document_dirty = false;
		return false;
	}
	if (!document_dirty) {
		return compiled_document != nullptr;
	}
	document_dirty = false;

	String html;
	String css;
	if (!_load_document_source(html, css)) {
		_set_terminal_failure("HCSR RuntimeSession could not load the HTMLDocument source.");
		return false;
	}

	const CharString html_utf8 = html.utf8();
	const CharString css_utf8 = css.utf8();
	hcsr_runtime_document_t *candidate_document = nullptr;
	hcsr_runtime_compilation_report_t *report = nullptr;
	const hcsr_runtime_status_t status = hcsr_runtime_document_compile(
			html_utf8.ptr(),
			css_utf8.ptr(),
			&candidate_document,
			&report);
	_report_compilation(report);
	if (report != nullptr) {
		hcsr_runtime_compilation_report_release(report);
	}
	if (status != HCSR_RUNTIME_OK || candidate_document == nullptr) {
		_set_terminal_failure("HCSR RuntimeSession rejected the HTMLDocument.");
		return false;
	}

	hcsr_runtime_document_t *previous_document = compiled_document;
	compiled_document = candidate_document;
	terminal_failure = false;
	terminal_failure_reason = String();

	if (session != nullptr && !session_configuration_dirty) {
		const uint64_t request_id = next_document_request_id++;
		if (hcsr_runtime_session_submit_document(session, request_id, compiled_document) != HCSR_RUNTIME_OK) {
			compiled_document = previous_document;
			hcsr_runtime_document_release(candidate_document);
			_set_terminal_failure("HCSR RuntimeSession rejected the compiled document submission.");
			return false;
		}
		queued_generation++;
		derivation_pending = true;
	}
	if (previous_document != nullptr) {
		hcsr_runtime_document_release(previous_document);
	}
	return true;
}

void HTMLSurfaceHCSRBackend::_begin_session_retirement() {
	if (session == nullptr) {
		return;
	}
	const hcsr_runtime_status_t status = hcsr_runtime_session_begin_shutdown(session);
	if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_CLOSED) {
		_set_terminal_failure(vformat("HCSR RuntimeSession shutdown failed with status %d.", (int)status));
	}
	retiring_sessions.push_back(session);
	session = nullptr;
	derivation_pending = false;
	consumed_runtime_generation = 0;
}

void HTMLSurfaceHCSRBackend::_service_retirement(uint64_t p_budget_usec) {
	const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	int session_index = 0;
	while (session_index < retiring_sessions.size()) {
		hcsr_runtime_step_info_t info;
		_initialize_abi(info);
		const hcsr_runtime_status_t status = hcsr_runtime_session_step_retirement(retiring_sessions[session_index], 1, &info);
		if (status == HCSR_RUNTIME_CLOSED) {
			if (hcsr_runtime_session_destroy(retiring_sessions[session_index]) != HCSR_RUNTIME_OK) {
				_set_terminal_failure("HCSR RuntimeSession could not destroy a closed session.");
			}
			retiring_sessions.remove_at(session_index);
			continue;
		}
		if (status != HCSR_RUNTIME_PENDING_CLEANUP && status != HCSR_RUNTIME_WAITING_FOR_LEASES) {
			_set_terminal_failure(vformat("HCSR RuntimeSession retirement failed with status %d.", (int)status));
		}
		session_index++;
		if (start_usec != 0 && OS::get_singleton()->get_ticks_usec() - start_usec >= p_budget_usec) {
			break;
		}
	}
}

bool HTMLSurfaceHCSRBackend::_recreate_session() {
	if (!session_configuration_dirty && session != nullptr) {
		return true;
	}
	_begin_session_retirement();

	Vector<hcsr_runtime_output_configuration_t> outputs;
	outputs.resize(1 + presentation_outputs.size());
	_initialize_abi(outputs.write[0]);
	outputs.write[0].output_id = 1;
	outputs.write[0].pixel_width = MAX(1, physical_size.x);
	outputs.write[0].pixel_height = MAX(1, physical_size.y);
	outputs.write[0].logical_width = MAX(1, size.x);
	outputs.write[0].logical_height = MAX(1, size.y);
	outputs.write[0].tile_size = HCSR_RUNTIME_TILE_SIZE;

	int output_index = 1;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		_initialize_abi(outputs.write[output_index]);
		outputs.write[output_index].output_id = entry.value->runtime_output_id;
		outputs.write[output_index].pixel_width = MAX(1, entry.value->requested_size.x);
		outputs.write[output_index].pixel_height = MAX(1, entry.value->requested_size.y);
		outputs.write[output_index].logical_width = MAX(1, size.x);
		outputs.write[output_index].logical_height = MAX(1, size.y);
		outputs.write[output_index].tile_size = HCSR_RUNTIME_TILE_SIZE;
		output_index++;
	}

	const hcsr_runtime_status_t create_status = hcsr_runtime_session_create(
			MAX(1, size.x),
			MAX(1, size.y),
			outputs.ptr(),
			outputs.size(),
			&session);
	if (create_status != HCSR_RUNTIME_OK || session == nullptr) {
		_set_terminal_failure(vformat("HCSR RuntimeSession creation failed with status %d.", (int)create_status));
		return false;
	}

	session_configuration_dirty = false;
	consumed_runtime_generation = 0;
	if (compiled_document != nullptr) {
		const uint64_t request_id = next_document_request_id++;
		if (hcsr_runtime_session_submit_document(session, request_id, compiled_document) != HCSR_RUNTIME_OK) {
			_set_terminal_failure("HCSR RuntimeSession rejected the initial document submission.");
			return false;
		}
		queued_generation++;
		derivation_pending = true;
	}
	return true;
}

bool HTMLSurfaceHCSRBackend::_step_session(uint64_t p_budget_usec) {
	if (session == nullptr || !derivation_pending) {
		return false;
	}
	const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	last_step_work_units = 0;
	bool completed = false;
	uint32_t step_count = 0;
	do {
		hcsr_runtime_step_info_t info;
		_initialize_abi(info);
		const hcsr_runtime_status_t status = hcsr_runtime_session_step(session, 1, &info);
		last_step_work_units += info.work_units;
		if (status == HCSR_RUNTIME_OK) {
			derivation_pending = false;
			completed = true;
			break;
		}
		if (status != HCSR_RUNTIME_PENDING) {
			_set_terminal_failure(vformat("HCSR RuntimeSession frame step failed with status %d.", (int)status));
			derivation_pending = false;
			break;
		}
		step_count++;
	} while ((start_usec != 0 && OS::get_singleton()->get_ticks_usec() - start_usec < p_budget_usec) || (start_usec == 0 && step_count < 1024));
	last_step_milliseconds = start_usec == 0 ? 0.0 : (OS::get_singleton()->get_ticks_usec() - start_usec) / 1000.0;
	return completed;
}

bool HTMLSurfaceHCSRBackend::_apply_output_frame(
		hcsr_runtime_publication_t *p_publication,
		uint64_t p_publication_generation,
		int32_t p_output_index,
		OutputState *p_output_state,
		bool p_primary) {
	hcsr_runtime_frame_t *frame = nullptr;
	hcsr_runtime_frame_info_t frame_info;
	_initialize_abi(frame_info);
	if (hcsr_runtime_publication_acquire_output_frame(
			p_publication,
			p_publication_generation,
			p_output_index,
			&frame,
			&frame_info) != HCSR_RUNTIME_OK || frame == nullptr) {
		return false;
	}

	bool succeeded = frame_info.pixel_format == HCSR_RUNTIME_PIXEL_FORMAT_BGRA32_STRAIGHT_SRGB;
	PackedByteArray &rgba_pixels = p_primary ? primary_rgba_pixels : p_output_state->rgba_pixels;
	uint64_t &copied_tile_bytes = p_primary ? primary_copied_tile_bytes : p_output_state->copied_tile_bytes;
	const int64_t required_bytes = int64_t(frame_info.pixel_width) * frame_info.pixel_height * 4;
	if (succeeded && rgba_pixels.size() != required_bytes) {
		rgba_pixels.resize(required_bytes);
		memset(rgba_pixels.ptrw(), 0, rgba_pixels.size());
	}

	const uint64_t copy_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	copied_tile_bytes = 0;
	Vector<Rect2i> changed_regions;
	Vector<Ref<Image>> changed_images;
	for (int tile_index = 0; succeeded && tile_index < frame_info.changed_tile_count; tile_index++) {
		hcsr_runtime_changed_tile_info_t tile;
		_initialize_abi(tile);
		if (hcsr_runtime_frame_get_changed_tile(frame, frame_info.runtime_generation, tile_index, &tile) != HCSR_RUNTIME_OK) {
			succeeded = false;
			break;
		}
		PackedByteArray bgra_tile;
		bgra_tile.resize(tile.minimum_destination_stride_bytes * tile.pixel_height);
		if (hcsr_runtime_frame_copy_changed_tile(
				frame,
				frame_info.runtime_generation,
				tile_index,
				bgra_tile.ptrw(),
				bgra_tile.size(),
				tile.minimum_destination_stride_bytes) != HCSR_RUNTIME_OK) {
			succeeded = false;
			break;
		}
		const uint8_t *source = bgra_tile.ptr();
		uint8_t *destination = rgba_pixels.ptrw();
		PackedByteArray rgba_tile;
		rgba_tile.resize(tile.pixel_width * tile.pixel_height * 4);
		uint8_t *tile_destination = rgba_tile.ptrw();
		for (int y = 0; y < tile.pixel_height; y++) {
			for (int x = 0; x < tile.pixel_width; x++) {
				const int source_offset = y * tile.minimum_destination_stride_bytes + x * 4;
				const int destination_offset = ((tile.pixel_y + y) * frame_info.pixel_width + tile.pixel_x + x) * 4;
				const int tile_offset = (y * tile.pixel_width + x) * 4;
				tile_destination[tile_offset + 0] = source[source_offset + 2];
				tile_destination[tile_offset + 1] = source[source_offset + 1];
				tile_destination[tile_offset + 2] = source[source_offset + 0];
				tile_destination[tile_offset + 3] = source[source_offset + 3];
				memcpy(destination + destination_offset, tile_destination + tile_offset, 4);
			}
		}
		changed_regions.push_back(Rect2i(tile.pixel_x, tile.pixel_y, tile.pixel_width, tile.pixel_height));
		changed_images.push_back(Image::create_from_data(
				tile.pixel_width,
				tile.pixel_height,
				false,
				Image::FORMAT_RGBA8,
				rgba_tile));
		copied_tile_bytes += uint64_t(tile.pixel_width) * tile.pixel_height * 4;
	}
	last_tile_copy_milliseconds = copy_start_usec == 0 ? 0.0 : (OS::get_singleton()->get_ticks_usec() - copy_start_usec) / 1000.0;

	if (succeeded && frame_info.changed_tile_count > 0) {
		Ref<HTMLTexture2D> target_texture = p_primary ? texture : p_output_state->texture;
		const bool full_initialization = target_texture->get_width() != frame_info.pixel_width
				|| target_texture->get_height() != frame_info.pixel_height;
		const uint64_t upload_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		target_texture->update_regions(
				Size2i(frame_info.pixel_width, frame_info.pixel_height),
				changed_regions,
				changed_images,
				true);
		last_texture_upload_milliseconds = upload_start_usec == 0 ? 0.0 : (OS::get_singleton()->get_ticks_usec() - upload_start_usec) / 1000.0;
		last_texture_upload_bytes += full_initialization ? required_bytes : copied_tile_bytes;
	}

	if (p_primary && succeeded) {
		frame_metadata.logical_size = size;
		frame_metadata.physical_size = Size2i(frame_info.pixel_width, frame_info.pixel_height);
		frame_metadata.device_scale_factor = device_scale_factor;
		frame_metadata.generation = p_publication_generation;
		frame_metadata.hits.clear();
		frame_metadata.backdrop_filter_regions.clear();
	} else if (!p_primary && succeeded) {
		p_output_state->generation = p_publication_generation;
	}

	const hcsr_runtime_status_t release_status = hcsr_runtime_frame_release(p_publication, frame);
	return succeeded && release_status == HCSR_RUNTIME_OK;
}

bool HTMLSurfaceHCSRBackend::_consume_publication() {
	if (session == nullptr) {
		return false;
	}
	hcsr_runtime_publication_t *publication = nullptr;
	hcsr_runtime_publication_info_t publication_info;
	_initialize_abi(publication_info);
	const hcsr_runtime_status_t acquire_status = hcsr_runtime_session_acquire_publication(
			session,
			consumed_runtime_generation,
			&publication,
			&publication_info);
	if (acquire_status == HCSR_RUNTIME_NO_NEW_PUBLICATION) {
		return false;
	}
	if (acquire_status != HCSR_RUNTIME_OK || publication == nullptr) {
		_set_terminal_failure(vformat("HCSR RuntimeSession publication acquisition failed with status %d.", (int)acquire_status));
		return false;
	}

	bool succeeded = true;
	last_texture_upload_bytes = 0;
	primary_copied_tile_bytes = 0;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		entry.value->copied_tile_bytes = 0;
	}
	for (int output_index = 0; output_index < publication_info.output_count; output_index++) {
		hcsr_runtime_output_info_t output_info;
		_initialize_abi(output_info);
		if (hcsr_runtime_publication_get_output(publication, publication_info.generation, output_index, &output_info) != HCSR_RUNTIME_OK) {
			succeeded = false;
			break;
		}
		if (output_info.output_id == 1) {
			succeeded = _apply_output_frame(publication, publication_info.generation, output_index, nullptr, true);
		} else {
			OutputState *state = nullptr;
			for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
				if (entry.value->runtime_output_id == output_info.output_id) {
					state = entry.value;
					break;
				}
			}
			succeeded = state != nullptr && _apply_output_frame(publication, publication_info.generation, output_index, state, false);
		}
		if (!succeeded) {
			break;
		}
	}

	const hcsr_runtime_status_t release_status = hcsr_runtime_publication_release(session, publication);
	if (!succeeded || release_status != HCSR_RUNTIME_OK) {
		_set_terminal_failure("HCSR RuntimeSession could not activate a complete publication.");
		return false;
	}
	consumed_runtime_generation = publication_info.generation;
	active_generation = publication_info.generation;
	_publish_metrics();
	return true;
}

void HTMLSurfaceHCSRBackend::_publish_metrics() {
	HCSRPerformanceMonitor::IntegrationCounters counters;
	counters.cpu_primary_publication_milliseconds = last_step_milliseconds;
	counters.cpu_primary_conversion_milliseconds = last_tile_copy_milliseconds;
	counters.cpu_primary_upload_milliseconds = last_texture_upload_milliseconds;
	counters.runtime_session_step_milliseconds = last_step_milliseconds;
	counters.runtime_session_work_units = last_step_work_units;
	counters.runtime_changed_tile_bytes = primary_copied_tile_bytes;
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		counters.runtime_changed_tile_bytes += entry.value->copied_tile_bytes;
	}
	counters.runtime_texture_upload_bytes = last_texture_upload_bytes;
	counters.runtime_retiring_sessions = retiring_sessions.size();
	HCSRPerformanceMonitor::update_integration((uint64_t)this, counters);
}

Error HTMLSurfaceHCSRBackend::_submit_attribute_mutations(const Array &p_mutations) {
	if (p_mutations.is_empty()) {
		return OK;
	}
	if (!_compile_document() || !_recreate_session() || session == nullptr) {
		return ERR_UNAVAILABLE;
	}

	hcsr_runtime_mutation_t *mutation = nullptr;
	if (hcsr_runtime_mutation_begin(session, &mutation) != HCSR_RUNTIME_OK || mutation == nullptr) {
		return FAILED;
	}
	bool valid = true;
	for (int mutation_index = 0; mutation_index < p_mutations.size(); mutation_index++) {
		if (p_mutations[mutation_index].get_type() != Variant::DICTIONARY) {
			valid = false;
			break;
		}
		const Dictionary entry = p_mutations[mutation_index];
		const String operation = entry.get("operation", String());
		const String id = entry.get("id", String());
		const String name = entry.get("name", String());
		const String value = entry.get("value", String());
		if (operation != "set_attribute" || id.is_empty() || name.is_empty()) {
			valid = false;
			break;
		}
		const CharString id_utf8 = id.utf8();
		const CharString name_utf8 = name.utf8();
		const CharString value_utf8 = value.utf8();
		if (hcsr_runtime_mutation_set_attribute_by_id(mutation, id_utf8.ptr(), name_utf8.ptr(), value_utf8.ptr()) != HCSR_RUNTIME_OK) {
			valid = false;
			break;
		}
	}
	if (!valid) {
		hcsr_runtime_mutation_release(mutation);
		return ERR_INVALID_PARAMETER;
	}

	const hcsr_runtime_status_t submit_status = hcsr_runtime_session_submit_mutation(session, mutation);
	if (submit_status != HCSR_RUNTIME_OK) {
		hcsr_runtime_mutation_release(mutation);
		return submit_status == HCSR_RUNTIME_LIMIT_EXCEEDED ? ERR_OUT_OF_MEMORY : FAILED;
	}
	queued_generation++;
	derivation_pending = true;
	return OK;
}

Error HTMLSurfaceHCSRBackend::_submit_attribute_mutation(const StringName &p_id, const StringName &p_name, const String &p_value) {
	Dictionary mutation;
	mutation["operation"] = "set_attribute";
	mutation["id"] = String(p_id);
	mutation["name"] = String(p_name);
	mutation["value"] = p_value;
	Array mutations;
	mutations.push_back(mutation);
	return _submit_attribute_mutations(mutations);
}

void HTMLSurfaceHCSRBackend::mark_document_dirty() {
	document_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_size(const Size2i &p_size) {
	const Size2i new_size(MAX(1, p_size.x), MAX(1, p_size.y));
	if (size == new_size) {
		return;
	}
	size = new_size;
	session_configuration_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_device_scale_factor(float p_device_scale_factor) {
	const float new_scale = CLAMP(p_device_scale_factor, 0.01f, 8.0f);
	if (Math::is_equal_approx(device_scale_factor, new_scale)) {
		return;
	}
	device_scale_factor = new_scale;
	session_configuration_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_physical_size(const Size2i &p_physical_size) {
	const Size2i new_size(MAX(1, p_physical_size.x), MAX(1, p_physical_size.y));
	if (physical_size == new_size) {
		return;
	}
	physical_size = new_size;
	session_configuration_dirty = true;
}

void HTMLSurfaceHCSRBackend::set_document(const Ref<HTMLDocument> &p_document) {
	if (document != p_document) {
		document = p_document;
		document_dirty = document.is_valid();
	}
	if (document_dirty && document.is_valid()) {
		_compile_document();
	}
}

void HTMLSurfaceHCSRBackend::set_background_color(const Color &p_background_color) {
	HTMLSurfaceCPUBackend::set_background_color(p_background_color);
}

Error HTMLSurfaceHCSRBackend::update_compositor(double p_timeline_time_seconds, bool *r_needs_output, bool *r_needs_begin_frame) {
	frame_metadata.timeline_time_seconds = p_timeline_time_seconds;
	bool ready = false;
	if (!terminal_failure && document.is_valid() && _compile_document() && _recreate_session()) {
		ready = _step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
	}
	_service_retirement(HCSR_RUNTIME_RETIREMENT_BUDGET_USEC);
	_publish_metrics();
	if (r_needs_output != nullptr) {
		*r_needs_output = ready;
	}
	if (r_needs_begin_frame != nullptr) {
		*r_needs_begin_frame = derivation_pending;
	}
	return terminal_failure ? FAILED : OK;
}

void HTMLSurfaceHCSRBackend::render_placeholder(const String &p_marker) {
	(void)p_marker;
	if (terminal_failure || document.is_null()) {
		return;
	}
	if (!_compile_document() || !_recreate_session()) {
		return;
	}
	if (derivation_pending) {
		_step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
	}
	_consume_publication();
}

bool HTMLSurfaceHCSRBackend::poll_pending_output(bool *r_waiting_for_completion) {
	bool changed = false;
	if (!terminal_failure && document.is_valid() && _compile_document() && _recreate_session()) {
		if (derivation_pending) {
			_step_session(HCSR_RUNTIME_FRAME_STEP_BUDGET_USEC);
		}
		changed = _consume_publication();
	}
	_service_retirement(HCSR_RUNTIME_RETIREMENT_BUDGET_USEC);
	_publish_metrics();
	if (r_waiting_for_completion != nullptr) {
		*r_waiting_for_completion = derivation_pending;
	}
	return changed;
}

HTMLPendingOutputState HTMLSurfaceHCSRBackend::consume_pending_output_state() {
	HTMLPendingOutputState state;
	state.presentation_changed = poll_pending_output(&state.producer_blocked);
	state.producer_blocked = false;
	state.retirement_pending = !retiring_sessions.is_empty();
	return state;
}

void HTMLSurfaceHCSRBackend::schedule_retirement_service() {
	_service_retirement(HCSR_RUNTIME_RETIREMENT_BUDGET_USEC);
}

bool HTMLSurfaceHCSRBackend::has_pending_output() const {
	return derivation_pending;
}

bool HTMLSurfaceHCSRBackend::has_pending_frame_request() const {
	return derivation_pending || document_dirty || session_configuration_dirty;
}

uint64_t HTMLSurfaceHCSRBackend::get_last_queued_frame_generation() const {
	return queued_generation;
}

uint64_t HTMLSurfaceHCSRBackend::get_active_frame_generation() const {
	return active_generation;
}

bool HTMLSurfaceHCSRBackend::uses_generation_bound_input() const {
	return true;
}

bool HTMLSurfaceHCSRBackend::has_terminal_render_failure() const {
	return terminal_failure;
}

String HTMLSurfaceHCSRBackend::get_terminal_render_failure_reason() const {
	return terminal_failure_reason;
}

Error HTMLSurfaceHCSRBackend::apply_element_mutations(const Array &p_mutations) {
	return _submit_attribute_mutations(p_mutations);
}

Error HTMLSurfaceHCSRBackend::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	return _submit_attribute_mutation(p_id, p_name, p_value);
}

Error HTMLSurfaceHCSRBackend::set_element_style(const StringName &p_id, const String &p_css_text) {
	return _submit_attribute_mutation(p_id, SNAME("style"), p_css_text);
}

void HTMLSurfaceHCSRBackend::get_frame_metadata(HTMLFrameMetadata &r_metadata) const {
	r_metadata = frame_metadata;
}

uint64_t HTMLSurfaceHCSRBackend::create_presentation_output(const Size2i &p_size, bool p_mipmaps) {
	const uint64_t output_id = next_presentation_output_id++;
	ERR_FAIL_COND_V_MSG(output_id > uint64_t(INT32_MAX), 0, "HCSR RuntimeSession exhausted its presentation-output identifier range.");
	OutputState *state = memnew(OutputState);
	state->runtime_output_id = int32_t(output_id);
	state->requested_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	state->mipmaps = p_mipmaps;
	state->texture.instantiate();
	presentation_outputs.insert(output_id, state);
	session_configuration_dirty = true;
	return output_id;
}

Error HTMLSurfaceHCSRBackend::resize_presentation_output(uint64_t p_output_id, const Size2i &p_size) {
	OutputState **state = presentation_outputs.getptr(p_output_id);
	ERR_FAIL_NULL_V(state, ERR_DOES_NOT_EXIST);
	const Size2i new_size(MAX(1, p_size.x), MAX(1, p_size.y));
	if ((*state)->requested_size != new_size) {
		(*state)->requested_size = new_size;
		session_configuration_dirty = true;
	}
	return OK;
}

void HTMLSurfaceHCSRBackend::destroy_presentation_output(uint64_t p_output_id) {
	OutputState **state = presentation_outputs.getptr(p_output_id);
	if (state == nullptr) {
		return;
	}
	memdelete(*state);
	presentation_outputs.erase(p_output_id);
	session_configuration_dirty = true;
}

Ref<Texture2D> HTMLSurfaceHCSRBackend::get_presentation_output_texture(uint64_t p_output_id) const {
	const OutputState *const *state = presentation_outputs.getptr(p_output_id);
	if (state == nullptr) {
		return Ref<Texture2D>();
	}
	return (*state)->texture;
}

uint64_t HTMLSurfaceHCSRBackend::get_presentation_output_generation(uint64_t p_output_id) const {
	const OutputState *const *state = presentation_outputs.getptr(p_output_id);
	return state != nullptr ? (*state)->generation : 0;
}

HTMLSurfaceHCSRBackend::HTMLSurfaceHCSRBackend() {
	if (hcsr_runtime_get_abi_version() != HCSR_RUNTIME_ABI_VERSION) {
		_set_terminal_failure(vformat(
				"HCSR RuntimeSession ABI mismatch: Godot expects %d, library reports %d.",
				HCSR_RUNTIME_ABI_VERSION,
				hcsr_runtime_get_abi_version()));
	}
}

HTMLSurfaceHCSRBackend::~HTMLSurfaceHCSRBackend() {
	_begin_session_retirement();
	const uint64_t shutdown_start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	while (!retiring_sessions.is_empty()
			&& (shutdown_start_usec == 0 || OS::get_singleton()->get_ticks_usec() - shutdown_start_usec < 100000)) {
		_service_retirement(HCSR_RUNTIME_RETIREMENT_BUDGET_USEC);
	}
	if (!retiring_sessions.is_empty()) {
		ERR_PRINT(vformat("HCSR RuntimeSession bounded shutdown left %d session(s) pending; handles are intentionally retained rather than destroyed unsafely.", retiring_sessions.size()));
	}
	if (compiled_document != nullptr) {
		hcsr_runtime_document_release(compiled_document);
		compiled_document = nullptr;
	}
	for (const KeyValue<uint64_t, OutputState *> &entry : presentation_outputs) {
		memdelete(entry.value);
	}
	presentation_outputs.clear();
	HCSRPerformanceMonitor::remove((uint64_t)this);
}
