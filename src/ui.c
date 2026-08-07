#include "ui.h"
#include "gpio_input.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ui, CONFIG_LOG_DEFAULT_LEVEL);

/* --- Key actions ---------------------------------------------------- */

enum ui_action {
	UI_ACTION_NONE = 0,
	UI_ACTION_LOCK,
	UI_ACTION_UNLOCK,
	UI_ACTION_DISCOVERY,
	UI_ACTION_FACTORY_RESET,
};

/*
 * What each key does, decided by how many keys the board has.
 *
 * Discovery and factory reset are mandatory in every layout — the BUILD_ASSERTs
 * below enforce it. Without discovery, a device that misses the window opened
 * at first boot has no way back to a commissionable state; without factory
 * reset, neither does one bonded to a phone or fabric that no longer exists.
 *
 * Both sit on long presses wherever something more frequent competes. Factory
 * reset is on the lock key rather than the unlock key deliberately: the key a
 * user leans on when the door will not open is the unlock one, and ten seconds
 * of that should not wipe the fabric.
 */
#if DT_NODE_EXISTS(DT_ALIAS(key1))
#define UI_HAVE_KEY1 1

#define KEY0_SHORT_ACTION   UI_ACTION_LOCK
#define KEY0_LONG_ACTION    UI_ACTION_FACTORY_RESET
#define KEY0_LONG_PRESS_MS  CONFIG_LOCK_FACTORY_RESET_PRESS_MS

#define KEY1_SHORT_ACTION   UI_ACTION_UNLOCK
#define KEY1_LONG_ACTION    UI_ACTION_DISCOVERY
#define KEY1_LONG_PRESS_MS  CONFIG_LOCK_DISCOVERY_PRESS_MS
#else
/*
 * One key: no room for bolt control, so both mandatory actions take it.
 * KEY1_* stay defined as NONE so the assertions below need no #ifdef.
 */
#define KEY0_SHORT_ACTION   UI_ACTION_DISCOVERY
#define KEY0_LONG_ACTION    UI_ACTION_FACTORY_RESET
#define KEY0_LONG_PRESS_MS  CONFIG_LOCK_FACTORY_RESET_PRESS_MS

#define KEY1_SHORT_ACTION   UI_ACTION_NONE
#define KEY1_LONG_ACTION    UI_ACTION_NONE
#define KEY1_LONG_PRESS_MS  0
#endif

#define UI_ACTION_ASSIGNED(action)                                            \
	(KEY0_SHORT_ACTION == (action) || KEY0_LONG_ACTION == (action) ||     \
	 KEY1_SHORT_ACTION == (action) || KEY1_LONG_ACTION == (action))

BUILD_ASSERT(UI_ACTION_ASSIGNED(UI_ACTION_DISCOVERY),
	     "No key starts discovery: a device that misses the commissioning "
	     "window opened at first boot would be unrecoverable.");
BUILD_ASSERT(UI_ACTION_ASSIGNED(UI_ACTION_FACTORY_RESET),
	     "No key triggers a factory reset: a lock bonded to a phone or "
	     "fabric that no longer exists could not be recovered.");

/* --- LED ------------------------------------------------------------- */

#define LED_BLINK_SLOW_MS 500
#define LED_BLINK_FAST_MS 100

/*
 * How long before a factory reset completes the LED starts blinking fast.
 * The only feedback during a ten-second hold is otherwise nothing at all,
 * so this is the cue to let go if the reset was not intended.
 */
#define UI_RESET_WARNING_MS 3000

BUILD_ASSERT(KEY0_SHORT_ACTION != UI_ACTION_FACTORY_RESET &&
		     KEY1_SHORT_ACTION != UI_ACTION_FACTORY_RESET,
	     "Factory reset must be a long press: a tap gives the user no "
	     "chance to notice the warning and let go.");
BUILD_ASSERT(KEY0_LONG_ACTION != UI_ACTION_FACTORY_RESET ||
		     KEY0_LONG_PRESS_MS > UI_RESET_WARNING_MS,
	     "Key 0 factory reset hold is too short to warn before it fires.");
