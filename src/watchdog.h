#ifndef WATCHDOG_H_
#define WATCHDOG_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure and start the watchdog.
 *
 * One task watchdog channel per thread the lock cannot function without, each
 * fed from that thread: the system workqueue (sensors, timers, GPIO work) and,
 * in a Matter build, the CHIP event loop. Monitoring only one would let a
 * wedged thread hide behind a healthy one — the CHIP thread in particular
 * serves every Matter command and nothing else touches it.
 *
 * Not covered: the Bluetooth RX thread, and main (which parks after init).
 *
 * @return 0 on success, negative errno on failure.
 */
int watchdog_init(void);

/**
 * Claim a watchdog channel for a thread that feeds itself.
 * @return channel id, or a negative errno.
 */
int watchdog_channel_add(void);

/** Feed a channel from watchdog_channel_add(). A negative channel is ignored. */
void watchdog_channel_feed(int channel);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H_ */
