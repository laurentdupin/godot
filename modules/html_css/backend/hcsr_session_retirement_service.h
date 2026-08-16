/**************************************************************************/
/*  hcsr_session_retirement_service.h                                    */
/**************************************************************************/

#pragma once

#include "core/templates/vector.h"

#include "hcsr_runtime.h"

class HCSRSessionRetirementService {
	static Vector<hcsr_runtime_session_t *> sessions;
	static bool callback_pending;
	static bool finalizing;
	static void _request_frame_callback();
	static void _service_frame();

public:
	static void enqueue(hcsr_runtime_session_t *p_session);
	static void service(uint64_t p_budget_usec);
	static int pending_count();
	static void finalize();
};
