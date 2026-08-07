#include "battery_manager.h"
#include "gpio_input.h"
#include "factory_data.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery_manager, CONFIG_LOG_DEFAULT_LEVEL);

/* MAX17205 register addresses (primary I2C address 0x36) */
#define MAX17205_REG_STATUS        0x00
#define MAX17205_REG_REP_CAP       0x05
#define MAX17205_REG_REP_SOC       0x06
#define MAX17205_REG_VCELL         0x09
#define MAX17205_REG_AVG_VCELL     0x19
#define MAX17205_REG_PACK_CFG      0xBD
#define MAX17205_REG_FSTAT         0x3D

/* ModelGauge m5 EZ configuration registers */
#define MAX17205_REG_DESIGN_CAP    0x18
#define MAX17205_REG_FULL_CAP      0x10
#define MAX17205_REG_MIX_CAP       0x0F
#define MAX17205_REG_FULL_CAP_NOM  0x23
#define MAX17205_REG_FULL_CAP_REP  0x35
#define MAX17205_REG_VFSOC         0xFF	/* voltage-fuel-gauge SoC, 1/256 %/LSB */
#define MAX17205_REG_ICHG_TERM     0x1E
#define MAX17205_REG_V_EMPTY       0x3A
#define MAX17205_REG_DQACC         0x45
#define MAX17205_REG_DPACC         0x46
#define MAX17205_REG_MODEL_CFG     0xDB
#define MAX17205_REG_HIB_CFG       0xBA
#define MAX17205_REG_COMMAND       0x60

/*
 * FullCapNom = dQAcc / dPAcc, so DesignCap alone does not set the gauge's full
 * capacity — dQAcc/dPAcc must be seeded to match, or they stay at their power-
 * on defaults (dQAcc 368 mAh / dPAcc 25% → FullCapNom ~1472 mAh). Per Maxim
 * AN6358 (mirrored by Zephyr's in-tree max17055/max17262 m5 drivers) the seed
 * that makes FullCapNom == DesignCap is:
 *   dQAcc = DesignCap / 32
 *   dPAcc = dQAcc * 44138 / DesignCap
 */
#define MAX17205_DPACC_SCALE       44138U

/* Soft-wakeup command to force-exit hibernate before (re)configuration */
#define MAX17205_CMD_SOFT_WAKEUP   0x0090

/* ModelCfg refresh-in-progress bit (self-clears when the model is applied) */
#define MAX17205_MODEL_CFG_REFRESH BIT(15)

/*
 * EZ config values for a standard 4.2 V Li-ion pack (per Maxim AN6358):
 *   - Capacity LSB = 5.0 µVh / Rsense = 0.5 mAh    → mAh * 2
 *   - Current  LSB = 1.5625 µV / Rsense = 156.25 µA → µA * 32 / 5000
 *   - VEmpty: VE (bits 15:7, 10 mV) = 3.3 V, VR (bits 6:0, 40 mV) = 3.88 V
 *   - ModelCfg: Refresh + ModelID 0 (Li-ion); use 0x8400 instead for cells
 *     charged above 4.275 V (4.35/4.4 V high-voltage Li-ion).
 *
 * Design capacity is the (user-selectable) pack rating, read per-device from
 * factory data; the charge-termination current is a fixed property of the
 * charger hardware, taken from devicetree.
 */
#define MAX17205_CAP_TO_LSB(mah)      ((uint16_t)((mah) * 2))
#define MAX17205_ITERM_LSB \
	((uint16_t)((uint32_t)DT_PROP(DT_NODELABEL(max17205), \
				      charge_term_current_microamp) * 32 / 5000))
#define MAX17205_V_EMPTY_DEFAULT      0xA561
#define MAX17205_MODEL_CFG_LIION_42V  0x8000

/* Status register bits */
#define MAX17205_STATUS_POR        BIT(1)

/* FSTAT register bits */
#define MAX17205_FSTAT_DNR         BIT(0)

/*
 * How long the gauge may take to bring its registers up after power is applied,
 * before FSTAT.DNR clears. The MAX17201/MAX17215 datasheet gives 445 ms typical
 * and 1.845 s maximum; the timeout is past the maximum with margin, because
 * cutting it short is what takes battery reporting down on a slow part.
 */
#define MAX17205_DNR_TIMEOUT_MS    2500
#define MAX17205_DNR_POLL_MS       10

