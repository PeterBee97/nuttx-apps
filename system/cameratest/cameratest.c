/****************************************************************************
 * apps/system/cameratest/cameratest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <nuttx/video/v4l2_cap.h>
#include <nuttx/video/video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMERATEST_DEVICE       "/dev/video"
#define CAMERATEST_WIDTH        320
#define CAMERATEST_HEIGHT       240
#define CAMERATEST_PORT         5001
#define CAMERATEST_BUFFER_COUNT 3

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void cameratest_usage(FAR const char *program)
{
  fprintf(stderr,
          "Usage: %s [-W 320|640] [-H 240|480] [-a IPv4] [-p port]\n",
          program);
}

static uint32_t cameratest_checksum(FAR const uint8_t *buffer, size_t size)
{
  uint32_t checksum = 2166136261u;
  size_t i;

  for (i = 0; i < size; i++)
    {
      checksum ^= buffer[i];
      checksum *= 16777619u;
    }

  return checksum;
}

static int cameratest_send(FAR const char *address, uint16_t port,
                           FAR const uint8_t *buffer, size_t size)
{
  struct sockaddr_in peer;
  size_t sent = 0;
  int sockfd;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    {
      return -errno;
    }

  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  if (inet_pton(AF_INET, address, &peer.sin_addr) != 1)
    {
      close(sockfd);
      return -EINVAL;
    }

  if (connect(sockfd, (FAR struct sockaddr *)&peer, sizeof(peer)) < 0)
    {
      int error = errno;

      close(sockfd);
      return -error;
    }

  while (sent < size)
    {
      ssize_t ret = send(sockfd, buffer + sent, size - sent, 0);

      if (ret <= 0)
        {
          int error = ret < 0 ? errno : EPIPE;

          close(sockfd);
          return -error;
        }

      sent += ret;
    }

  close(sockfd);
  return OK;
}

static void cameratest_report(FAR const uint8_t *buffer, size_t size)
{
  uint64_t sums[2] =
    {
      0, 0
    };

  uint8_t minimum[2] =
    {
      UINT8_MAX, UINT8_MAX
    };

  uint8_t maximum[2] =
    {
      0, 0
    };

  size_t i;

  for (i = 0; i < size; i++)
    {
      unsigned int lane = i & 1;

      sums[lane] += buffer[i];
      if (buffer[i] < minimum[lane])
        {
          minimum[lane] = buffer[i];
        }

      if (buffer[i] > maximum[lane])
        {
          maximum[lane] = buffer[i];
        }
    }

  printf("bytes=%zu checksum=%08" PRIx32 "\n",
         size, cameratest_checksum(buffer, size));
  printf("byte-even: min=%u max=%u mean=%" PRIu64 "\n",
         minimum[0], maximum[0], sums[0] / (size / 2));
  printf("byte-odd : min=%u max=%u mean=%" PRIu64 "\n",
         minimum[1], maximum[1], sums[1] / (size / 2));
  printf("first16:");
  for (i = 0; i < 16 && i < size; i++)
    {
      printf(" %02x", buffer[i]);
    }

  printf("\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct v4l2_requestbuffers request;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_buffer vbuf;
  struct v4l2_format format;
  FAR const char *address = NULL;
  FAR uint8_t *buffers[CAMERATEST_BUFFER_COUNT] =
    {
      MAP_FAILED, MAP_FAILED, MAP_FAILED
    };

  FAR uint8_t *last_buffer = NULL;
  uint32_t lengths[CAMERATEST_BUFFER_COUNT] =
    {
      0, 0, 0
    };

  uint32_t last_size = 0;
  uint16_t width = CAMERATEST_WIDTH;
  uint16_t height = CAMERATEST_HEIGHT;
  uint16_t port = CAMERATEST_PORT;
  size_t frame_size;
  bool registered = false;
  bool streaming = false;
  int option;
  int fd = -1;
  int ret;
  int i;

  optind = 1;
  while ((option = getopt(argc, argv, "W:H:a:p:h")) != ERROR)
    {
      switch (option)
        {
          case 'W':
            width = strtoul(optarg, NULL, 10);
            break;

          case 'H':
            height = strtoul(optarg, NULL, 10);
            break;

          case 'a':
            address = optarg;
            break;

          case 'p':
            port = strtoul(optarg, NULL, 10);
            break;

          case 'h':
          default:
            cameratest_usage(argv[0]);
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (!((width == 320 && height == 240) ||
        (width == 640 && height == 480)) || port == 0)
    {
      cameratest_usage(argv[0]);
      return EXIT_FAILURE;
    }

  ret = capture_initialize(CAMERATEST_DEVICE);
  if (ret < 0)
    {
      fprintf(stderr, "cameratest: register failed: %d\n", ret);
      goto out;
    }

  registered = true;
  printf("cameratest: opening %s\n", CAMERATEST_DEVICE);
  fd = open(CAMERATEST_DEVICE, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      fprintf(stderr, "cameratest: open failed: %d\n", errno);
      goto out;
    }

  printf("cameratest: configuring %ux%u YUYV\n", width, height);
  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      ret = -errno;
      fprintf(stderr, "cameratest: S_FMT failed: %d\n", errno);
      goto out;
    }

  printf("cameratest: allocating capture buffer\n");
  frame_size = width * height * 2;

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_MMAP;
  request.count = CAMERATEST_BUFFER_COUNT;
  request.mode = V4L2_BUF_MODE_FIFO;
  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      ret = -errno;
      fprintf(stderr, "cameratest: REQBUFS failed: %d\n", errno);
      goto out;
    }

  for (i = 0; i < CAMERATEST_BUFFER_COUNT; i++)
    {
      memset(&vbuf, 0, sizeof(vbuf));
      vbuf.type = type;
      vbuf.memory = V4L2_MEMORY_MMAP;
      vbuf.index = i;
      if (ioctl(fd, VIDIOC_QUERYBUF, (uintptr_t)&vbuf) < 0)
        {
          ret = -errno;
          fprintf(stderr, "cameratest: QUERYBUF failed: %d\n", errno);
          goto out;
        }

      lengths[i] = vbuf.length;
      buffers[i] = mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, vbuf.m.offset);
      if (buffers[i] == MAP_FAILED)
        {
          ret = -errno;
          fprintf(stderr, "cameratest: mmap failed: %d\n", errno);
          goto out;
        }

      printf("cameratest: buffer%d=%p offset=%" PRIu32
             " length=%" PRIu32 "\n",
             i, buffers[i], vbuf.m.offset, vbuf.length);
    }

  printf("cameratest: queueing capture buffer\n");
  for (i = 0; i < CAMERATEST_BUFFER_COUNT; i++)
    {
      memset(&vbuf, 0, sizeof(vbuf));
      vbuf.type = type;
      vbuf.memory = V4L2_MEMORY_MMAP;
      vbuf.index = i;
      vbuf.length = frame_size;
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&vbuf) < 0)
        {
          ret = -errno;
          fprintf(stderr, "cameratest: QBUF failed: %d\n", errno);
          goto out;
        }
    }

  printf("cameratest: starting stream\n");
  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      ret = -errno;
      fprintf(stderr, "cameratest: STREAMON failed: %d\n", errno);
      goto out;
    }

  streaming = true;
  for (i = 0; i < CAMERATEST_BUFFER_COUNT; i++)
    {
      printf("cameratest: waiting for frame %d\n", i + 1);
      memset(&vbuf, 0, sizeof(vbuf));
      vbuf.type = type;
      vbuf.memory = V4L2_MEMORY_MMAP;
      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&vbuf) < 0)
        {
          ret = -errno;
          fprintf(stderr, "cameratest: DQBUF failed: %d\n", errno);
          goto out;
        }

      if (vbuf.index >= CAMERATEST_BUFFER_COUNT)
        {
          ret = -EIO;
          goto out;
        }

      printf("frame %d buffer=%" PRIu32 "\n", i + 1, vbuf.index);
      last_buffer = buffers[vbuf.index];
      last_size = vbuf.bytesused;
      cameratest_report(last_buffer, last_size);
    }

  if (address != NULL)
    {
      ret = cameratest_send(address, port, last_buffer, last_size);
      if (ret < 0)
        {
          fprintf(stderr, "cameratest: send failed: %d\n", ret);
          goto out;
        }

      printf("sent=%" PRIu32 " to %s:%u\n", last_size, address, port);
    }

  ret = OK;

out:
  if (streaming)
    {
      ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  for (i = 0; i < CAMERATEST_BUFFER_COUNT; i++)
    {
      if (buffers[i] != MAP_FAILED)
        {
          munmap(buffers[i], lengths[i]);
        }
    }

  if (registered)
    {
      capture_uninitialize(CAMERATEST_DEVICE);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
