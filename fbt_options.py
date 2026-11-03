from pathlib import Path
import posixpath

# For more details on these options, run 'fbt -h'

FIRMWARE_ORIGIN = "Unleashed"

# Default hardware target
TARGET_HW = 7

# Optimization flags
## Optimize for size
COMPACT = 1
## Optimize for debugging experience
DEBUG = 1

# Suffix to add to files when building distribution
# If OS environment has DIST_SUFFIX set, it will be used instead
DIST_SUFFIX = "local"

# Coprocessor firmware
# Full stack moves SFSA (0xD7 -> 0xCE), so use the OB set that does not pin it
COPRO_OB_DATA = "scripts/ob_custradio.data"

# Must match lib/stm32wb_copro version
COPRO_CUBE_VERSION = "1.20.0"

COPRO_CUBE_DIR = "lib/stm32wb_copro"

# Default radio stack
# Full stack: central/observer roles. Installs at 0x080CE000 on 1M parts (copro 1.20.0
# release notes), so firmware must stay below 843776 bytes. Does not fit debug builds.
COPRO_STACK_BIN = "stm32wb5x_BLE_Stack_full_fw.bin"
COPRO_STACK_TYPE = "ble_full"

# Full stack 1.20.0 on a 1M WB55, from lib/stm32wb_copro/firmware/Release_Notes.html.
# Re-verify after any copro update. Set to "0x0" to let scripts calculate it.
COPRO_STACK_ADDR = "0x080CE000"

# If you override COPRO_CUBE_DIR on commandline, override this as well
COPRO_STACK_BIN_DIR = posixpath.join(COPRO_CUBE_DIR, "firmware")

# Supported toolchain versions
# Also specify in scripts/ufbt/SConstruct
FBT_TOOLCHAIN_VERSIONS = (" 12.3.", " 13.2.")

OPENOCD_OPTS = [
    "-f",
    "interface/stlink.cfg",
    "-c",
    "transport select hla_swd",
    "-f",
    "${FBT_DEBUG_DIR}/stm32wbx.cfg",
    "-c",
    "stm32wbx.cpu configure -rtos auto",
]

SVD_FILE = "${FBT_DEBUG_DIR}/STM32WB55_CM4.svd"

# Look for blackmagic probe on serial ports and local network
BLACKMAGIC = "auto"

# Application to start on boot
LOADER_AUTOSTART = ""

FIRMWARE_APPS = {
    "default": [
        # Svc
        "basic_services",
        # Apps
        "main_apps",
        "system_apps",
        # Settings
        "settings_apps",
    ],
    "unit_tests": [
        "basic_services",
        "updater_app",
        "radio_device_cc1101_ext",
        "unit_tests",
        "js_app",
        "infrared",
        "archive",
    ],
}

FIRMWARE_APP_SET = "default"

custom_options_fn = "fbt_options_local.py"

if Path(custom_options_fn).exists():
    exec(compile(Path(custom_options_fn).read_text(), custom_options_fn, "exec"))
