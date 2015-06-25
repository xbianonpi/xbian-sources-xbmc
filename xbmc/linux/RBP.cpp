/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "RBP.h"
#if defined(TARGET_RASPBERRY_PI)

#include <assert.h>
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"

#include "cores/omxplayer/OMXImage.h"

#include "guilib/GraphicContext.h"
#include "settings/DisplaySettings.h"

#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include "rpi_user_vcsm.h"

#define MAJOR_NUM 100
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char *)
#define DEVICE_FILE_NAME "/dev/vcio"

typedef struct gpu_mem_ptr_s {
  void *arm; // Pointer to memory mapped on ARM side
  int vc_handle;   // Videocore handle of relocatable memory
  int vcsm_handle; // Handle for use by VCSM
  int vc;       // Address for use in GPU code
  int numbytes; // Size of memory block
  int suballoc;
} GPU_MEM_PTR_T;

static int mbox_open();
static void mbox_close(int file_desc);
static void gpu_free_internal(GPU_MEM_PTR_T *p, int mb);
static int gpu_malloc_uncached_internal(int numbytes, GPU_MEM_PTR_T *p, int mb);

CRBP::CRBP()
{
  m_initialized     = false;
  m_omx_initialized = false;
  m_DllBcmHost      = new DllBcmHost();
  m_OMX             = new COMXCore();
  m_display = DISPMANX_NO_HANDLE;
  m_p = NULL;
  m_mb = mbox_open();
  vcsm_init();
}

CRBP::~CRBP()
{
  Deinitialize();
  delete m_OMX;
  delete m_DllBcmHost;
}

void CRBP::InitializeSettings()
{
  if (m_initialized && g_advancedSettings.m_cacheMemBufferSize == ~0U)
    g_advancedSettings.m_cacheMemBufferSize = m_arm_mem < 256 ? 1024 * 1024 * 2 : 1024 * 1024 * 20;
}

bool CRBP::Initialize()
{
  CSingleLock lock (m_critSection);
  if (m_initialized)
    return true;

  m_initialized = m_DllBcmHost->Load();
  if(!m_initialized)
    return false;

  m_DllBcmHost->bcm_host_init();

  m_omx_initialized = m_OMX->Initialize();
  if(!m_omx_initialized)
    return false;

  char response[80] = "";
  m_arm_mem = 0;
  m_gpu_mem = 0;
  m_codec_mpg2_enabled = false;
  m_codec_wvc1_enabled = false;

  if (vc_gencmd(response, sizeof response, "get_mem arm") == 0)
    vc_gencmd_number_property(response, "arm", &m_arm_mem);
  if (vc_gencmd(response, sizeof response, "get_mem gpu") == 0)
    vc_gencmd_number_property(response, "gpu", &m_gpu_mem);

  if (vc_gencmd(response, sizeof response, "codec_enabled MPG2") == 0)
    m_codec_mpg2_enabled = strcmp("MPG2=enabled", response) == 0;
  if (vc_gencmd(response, sizeof response, "codec_enabled WVC1") == 0)
    m_codec_wvc1_enabled = strcmp("WVC1=enabled", response) == 0;

  if (m_gpu_mem < 128)
    setenv("V3D_DOUBLE_BUFFER", "1", 1);

  m_gui_resolution_limit = CSettings::Get().GetInt("videoscreen.limitgui");
  if (!m_gui_resolution_limit)
    m_gui_resolution_limit = m_gpu_mem < 128 ? 720:1080;

  InitializeSettings();

  g_OMXImage.Initialize();
  m_omx_image_init = true;
  return true;
}

void CRBP::LogFirmwareVerison()
{
  char  response[1024];
  m_DllBcmHost->vc_gencmd(response, sizeof response, "version");
  response[sizeof(response) - 1] = '\0';
  CLog::Log(LOGNOTICE, "Raspberry PI firmware version: %s", response);
  CLog::Log(LOGNOTICE, "ARM mem: %dMB GPU mem: %dMB MPG2:%d WVC1:%d", m_arm_mem, m_gpu_mem, m_codec_mpg2_enabled, m_codec_wvc1_enabled);
  CLog::Log(LOGNOTICE, "cacheMemBufferSize: %dMB",  g_advancedSettings.m_cacheMemBufferSize >> 20);
  m_DllBcmHost->vc_gencmd(response, sizeof response, "get_config int");
  response[sizeof(response) - 1] = '\0';
  CLog::Log(LOGNOTICE, "Config:\n%s", response);
  m_DllBcmHost->vc_gencmd(response, sizeof response, "get_config str");
  response[sizeof(response) - 1] = '\0';
  CLog::Log(LOGNOTICE, "Config:\n%s", response);
}

