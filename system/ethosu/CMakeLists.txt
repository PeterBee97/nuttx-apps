# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SYSTEM_ETHOSU)
  nuttx_add_application(
    NAME
    ${CONFIG_SYSTEM_ETHOSU_PROGNAME}
    STACKSIZE
    ${CONFIG_SYSTEM_ETHOSU_STACKSIZE}
    PRIORITY
    ${CONFIG_SYSTEM_ETHOSU_PRIORITY}
    MODULE
    ${CONFIG_SYSTEM_ETHOSU}
    SRCS
    ethosu_main.c)
endif()
