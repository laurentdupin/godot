/**************************************************************************/
/*  hcsr_frame_budget_service.h                                           */
/**************************************************************************/

#pragma once

#include "core/typedefs.h"
#include "core/templates/vector.h"

class HCSRFrameBudgetService {
	struct OwnerState {
		uint64_t id = 0;
		bool semantic_pending = false;
		bool presentation_pending = false;
		uint64_t semantic_used_usec = 0;
		uint64_t presentation_used_usec = 0;
	};

	static uint64_t process_frame;
	static uint64_t semantic_used_usec;
	static uint64_t presentation_used_usec;
	static int semantic_cursor;
	static int presentation_cursor;
	static Vector<OwnerState> owners;
	static Vector<uint64_t> semantic_grants;
	static Vector<uint64_t> presentation_grants;

	static void _refresh_frame();
	static void _build_grants(bool p_semantic, Vector<uint64_t> &r_grants, int &r_cursor);
	static OwnerState *_find_owner(uint64_t p_owner);
	static bool _has_grant(const Vector<uint64_t> &p_grants, uint64_t p_owner);

public:
	static void register_owner(uint64_t p_owner);
	static void unregister_owner(uint64_t p_owner);
	static void set_semantic_pending(uint64_t p_owner, bool p_pending);
	static void set_presentation_pending(uint64_t p_owner, bool p_pending);
	static uint64_t claim_semantic(uint64_t p_owner, uint64_t p_maximum_usec);
	static void consume_semantic(uint64_t p_owner, uint64_t p_elapsed_usec);
	static uint64_t claim_presentation(uint64_t p_owner, uint64_t p_maximum_usec);
	static void consume_presentation(uint64_t p_owner, uint64_t p_elapsed_usec);
};
