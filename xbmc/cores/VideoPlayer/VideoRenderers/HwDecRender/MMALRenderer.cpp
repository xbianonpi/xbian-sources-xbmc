/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util_params.h>

#include "Util.h"
#include "MMALRenderer.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "filesystem/File.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "cores/VideoPlayer/DVDCodecs/Video/MMALCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/MMALFFmpeg.h"
#include "xbmc/Application.h"
#include "platform/linux/RBP.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/Interface/Addon/TimingConstants.h"

extern "C" {
#include <libavutil/imgutils.h>
}

#define VERBOSE 0

using namespace MMAL;

#define CLASSNAME "CMMALBuffer"

CMMALBuffer::CMMALBuffer(int id) : CVideoBuffer(id)
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, static_cast<void*>(this));
}

CMMALBuffer::~CMMALBuffer()
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, static_cast<void*>(this));
}

void CMMALBuffer::Unref()
{
  if (mmal_buffer)
  {
    mmal_buffer_header_release(mmal_buffer);
    mmal_buffer = nullptr;
  }
}

void CMMALBuffer::Update()
{
  if (mmal_buffer)
  {
    CMMALYUVBuffer *yuv = dynamic_cast<CMMALYUVBuffer *>(this);
    if (yuv)
    {
      int size = 0;
      std::shared_ptr<CMMALPool> pool = std::dynamic_pointer_cast<CMMALPool>(m_pool);
      if (pool)
        size = pool->Size();
      mmal_buffer->alloc_size = size;
      mmal_buffer->length = size;
      CGPUMEM *gmem = yuv->GetMem();
      if (gmem)
        mmal_buffer->data = (uint8_t *)gmem->m_vc_handle;
    }
  }
}

void CMMALBuffer::SetVideoDeintMethod(std::string method)
{
  std::shared_ptr<CMMALPool> pool = std::dynamic_pointer_cast<CMMALPool>(m_pool);
  if (pool)
    pool->SetVideoDeintMethod(method);
}


#undef CLASSNAME
#define CLASSNAME "CMMALPool"


void CMMALPool::Return(int id)
{
  CSingleLock lock(m_critSection);

  m_all[id]->Unref();
  auto it = m_used.begin();
  while (it != m_used.end())
  {
    if (*it == id)
    {
      m_used.erase(it);
      break;
    }
    else
      ++it;
  }
  m_free.push_back(id);
  Prime();
}

CMMALPool::CMMALPool(const char *component_name, bool input, uint32_t num_buffers, uint32_t buffer_size, uint32_t encoding, MMALState state)
 :  m_mmal_format(encoding), m_state(state), m_input(input)
{
  CSingleLock lock(m_critSection);
  MMAL_STATUS_T status;

  status = mmal_component_create(component_name, &m_component);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to create component %s", CLASSNAME, __func__, component_name);

  MMAL_PORT_T *port = m_input ? m_component->input[0] : m_component->output[0];

  // set up initial decoded frame format - may change from this
  port->format->encoding = encoding;

  status = mmal_port_parameter_set_boolean(port, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, port->name, status, mmal_status_to_string(status));

  status = mmal_port_format_commit(port);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to commit format for %s (status=%x %s)", CLASSNAME, __func__, port->name, status, mmal_status_to_string(status));

  port->buffer_size = buffer_size;
  port->buffer_num = std::max(num_buffers, port->buffer_num_recommended);

  m_mmal_pool = mmal_port_pool_create(port, port->buffer_num, port->buffer_size);
  if (!m_mmal_pool)
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for port %s", CLASSNAME, __func__, port->name);
  else
    {
      CLog::Log(LOGDEBUG, "%s::%s Created pool %p of size %d x %d for port %s", CLASSNAME, __func__,
                static_cast<void*>(m_mmal_pool), num_buffers, buffer_size, port->name);
    }
}

CMMALPool::~CMMALPool()
{
  CSingleLock lock(m_critSection);
  MMAL_STATUS_T status;

  MMAL_PORT_T *port = m_input ? m_component->input[0] : m_component->output[0];
  CLog::Log(LOGDEBUG, "%s::%s Destroying pool %p for port %s", CLASSNAME, __func__,
            static_cast<void*>(m_mmal_pool), port->name);

  if (port && port->is_enabled)
  {
    status = mmal_port_disable(port);
    if (status != MMAL_SUCCESS)
       CLog::Log(LOGERROR, "%s::%s Failed to disable port %s (status=%x %s)", CLASSNAME, __func__, port->name, status, mmal_status_to_string(status));
  }

  if (m_component && m_component->is_enabled)
  {
    status = mmal_component_disable(m_component);
    if (status != MMAL_SUCCESS)
      CLog::Log(LOGERROR, "%s::%s Failed to disable component %s (status=%x %s)", CLASSNAME, __func__, m_component->name, status, mmal_status_to_string(status));
  }

  mmal_port_pool_destroy(port, m_mmal_pool);

  if (m_component)
    mmal_component_destroy(m_component);
  m_component = nullptr;

  m_mmal_pool = nullptr;
  for (auto buf : m_all)
  {
    delete buf;
  }
}

std::vector<CMMALPool::MMALEncodingTable> CMMALPool::mmal_encoding_table =
{
  { AV_PIX_FMT_YUV420P,  MMAL_ENCODING_I420 },
  { AV_PIX_FMT_YUVJ420P, MMAL_ENCODING_I420 },
  { AV_PIX_FMT_YUV420P10,MMAL_ENCODING_I420_16, },
  { AV_PIX_FMT_YUV420P12,MMAL_ENCODING_I420_16, },
  { AV_PIX_FMT_YUV420P14,MMAL_ENCODING_I420_16, },
  { AV_PIX_FMT_YUV420P16,MMAL_ENCODING_I420_16, },
  { AV_PIX_FMT_RGBA,     MMAL_ENCODING_RGBA, },
  { AV_PIX_FMT_BGRA,     MMAL_ENCODING_BGRA },
  { AV_PIX_FMT_RGB0,     MMAL_ENCODING_RGBA },
  { AV_PIX_FMT_BGR0,     MMAL_ENCODING_BGRA },
  { AV_PIX_FMT_RGB24,    MMAL_ENCODING_RGB24 },
  { AV_PIX_FMT_BGR24,    MMAL_ENCODING_BGR24 },
  { AV_PIX_FMT_RGB565,   MMAL_ENCODING_RGB16 },
  { AV_PIX_FMT_RGB565LE, MMAL_ENCODING_RGB16 },
  { AV_PIX_FMT_BGR565,   MMAL_ENCODING_BGR16 },
  { AV_PIX_FMT_NONE,     MMAL_ENCODING_UNKNOWN },
};


uint32_t CMMALPool::TranslateFormat(AVPixelFormat pixfmt)
{
  for (const auto& entry : mmal_encoding_table)
  {
    if (entry.pixfmt == pixfmt)
      return entry.encoding;
  }
  assert(0);
  return MMAL_ENCODING_UNKNOWN;
}

