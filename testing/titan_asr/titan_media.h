/****************************************************************************
 * apps/testing/titan_asr/titan_media.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef TITAN_MEDIA_H
#define TITAN_MEDIA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct titan_media_s
{
  int fd;
  uint32_t bytes;
  uint32_t frames;
  char path[128];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int titan_media_open(struct titan_media_s *media, const char *directory,
                     const char *suffix);
int titan_media_write(struct titan_media_s *media, const void *data,
                      size_t bytes);
int titan_media_header(struct titan_media_s *media, bool video);
int titan_media_finish(struct titan_media_s *media, bool video);
int titan_media_photo(struct titan_media_s *media, const uint8_t *yuyv);

#endif /* TITAN_MEDIA_H */