BUILD_ASSERT(KEY1_LONG_ACTION != UI_ACTION_FACTORY_RESET ||
		     KEY1_LONG_PRESS_MS > UI_RESET_WARNING_MS,
	     "Key 1 factory reset hold is too short to warn before it fires.");

/* --- State ----------------------------------------------------------- */

struct ui_key {
	const struct gpio_dt_spec gpio;
	const enum ui_action short_action;
	const enum ui_action long_action;
	const uint32_t long_press_ms;

	struct gpio_input input;
	struct k_timer long_timer;
	struct k_work long_work;
	bool held;
	/* Set once the long action has fired, so releasing the key does not
	 * also run the short one. */
	bool long_fired;
};

static struct ui_key keys[] = {
	{
		.gpio = GPIO_DT_SPEC_GET(DT_ALIAS(key0), gpios),
		.short_action = KEY0_SHORT_ACTION,
		.long_action = KEY0_LONG_ACTION,
		.long_press_ms = KEY0_LONG_PRESS_MS,
	},
#ifdef UI_HAVE_KEY1
	{
		.gpio = GPIO_DT_SPEC_GET(DT_ALIAS(key1), gpios),
		.short_action = KEY1_SHORT_ACTION,
		.long_action = KEY1_LONG_ACTION,
		.long_press_ms = KEY1_LONG_PRESS_MS,
	},
#endif
};

static const struct gpio_dt_spec led_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/*
 * The flags below are written from the BLE and Matter threads and read from the
 * system workqueue and the timer ISR. No lock: k_timer and GPIO are internally
 * synchronised and the worst-case race is a one-tick LED glitch.
 */
static const struct ui_cb *app_cbs;

static struct k_timer led_blink_timer;
static struct k_timer reset_warning_timer;

static bool discovery_active;
static bool identify_active;
static bool charging_active;
static bool charge_complete;
static bool reset_warning;

/* --- LED ------------------------------------------------------------- */

static void led_blink_handler(struct k_timer *timer)
{
	gpio_pin_toggle_dt(&led_gpio);
}

static void led_update(void)
{
	if (reset_warning) {
		k_timer_start(&led_blink_timer, K_MSEC(LED_BLINK_FAST_MS),
			      K_MSEC(LED_BLINK_FAST_MS));
	} else if (identify_active || discovery_active || charging_active) {
		k_timer_start(&led_blink_timer, K_MSEC(LED_BLINK_SLOW_MS),
			      K_MSEC(LED_BLINK_SLOW_MS));
	} else if (charge_complete) {
		k_timer_stop(&led_blink_timer);
		gpio_pin_set_dt(&led_gpio, 1);
	} else {
		k_timer_stop(&led_blink_timer);
		gpio_pin_set_dt(&led_gpio, 0);
	}
}

/*
 * Runs from the timer ISR; led_update() only touches k_timer and GPIO, both
 * of which are ISR-safe.
 */
static void reset_warning_expiry(struct k_timer *timer)
{
	reset_warning = true;
	led_update();
}

static void reset_warning_cancel(void)
{
	k_timer_stop(&reset_warning_timer);

	if (reset_warning) {
		reset_warning = false;
		led_update();
	}
}

/* --- Actions --------------------------------------------------------- */

static void run_action(enum ui_action action)
{
	int err;

	switch (action) {
	case UI_ACTION_LOCK:
	case UI_ACTION_UNLOCK:
		if (!app_cbs || !app_cbs->lock_request) {
			break;
		}

		/*
		 * Unlock, not open: this is a mortise lock, so retracting the
		 * bolt is enough — the handle works the latch from either side.
		 */
		err = app_cbs->lock_request(action == UI_ACTION_LOCK ?
						    LOCK_ACTION_LOCK :
						    LOCK_ACTION_UNLOCK);
		if (err == -EALREADY) {
			LOG_DBG("UI: lock already in requested position");
		} else if (err) {
			LOG_WRN("UI: lock request failed: %d", err);
		}
		break;

	case UI_ACTION_DISCOVERY:
		/*
		 * discovery_active is deliberately not latched here: the backend
		 * owns it and reports every end, its own timeout included, so
		 * latching would leave the LED blinking forever.
		 */
		if (discovery_active || !app_cbs || !app_cbs->start_discovery) {
			break;
		}

		err = app_cbs->start_discovery();
		if (err) {
			LOG_WRN("UI: start discovery failed: %d", err);
		}
		break;

	case UI_ACTION_FACTORY_RESET:
		if (app_cbs && app_cbs->factory_reset) {
			LOG_INF("UI: factory reset requested");
			app_cbs->factory_reset();
		}
		break;

	case UI_ACTION_NONE:
		break;
	}
}