void CMMALPool::Configure(AVPixelFormat format, int width, int height, int alignedWidth, int alignedHeight, int size)
{
  CSingleLock lock(m_critSection);
  if (format != AV_PIX_FMT_NONE)
    m_mmal_format = TranslateFormat(format);
  m_width = width;
  m_height = height;
  m_size = size;
  m_software = true;
  m_configured = true;

  if (m_mmal_format != MMAL_ENCODING_UNKNOWN)
  {
    m_geo = g_RBP.GetFrameGeometry(m_mmal_format, alignedWidth, alignedHeight);
    if (m_mmal_format != MMAL_ENCODING_YUVUV128 && m_mmal_format != MMAL_ENCODING_YUVUV64_16 )
    {
      if (alignedWidth)
      {
        m_geo.setStrideY(alignedWidth * m_geo.getBytesPerPixel());
        m_geo.setStrideC(alignedWidth * m_geo.getBytesPerPixel() >> 1);
      }
      if (alignedHeight)
      {
        m_geo.setHeightY(alignedHeight);
        m_geo.setHeightC(alignedHeight >> 1);
      }
    }
  }
  if (m_size == 0)
    m_size = m_geo.getSize();
  CLog::Log(LOGDEBUG, "%s::%s pool:%p %dx%d (%dx%d) pix:%d size:%d fmt:%.4s", CLASSNAME, __func__,
            static_cast<void*>(m_mmal_pool), width, height, alignedWidth, alignedHeight, format, size,
            (char*)&m_mmal_format);
}

void CMMALPool::Configure(AVPixelFormat format, int size)
{
  Configure(format, 0, 0, 0, 0, size);
}

void CMMALPool::SetDimensions(int width, int height, const int (&strides)[YuvImage::MAX_PLANES], const int (&planeOffsets)[YuvImage::MAX_PLANES])
{
  assert(m_geo.getBytesPerPixel());
  int alignedWidth = strides[0] ? strides[0] / m_geo.getBytesPerPixel() : width;
  int alignedHeight = planeOffsets[1] ? planeOffsets[1] / strides[0] : height;
  Configure(AV_PIX_FMT_NONE, width, height, alignedWidth, alignedHeight, 0);
  // libwv side-by-side UV format
  if (planeOffsets[2] - planeOffsets[1] == strides[1] >> 1)
    m_mmal_format = MMAL_ENCODING_I420_S;
}

inline bool CMMALPool::IsConfigured()
{
  CSingleLock lock(m_critSection);
  return m_configured;
}

bool CMMALPool::IsCompatible(AVPixelFormat format, int size)
{
  CSingleLock lock(m_critSection);
  uint32_t mmal_format = TranslateFormat(format);
  if (m_mmal_format == MMAL_ENCODING_I420_S && mmal_format == MMAL_ENCODING_I420)
    return true;
  if (m_mmal_format == mmal_format &&
      m_size == size)
    return true;

  return false;
}

CVideoBuffer *CMMALPool::Get()
{
  return GetBuffer(500);
}

CMMALBuffer *CMMALPool::GetBuffer(uint32_t timeout)
{
  MMAL_BUFFER_HEADER_T *buffer = nullptr;
  CMMALBuffer *omvb = nullptr;
  int id = -1;
  bool newbuf = false;
  CGPUMEM *gmem = nullptr;

  if (m_mmal_pool && m_mmal_pool->queue)
    buffer = mmal_queue_timedwait(m_mmal_pool->queue, timeout);
  CSingleLock lock(m_critSection);
  if (buffer)
  {
    mmal_buffer_header_reset(buffer);
    buffer->cmd = 0;
    buffer->offset = 0;
    buffer->flags = 0;
    buffer->user_data = 0;

    if (!m_free.empty())
    {
      id = m_free.front();
      m_free.pop_front();
      m_used.push_back(id);
      omvb = m_all[id];
    }
    else
    {
      newbuf = true;
      id = m_all.size();
      if (!IsSoftware())
      {
        CMMALVideoBuffer *vid = new CMMALVideoBuffer(id);
        omvb = vid;
      }
      else
      {
        CMMALYUVBuffer *yuv = new CMMALYUVBuffer(id);
        if (yuv)
        {
          assert(m_size > 0);
          gmem = yuv->Allocate(m_size, (void *)yuv);
          if (!gmem)
          {
            delete yuv;
            yuv = nullptr;
          }
        }
        omvb = yuv;
      }
      if (omvb)
      {
        m_all.push_back(omvb);
        m_used.push_back(id);
      }
    }
    if (omvb)
    {
      omvb->Acquire(GetPtr());
      omvb->m_rendered = false;
      omvb->m_state = m_state;
      buffer->user_data = omvb;
      omvb->mmal_buffer = buffer;
      omvb->Update();
      assert(omvb->Pool() == GetPtr());
    }
  }
  if (timeout > 0 && !omvb)
    {
      CLog::Log(LOGERROR, "%s::%s - failed pool:%p omvb:%p mmal:%p timeout:%d", CLASSNAME,
                __FUNCTION__, static_cast<void*>(m_mmal_pool), static_cast<void*>(omvb),
                static_cast<void*>(buffer), timeout);
    }
  else if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG,
              "%s::%s pool:%p omvb:%p mmal:%p gmem:%p new:%d id:%d to:%d %dx%d (%dx%d) size:%d "
              "pool:%p:%p enc:%.4s",
              CLASSNAME, __FUNCTION__, static_cast<void*>(m_mmal_pool), static_cast<void*>(omvb),
              static_cast<void*>(buffer), static_cast<void*>(gmem), newbuf, id, timeout, m_width,
              m_height, AlignedWidth(), AlignedHeight(), buffer ? buffer->alloc_size : 0,
              omvb ? static_cast<void*>(omvb->Pool().get()) : nullptr, static_cast<void*>(GetPtr().get()),
              (char*)&m_mmal_format);
  }
  return omvb;
}

void CMMALPool::Prime()
{
  CSingleLock lock(m_critSection);
  CMMALBuffer *omvb;
  if (!m_mmal_pool || !m_component)
    return;
  MMAL_PORT_T *port = m_input ? m_component->input[0] : m_component->output[0];
  if (!port->is_enabled)
    return;
  while (omvb = GetBuffer(0), omvb)
  {
    if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    {
      CLog::Log(
          LOGDEBUG, "%s::%s Send omvb:%p mmal:%p from pool %p to %s len:%d cmd:%x flags:%x pool:%p",
          CLASSNAME, __func__, static_cast<void*>(omvb), static_cast<void*>(omvb->mmal_buffer),
          static_cast<void*>(m_mmal_pool), port->name, omvb->mmal_buffer->length,
          omvb->mmal_buffer->cmd, omvb->mmal_buffer->flags, static_cast<void*>(omvb->Pool().get()));
    }
    MMAL_STATUS_T status = mmal_port_send_buffer(port, omvb->mmal_buffer);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(
          LOGERROR, "%s::%s - Failed to send omvb:%p mmal:%p from pool %p to %s (status=0%x %s)",
          CLASSNAME, __func__, static_cast<void*>(omvb), static_cast<void*>(omvb->mmal_buffer),
          static_cast<void*>(m_mmal_pool), port->name, status, mmal_status_to_string(status));
    }
  }
}

void CMMALPool::SetVideoDeintMethod(std::string method)
{
  CSingleLock lock(m_critSection);
  if (m_processInfo)
    m_processInfo->SetVideoDeintMethod(method);
}

void CMMALPool::Released(CVideoBufferManager &videoBufferManager)
{
  /* Create dummy component with attached pool */
  std::shared_ptr<IVideoBufferPool> pool = std::make_shared<CMMALPool>(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, false, MMAL_NUM_OUTPUT_BUFFERS, 0, MMAL_ENCODING_UNKNOWN, MMALStateFFDec);
  videoBufferManager.RegisterPool(pool);
}

#undef CLASSNAME
#define CLASSNAME "CMMALRenderer"

CRenderInfo CMMALRenderer::GetRenderInfo()
{
  CSingleLock lock(m_sharedSection);
  CRenderInfo info;

  CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s opaque:%p", CLASSNAME, __func__, static_cast<void*>(this));

  info.max_buffer_size = NUM_BUFFERS;
  info.optimal_buffer_size = NUM_BUFFERS;
  info.opaque_pointer = (void *)this;

  return info;
}

