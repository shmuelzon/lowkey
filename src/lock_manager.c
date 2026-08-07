#include "lock_manager.h"
#include "gpio_input.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lock_manager, CONFIG_LOG_DEFAULT_LEVEL);

/* Motor driver GPIOs */
#define MOTOR_NODE DT_NODELABEL(motor_driver)

/*
 * nSLEEP is optional (rev A DRV8837 has it, rev B DRV8871 does not). When
 * absent GPIO_DT_SPEC_GET_OR yields a NULL port, so every access is guarded.
 */
static const struct gpio_dt_spec motor_nsleep =
	GPIO_DT_SPEC_GET_OR(MOTOR_NODE, nsleep_gpios, {0});
static const struct gpio_dt_spec motor_in1 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, in1_gpios);
static const struct gpio_dt_spec motor_in2 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, in2_gpios);

/* Hall-effect position sensors (gpio-keys compatible) */
static const struct gpio_dt_spec lock_sensor_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(lock_sensor), gpios);
static const struct gpio_dt_spec unlock_sensor_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(unlock_sensor), gpios);

/*
 * lock_manager_request() is called from the BLE and Matter threads while the
 * state is modified from the system workqueue, so the mutex serializes access.
 *
 * State changes are staged here and delivered by unlock_and_notify() once the
 * mutex is released, never from inside it. state_cb reaches bt_gatt_notify(),
 * which waits K_FOREVER for an ATT buffer on any thread that is not the system
 * workqueue (see the timeout selection in Zephyr's att.c). Called under the
 * mutex, the BT RX thread would hold it while waiting for buffers that only the
 * system workqueue recycles — and the system workqueue would be blocked on the
 * same mutex.
 */
static K_MUTEX_DEFINE(lock_mutex);

/* Deepest run is a set_state() plus the settle in finish_operation(). */
static struct lock_state_change pending[3];
static uint8_t pending_count;

static enum lock_state current_state;
static lock_state_changed_cb_t state_cb;
static bool motor_running;
static enum lock_action motor_action;
static enum lock_op_source motor_source;
static struct lock_originator motor_originator;

static const char *lock_state_str(enum lock_state state)
{
	switch (state) {
	case LOCK_STATE_LOCKED: return "locked";
	case LOCK_STATE_LOCKING: return "locking";
	case LOCK_STATE_UNLOCKED: return "unlocked";
	case LOCK_STATE_UNLOCKING: return "unlocking";
	case LOCK_STATE_JAMMED: return "jammed";
	case LOCK_STATE_UNKNOWN: return "unknown";
	case LOCK_STATE_UNLATCHED: return "unlatched";
	}
	return "?";
}

static struct k_work motor_timeout_work;
static struct k_work motor_complete_work;

static struct k_timer motor_timeout_timer;
static struct k_timer motor_complete_timer;

static struct gpio_input lock_sensor_input;
static struct gpio_input unlock_sensor_input;

/*
 * Sequence number of the operation currently owning the motor, and the sequence
 * each timed phase was armed for.
 *
 * k_timer_stop() keeps a stopped timer from expiring again, but it cannot
 * unqueue work that an earlier expiry already submitted, and a handler that has
 * begun running is merely blocked on the mutex. So a phase handler can reach the
 * critical section belonging to an operation that has since been replaced —
 * where motor_running is true again and every other check passes. It would then
 * brake a motor run it knows nothing about, or declare it jammed.
 *
 * Each phase therefore records which operation armed it and verifies that
 * operation is still the current one. Comparison is by equality only, so the
 * counter wrapping is harmless.
 *
 * complete_armed tracks the same thing within one operation, which the sequence
 * numbers cannot: k_timer_remaining_get() reads 0 both for a stopped timer and
 * for one that has expired but whose work has not run yet, so using it as the
 * armed flag would let a sensor edge re-arm the phase under the already-queued
 * handler and stop the motor early.
 */
