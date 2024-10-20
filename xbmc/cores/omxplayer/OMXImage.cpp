/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OMXImage.h"

#include "messaging/ApplicationMessenger.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/rpi/WinSystemRpiGLESContext.h"

#include "platform/linux/RBP.h"

#include <algorithm>
#include <cassert>
#include <inttypes.h>

#include <sys/time.h>

#ifdef _DEBUG
#define CheckError() { GLint result = eglGetError(); if (result != EGL_SUCCESS) CLog::Log(LOGERROR, "EGL error in {}: {:x}", __FUNCTION__, result); }
#else
#define CheckError()
#endif

#define EXIF_TAG_ORIENTATION    0x0112


// A helper for restricting threads calling GPU functions to limit memory use
// Experimentally, 3 outstanding operations is optimal
static XbmcThreads::ConditionVariable g_count_cond;
static CCriticalSection               g_count_lock;
static int g_count_val;

static bool limit_calls_enter()
{
  std::unique_lock<CCriticalSection> lock(g_count_lock);
  // on Pi2 fall back to arm decode if the queue is getting big
  if (g_RBP.RaspberryPiVersion() > 1 && g_count_val >= 2)
    return false;

  while (g_count_val >= 3)
    g_count_cond.wait(lock);
  g_count_val++;
  return true;
}

static void limit_calls_leave()
{
  std::unique_lock<CCriticalSection> lock(g_count_lock);
  g_count_val--;
  g_count_cond.notifyAll();
}


#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImage"

using namespace XFILE;

COMXImage::COMXImage()
: CThread("CRBPWorker")
{
  m_egl_context = EGL_NO_CONTEXT;
}

COMXImage::~COMXImage()
{
  Deinitialize();
}

void COMXImage::Initialize()
{
  Create();
}

void COMXImage::Deinitialize()
{
  // wake up thread so it can quit
  {
    std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
    m_bStop = true;
    m_texqueue_cond.notifyAll();
  }
  if (IsRunning())
    StopThread();
}

bool COMXImage::CreateThumbnailFromSurface(unsigned char* buffer, unsigned int width, unsigned int height,
      unsigned int format, unsigned int pitch, const std::string& destFile)
{
  COMXImageEnc omxImageEnc;
  if (!omxImageEnc.Gpu())
    return false;

  bool ret = omxImageEnc.CreateThumbnailFromSurface(buffer, width, height, format, pitch, destFile);
  if (!ret)
    CLog::Log(LOGINFO, "{}: unable to create thumbnail {} {}x{}", __func__, destFile.c_str(), width,
              height);
  return ret;
}

COMXImageFile *COMXImage::LoadJpeg(const std::string& texturePath)
{
  COMXImageFile *file = new COMXImageFile();
  if (!file->ReadFile(texturePath))
  {
    delete file;
    file = NULL;
  }
  return file;
}

void COMXImage::CloseJpeg(COMXImageFile *file)
{
  delete file;
}

bool COMXImage::DecodeJpeg(COMXImageFile *file, unsigned int width, unsigned int height, unsigned int stride, void *pixels)
{
  bool ret = false;
  COMXImageDec omx_image;
  if (omx_image.Decode(file->GetImageBuffer(), file->GetImageSize(), width, height, stride, pixels))
  {
    assert(width  == omx_image.GetDecodedWidth());
    assert(height == omx_image.GetDecodedHeight());
    assert(stride == omx_image.GetDecodedStride());
    ret = true;
  }
  else
    CLog::Log(LOGINFO, "{}: unable to decode {} {}x{}", __func__, file->GetFilename(), width,
              height);
  omx_image.Close();
  return ret;
}

bool COMXImage::ClampLimits(unsigned int &width, unsigned int &height, unsigned int m_width, unsigned int m_height, bool transposed)
{
  unsigned int owidth = width;
  unsigned int oheight = height;
  unsigned int max_width = width;
  unsigned int max_height = height;
  const float aspect = (float)m_width / m_height;
  bool clamped = false;

  if (max_width == 0 || max_height == 0)
  {
    const std::shared_ptr<CAdvancedSettings> advancedSettings = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();

    max_height = advancedSettings->m_imageRes;

    if (advancedSettings->m_fanartRes > advancedSettings->m_imageRes)
    { // 16x9 images larger than the fanart res use that rather than the image res
      if (fabsf(aspect / (16.0f/9.0f) - 1.0f) <= 0.01f && m_height >= advancedSettings->m_fanartRes)
      {
        max_height = advancedSettings->m_fanartRes;
      }
    }
    max_width = max_height * 16/9;

    RESOLUTION_INFO& res_info = CDisplaySettings::GetInstance().GetResolutionInfo(CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution());
    const unsigned int gui_width = transposed ? res_info.iHeight:res_info.iWidth;
    const unsigned int gui_height = transposed ? res_info.iWidth:res_info.iHeight;
    if (gui_width)
      max_width = std::min(max_width, gui_width);
    if (gui_height)
      max_height = std::min(max_height, gui_height);
  }
  max_width  = std::min(max_width, 2048U);
  max_height = std::min(max_height, 2048U);

  width = m_width;
  height = m_height;
  if (width > max_width || height > max_height)
  {
    if ((unsigned int)(max_width / aspect + 0.5f) > max_height)
      max_width = (unsigned int)(max_height * aspect + 0.5f);
    else
      max_height = (unsigned int)(max_width / aspect + 0.5f);
    width = max_width;
    height = max_height;
    clamped = true;
  }

  CLog::Log(LOGDEBUG, "{}: {}x{} {}x{} -> {}x{} clamped={}", __func__, owidth, oheight, m_width, m_height, width, height, clamped);
  return clamped;
}

bool COMXImage::CreateThumb(const std::string& srcFile, unsigned int maxHeight, unsigned int maxWidth, std::string &additional_info, const std::string& destFile)
{
  bool okay = false;
  COMXImageFile file;
  COMXImageReEnc reenc;
  if (!reenc.Gpu())
    return false;
  void *pDestBuffer;
  unsigned int nDestSize;
  int orientation = additional_info == "flipped" ? 1:0;
  if (URIUtils::HasExtension(srcFile, ".jpg|.tbn") && file.ReadFile(srcFile, orientation) && reenc.ReEncode(file, maxWidth, maxHeight, pDestBuffer, nDestSize))
  {
    XFILE::CFile outfile;
    if (outfile.OpenForWrite(destFile, true))
    {
      outfile.Write(pDestBuffer, nDestSize);
      outfile.Close();
      okay = true;
    }
    else
      CLog::Log(LOGERROR, "{}: can't open output file: {}", __func__, destFile.c_str());
  }
  return okay;
}

bool COMXImage::SendMessage(bool (*callback)(EGLDisplay egl_display, EGLContext egl_context, void *cookie), void *cookie)
{
  // we can only call gl functions from the application thread or texture thread
  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    CWinSystemRpiGLESContext *winsystem = static_cast<CWinSystemRpiGLESContext *>(CServiceBroker::GetWinSystem());
    return callback(winsystem->GetEGLDisplay(), GetEGLContext(), cookie);
  }
  struct callbackinfo mess;
  mess.callback = callback;
  mess.cookie = cookie;
  mess.result = false;
  mess.sync.Reset();
  {
    std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
    m_texqueue.push(&mess);
    m_texqueue_cond.notifyAll();
  }
  // wait for function to have finished (in texture thread)
  mess.sync.Wait();
  // need to ensure texture thread has returned from mess.sync.Set() before we exit and free tex
  std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
  return mess.result;
}