/*
 * PackCfg register layout for 2S, no thermistor, no per-cell monitoring:
 *   [3:0]   NCELLS-1 = 1  (2S pack)
 *   [7:4]   reserved / 0
 *   [8]     ChEn = 0      (no cell channel measurement)
 *   [12:9]  reserved / 0
 *   [15:13] ThmCfg = 0b000 (thermistor disabled)
 */
#define MAX17205_PACK_CFG_2S_NO_NTC  0x0001

/*
 * Voltage scaling: AvgVCell LSB = 78.125 µV per cell = 5/64 mV per cell.
 * Pack voltage = raw * (5/64) mV * NUM_CELLS.
 * For a 2S pack this is raw * 10 / 64 = raw * 5/32.
 */
#define MAX17205_NUM_CELLS  2

static const struct i2c_dt_spec max17205_dev =
	I2C_DT_SPEC_GET(DT_NODELABEL(max17205));

static const struct gpio_dt_spec pwr_good_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(chg_power_good), gpios);
static const struct gpio_dt_spec chg_stat_gpio =
	GPIO_DT_SPEC_GET(DT_ALIAS(chg_stat), gpios);

static const struct battery_manager_cb *cbs;
static uint8_t cached_soc;
static uint16_t cached_voltage_mv;
static uint16_t cached_remaining_mah;
static bool cached_charging;
static bool cached_complete;

static struct k_timer poll_timer;
static struct k_work poll_work;
static struct k_work charger_work;

/*
 * The gauge has answered with its data-not-ready flag down, so its registers
 * mean something. Set by whichever of init or the poll path first sees it.
 */
static bool gauge_ready;

/*
 * Gates the poll path until init has finished with the gauge. Both talk to the
 * same device, and the configuration each may run is a multi-second
 * conversation that must not be interleaved — the charger GPIO interrupts are
 * live before the gauge is brought up, so an edge arriving mid-init would
 * otherwise start a second one on the system workqueue.
 */
static bool poll_enabled;

static struct gpio_input pwr_good_input;
static struct gpio_input chg_stat_input;

static int max17205_read16(uint8_t reg, uint16_t *val)
{
	uint8_t buf[2];
	int err;

	err = i2c_burst_read_dt(&max17205_dev, reg, buf, sizeof(buf));
	if (err) {
		return err;
	}

	/* MAX17205 uses little-endian register format */
	*val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
	return 0;
}

static int max17205_write16(uint8_t reg, uint16_t val)
{
	uint8_t buf[3] = { reg, val & 0xFF, val >> 8 };

	return i2c_write_dt(&max17205_dev, buf, sizeof(buf));
}

static int max17205_read_soc(uint8_t *soc_percent)
{
	uint16_t raw;
	int err;

	err = max17205_read16(MAX17205_REG_REP_SOC, &raw);
	if (err) {
		return err;
	}

	/* RepSOC: MSB is integer percentage, LSB is 1/256% fraction */
	uint8_t soc = raw >> 8;

	*soc_percent = MIN(soc, 100);
	return 0;
}

static int max17205_read_voltage(uint16_t *voltage_mv)
{
	uint16_t raw;
	int err;

	err = max17205_read16(MAX17205_REG_AVG_VCELL, &raw);
	if (err) {
		return err;
	}

	/* Pack voltage = raw * (5/64 mV per cell) * NUM_CELLS */
	*voltage_mv = (uint16_t)((uint32_t)raw * 5 * MAX17205_NUM_CELLS / 64);
	return 0;
}

static int max17205_read_remaining(uint16_t *remaining_mah)
{
	uint16_t raw;
	int err;

	err = max17205_read16(MAX17205_REG_REP_CAP, &raw);
	if (err) {
		return err;
	}

	/* Capacity LSB = 5.0 µVh / Rsense = 0.5 mAh at the 10 mOhm sense
	 * resistor (DT sense-resistor-micro-ohms = 10000). */
	*remaining_mah = (uint16_t)((uint32_t)raw / 2);
	return 0;
}

/* Keeps the first error so a half-applied configuration is still reported. */
static void write_checked(uint8_t reg, uint16_t val, int *err)
{
	int ret = max17205_write16(reg, val);

	if (ret && !*err) {
		*err = ret;
	}
}

/*
 * ModelGauge m5 EZ configuration (AN6358). Runs once per power-on, after the
 * data-not-ready flag clears. The hibernate exit/restore is required for the
 * configuration writes to take effect.
 *
 * Every write is checked: the caller clears the POR bit only on success, and a
 * bus glitch here would otherwise leave the gauge mis-configured permanently,
 * since POR is never set again.
 */