DISPMANX_DISPLAY_HANDLE_T CRBP::OpenDisplay(uint32_t device)
{
  if (m_display == DISPMANX_NO_HANDLE)
  {
    m_display = vc_dispmanx_display_open( 0 /*screen*/ );
    init_cursor();
  }
  return m_display;
}

void CRBP::CloseDisplay(DISPMANX_DISPLAY_HANDLE_T display)
{
  assert(display == m_display);
  vc_dispmanx_display_close(m_display);
  m_display = DISPMANX_NO_HANDLE;
  uninit_cursor();
}

void CRBP::GetDisplaySize(int &width, int &height)
{
  DISPMANX_MODEINFO_T info;
  if (vc_dispmanx_display_get_info(m_display, &info) == 0)
  {
    width = info.width;
    height = info.height;
  }
  else
  {
    width = 0;
    height = 0;
  }
}

unsigned char *CRBP::CaptureDisplay(int width, int height, int *pstride, bool swap_red_blue, bool video_only)
{
  DISPMANX_RESOURCE_HANDLE_T resource;
  VC_RECT_T rect;
  unsigned char *image = NULL;
  uint32_t vc_image_ptr;
  int stride;
  uint32_t flags = 0;

  if (video_only)
    flags |= DISPMANX_SNAPSHOT_NO_RGB|DISPMANX_SNAPSHOT_FILL;
  if (swap_red_blue)
    flags |= DISPMANX_SNAPSHOT_SWAP_RED_BLUE;
  if (!pstride)
    flags |= DISPMANX_SNAPSHOT_PACK;

  stride = ((width + 15) & ~15) * 4;
  image = new unsigned char [height * stride];

  if (image)
  {
    resource = vc_dispmanx_resource_create( VC_IMAGE_RGBA32, width, height, &vc_image_ptr );

    assert(m_display != DISPMANX_NO_HANDLE);
    vc_dispmanx_snapshot(m_display, resource, (DISPMANX_TRANSFORM_T)flags);

    vc_dispmanx_rect_set(&rect, 0, 0, width, height);
    vc_dispmanx_resource_read_data(resource, &rect, image, stride);
    vc_dispmanx_resource_delete( resource );
  }
  if (pstride)
    *pstride = stride;
  return image;
}


static void vsync_callback(DISPMANX_UPDATE_HANDLE_T u, void *arg)
{
  CEvent *sync = (CEvent *)arg;
  sync->Set();
}

void CRBP::WaitVsync()
{
  int s;
  DISPMANX_DISPLAY_HANDLE_T m_display = vc_dispmanx_display_open( 0 /*screen*/ );
  if (m_display == DISPMANX_NO_HANDLE)
  {
    CLog::Log(LOGDEBUG, "CRBP::%s skipping while display closed", __func__);
    return;
  }
  m_vsync.Reset();
  s = vc_dispmanx_vsync_callback(m_display, vsync_callback, (void *)&m_vsync);
  if (s == 0)
  {
    m_vsync.WaitMSec(1000);
  }
  else assert(0);
  s = vc_dispmanx_vsync_callback(m_display, NULL, NULL);
  assert(s == 0);
  vc_dispmanx_display_close( m_display );
}


void CRBP::Deinitialize()
{
  if (m_omx_image_init)
    g_OMXImage.Deinitialize();

  if(m_omx_initialized)
    m_OMX->Deinitialize();

  if (m_display)
    CloseDisplay(m_display);

  m_DllBcmHost->bcm_host_deinit();

  if(m_initialized)
    m_DllBcmHost->Unload();

  m_omx_image_init  = false;
  m_initialized     = false;
  m_omx_initialized = false;
  uninit_cursor();
  if (m_mb && m_p)
    gpu_free_internal(m_p, m_mb);
  delete m_p;
  m_p = NULL;
  if (m_mb)
    mbox_close(m_mb);
  m_mb = 0;
  vcsm_exit();
}

