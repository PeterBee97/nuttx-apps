/* SPDX-License-Identifier: Apache-2.0 */

#ifndef TITAN_SPEEX_OS_H
#define TITAN_SPEEX_OS_H

#define OVERRIDE_SPEEX_ALLOC
#define OVERRIDE_SPEEX_FREE

void *asr_aec_allocate(int size);

#define speex_alloc asr_aec_allocate
#define speex_free(ptr) ((void)(ptr))

#endif
