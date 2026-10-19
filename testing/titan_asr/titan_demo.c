/****************************************************************************
 * apps/testing/titan_asr/titan_demo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <nuttx/leds/userled.h>
#include <nuttx/video/v4l2_cap.h>
#include <nuttx/video/video.h>
#include <arch/board/board.h>

#include "titan_demo.h"
#include "titan_media.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DEMO_COMMANDS 8
#define DEMO_BUFFERS  3
#define DEMO_FRAME    (320 * 240 * 2)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct demo_camera_s
{
  int fd;
  bool registered;
  bool streaming;
  uint8_t *buffers[DEMO_BUFFERS];
  uint32_t lengths[DEMO_BUFFERS];
  uint64_t next;
  uint64_t deadline;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_running;
static bool g_stop;
static bool g_speech;
static bool g_photo;
static bool g_active[TITAN_INTENT_MODES];
static bool g_audio_overflow;
static uint64_t g_started[TITAN_INTENT_MODES];
static uint64_t g_order;
static uint64_t g_wake_until;
static uint64_t g_error_until;
static uint64_t g_audio_deadline;
static pthread_t g_media_thread;
static pthread_t g_led_thread;
static struct titan_intent_event_s g_commands[DEMO_COMMANDS];
static unsigned int g_command_read;
static unsigned int g_command_count;
static int16_t (*g_pcm)[160];
static unsigned int g_pcm_read;
static unsigned int g_pcm_count;
static char g_directory[96];
static int g_led;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t demo_now(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void demo_error(int ret)
{
  pthread_mutex_lock(&g_lock);
  g_error_until = demo_now() + 2000;
  pthread_mutex_unlock(&g_lock);
  printf("DEMO_ERROR ret=%d\n", ret);
}

static void demo_active(unsigned int mode, bool active)
{
  pthread_mutex_lock(&g_lock);
  if (active && !g_active[mode])
    {
      g_started[mode] = ++g_order;
    }

  g_active[mode] = active;
  pthread_mutex_unlock(&g_lock);
}

static void demo_camera_close(struct demo_camera_s *camera)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;

  if (camera->streaming)
    {
      ioctl(camera->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
    }

  if (camera->fd >= 0)
    {
      close(camera->fd);
    }

  for (i = 0; i < DEMO_BUFFERS; i++)
    {
      if (camera->buffers[i] != MAP_FAILED)
        {
          munmap(camera->buffers[i], camera->lengths[i]);
        }
    }

  if (camera->registered)
    {
      capture_uninitialize("/dev/video");
    }

  camera->fd = -1;
  camera->registered = false;
  camera->streaming = false;
}

static int demo_camera_open(struct demo_camera_s *camera)
{
  struct v4l2_format format;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;
  int ret;
  const char *stage = "open";

  memset(camera, 0, sizeof(*camera));
  camera->fd = -1;
  for (i = 0; i < DEMO_BUFFERS; i++)
    {
      camera->buffers[i] = MAP_FAILED;
    }

  ret = capture_initialize("/dev/video");
  if (ret < 0)
    {
      printf("DEMO_CAMERA stage=register ret=%d\n", ret);
      return ret;
    }

  camera->registered = true;
  camera->fd = open("/dev/video", O_RDWR | O_NONBLOCK);
  if (camera->fd < 0)
    {
      goto failed;
    }

  memset(&format, 0, sizeof(format));
  stage = "format";
  format.type = type;
  format.fmt.pix.width = 320;
  format.fmt.pix.height = 240;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  if (ioctl(camera->fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      goto failed;
    }

  memset(&request, 0, sizeof(request));
  stage = "buffers";
  request.type = type;
  request.memory = V4L2_MEMORY_MMAP;
  request.count = DEMO_BUFFERS;
  request.mode = V4L2_BUF_MODE_RING;
  if (ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      goto failed;
    }

  for (i = 0; i < DEMO_BUFFERS; i++)
    {
      stage = "query";
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (ioctl(camera->fd, VIDIOC_QUERYBUF, (uintptr_t)&buffer) < 0)
        {
          goto failed;
        }

      camera->lengths[i] = buffer.length;
      stage = "mmap";
      camera->buffers[i] = mmap(NULL, buffer.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                camera->fd, buffer.m.offset);
      if (camera->buffers[i] == MAP_FAILED)
        {
          goto failed;
        }

      stage = "queue";
      if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          goto failed;
        }
    }

  stage = "stream";
  if (ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      goto failed;
    }

  camera->streaming = true;
  camera->next = demo_now();
  camera->deadline = camera->next + 2000;
  return 0;

failed:
  ret = -errno;
  printf("DEMO_CAMERA stage=%s ret=%d\n", stage, ret);
  demo_camera_close(camera);
  return ret;
}

static int demo_camera_frame(struct demo_camera_s *camera,
                             struct titan_media_s *video, bool photo)
{
  struct pollfd pfd =
    {
      .fd = camera->fd, .events = POLLIN
    };

  struct v4l2_buffer buffer;
  struct titan_media_s image;
  const uint8_t chunk[8] =
    {
      '0', '0', 'd', 'b', 0, 0x58, 2, 0
    };

  uint64_t now;
  int ret;

  ret = poll(&pfd, 1, 10);
  if (ret < 0)
    {
      return errno == EINTR ? 0 : -errno;
    }

  if (ret == 0)
    {
      return demo_now() > camera->deadline ? -ETIMEDOUT : 0;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
    {
      printf("DEMO_CAMERA stage=dequeue errno=%d\n", errno);
      return errno == EAGAIN ? 0 : -errno;
    }

  if (buffer.index >= DEMO_BUFFERS ||
      buffer.bytesused != DEMO_FRAME ||
      camera->lengths[buffer.index] < DEMO_FRAME)
    {
      return -EIO;
    }

  now = demo_now();
  camera->deadline = now + 2000;
  ret = 0;
  if (photo)
    {
      ret = titan_media_open(&image, g_directory, "bmp");
      if (ret == 0)
        {
          ret = titan_media_photo(&image, camera->buffers[buffer.index]);
        }

      if (ret < 0)
        {
          printf("DEMO_CAMERA stage=photo ret=%d\n", ret);
        }
    }

  if (ret == 0 && video->fd >= 0 && now >= camera->next)
    {
      off_t start = lseek(video->fd, 0, SEEK_CUR);

      ret = start < 0 ? -errno :
            titan_media_write(video, chunk, sizeof(chunk));
      if (ret == 0)
        {
          ret = titan_media_write(video, camera->buffers[buffer.index],
                                  DEMO_FRAME);
        }

      if (ret == 0)
        {
          video->bytes += DEMO_FRAME + sizeof(chunk);
          video->frames++;
          camera->next = now + 1000 / CONFIG_TESTING_TITAN_DEMO_FPS;
        }
      else if (start >= 0)
        {
          ftruncate(video->fd, start);
        }
    }

  if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0 && ret == 0)
    {
      printf("DEMO_CAMERA stage=requeue errno=%d length=%lu\n",
             errno, (unsigned long)buffer.length);
      ret = -errno;
    }

  return ret < 0 ? ret : 1;
}

static void demo_finish(struct titan_media_s *media, bool video)
{
  int ret = titan_media_finish(media, video);

  demo_active(video ? TITAN_MODE_VIDEO : TITAN_MODE_AUDIO, false);
  if (ret < 0)
    {
      demo_error(ret);
    }
}

static int demo_audio_drain(struct titan_media_s *audio,
                            unsigned int maximum)
{
  int16_t pcm[160];
  unsigned int i;
  bool block;
  int ret;

  for (i = 0; i < maximum; i++)
    {
      pthread_mutex_lock(&g_lock);
      block = g_pcm_count > 0;
      if (block)
        {
          memcpy(pcm, g_pcm[g_pcm_read++ %
                 CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS], sizeof(pcm));
          g_pcm_count--;
        }

      pthread_mutex_unlock(&g_lock);
      if (!block)
        {
          break;
        }

      ret = titan_media_write(audio, pcm, sizeof(pcm));
      if (ret < 0)
        {
          ftruncate(audio->fd, 44 + audio->bytes);
          return ret;
        }

      audio->bytes += sizeof(pcm);
      g_audio_deadline = demo_now() + 2000;
    }

  return 0;
}

static void *demo_media_worker(void *argument)
{
  struct demo_camera_s camera;
  struct titan_media_s video =
    {
      .fd = -1
    };

  struct titan_media_s audio =
    {
      .fd = -1
    };

  struct titan_intent_event_s event;
  bool command;
  bool photo = false;
  bool stop;
  bool overflow;
  unsigned int intent;
  int ret;

  camera.fd = -1;
  for (; ; )
    {
      pthread_mutex_lock(&g_lock);
      stop = g_stop;
      command = g_command_count > 0;
      if (command)
        {
          event = g_commands[g_command_read++ % DEMO_COMMANDS];
          g_command_count--;
        }

      overflow = g_audio_overflow;
      g_audio_overflow = false;
      pthread_mutex_unlock(&g_lock);
      if (stop)
        {
          break;
        }

      if (overflow && audio.fd >= 0)
        {
          demo_finish(&audio, false);
          demo_error(-ENOSPC);
        }

      if (command)
        {
          intent = event.intent;
          ret = 0;
          if (intent == TITAN_STOP_RECENT)
            {
              if (event.resolved == TITAN_MODE_NONE)
                {
                  printf("DEMO_ACTION intent=stop_recent ret=0\n");
                  continue;
                }

              intent = event.resolved == TITAN_MODE_VIDEO ?
                       TITAN_VIDEO_STOP :
                       event.resolved == TITAN_MODE_AUDIO ?
                       TITAN_AUDIO_STOP : TITAN_COMPANION_EXIT;
            }

          switch (intent)
            {
              case TITAN_PHOTO_TAKE:
                photo = true;
                pthread_mutex_lock(&g_lock);
                g_photo = true;
                pthread_mutex_unlock(&g_lock);
                break;

              case TITAN_VIDEO_START:
                if (video.fd < 0)
                  {
                    ret = camera.fd >= 0 ? 0 : demo_camera_open(&camera);
                    if (ret == 0)
                      {
                        ret = titan_media_open(&video, g_directory, "avi");
                      }

                    if (ret == 0)
                      {
                        ret = titan_media_header(&video, true);
                      }

                    if (ret == 0)
                      {
                        demo_active(TITAN_MODE_VIDEO, true);
                      }
                    else if (video.fd >= 0)
                      {
                        demo_finish(&video, true);
                      }
                  }
                break;

              case TITAN_VIDEO_STOP:
                if (video.fd >= 0)
                  {
                    demo_finish(&video, true);
                  }
                break;

              case TITAN_AUDIO_START:
                if (audio.fd < 0)
                  {
                    ret = titan_media_open(&audio, g_directory, "wav");
                    if (ret == 0)
                      {
                        ret = titan_media_header(&audio, false);
                      }

                    if (ret == 0)
                      {
                        pthread_mutex_lock(&g_lock);
                        g_pcm_read = g_pcm_count = 0;
                        g_audio_overflow = false;
                        pthread_mutex_unlock(&g_lock);
                        g_audio_deadline = demo_now() + 2000;
                        demo_active(TITAN_MODE_AUDIO, true);
                      }
                    else if (audio.fd >= 0)
                      {
                        demo_finish(&audio, false);
                      }
                  }
                break;

              case TITAN_AUDIO_STOP:
                if (audio.fd >= 0)
                  {
                    demo_active(TITAN_MODE_AUDIO, false);

                    /* Drain every accepted block before closing WAV. */

                    ret = demo_audio_drain(&audio,
                      CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS);
                    demo_finish(&audio, false);
                  }
                break;

              case TITAN_COMPANION_ENTER:
              case TITAN_COMPANION_EXIT:

                /* Demo requires a fresh wake for each command. */

                ret = -ENOTSUP;
                break;

              default:
                ret = -EINVAL;
                break;
            }

          printf("DEMO_ACTION intent=%s ret=%d\n",
                 titan_intent_name(intent), ret);
          if (ret < 0)
            {
              demo_error(ret);
            }
        }

      if (photo && camera.fd < 0)
        {
          ret = demo_camera_open(&camera);
          if (ret < 0)
            {
              photo = false;
              demo_error(ret);
            }
        }

      if (camera.fd >= 0 && (photo || video.fd >= 0))
        {
          ret = demo_camera_frame(&camera, &video, photo);
          if (ret != 0)
            {
              photo = false;
            }

          if (ret < 0)
            {
              demo_error(ret);
              if (video.fd >= 0)
                {
                  demo_finish(&video, true);
                }
            }
        }

      pthread_mutex_lock(&g_lock);
      g_photo = photo;
      pthread_mutex_unlock(&g_lock);
      if (video.fd >= 0 &&
          video.bytes >= CONFIG_TESTING_TITAN_DEMO_LIMIT_KB * 1024u)
        {
          demo_finish(&video, true);
        }

      if (camera.fd >= 0 && !photo && video.fd < 0)
        {
          demo_camera_close(&camera);
        }

      if (audio.fd >= 0)
        {
          ret = demo_audio_drain(&audio, 32);
          if (ret < 0)
            {
              demo_finish(&audio, false);
              demo_error(ret);
            }
        }

      if (audio.fd >= 0 &&
          (audio.bytes >= CONFIG_TESTING_TITAN_DEMO_LIMIT_KB * 1024u ||
           demo_now() > g_audio_deadline))
        {
          bool timedout = demo_now() > g_audio_deadline;

          demo_finish(&audio, false);
          if (timedout)
            {
              demo_error(-ETIMEDOUT);
            }
        }

      usleep(10000);
    }

  if (video.fd >= 0)
    {
      demo_finish(&video, true);
    }

  if (audio.fd >= 0)
    {
      demo_active(TITAN_MODE_AUDIO, false);
      ret = demo_audio_drain(&audio,
                             CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS);
      if (ret < 0)
        {
          demo_error(ret);
        }

      demo_finish(&audio, false);
    }

  if (camera.fd >= 0)
    {
      demo_camera_close(&camera);
    }

  return NULL;
}

