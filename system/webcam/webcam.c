/****************************************************************************
 * apps/system/webcam/webcam.c
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
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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

#define WEBCAM_VIDEO_DEVICE       "/dev/video"
#define WEBCAM_AUDIO_DEVICE       "/dev/pdm0"
#define WEBCAM_VIDEO_WIDTH        320
#define WEBCAM_VIDEO_HEIGHT       240
#define WEBCAM_VIDEO_FRAME_SIZE   (WEBCAM_VIDEO_WIDTH * \
                                   WEBCAM_VIDEO_HEIGHT * 2)
#define WEBCAM_VIDEO_BUFFER_COUNT 3
#define WEBCAM_AUDIO_RATE         16000
#define WEBCAM_AUDIO_CHANNELS     2
#define WEBCAM_AUDIO_FRAMES       320
#define WEBCAM_AUDIO_FRAME_SIZE   (WEBCAM_AUDIO_CHANNELS * sizeof(int16_t))
#define WEBCAM_REQUEST_SIZE       1024
#define WEBCAM_HEADER_SIZE        256
#define WEBCAM_SOCKET_TIMEOUT_SEC 3

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum webcam_stream_e
{
  WEBCAM_STREAM_VIDEO = 0,
  WEBCAM_STREAM_AUDIO,
  WEBCAM_STREAM_COUNT
};

struct webcam_client_s
{
  int fd;
  enum webcam_stream_e stream;
  bool active;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static struct webcam_client_s g_clients[WEBCAM_STREAM_COUNT] =
{
  {
    .fd = -1,
    .stream = WEBCAM_STREAM_VIDEO,
  },
  {
    .fd = -1,
    .stream = WEBCAM_STREAM_AUDIO,
  }
};

static const char g_webcam_page[] =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Titan Mini Webcam</title>\n"
"<style>\n"
":root{color-scheme:light;--bg:#f4f4f1;--fg:#171717;--muted:#62625e;\n"
"--line:#d7d7d2;--surface:#fff;--accent:#0f766e;--error:#b42318}\n"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);\n"
"color:var(--fg);\n"
"font:15px system-ui,sans-serif;letter-spacing:0;min-height:100vh;\n"
"display:grid;place-items:center;padding:20px}.app{width:min(920px,100%,\n"
"max(280px,calc(133.333vh - 168px)))}\n"
"header{display:flex;align-items:center;justify-content:space-between;\n"
"gap:16px;margin-bottom:12px}h1{font-size:20px;line-height:1.2;margin:0}\n"
".state{display:flex;align-items:center;gap:8px;color:var(--muted);\n"
"font-size:13px}.dot{width:9px;height:9px;border-radius:50%;\n"
"background:var(--error)}.dot.live{background:var(--accent)}\n"
".viewer{background:#090909;border:1px solid #202020;border-radius:6px;\n"
"overflow:hidden;aspect-ratio:4/3;display:grid;place-items:center}\n"
"canvas{width:100%;height:100%;object-fit:contain;image-rendering:auto}\n"
"footer{display:flex;align-items:center;justify-content:space-between;\n"
"gap:12px;margin-top:12px;color:var(--muted);font-size:13px}\n"
"button{border:1px solid var(--fg);border-radius:5px;background:var(--fg);\n"
"color:#fff;font:inherit;font-weight:600;padding:9px 14px;cursor:pointer}\n"
"button.on{background:var(--surface);color:var(--fg)}\n"
"@media(max-width:520px){body{padding:12px}footer{align-items:flex-start;\n"
"flex-direction:column}button{width:100%}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<main class=\"app\">\n"
"<header><h1>Titan Mini Webcam</h1><div class=\"state\">\n"
"<span id=\"dot\" class=\"dot\"></span>\n"
"<span id=\"state\">Connecting</span>\n"
"</div></header>\n"
"<div class=\"viewer\">\n"
"<canvas id=\"video\" width=\"320\" height=\"240\"></canvas></div>\n"
"<footer><span id=\"stats\">320 x 240 YUYV</span>\n"
"<button id=\"audio\" type=\"button\">Start audio</button></footer>\n"
"</main>\n"
"<script>\n"
"const video=document.getElementById('video');\n"
"const videoContext=video.getContext('2d');\n"
"const videoImage=videoContext.createImageData(320,240);\n"
"const videoPixels=videoImage.data,videoFrameSize=320*240*2;\n"
"const dot=document.getElementById('dot');\n"
"const state=document.getElementById('state');\n"
"const audio=document.getElementById('audio');\n"
"const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));\n"
"function videoLive(){state.textContent='Live';dot.className='dot live'}\n"
"function videoRetry(){state.textContent='Reconnecting';\n"
"dot.className='dot'}\n"
"const clip=value=>Math.max(0,Math.min(255,value));\n"
"function videoPixel(y,u,v,offset){const c=298*(y-16),d=u-128,e=v-128;\n"
"videoPixels[offset]=clip((c+409*e+128)>>8);\n"
"videoPixels[offset+1]=clip((c-100*d-208*e+128)>>8);\n"
"videoPixels[offset+2]=clip((c+516*d+128)>>8);\n"
"videoPixels[offset+3]=255}\n"
"function videoRender(frame){let output=0;\n"
"for(let i=0;i<frame.length;i+=4){const u=frame[i+1],v=frame[i+3];\n"
"videoPixel(frame[i],u,v,output);output+=4;\n"
"videoPixel(frame[i+2],u,v,output);output+=4}\n"
"videoContext.putImageData(videoImage,0,0)}\n"
"async function videoLoop(){const frame=new Uint8Array(videoFrameSize);\n"
"for(;;){let offset=0;try{const response=await fetch('/video.yuyv',\n"
"{cache:'no-store'});if(!response.ok)throw new Error(response.status);\n"
"const reader=response.body.getReader();for(;;){\n"
"const part=await reader.read();\n"
"if(part.done)throw new Error('closed');let input=0;\n"
"while(input<part.value.length){\n"
"const count=Math.min(videoFrameSize-offset,part.value.length-input);\n"
"frame.set(part.value.subarray(input,input+count),offset);\n"
"input+=count;offset+=count;if(offset===videoFrameSize){\n"
"videoRender(frame);videoLive();offset=0}}}}\n"
"catch(error){videoRetry();await wait(500)}}}\n"
"window.addEventListener('load',videoLoop);\n"
"let audioRun=0,audioContext=null,audioAbort=null;\n"
"async function audioLoop(run){let carry=new Uint8Array(0),next=0;\n"
"while(run===audioRun){try{audioAbort=new AbortController();\n"
"const response=await fetch('/audio.pcm',{cache:'no-store',\n"
"signal:audioAbort.signal});\n"
"if(!response.ok)throw new Error(response.status);\n"
"const reader=response.body.getReader();for(;;){\n"
"const part=await reader.read();if(part.done)throw new Error('closed');\n"
"let data=new Uint8Array(carry.length+part.value.length);\n"
"data.set(carry);data.set(part.value,carry.length);\n"
"const bytes=data.length&~3,views=new DataView(data.buffer,0,bytes);\n"
"const count=bytes/4,buffer=audioContext.createBuffer(2,count,16000);\n"
"const left=buffer.getChannelData(0),right=buffer.getChannelData(1);\n"
"for(let i=0;i<count;i++){left[i]=views.getInt16(i*4,true)/32768;\n"
"right[i]=views.getInt16(i*4+2,true)/32768}\n"
"if(next<audioContext.currentTime+.04||\n"
"next>audioContext.currentTime+.5)next=audioContext.currentTime+.08;\n"
"const source=audioContext.createBufferSource();source.buffer=buffer;\n"
"source.connect(audioContext.destination);source.start(next);\n"
"next+=count/16000;carry=data.slice(bytes)}}\n"
"catch(error){if(run!==audioRun)return;await wait(500)}}}\n"
"audio.onclick=async()=>{if(audioContext){audioRun++;\n"
"if(audioAbort)audioAbort.abort();await audioContext.close();\n"
"audioContext=null;audio.className='';\n"
"audio.textContent='Start audio';return}\n"
"audioContext=new AudioContext();await audioContext.resume();audioRun++;\n"
"audio.className='on';audio.textContent='Mute audio';audioLoop(audioRun)};\n"
"</script>\n"
"</body>\n"
"</html>\n";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int webcam_send_all(int fd, FAR const void *buffer, size_t length)
{
  FAR const uint8_t *cursor = buffer;

  while (length > 0)
    {
      ssize_t nsent = send(fd, cursor, length, 0);

      if (nsent <= 0)
        {
          return nsent < 0 ? -errno : -EPIPE;
        }

      cursor += nsent;
      length -= nsent;
    }

  return OK;
}

static bool webcam_peer_closed(int fd)
{
  uint8_t byte;
  ssize_t nbytes;

  nbytes = recv(fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
  if (nbytes == 0)
    {
      return true;
    }

  return nbytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK;
}

static int webcam_send_response(int fd, FAR const char *status,
                                FAR const char *type,
                                FAR const void *body, size_t length)
{
  char header[WEBCAM_HEADER_SIZE];
  int header_length;
  int ret;

  header_length = snprintf(header, sizeof(header),
                           "HTTP/1.0 %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n",
                           status, type, length);
  if (header_length < 0 ||
      (size_t)header_length >= sizeof(header))
    {
      return -EOVERFLOW;
    }

  ret = webcam_send_all(fd, header, header_length);
  if (ret < 0 || length == 0)
    {
      return ret;
    }

  return webcam_send_all(fd, body, length);
}

static int webcam_send_stream_header(int fd, enum webcam_stream_e stream)
{
  FAR const char *header;

  if (stream == WEBCAM_STREAM_VIDEO)
    {
      header =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Video-Format: YUYV\r\n"
        "X-Video-Width: 320\r\n"
        "X-Video-Height: 240\r\n"
        "X-Video-Frame-Size: 153600\r\n\r\n";
    }
  else
    {
      header =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Audio-Format: S16LE\r\n"
        "X-Audio-Rate: 16000\r\n"
        "X-Audio-Channels: 2\r\n\r\n";
    }

  return webcam_send_all(fd, header, strlen(header));
}

static int webcam_send_video_frame(int fd, FAR const uint8_t *buffer,
                                   size_t length)
{
  return webcam_send_all(fd, buffer, length);
}

static void webcam_release_client(FAR struct webcam_client_s *client)
{
  close(client->fd);

  pthread_mutex_lock(&g_client_lock);
  client->fd = -1;
  client->active = false;
  pthread_mutex_unlock(&g_client_lock);
}

static FAR void *webcam_video_thread(FAR void *argument)
{
  FAR struct webcam_client_s *client = argument;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer vbuf;
  struct v4l2_format format;
  FAR uint8_t *buffers[WEBCAM_VIDEO_BUFFER_COUNT] =
  {
    MAP_FAILED, MAP_FAILED, MAP_FAILED
  };

  uint32_t lengths[WEBCAM_VIDEO_BUFFER_COUNT] =
  {
    0, 0, 0
  };

  bool registered = false;
  bool streaming = false;
  int fd = -1;
  int ret;
  int i;

  ret = capture_initialize(WEBCAM_VIDEO_DEVICE);
  if (ret < 0)
    {
      goto errout;
    }

  registered = true;
  fd = open(WEBCAM_VIDEO_DEVICE, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      ret = -errno;
      goto errout;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = WEBCAM_VIDEO_WIDTH;
  format.fmt.pix.height = WEBCAM_VIDEO_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      ret = -errno;
      goto errout;
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_MMAP;
  request.count = WEBCAM_VIDEO_BUFFER_COUNT;
  request.mode = V4L2_BUF_MODE_RING;
  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      ret = -errno;
      goto errout;
    }

  for (i = 0; i < WEBCAM_VIDEO_BUFFER_COUNT; i++)
    {
      memset(&vbuf, 0, sizeof(vbuf));
      vbuf.type = type;
      vbuf.memory = V4L2_MEMORY_MMAP;
      vbuf.index = i;
      if (ioctl(fd, VIDIOC_QUERYBUF, (uintptr_t)&vbuf) < 0)
        {
          ret = -errno;
          goto errout;
        }

      lengths[i] = vbuf.length;
      buffers[i] = mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, vbuf.m.offset);
      if (buffers[i] == MAP_FAILED)
        {
          ret = -errno;
          goto errout;
        }

      vbuf.length = WEBCAM_VIDEO_FRAME_SIZE;
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&vbuf) < 0)
        {
          ret = -errno;
          goto errout;
        }
    }

  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      ret = -errno;
      goto errout;
    }

  streaming = true;
  ret = webcam_send_stream_header(client->fd, WEBCAM_STREAM_VIDEO);
  while (ret == OK)
    {
      memset(&vbuf, 0, sizeof(vbuf));
      vbuf.type = type;
      vbuf.memory = V4L2_MEMORY_MMAP;
      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&vbuf) < 0)
        {
          if (errno == EAGAIN)
            {
              if (webcam_peer_closed(client->fd))
                {
                  ret = -EPIPE;
                  break;
                }

              usleep(10000);
              continue;
            }

          ret = -errno;
          break;
        }

      if (vbuf.index >= WEBCAM_VIDEO_BUFFER_COUNT ||
          vbuf.bytesused != WEBCAM_VIDEO_FRAME_SIZE)
        {
          ret = -EIO;
          break;
        }

      if (webcam_peer_closed(client->fd))
        {
          ret = -EPIPE;
        }
      else
        {
          ret = webcam_send_video_frame(client->fd, buffers[vbuf.index],
                                        WEBCAM_VIDEO_FRAME_SIZE);
        }

      vbuf.length = WEBCAM_VIDEO_FRAME_SIZE;
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&vbuf) < 0 && ret == OK)
        {
          ret = -errno;
        }
    }

errout:
  if (!streaming)
    {
      static const char message[] = "Video capture unavailable\n";

      webcam_send_response(client->fd, "503 Service Unavailable",
                           "text/plain", message, sizeof(message) - 1);
    }
  else
    {
      ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
    }

  for (i = 0; i < WEBCAM_VIDEO_BUFFER_COUNT; i++)
    {
      if (buffers[i] != MAP_FAILED)
        {
          munmap(buffers[i], lengths[i]);
        }
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (registered)
    {
      capture_uninitialize(WEBCAM_VIDEO_DEVICE);
    }

  webcam_release_client(client);
  return NULL;
}

static FAR void *webcam_audio_thread(FAR void *argument)
{
  FAR struct webcam_client_s *client = argument;
  int16_t samples[WEBCAM_AUDIO_FRAMES * WEBCAM_AUDIO_CHANNELS];
  struct pollfd pfd;
  int fd;
  int ret;

  fd = open(WEBCAM_AUDIO_DEVICE, O_RDONLY);
  if (fd < 0)
    {
      static const char message[] = "Audio capture unavailable\n";

      webcam_send_response(client->fd, "503 Service Unavailable",
                           "text/plain", message, sizeof(message) - 1);
      webcam_release_client(client);
      return NULL;
    }

  ret = webcam_send_stream_header(client->fd, WEBCAM_STREAM_AUDIO);
  pfd.fd = fd;
  pfd.events = POLLIN;
  while (ret == OK)
    {
      ssize_t nbytes;

      if (webcam_peer_closed(client->fd))
        {
          ret = -EPIPE;
          break;
        }

      pfd.revents = 0;
      ret = poll(&pfd, 1, 2000);
      if (ret <= 0)
        {
          ret = ret < 0 ? -errno : -ETIMEDOUT;
          break;
        }

      nbytes = read(fd, samples, sizeof(samples));
      if (nbytes <= 0 ||
          (nbytes % WEBCAM_AUDIO_FRAME_SIZE) != 0)
        {
          ret = nbytes < 0 ? -errno : -EIO;
          break;
        }

      if (webcam_peer_closed(client->fd))
        {
          ret = -EPIPE;
        }
      else
        {
          ret = webcam_send_all(client->fd, samples, nbytes);
        }
    }

  close(fd);
  webcam_release_client(client);
  return NULL;
}

static int webcam_start_stream(int fd, enum webcam_stream_e stream)
{
  FAR struct webcam_client_s *client = &g_clients[stream];
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_mutex_lock(&g_client_lock);
  if (client->active)
    {
      pthread_mutex_unlock(&g_client_lock);
      return -EBUSY;
    }

  client->fd = fd;
  client->active = true;
  pthread_mutex_unlock(&g_client_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_SYSTEM_WEBCAM_STREAM_STACKSIZE);
  ret = pthread_create(&thread, &attr,
                       stream == WEBCAM_STREAM_VIDEO ?
                       webcam_video_thread : webcam_audio_thread,
                       client);
  pthread_attr_destroy(&attr);
  if (ret != OK)
    {
      pthread_mutex_lock(&g_client_lock);
      client->fd = -1;
      client->active = false;
      pthread_mutex_unlock(&g_client_lock);
      return -ret;
    }

  pthread_detach(thread);
  return OK;
}

static int webcam_get_path(FAR const char *request, FAR char *path,
                           size_t path_size)
{
  FAR const char *start;
  FAR const char *end;
  size_t length;

  if (strncmp(request, "GET ", 4) != 0)
    {
      return -ENOSYS;
    }

  start = request + 4;
  end = strchr(start, ' ');
  if (end == NULL)
    {
      return -EINVAL;
    }

  length = end - start;
  if (length == 0 || length >= path_size)
    {
      return -ENAMETOOLONG;
    }

  memcpy(path, start, length);
  path[length] = '\0';
  return OK;
}

static void webcam_handle_client(int fd)
{
  static const char busy[] = "Stream already in use\n";
  static const char missing[] = "Not found\n";
  static const char unsupported[] = "Only GET is supported\n";
  char request[WEBCAM_REQUEST_SIZE];
  char path[64];
  size_t received = 0;
  int ret;

  do
    {
      ssize_t nbytes = recv(fd, request + received,
                            sizeof(request) - received - 1, 0);

      if (nbytes <= 0)
        {
          close(fd);
          return;
        }

      received += nbytes;
      request[received] = '\0';
    }
  while (strstr(request, "\r\n\r\n") == NULL &&
         received < sizeof(request) - 1);

  if (strstr(request, "\r\n\r\n") == NULL)
    {
      webcam_send_response(fd, "431 Request Header Fields Too Large",
                           "text/plain", missing, sizeof(missing) - 1);
      close(fd);
      return;
    }

  ret = webcam_get_path(request, path, sizeof(path));
  if (ret < 0)
    {
      webcam_send_response(fd, "405 Method Not Allowed", "text/plain",
                           unsupported, sizeof(unsupported) - 1);
      close(fd);
      return;
    }

  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    {
      webcam_send_response(fd, "200 OK", "text/html; charset=utf-8",
                           g_webcam_page, sizeof(g_webcam_page) - 1);
      close(fd);
    }
  else if (strcmp(path, "/video.yuyv") == 0)
    {
      ret = webcam_start_stream(fd, WEBCAM_STREAM_VIDEO);
      if (ret < 0)
        {
          webcam_send_response(fd, "409 Conflict", "text/plain",
                               busy, sizeof(busy) - 1);
          close(fd);
        }
    }
  else if (strcmp(path, "/audio.pcm") == 0)
    {
      ret = webcam_start_stream(fd, WEBCAM_STREAM_AUDIO);
      if (ret < 0)
        {
          webcam_send_response(fd, "409 Conflict", "text/plain",
                               busy, sizeof(busy) - 1);
          close(fd);
        }
    }
  else if (strcmp(path, "/favicon.ico") == 0)
    {
      webcam_send_response(fd, "204 No Content", "image/x-icon", NULL, 0);
      close(fd);
    }
  else
    {
      webcam_send_response(fd, "404 Not Found", "text/plain",
                           missing, sizeof(missing) - 1);
      close(fd);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct sockaddr_in address;
  struct timeval timeout;
  int reuse = 1;
  int listenfd;
  int ret;

  (void)argc;
  (void)argv;

  signal(SIGPIPE, SIG_IGN);

  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    {
      fprintf(stderr, "webcam: socket failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(CONFIG_SYSTEM_WEBCAM_PORT);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listenfd, (FAR struct sockaddr *)&address, sizeof(address)) < 0)
    {
      fprintf(stderr, "webcam: bind port %d failed: %d\n",
              CONFIG_SYSTEM_WEBCAM_PORT, errno);
      close(listenfd);
      return EXIT_FAILURE;
    }

  if (listen(listenfd, 4) < 0)
    {
      fprintf(stderr, "webcam: listen failed: %d\n", errno);
      close(listenfd);
      return EXIT_FAILURE;
    }

  printf("webcam: listening on http://0.0.0.0:%d/\n",
         CONFIG_SYSTEM_WEBCAM_PORT);
  printf("webcam: one video and one audio client are supported\n");

  timeout.tv_sec = WEBCAM_SOCKET_TIMEOUT_SEC;
  timeout.tv_usec = 0;
  for (; ; )
    {
      int clientfd = accept(listenfd, NULL, NULL);

      if (clientfd < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "webcam: accept failed: %d\n", errno);
          ret = EXIT_FAILURE;
          break;
        }

      setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO,
                 &timeout, sizeof(timeout));
      setsockopt(clientfd, SOL_SOCKET, SO_SNDTIMEO,
                 &timeout, sizeof(timeout));
      webcam_handle_client(clientfd);
    }

  close(listenfd);
  return ret;
}
