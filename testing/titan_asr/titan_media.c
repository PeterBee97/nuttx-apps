/****************************************************************************
 * apps/testing/titan_asr/titan_media.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "titan_media.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void media_u16(uint8_t *p, uint16_t value)
{
  p[0] = value;
  p[1] = value >> 8;
}

static void media_u32(uint8_t *p, uint32_t value)
{
  media_u16(p, value);
  media_u16(p + 2, value >> 16);
}

static uint8_t media_clip(int value)
{
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int titan_media_open(struct titan_media_s *media, const char *directory,
                     const char *suffix)
{
  static unsigned int sequence;
  unsigned int i;
  int length;

  media->fd = -1;
  media->bytes = 0;
  media->frames = 0;
  for (i = 0; i < 10000; i++)
    {
      length = snprintf(media->path, sizeof(media->path),
                        "%s/t%07u.%s", directory,
                        sequence = sequence % 9999999 + 1, suffix);
      if (length < 0 || length >= sizeof(media->path))
        {
          return -ENAMETOOLONG;
        }

      media->fd = open(media->path, O_RDWR | O_CREAT | O_EXCL, 0666);
      if (media->fd >= 0)
        {
          return 0;
        }

      if (errno != EEXIST)
        {
          return -errno;
        }
    }

  return -EEXIST;
}

int titan_media_write(struct titan_media_s *media, const void *data,
                      size_t bytes)
{
  const uint8_t *p = data;
  ssize_t written;

  while (bytes > 0)
    {
      written = write(media->fd, p, bytes);
      if (written < 0 && errno == EINTR)
        {
          continue;
        }

      if (written <= 0)
        {
          return written < 0 ? -errno : -EIO;
        }

      p += written;
      bytes -= written;
    }

  return 0;
}

int titan_media_header(struct titan_media_s *media, bool video)
{
  uint8_t header[224] =
    {
      0
    };

  memcpy(header, "RIFF", 4);
  if (!video)
    {
      media_u32(header + 4, media->bytes + 36);
      memcpy(header + 8, "WAVEfmt ", 8);
      media_u32(header + 16, 16);
      media_u16(header + 20, 1);
      media_u16(header + 22, 1);
      media_u32(header + 24, 16000);
      media_u32(header + 28, 32000);
      media_u16(header + 32, 2);
      media_u16(header + 34, 16);
      memcpy(header + 36, "data", 4);
      media_u32(header + 40, media->bytes);
      return titan_media_write(media, header, 44);
    }

  /* One uncompressed YUY2 stream. No optional AVI index is advertised. */

  media_u32(header + 4, media->bytes + 216);
  memcpy(header + 8, "AVI LIST", 8);
  media_u32(header + 16, 192);
  memcpy(header + 20, "hdrlavih", 8);
  media_u32(header + 28, 56);
  media_u32(header + 32, 1000000 / CONFIG_TESTING_TITAN_DEMO_FPS);
  media_u32(header + 36, 153608 * CONFIG_TESTING_TITAN_DEMO_FPS);
  media_u32(header + 48, media->frames);
  media_u32(header + 56, 1);
  media_u32(header + 60, 153600);
  media_u32(header + 64, 320);
  media_u32(header + 68, 240);
  memcpy(header + 88, "LIST", 4);
  media_u32(header + 92, 116);
  memcpy(header + 96, "strlstrh", 8);
  media_u32(header + 104, 56);
  memcpy(header + 108, "vidsYUY2", 8);
  media_u32(header + 128, 1);
  media_u32(header + 132, CONFIG_TESTING_TITAN_DEMO_FPS);
  media_u32(header + 140, media->frames);
  media_u32(header + 144, 153600);
  media_u32(header + 148, UINT32_MAX);
  media_u16(header + 160, 320);
  media_u16(header + 162, 240);
  memcpy(header + 164, "strf", 4);
  media_u32(header + 168, 40);
  media_u32(header + 172, 40);
  media_u32(header + 176, 320);
  media_u32(header + 180, 240);
  media_u16(header + 184, 1);
  media_u16(header + 186, 16);
  memcpy(header + 188, "YUY2", 4);
  media_u32(header + 192, 153600);
  memcpy(header + 212, "LIST", 4);
  media_u32(header + 216, media->bytes + 4);
  memcpy(header + 220, "movi", 4);
  return titan_media_write(media, header, sizeof(header));
}

int titan_media_finish(struct titan_media_s *media, bool video)
{
  int ret;

  ret = lseek(media->fd, 0, SEEK_SET) < 0 ? -errno :
        titan_media_header(media, video);
  if (ret == 0 && fsync(media->fd) < 0)
    {
      ret = -errno;
    }

  if (close(media->fd) < 0 && ret == 0)
    {
      ret = -errno;
    }

  media->fd = -1;
  if (media->bytes == 0)
    {
      unlink(media->path);
    }

  printf("DEMO_SAVED path=%s bytes=%lu frames=%lu ret=%d\n",
         media->path, (unsigned long)media->bytes,
         (unsigned long)media->frames, ret);
  return ret;
}

int titan_media_photo(struct titan_media_s *media, const uint8_t *yuyv)
{
  uint8_t header[54] =
    {
      0
    };

  uint8_t row[960];
  const uint8_t *input;
  int y;
  int x;
  int n;
  int u;
  int v;
  int luma;
  int ret;

  memcpy(header, "BM", 2);
  media_u32(header + 2, 230454);
  media_u32(header + 10, 54);
  media_u32(header + 14, 40);
  media_u32(header + 18, 320);
  media_u32(header + 22, 240);
  media_u16(header + 26, 1);
  media_u16(header + 28, 24);
  media_u32(header + 34, 230400);
  ret = titan_media_write(media, header, sizeof(header));
  for (y = 239; y >= 0 && ret == 0; y--)
    {
      input = yuyv + y * 640;
      for (x = 0; x < 320; x += 2)
        {
          u = input[x * 2 + 1] - 128;
          v = input[x * 2 + 3] - 128;
          for (n = 0; n < 2; n++)
            {
              luma = 298 * (input[(x + n) * 2] - 16);
              row[(x + n) * 3] = media_clip((luma + 516 * u + 128) >> 8);
              row[(x + n) * 3 + 1] =
                media_clip((luma - 100 * u - 208 * v + 128) >> 8);
              row[(x + n) * 3 + 2] =
                media_clip((luma + 409 * v + 128) >> 8);
            }
        }

      ret = titan_media_write(media, row, sizeof(row));
    }

  if (ret == 0 && fsync(media->fd) < 0)
    {
      ret = -errno;
    }

  if (close(media->fd) < 0 && ret == 0)
    {
      ret = -errno;
    }

  media->fd = -1;
  if (ret < 0)
    {
      unlink(media->path);
    }

  printf("DEMO_SAVED path=%s bytes=230454 frames=1 ret=%d\n",
         media->path, ret);
  return ret;
}