static bool AllocTextureCallback(EGLDisplay egl_display, EGLContext egl_context, void *cookie)
{
  struct COMXImage::textureinfo *tex = static_cast<struct COMXImage::textureinfo *>(cookie);
  COMXImage *img = static_cast<COMXImage*>(tex->parent);
  return img->AllocTextureInternal(egl_display, egl_context, tex);
}

bool COMXImage::AllocTextureInternal(EGLDisplay egl_display, EGLContext egl_context, struct textureinfo *tex)
{
  glGenTextures(1, (GLuint*) &tex->texture);
  glBindTexture(GL_TEXTURE_2D, tex->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLenum type = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool("videoscreen.textures32") ? GL_UNSIGNED_BYTE:GL_UNSIGNED_SHORT_5_6_5;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex->width, tex->height, 0, GL_RGB, type, 0);
  tex->egl_image = eglCreateImageKHR(egl_display, egl_context, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)tex->texture, NULL);
  if (!tex->egl_image)
  {
    CLog::Log(LOGDEBUG, "{}: eglCreateImageKHR failed to allocate", __func__);
  }
  CheckError();
  return true;
}

void COMXImage::GetTexture(void *userdata, GLuint *texture)
{
  struct textureinfo *tex = static_cast<struct textureinfo *>(userdata);
  *texture = tex->texture;
}

static bool DestroyTextureCallback(EGLDisplay egl_display, EGLContext egl_context, void *cookie)
{
  struct COMXImage::textureinfo *tex = static_cast<struct COMXImage::textureinfo *>(cookie);
  COMXImage *img = static_cast<COMXImage*>(tex->parent);
  return img->DestroyTextureInternal(egl_display, egl_context, tex);
}

void COMXImage::DestroyTexture(void *userdata)
{
  SendMessage(DestroyTextureCallback, userdata);
}

bool COMXImage::DestroyTextureInternal(EGLDisplay egl_display, EGLContext egl_context, struct textureinfo *tex)
{
  bool s = true;
  if (tex->egl_image)
  {
    s = eglDestroyImageKHR(egl_display, tex->egl_image);
    if (!s)
      CLog::Log(LOGINFO, "{}: failed to destroy texture", __func__);
  }
  if (tex->texture)
    glDeleteTextures(1, (GLuint*) &tex->texture);
  return s;
}

bool COMXImage::DecodeJpegToTexture(COMXImageFile *file, unsigned int width, unsigned int height, void **userdata)
{
  bool ret = false;
  COMXTexture omx_image;

  if (!omx_image.Gpu())
    return false;

  struct textureinfo *tex = new struct textureinfo;
  if (!tex)
    return NULL;

  tex->parent = (void *)this;
  tex->width = width;
  tex->height = height;
  tex->texture = 0;
  tex->egl_image = NULL;

  SendMessage(AllocTextureCallback, tex);

  if (tex->egl_image && tex->texture && omx_image.Decode(file->GetImageBuffer(), file->GetImageSize(), width, height, tex->egl_image))
  {
    ret = true;
    *userdata = tex;
    CLog::Log(LOGDEBUG, "{}: decoded {} {}x{}", __func__, file->GetFilename(), width, height);
  }
  else
  {
    CLog::Log(LOGINFO, "{}: unable to decode to texture {} {}x{}", __func__, file->GetFilename(),
              width, height);
    DestroyTexture(tex);
  }
  return ret;
}

EGLContext COMXImage::GetEGLContext()
{
  std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
  CWinSystemRpiGLESContext *winsystem = static_cast<CWinSystemRpiGLESContext *>(CServiceBroker::GetWinSystem());
  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
    return winsystem->GetEGLContext();
  if (m_egl_context == EGL_NO_CONTEXT)
    CreateContext();
  return m_egl_context;
}

static bool ChooseConfig(EGLDisplay display, const EGLint *configAttrs, EGLConfig *config)
{
  EGLBoolean eglStatus = true;
  EGLint     configCount = 0;
  EGLConfig* configList = NULL;
  // Find out how many configurations suit our needs
  eglStatus = eglChooseConfig(display, configAttrs, NULL, 0, &configCount);
  CheckError();

  if (!eglStatus || !configCount)
  {
    CLog::Log(LOGERROR, "EGL failed to return any matching configurations: {}", configCount);
    return false;
  }

  // Allocate room for the list of matching configurations
  configList = (EGLConfig*)malloc(configCount * sizeof(EGLConfig));
  if (!configList)
  {
    CLog::Log(LOGERROR, "EGL failure obtaining configuration list");
    return false;
  }

  // Obtain the configuration list from EGL
  eglStatus = eglChooseConfig(display, configAttrs, configList, configCount, &configCount);
  CheckError();
  if (!eglStatus || !configCount)
  {
    CLog::Log(LOGERROR, "EGL failed to populate configuration list: {}", eglStatus);
    return false;
  }

  // Select an EGL configuration that matches the native window
  *config = configList[0];

  free(configList);
  return true;
}

void COMXImage::CreateContext()
{
  EGLConfig egl_config;
  CWinSystemRpiGLESContext *winsystem = static_cast<CWinSystemRpiGLESContext *>(CServiceBroker::GetWinSystem());
  EGLDisplay egl_display = winsystem->GetEGLDisplay();

  eglInitialize(egl_display, NULL, NULL);
  CheckError();
  eglBindAPI(EGL_OPENGL_ES_API);
  CheckError();
  static const EGLint contextAttrs [] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  static const EGLint configAttrs [] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,     16,
        EGL_STENCIL_SIZE,    0,
        EGL_SAMPLE_BUFFERS,  0,
        EGL_SAMPLES,         0,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
  };
  bool s = ChooseConfig(egl_display, configAttrs, &egl_config);
  CheckError();
  if (!s)
  {
    CLog::Log(LOGERROR, "{}: Could not find a compatible configuration",__FUNCTION__);
    return;
  }
  m_egl_context = eglCreateContext(egl_display, egl_config, winsystem->GetEGLContext(), contextAttrs);
  CheckError();
  if (m_egl_context == EGL_NO_CONTEXT)
  {
    CLog::Log(LOGERROR, "{}: Could not create a context",__FUNCTION__);
    return;
  }
  EGLSurface egl_surface = eglCreatePbufferSurface(egl_display, egl_config, NULL);
  CheckError();
  if (egl_surface == EGL_NO_SURFACE)
  {
    CLog::Log(LOGERROR, "{}: Could not create a surface",__FUNCTION__);
    return;
  }
  s = eglMakeCurrent(egl_display, egl_surface, egl_surface, m_egl_context);
  CheckError();
  if (!s)
  {
    CLog::Log(LOGERROR, "{}: Could not make current",__FUNCTION__);
    return;
  }
}