void CMMALRenderer::vout_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "%s::%s omvb:%p mmal:%p dts:%.3f pts:%.3f len:%d cmd:%x flags:%x",
              CLASSNAME, __func__, static_cast<void*>(buffer->user_data),
              static_cast<void*>(buffer), buffer->dts * 1e-6, buffer->pts * 1e-6, buffer->length,
              buffer->cmd, buffer->flags);
  }
  buffer->length = 0;
  mmal_queue_put(m_queue_process, buffer);
}

static void vout_input_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALRenderer *mmal = reinterpret_cast<CMMALRenderer*>(port->userdata);
  mmal->vout_input_port_cb(port, buffer);
}

bool CMMALRenderer::CheckConfigurationVout(uint32_t width, uint32_t height, uint32_t aligned_width, uint32_t aligned_height, uint32_t encoding)
{
  MMAL_STATUS_T status;
  bool sizeChanged = width != m_vout_width || height != m_vout_height || aligned_width != m_vout_aligned_width || aligned_height != m_vout_aligned_height;
  bool encodingChanged = !m_vout_input || !m_vout_input->format || encoding != m_vout_input->format->encoding;

  if (!m_vout)
  {
    /* Create video renderer */
    CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s CreateRenderer", CLASSNAME, __func__);

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &m_vout);
    if(status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to create vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }

    m_vout_input = m_vout->input[0];

    status = mmal_port_parameter_set_boolean(m_vout_input, MMAL_PARAMETER_NO_IMAGE_PADDING, MMAL_TRUE);
    if (status != MMAL_SUCCESS)
      CLog::Log(LOGERROR, "%s::%s Failed to enable no image padding mode on %s (status=%x %s)", CLASSNAME, __func__, m_vout_input->name, status, mmal_status_to_string(status));

    status = mmal_port_parameter_set_boolean(m_vout_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    if (status != MMAL_SUCCESS)
       CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, m_vout_input->name, status, mmal_status_to_string(status));

    m_vout_input->format->type = MMAL_ES_TYPE_VIDEO;
    if (CONF_FLAGS_YUVCOEF_MASK(m_iFlags) == CONF_FLAGS_YUVCOEF_BT709)
      m_vout_input->format->es->video.color_space = MMAL_COLOR_SPACE_ITUR_BT709;
    else if (CONF_FLAGS_YUVCOEF_MASK(m_iFlags) == CONF_FLAGS_YUVCOEF_BT601)
      m_vout_input->format->es->video.color_space = MMAL_COLOR_SPACE_ITUR_BT601;
    else if (CONF_FLAGS_YUVCOEF_MASK(m_iFlags) == CONF_FLAGS_YUVCOEF_240M)
      m_vout_input->format->es->video.color_space = MMAL_COLOR_SPACE_SMPTE240M;
  }

  if (m_vout_input && (sizeChanged || encodingChanged))
  {
    assert(m_vout_input != nullptr && m_vout_input->format != nullptr && m_vout_input->format->es != nullptr);
    CLog::Log(LOGDEBUG, "%s::%s Changing Vout dimensions from %dx%d (%dx%d) to %dx%d (%dx%d) %.4s", CLASSNAME, __func__,
        m_vout_width, m_vout_height, m_vout_aligned_width, m_vout_aligned_height, width, height, aligned_width, aligned_height, (char *)&encoding);

    // we need to disable port when encoding changes, but not if just resolution changes
    if (encodingChanged && m_vout_input->is_enabled)
    {
      status = mmal_port_disable(m_vout_input);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to disable vout input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }

    m_vout_width = width;
    m_vout_height = height;
    m_vout_aligned_width = aligned_width;
    m_vout_aligned_height = aligned_height;

    m_vout_input->format->es->video.crop.width = width;
    m_vout_input->format->es->video.crop.height = height;
    m_vout_input->format->es->video.width = aligned_width;
    m_vout_input->format->es->video.height = aligned_height;
    m_vout_input->format->encoding = encoding;

    status = mmal_port_format_commit(m_vout_input);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to commit vout input format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }

    if (!m_vout_input->is_enabled)
    {
      m_vout_input->buffer_num = MMAL_NUM_OUTPUT_BUFFERS;
      m_vout_input->buffer_size = m_vout_input->buffer_size_recommended;
      m_vout_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;

      status = mmal_port_enable(m_vout_input, vout_input_port_cb_static);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to enable vout input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }
  }

  if (m_vout && !m_vout->is_enabled)
  {
    status = mmal_component_enable(m_vout);
    if(status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to enable vout component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }

    if (!m_queue_render && !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool("videoplayer.usedisplayasclock"))
    {
      m_queue_render = mmal_queue_create();
      CThread::Create();
    }
  }
  SetVideoRect(m_cachedSourceRect, m_cachedDestRect);
  return true;
}

CMMALRenderer::CMMALRenderer() : CThread("MMALRenderer"), m_processThread(this, "MMALProcess")
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  m_vout = NULL;
  m_vout_input = NULL;
  memset(m_buffers, 0, sizeof m_buffers);
  m_iFlags = 0;
  m_bConfigured = false;
  m_queue_render = nullptr;
  m_error = 0.0;
  m_fps = 0.0;
  m_lastPts = DVD_NOPTS_VALUE;
  m_frameInterval = 0.0;
  m_frameIntervalDiff = 1e5;
  m_vsync_count = ~0U;
  m_sharpness = -2.0f;
  m_vout_width = 0;
  m_vout_height = 0;
  m_vout_aligned_width = 0;
  m_vout_aligned_height = 0;
  m_deint = NULL;
  m_deint_input = NULL;
  m_deint_output = NULL;
  m_deint_width = 0;
  m_deint_height = 0;
  m_deint_aligned_width = 0;
  m_deint_aligned_height = 0;
  m_cachedSourceRect.SetRect(0, 0, 0, 0);
  m_cachedDestRect.SetRect(0, 0, 0, 0);
  m_isPi1 = g_RBP.RaspberryPiVersion() == 1;

  m_queue_process = mmal_queue_create();
  m_processThread.Create();
}

CMMALRenderer::~CMMALRenderer()
{
  CSingleLock lock(m_sharedSection);
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  UnInit();

  if (m_queue_process)
    mmal_queue_put(m_queue_process, &m_quitpacket);

  {
    // leave the lock to allow other threads to exit
    CSingleExit unlock(m_sharedSection);
    m_processThread.StopThread();
  }

  mmal_queue_destroy(m_queue_process);
  m_queue_process = nullptr;
}


