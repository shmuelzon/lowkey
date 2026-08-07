#ifndef LOCK_MANAGER_H_
#define LOCK_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bolt and latch position.
 *
 * The bolt and the latch are separate mechanisms: the bolt is driven by the
 * motor and holds the door locked, while the spring latch holds a closed door
 * shut on its own. Retracting the bolt (UNLOCKED) leaves the latch engaged, so
 * the door still cannot swing; holding the latch back as well (UNLATCHED) is
 * what lets the door open. See enum lock_action.
 *
 * UNLATCHED lasts exactly as long as the motor holds the cam over
 * (CONFIG_LOCK_LATCH_RELEASE_MS), because the latch is spring-loaded and pops
 * straight back out when the motor lets go. So it is only ever reported while
 * the motor is running, and the lock returns to UNLOCKED — bolt still
 * retracted, latch engaged again — as soon as the motor stops.
 *
 * Values are part of the BLE Lock Status characteristic's wire format, so new
 * states are appended rather than inserted.
 */
enum lock_state {
	LOCK_STATE_LOCKED    = 0,
	LOCK_STATE_LOCKING   = 1,
	LOCK_STATE_UNLOCKED  = 2,
	LOCK_STATE_UNLOCKING = 3,
	LOCK_STATE_JAMMED    = 4,
	LOCK_STATE_UNLATCHED = 5,
	LOCK_STATE_UNKNOWN   = 6,
};

/*
 * LOCK_ACTION_UNLOCK retracts the bolt and stops — the door stays held by the
 * spring latch. LOCK_ACTION_OPEN retracts the bolt and keeps driving to pull
 * the latch back too, so the door can actually be opened. These are the two
 * distinct operations Matter calls UnboltDoor and UnlockDoor respectively.
 */
enum lock_action {
	LOCK_ACTION_LOCK   = 0,
	LOCK_ACTION_UNLOCK = 1,
	LOCK_ACTION_OPEN   = 2,
};

enum lock_op_source {
	LOCK_OP_SOURCE_MANUAL = 0,
	LOCK_OP_SOURCE_REMOTE = 1,
	LOCK_OP_SOURCE_AUTO   = 2,
};

/*
 * Which client asked for a remote operation.
 *
 * The fields are Matter's because Matter is the only backend with the concept:
 * its DoorLock cluster reports the operating fabric and node in the
 * LockOperation event, and a controller turns that into "unlocked by Alice"
 * rather than attributing the action to nobody. lock_manager does not interpret
 * the values — it stores them with the running operation and hands them back
 * with every state change that operation produces, so a backend never has to
 * correlate its own callbacks to its own requests. Backends without the
 * concept (BLE) leave valid clear.
 */
struct lock_originator {
	bool valid;
	uint8_t fabric_index;
	uint64_t node_id;
};

/*
 * A reported change of bolt/latch position.
 *
 * operated separates a change someone caused from one the mechanism made on
 * its own: the spring latch re-engaging after an open, and the state pushed at
 * startup, both change what the lock reports without anything having been
 * operated. Consumers that log operations need the distinction — for Matter it
 * decides whether a LockOperation event is emitted, and without it a single
 * UnlockDoor would report both an Unlatch and a stray Unlock.
 *
 * originator is only populated for an operated remote change.
 */
struct lock_state_change {
	enum lock_state state;
	enum lock_op_source source;
	struct lock_originator originator;
	bool operated;
};

typedef void (*lock_state_changed_cb_t)(const struct lock_state_change *change);

int lock_manager_init(lock_state_changed_cb_t cb);
enum lock_state lock_manager_get_state(void);

/**
 * Request a bolt/latch operation.
 * @param originator Client behind a remote request, or NULL when there is
 *                   none to report. Copied; need not outlive the call.
 * @return 0 if the operation started, -EALREADY if the lock is already in (or
 *         already moving to) the requested position, negative errno otherwise.
 */
int lock_manager_request(enum lock_action action, enum lock_op_source source,
			 const struct lock_originator *originator);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_MANAGER_H_ */
