/****************************************************************************
 * apps/testing/titan_asr/titan_storage.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/partition.h>
#include <nuttx/leds/userled.h>
#include <arch/board/board.h>
#include <netutils/httpd.h>
#include <netutils/netlib.h>

#include "titan_demo.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_server;
static pthread_mutex_t g_storage_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void storage_led(userled_set_t mask)
{
  int fd = open("/dev/userleds", O_WRONLY);

  if (fd >= 0)
    {
      ioctl(fd, ULEDIOC_SETALL, (unsigned long)mask);
      close(fd);
    }
}

static void storage_partition(struct partition_s *part, void *argument)
{
  char path[64];
  int *result = argument;
  int ret;

  if (*result < 0)
    {
      return;
    }

  ret = snprintf(path, sizeof(path), "%sp%u",
                 CONFIG_TESTING_TITAN_DEMO_DISK,
                 (unsigned int)part->index + 1);
  if (ret >= sizeof(path))
    {
      *result = -ENAMETOOLONG;
      return;
    }

  ret = register_blockpartition(path, 0660,
                                CONFIG_TESTING_TITAN_DEMO_DISK,
                                part->firstblock, part->nblocks);
  if (ret < 0 && ret != -EEXIST)
    {
      *result = ret;
      return;
    }

  printf("DEMO_PARTITION path=%s start=%lu sectors=%lu name=%s\n",
         path, (unsigned long)part->firstblock,
         (unsigned long)part->nblocks, part->name);
}

static int storage_server(int argc, char **argv)
{
  struct in_addr address;
  char text[INET_ADDRSTRLEN];
  unsigned int i;
#ifdef CONFIG_NETINIT_DHCPC
  int ret;
#endif

  for (i = 0; ; i++)
    {
      if (netlib_get_ipv4addr("eth0", &address) == 0 &&
          address.s_addr != INADDR_ANY)
        {
          printf("DEMO_FILES url=http://%s/ root=%s\n",
                 inet_ntop(AF_INET, &address, text, sizeof(text)),
                 CONFIG_NETUTILS_HTTPD_PATH);
          break;
        }

#ifdef CONFIG_NETINIT_DHCPC
      /* Let NSH's initial DHCP attempt finish before retrying. */

      if (i >= 30)
        {
          ret = netlib_obtain_ipv4addr("eth0");
          printf("DEMO_DHCP ret=%d\n", ret);
          sleep(3);
          continue;
        }

#endif
      sleep(1);
    }

  httpd_init();
  httpd_listen();
  pthread_mutex_lock(&g_storage_lock);
  g_server = false;
  pthread_mutex_unlock(&g_storage_lock);
  return 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int titan_storage_start(void)
{
  struct stat st;
  int ret;
  int result = 0;

  pthread_mutex_lock(&g_storage_lock);
  storage_led(BOARD_LED_RED_BIT | BOARD_LED_GREEN_BIT);
  if (stat(CONFIG_TESTING_TITAN_DEMO_DEVICE, &st) < 0)
    {
      ret = parse_block_partition(CONFIG_TESTING_TITAN_DEMO_DISK,
                                  storage_partition, &result);
      if (ret < 0 || result < 0)
        {
          errno = ret < 0 ? -ret : -result;
          goto failed;
        }
    }

  if (mkdir(CONFIG_TESTING_TITAN_DEMO_MOUNTPOINT, 0777) < 0 &&
      errno != EEXIST)
    {
      goto failed;
    }

  if (mount(CONFIG_TESTING_TITAN_DEMO_DEVICE,
            CONFIG_TESTING_TITAN_DEMO_MOUNTPOINT, "vfat", 0, NULL) < 0 &&
      errno != EBUSY)
    {
      goto failed;
    }

  if (mkdir(CONFIG_TESTING_TITAN_DEMO_DIRECTORY, 0777) < 0 &&
      errno != EEXIST)
    {
      goto failed;
    }

  if (stat(CONFIG_TESTING_TITAN_DEMO_DIRECTORY, &st) < 0)
    {
      goto failed;
    }

  if (!S_ISDIR(st.st_mode))
    {
      errno = ENOTDIR;
      goto failed;
    }

  if (!g_server)
    {
      ret = task_create("titan_files", CONFIG_TESTING_TITAN_DEMO_PRIORITY,
                         4096, storage_server, NULL);
      if (ret < 0)
        {
          goto failed;
        }

      g_server = true;
    }

  printf("DEMO_STORAGE device=%s mount=%s media=%s\n",
         CONFIG_TESTING_TITAN_DEMO_DEVICE,
         CONFIG_TESTING_TITAN_DEMO_MOUNTPOINT,
         CONFIG_TESTING_TITAN_DEMO_DIRECTORY);
  storage_led(0);
  pthread_mutex_unlock(&g_storage_lock);
  return 0;

failed:
  ret = -errno;
  printf("DEMO_STORAGE_ERROR device=%s ret=%d\n",
         CONFIG_TESTING_TITAN_DEMO_DEVICE, ret);
  storage_led(BOARD_LED_RED_BIT);
  pthread_mutex_unlock(&g_storage_lock);
  return ret;
}
