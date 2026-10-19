# SpeexDSP AEC subset

AEC, preprocessing, filterbank and KISS FFT sources from SpeexDSP 1.2.1:
https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz

Archive SHA256:
`8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`

Original BSD license and copyright notices are retained in COPYING and sources.
Local integration files: config.h, os_support_custom.h and
speex/speexdsp_config_types.h. Configuration selects floating point KISS FFT,
prefixes FFT symbols and uses a bounded, zeroed initialization arena. Only
initialization allocates; the selected cancellation path does not allocate.

Local mdf.c fixes cover multichannel reset bounds and mean microphone echo
indexing for residual preprocessing. Residual echo history also uses the
cancelled echo while the filter adapts, with full estimated leakage during
that phase. This avoids an all-zero startup residual spectrum.

This is an experimental diagnostic dependency, gated by
TESTING_TITAN_ASR_DUPLEX and ALLOW_BSD_COMPONENTS.
Residual preprocessing additionally requires TESTING_TITAN_ASR_RESIDUAL.