void COMXImage::Process()
{
  while(!m_bStop)
  {
    std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
    if (m_texqueue.empty())
    {
      m_texqueue_cond.wait(lock);
    }
    else
    {
      struct callbackinfo *mess = m_texqueue.front();
      m_texqueue.pop();
      lock.unlock();

      CWinSystemRpiGLESContext *winsystem = static_cast<CWinSystemRpiGLESContext *>(CServiceBroker::GetWinSystem());
      mess->result = mess->callback(winsystem->GetEGLDisplay(), GetEGLContext(), mess->cookie);
      {
        std::unique_lock<CCriticalSection> lock(m_texqueue_lock);
        mess->sync.Set();
      }
    }
  }
}

void COMXImage::OnStartup()
{
}

void COMXImage::OnExit()
{
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImageFile"

COMXImageFile::COMXImageFile()
{
  m_image_size    = 0;
  m_image_buffer  = NULL;
  m_orientation   = 0;
  m_width         = 0;
  m_height        = 0;
}

COMXImageFile::~COMXImageFile()
{
  if(m_image_buffer)
    free(m_image_buffer);
}

typedef enum {      /* JPEG marker codes */
  M_SOF0  = 0xc0,
  M_SOF1  = 0xc1,
  M_SOF2  = 0xc2,
  M_SOF3  = 0xc3,
  M_SOF5  = 0xc5,
  M_SOF6  = 0xc6,
  M_SOF7  = 0xc7,
  M_JPG   = 0xc8,
  M_SOF9  = 0xc9,
  M_SOF10 = 0xca,
  M_SOF11 = 0xcb,
  M_SOF13 = 0xcd,
  M_SOF14 = 0xce,
  M_SOF15 = 0xcf,

  M_DHT   = 0xc4,
  M_DAC   = 0xcc,

  M_RST0  = 0xd0,
  M_RST1  = 0xd1,
  M_RST2  = 0xd2,
  M_RST3  = 0xd3,
  M_RST4  = 0xd4,
  M_RST5  = 0xd5,
  M_RST6  = 0xd6,
  M_RST7  = 0xd7,

  M_SOI   = 0xd8,
  M_EOI   = 0xd9,
  M_SOS   = 0xda,
  M_DQT   = 0xdb,
  M_DNL   = 0xdc,
  M_DRI   = 0xdd,
  M_DHP   = 0xde,
  M_EXP   = 0xdf,

  M_APP0  = 0xe0,
  M_APP1  = 0xe1,
  M_APP2  = 0xe2,
  M_APP3  = 0xe3,
  M_APP4  = 0xe4,
  M_APP5  = 0xe5,
  M_APP6  = 0xe6,
  M_APP7  = 0xe7,
  M_APP8  = 0xe8,
  M_APP9  = 0xe9,
  M_APP10 = 0xea,
  M_APP11 = 0xeb,
  M_APP12 = 0xec,
  M_APP13 = 0xed,
  M_APP14 = 0xee,
  M_APP15 = 0xef,
  // extensions
  M_JPG0  = 0xf0,
  M_JPG1  = 0xf1,
  M_JPG2  = 0xf2,
  M_JPG3  = 0xf3,
  M_JPG4  = 0xf4,
  M_JPG5  = 0xf5,
  M_JPG6  = 0xf6,
  M_JPG7  = 0xf7,
  M_JPG8  = 0xf8,
  M_JPG9  = 0xf9,
  M_JPG10 = 0xfa,
  M_JPG11 = 0xfb,
  M_JPG12 = 0xfc,
  M_JPG13 = 0xfd,
  M_JPG14 = 0xfe,
  M_COM   = 0xff,

  M_TEM   = 0x01,
} JPEG_MARKER;

static uint8_t inline READ8(uint8_t * &p)
{
  uint8_t r = p[0];
  p += 1;
  return r;
}

static uint16_t inline READ16(uint8_t * &p)
{
  uint16_t r = (p[0] << 8) | p[1];
  p += 2;
  return r;
}

static uint32_t inline READ32(uint8_t * &p)
{
  uint32_t r = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  p += 4;
  return r;
}

static void inline SKIPN(uint8_t * &p, unsigned int n)
{
  p += n;
}

bool COMXImageFile::GetCodingType(unsigned int &width, unsigned int &height, int orientation, std::string &error)
{
  bool valid = false;
  bool progressive = false;
  int components = 0;
  m_orientation   = 0;

  if(!m_image_size)
  {
    error = "m_image_size unexpected";
    return OMX_IMAGE_CodingMax;
  }

  uint8_t *p = m_image_buffer;
  uint8_t *q = m_image_buffer + m_image_size;

  /* JPEG Header */
  if(READ16(p) == 0xFFD8)
  {
    valid = true;
    READ8(p);
    unsigned char marker = READ8(p);
    unsigned short block_size = 0;
    bool nMarker = false;

    while(p < q && !progressive)
    {
      switch(marker)
      {
        case M_DQT:
        case M_DNL:
        case M_DHP:
        case M_EXP:

        case M_DHT:

        case M_SOF0:
        case M_SOF1:
        case M_SOF2:
        case M_SOF3:

        case M_SOF5:
        case M_SOF6:
        case M_SOF7:

        case M_JPG:
        case M_SOF9:
        case M_SOF10:
        case M_SOF11:

        case M_SOF13:
        case M_SOF14:
        case M_SOF15:

        case M_APP0:
        case M_APP1:
        case M_APP2:
        case M_APP3:
        case M_APP4:
        case M_APP5:
        case M_APP6:
        case M_APP7:
        case M_APP8:
        case M_APP9:
        case M_APP10:
        case M_APP11:
        case M_APP12:
        case M_APP13:
        case M_APP14:
        case M_APP15:

        case M_JPG0:
        case M_JPG1:
        case M_JPG2:
        case M_JPG3:
        case M_JPG4:
        case M_JPG5:
        case M_JPG6:
        case M_JPG7:
        case M_JPG8:
        case M_JPG9:
        case M_JPG10:
        case M_JPG11:
        case M_JPG12:
        case M_JPG13:
        case M_JPG14:
        case M_COM:
          block_size = READ16(p);
          nMarker = true;
          break;

        case M_SOS:
        default:
          nMarker = false;
          break;
      }

      if(!nMarker)
      {
        break;
      }

      if(marker >= M_SOF0 && marker <= M_SOF15 && marker != M_DHT && marker != M_DAC)
      {
        if(marker == M_SOF2 || marker == M_SOF6 || marker == M_SOF10 || marker == M_SOF14)
        {
          progressive = true;
        }
        int readBits = 2;
        SKIPN(p, 1);
        readBits ++;
        height = READ16(p);
        readBits += 2;
        width = READ16(p);
        readBits += 2;
        components = READ8(p);
        readBits += 1;
        SKIPN(p, 1 * (block_size - readBits));
      }
      else if(marker == M_APP1)
      {
        int readBits = 2;

        // Exif header
        if(READ32(p) == 0x45786966)
        {
          bool bMotorola = false;
          bool bError = false;
          SKIPN(p, 1 * 2);
          readBits += 2;

          char o1 = READ8(p);
          char o2 = READ8(p);
          readBits += 2;

          /* Discover byte order */
          if(o1 == 'M' && o2 == 'M')
            bMotorola = true;
          else if(o1 == 'I' && o2 == 'I')
            bMotorola = false;
          else
            bError = true;

          SKIPN(p, 1 * 2);
          readBits += 2;

          if(!bError)
          {
            unsigned int offset, a, b, numberOfTags, tagNumber;

            // Get first IFD offset (offset to IFD0)
            if(bMotorola)
            {
              SKIPN(p, 1 * 2);
              readBits += 2;

              a = READ8(p);
              b = READ8(p);
              readBits += 2;
              offset = (a << 8) + b;
            }
            else
            {
              a = READ8(p);
              b = READ8(p);
              readBits += 2;
              offset = (b << 8) + a;

              SKIPN(p, 1 * 2);
              readBits += 2;
            }

            offset -= 8;
            if(offset > 0)
            {
              SKIPN(p, 1 * offset);
              readBits += offset;
            }

            // Get the number of directory entries contained in this IFD
            if(bMotorola)
            {
              a = READ8(p);
              b = READ8(p);
              numberOfTags = (a << 8) + b;
            }
            else
            {
              a = READ8(p);
              b = READ8(p);
              numberOfTags = (b << 8) + a;
            }
            readBits += 2;

            while(numberOfTags && p < q)
            {
              // Get Tag number
              if(bMotorola)
              {
                a = READ8(p);
                b = READ8(p);
                tagNumber = (a << 8) + b;
                readBits += 2;
              }
              else
              {
                a = READ8(p);
                b = READ8(p);
                tagNumber = (b << 8) + a;
                readBits += 2;
              }

              //found orientation tag
              if(tagNumber == EXIF_TAG_ORIENTATION)
              {
                if(bMotorola)
                {
                  SKIPN(p, 1 * 7);
                  readBits += 7;
                  m_orientation = READ8(p)-1;
                  readBits += 1;
                  SKIPN(p, 1 * 2);
                  readBits += 2;
                }
                else
                {
                  SKIPN(p, 1 * 6);
                  readBits += 6;
                  m_orientation = READ8(p)-1;
                  readBits += 1;
                  SKIPN(p, 1 * 3);
                  readBits += 3;
                }
                break;
              }
              else
              {
                SKIPN(p, 1 * 10);
                readBits += 10;
              }
              numberOfTags--;
            }
          }
        }
        readBits += 4;
        SKIPN(p, 1 * (block_size - readBits));
      }
      else
      {
        SKIPN(p, 1 * (block_size - 2));
      }

      READ8(p);
      marker = READ8(p);

    }
  }
  else
    error = "unsupported image format";

  // apply input orientation
  m_orientation = m_orientation ^ orientation;
  if(m_orientation < 0 || m_orientation >= 8)
    m_orientation = 0;

  if(progressive)
  {
    error = "progressive images not supported by decoder";
    valid = false;
  }

  if(components > 3)
  {
    error = "only YUV images are supported by decoder";
    valid = false;
  }

  if(m_width < 1 || m_height < 1)
  {
    error = "invalid dimensions";
    valid = false;
  }

  return valid;
}


bool COMXImageFile::ReadFile(const std::string& inputFile, int orientation)
{
  XFILE::CFile      m_pFile;
  m_filename = CURL::GetRedacted(inputFile);
  if(!m_pFile.Open(inputFile, 0))
  {
    CLog::Log(LOGERROR, "{}::{} {} not found", CLASSNAME, __func__, GetFilename());
    return false;
  }

  if(m_image_buffer)
    free(m_image_buffer);
  m_image_buffer = NULL;

  m_image_size = m_pFile.GetLength();

  if(!m_image_size)
  {
    CLog::Log(LOGERROR, "{}::{} {} m_image_size zero", CLASSNAME, __func__, GetFilename());
    return false;
  }
  m_image_buffer = (uint8_t *)malloc(m_image_size);
  if(!m_image_buffer)
  {
    CLog::Log(LOGERROR, "{}::{} {} m_image_buffer null ({})", CLASSNAME, __func__, GetFilename(),
              m_image_size);  // **KM** {:u} -> {}
    return false;
  }

  m_pFile.Read(m_image_buffer, m_image_size);
  m_pFile.Close();

  std::string error;
  if (!GetCodingType(m_width, m_height, orientation, error))
  {
    CLog::Log(LOGINFO, "{}::{} {} {}x{} {}", CLASSNAME, __func__, GetFilename(), m_width, m_height, error.c_str());
    return false;
  }
  return true;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImageDec"

COMXImageDec::COMXImageDec()
{
  m_gpu = limit_calls_enter();
  m_decoded_buffer = NULL;
  OMX_INIT_STRUCTURE(m_decoded_format);
  m_success = false;
}

COMXImageDec::~COMXImageDec()
{
  Close();

  OMX_INIT_STRUCTURE(m_decoded_format);
  m_decoded_buffer = NULL;
  if (m_gpu)
    limit_calls_leave();
}

void COMXImageDec::Close()
{
  std::unique_lock<CCriticalSection> lock(m_OMXSection);

  if (!m_success)
  {
    if(m_omx_decoder.IsInitialized())
    {
      m_omx_decoder.SetStateForComponent(OMX_StateIdle);
      m_omx_decoder.FlushInput();
      m_omx_decoder.FreeInputBuffers();
    }
    if(m_omx_resize.IsInitialized())
    {
      m_omx_resize.SetStateForComponent(OMX_StateIdle);
      m_omx_resize.FlushOutput();
      m_omx_resize.FreeOutputBuffers();
    }
  }
  if(m_omx_tunnel_decode.IsInitialized())
    m_omx_tunnel_decode.Deestablish();
  if(m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if(m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
}

bool COMXImageDec::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height, unsigned int resize_stride)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  // on the first port settings changed event, we create the tunnel and alloc the buffer
  if (!m_decoded_buffer)
  {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.format.image.nSliceHeight = 16;
    m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_decode.Establish", CLASSNAME, __func__);
      return false;
    }
    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.WaitForEvent={:x}", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nStride = resize_stride;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    OMX_INIT_STRUCTURE(m_decoded_format);
    m_decoded_format.nPortIndex = m_omx_resize.GetOutputPort();
    omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &m_decoded_format);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.GetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }
    assert(m_decoded_format.nBufferCountActual == 1);

    omx_err = m_omx_resize.AllocOutputBuffers();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.AllocOutputBuffers result(0x{:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }
    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetStateForComponent result(0x{:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }

    m_decoded_buffer = m_omx_resize.GetOutputBuffer();

    if(!m_decoded_buffer)
    {
      CLog::Log(LOGERROR, "{}::{} no output buffer", CLASSNAME, __func__);
      return false;
    }

    omx_err = m_omx_resize.FillThisBuffer(m_decoded_buffer);
    if(omx_err != OMX_ErrorNone)
     {
       CLog::Log(LOGERROR, "{}::{} m_omx_resize FillThisBuffer result(0x{:x})", CLASSNAME, __func__,
                 omx_err);
       m_omx_resize.DecoderFillBufferDone(m_omx_resize.GetComponent(), m_decoded_buffer);
       return false;
    }
  }
  // on subsequent port settings changed event, we just copy the port settings
  else
  {
    // a little surprising, make a note
    CLog::Log(LOGDEBUG, "{}::{} m_omx_resize second port changed event", CLASSNAME, __func__);
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.DisablePort(m_omx_resize.GetInputPort(), true);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.WaitForEvent={:x}", CLASSNAME, __func__, omx_err);
      return false;
    }
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.EnablePort(m_omx_resize.GetInputPort(), true);
  }
  return true;
}