void CMMALRenderer::Process()
{
  bool bStop = false;
  SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);
  CLog::Log(LOGDEBUG, "%s::%s - starting", CLASSNAME, __func__);
  while (!bStop)
  {
    double dfps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
    double fps = 0.0;
    double inc = 1.0;
    g_RBP.WaitVsync();

    CSingleLock lock(m_sharedSection);
    // if good enough framerate measure then use it
    if (dfps > 0.0 && m_frameInterval > 0.0 && m_frameIntervalDiff * 1e-6 < 1e-3)
    {
      fps = 1e6 / m_frameInterval;
      inc = fps / dfps;
      if (fabs(inc - 1.0) < 1e-2)
        inc = 1.0;
      else if (fabs(inc - 0.5) < 1e-2)
        inc = 0.5;
      else if (fabs(inc - 24.0/60.0) < 1e-2)
        inc = 24.0/60.0;
      if (m_deint)
        inc *= 2.0;
    }
    // This algorithm is basically making the decision according to Bresenham's line algorithm.  Imagine drawing a line where x-axis is display frames, and y-axis is video frames
    m_error += inc;
    CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s - debug vsync:%d queue:%d fps:%.2f/%.2f/%.2f inc:%f diff:%f", CLASSNAME, __func__, g_RBP.LastVsync(), mmal_queue_length(m_queue_render), fps, m_fps, dfps, inc, m_error);
    // we may need to discard frames if queue length gets too high or video frame rate is above display frame rate
    while (mmal_queue_length(m_queue_render) > 2 || (mmal_queue_length(m_queue_render) > 1 && m_error > 1.0))
    {
      if (m_error > 1.0)
        m_error -= 1.0;
      MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(m_queue_render);
      if (buffer == &m_quitpacket)
        bStop = true;
      else if (buffer)
      {
        CMMALBuffer *omvb = (CMMALBuffer *)buffer->user_data;
        assert(buffer == omvb->mmal_buffer);
        omvb->Release();
        CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s - discard omvb:%p mmal:%p vsync:%d queue:%d diff:%f",
                  CLASSNAME, __func__, static_cast<void*>(omvb), static_cast<void*>(buffer),
                  g_RBP.LastVsync(), mmal_queue_length(m_queue_render), m_error);
      }
    }
    // this is case where we would like to display a new frame
    if (m_error > 0.0)
    {
      m_error -= 1.0;
      MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(m_queue_render);
      if (buffer == &m_quitpacket)
        bStop = true;
      else if (buffer)
      {
        CMMALBuffer *omvb = (CMMALBuffer *)buffer->user_data;
        assert(buffer == omvb->mmal_buffer);
        CheckConfigurationVout(omvb->Width(), omvb->Height(), omvb->AlignedWidth(), omvb->AlignedHeight(), omvb->Encoding());
        MMAL_STATUS_T status = mmal_port_send_buffer(m_vout_input, buffer);
        if (status != MMAL_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s::%s - Failed to send omvb:%p mmal:%p to %s (status=0%x %s)",
                    CLASSNAME, __func__, static_cast<void*>(omvb), static_cast<void*>(buffer),
                    m_vout_input->name, status, mmal_status_to_string(status));
        }
      }
      CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s - omvb:%p mmal:%p vsync:%d queue:%d diff:%f", CLASSNAME,
                __func__, buffer ? static_cast<void*>(buffer->user_data) : nullptr,
                static_cast<void*>(buffer), g_RBP.LastVsync(), mmal_queue_length(m_queue_render),
                m_error);
    }
  }
  CLog::Log(LOGDEBUG, "%s::%s - stopping", CLASSNAME, __func__);
}

void CMMALRenderer::Run()
{
  CLog::Log(LOGDEBUG, "%s::%s - starting", CLASSNAME, __func__);
  while (1)
  {
    MMAL_BUFFER_HEADER_T *buffer = mmal_queue_wait(m_queue_process);
    assert(buffer);
    if (buffer == &m_quitpacket)
      break;
    CSingleLock lock(m_sharedSection);
    bool kept = false;

    CMMALBuffer *omvb = (CMMALBuffer *)buffer->user_data;
    if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    {
      CLog::Log(LOGDEBUG,
                "%s::%s %s omvb:%p mmal:%p dts:%.3f pts:%.3f len:%d cmd:%x flags:%x enc:%.4s",
                CLASSNAME, __func__, omvb ? omvb->GetStateName() : "", static_cast<void*>(omvb),
                static_cast<void*>(buffer), buffer->dts * 1e-6, buffer->pts * 1e-6, buffer->length,
                buffer->cmd, buffer->flags, omvb ? (char*)&omvb->Encoding() : "");
    }

    assert(omvb && buffer == omvb->mmal_buffer);
    assert(buffer->cmd == 0);
    assert(!(buffer->flags & (MMAL_BUFFER_HEADER_FLAG_EOS | MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED)));
    if (m_bConfigured)
    switch (omvb->m_state)
    {
    case MMALStateHWDec:
    case MMALStateFFDec:
    {
      if (buffer->length > 0)
      {
        int yuv16 = omvb->Encoding() == MMAL_ENCODING_I420_16 || omvb->Encoding() == MMAL_ENCODING_YUVUV64_16;
        EINTERLACEMETHOD last_interlace_method = m_interlace_method;
        EINTERLACEMETHOD interlace_method = m_videoSettings.m_InterlaceMethod;
        if (interlace_method == VS_INTERLACEMETHOD_AUTO)
        {
          interlace_method = VS_INTERLACEMETHOD_MMAL_ADVANCED;
          // avoid advanced deinterlace when using software decode and HD resolution
          if ((omvb->m_state == MMALStateFFDec || m_isPi1) && omvb->Width() * omvb->Height() > 720*576)
            interlace_method = VS_INTERLACEMETHOD_MMAL_BOB;
        }
        bool interlace = (omvb->mmal_buffer->flags & MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED) ? true:false;

        // advanced deinterlace requires 3 frames of context so disable when showing stills
        if (omvb->m_stills)
        {
          if (interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED)
             interlace_method = VS_INTERLACEMETHOD_MMAL_BOB;
           if (interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF)
             interlace_method = VS_INTERLACEMETHOD_MMAL_BOB_HALF;
        }

        // we don't keep up when running at 60fps in the background so switch to half rate
        if (!CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo())
        {
          if (interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED)
            interlace_method = VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF;
          if (interlace_method == VS_INTERLACEMETHOD_MMAL_BOB)
            interlace_method = VS_INTERLACEMETHOD_MMAL_BOB_HALF;
        }

        if (interlace_method == VS_INTERLACEMETHOD_NONE && !yuv16)
        {
          if (m_deint_input)
            DestroyDeinterlace();
        }

        if (yuv16)
          interlace_method = VS_INTERLACEMETHOD_NONE;

        if (yuv16 || (interlace_method != VS_INTERLACEMETHOD_NONE && (m_deint_input || interlace)))
          CheckConfigurationDeint(omvb->Width(), omvb->Height(), omvb->AlignedWidth(), omvb->AlignedHeight(), omvb->Encoding(), interlace_method, omvb->BitsPerPixel());

        if (!m_deint_input)
          m_interlace_method = VS_INTERLACEMETHOD_NONE;

        if (last_interlace_method == m_interlace_method)
          ;
        else if (m_interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED)
          omvb->SetVideoDeintMethod("adv(x2)");
        else if (m_interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF)
          omvb->SetVideoDeintMethod("adv(x1)");
        else if (m_interlace_method == VS_INTERLACEMETHOD_MMAL_BOB)
          omvb->SetVideoDeintMethod("bob(x2)");
        else if (m_interlace_method == VS_INTERLACEMETHOD_MMAL_BOB_HALF)
          omvb->SetVideoDeintMethod("bob(x1)");
        else
          omvb->SetVideoDeintMethod("none");

        if (m_deint_input)
        {
          MMAL_STATUS_T status = mmal_port_send_buffer(m_deint_input, omvb->mmal_buffer);
          if (status != MMAL_SUCCESS)
          {
            CLog::Log(LOGERROR, "%s::%s - Failed to send omvb:%p mmal:%p to %s (status=0%x %s)",
                      CLASSNAME, __func__, static_cast<void*>(omvb),
                      static_cast<void*>(omvb->mmal_buffer), m_deint_input->name, status,
                      mmal_status_to_string(status));
          }
          else
            kept = true;
        }
        else if (m_queue_render)
        {
          mmal_queue_put(m_queue_render, omvb->mmal_buffer);
          kept = true;
        }
        else
        {
          CheckConfigurationVout(omvb->Width(), omvb->Height(), omvb->AlignedWidth(), omvb->AlignedHeight(), omvb->Encoding());
          if (m_vout_input)
          {
            MMAL_STATUS_T status = mmal_port_send_buffer(m_vout_input, omvb->mmal_buffer);
            if (status != MMAL_SUCCESS)
            {
              CLog::Log(LOGERROR, "%s::%s - Failed to send omvb:%p mmal:%p to %s (status=0%x %s)",
                        CLASSNAME, __func__, static_cast<void*>(omvb),
                        static_cast<void*>(omvb->mmal_buffer), m_vout_input->name, status,
                        mmal_status_to_string(status));
            }
            else
              kept = true;
          }
        }
      }
      break;
    }
    case MMALStateDeint:
    {
      if (buffer->length > 0)
      {
        if (m_queue_render)
        {
          mmal_queue_put(m_queue_render, buffer);
          if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
            CLog::Log(LOGDEBUG, "%s::%s send %p to m_queue_render", CLASSNAME, __func__, static_cast<void*>(omvb));
          kept = true;
        }
        else
        {
          CheckConfigurationVout(omvb->Width(), omvb->Height(), omvb->AlignedWidth(), omvb->AlignedHeight(), omvb->Encoding());
          if (m_vout_input)
          {
            if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
              CLog::Log(LOGDEBUG, "%s::%s send %p to m_vout_input", CLASSNAME, __func__, static_cast<void*>(omvb));
            MMAL_STATUS_T status = mmal_port_send_buffer(m_vout_input, buffer);
            if (status != MMAL_SUCCESS)
            {
              CLog::Log(LOGERROR, "%s::%s - Failed to send omvb:%p mmal%:p to %s (status=0%x %s)",
                        CLASSNAME, __func__, static_cast<void*>(omvb), static_cast<void*>(buffer),
                        m_vout_input->name, status, mmal_status_to_string(status));
            }
            else
              kept = true;
          }
        }
      }
      break;
    }
    default: assert(0); break;
    }
    if (!kept)
    {
      if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
      {
        CLog::Log(
            LOGDEBUG,
            "%s::%s %s Not kept: omvb:%p mmal:%p dts:%.3f pts:%.3f len:%d cmd:%x flags:%x enc:%.4s",
            CLASSNAME, __func__, omvb ? omvb->GetStateName() : "", static_cast<void*>(omvb),
            static_cast<void*>(buffer), buffer->dts * 1e-6, buffer->pts * 1e-6, buffer->length,
            buffer->cmd, buffer->flags, omvb ? (char*)&omvb->Encoding() : "");
      }
      if (omvb)
        omvb->Release();
      else
      {
        mmal_buffer_header_reset(buffer);
        buffer->cmd = 0;
        mmal_buffer_header_release(buffer);
      }
    }
  }
  CLog::Log(LOGDEBUG, "%s::%s - stopping", CLASSNAME, __func__);
}

