#ifndef GPIO_INPUT_H_
#define GPIO_INPUT_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Interrupt-driven GPIO input, optionally debounced. */
struct gpio_input {
	const struct gpio_dt_spec *spec;
	struct gpio_callback cb_data;
	struct k_timer timer;
	struct k_work work;
	uint32_t debounce_ms;
	int last_level;
};

/**
 * Configure @p spec as an interrupt-driven input and route both edges to
 * @p handler on the system workqueue.
 *
 * @p debounce_ms > 0 restarts a one-shot timer on every edge and only submits
 * the work once the pin has been stable for that long; an edge that settles
 * back to the last reported level is dropped. Use this for mechanical contacts.
 *
 * @p debounce_ms == 0 submits the work straight from the ISR. Solid-state
 * outputs (Hall-effect sensors, charger status pins) do not bounce, and the
 * timer would only add latency — on the position sensors that latency lands
 * between the endstop firing and the motor stopping. Handlers must re-read the
 * pin and tolerate being run more often than the level changes.
 *
 * @param in          Context, caller-allocated, must outlive the input.
 * @param spec        GPIO dt-spec for the pin.
 * @param handler     Work handler, run on the system workqueue.
 * @param debounce_ms Debounce interval, or 0 for none.
 * @return 0 on success, negative errno on failure.
 */
int gpio_input_init(struct gpio_input *in, const struct gpio_dt_spec *spec,
		    k_work_handler_t handler, uint32_t debounce_ms);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_INPUT_H_ */