double CRBP::AdjustHDMIClock(double adjust)
{
  char response[80];
  vc_gencmd(response, sizeof response, "hdmi_adjust_clock %f", adjust);
  float new_adjust = 1.0f;
  char *p = strchr(response, '=');
  if (p)
    new_adjust = atof(p+1);
  CLog::Log(LOGDEBUG, "CRBP::%s(%.4f) = %.4f", __func__, adjust, new_adjust);
  return new_adjust;
}


static int mbox_property(int file_desc, void *buf)
{
   int ret_val = ioctl(file_desc, IOCTL_MBOX_PROPERTY, buf);

   if (ret_val < 0) {
      printf("ioctl_set_msg failed:%d\n", ret_val);
   }
   return ret_val;
}

static int mbox_open()
{
   int file_desc;

   // open a char device file used for communicating with kernel mbox driver
   file_desc = open(DEVICE_FILE_NAME, 0);
   if (file_desc < 0) {
      printf("Can't open device file: %s (%d)\n", DEVICE_FILE_NAME, file_desc);
      printf("Try creating a device file with: sudo mknod %s c %d 0\n", DEVICE_FILE_NAME, MAJOR_NUM);
   }
   return file_desc;
}

static void mbox_close(int file_desc)
{
  close(file_desc);
}

static unsigned mem_lock(int file_desc, unsigned handle)
{
   int i=0;
   unsigned p[32];
   p[i++] = 0; // size
   p[i++] = 0x00000000; // process request

   p[i++] = 0x3000d; // (the tag id)
   p[i++] = 4; // (size of the buffer)
   p[i++] = 4; // (size of the data)
   p[i++] = handle;

   p[i++] = 0x00000000; // end tag
   p[0] = i*sizeof *p; // actual size

   mbox_property(file_desc, p);
   return p[5];
}

unsigned mem_unlock(int file_desc, unsigned handle)
{
   int i=0;
   unsigned p[32];
   p[i++] = 0; // size
   p[i++] = 0x00000000; // process request

   p[i++] = 0x3000e; // (the tag id)
   p[i++] = 4; // (size of the buffer)
   p[i++] = 4; // (size of the data)
   p[i++] = handle;

   p[i++] = 0x00000000; // end tag
   p[0] = i*sizeof *p; // actual size

   mbox_property(file_desc, p);
   return p[5];
}

unsigned int mailbox_set_cursor_info(int file_desc, int width, int height, int format, uint32_t buffer, int hotspotx, int hotspoty)
{
   int i=0;
   unsigned int p[32];
   p[i++] = 0; // size
   p[i++] = 0x00000000; // process request
   p[i++] = 0x00008010; // set cursor state
   p[i++] = 24; // buffer size
   p[i++] = 24; // data size

   p[i++] = width;
   p[i++] = height;
   p[i++] = format;
   p[i++] = buffer;           // ptr to VC memory buffer. Doesn't work in 64bit....
   p[i++] = hotspotx;
   p[i++] = hotspoty;

   p[i++] = 0x00000000; // end tag
   p[0] = i*sizeof(*p); // actual size

   mbox_property(file_desc, p);
   return p[5];

}

unsigned int mailbox_set_cursor_position(int file_desc, int enabled, int x, int y)
{
   int i=0;
   unsigned p[32];
   p[i++] = 0; // size
   p[i++] = 0x00000000; // process request
   p[i++] = 0x00008011; // set cursor state
   p[i++] = 12; // buffer size
   p[i++] = 12; // data size

   p[i++] = enabled;
   p[i++] = x;
   p[i++] = y;

   p[i++] = 0x00000000; // end tag
   p[0] = i*sizeof *p; // actual size

   mbox_property(file_desc, p);
   return p[5];
}