void CMMALRenderer::UpdateFramerateStats(double pts)
{
  double diff = 0.0;
  if (m_lastPts != DVD_NOPTS_VALUE && pts != DVD_NOPTS_VALUE && pts - m_lastPts > 0.0 && pts - m_lastPts < DVD_SEC_TO_TIME(1./20.0))
  {
    diff = pts - m_lastPts;
    if (m_frameInterval == 0.0)
      m_frameInterval = diff;
    else if (diff > 0.0)
    {
      m_frameIntervalDiff = m_frameIntervalDiff * 0.9 + 0.1 * fabs(m_frameInterval - diff);
      m_frameInterval = m_frameInterval * 0.9 + diff * 0.1;
    }
  }
  if (pts != DVD_NOPTS_VALUE)
    m_lastPts = pts;
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s pts:%.3f diff:%.3f m_frameInterval:%.6f m_frameIntervalDiff:%.6f", CLASSNAME, __func__, pts*1e-6, diff * 1e-6 , m_frameInterval * 1e-6, m_frameIntervalDiff *1e-6);
}

void CMMALRenderer::AddVideoPicture(const VideoPicture& pic, int id)
{
  CMMALBuffer *buffer = dynamic_cast<CMMALBuffer*>(pic.videoBuffer);
  assert(buffer);
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "%s::%s MMAL - %p (%p) %i", CLASSNAME, __func__, static_cast<void*>(buffer),
              static_cast<void*>(buffer->mmal_buffer), id);
  }

  assert(!m_buffers[id]);
  buffer->Acquire();
  m_buffers[id] = buffer;
  UpdateFramerateStats(pic.pts);
}

bool CMMALRenderer::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  CSingleLock lock(m_sharedSection);
  ReleaseBuffers();

  if (picture.videoBuffer)
    m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  m_fps = fps;
  m_error = 0.0;
  m_lastPts = DVD_NOPTS_VALUE;
  m_frameInterval = 0.0;
  m_frameIntervalDiff = 1e5;

  // cause SetVideoRect to trigger - needed after a hdmi mode change
  m_src_rect.SetRect(0, 0, 0, 0);
  m_dst_rect.SetRect(0, 0, 0, 0);

  CLog::Log(LOGDEBUG, "%s::%s - %dx%d->%dx%d@%.2f flags:%x format:%d orient:%d", CLASSNAME, __func__, picture.iWidth, picture.iHeight, picture.iDisplayWidth, picture.iDisplayHeight, fps, m_iFlags, m_format, orientation);

  // calculate the input frame aspect ratio
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  m_bConfigured = true;
  return true;
}

void CMMALRenderer::ReleaseBuffer(int id)
{
  CSingleLock lock(m_sharedSection);
  CMMALBuffer *omvb = m_buffers[id];
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "%s::%s - MMAL: source:%d omvb:%p mmal:%p", CLASSNAME, __func__, id,
              static_cast<void*>(omvb), omvb ? static_cast<void*>(omvb->mmal_buffer) : nullptr);
  }
  if (m_buffers[id])
    m_buffers[id]->Release();
  m_buffers[id] = nullptr;
}

bool CMMALRenderer::Flush(bool saveBuffers)
{
  CSingleLock lock(m_sharedSection);
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  if (m_vout_input)
    mmal_port_flush(m_vout_input);
  ReleaseBuffers();

  return false;
}

void CMMALRenderer::Update()
{
  CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s", CLASSNAME, __func__);
  if (!m_bConfigured) return;
  ManageRenderArea();
}

