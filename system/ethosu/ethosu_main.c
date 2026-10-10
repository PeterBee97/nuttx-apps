/****************************************************************************
 * apps/system/ethosu/ethosu_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/aie/ethosu.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ethosu_info(const char *devpath)
{
  struct ethosu_info_s info;
  int fd;
  int ret;

  fd = open(devpath, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ethosu: open %s failed: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, ETHOSUIOC_GET_INFO, (unsigned long)(uintptr_t)&info);
  close(fd);
  if (ret < 0)
    {
      fprintf(stderr, "ethosu: GET_INFO failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  printf("device: %s\n", devpath);
  printf("driver: %u.%u.%u\n", info.driver_major, info.driver_minor,
         info.driver_patch);
  printf("architecture: %lu.%lu.%lu\n",
         (unsigned long)info.arch_major_rev,
         (unsigned long)info.arch_minor_rev,
         (unsigned long)info.arch_patch_rev);
  printf("product revision: %lu, version %lu.%lu, status %lu\n",
         (unsigned long)info.product_major,
         (unsigned long)info.version_major,
         (unsigned long)info.version_minor,
         (unsigned long)info.version_status);
  printf("macs/cycle: %lu\n", 1ul << info.macs_per_cc);
  printf("command stream: %lu\n",
         (unsigned long)info.cmd_stream_version);
  printf("custom DMA: %s\n", info.custom_dma != 0 ? "yes" : "no");
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
  const char *devpath = "/dev/ethosu0";

  if (argc > 1 && strcmp(argv[1], "info") != 0)
    {
      fprintf(stderr, "usage: ethosu [info] [device]\n");
      return EXIT_FAILURE;
    }

  if (argc > 2)
    {
      devpath = argv[2];
    }

  return ethosu_info(devpath);
}
