#include "gpio_input.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gpio_input, CONFIG_LOG_DEFAULT_LEVEL);

static void debounce_timer_expiry(struct k_timer *timer)
{
	struct gpio_input *in = CONTAINER_OF(timer, struct gpio_input, timer);
	int level = gpio_pin_get_dt(in->spec);

	/*
	 * Drop edges that settled back to the level last reported: a transient
	 * latches a GPIOTE interrupt and starts the timer, but by expiry the pin
	 * is where it was, so there is nothing to report. Read errors (<0) fall
	 * through so the handler can log them.
	 */
	if (level >= 0 && level == in->last_level) {
		return;
	}

	if (level >= 0) {
		in->last_level = level;
	}

	k_work_submit(&in->work);
}

static void gpio_input_isr(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	struct gpio_input *in = CONTAINER_OF(cb, struct gpio_input, cb_data);

	if (in->debounce_ms) {
		k_timer_start(&in->timer, K_MSEC(in->debounce_ms), K_NO_WAIT);
	} else {
		k_work_submit(&in->work);
	}
}

int gpio_input_init(struct gpio_input *in, const struct gpio_dt_spec *spec,
		    k_work_handler_t handler, uint32_t debounce_ms)
{
	int err;

	in->spec = spec;
	in->debounce_ms = debounce_ms;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO not ready (port %s pin %u)", spec->port->name,
			spec->pin);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (err) {
		return err;
	}

	k_work_init(&in->work, handler);

	if (debounce_ms) {
		k_timer_init(&in->timer, debounce_timer_expiry, NULL);
		/* Seed the stable level so the first real edge is reported. */
		in->last_level = gpio_pin_get_dt(spec);
	}

	gpio_init_callback(&in->cb_data, gpio_input_isr, BIT(spec->pin));

	err = gpio_add_callback(spec->port, &in->cb_data);
	if (err) {
		return err;
	}

	return gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_BOTH);
}