static int max17205_ez_config(void)
{
	uint16_t hibcfg, modelcfg;
	uint16_t design_cap_mah = factory_data_battery_capacity_mah();
	uint16_t design_cap = MAX17205_CAP_TO_LSB(design_cap_mah);
	uint16_t d_qacc = design_cap / 32;
	uint16_t d_pacc = (uint16_t)((uint32_t)d_qacc * MAX17205_DPACC_SCALE /
				     design_cap);
	int err = 0;

	LOG_INF("Bat: EZ config, DesignCap %u mAh (dQAcc 0x%04x, dPAcc 0x%04x)",
		design_cap_mah, d_qacc, d_pacc);

	int rc = max17205_read16(MAX17205_REG_HIB_CFG, &hibcfg);

	if (rc) {
		return rc;
	}

	/* Force-exit hibernate so config writes are accepted. */
	write_checked(MAX17205_REG_COMMAND, MAX17205_CMD_SOFT_WAKEUP, &err);
	write_checked(MAX17205_REG_HIB_CFG, 0x0000, &err);
	write_checked(MAX17205_REG_COMMAND, 0x0000, &err);

	/*
	 * Seed capacity so the model refresh resolves FullCapNom == DesignCap.
	 * Order/values mirror the AN6358 sequence in Zephyr's max17055 driver.
	 */
	write_checked(MAX17205_REG_DESIGN_CAP, design_cap, &err);
	write_checked(MAX17205_REG_DQACC, d_qacc, &err);
	write_checked(MAX17205_REG_ICHG_TERM, MAX17205_ITERM_LSB, &err);
	write_checked(MAX17205_REG_V_EMPTY, MAX17205_V_EMPTY_DEFAULT, &err);
	write_checked(MAX17205_REG_DPACC, d_pacc, &err);
	write_checked(MAX17205_REG_MODEL_CFG, MAX17205_MODEL_CFG_LIION_42V, &err);

	if (err) {
		return err;
	}

	/*
	 * Refresh self-clears when the model is applied. AN6358 specifies this
	 * takes ~1 s, so poll generously (up to ~3 s) — too tight a window
	 * leaves FullCapNom at its stale value and SoC computed against the
	 * wrong full capacity.
	 */
	for (int i = 0; i < 300; i++) {
		rc = max17205_read16(MAX17205_REG_MODEL_CFG, &modelcfg);
		if (rc) {
			return rc;
		}
		if (!(modelcfg & MAX17205_MODEL_CFG_REFRESH)) {
			break;
		}
		k_msleep(10);
	}

	if (modelcfg & MAX17205_MODEL_CFG_REFRESH) {
		LOG_WRN("Bat: ModelCfg refresh timeout");
	}

	/*
	 * The refresh recalls the capacity registers from nonvolatile nFullCapNom
	 * (a stale ~1500 mAh on this part) and initialises MixCap/RepCap against
	 * that wrong full-scale. dQAcc/dPAcc are seeded correctly so learning
	 * would converge over a cycle, but reconstruct the state in RAM so SoC is
	 * right from the first boot without touching NVM. Seeding MixCap/RepCap
	 * from VFSOC as well as the FullCap registers is what makes
	 * RepSOC = RepCap/FullCapRep agree with the voltage gauge; forcing
	 * FullCap alone leaves RepSOC = oldMixCap/DesignCap (~25%).
	 */
	{
		uint16_t vfsoc = 0;
		uint8_t soc_pct;
		uint16_t seed_cap;

		max17205_read16(MAX17205_REG_VFSOC, &vfsoc);
		soc_pct = MIN((uint8_t)(vfsoc >> 8), 100);
		seed_cap = (uint16_t)((uint32_t)design_cap * soc_pct / 100);

		write_checked(MAX17205_REG_FULL_CAP_NOM, design_cap, &err);
		write_checked(MAX17205_REG_FULL_CAP, design_cap, &err);
		write_checked(MAX17205_REG_FULL_CAP_REP, design_cap, &err);
		write_checked(MAX17205_REG_MIX_CAP, seed_cap, &err);
		write_checked(MAX17205_REG_REP_CAP, seed_cap, &err);

		LOG_INF("Bat: seeded charge state from VFSOC %u%% (%u mAh)",
			soc_pct, seed_cap / 2);
	}

	/* Restore the original hibernate configuration. */
	write_checked(MAX17205_REG_HIB_CFG, hibcfg, &err);

	return err;
}