static uint32_t operation_seq;
static uint32_t timeout_seq;
static uint32_t complete_seq;
static bool complete_armed;

static void arm_timeout(uint32_t delay_ms)
{
	timeout_seq = operation_seq;
	k_timer_start(&motor_timeout_timer, K_MSEC(delay_ms), K_NO_WAIT);
}

static void arm_complete(uint32_t delay_ms)
{
	complete_seq = operation_seq;
	complete_armed = true;
	k_timer_start(&motor_complete_timer, K_MSEC(delay_ms), K_NO_WAIT);
}

/* Stage a state change for delivery after the mutex is dropped. */
static void notify_state(enum lock_state new_state, enum lock_op_source source,
			 bool operated)
{
	if (current_state == new_state) {
		return;
	}

	LOG_INF("Lock: %s -> %s", lock_state_str(current_state),
		lock_state_str(new_state));
	current_state = new_state;

	if (!state_cb) {
		return;
	}

	if (pending_count >= ARRAY_SIZE(pending)) {
		LOG_ERR("Lock: dropped %s notification", lock_state_str(new_state));
		return;
	}

	struct lock_state_change *change = &pending[pending_count++];

	*change = (struct lock_state_change){
		.state = new_state,
		.source = source,
		.operated = operated,
	};

	/* Only a remote operation has a client identity worth reporting. */
	if (operated && source == LOCK_OP_SOURCE_REMOTE) {
		change->originator = motor_originator;
	}
}

static void unlock_and_notify(void)
{
	struct lock_state_change flush[ARRAY_SIZE(pending)];
	uint8_t count = pending_count;

	memcpy(flush, pending, count * sizeof(flush[0]));
	pending_count = 0;

	k_mutex_unlock(&lock_mutex);

	for (uint8_t i = 0; i < count; i++) {
		state_cb(&flush[i]);
	}
}

/* A position reached because someone asked for it. */
static void set_state(enum lock_state new_state, enum lock_op_source source)
{
	notify_state(new_state, source, true);
}

/*
 * A position the mechanism reached on its own, with nobody operating the lock —
 * currently only the spring latch re-engaging after an open. The source of the
 * operation that led here is carried through for context, but consumers are
 * told not to log this as an operation.
 */
static void settle_state(enum lock_state new_state)
{
	notify_state(new_state, motor_source, false);
}

static void motor_stop(void)
{
	gpio_pin_set_dt(&motor_in1, 0);
	gpio_pin_set_dt(&motor_in2, 0);
	if (motor_nsleep.port) {
		gpio_pin_set_dt(&motor_nsleep, 0);
	}
	motor_running = false;
	complete_armed = false;
	k_timer_stop(&motor_timeout_timer);
	k_timer_stop(&motor_complete_timer);
}

static void motor_brake(uint32_t usec)
{
	gpio_pin_set_dt(&motor_in1, 1);
	gpio_pin_set_dt(&motor_in2, 1);
	k_busy_wait(usec);
	gpio_pin_set_dt(&motor_in1, 0);
	gpio_pin_set_dt(&motor_in2, 0);
}

/*
 * Only an open needs reporting here: it was holding the latch back, and the
 * spring returns it the moment the motor releases the cam, so the lock lands on
 * UNLOCKED. A settle rather than an operation — the open was already reported
 * when the latch pull began, and reporting it twice would add a bogus
 * LockOperation(Unlock) beside the Unlatch for one UnlockDoor command.
 *
 * Called with the mutex held.
 */
static void finish_operation(void)
{
	if (motor_action != LOCK_ACTION_OPEN) {
		return;
	}

	settle_state(LOCK_STATE_UNLOCKED);
}

/*
 * Shared by both sensors, which read both GPIOs on every run: a polling-style
 * state machine that is correct whichever sensor interrupted, and that tolerates
 * k_work coalescing when the two fire close together.
 */