bool COMXImageDec::Decode(const uint8_t *demuxer_content, unsigned demuxer_bytes, unsigned width, unsigned height, unsigned stride, void *pixels)
{
  if (!m_gpu)
    return false;

  std::unique_lock<CCriticalSection> lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  if(!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "{}::{} no input buffer", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_decoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_resize.Initialize", CLASSNAME, __func__);
    return false;
  }

  // set input format
  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error GetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  portParam.nBufferCountActual = portParam.nBufferCountMin;
  portParam.nBufferSize = std::max(portParam.nBufferSize, ALIGN_UP(demuxer_bytes, portParam.nBufferAlignment));
  portParam.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error SetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_decoder.AllocInputBuffers result(0x{:x})", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_decoder.SetStateForComponent result(0x{:x})", CLASSNAME,
              __func__, omx_err);
    return false;
  }

  while(demuxer_bytes > 0 || !m_decoded_buffer)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
       omx_buffer = m_omx_decoder.GetInputBuffer(1000);
       if(omx_buffer == NULL)
         return false;

       omx_buffer->nOffset = omx_buffer->nFlags  = 0;

       omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
       memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

       demuxer_content += omx_buffer->nFilledLen;
       demuxer_bytes -= omx_buffer->nFilledLen;

       if(demuxer_bytes == 0)
         omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

       omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
       if (omx_err != OMX_ErrorNone)
       {
         CLog::Log(LOGERROR, "{}::{} OMX_EmptyThisBuffer() failed with result(0x{:x})", CLASSNAME,
                   __func__, omx_err);
         m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
         return false;
       }
    }
    if (!demuxer_bytes)
    {
       // we've submitted all buffers so can wait now
       timeout = 1000;
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, timeout);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(width, height, stride))
      {
        CLog::Log(LOGERROR, "{}::{} HandlePortSettingChange() failed", CLASSNAME, __func__);
        return false;
      }
    }
    else if(omx_err == OMX_ErrorStreamCorrupt)
    {
      CLog::Log(LOGERROR, "{}::{} - image not supported", CLASSNAME, __func__);
      return false;
    }
    else if(timeout || omx_err != OMX_ErrorTimeout)
    {
      CLog::Log(LOGERROR, "{}::{} WaitForEvent:OMX_EventPortSettingsChanged failed ({:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }
  }

  omx_err = m_omx_resize.WaitForOutputDone(1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_resize.WaitForOutputDone result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  if(m_omx_decoder.BadState())
    return false;

  memcpy( (char*)pixels, m_decoded_buffer->pBuffer, stride * height);

  m_success = true;
  Close();
  return true;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImageEnc"

COMXImageEnc::COMXImageEnc()
{
  m_gpu = limit_calls_enter();
  std::unique_lock<CCriticalSection> lock(m_OMXSection);
  OMX_INIT_STRUCTURE(m_encoded_format);
  m_encoded_buffer = NULL;
  m_success = false;
}

COMXImageEnc::~COMXImageEnc()
{
  std::unique_lock<CCriticalSection> lock(m_OMXSection);

  OMX_INIT_STRUCTURE(m_encoded_format);
  m_encoded_buffer = NULL;
  if (!m_success)
  {
    if(m_omx_encoder.IsInitialized())
    {
      m_omx_encoder.SetStateForComponent(OMX_StateIdle);
      m_omx_encoder.FlushAll();
      m_omx_encoder.FreeInputBuffers();
      m_omx_encoder.FreeOutputBuffers();
      m_omx_encoder.Deinitialize();
    }
  }
  if (m_gpu)
    limit_calls_leave();
}

bool COMXImageEnc::Encode(unsigned char *buffer, int size, unsigned width, unsigned height, unsigned int pitch)
{
  if (!m_gpu)
    return false;

  std::unique_lock<CCriticalSection> lock(m_OMXSection);

  unsigned int demuxer_bytes = 0;
  const uint8_t *demuxer_content = NULL;
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;
  OMX_INIT_STRUCTURE(m_encoded_format);

  if (pitch == 0)
     pitch = 4 * width;

  if (!buffer || !size)
  {
    CLog::Log(LOGERROR, "{}::{} error no buffer", CLASSNAME, __func__);
    return false;
  }

  if(!m_omx_encoder.Initialize("OMX.broadcom.image_encode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_encoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetInputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = pitch;
  port_def.format.image.nSliceHeight = (height+15) & ~15;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetOutputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
  port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = 0;
  port_def.format.image.nSliceHeight = 0;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
  OMX_INIT_STRUCTURE(qfactor);
  qfactor.nPortIndex = m_omx_encoder.GetOutputPort();
  qfactor.nQFactor = 16;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamQFactor, &qfactor);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter OMX_IndexParamQFactor result(0x{:x})",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.AllocInputBuffers result(0x{:x})", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocOutputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.AllocOutputBuffers result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  omx_err = m_omx_encoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetStateForComponent result(0x{:x})", CLASSNAME,
              __func__, omx_err);
    return false;
  }

  demuxer_content = buffer;
  demuxer_bytes   = height * pitch;

  if(!demuxer_bytes || !demuxer_content)
    return false;

  while(demuxer_bytes > 0)
  {
    omx_buffer = m_omx_encoder.GetInputBuffer(1000);
    if(omx_buffer == NULL)
    {
      return false;
    }

    omx_buffer->nOffset = omx_buffer->nFlags  = 0;

    omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
    memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

    demuxer_content += omx_buffer->nFilledLen;
    demuxer_bytes -= omx_buffer->nFilledLen;

    if(demuxer_bytes == 0)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

    omx_err = m_omx_encoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} OMX_EmptyThisBuffer() failed with result(0x{:x})", CLASSNAME,
                __func__, omx_err);
      m_omx_encoder.DecoderEmptyBufferDone(m_omx_encoder.GetComponent(), omx_buffer);
      break;
    }
  }

  m_encoded_buffer = m_omx_encoder.GetOutputBuffer();

  if(!m_encoded_buffer)
  {
    CLog::Log(LOGERROR, "{}::{} no output buffer", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_encoder.FillThisBuffer(m_encoded_buffer);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.FillThisBuffer result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    m_omx_encoder.DecoderFillBufferDone(m_omx_encoder.GetComponent(), m_encoded_buffer);
    return false;
  }
  omx_err = m_omx_encoder.WaitForOutputDone(2000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.WaitForOutputDone result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  m_encoded_format.nPortIndex = m_omx_encoder.GetOutputPort();
  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &m_encoded_format);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_encoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  if(m_omx_encoder.BadState())
    return false;

  return true;
}

