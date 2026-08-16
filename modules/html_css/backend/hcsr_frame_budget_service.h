/**************************************************************************/
/*  hcsr_frame_budget_service.h                                           */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"

class HCSRFrameBudgetService {
	static uint64_t process_frame;
	static uint64_t semantic_used_usec;
	static uint64_t presentation_used_usec;

	static void _refresh_frame();

public:
	static uint64_t claim_semantic(uint64_t p_maximum_usec);
	static void consume_semantic(uint64_t p_elapsed_usec);
	static uint64_t claim_presentation(uint64_t p_maximum_usec);
	static void consume_presentation(uint64_t p_elapsed_usec);
};
