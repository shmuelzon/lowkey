#include "door_monitor.h"
#include "gpio_input.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(door_monitor, CONFIG_LOG_DEFAULT_LEVEL);

static const struct gpio_dt_spec door_sensor_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(door_sensor), gpios);

/* Written only from the system workqueue, read from the radio threads. */
static atomic_t current_state;
static door_state_changed_cb_t state_cb;

static struct gpio_input sensor_input;

static void sensor_work_handler(struct k_work *work)
{
	int active = gpio_pin_get_dt(&door_sensor_gpio);
	enum door_state new_state;

	if (active < 0) {
		LOG_WRN("Door: sensor read error: %d", active);
		return;
	}

	new_state = active ? DOOR_STATE_CLOSED : DOOR_STATE_OPEN;

	if (atomic_set(&current_state, new_state) == new_state) {
		return;
	}

	LOG_INF("Door: %s", new_state == DOOR_STATE_CLOSED ? "closed" : "open");

	if (state_cb) {
		state_cb(new_state);
	}
}

int door_monitor_init(door_state_changed_cb_t cb)
{
	int err;

	state_cb = cb;

	err = gpio_input_init(&sensor_input, &door_sensor_gpio,
			      sensor_work_handler, 0);
	if (err) {
		LOG_ERR("Door: sensor init failed: %d", err);
		return err;
	}

	int active = gpio_pin_get_dt(&door_sensor_gpio);

	atomic_set(&current_state,
		   (active > 0) ? DOOR_STATE_CLOSED : DOOR_STATE_OPEN);

	LOG_INF("Door: init, %s",
		door_monitor_get_state() == DOOR_STATE_CLOSED ? "closed" : "open");
	return 0;
}

enum door_state door_monitor_get_state(void)
{
	return (enum door_state)atomic_get(&current_state);
}
