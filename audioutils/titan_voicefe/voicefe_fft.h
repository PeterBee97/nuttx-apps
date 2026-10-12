/****************************************************************************
 * apps/audioutils/titan_voicefe/voicefe_fft.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_AUDIOUTILS_TITAN_VOICEFE_VOICEFE_FFT_H
#define __APPS_AUDIOUTILS_TITAN_VOICEFE_VOICEFE_FFT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOICEFE_FFT_SIZE 512

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Forward transform divides by 512 over nine stages. Inverse is unscaled.
 * Both arrays must contain VOICEFE_FFT_SIZE elements.
 */

void voicefe_fft(int32_t *real, int32_t *imag, bool inverse);

#endif /* __APPS_AUDIOUTILS_TITAN_VOICEFE_VOICEFE_FFT_H */