/* Apply the full configuration. The caller has confirmed POR is set. */
static int max17205_configure(void)
{
	uint16_t status;
	int err;

	LOG_INF("Bat: MAX17205 POR set, configuring");

	err = max17205_write16(MAX17205_REG_PACK_CFG,
			       MAX17205_PACK_CFG_2S_NO_NTC);
	if (err) {
		LOG_ERR("Bat: PackCfg write failed: %d", err);
		return err;
	}

	err = max17205_ez_config();
	if (err) {
		LOG_ERR("Bat: EZ config failed: %d", err);
		return err;
	}

	err = max17205_read16(MAX17205_REG_STATUS, &status);
	if (err) {
		return err;
	}

	/*
	 * Cleared last and only on success: POR is the gauge's own record that
	 * it is unconfigured, and it is never set again by anything but a power
	 * cycle. Clearing it after a partially applied configuration would make
	 * that state permanent.
	 */
	err = max17205_write16(MAX17205_REG_STATUS,
			       status & ~MAX17205_STATUS_POR);
	if (err) {
		LOG_ERR("Bat: status clear failed: %d", err);
	}

	return err;
}

/*
 * Configure the gauge if it says it needs it, cheaply enough to run on every
 * poll.
 *
 * POR is set whenever the gauge loses power, not only at system boot: a pack
 * swap or a deep-discharge recovery brings it back with its configuration gone
 * and its SoC meaningless. Checking here rather than at init only also retries
 * a configuration that failed at boot on a transient bus error.
 *
 * Costs one register read when there is nothing to do, and nothing at all when
 * the part is absent — the read fails immediately. Configuring does block the
 * system workqueue for up to ~3 s waiting on the model refresh, which is why
 * repeated failures eventually give up rather than paying that every minute.
 */
#define MAX17205_CONFIG_ATTEMPTS 5

static uint8_t config_failures;

static void max17205_check_config(void)
{
	uint16_t status;

	if (max17205_read16(MAX17205_REG_STATUS, &status)) {
		return;
	}

	if (!(status & MAX17205_STATUS_POR)) {
		config_failures = 0;
		return;
	}

	if (config_failures >= MAX17205_CONFIG_ATTEMPTS) {
		return;
	}

	if (max17205_configure()) {
		if (++config_failures >= MAX17205_CONFIG_ATTEMPTS) {
			LOG_ERR("Bat: giving up configuring the gauge after %u "
				"attempts; readings are not trustworthy",
				config_failures);
		}
	}
}

/*
 * Whether the gauge is usable: 0 once its data-not-ready flag is down, -EAGAIN
 * while it is still bringing its registers up, and the bus error if it did not
 * answer at all. The two failures are worth keeping apart — one is worth
 * waiting out, the other means there is nothing there to wait for.
 *
 * Latched on the first success, so it costs one register read per call before
 * the gauge comes up and nothing afterwards.
 *
 * Deliberately not fatal to the caller. A gauge that is merely slow, or a board
 * with none fitted, must not take battery polling down with it: aborting init
 * left the poll timer unstarted, and so nothing to notice the part when it did
 * come up.
 */
static int max17205_ensure_ready(void)
{
	uint16_t fstat;
	int err;

	if (gauge_ready) {
		return 0;
	}

	err = max17205_read16(MAX17205_REG_FSTAT, &fstat);
	if (err) {
		return err;
	}

	if (fstat & MAX17205_FSTAT_DNR) {
		return -EAGAIN;
	}

	gauge_ready = true;
	LOG_INF("Bat: MAX17205 ready");
	return 0;
}

static void poll_work_handler(struct k_work *work)
{
	uint8_t soc;
	uint16_t voltage_mv;
	uint16_t remaining_mah;

	if (!poll_enabled || max17205_ensure_ready()) {
		return;
	}

	max17205_check_config();

	if (max17205_read_soc(&soc)) {
		LOG_WRN("Bat: SOC read failed");
		return;
	}

	if (max17205_read_voltage(&voltage_mv)) {
		LOG_WRN("Bat: voltage read failed");
		return;
	}

	if (max17205_read_remaining(&remaining_mah)) {
		LOG_WRN("Bat: remaining capacity read failed");
		return;
	}

	LOG_INF("Bat: SOC %u%%, %u mV, %u mAh", soc, voltage_mv, remaining_mah);

	bool changed = (soc != cached_soc) ||
		       (voltage_mv != cached_voltage_mv) ||
		       (remaining_mah != cached_remaining_mah);

	cached_soc = soc;
	cached_voltage_mv = voltage_mv;
	cached_remaining_mah = remaining_mah;

	if (changed && cbs && cbs->on_battery_changed) {
		cbs->on_battery_changed(soc, voltage_mv, remaining_mah);
	}
}