void CMMALRenderer::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  CSingleLock lock(m_sharedSection);
  CMMALBuffer *omvb = nullptr;

  if (!m_bConfigured)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s - not configured: clear:%d flags:%x alpha:%d source:%d", CLASSNAME, __func__, clear, flags, alpha, index);
    goto exit;
  }

  omvb = m_buffers[index];

  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoView() != RENDER_STEREO_VIEW_RIGHT)
  {
    ManageRenderArea();
    CRect view;
    CBaseRenderer::GetVideoRect(m_cachedSourceRect, m_cachedDestRect, view);
  }

  // we only want to upload frames once
  if (omvb && omvb->m_rendered)
  {
    CLog::Log(
        LOGDEBUG, LOGVIDEO,
        "%s::%s - MMAL: clear:%d flags:%x alpha:%d source:%d omvb:%p mmal:%p mflags:%x skipping",
        CLASSNAME, __func__, clear, flags, alpha, index, static_cast<void*>(omvb),
        static_cast<void*>(omvb->mmal_buffer), omvb->mmal_buffer->flags);
    SetVideoRect(m_cachedSourceRect, m_cachedDestRect);
    goto exit;
  }

  // if sharpness setting has changed, we should update it
  if (m_sharpness != m_videoSettings.m_Sharpness)
  {
    m_sharpness = m_videoSettings.m_Sharpness;
    char command[80], response[80];
    sprintf(command, "scaling_sharpness %d", ((int)(50.0f * (m_sharpness + 1.0f) + 0.5f)));
    vc_gencmd(response, sizeof response, command);
  }

  if (omvb && omvb->m_state == MMALStateBypass)
  {
    // dummy buffer from omxplayer
    if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    {
      CLog::Log(LOGDEBUG, "%s::%s - OMX: clear:%d flags:%x alpha:%d source:%d omvb:%p", CLASSNAME,
                __func__, clear, flags, alpha, index, static_cast<void*>(omvb));
    }
  }
  else if (omvb && omvb->mmal_buffer)
  {
    if (flags & RENDER_FLAG_TOP)
      omvb->mmal_buffer->flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED | MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
    else if (flags & RENDER_FLAG_BOT)
      omvb->mmal_buffer->flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
    CLog::Log(LOGDEBUG, LOGVIDEO,
              "%s::%s - MMAL: clear:%d flags:%x alpha:%d source:%d omvb:%p mmal:%p mflags:%x "
              "len:%d data:%p enc:%.4s",
              CLASSNAME, __func__, clear, flags, alpha, index, static_cast<void*>(omvb),
              static_cast<void*>(omvb->mmal_buffer), omvb->mmal_buffer->flags,
              omvb->mmal_buffer->length, static_cast<void*>(omvb->mmal_buffer->data),
              (char*)&omvb->Encoding());
    assert(omvb->mmal_buffer && omvb->mmal_buffer->data && omvb->mmal_buffer->length);
    omvb->Acquire();
    omvb->m_rendered = true;
    assert(omvb->mmal_buffer->user_data == omvb);
    mmal_queue_put(m_queue_process, omvb->mmal_buffer);
  }
  else
  {
    CLog::Log(
        LOGDEBUG,
        "%s::%s - MMAL: No buffer to update clear:%d flags:%x alpha:%d source:%d omvb:%p mmal:%p",
        CLASSNAME, __func__, clear, flags, alpha, index, static_cast<void*>(omvb),
        omvb ? static_cast<void*>(omvb->mmal_buffer) : nullptr);
  }

exit:
   lock.Leave();
   uint32_t v = g_RBP.WaitVsync(m_vsync_count);
   // allow a frame of slop
   if (m_vsync_count == ~0U || !(v == m_vsync_count))
   {
     CLog::Log(LOGDEBUG, "%s::%s - vsync %d (+%d)", CLASSNAME, __func__, m_vsync_count, v - m_vsync_count);
     m_vsync_count = v + 1;
   }
   else
     m_vsync_count++;
}

void CMMALRenderer::ReleaseBuffers()
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  for (int i=0; i<NUM_BUFFERS; i++)
    ReleaseBuffer(i);
}

void CMMALRenderer::UnInitMMAL()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  if (m_queue_render)
  {
    mmal_queue_put(m_queue_render, &m_quitpacket);
    {
      // leave the lock to allow other threads to exit
      CSingleExit unlock(m_sharedSection);
      StopThread(true);
    }
    mmal_queue_destroy(m_queue_render);
    m_queue_render = nullptr;
  }

  if (m_vout)
  {
    mmal_component_disable(m_vout);
  }

  if (m_vout_input)
  {
    mmal_port_flush(m_vout_input);
    mmal_port_disable(m_vout_input);
  }

  ReleaseBuffers();

  m_vout_input = NULL;

  if (m_vout)
  {
    mmal_component_release(m_vout);
    m_vout = NULL;
  }

  m_src_rect.SetRect(0, 0, 0, 0);
  m_dst_rect.SetRect(0, 0, 0, 0);
  m_video_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_display_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_StereoInvert = false;
  m_vout_width = 0;
  m_vout_height = 0;
  m_vout_aligned_width = 0;
  m_vout_aligned_height = 0;
}

void CMMALRenderer::UnInit()
{
  CSingleLock lock(m_sharedSection);
  m_bConfigured = false;
  DestroyDeinterlace();
  UnInitMMAL();
}

bool CMMALRenderer::RenderCapture(CRenderCapture* capture)
{
  if (!m_bConfigured)
    return false;

  CLog::Log(LOGDEBUG, "%s::%s - %p", CLASSNAME, __func__, static_cast<void*>(capture));

  capture->BeginRender();
  capture->EndRender();

  return true;
}

//********************************************************************************************************
// YV12 Texture creation, deletion, copying + clearing
//********************************************************************************************************

bool CMMALRenderer::Supports(ERENDERFEATURE feature)
{
  if (feature == RENDERFEATURE_STRETCH         ||
      feature == RENDERFEATURE_ZOOM            ||
      feature == RENDERFEATURE_ROTATION        ||
      feature == RENDERFEATURE_VERTICAL_SHIFT  ||
      feature == RENDERFEATURE_PIXEL_RATIO     ||
      feature == RENDERFEATURE_SHARPNESS)
    return true;

  return false;
}

bool CMMALRenderer::Supports(ESCALINGMETHOD method)
{
  return false;
}

