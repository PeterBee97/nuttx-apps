/* SPDX-License-Identifier: Apache-2.0 */

  .section .rodata.titan_intent_models, "a"
  .balign 16
  .global g_titan_wake_model
  .type g_titan_wake_model, %object
g_titan_wake_model:
  .incbin TITAN_WAKE_MODEL
  .size g_titan_wake_model, . - g_titan_wake_model

  .balign 16
  .global g_titan_command_model
  .type g_titan_command_model, %object
g_titan_command_model:
  .incbin TITAN_COMMAND_MODEL
  .size g_titan_command_model, . - g_titan_command_model