static void poll_timer_expiry(struct k_timer *timer)
{
	k_work_submit(&poll_work);
}

static void refresh_charger_state(void)
{
	int power_good = gpio_pin_get_dt(&pwr_good_gpio);
	int charging = gpio_pin_get_dt(&chg_stat_gpio);

	if (power_good < 0 || charging < 0) {
		LOG_WRN("Bat: charger GPIO read error: %d / %d",
			power_good, charging);
		return;
	}

	LOG_INF("Bat: power_good=%d charging=%d", power_good, charging);

	cached_charging = power_good && charging;
	cached_complete = power_good && !charging;
}

static void charger_work_handler(struct k_work *work)
{
	refresh_charger_state();

	if (cbs && cbs->on_charger_changed) {
		cbs->on_charger_changed(cached_charging, cached_complete);
	}

	k_work_submit(&poll_work);
}

int battery_manager_init(const struct battery_manager_cb *app_cbs)
{
	int err;

	cbs = app_cbs;

	k_work_init(&poll_work, poll_work_handler);
	k_work_init(&charger_work, charger_work_handler);

	/*
	 * Charger status is plain GPIO and independent of the gauge, so it is
	 * set up first and kept even when the I2C part is absent or failing —
	 * losing the fuel gauge should not also cost charge indication and the
	 * Matter BatChargeState.
	 */
	err = gpio_input_init(&pwr_good_input, &pwr_good_gpio,
			      charger_work_handler, 0);
	if (err) {
		LOG_ERR("Bat: power-good GPIO init failed: %d", err);
		return err;
	}

	err = gpio_input_init(&chg_stat_input, &chg_stat_gpio,
			      charger_work_handler, 0);
	if (err) {
		LOG_ERR("Bat: charge status GPIO init failed: %d", err);
		return err;
	}

	/*
	 * Published from here rather than by submitting charger_work: that item
	 * re-reads the pins and chains into a poll, which would run the gauge's
	 * configuration on the system workqueue at the same time as the sequence
	 * below runs it on this thread. Two interleaved EZ-config sequences is
	 * what the duplicated "POR set, configuring" at boot was.
	 */
	refresh_charger_state();
	if (cbs && cbs->on_charger_changed) {
		cbs->on_charger_changed(cached_charging, cached_complete);
	}

	if (!i2c_is_ready_dt(&max17205_dev)) {
		LOG_ERR("Bat: MAX17205 I2C not ready");
		return -ENODEV;
	}

	/*
	 * Wait out the gauge's own power-up. Anything other than -EAGAIN ends
	 * the wait immediately, so a board with no gauge fitted costs one
	 * transaction rather than the whole timeout.
	 */
	for (int i = 0; i < MAX17205_DNR_TIMEOUT_MS / MAX17205_DNR_POLL_MS; i++) {
		err = max17205_ensure_ready();
		if (err != -EAGAIN) {
			break;
		}
		k_msleep(MAX17205_DNR_POLL_MS);
	}

	if (!err) {
		max17205_check_config();

		/* Synchronous, so the getters are valid as soon as init returns. */
		max17205_read_soc(&cached_soc);
		max17205_read_voltage(&cached_voltage_mv);
		max17205_read_remaining(&cached_remaining_mah);

		LOG_INF("Bat: init, SOC %u%%, %u mV, %u mAh", cached_soc,
			cached_voltage_mv, cached_remaining_mah);
	} else {
		LOG_WRN("Bat: MAX17205 not usable at boot (%d); the poll will "
			"keep retrying", err);
	}

	/*
	 * Started either way. The gauge is the only thing that might be missing,
	 * and the poll is what picks it up if it appears later.
	 */
	poll_enabled = true;
	k_timer_init(&poll_timer, poll_timer_expiry, NULL);
	k_timer_start(&poll_timer,
		      K_SECONDS(CONFIG_LOCK_BATTERY_POLL_INTERVAL_S),
		      K_SECONDS(CONFIG_LOCK_BATTERY_POLL_INTERVAL_S));

	return 0;
}

uint8_t battery_get_soc(void)
{
	return cached_soc;
}

uint16_t battery_get_voltage(void)
{
	return cached_voltage_mv;
}

uint16_t battery_get_remaining_mah(void)
{
	return cached_remaining_mah;
}

void battery_manager_poll(void)
{
	k_work_submit(&poll_work);
}

bool battery_is_charging(void)
{
	return cached_charging;
}

bool battery_is_charge_complete(void)
{
	return cached_complete;
}