void CMMALRenderer::SetVideoRect(const CRect& InSrcRect, const CRect& InDestRect)
{
  CSingleLock lock(m_sharedSection);

  if (!m_vout_input)
    return;

  CRect SrcRect = InSrcRect, DestRect = InDestRect;
  RENDER_STEREO_MODE video_stereo_mode = (m_iFlags & CONF_FLAGS_STEREO_MODE_SBS) ? RENDER_STEREO_MODE_SPLIT_VERTICAL :
                                         (m_iFlags & CONF_FLAGS_STEREO_MODE_TAB) ? RENDER_STEREO_MODE_SPLIT_HORIZONTAL : RENDER_STEREO_MODE_OFF;
  bool stereo_invert                   = (m_iFlags & CONF_FLAGS_STEREO_CADANCE_RIGHT_LEFT) ? true : false;
  RENDER_STEREO_MODE display_stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // ignore video stereo mode when 3D display mode is disabled
  if (display_stereo_mode == RENDER_STEREO_MODE_OFF)
    video_stereo_mode = RENDER_STEREO_MODE_OFF;

  // fix up transposed video
  if (m_renderOrientation == 90 || m_renderOrientation == 270)
  {
    float newWidth, newHeight;
    float aspectRatio = GetAspectRatio();
    // clamp width if too wide
    if (DestRect.Height() > DestRect.Width())
    {
      newWidth = DestRect.Width(); // clamp to the width of the old dest rect
      newHeight = newWidth * aspectRatio;
    }
    else // else clamp to height
    {
      newHeight = DestRect.Height(); // clamp to the height of the old dest rect
      newWidth = newHeight / aspectRatio;
    }

    // calculate the center point of the view and offsets
    float centerX = DestRect.x1 + DestRect.Width() * 0.5f;
    float centerY = DestRect.y1 + DestRect.Height() * 0.5f;
    float diffX = newWidth * 0.5f;
    float diffY = newHeight * 0.5f;

    DestRect.x1 = centerX - diffX;
    DestRect.x2 = centerX + diffX;
    DestRect.y1 = centerY - diffY;
    DestRect.y2 = centerY + diffY;
  }

  // check if destination rect or video view mode has changed
  if (!(m_dst_rect != DestRect) && !(m_src_rect != SrcRect) && m_video_stereo_mode == video_stereo_mode && m_display_stereo_mode == display_stereo_mode && m_StereoInvert == stereo_invert)
    return;

  CLog::Log(LOGDEBUG, "%s::%s %d,%d,%d,%d -> %d,%d,%d,%d (o:%d v:%d d:%d i:%d)", CLASSNAME, __func__,
      (int)SrcRect.x1, (int)SrcRect.y1, (int)SrcRect.x2, (int)SrcRect.y2,
      (int)DestRect.x1, (int)DestRect.y1, (int)DestRect.x2, (int)DestRect.y2,
      m_renderOrientation, video_stereo_mode, display_stereo_mode, stereo_invert);

  m_src_rect = SrcRect;
  m_dst_rect = DestRect;
  m_video_stereo_mode = video_stereo_mode;
  m_display_stereo_mode = display_stereo_mode;
  m_StereoInvert = stereo_invert;

  // might need to scale up m_dst_rect to display size as video decodes
  // to separate video plane that is at display size.
  RESOLUTION res = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution();
  CRect gui(0, 0, CDisplaySettings::GetInstance().GetResolutionInfo(res).iWidth, CDisplaySettings::GetInstance().GetResolutionInfo(res).iHeight);
  CRect display(0, 0, CDisplaySettings::GetInstance().GetResolutionInfo(res).iScreenWidth, CDisplaySettings::GetInstance().GetResolutionInfo(res).iScreenHeight);

  if (display_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL)
  {
    float width = DestRect.x2 - DestRect.x1;
    DestRect.x1 *= 2.0f;
    DestRect.x2 = DestRect.x1 + 2.0f * width;
  }
  else if (display_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL)
  {
    float height = DestRect.y2 - DestRect.y1;
    DestRect.y1 *= 2.0f;
    DestRect.y2 = DestRect.y1 + 2.0f * height;
  }

  if (gui != display)
  {
    float xscale = display.Width()  / gui.Width();
    float yscale = display.Height() / gui.Height();
    DestRect.x1 *= xscale;
    DestRect.x2 *= xscale;
    DestRect.y1 *= yscale;
    DestRect.y2 *= yscale;
  }

  MMAL_DISPLAYREGION_T region;
  memset(&region, 0, sizeof region);

  region.set                 = MMAL_DISPLAY_SET_DEST_RECT|MMAL_DISPLAY_SET_SRC_RECT|MMAL_DISPLAY_SET_FULLSCREEN|MMAL_DISPLAY_SET_NOASPECT|MMAL_DISPLAY_SET_MODE|MMAL_DISPLAY_SET_TRANSFORM;
  region.dest_rect.x         = lrintf(DestRect.x1);
  region.dest_rect.y         = lrintf(DestRect.y1);
  region.dest_rect.width     = lrintf(DestRect.Width());
  region.dest_rect.height    = lrintf(DestRect.Height());

  region.src_rect.x          = lrintf(SrcRect.x1);
  region.src_rect.y          = lrintf(SrcRect.y1);
  region.src_rect.width      = lrintf(SrcRect.Width());
  region.src_rect.height     = lrintf(SrcRect.Height());

  region.fullscreen = MMAL_FALSE;
  region.noaspect = MMAL_TRUE;
  region.mode = MMAL_DISPLAY_MODE_LETTERBOX;

  if (m_renderOrientation == 90)
    region.transform = MMAL_DISPLAY_ROT90;
  else if (m_renderOrientation == 180)
    region.transform = MMAL_DISPLAY_ROT180;
  else if (m_renderOrientation == 270)
    region.transform = MMAL_DISPLAY_ROT270;
  else
    region.transform = MMAL_DISPLAY_ROT0;

  if (m_video_stereo_mode == RENDER_STEREO_MODE_SPLIT_HORIZONTAL)
    region.transform = (MMAL_DISPLAYTRANSFORM_T)(region.transform | DISPMANX_STEREOSCOPIC_TB);
  else if (m_video_stereo_mode == RENDER_STEREO_MODE_SPLIT_VERTICAL)
    region.transform = (MMAL_DISPLAYTRANSFORM_T)(region.transform | DISPMANX_STEREOSCOPIC_SBS);
  else
    region.transform = (MMAL_DISPLAYTRANSFORM_T)(region.transform | DISPMANX_STEREOSCOPIC_MONO);

  if (m_StereoInvert)
    region.transform = (MMAL_DISPLAYTRANSFORM_T)(region.transform | DISPMANX_STEREOSCOPIC_INVERT);

  MMAL_STATUS_T status = mmal_util_set_display_region(m_vout_input, &region);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to set display region (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));

  CLog::Log(LOGDEBUG, "%s::%s %d,%d,%d,%d -> %d,%d,%d,%d t:%x", CLASSNAME, __func__,
      region.src_rect.x, region.src_rect.y, region.src_rect.width, region.src_rect.height,
      region.dest_rect.x, region.dest_rect.y, region.dest_rect.width, region.dest_rect.height, region.transform);
}

void CMMALRenderer::deint_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "%s::%s omvb:%p mmal:%p dts:%.3f pts:%.3f len:%d cmd:%x flags:%x",
              CLASSNAME, __func__, static_cast<void*>(buffer->user_data),
              static_cast<void*>(buffer), buffer->dts * 1e-6, buffer->pts * 1e-6, buffer->length,
              buffer->cmd, buffer->flags);
  }
  mmal_queue_put(m_queue_process, buffer);
}

static void deint_input_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALRenderer *mmal = reinterpret_cast<CMMALRenderer*>(port->userdata);
  mmal->deint_input_port_cb(port, buffer);
}

void CMMALRenderer::deint_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  if (VERBOSE && CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "%s::%s omvb:%p mmal:%p dts:%.3f pts:%.3f len:%d cmd:%x flags:%x",
              CLASSNAME, __func__, static_cast<void*>(buffer->user_data),
              static_cast<void*>(buffer), buffer->dts * 1e-6, buffer->pts * 1e-6, buffer->length,
              buffer->cmd, buffer->flags);
  }
  mmal_queue_put(m_queue_process, buffer);
}

static void deint_output_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALRenderer *mmal = reinterpret_cast<CMMALRenderer*>(port->userdata);
  mmal->deint_output_port_cb(port, buffer);
}

