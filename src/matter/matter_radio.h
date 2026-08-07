#ifndef MATTER_RADIO_H_
#define MATTER_RADIO_H_

#include "lock_manager.h"
#include "door_monitor.h"
#include "radio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The Matter backend, fanned out to by radio.c. */
extern const struct radio_backend_ops matter_radio_ops;

/**
 * Request a lock action on behalf of a Matter client.
 * Called by ZCL callbacks; routes through the registered radio callback.
 * @param originator Fabric/node of the requesting client, or NULL when there
 *                   is none (a local or automatic operation).
 * @return 0 on success, negative errno on failure.
 */
int matter_radio_lock_request(enum lock_action action,
			      enum lock_op_source source,
			      const struct lock_originator *originator);

/**
 * Seed the DoorLock cluster with the current lock and door state.
 * Called from the ZCL cluster init callback. Writes the LockState and
 * DoorState attributes only — no LockOperation or DoorStateChange event is
 * emitted, because nothing was operated and nothing moved.
 */
void matter_radio_publish_initial_state(enum lock_state lock,
					enum door_state door);

#ifdef __cplusplus
}
#endif

#endif /* MATTER_RADIO_H_ */