bool COMXImageEnc::CreateThumbnailFromSurface(unsigned char* buffer, unsigned int width, unsigned int height,
    unsigned int format, unsigned int pitch, const std::string& destFile)
{
  if (!m_gpu)
    return false;

  if(format != XB_FMT_A8R8G8B8 || !buffer)
  {
    CLog::Log(LOGDEBUG, "{}::{} : {} failed format=0x{:x}", CLASSNAME, __func__, destFile.c_str(),
              format);
    return false;
  }

  if(!Encode(buffer, height * pitch, width, height, pitch))
  {
    CLog::Log(LOGDEBUG, "{}::{} : {} encode failed", CLASSNAME, __func__, destFile.c_str());
    return false;
  }

  XFILE::CFile file;
  if (file.OpenForWrite(destFile, true))
  {
    CLog::Log(LOGDEBUG, "{}::{} : {} width {} height {}", CLASSNAME, __func__, destFile.c_str(),
              width, height);

    file.Write(m_encoded_buffer->pBuffer, m_encoded_buffer->nFilledLen);
    file.Close();
    return true;
  }

  m_success = true;
  return false;
}

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXReEnc"

COMXImageReEnc::COMXImageReEnc()
{
  m_gpu = limit_calls_enter();
  m_encoded_buffer = NULL;
  m_pDestBuffer = NULL;
  m_nDestAllocSize = 0;
  m_success = false;
}