void CMMALRenderer::DestroyDeinterlace()
{
  MMAL_STATUS_T status;

  CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s", CLASSNAME, __func__);

  // (lazily) destroy pool first so new buffers aren't allocated when flushing
  m_deint_output_pool = nullptr;

  if (m_deint_input && m_deint_input->is_enabled)
  {
    status = mmal_port_disable(m_deint_input);
    if (status != MMAL_SUCCESS)
      CLog::Log(LOGERROR, "%s::%s Failed to disable deinterlace input port(status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
  }
  m_deint_input = nullptr;
  if (m_deint_output && m_deint_output->is_enabled)
  {
    status = mmal_port_disable(m_deint_output);
    if (status != MMAL_SUCCESS)
      CLog::Log(LOGERROR, "%s::%s Failed to disable deinterlace output port(status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
  }
  m_deint_output = nullptr;
  m_interlace_method = VS_INTERLACEMETHOD_MAX;
  m_deint_width = 0;
  m_deint_height = 0;
  m_deint_aligned_width = 0;
  m_deint_aligned_height = 0;
  m_deint = nullptr;
}

bool CMMALRenderer::CheckConfigurationDeint(uint32_t width, uint32_t height, uint32_t aligned_width, uint32_t aligned_height, uint32_t encoding, EINTERLACEMETHOD interlace_method, int bitsPerPixel)
{
  MMAL_STATUS_T status;
  bool sizeChanged = width != m_deint_width || height != m_deint_height || aligned_width != m_deint_aligned_width || aligned_height != m_deint_aligned_height;
  bool deinterlaceChanged = interlace_method != m_interlace_method;
  bool encodingChanged = !m_deint_input || !m_deint_input->format || m_deint_input->format->encoding != encoding;
  bool advanced_deinterlace = interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED || interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF;
  bool half_framerate = interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF || interlace_method == VS_INTERLACEMETHOD_MMAL_BOB_HALF;
  uint32_t output_encoding = advanced_deinterlace ? MMAL_ENCODING_YUVUV128 : MMAL_ENCODING_I420;
  const char *component = interlace_method == VS_INTERLACEMETHOD_NONE ? "vc.ril.isp" : "vc.ril.image_fx";

  if (!m_bConfigured)
  {
    CLog::Log(LOGDEBUG, "%s::%s Unconfigured", CLASSNAME, __func__);
    return false;
  }

  if (!m_deint)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "%s::%s CreateDeinterlace", CLASSNAME, __func__);

    /* Create deinterlace component with attached pool */
    m_deint_output_pool = std::make_shared<CMMALPool>(component, false, 3, 0, output_encoding, MMALStateDeint);
    if (!m_deint_output_pool)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to create pool for deint output", CLASSNAME, __func__);
      return false;
    }

    m_deint = m_deint_output_pool->GetComponent();
    m_deint_output = m_deint->output[0];
    m_deint_input = m_deint->input[0];

    status = mmal_port_parameter_set_boolean(m_deint_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    if (status != MMAL_SUCCESS)
      CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, m_deint_input->name, status, mmal_status_to_string(status));
  }

  if (m_deint_input && (sizeChanged || deinterlaceChanged || encodingChanged))
  {
    assert(m_deint_input != nullptr && m_deint_input->format != nullptr && m_deint_input->format->es != nullptr);
    CLog::Log(LOGDEBUG, "%s::%s Changing Deint dimensions from %dx%d (%dx%d) to %dx%d (%dx%d) %.4s->%.4s mode %d->%d bpp:%d", CLASSNAME, __func__,
        m_deint_input->format->es->video.crop.width, m_deint_input->format->es->video.crop.height,
        m_deint_input->format->es->video.width, m_deint_input->format->es->video.height, width, height, aligned_width, aligned_height,
        (char *)&m_deint_input->format->encoding, (char *)&encoding, m_interlace_method, interlace_method, bitsPerPixel);

    // we need to disable port when parameters change
    if (m_deint_input && m_deint_input->is_enabled)
    {
      status = mmal_port_disable(m_deint_input);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to disable deint input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }
  }

  if (m_deint_output && (sizeChanged || deinterlaceChanged || encodingChanged))
  {
    if (m_deint_output && m_deint_output->is_enabled)
    {
      status = mmal_port_disable(m_deint_output);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to disable deint output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }
  }

  if (m_deint_input && (sizeChanged || deinterlaceChanged || encodingChanged))
  {
    m_deint_width = width;
    m_deint_height = height;
    m_deint_aligned_width = aligned_width;
    m_deint_aligned_height = aligned_height;

    m_deint_input->format->es->video.crop.width = width;
    m_deint_input->format->es->video.crop.height = height;
    m_deint_input->format->es->video.width = aligned_width;
    m_deint_input->format->es->video.height = aligned_height;
    m_deint_input->format->encoding = encoding;

    status = mmal_port_format_commit(m_deint_input);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to commit deint input format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }

    if (!m_deint_input->is_enabled)
    {
      m_deint_input->buffer_num = MMAL_NUM_OUTPUT_BUFFERS;
      m_deint_input->buffer_size = m_deint_input->buffer_size_recommended;
      m_deint_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;

      status = mmal_port_enable(m_deint_input, deint_input_port_cb_static);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to enable deint input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }
  }


  if (m_deint_output && (sizeChanged || deinterlaceChanged || encodingChanged))
  {
    if (interlace_method != VS_INTERLACEMETHOD_NONE)
    {
      MMAL_PARAMETER_IMAGEFX_PARAMETERS_T imfx_param = {{MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS, sizeof(imfx_param)},
            advanced_deinterlace ? MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV : MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST, 4, {5, 0, half_framerate, 1 }};

      status = mmal_port_parameter_set(m_deint_output, &imfx_param.hdr);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to set deinterlace parameters (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }

      // Image_fx assumed 3 frames of context. simple deinterlace doesn't require this
      status = mmal_port_parameter_set_uint32(m_deint_input, MMAL_PARAMETER_EXTRA_BUFFERS, 6 - 5 + advanced_deinterlace ? 2:0);
      if (status != MMAL_SUCCESS)
        CLog::Log(LOGERROR, "%s::%s Failed to enable extra buffers on %s (status=%x %s)", CLASSNAME, __func__, m_deint_input->name, status, mmal_status_to_string(status));
    }
    else
    {
      // We need to scale the YUV to 16-bit
      status = mmal_port_parameter_set_int32(m_deint_input, MMAL_PARAMETER_CCM_SHIFT, 16-bitsPerPixel-1);
      if (status != MMAL_SUCCESS)
        CLog::Log(LOGERROR, "%s::%s Failed to configure MMAL_PARAMETER_CCM_SHIFT on %s (status=%x %s)", CLASSNAME, __func__, m_deint_input->name, status, mmal_status_to_string(status));
      status = mmal_port_parameter_set_uint32(m_deint_output, MMAL_PARAMETER_OUTPUT_SHIFT, 1);
      if (status != MMAL_SUCCESS)
        CLog::Log(LOGERROR, "%s::%s Failed to configure MMAL_PARAMETER_OUTPUT_SHIFT on %s (status=%x %s)", CLASSNAME, __func__, m_deint_output->name, status, mmal_status_to_string(status));
    }
  }

  if (m_deint_output && (sizeChanged || deinterlaceChanged || encodingChanged))
  {
    m_deint_output->format->es->video.crop.width = width;
    m_deint_output->format->es->video.crop.height = height;
    m_deint_output->format->es->video.width = ALIGN_UP(width, 32);
    m_deint_output->format->es->video.height = ALIGN_UP(height, 16);
    m_deint_output->format->encoding = output_encoding;

    status = mmal_port_format_commit(m_deint_output);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to commit deint output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
      return false;
    }

    if (!m_deint_output->is_enabled)
    {
      m_deint_output->buffer_num = 3;
      m_deint_output->buffer_size = m_deint_output->buffer_size_recommended;
      m_deint_output->userdata = (struct MMAL_PORT_USERDATA_T *)this;

      status = mmal_port_enable(m_deint_output, deint_output_port_cb_static);
      if (status != MMAL_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s::%s Failed to enable deint output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
        return false;
      }
    }
    if (m_deint_output_pool)
      m_deint_output_pool->Configure(AV_PIX_FMT_NONE,
        m_deint_output->format->es->video.crop.width, m_deint_output->format->es->video.crop.height,
        m_deint_output->format->es->video.width, m_deint_output->format->es->video.height, m_deint_output->buffer_size);
  }

  if (m_deint && !m_deint->is_enabled)
  {
    status = mmal_component_enable(m_deint);
    if (status != MMAL_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s::%s Failed to enable deinterlacer component %s (status=%x %s)", CLASSNAME, __func__, m_deint->name, status, mmal_status_to_string(status));
      return false;
    }
  }
  m_interlace_method = interlace_method;

  // give buffers to deint
  if (m_deint_output_pool)
    m_deint_output_pool->Prime();
  return true;
}

CBaseRenderer* CMMALRenderer::Create(CVideoBuffer *buffer)
{
  return new CMMALRenderer();
}

bool CMMALRenderer::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("mmal", CMMALRenderer::Create);
  return true;
}
