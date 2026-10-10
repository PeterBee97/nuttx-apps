/* SPDX-License-Identifier: Apache-2.0 */

  .section .rodata.ethosu_vww_model, "a"
  .balign 16
  .global g_ethosu_vww_model
  .global g_ethosu_vww_model_end
  .type g_ethosu_vww_model, %object

g_ethosu_vww_model:
  .incbin ETHOSU_VWW_MODEL
g_ethosu_vww_model_end:
  .size g_ethosu_vww_model, . - g_ethosu_vww_model