/* --- Keys ------------------------------------------------------------ */

static void key_work_handler(struct k_work *work)
{
	struct gpio_input *input = CONTAINER_OF(work, struct gpio_input, work);
	struct ui_key *key = CONTAINER_OF(input, struct ui_key, input);
	int pressed = gpio_pin_get_dt(&key->gpio);

	if (pressed < 0) {
		LOG_WRN("UI: key read error: %d", pressed);
		return;
	}

	if (pressed && !key->held) {
		key->held = true;
		key->long_fired = false;

		if (key->long_action == UI_ACTION_NONE) {
			return;
		}

		k_timer_start(&key->long_timer, K_MSEC(key->long_press_ms),
			      K_NO_WAIT);

		if (key->long_action == UI_ACTION_FACTORY_RESET) {
			k_timer_start(&reset_warning_timer,
				      K_MSEC(key->long_press_ms -
					     UI_RESET_WARNING_MS),
				      K_NO_WAIT);
		}
	} else if (!pressed && key->held) {
		key->held = false;
		k_timer_stop(&key->long_timer);
		reset_warning_cancel();

		if (!key->long_fired) {
			run_action(key->short_action);
		}
	}
}

static void long_press_work_handler(struct k_work *work)
{
	struct ui_key *key = CONTAINER_OF(work, struct ui_key, long_work);

	/* Released between the timer firing and this work item running. */
	if (!key->held) {
		return;
	}

	reset_warning_cancel();
	run_action(key->long_action);
}

static void long_press_expiry(struct k_timer *timer)
{
	struct ui_key *key = CONTAINER_OF(timer, struct ui_key, long_timer);

	/*
	 * Set here rather than in the work handler: a release landing between
	 * this expiry and the handler would otherwise still look like a short
	 * press, and run the short action after a full-length hold.
	 */
	key->long_fired = true;
	k_work_submit(&key->long_work);
}

/* --- Notifications --------------------------------------------------- */

void ui_on_discovery_changed(bool discoverable)
{
	discovery_active = discoverable;
	led_update();
}

void ui_on_identify(bool active)
{
	identify_active = active;
	led_update();
}

void ui_on_charging_changed(bool charging, bool complete)
{
	charging_active = charging;
	charge_complete = complete;
	led_update();
}

/* --- Init ------------------------------------------------------------ */

int ui_init(const struct ui_cb *cbs)
{
	int err;

	app_cbs = cbs;

	if (!gpio_is_ready_dt(&led_gpio)) {
		LOG_ERR("UI: LED GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	k_timer_init(&led_blink_timer, led_blink_handler, NULL);
	k_timer_init(&reset_warning_timer, reset_warning_expiry, NULL);

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		struct ui_key *key = &keys[i];

		k_work_init(&key->long_work, long_press_work_handler);
		k_timer_init(&key->long_timer, long_press_expiry, NULL);

		err = gpio_input_init(&key->input, &key->gpio, key_work_handler,
				      CONFIG_LOCK_BUTTON_DEBOUNCE_MS);
		if (err) {
			LOG_ERR("UI: key%u init failed: %d", (unsigned int)i,
				err);
			return err;
		}
	}

	LOG_INF("UI: init (%u key%s)", (unsigned int)ARRAY_SIZE(keys),
		ARRAY_SIZE(keys) == 1 ? "" : "s");
	return 0;
}