static void *demo_led_worker(void *argument)
{
  userled_set_t leds;
  userled_set_t previous = UINT32_MAX;
  uint64_t now;
  bool stop;
  const char *name;
  const char *last_name = "";

  for (; ; )
    {
      now = demo_now();
      pthread_mutex_lock(&g_lock);
      stop = g_stop;
      name = "idle";
      leds = now % 2000 < 200 ? BOARD_LED_BLUE_BIT : 0;
      if (g_active[TITAN_MODE_AUDIO])
        {
          name = "audio";
          leds = now % 1000 < 500 ? BOARD_LED_RED_BIT : 0;
        }

      if (g_active[TITAN_MODE_VIDEO])
        {
          name = g_active[TITAN_MODE_AUDIO] ? "video_audio" : "video";
          leds = g_active[TITAN_MODE_AUDIO] && now % 1000 >= 500 ?
                 BOARD_LED_BLUE_BIT : BOARD_LED_RED_BIT;
        }

      if (g_speech)
        {
          name = "vad";
          leds = BOARD_LED_GREEN_BIT;
        }

      if (now < g_wake_until)
        {
          name = "wake";
          leds = BOARD_LED_GREEN_BIT | BOARD_LED_BLUE_BIT;
        }

      if (g_photo)
        {
          name = "photo";
          leds = BOARD_LED_GREEN_BIT | BOARD_LED_RED_BIT |
                 BOARD_LED_BLUE_BIT;
        }

      if (now < g_error_until)
        {
          name = "error";
          leds = now % 200 < 100 ? BOARD_LED_RED_BIT : 0;
        }

      pthread_mutex_unlock(&g_lock);
      if (stop)
        {
          break;
        }

      if (leds != previous)
        {
          if (ioctl(g_led, ULEDIOC_SETALL, (unsigned long)leds) < 0)
            {
              printf("DEMO_LED_ERROR errno=%d\n", errno);
              break;
            }

          previous = leds;
        }

      if (strcmp(name, last_name) != 0)
        {
          printf("DEMO_LED state=%s mask=%lu\n", name,
                 (unsigned long)leds);
          last_name = name;
        }

      usleep(50000);
    }

  ioctl(g_led, ULEDIOC_SETALL, 0);
  return NULL;
}