static int gpu_malloc_uncached_internal(int numbytes, GPU_MEM_PTR_T *p, int mb)
{
  //printf("%s %d\n", __func__, numbytes);

  p->numbytes = numbytes;
  p->suballoc = 0;
  p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Mouse pointer");
  assert(p->vcsm_handle);
  p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
  assert(p->vc_handle);
  p->arm = vcsm_lock(p->vcsm_handle);
  assert(p->arm);
  p->vc = mem_lock(mb, p->vc_handle);
  assert(p->vc);
  return 0;
}

static void gpu_free_internal(GPU_MEM_PTR_T *p, int mb)
{
  mem_unlock(mb,p->vc_handle);
  vcsm_unlock_ptr(p->arm);
  vcsm_free(p->vcsm_handle);
}

#define T 0
#define W 0xffffffff
#define B 0xff000000

const static uint32_t default_cursor_pixels[] =
{
   B,B,B,B,B,B,B,B,B,T,T,T,T,T,T,T,
   B,W,W,W,W,W,W,B,T,T,T,T,T,T,T,T,
   B,W,W,W,W,W,B,T,T,T,T,T,T,T,T,T,
   B,W,W,W,W,B,T,T,T,T,T,T,T,T,T,T,
   B,W,W,W,W,W,B,T,T,T,T,T,T,T,T,T,
   B,W,W,B,W,W,W,B,T,T,T,T,T,T,T,T,
   B,W,B,T,B,W,W,W,B,T,T,T,T,T,T,T,
   B,B,T,T,T,B,W,W,W,B,T,T,T,T,T,T,
   B,T,T,T,T,T,B,W,W,W,B,T,T,T,T,T,
   T,T,T,T,T,T,T,B,W,W,W,B,T,T,T,T,
   T,T,T,T,T,T,T,T,B,W,W,W,B,T,T,T,
   T,T,T,T,T,T,T,T,T,B,W,W,W,B,T,T,
   T,T,T,T,T,T,T,T,T,T,B,W,W,W,B,T,
   T,T,T,T,T,T,T,T,T,T,T,B,W,W,W,B,
   T,T,T,T,T,T,T,T,T,T,T,T,B,W,B,T,
   T,T,T,T,T,T,T,T,T,T,T,T,T,B,T,T
};

#undef T
#undef W
#undef B

void CRBP::init_cursor()
{
  //printf("%s\n", __func__);
  if (!m_mb)
    return;
  if (!m_p)
  {
    m_p = new GPU_MEM_PTR_T;
    if (m_p)
      gpu_malloc_uncached_internal(64 * 64 * 4, m_p, m_mb);
  }
  if (m_mb && m_p && m_p->arm && m_p->vc)
    set_cursor(default_cursor_pixels, 16, 16, 0, 0);
}

void CRBP::set_cursor(const void *pixels, int width, int height, int hotspot_x, int hotspot_y)
{
  if (!m_mb || !m_p || !m_p->arm || !m_p->vc || !pixels || width * height > 64 * 64)
    return;
  //printf("%s %dx%d %p\n", __func__, width, height, pixels);
  memcpy(m_p->arm, pixels, width * height * 4);
  unsigned int s = mailbox_set_cursor_info(m_mb, width, height, 0, m_p->vc, hotspot_x, hotspot_y);
  assert(s == 0);
}

void CRBP::update_cursor(int x, int y, bool enabled)
{
  if (!m_mb || !m_p || !m_p->arm || !m_p->vc)
    return;

  RESOLUTION res = g_graphicsContext.GetVideoResolution();
  CRect gui(0, 0, CDisplaySettings::Get().GetResolutionInfo(res).iWidth, CDisplaySettings::Get().GetResolutionInfo(res).iHeight);
  CRect display(0, 0, CDisplaySettings::Get().GetResolutionInfo(res).iScreenWidth, CDisplaySettings::Get().GetResolutionInfo(res).iScreenHeight);

  int x2 = x * display.Width()  / gui.Width();
  int y2 = y * display.Height() / gui.Height();

  //printf("%s %d,%d (%d)\n", __func__, x, y, enabled);
  mailbox_set_cursor_position(m_mb, enabled, x2, y2);
}

void CRBP::uninit_cursor()
{
  if (!m_mb || !m_p || !m_p->arm || !m_p->vc)
    return;
  //printf("%s\n", __func__);
  mailbox_set_cursor_position(m_mb, 0, 0, 0);
}

#endif
