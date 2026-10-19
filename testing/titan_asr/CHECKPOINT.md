# Local Titan ASR and dual-microphone checkpoint

This diagnostic snapshot preserves the c12 acoustic model and c14 CPU search
experiments, plus the subsequent dual-microphone AEC experiments. It is not a
production accuracy acceptance or an upstream-ready patch set. Existing
diagnostic formatting and upstream SpeexDSP source formatting are retained.

Use `titan-mini:asrcheckpoint` for the earlier decoder checkpoint and
`titan-mini:asrduplex` for the October 19 tested duplex configuration. The latter
requires the matching NuttX SSI playback reference, PDM reference and SRAM wait
changes. Optional algorithms and diagnostics are Kconfig-controlled.
`titan_asr deep 3 0` compares runtime masks: 1=MVE, 2=prefix hash,
4=higher-order-first LM, 8=top-k early rejection. `titan_asr micro` measures
kernel costs separately.

Real-board CPU checks on 64 additional INT8 model outputs measured:

- Scalar: 290.191 ms.
- Exact search optimizations: 137.649 ms; complete results bitwise identical.
- Search plus MVE: 107.894 ms; text/intents/alignment identical, four tiny
  confidence differences (maximum total-confidence difference about 1.51e-8).

Host verification compared 102400 frame states across 256 genuine validation
outputs. Full state bytes matched except independently allocated LM pointers.
Make/CMake enabled builds have identical sets of 1882 global symbols; disabled
builds have identical sets of 1873 globals and baseline memory use.

Duplex modes are 0=raw mean, 1=dual-channel AEC mean, 2=AEC beam,
3=AEC beam and NS, 4=AEC beam and residual spectral suppression.
`titan_asr mic SECONDS MODE DUMP CAPTURE_ONLY` uses live microphones.
`titan_asr replay FILE BLOCKS MODE DUMP` uses interleaved L/R/reference PCM.
Diagnostic dumps contain six channels: resampled L/R/reference, cancelled L/R
and final mono. Buffered dumping requires external RAM; asynchronous dumping is
an alternative experiment. Capture and decoding use separate tasks with bounded
queues. Initialization uses a bounded SpeexDSP arena.

Playback testing uses the onboard ES8156 speaker beside the microphones, volume
register 0x14 = 0x80. USB CDC is only transport. Eight fixed board replays passed
with no processing failures. Exact repeated wake text can optionally authorize
the next command when KWS misses it; truncated or extended text does not open
that window. This fallback and detailed gate tracing default to disabled.
Negation, inline-prefix confirmation, onset retention and snapshot trimming are
separate experiments. Energy/noise adaptation is not a validated double-talk
detector.

A 60-second live far-end playback run processed 960000 samples with no failures,
queue peak 2 blocks, and frontend maximum 8.293 ms per 10 ms block. It produced
no utterances or actions. This is far-end stability evidence, not successful
live near-end speech or acoustic double-talk validation.

The latest Make/CMake builds have identical sets of 1931 global symbols.
The complete-text fallback alone added 32 bytes of MRAM and no static SRAM.
Disabling fallback and gate tracing produced identical load-image bytes to the
unmodified stream implementation. These size claims concern those two options,
not the whole duplex integration.

Normal-volume extended wake text and the old weak final-character truncation
remain unresolved. Fixed digital mixtures and host checks must not be described
as acoustic double-talk acceptance. Board tasks restore v89 after testing.
Detailed evidence is in workspace `artifacts/titan-wake-gate-20261019` and
`artifacts/titan-tail-stages-20261019`; earlier CPU measurements are in
`artifacts/asr-opt-c14`.