static void sensor_work_handler(struct k_work *work)
{
	k_mutex_lock(&lock_mutex, K_FOREVER);

	int lock_active = gpio_pin_get_dt(&lock_sensor_gpio);
	int unlock_active = gpio_pin_get_dt(&unlock_sensor_gpio);

	if (lock_active < 0 || unlock_active < 0) {
		LOG_WRN("Lock: sensor read error: %d / %d",
			lock_active, unlock_active);
		unlock_and_notify();
		return;
	}

	if (!motor_running) {
		/*
		 * No UNLATCHED case to consider here: the latch is only held
		 * back while the motor drives the cam, so UNLATCHED never
		 * outlives motor_running and cannot be the state on this path.
		 */
		if (lock_active) {
			set_state(LOCK_STATE_LOCKED, LOCK_OP_SOURCE_MANUAL);
		} else if (unlock_active) {
			set_state(LOCK_STATE_UNLOCKED, LOCK_OP_SOURCE_MANUAL);
		} else if (current_state == LOCK_STATE_LOCKED) {
			set_state(LOCK_STATE_UNLOCKING, LOCK_OP_SOURCE_MANUAL);
		} else if (current_state == LOCK_STATE_UNLOCKED) {
			set_state(LOCK_STATE_LOCKING, LOCK_OP_SOURCE_MANUAL);
		}
		unlock_and_notify();
		return;
	}

	/*
	 * Motor running — react only to the TARGET sensor. The source sensor
	 * passes back through its activation zone as the bolt leaves, which
	 * would stop the motor early if it were checked here.
	 */
	if (motor_action == LOCK_ACTION_LOCK && lock_active) {
		if (!complete_armed) {
			k_timer_stop(&motor_timeout_timer);
			arm_complete(CONFIG_LOCK_BOLT_SEATING_MS);
		}
		set_state(LOCK_STATE_LOCKED, motor_source);
		unlock_and_notify();
		return;
	}

	if ((motor_action == LOCK_ACTION_UNLOCK ||
	     motor_action == LOCK_ACTION_OPEN) && unlock_active) {
		bool stopped = false;

		if (!complete_armed) {
			uint32_t duration_ms;

			k_timer_stop(&motor_timeout_timer);

			if (motor_action == LOCK_ACTION_OPEN) {
				duration_ms = CONFIG_LOCK_LATCH_RELEASE_MS;
			} else {
				duration_ms = CONFIG_LOCK_UNBOLT_SEATING_MS;
			}

			if (duration_ms == 0) {
				/*
				 * Stop inline rather than arming a 0 ms timer:
				 * the extra workqueue hop would keep the motor
				 * driving toward the latch.
				 */
				motor_brake(CONFIG_LOCK_MOTOR_BRAKE_US);
				motor_stop();
				stopped = true;
			} else {
				arm_complete(duration_ms);
			}
		}

		/*
		 * The bolt is retracted. An open keeps driving from here to hold
		 * the latch back, so this — not the later spring return — is when
		 * the door becomes openable. With no latch travel configured the
		 * motor stopped above and an open amounts to an unbolt.
		 */
		enum lock_state reached = LOCK_STATE_UNLOCKED;

		if (motor_action == LOCK_ACTION_OPEN &&
		    CONFIG_LOCK_LATCH_RELEASE_MS > 0) {
			reached = LOCK_STATE_UNLATCHED;
		}

		set_state(reached, motor_source);

		if (stopped) {
			finish_operation();
		}

		unlock_and_notify();
		return;
	}

	if (motor_action == LOCK_ACTION_LOCK) {
		set_state(LOCK_STATE_LOCKING, motor_source);
	} else if (motor_action == LOCK_ACTION_OPEN &&
		   current_state == LOCK_STATE_UNLATCHED) {
		/*
		 * Already past the bolt and holding the latch back; a sensor edge
		 * here is the bolt leaving the unlock sensor's zone, not a step
		 * backwards in the operation.
		 */
	} else {
		set_state(LOCK_STATE_UNLOCKING, motor_source);
	}

	unlock_and_notify();
}

static void motor_timeout_work_handler(struct k_work *work)
{
	k_mutex_lock(&lock_mutex, K_FOREVER);

	if (timeout_seq == operation_seq && motor_running) {
		enum lock_op_source source = motor_source;

		LOG_WRN("Lock: motor timeout, jammed");
		motor_stop();
		set_state(LOCK_STATE_JAMMED, source);
	}

	unlock_and_notify();
}

static void motor_complete_work_handler(struct k_work *work)
{
	k_mutex_lock(&lock_mutex, K_FOREVER);

	if (complete_seq == operation_seq && motor_running) {
		motor_brake(CONFIG_LOCK_MOTOR_BRAKE_US);
		motor_stop();
		finish_operation();
	}

	unlock_and_notify();
}

static void motor_timeout_expiry(struct k_timer *timer)
{
	k_work_submit(&motor_timeout_work);
}

static void motor_complete_expiry(struct k_timer *timer)
{
	k_work_submit(&motor_complete_work);
}

static int motor_gpio_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&motor_in1) ||
	    !gpio_is_ready_dt(&motor_in2)) {
		LOG_ERR("Lock: motor GPIO not ready");
		return -ENODEV;
	}

	/* nSLEEP is only present on rev A (DRV8837); rev B auto-sleeps. */
	if (motor_nsleep.port) {
		if (!gpio_is_ready_dt(&motor_nsleep)) {
			LOG_ERR("Lock: motor nSLEEP GPIO not ready");
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&motor_nsleep, GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	err = gpio_pin_configure_dt(&motor_in1, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&motor_in2, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	return 0;
}

/*
 * nRF54L15 GPIOs come up as floating inputs, so on rev A a drifting nSLEEP can
 * wake the H-bridge while IN1/IN2 float — observed as a short spin toward open
 * at power-on. Idling the pins at PRE_KERNEL_1, right after the GPIO controller,
 * shrinks that window to the reset-to-PRE_KERNEL_1 boot time.
 */
static int motor_safe_state_init(void)
{
	int err = motor_gpio_init();

	if (err) {
		LOG_WRN("Lock: early motor safe-state init failed: %d", err);
	}

	return err;
}

SYS_INIT(motor_safe_state_init, PRE_KERNEL_1,
	 UTIL_INC(CONFIG_GPIO_INIT_PRIORITY));

/* IN1=0 IN2=1 → reverse → lock direction */
static void start_motor_lock(void)
{
	if (motor_nsleep.port) {
		gpio_pin_set_dt(&motor_nsleep, 1);
	}
	gpio_pin_set_dt(&motor_in1, 0);
	gpio_pin_set_dt(&motor_in2, 1);
	motor_running = true;
}

/* IN1=1 IN2=0 → forward → unlock direction */
static void start_motor_unlock(void)
{
	if (motor_nsleep.port) {
		gpio_pin_set_dt(&motor_nsleep, 1);
	}
	gpio_pin_set_dt(&motor_in1, 1);
	gpio_pin_set_dt(&motor_in2, 0);
	motor_running = true;
}

int lock_manager_init(lock_state_changed_cb_t cb)
{
	int err;

	state_cb = cb;

	k_work_init(&motor_timeout_work, motor_timeout_work_handler);
	k_work_init(&motor_complete_work, motor_complete_work_handler);

	k_timer_init(&motor_timeout_timer, motor_timeout_expiry, NULL);
	k_timer_init(&motor_complete_timer, motor_complete_expiry, NULL);

	err = motor_gpio_init();
	if (err) {
		LOG_ERR("Lock: motor GPIO init failed: %d", err);
		return err;
	}

	err = gpio_input_init(&lock_sensor_input, &lock_sensor_gpio,
			      sensor_work_handler, 0);
	if (err) {
		LOG_ERR("Lock: lock sensor init failed: %d", err);
		return err;
	}

	err = gpio_input_init(&unlock_sensor_input, &unlock_sensor_gpio,
			      sensor_work_handler, 0);
	if (err) {
		LOG_ERR("Lock: unlock sensor init failed: %d", err);
		return err;
	}

	int lock_active = gpio_pin_get_dt(&lock_sensor_gpio);
	int unlock_active = gpio_pin_get_dt(&unlock_sensor_gpio);

	if (lock_active > 0) {
		current_state = LOCK_STATE_LOCKED;
	} else if (unlock_active > 0) {
		current_state = LOCK_STATE_UNLOCKED;
	} else {
		current_state = LOCK_STATE_UNKNOWN;
	}

	LOG_INF("Lock: init, state %s", lock_state_str(current_state));
	return 0;
}

enum lock_state lock_manager_get_state(void)
{
	return current_state;
}

int lock_manager_request(enum lock_action action, enum lock_op_source source,
			 const struct lock_originator *originator)
{
	/*
	 * Rejected before anything is touched. Past this point the request bumps
	 * operation_seq, invalidating whatever timed phases are pending, and an
	 * unsupported action must not be able to strand the lock that way.
	 */
	if (action != LOCK_ACTION_LOCK && action != LOCK_ACTION_UNLOCK &&
	    action != LOCK_ACTION_OPEN) {
		return -EINVAL;
	}

	k_mutex_lock(&lock_mutex, K_FOREVER);

	if (motor_running) {
		if (motor_action == action) {
			unlock_and_notify();
			return -EALREADY;
		}
		LOG_INF("Lock: overriding %d -> %d", motor_action, action);
		motor_stop();
		/* Let the motor current decay before reversing. */
		motor_brake(CONFIG_LOCK_MOTOR_BRAKE_US);
	} else {
		if (action == LOCK_ACTION_LOCK &&
			current_state == LOCK_STATE_LOCKED) {
			unlock_and_notify();
			return -EALREADY;
		}
		if (action == LOCK_ACTION_UNLOCK &&
			current_state == LOCK_STATE_UNLOCKED) {
			unlock_and_notify();
			return -EALREADY;
		}
		/*
		 * OPEN when already unlocked is valid: the bolt is retracted but
		 * the latch is engaged, so there is still latch travel to do.
		 */
	}

	/* Kept after the early returns so a redundant request changes nothing. */
	operation_seq++;

	motor_source = source;

	if (originator != NULL) {
		motor_originator = *originator;
	} else {
		motor_originator = (struct lock_originator){ 0 };
	}

	switch (action) {
	case LOCK_ACTION_LOCK:
		LOG_INF("Lock: locking");
		motor_action = LOCK_ACTION_LOCK;
		start_motor_lock();
		arm_timeout(CONFIG_LOCK_MOTOR_TIMEOUT_MS);
		break;

	case LOCK_ACTION_UNLOCK:
		LOG_INF("Lock: unlocking");
		motor_action = LOCK_ACTION_UNLOCK;
		start_motor_unlock();
		arm_timeout(CONFIG_LOCK_MOTOR_TIMEOUT_MS);
		break;

	case LOCK_ACTION_OPEN:
		LOG_INF("Lock: opening");
		motor_action = LOCK_ACTION_OPEN;
		start_motor_unlock();

		if (current_state == LOCK_STATE_UNLOCKED) {
			/*
			 * Bolt is already retracted, so the motor goes straight
			 * to holding the latch back without waiting on the unlock
			 * sensor — the lock is unlatched from here.
			 */
			set_state(LOCK_STATE_UNLATCHED, source);
			arm_complete(CONFIG_LOCK_LATCH_RELEASE_MS);
		} else {
			arm_timeout(CONFIG_LOCK_MOTOR_TIMEOUT_MS);
		}
		break;
	}

	unlock_and_notify();
	return 0;
}
