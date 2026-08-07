#ifndef FACTORY_DATA_H_
#define FACTORY_DATA_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fallback battery capacity used when no per-device value is provisioned. */
#define FACTORY_DATA_DEFAULT_BATTERY_CAP_MAH 5000

/**
 * Battery pack design capacity in mAh.
 *
 * Read once from the per-device factory_data partition, independent of the
 * radio backend. The partition uses the nRF Connect SDK factory-data CBOR
 * layout on every build; the value lives in the CBOR "user" map as the string
 * field `battery_capacity_mah` and is parsed with the SDK parser
 * (FactoryDataParser.c). Returns FACTORY_DATA_DEFAULT_BATTERY_CAP_MAH if the
 * partition is erased or the field is missing/invalid.
 */
uint16_t factory_data_battery_capacity_mah(void);

/*
 * The accessors below expose the top-level Device-Information string fields and
 * the BLE pairing passkey. They are compiled on every build but only used by
 * the BLE radio; the Matter build serves the equivalent values to its stack
 * through the SDK (DeviceInstanceInfoProvider / commissioning).
 */

/** Identifies a top-level factory-data string field. */
enum factory_data_string {
	FACTORY_DATA_STRING_SERIAL,       /* sn */
	FACTORY_DATA_STRING_VENDOR_NAME,  /* vendor_name */
	FACTORY_DATA_STRING_PRODUCT_NAME, /* product_name */
	FACTORY_DATA_STRING_HW_VER,       /* hw_ver_str */
};

/**
 * Fetch a factory-data string field (not NUL-terminated).
 *
 * On success @p value points into an internal cache that stays valid for the
 * lifetime of the program (do not free) and @p len holds its length. Returns
 * false if the partition is erased/invalid or the field is absent.
 */
bool factory_data_get_string(enum factory_data_string field,
			     const uint8_t **value, size_t *len);

/**
 * BLE pairing passkey provisioned in the factory_data partition.
 *
 * Returns the passkey (0-999999), or @p dflt if the partition is erased/invalid
 * or the field is missing/out of range.
 */
uint32_t factory_data_ble_passkey(uint32_t dflt);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_DATA_H_ */
