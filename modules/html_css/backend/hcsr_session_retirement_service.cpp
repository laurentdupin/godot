/**************************************************************************/
/*  hcsr_session_retirement_service.cpp                                  */
/**************************************************************************/

#include "hcsr_session_retirement_service.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/scene_tree.h"

Vector<hcsr_runtime_session_t *> HCSRSessionRetirementService::sessions;
bool HCSRSessionRetirementService::callback_pending = false;
bool HCSRSessionRetirementService::finalizing = false;

void HCSRSessionRetirementService::_request_frame_callback() {
	if (finalizing || callback_pending || sessions.is_empty() || OS::get_singleton() == nullptr) {
		return;
	}
	SceneTree *scene_tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	if (scene_tree == nullptr) {
		return;
	}
	callback_pending = true;
	scene_tree->connect(SNAME("process_frame"), callable_mp_static(&HCSRSessionRetirementService::_service_frame), Object::CONNECT_ONE_SHOT);
}

void HCSRSessionRetirementService::_service_frame() {
	callback_pending = false;
	service(500);
	_request_frame_callback();
}

void HCSRSessionRetirementService::enqueue(hcsr_runtime_session_t *p_session) {
	if (p_session == nullptr) {
		return;
	}
	const hcsr_runtime_status_t status = hcsr_runtime_session_begin_shutdown(p_session);
	if (status != HCSR_RUNTIME_OK && status != HCSR_RUNTIME_CLOSED) {
		ERR_PRINT(vformat("HCSR RuntimeSession shutdown failed with status %d.", (int)status));
		return;
	}
	sessions.push_back(p_session);
	_request_frame_callback();
}

void HCSRSessionRetirementService::service(uint64_t p_budget_usec) {
	const uint64_t start_usec = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	int session_index = 0;
	uint32_t work_units = 0;
	while (!sessions.is_empty()) {
		if (session_index >= sessions.size()) {
			session_index = 0;
		}
		hcsr_runtime_step_info_t info = {};
		info.struct_size = sizeof(info);
		info.abi_version = HCSR_RUNTIME_ABI_VERSION;
		const hcsr_runtime_status_t status = hcsr_runtime_session_step_retirement(sessions[session_index], 1, &info);
		if (status == HCSR_RUNTIME_CLOSED) {
			if (hcsr_runtime_session_destroy(sessions[session_index]) != HCSR_RUNTIME_OK) {
				ERR_PRINT("HCSR RuntimeSession could not destroy a closed session.");
			}
			sessions.remove_at(session_index);
		} else {
			if (status != HCSR_RUNTIME_PENDING_CLEANUP && status != HCSR_RUNTIME_WAITING_FOR_LEASES) {
				ERR_PRINT(vformat("HCSR RuntimeSession retirement failed with status %d.", (int)status));
			}
			session_index++;
		}
		work_units++;
		if ((start_usec != 0 && OS::get_singleton()->get_ticks_usec() - start_usec >= p_budget_usec)
				|| (start_usec == 0 && work_units >= 1024)) {
			break;
		}
	}
	_request_frame_callback();
}

int HCSRSessionRetirementService::pending_count() {
	return sessions.size();
}

void HCSRSessionRetirementService::finalize() {
	finalizing = true;
	callback_pending = false;
	while (!sessions.is_empty()) {
		service(UINT64_MAX);
	}
	finalizing = false;
}
