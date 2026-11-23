# SPDX-License-Identifier: Apache-2.0

board_set_flasher_ifnset(esp32)
board_set_debugger_ifnset(openocd)

board_runner_args(openocd  --no-halt --no-targets --no-load --target-handle _TARGETNAME_0)

# Set hardware watchpoint limit based on SOC capabilities
if(CONFIG_SOC_SERIES_ESP32C3)
  set(ESP_HW_WATCHPOINT_LIMIT 8)
elseif(CONFIG_SOC_SERIES_ESP32C6 OR CONFIG_SOC_SERIES_ESP32H2 OR CONFIG_SOC_SERIES_ESP32P4)
  set(ESP_HW_WATCHPOINT_LIMIT 4)
elseif(CONFIG_SOC_SERIES_ESP32C5)
  set(ESP_HW_WATCHPOINT_LIMIT 3)
else()
  # ESP32, ESP32-S2, ESP32-S3, ESP32-C2 all have 2 watchpoints
  set(ESP_HW_WATCHPOINT_LIMIT 2)
endif()
board_runner_args(openocd  --gdb-init "set remote hardware-watchpoint-limit ${ESP_HW_WATCHPOINT_LIMIT}")

# Flash address of the application image, which is not CONFIG_FLASH_LOAD_OFFSET
# with simple boot. Keep in step with --esp-app-address in
# soc/espressif/common/CMakeLists.txt; that runs after the openocd runner
# arguments are finalized, so it cannot pass this on.
if(CONFIG_ESP_SIMPLE_BOOT AND NOT (CONFIG_SOC_ESP32C5_LPCORE OR CONFIG_SOC_ESP32C6_LPCORE OR CONFIG_SOC_ESP32P4_LPCORE))
  dt_nodelabel(esp_image_part_path NODELABEL "boot_partition")
else()
  dt_chosen(esp_image_part_path PROPERTY "zephyr,code-partition")
endif()
dt_reg_addr(esp_image_off PATH ${esp_image_part_path})
if(DEFINED CONFIG_BUILD_OUTPUT_ADJUST_LMA)
  math(EXPR esp_image_off "${esp_image_off} + ${CONFIG_BUILD_OUTPUT_ADJUST_LMA}" OUTPUT_FORMAT HEXADECIMAL)
endif()

board_runner_args(openocd  --gdb-init "maintenance flush register-cache")
board_runner_args(openocd  --gdb-init "mon esp appimage_offset ${esp_image_off}")
board_runner_args(openocd  --gdb-init "mon reset halt")
board_runner_args(openocd  --gdb-init "thb main")

board_runner_args(openocd  --file-type=bin)
board_runner_args(openocd  "--flash-address=${esp_image_off}")
board_runner_args(openocd  --cmd-load "program_esp")
board_runner_args(openocd  --cmd-verify "esp verify_bank_hash 0")

set(ESP_IDF_PATH ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR})
assert(ESP_IDF_PATH "ESP_IDF_PATH is not set")

board_runner_args(esp32 "--esp-idf-path=${ESP_IDF_PATH}")
