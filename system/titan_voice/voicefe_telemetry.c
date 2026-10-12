/****************************************************************************
 * apps/system/titan_voice/voicefe_telemetry.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "voicefe_intent.h"
#include "voicefe_telemetry.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VT_BATCH 8
#define VT_BODY  4096

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct titan_intent_event_s g_pending[VT_BATCH];
static unsigned int g_pending_count;
static uint64_t g_next;
static unsigned int g_backoff;
static char g_body[VT_BODY];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t telemetry_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int telemetry_wait(int fd, short events, uint64_t deadline)
{
  struct pollfd pfd;
  uint64_t now;
  int ret;

  pfd.fd = fd;
  pfd.events = events;
  for (; ; )
    {
      now = telemetry_now();
      if (now >= deadline)
        {
          return -ETIMEDOUT;
        }

      ret = poll(&pfd, 1, deadline - now);
      if (ret < 0 && errno == EINTR)
        {
          continue;
        }

      if (ret <= 0)
        {
          return ret == 0 ? -ETIMEDOUT : -errno;
        }

      if ((pfd.revents & events) != 0)
        {
          return 0;
        }

      return -EIO;
    }
}

static int telemetry_send(int fd, const char *data, size_t bytes,
                          uint64_t deadline)
{
  ssize_t sent;
  int ret;

  while (bytes != 0)
    {
      ret = telemetry_wait(fd, POLLOUT, deadline);
      if (ret < 0)
        {
          return ret;
        }

      sent = send(fd, data, bytes, 0);
      if (sent < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (sent <= 0)
        {
          return -EIO;
        }

      data += sent;
      bytes -= sent;
    }

  return 0;
}

static int telemetry_post(const char *host, int port, uint64_t seq)
{
  struct sockaddr_in addr;
  char header[256];
  char response[512];
  char *body;
  char *ack;
  uint64_t deadline = telemetry_now() + 2000;
  socklen_t length;
  unsigned int used = 0;
  int error;
  int size;
  int fd;
  int ret;
  ssize_t received;
  bool accepted = false;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  fcntl(fd, F_SETFL, O_NONBLOCK);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host, &addr.sin_addr);
  ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno == EINPROGRESS)
    {
      ret = telemetry_wait(fd, POLLOUT, deadline);
      if (ret == 0)
        {
          length = sizeof(error);
          ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
          if (ret == 0 && error != 0)
            {
              ret = -error;
            }
        }
    }

  if (ret == 0)
    {
      size = snprintf(header, sizeof(header),
                      "POST /v1/device/event HTTP/1.1\r\n"
                      "Host: %s:%d\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      host, port, strlen(g_body));
      ret = telemetry_send(fd, header, size, deadline);
    }

  if (ret == 0)
    {
      ret = telemetry_send(fd, g_body, strlen(g_body), deadline);
    }

  while (ret == 0 && used < sizeof(response) - 1)
    {
      ret = telemetry_wait(fd, POLLIN, deadline);
      if (ret < 0)
        {
          break;
        }

      received = recv(fd, response + used, sizeof(response) - 1 - used, 0);
      if (received < 0 && (errno == EAGAIN || errno == EINTR))
        {
          ret = 0;
          continue;
        }

      if (received <= 0)
        {
          ret = -EPROTO;
          break;
        }

      used += received;
      response[used] = '\0';
      body = strstr(response, "\r\n\r\n");
      if (body != NULL && strchr(body + 4, '}') != NULL)
        {
          ack = strstr(body + 4, "\"accepted_seq\"");
          if (strncmp(response, "HTTP/1.1 200 ", 13) != 0 ||
              ack == NULL || (ack = strchr(ack, ':')) == NULL ||
              strtoull(ack + 1, NULL, 10) < seq)
            {
              ret = -EPROTO;
            }

          accepted = ret == 0;
          break;
        }
    }

  close(fd);
  return ret == 0 && !accepted ? -EMSGSIZE : ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void voicefe_telemetry_init(void)
{
  g_pending_count = 0;
  g_next = 0;
  g_backoff = 1;
}

void voicefe_telemetry_tick(const char *host, int port, const char *boot)
{
  struct voicefe_intent_stats_s stats;
  struct titan_intent_event_s *event;
  uint64_t now = telemetry_now();
  unsigned int i;
  int size;
  int written;

  if (now < g_next)
    {
      return;
    }

  while (g_pending_count < VT_BATCH &&
         voicefe_intent_poll(&g_pending[g_pending_count]))
    {
      g_pending_count++;
    }

  voicefe_intent_stats(&stats);
  size = snprintf(g_body, sizeof(g_body),
    "{\"version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
    "\"seq\":%" PRIu64 ",\"state\":{\"video\":%s,\"audio\":%s,"
    "\"companion\":%s,\"wake_deadline\":%" PRIu64 ","
    "\"model_version\":\"%s\",\"overrun\":%" PRIu32 ","
    "\"event_drop\":%" PRIu32 "},\"events\":[",
    CONFIG_SYSTEM_TITAN_VOICE_INTENT_DEVICE, boot, stats.state.seq,
    stats.state.active[0] ? "true" : "false",
    stats.state.active[1] ? "true" : "false",
    stats.state.active[2] ? "true" : "false", stats.state.wake_deadline,
    tflm_titan_intent_version(), stats.overruns, stats.event_drop);
  if (size < 0 || size >= sizeof(g_body))
    {
      g_next = now + 1000;
      return;
    }

  for (i = 0; i < g_pending_count; i++)
    {
      event = &g_pending[i];
      written = snprintf(g_body + size, sizeof(g_body) - size,
        "%s{\"version\":1,\"seq\":%" PRIu64 ","
        "\"utterance_id\":%" PRIu64 ",\"monotonic_ms\":%" PRIu64 ","
        "\"kind\":\"%s\",\"intent\":\"%s\",\"resolved\":%d,"
        "\"score\":%u,\"margin\":%u,\"result\":\"%s\","
        "\"video\":%s,\"audio\":%s,\"companion\":%s}",
        i == 0 ? "" : ",", event->seq, event->utterance_id,
        event->monotonic_ms, titan_intent_event_name(event->kind),
        titan_intent_name(event->intent), event->resolved, event->score,
        event->margin, titan_intent_result_name(event->result),
        event->active[0] ? "true" : "false",
        event->active[1] ? "true" : "false",
        event->active[2] ? "true" : "false");
      if (written < 0 || written >= sizeof(g_body) - size - 3)
        {
          g_next = now + 1000;
          return;
        }

      size += written;
    }

  strcpy(g_body + size, "]}");
  if (telemetry_post(host, port, stats.state.seq) == 0)
    {
      g_pending_count = 0;
      g_backoff = 1;
      g_next = telemetry_now() + 1000;
    }
  else
    {
      g_next = telemetry_now() + g_backoff * 1000;
      if (g_backoff < 8)
        {
          g_backoff *= 2;
        }
    }
}
