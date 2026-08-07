#ifndef BATTERY_MANAGER_H_
#define BATTERY_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*battery_changed_cb_t)(uint8_t soc_percent, uint16_t voltage_mv,
				     uint16_t remaining_mah);
typedef void (*charger_changed_cb_t)(bool charging, bool complete);

struct battery_manager_cb {
	battery_changed_cb_t on_battery_changed;
	charger_changed_cb_t on_charger_changed;
};

int battery_manager_init(const struct battery_manager_cb *cbs);
uint8_t battery_get_soc(void);
uint16_t battery_get_voltage(void);

/** Remaining capacity in mAh (RepCap), as of the last poll. */
uint16_t battery_get_remaining_mah(void);

/** Current charger state: true while the pack is actively charging. */
bool battery_is_charging(void);

/**
 * Current charger state: true when external power is present but charging has
 * finished (fully charged). Both getters false means running on battery.
 */
bool battery_is_charge_complete(void);

/**
 * Trigger an immediate battery status read.
 * Submits the poll work item to the system workqueue.
 * Safe to call from ISR or workqueue context.
 */
void battery_manager_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_MANAGER_H_ */
