#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(watchdog, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(CONFIG_LOCK_WATCHDOG_FEED_MS < CONFIG_LOCK_WATCHDOG_TIMEOUT_MS,
	     "The watchdog must be fed more often than it expires");

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int sysworkq_channel = -1;
static struct k_timer feed_timer;
static struct k_work feed_work;

static void wdt_expired(int channel_id, void *user_data)
{
	/*
	 * Runs from the task watchdog's own timer, so it reports which thread
	 * stopped feeding before the hardware watchdog resets the SoC. Nothing
	 * else is safe to do here.
	 */
	LOG_ERR("WDT: channel %d expired", channel_id);
}

int watchdog_channel_add(void)
{
	int channel = task_wdt_add(CONFIG_LOCK_WATCHDOG_TIMEOUT_MS, wdt_expired,
				   NULL);

	if (channel < 0) {
		LOG_ERR("WDT: channel add failed: %d", channel);
	}

	return channel;
}

void watchdog_channel_feed(int channel)
{
	int err;

	if (channel < 0) {
		return;
	}

	err = task_wdt_feed(channel);
	if (err) {
		LOG_ERR("WDT: feed of channel %d failed: %d", channel, err);
	}
}

static void feed_work_handler(struct k_work *work)
{
	watchdog_channel_feed(sysworkq_channel);
}

static void feed_timer_expiry(struct k_timer *timer)
{
	k_work_submit(&feed_work);
}

int watchdog_init(void)
{
	int err;

	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("WDT: device not ready");
		return -ENODEV;
	}

	err = task_wdt_init(wdt_dev);
	if (err) {
		LOG_ERR("WDT: init failed: %d", err);
		return err;
	}

	sysworkq_channel = watchdog_channel_add();
	if (sysworkq_channel < 0) {
		return sysworkq_channel;
	}

	k_work_init(&feed_work, feed_work_handler);
	k_timer_init(&feed_timer, feed_timer_expiry, NULL);
	k_timer_start(&feed_timer, K_MSEC(CONFIG_LOCK_WATCHDOG_FEED_MS),
		      K_MSEC(CONFIG_LOCK_WATCHDOG_FEED_MS));

	LOG_INF("WDT: init, %d ms timeout, %d ms feed",
		CONFIG_LOCK_WATCHDOG_TIMEOUT_MS, CONFIG_LOCK_WATCHDOG_FEED_MS);
	return 0;
}
