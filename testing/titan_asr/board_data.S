/* Binary assets must be assembled again when model contents change. */

.section .rodata.asr_model0,"a"
.balign 32
.global board_model0
board_model0:
.incbin ASR_MODEL0
.global board_model0_end
board_model0_end:
.section .rodata.asr_model1,"a"
.balign 32
.global board_model1
board_model1:
.incbin ASR_MODEL1
.global board_model1_end
board_model1_end:
.section .rodata.asr_lm,"a"
.balign 32
.global board_lm
board_lm:
.incbin ASR_LM
.global board_lm_end
board_lm_end:
.section .rodata.asr_fixtures,"a"
.balign 32
.global board_fixtures
board_fixtures:
.incbin ASR_FIXTURES
.global board_fixtures_end
board_fixtures_end:
