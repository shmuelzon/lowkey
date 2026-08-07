cmake_minimum_required(VERSION 3.20.0)

# Derive the application version from git so it can be embedded in both
# the BLE Device Information Service and the Matter software version.
execute_process(
  COMMAND git describe --always --tags
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE PROJECT_VER
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT PROJECT_VER)
  set(PROJECT_VER "0.0.0")
endif()

# Derive the Matter software version (Basic Information cluster + OTA image
# header) from the git version and expose it to Kconfig via environment
# variables, consumed by the CHIP_DEVICE_SOFTWARE_VERSION* overrides in Kconfig.
# `git describe` gives "1.2.3" on a tagged commit, "1.2.3-5-gabc123" past a tag,
# or a bare hash when untagged; take the leading MAJOR.MINOR.PATCH and fall back
# to 0. Encode it as decimal MMmmpp (1.2.3 -> 10203, 10.54.1 -> 105401) so a
# newer tag is always a larger number, which is what the Matter OTA mechanism
# compares. Requires minor/patch < 100. Setting these before find_package() puts
# them in the environment inherited by the Kconfig invocation.
if(PROJECT_VER MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  math(EXPR matter_sw_version
       "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
else()
  set(matter_sw_version 0)
endif()
set(ENV{LOWKEY_SW_VERSION} "${matter_sw_version}")
set(ENV{LOWKEY_SW_VERSION_STR} "${PROJECT_VER}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lowkey)

target_include_directories(app PRIVATE src)

target_compile_definitions(app PRIVATE APP_VERSION_STRING="${PROJECT_VER}")

target_sources(app PRIVATE
  src/main.c
  src/lock_manager.c
  src/battery_manager.c
  src/door_monitor.c
  src/watchdog.c
  src/gpio_input.c
  src/ui.c
  # Implements the radio_*() API the application uses, fanning out to whichever
  # protocol backends below are enabled. Includes their headers by path
  # (src/ble/..., src/matter/...) rather than relying on those directories being
  # on the include path, since each is only added inside its own branch.
  src/radio.c
)

# Application-level factory-data accessor, shared by every build. All builds
# store data in the nRF Connect SDK factory-data CBOR layout and read it with
# the SDK's standalone C parser (FactoryDataParser.c). A build without Matter
# compiles that parser itself (it depends only on zcbor, so it links without
# the rest of the Matter stack); a Matter build already links it as part of the
# Matter library, so only its header include path is needed there.
if(NOT DEFINED ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR)
  message(FATAL_ERROR
    "ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR is not set; the Matter module is "
    "required for src/platform/nrfconnect/FactoryDataParser.{c,h}.")
endif()
set(matter_nrfconnect_dir
    ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/platform/nrfconnect)
target_sources(app PRIVATE src/factory_data.c)
target_include_directories(app PRIVATE ${matter_nrfconnect_dir})
if(NOT CONFIG_CHIP)
  target_sources(app PRIVATE ${matter_nrfconnect_dir}/FactoryDataParser.c)
endif()

if(CONFIG_LOCK_RADIO_BLE)
  target_include_directories(app PRIVATE src/ble)

  target_sources(app PRIVATE
    src/ble/ble_radio.c
    src/ble/ble_lock_service.c
    src/ble/ble_door_service.c
  )
endif()

if(CONFIG_LOCK_RADIO_MATTER)
  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
  include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)
  include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)
  include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)

  ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

  target_include_directories(app PRIVATE
    src/matter
    ${ZAP_PARENT_DIR}
  )

  target_sources(app PRIVATE
    src/matter/matter_radio.cpp
    src/matter/zcl_callbacks.cpp
  )

  ncs_configure_data_model()
endif()
