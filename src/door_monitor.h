#ifndef DOOR_MONITOR_H_
#define DOOR_MONITOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum door_state {
	DOOR_STATE_CLOSED = 0,
	DOOR_STATE_OPEN   = 1,
};

typedef void (*door_state_changed_cb_t)(enum door_state state);

int door_monitor_init(door_state_changed_cb_t cb);
enum door_state door_monitor_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* DOOR_MONITOR_H_ */
