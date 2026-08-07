/*
 * Application-level factory-data accessor, shared by every build.
 *
 * The partition uses the nRF Connect SDK factory-data CBOR layout on every
 * build, so the SDK's standalone parser (FactoryDataParser.c) serves them all.
 * Top-level fields come from the parsed struct; per-device application values
 * live in the CBOR "user" map as decimal strings. See factory_data.h.
 */
#include "factory_data.h"

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <FactoryDataParser.h>

LOG_MODULE_REGISTER(factory_data, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Kept for the lifetime of the program: the parsed FactoryData struct holds
 * pointers into this buffer. Sized to the whole partition regardless of which
 * radios are built — with DAC/PAI certificates present, ParseFactoryData() has
 * to read all of it to reach the trailing "user" map, so a smaller buffer would
 * work in one configuration and silently fall back to defaults in another.
 */
#define FD_RAW_SIZE 4096
static uint8_t fd_raw[FD_RAW_SIZE];
static struct FactoryData fd;
static bool fd_valid;
static bool fd_parsed;

/* Read and parse the partition once; cache the result. */
static bool ensure_parsed(void)
{
	if (fd_parsed) {
		return fd_valid;
	}
	fd_parsed = true;

	const struct flash_area *fa;
	int rc = flash_area_open(FIXED_PARTITION_ID(factory_data), &fa);

	if (rc) {
		LOG_WRN("factory_data open failed: %d", rc);
		return false;
	}

	size_t len = MIN(fa->fa_size, sizeof(fd_raw));

	rc = flash_area_read(fa, 0, fd_raw, len);
	flash_area_close(fa);
	if (rc) {
		LOG_WRN("factory_data read failed: %d", rc);
		return false;
	}
	if (fd_raw[0] == 0xFF) {	/* erased partition */
		LOG_WRN("factory_data erased, using defaults");
		return false;
	}

	if (!ParseFactoryData(fd_raw, (uint16_t)len, &fd)) {
		LOG_WRN("factory_data parse failed");
		return false;
	}

	fd_valid = true;
	return true;
}

/* Read a decimal string value from the CBOR "user" map. */
static bool user_uint(const char *key, unsigned long *out)
{
	char buf[8];
	size_t out_len = 0;

	if (!ensure_parsed()) {
		return false;
	}

	if (!FindUserDataEntry(&fd, key, buf, sizeof(buf) - 1, &out_len) ||
	    out_len == 0 || out_len >= sizeof(buf)) {
		return false;
	}

	buf[out_len] = '\0';
	*out = strtoul(buf, NULL, 10);
	return true;
}

uint16_t factory_data_battery_capacity_mah(void)
{
	unsigned long v;

	/*
	 * Capped at 32767: the MAX17205 capacity LSB is 0.5 mAh at the 10 mOhm
	 * shunt, so the register value is twice this and must still fit uint16.
	 */
	if (user_uint("battery_capacity_mah", &v) && v > 0 && v <= INT16_MAX) {
		return (uint16_t)v;
	}

	return FACTORY_DATA_DEFAULT_BATTERY_CAP_MAH;
}

uint32_t factory_data_ble_passkey(uint32_t dflt)
{
	unsigned long v;

	if (user_uint("ble_passkey", &v) && v <= 999999) {
		return (uint32_t)v;
	}

	/*
	 * Loud, unlike the other accessors' fallbacks: this one is a pairing
	 * secret, and the compiled-in default is the same on every unit. A blob
	 * predating the ble_passkey field looks identical to a correctly
	 * provisioned one from the outside, so the log line is the only clue.
	 */
	LOG_WRN("factory_data: no ble_passkey, falling back to the built-in "
		"default — this device is not uniquely paired");

	return dflt;
}

bool factory_data_get_string(enum factory_data_string field,
			     const uint8_t **value, size_t *len)
{
	const struct FactoryDataString *s;

	if (!ensure_parsed()) {
		return false;
	}

	switch (field) {
	case FACTORY_DATA_STRING_SERIAL:
		s = &fd.sn;
		break;
	case FACTORY_DATA_STRING_VENDOR_NAME:
		s = &fd.vendor_name;
		break;
	case FACTORY_DATA_STRING_PRODUCT_NAME:
		s = &fd.product_name;
		break;
	case FACTORY_DATA_STRING_HW_VER:
		s = &fd.hw_ver_str;
		break;
	default:
		return false;
	}

	if (!s->data || s->len == 0) {
		return false;
	}

	*value = (const uint8_t *)s->data;
	*len = s->len;
	return true;
}