COMXImageReEnc::~COMXImageReEnc()
{
  Close();
  if (m_pDestBuffer)
    free (m_pDestBuffer);
  m_pDestBuffer = NULL;
  m_nDestAllocSize = 0;
  if (m_gpu)
    limit_calls_leave();
}

void COMXImageReEnc::Close()
{
  std::unique_lock<CCriticalSection> lock(m_OMXSection);

  if (!m_success)
  {
    if(m_omx_decoder.IsInitialized())
    {
      m_omx_decoder.SetStateForComponent(OMX_StateIdle);
      m_omx_decoder.FlushInput();
      m_omx_decoder.FreeInputBuffers();
    }
    if(m_omx_resize.IsInitialized())
    {
      m_omx_resize.SetStateForComponent(OMX_StateIdle);
    }
    if(m_omx_encoder.IsInitialized())
    {
      m_omx_encoder.SetStateForComponent(OMX_StateIdle);
      m_omx_encoder.FlushOutput();
      m_omx_encoder.FreeOutputBuffers();
    }
  }
  if(m_omx_tunnel_decode.IsInitialized())
    m_omx_tunnel_decode.Deestablish();
  if(m_omx_tunnel_resize.IsInitialized())
    m_omx_tunnel_resize.Deestablish();
  if(m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if(m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
  if(m_omx_encoder.IsInitialized())
    m_omx_encoder.Deinitialize();
}



bool COMXImageReEnc::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height, int orientation, bool port_settings_changed)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  // on the first port settings changed event, we create the tunnel and alloc the buffer
  if (!port_settings_changed)
  {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_decoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    if (resize_width != port_def.format.image.nFrameWidth || resize_height != port_def.format.image.nFrameHeight || (orientation & 4))
    {
      if(!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
      {
        CLog::Log(LOGERROR, "{}::{} error m_omx_resize.Initialize", CLASSNAME, __func__);
        return false;
      }
    }

    //! @todo jpeg decoder can decimate by factors of 2
    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    if (m_omx_resize.IsInitialized())
      port_def.format.image.nSliceHeight = 16;
    else
      port_def.format.image.nSliceHeight = (resize_height+15) & ~15;

    port_def.format.image.nStride = 0;

    m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_decoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    if (m_omx_resize.IsInitialized())
    {
      port_def.nPortIndex = m_omx_resize.GetInputPort();

      m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetParameter result(0x{:x})", CLASSNAME, __func__,
                  omx_err);
        return false;
      }

      port_def.nPortIndex = m_omx_resize.GetOutputPort();
      m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_resize.GetParameter result(0x{:x})", CLASSNAME, __func__,
                  omx_err);
        return false;
      }
      port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
      port_def.format.image.nFrameWidth = resize_width;
      port_def.format.image.nFrameHeight = resize_height;
      port_def.format.image.nSliceHeight = (resize_height+15) & ~15;
      port_def.format.image.nStride = 0;
      m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetParameter result(0x{:x})", CLASSNAME, __func__,
                  omx_err);
        return false;
      }
    }

    if(!m_omx_encoder.Initialize("OMX.broadcom.image_encode", OMX_IndexParamImageInit))
    {
      CLog::Log(LOGERROR, "{}::{} error m_omx_encoder.Initialize", CLASSNAME, __func__);
      return false;
    }

    port_def.nPortIndex = m_omx_encoder.GetInputPort();
    m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }
    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nSliceHeight = (resize_height+15) & ~15;
    port_def.format.image.nStride = 0;
    m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_encoder.GetOutputPort();
    omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
    port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nStride = 0;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
    OMX_INIT_STRUCTURE(qfactor);
    qfactor.nPortIndex = m_omx_encoder.GetOutputPort();
    qfactor.nQFactor = 16;

    omx_err = m_omx_encoder.SetParameter(OMX_IndexParamQFactor, &qfactor);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetParameter OMX_IndexParamQFactor result(0x{:x})",
                CLASSNAME, __func__, omx_err);
      return false;
    }

    if (orientation)
    {
      struct {
        // metadata, these two fields need to be together
        OMX_CONFIG_METADATAITEMTYPE metadata;
        char metadata_space[64];
      } item;
      OMX_INIT_STRUCTURE(item.metadata);

      item.metadata.nSize = sizeof(item);
      item.metadata.eScopeMode = OMX_MetadataScopePortLevel;
      item.metadata.nScopeSpecifier = m_omx_encoder.GetOutputPort();
      item.metadata.nMetadataItemIndex = 0;
      item.metadata.eSearchMode = OMX_MetadataSearchValueSizeByIndex;
      item.metadata.eKeyCharset = OMX_MetadataCharsetASCII;
      strcpy((char *)item.metadata.nKey, "IFD0.Orientation");
      item.metadata.nKeySizeUsed = strlen((char *)item.metadata.nKey);

      item.metadata.eValueCharset = OMX_MetadataCharsetASCII;
      item.metadata.sLanguageCountry = 0;
      item.metadata.nValueMaxSize = sizeof(item.metadata_space);
      sprintf((char *)item.metadata.nValue, "%d", orientation + 1);
      item.metadata.nValueSizeUsed = strlen((char *)item.metadata.nValue);

      omx_err = m_omx_encoder.SetParameter(OMX_IndexConfigMetadataItem, &item);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR,
                  "{}::{} m_omx_encoder.SetParameter:OMX_IndexConfigMetadataItem omx_err(0x{:08x})",
                  CLASSNAME, __func__, omx_err);
        return false;
      }
    }
    omx_err = m_omx_encoder.AllocOutputBuffers();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.AllocOutputBuffers result(0x{:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }

    if (m_omx_resize.IsInitialized())
    {
      m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

      omx_err = m_omx_tunnel_decode.Establish();
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_decode.Establish", CLASSNAME, __func__);
        return false;
      }

      m_omx_tunnel_resize.Initialize(&m_omx_resize, m_omx_resize.GetOutputPort(), &m_omx_encoder, m_omx_encoder.GetInputPort());

      omx_err = m_omx_tunnel_resize.Establish();
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_resize.Establish", CLASSNAME, __func__);
        return false;
      }

      omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetStateForComponent result(0x{:x})", CLASSNAME,
                  __func__, omx_err);
        return false;
      }
    }
    else
    {
      m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_encoder, m_omx_encoder.GetInputPort());

      omx_err = m_omx_tunnel_decode.Establish();
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_decode.Establish", CLASSNAME, __func__);
        return false;
      }
    }
    omx_err = m_omx_encoder.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_encoder.SetStateForComponent result(0x{:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }

    if(m_omx_encoder.BadState())
      return false;
  }
  // on subsequent port settings changed event, we just copy the port settings
  else
  {
    // a little surprising, make a note
    CLog::Log(LOGDEBUG, "{}::{} m_omx_resize second port changed event", CLASSNAME, __func__);
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    if (m_omx_resize.IsInitialized())
    {
      m_omx_resize.DisablePort(m_omx_resize.GetInputPort(), true);

      OMX_PARAM_PORTDEFINITIONTYPE port_def;
      OMX_INIT_STRUCTURE(port_def);

      port_def.nPortIndex = m_omx_decoder.GetOutputPort();
      m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
      port_def.nPortIndex = m_omx_resize.GetInputPort();
      m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

      omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_resize.WaitForEvent={:x}", CLASSNAME, __func__, omx_err);
        return false;
      }
      m_omx_resize.EnablePort(m_omx_resize.GetInputPort(), true);
    }
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
  }
  return true;
}

