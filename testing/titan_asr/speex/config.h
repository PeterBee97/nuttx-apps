/* SPDX-License-Identifier: Apache-2.0 */

#ifndef TITAN_SPEEX_CONFIG_H
#define TITAN_SPEEX_CONFIG_H

#define FLOATING_POINT
#define USE_KISS_FFT
#define OS_SUPPORT_CUSTOM
#define EXPORT

/* Avoid collisions with independently enabled KISS FFT users. */

#define kiss_fft_alloc titan_aec_fft_alloc
#define kiss_fft titan_aec_fft
#define kiss_fft_stride titan_aec_fft_stride
#define kiss_fftr_alloc titan_aec_fftr_alloc
#define kiss_fftr titan_aec_fftr
#define kiss_fftri titan_aec_fftri
#define kiss_fftr2 titan_aec_fftr2
#define kiss_fftri2 titan_aec_fftri2

#endif