static int demo_thread(pthread_t *thread, void *(*entry)(void *))
{
  pthread_attr_t attr;
  struct sched_param param =
    {
      .sched_priority = CONFIG_TESTING_TITAN_DEMO_PRIORITY
    };

  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(thread, &attr, entry, NULL);
  pthread_attr_destroy(&attr);
  return -ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int titan_demo_open(const char *directory)
{
  struct stat st;
  userled_set_t supported;
  int ret;

  if (strlen(directory) >= sizeof(g_directory))
    {
      return -ENAMETOOLONG;
    }

  if (stat(directory, &st) < 0)
    {
      return -errno;
    }

  if (!S_ISDIR(st.st_mode))
    {
      return -ENOTDIR;
    }

  pthread_mutex_lock(&g_lock);
  if (g_running)
    {
      pthread_mutex_unlock(&g_lock);
      return -EBUSY;
    }

  g_running = true;
  g_stop = false;
  g_speech = g_photo = false;
  g_audio_overflow = false;
  g_command_read = g_command_count = 0;
  g_pcm_read = g_pcm_count = 0;
  g_order = g_wake_until = g_error_until = 0;
  memset(g_active, 0, sizeof(g_active));
  memset(g_started, 0, sizeof(g_started));
  strcpy(g_directory, directory);
  pthread_mutex_unlock(&g_lock);
  g_pcm = calloc(CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS, sizeof(*g_pcm));
  g_led = open("/dev/userleds", O_RDWR);
  ret = g_pcm == NULL ? -ENOMEM : g_led < 0 ? -errno : 0;
  if (ret == 0 &&
      ioctl(g_led, ULEDIOC_SUPPORTED, (uintptr_t)&supported) < 0)
    {
      ret = -errno;
    }

  if (ret == 0 && (supported & 7) != 7)
    {
      ret = -ENODEV;
    }

  if (ret == 0)
    {
      ret = demo_thread(&g_media_thread, demo_media_worker);
      if (ret == 0)
        {
          ret = demo_thread(&g_led_thread, demo_led_worker);
          if (ret < 0)
            {
              titan_demo_stop();
              pthread_join(g_media_thread, NULL);
            }
        }
    }

  if (ret < 0)
    {
      if (g_led >= 0)
        {
          close(g_led);
        }

      free(g_pcm);
      g_pcm = NULL;
      pthread_mutex_lock(&g_lock);
      g_running = false;
      pthread_mutex_unlock(&g_lock);
    }

  return ret;
}

void titan_demo_close(void)
{
  titan_demo_stop();
  pthread_join(g_media_thread, NULL);
  pthread_join(g_led_thread, NULL);
  close(g_led);
  free(g_pcm);
  pthread_mutex_lock(&g_lock);
  g_pcm = NULL;
  g_running = false;
  pthread_mutex_unlock(&g_lock);
}

bool titan_demo_stopping(void)
{
  bool stop;

  pthread_mutex_lock(&g_lock);
  stop = g_stop;
  pthread_mutex_unlock(&g_lock);
  return stop;
}

void titan_demo_stop(void)
{
  pthread_mutex_lock(&g_lock);
  g_stop = true;
  pthread_mutex_unlock(&g_lock);
}

void titan_demo_status(void)
{
  pthread_mutex_lock(&g_lock);
  printf("DEMO_STATUS running=%u stopping=%u video=%u audio=%u photo=%u "
         "pcm_queue=%u directory=%s\n",
         g_running, g_stop, g_active[TITAN_MODE_VIDEO],
         g_active[TITAN_MODE_AUDIO], g_photo, g_pcm_count, g_directory);
  pthread_mutex_unlock(&g_lock);
}

void titan_demo_event(const struct titan_intent_event_s *event)
{
  pthread_mutex_lock(&g_lock);
  if (event->kind == TITAN_EVENT_WAKE_WINDOW)
    {
      g_wake_until = demo_now() + 8000;
    }
  else if (event->kind == TITAN_EVENT_INTENT &&
           event->result != TITAN_RESULT_COOLDOWN)
    {
      g_wake_until = 0;
      if (g_command_count == DEMO_COMMANDS)
        {
          g_error_until = demo_now() + 2000;
          printf("DEMO_ERROR ret=%d command_queue=full\n", -ENOSPC);
        }
      else
        {
          g_commands[(g_command_read + g_command_count++) %
                     DEMO_COMMANDS] = *event;
        }
    }

  pthread_mutex_unlock(&g_lock);
}

void titan_demo_vad(bool speech, bool authorized)
{
  pthread_mutex_lock(&g_lock);
  g_speech = speech;
  g_wake_until = authorized ? demo_now() + 100 : 0;
  pthread_mutex_unlock(&g_lock);
}

void titan_demo_pcm(const int16_t *pcm)
{
  pthread_mutex_lock(&g_lock);
  if (g_active[TITAN_MODE_AUDIO] && !g_audio_overflow)
    {
      if (g_pcm_count == CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS)
        {
          g_audio_overflow = true;
          g_active[TITAN_MODE_AUDIO] = false;
        }
      else
        {
          memcpy(g_pcm[(g_pcm_read + g_pcm_count++) %
                 CONFIG_TESTING_TITAN_DEMO_AUDIO_BLOCKS], pcm, 320);
        }
    }

  pthread_mutex_unlock(&g_lock);
}

void titan_demo_sync(struct titan_intent_state_s *state)
{
  pthread_mutex_lock(&g_lock);
  memcpy(state->active, g_active, sizeof(g_active));
  memcpy(state->started, g_started, sizeof(g_started));
  state->order = g_order;
  pthread_mutex_unlock(&g_lock);
}