bool COMXImageReEnc::ReEncode(COMXImageFile &srcFile, unsigned int maxWidth, unsigned int maxHeight, void * &pDestBuffer, unsigned int &nDestSize)
{
  if (!m_gpu)
    return false;

  std::unique_lock<CCriticalSection> lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  COMXImage::ClampLimits(maxWidth, maxHeight, srcFile.GetWidth(), srcFile.GetHeight(), srcFile.GetOrientation() & 4);
  unsigned int demuxer_bytes = srcFile.GetImageSize();
  unsigned char *demuxer_content = (unsigned char *)srcFile.GetImageBuffer();
  // initial dest buffer size
  nDestSize = 0;

  if(!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "{}::{} {} no input buffer", CLASSNAME, __func__, srcFile.GetFilename());
    return false;
  }

  if(!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "{}::{} {} error m_omx_decoder.Initialize", CLASSNAME, __func__,
              srcFile.GetFilename());
    return false;
  }

  // set input format
  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} {} error GetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, srcFile.GetFilename(), omx_err);
    return false;
  }

  portParam.nBufferCountActual = portParam.nBufferCountMin;
  portParam.nBufferSize = std::max(portParam.nBufferSize, ALIGN_UP(demuxer_bytes, portParam.nBufferAlignment));
  portParam.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} {} error SetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, srcFile.GetFilename(), omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} {} m_omx_decoder.AllocInputBuffers result(0x{:x})", CLASSNAME, __func__, srcFile.GetFilename(), omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} {} m_omx_decoder.SetStateForComponent result(0x{:x})", CLASSNAME,
              __func__, srcFile.GetFilename(), omx_err);
    return false;
  }

  bool port_settings_changed = false, eos = false;
  while(demuxer_bytes > 0 || !port_settings_changed || !eos)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
       OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);
       if(omx_buffer)
       {
         omx_buffer->nOffset = omx_buffer->nFlags  = 0;

         omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
         memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

         demuxer_content += omx_buffer->nFilledLen;
         demuxer_bytes -= omx_buffer->nFilledLen;
         if(demuxer_bytes == 0)
           omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

         omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
         if (omx_err != OMX_ErrorNone)
         {
           CLog::Log(LOGERROR, "{}::{} {} OMX_EmptyThisBuffer() failed with result(0x{:x})",
                     CLASSNAME, __func__, srcFile.GetFilename(), omx_err);
           m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
           return false;
         }
      }
    }
    if (!demuxer_bytes)
    {
       // we've submitted all buffers so can wait now
       timeout = 1000;
    }

    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, timeout);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(maxWidth, maxHeight, srcFile.GetOrientation(), port_settings_changed))
      {
        CLog::Log(LOGERROR, "{}::{} {} HandlePortSettingChange() failed", srcFile.GetFilename(),
                  CLASSNAME, __func__);
        return false;
      }
      port_settings_changed = true;
    }
    else if(omx_err == OMX_ErrorStreamCorrupt)
    {
      CLog::Log(LOGERROR, "{}::{} {} - image not supported", CLASSNAME, __func__, srcFile.GetFilename());
      return false;
    }
    else if(timeout || omx_err != OMX_ErrorTimeout)
    {
      CLog::Log(LOGERROR, "{}::{} {} WaitForEvent:OMX_EventPortSettingsChanged failed ({:x})",
                CLASSNAME, __func__, srcFile.GetFilename(), omx_err);
      return false;
    }

    if (!m_encoded_buffer && port_settings_changed && demuxer_bytes == 0)
    {
      m_encoded_buffer = m_omx_encoder.GetOutputBuffer();
      omx_err = m_omx_encoder.FillThisBuffer(m_encoded_buffer);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} {} FillThisBuffer() failed ({:x})", CLASSNAME, __func__,
                  srcFile.GetFilename(), omx_err);
        m_omx_encoder.DecoderFillBufferDone(m_omx_encoder.GetComponent(), m_encoded_buffer);
        return false;
      }
    }
    if (m_encoded_buffer)
    {
      omx_err = m_omx_encoder.WaitForOutputDone(2000);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} {} m_omx_encoder.WaitForOutputDone result(0x{:x})", CLASSNAME,
                  __func__, srcFile.GetFilename(), omx_err);
        return false;
      }
      if (!m_encoded_buffer->nFilledLen)
      {
        CLog::Log(LOGERROR, "{}::{} {} m_omx_encoder.WaitForOutputDone no data", CLASSNAME,
                  __func__, srcFile.GetFilename());
        return false;
      }
      if (m_encoded_buffer->nFlags & OMX_BUFFERFLAG_EOS)
         eos = true;

      if (nDestSize + m_encoded_buffer->nFilledLen > m_nDestAllocSize)
      {
         while (nDestSize + m_encoded_buffer->nFilledLen > m_nDestAllocSize)
           m_nDestAllocSize = std::max(1024U*1024U, m_nDestAllocSize*2);
         m_pDestBuffer = realloc(m_pDestBuffer, m_nDestAllocSize);
      }
      memcpy((char *)m_pDestBuffer + nDestSize, m_encoded_buffer->pBuffer, m_encoded_buffer->nFilledLen);
      nDestSize += m_encoded_buffer->nFilledLen;
      m_encoded_buffer = NULL;
    }
  }

  if(m_omx_decoder.BadState())
    return false;

  pDestBuffer = m_pDestBuffer;
  CLog::Log(LOGDEBUG, "{}::{} : {} {}x{} -> {}x{}", CLASSNAME, __func__, srcFile.GetFilename(),
            srcFile.GetWidth(), srcFile.GetHeight(), maxWidth, maxHeight);

  m_success = true;
  Close();

  return true;
}


#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXTexture"

COMXTexture::COMXTexture()
{
  m_gpu = limit_calls_enter();
  m_success = false;
}

COMXTexture::~COMXTexture()
{
  Close();
  if (m_gpu)
    limit_calls_leave();
}

void COMXTexture::Close()
{
  std::unique_lock<CCriticalSection> lock(m_OMXSection);

  if (!m_success)
  {
    if(m_omx_decoder.IsInitialized())
    {
      m_omx_decoder.SetStateForComponent(OMX_StateIdle);
      m_omx_decoder.FlushInput();
      m_omx_decoder.FreeInputBuffers();
    }
    if(m_omx_egl_render.IsInitialized())
    {
      m_omx_egl_render.SetStateForComponent(OMX_StateIdle);
      m_omx_egl_render.FlushOutput();
      m_omx_egl_render.FreeOutputBuffers();
    }
  }
  if (m_omx_tunnel_decode.IsInitialized())
    m_omx_tunnel_decode.Deestablish();
  if (m_omx_tunnel_egl.IsInitialized())
    m_omx_tunnel_egl.Deestablish();
  // delete components
  if (m_omx_decoder.IsInitialized())
    m_omx_decoder.Deinitialize();
  if (m_omx_resize.IsInitialized())
    m_omx_resize.Deinitialize();
  if (m_omx_egl_render.IsInitialized())
    m_omx_egl_render.Deinitialize();
}

