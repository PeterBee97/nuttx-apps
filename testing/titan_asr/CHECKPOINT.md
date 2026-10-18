# Local ASR checkpoint before dual-microphone AEC work

This diagnostic snapshot preserves the c12 acoustic model and c14 CPU search
experiments. It is not a production accuracy acceptance or an upstream-ready
patch set. Existing diagnostic formatting is intentionally retained.

Use the matching NuttX `titan-mini:asrcheckpoint` profile. The MVE and search
experiments are Kconfig-controlled. `titan_asr deep 3 0` compares runtime masks:
1=MVE, 2=prefix hash, 4=higher-order-first LM, 8=top-k early rejection.
`titan_asr micro` measures kernel costs separately.

Real-board CPU checks on 64 additional INT8 model outputs measured:

- Scalar: 290.191 ms.
- Exact search optimizations: 137.649 ms; complete results bitwise identical.
- Search plus MVE: 107.894 ms; text/intents/alignment identical, four tiny
  confidence differences (maximum total-confidence difference about 1.51e-8).

Host verification compared 102400 frame states across 256 genuine validation
outputs. Full state bytes matched except independently allocated LM pointers.
Make/CMake enabled builds have identical sets of 1882 global symbols; disabled
builds have identical sets of 1873 globals and baseline memory use.

The present microphone path averages two channels; it is not a verified dual-mic
beamformer or AEC implementation. Playback testing uses the onboard ES8156 speaker
beside the microphones, volume register 0x14 = 0x80. USB CDC is only transport.
The current c12 acoustic accuracy remains unaccepted for negative/short utterances.
At the checkpoint the board was restored to the pre-optimization c12 image.

Detailed measurements and the tested firmware accompany this source in the
`asr-opt-c14` evidence directory of the Downloads backup.
