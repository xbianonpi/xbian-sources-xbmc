/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "../BaseRenderer.h"
#include "../RenderCapture.h"
#include "../RenderFlags.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoSettings.h"
#include "threads/IRunnable.h"
#include "threads/Thread.h"
#include "utils/Geometry.h"
#include "windowing/GraphicContext.h"

#include <vector>

#include <interface/mmal/mmal.h>

// worst case number of buffers. 12 for decoder. 8 for multi-threading in ffmpeg. NUM_BUFFERS for renderer.
// Note, generally these won't necessarily result in allocated pictures
#define MMAL_NUM_OUTPUT_BUFFERS (12 + 8 + NUM_BUFFERS)

struct VideoPicture;
class CProcessInfo;

namespace MMAL {

class CMMALBuffer;

enum MMALState { MMALStateNone, MMALStateHWDec, MMALStateFFDec, MMALStateDeint, MMALStateBypass, };

class CMMALPool : public IVideoBufferPool
{
public:
  CMMALPool(const char *component_name, bool input, uint32_t num_buffers, uint32_t buffer_size, uint32_t encoding, MMALState state);
  ~CMMALPool();

  virtual CVideoBuffer* Get() override;
  virtual void Return(int id) override;
  virtual void Configure(AVPixelFormat format, int size) override;
  virtual bool IsConfigured() override;
  virtual bool IsCompatible(AVPixelFormat format, int size) override;

  void SetDimensions(int width, int height, const int (&strides)[YuvImage::MAX_PLANES], const int (&planeOffsets)[YuvImage::MAX_PLANES]);
  MMAL_COMPONENT_T *GetComponent() { return m_component; }
  CMMALBuffer *GetBuffer(uint32_t timeout);
  void Prime();
  void SetProcessInfo(CProcessInfo *processInfo) { m_processInfo = processInfo; }
  void Configure(AVPixelFormat format, int width, int height, int alignedWidth, int alignedHeight, int size);
  bool IsSoftware() { return m_software; }
  void SetVideoDeintMethod(std::string method);
  static uint32_t TranslateFormat(AVPixelFormat pixfmt);
  virtual int Width() { return m_width; }
  virtual int Height() { return m_height; }
  virtual int AlignedWidth() { return m_mmal_format == MMAL_ENCODING_YUVUV128 || m_mmal_format == MMAL_ENCODING_YUVUV64_16 || m_geo.getBytesPerPixel() == 0 ? 0 : m_geo.getStrideY() / m_geo.getBytesPerPixel(); }
  virtual int AlignedHeight() { return m_mmal_format == MMAL_ENCODING_YUVUV128 || m_mmal_format == MMAL_ENCODING_YUVUV64_16 ? 0 : m_geo.getHeightY(); }
  virtual int BitsPerPixel() { return m_geo.getBitsPerPixel(); }
  virtual uint32_t &Encoding() { return m_mmal_format; }
  virtual int Size() { return m_size; }
  AVRpiZcFrameGeometry &GetGeometry() { return m_geo; }
  virtual void Released(CVideoBufferManager &videoBufferManager);

protected:
  int m_width = 0;
  int m_height = 0;
  bool m_configured = false;
  CCriticalSection m_critSection;

  std::vector<CMMALBuffer*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;

  int m_size = 0;
  uint32_t m_mmal_format = 0;
  bool m_software = false;
  CProcessInfo *m_processInfo = nullptr;
  MMALState m_state;
  bool m_input;
  MMAL_POOL_T *m_mmal_pool;
  MMAL_COMPONENT_T *m_component;
  AVRpiZcFrameGeometry m_geo;
  struct MMALEncodingTable
  {
    AVPixelFormat pixfmt;
    uint32_t      encoding;
  };
  static std::vector<MMALEncodingTable> mmal_encoding_table;
};

// a generic mmal video frame. May be overridden as either software or hardware decoded buffer
class CMMALBuffer : public CVideoBuffer
{
public:
  CMMALBuffer(int id);
  virtual ~CMMALBuffer();
  MMAL_BUFFER_HEADER_T *mmal_buffer = nullptr;
  float m_aspect_ratio = 0.0f;
  MMALState m_state = MMALStateNone;
  bool m_rendered = false;
  bool m_stills = false;