bool COMXTexture::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height, void *egl_image, bool port_settings_changed)
{
  CWinSystemRpiGLESContext *winsystem = static_cast<CWinSystemRpiGLESContext *>(CServiceBroker::GetWinSystem());
  EGLDisplay egl_display = winsystem->GetEGLDisplay();
  OMX_ERRORTYPE omx_err;

  if (port_settings_changed)
    CLog::Log(LOGERROR, "{}::{} Unexpected second port_settings_changed call", CLASSNAME, __func__);

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);

  port_def.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_decoder.GetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  //! @todo jpeg decoder can decimate by factors of 2
  port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  port_def.format.image.nSliceHeight = 16;
  port_def.format.image.nStride = 0;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_decoder.SetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }
  if (resize_width != port_def.format.image.nFrameWidth || resize_height != port_def.format.image.nFrameHeight)
  {
    if (!m_omx_resize.Initialize("OMX.broadcom.resize", OMX_IndexParamImageInit))
    {
      CLog::Log(LOGERROR, "{}::{} error m_omx_resize.Initialize", CLASSNAME, __func__);
      return false;
    }
  }
  if (m_omx_resize.IsInitialized())
  {
    port_def.nPortIndex = m_omx_resize.GetInputPort();

    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.GetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }

    port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nSliceHeight = 16;
    port_def.format.image.nStride = 0;
    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_resize.SetParameter result(0x{:x})", CLASSNAME, __func__,
                omx_err);
      return false;
    }
  }
  if (!m_omx_egl_render.Initialize("OMX.broadcom.egl_render", OMX_IndexParamVideoInit))
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.Initialize", CLASSNAME, __func__);
    return false;
  }

  port_def.nPortIndex = m_omx_egl_render.GetOutputPort();
  omx_err = m_omx_egl_render.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_egl_render.SetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }
  port_def.nBufferCountActual = 1;
  port_def.format.video.pNativeWindow = egl_display;

  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} m_omx_egl_render.SetParameter result(0x{:x})", CLASSNAME, __func__,
              omx_err);
    return false;
  }

  omx_err = m_omx_egl_render.UseEGLImage(&m_egl_buffer, m_omx_egl_render.GetOutputPort(), NULL, egl_image);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.UseEGLImage ({:x})", CLASSNAME, __func__, omx_err);
    return false;
  }
  if (m_omx_resize.IsInitialized())
  {
    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish();
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_decode.Establish ({:x})", CLASSNAME, __func__, omx_err);
      return false;
    }

    m_omx_tunnel_egl.Initialize(&m_omx_resize, m_omx_resize.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());

    omx_err = m_omx_tunnel_egl.Establish();
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_egl.Establish ({:x})", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.GetParameter ({:x})", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  else
  {
    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish();
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "{}::{} m_omx_tunnel_decode.Establish ({:x})", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.SetStateForComponent ({:x})", CLASSNAME, __func__, omx_err);
    return false;
  }

  return true;
}

bool COMXTexture::Decode(const uint8_t *demuxer_content, unsigned demuxer_bytes, unsigned int width, unsigned int height, void *egl_image)
{
  if (!m_gpu)
    return false;

  std::unique_lock<CCriticalSection> lock(m_OMXSection);
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  if (!demuxer_content || !demuxer_bytes)
  {
    CLog::Log(LOGERROR, "{}::{} no input buffer", CLASSNAME, __func__);
    return false;
  }

  if (!m_omx_decoder.Initialize("OMX.broadcom.image_decode", OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_decoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  // set input format
  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error GetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  portParam.nBufferCountActual = portParam.nBufferCountMin;
  portParam.nBufferSize = std::max(portParam.nBufferSize, ALIGN_UP(demuxer_bytes, portParam.nBufferAlignment));
  portParam.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error SetParameter:OMX_IndexParamPortDefinition omx_err(0x{:08x})",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} - Error alloc buffers  ({:x})", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "{}::{} error m_omx_sched.SetStateForComponent ({:x})", CLASSNAME, __func__, omx_err);
    return false;
  }

  bool port_settings_changed = false;
  bool eos = false;
  while(demuxer_bytes > 0 || !port_settings_changed || !eos)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);
      if (omx_buffer)
      {
        omx_buffer->nOffset = omx_buffer->nFlags  = 0;

        omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
        memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

        demuxer_content += omx_buffer->nFilledLen;
        demuxer_bytes -= omx_buffer->nFilledLen;

        if (demuxer_bytes == 0)
          omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

        omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
        if (omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "{}::{} - m_omx_decoder.OMX_EmptyThisBuffer ({:x})", CLASSNAME, __func__, omx_err);
          m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
          return false;
         }
      }
    }
    if (!demuxer_bytes)
    {
       // we've submitted all buffers so can wait now
       timeout = 1000;
    }

    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, timeout);
    if (omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(width, height, egl_image, port_settings_changed))
      {
        CLog::Log(LOGERROR, "{}::{} - HandlePortSettingChange failed ({:x})", CLASSNAME, __func__, omx_err);
        return false;
      }
      port_settings_changed = true;
    }
    else if (omx_err == OMX_ErrorStreamCorrupt)
    {
      CLog::Log(LOGERROR, "{}::{} - image not supported", CLASSNAME, __func__);
      return false;
    }
    else if (timeout || omx_err != OMX_ErrorTimeout)
    {
      CLog::Log(LOGERROR, "{}::{} WaitForEvent:OMX_EventPortSettingsChanged failed ({:x})", CLASSNAME,
                __func__, omx_err);
      return false;
    }

    if (port_settings_changed && m_egl_buffer && demuxer_bytes == 0 && !eos)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_egl_render.GetOutputBuffer();
      if (!omx_buffer)
      {
        CLog::Log(LOGERROR, "{}::{} GetOutputBuffer failed", CLASSNAME, __func__);
        return false;
      }
      if (omx_buffer != m_egl_buffer)
      {
        CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.GetOutputBuffer ({:p},{:p})", CLASSNAME,
                  __func__, static_cast<void*>(omx_buffer), static_cast<void*>(m_egl_buffer));
        return false;
      }

      omx_err = m_omx_egl_render.FillThisBuffer(m_egl_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} error m_omx_egl_render.FillThisBuffer ({:x})", CLASSNAME, __func__, omx_err);
        m_omx_egl_render.DecoderFillBufferDone(m_omx_egl_render.GetComponent(), m_egl_buffer);
        return false;
      }

      omx_err = m_omx_egl_render.WaitForOutputDone(2000);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "{}::{} m_omx_egl_render.WaitForOutputDone result(0x{:x})", CLASSNAME,
                  __func__, omx_err);
        return false;
      }
      eos = true;
    }
  }
  m_success = true;
  Close();
  return true;
}

COMXImage g_OMXImage;