  virtual void Unref();
  virtual std::shared_ptr<CMMALPool> Pool() { return std::dynamic_pointer_cast<CMMALPool>(m_pool); };
  virtual int Width() { return Pool()->Width(); }
  virtual int Height() { return Pool()->Height(); }
  virtual int AlignedWidth() { return Pool()->AlignedWidth(); }
  virtual int AlignedHeight() { return Pool()->AlignedHeight(); }
  virtual uint32_t &Encoding() { return Pool()->Encoding(); }
  virtual int BitsPerPixel() { return Pool()->BitsPerPixel(); }
  virtual void Update();

  void SetVideoDeintMethod(std::string method);
  const char *GetStateName() {
    static const char *names[] = { "MMALStateNone", "MMALStateHWDec", "MMALStateFFDec", "MMALStateDeint", "MMALStateBypass", };
    if ((size_t)m_state < vcos_countof(names))
      return names[(size_t)m_state];
    else
      return "invalid";
  }
protected:
};


class CMMALRenderer : public CBaseRenderer, public CThread, public IRunnable
{
public:
  CMMALRenderer();
  ~CMMALRenderer();

  void Process();
  virtual void Update();

  bool RenderCapture(CRenderCapture* capture);

  // Player functions
  virtual bool         Configure(const VideoPicture &picture, float fps, unsigned int orientation) override;
  virtual void         ReleaseBuffer(int idx) override;
  virtual void         UnInit();
  virtual bool         Flush(bool saveBuffers) override;
  virtual bool         IsConfigured() override { return m_bConfigured; }
  virtual void         AddVideoPicture(const VideoPicture& pic, int index) override;
  virtual bool         IsPictureHW(const VideoPicture &picture) override { return false; };
  virtual CRenderInfo GetRenderInfo() override;

  virtual bool         SupportsMultiPassRendering() override { return false; };
  virtual bool         Supports(ERENDERFEATURE feature) override;
  virtual bool         Supports(ESCALINGMETHOD method) override;

  virtual void         RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;

  virtual void SetVideoRect(const CRect& SrcRect, const CRect& DestRect);
  virtual bool         IsGuiLayer() override { return false; }
  virtual bool         ConfigChanged(const VideoPicture &picture) override { return false; }

  void vout_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
  void deint_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
  void deint_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);

  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static bool Register();

protected:
  CMMALBuffer         *m_buffers[NUM_BUFFERS];
  bool                 m_bConfigured;
  unsigned int         m_extended_format;
  int                  m_neededBuffers;

  CRect                     m_cachedSourceRect;
  CRect                     m_cachedDestRect;
  CRect                     m_src_rect;
  CRect                     m_dst_rect;
  RENDER_STEREO_MODE        m_video_stereo_mode;
  RENDER_STEREO_MODE        m_display_stereo_mode;
  bool                      m_StereoInvert;
  bool                      m_isPi1;

  CCriticalSection m_sharedSection;
  MMAL_COMPONENT_T *m_vout;
  MMAL_PORT_T *m_vout_input;
  MMAL_QUEUE_T *m_queue_render;
  MMAL_QUEUE_T *m_queue_process;
  CThread m_processThread;
  MMAL_BUFFER_HEADER_T m_quitpacket;
  double m_error;
  double m_lastPts;
  double m_frameInterval;
  double m_frameIntervalDiff;
  uint32_t m_vout_width, m_vout_height, m_vout_aligned_width, m_vout_aligned_height;
  // deinterlace
  MMAL_COMPONENT_T *m_deint;
  MMAL_PORT_T *m_deint_input;
  MMAL_PORT_T *m_deint_output;
  std::shared_ptr<CMMALPool> m_deint_output_pool;
  MMAL_INTERLACETYPE_T m_interlace_mode;
  EINTERLACEMETHOD  m_interlace_method;
  uint32_t m_deint_width, m_deint_height, m_deint_aligned_width, m_deint_aligned_height;
  MMAL_FOURCC_T m_deinterlace_out_encoding;
  void DestroyDeinterlace();
  bool CheckConfigurationDeint(uint32_t width, uint32_t height, uint32_t aligned_width, uint32_t aligned_height, uint32_t encoding, EINTERLACEMETHOD interlace_method, int bitsPerPixel);

  bool CheckConfigurationVout(uint32_t width, uint32_t height, uint32_t aligned_width, uint32_t aligned_height, uint32_t encoding);
  uint32_t m_vsync_count;
  void ReleaseBuffers();
  void UnInitMMAL();
  void UpdateFramerateStats(double pts);
  virtual void Run() override;
};

};
