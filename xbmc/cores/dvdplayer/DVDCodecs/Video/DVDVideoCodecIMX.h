#pragma once
/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://www.xbmc.org
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

#include "threads/CriticalSection.h"
#include "threads/Condition.h"
#include "threads/Thread.h"
#include "utils/BitstreamConverter.h"
#include "guilib/Geometry.h"
#include "system_gl.h"
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "guilib/DispResource.h"
#include "cores/dvdplayer/DVDCodecs/DVDCodecs.h"
#include "DVDClock.h"
#include "utils/log.h"
#include "utils/StringUtils.h"

#include <vector>
#include <linux/ipu.h>
#include <linux/mxcfb.h>
#include <imx-mm/vpu/vpu_wrapper.h>
#include <g2d.h>

#include <unordered_map>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

#define likely(x)       __builtin_expect(!!(x),1)
#define unlikely(x)     __builtin_expect(!!(x),0)

// The decoding format of the VPU buffer. Comment this to decode
// as NV12. The VPU works faster with NV12 in combination with
// deinterlacing.
// Progressive content seems to be handled faster with I420 whereas
// interlaced content is processed faster with NV12 as output format.
//#define IMX_INPUT_FORMAT_I420

// This enables logging of times for Decode, Render->Render,
// Deinterlace. It helps to profile several stages of
// processing with respect to changed kernels or other configurations.
// Since we utilize VPU, IPU and GPU at the same time different kernel
// priorities to those subsystems can result in a very different user
// experience. With that setting enabled we can build some statistics,
// as numbers are always better than "feelings"
//#define IMX_PROFILE_BUFFERS

//#define IMX_PROFILE
//#define TRACE_FRAMES

// If uncommented a file "stream.dump" will be created in the current
// directory whenever a new stream is started. This is only for debugging
// and performance tests. This define must never be active in distributions.
//#define DUMP_STREAM

inline
double recalcPts(double pts)
{
  return (double)(pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6);
}

template <typename T>
class lkFIFO
{
public:
  lkFIFO() { size_ = queue_.max_size(); abort_ = &no; }

public:
  T pop()
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    while (queue_.empty() && !*abort_)
      read_.wait(mlock);

    T val;
    if (likely(!*abort_))
    {
      val = queue_.front();
      queue_.pop_front();
    }

    mlock.unlock();
    write_.notify_one();
    return val;
  }

  bool push(const T& item)
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    while (queue_.size() == size_ && !*abort_)
      write_.wait(mlock);

    queue_.push_back(item);
    mlock.unlock();
    read_.notify_all();
    return true;
  }

  void signal_stop()
  {
    abort_ = &yes;
    write_.notify_all();
    read_.notify_all();
  }

  void shrink(std::deque<T> shrinkTo)
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.swap(shrinkTo);
    write_.notify_all();
  }

  // will expire unused data
  // and resize to empty
  void clear()
  {
    std::deque<T> tmp;
    shrink(tmp);
  }

  // will expire unused data
  // and resize to just fit
  void shrink_to_fit()
  {
    std::deque<T> tmp(queue_);
    shrink(tmp);
  }

  void set_quota_size(int newsize)
  {
    size_ = newsize;
    if (queue_.size() > size_)
    {
      queue_.resize(size_);
      shrink_to_fit();
    }
  }

  void for_each(void (*fn)(T &t))
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    std::for_each(queue_.begin(), queue_.end(), fn);
    write_.notify_all();
  }

  int empty() { return queue_.empty(); }
  int size() { return queue_.size(); }
private:
  std::deque<T> queue_;
  std::mutex mutex_;
  std::condition_variable read_, write_;

  unsigned int size_;
  volatile bool *abort_;
  bool no = false;
  bool yes = true;
};

/*>> TO BE MOVED TO A MORE CENTRAL PLACE IN THE SOURCE DIR >>>>>>>>>>>>>>>>>>>*/
// Generell description of a buffer used by
// the IMX context, e.g. for blitting
class CIMXBuffer {
public:
  CIMXBuffer() : m_iRefs(0) {}

  // Shared pointer interface
  virtual void Lock() = 0;
  virtual long Release() = 0;
  virtual bool IsValid() = 0;

  virtual void BeginRender() = 0;
  virtual void EndRender() = 0;

public:
  uint32_t     iWidth;
  uint32_t     iHeight;
  int          pPhysAddr;
  uint8_t     *pVirtAddr;
  int          iFormat;

protected:
  long         m_iRefs;
};

#define RENDER_TASK_AUTOPAGE -1
#define RENDER_TASK_CAPTURE  -2

// iMX context class that handles all iMX hardware
// related stuff
class CIMXContext : private CThread, IDispResource
{
public:
  CIMXContext();
  ~CIMXContext();

  bool AdaptScreen();
  bool TaskRestart();
  void CloseDevices();
  void g2dCloseDevices();
  bool OpenDevices();
  void g2dOpenDevices();

  bool Blank();
  bool Unblank();
  bool SetVSync(bool enable);

  bool IsValid() const { return IsRunning() && m_bFbIsConfigured; }

  // Populates a CIMXBuffer with attributes of a page
  bool GetPageInfo(CIMXBuffer *info, int page);

  // Blitter configuration
  bool IsDoubleRate() const { return m_currentFieldFmt & IPU_DEINTERLACE_RATE_EN; }

  void SetBlitRects(const CRect &srcRect, const CRect &dstRect);

  // Blits a buffer to a particular page (-1 for auto page)
  // source_p (previous buffer) is required for de-interlacing
  // modes LOW_MOTION and MED_MOTION.
  void Blit(CIMXBuffer *source_p, CIMXBuffer *source,
            uint8_t fieldFmt = 0, int targetPage = RENDER_TASK_AUTOPAGE,
            CRect *dest = NULL);

  // Shows a page vsynced
  bool ShowPage(int page, bool shift = false);

  // Returns the visible page
  int  GetCurrentPage() const { return m_fbCurrentPage; }

  // Clears the pages or a single page with 'black'
  void Clear(int page = -1);

  // Captures the current visible frame buffer page and blends it into
  // the passed overlay. The buffer format is BGRA (4 byte)
  void CaptureDisplay(unsigned char *buffer, int iWidth, int iHeight);
  bool PushCaptureTask(CIMXBuffer *source, CRect *dest);
  void *GetCaptureBuffer() const { if (m_bufferCapture) return m_bufferCapture->buf_vaddr; else return NULL; }
  void WaitCapture();

  void RendererAllowed(bool yes);
  void OnResetDevice();

  void SetSpeed(int iSpeed) { m_speed = iSpeed; }

  static const int               m_fbPages;

private:
  struct IPUTask
  {
    void Zero()
    {
      current = previous = NULL;
      memset(&task, 0, sizeof(task));
    }

    void Done()
    {
      SAFE_RELEASE(previous);
      SAFE_RELEASE(current);
    }

    // Kept for reference
    CIMXBuffer *previous;
    CIMXBuffer *current;

    // The actual task
    struct ipu_task task;

    int page;
    int shift;
  };

  static void callDone(IPUTask &t) { t.Done(); }

  bool GetFBInfo(const std::string &fbdev, struct fb_var_screeninfo *fbVar);

  bool PushTask(const IPUTask &);
  void PrepareTask(IPUTask &ipu, CIMXBuffer *source_p, CIMXBuffer *source,
                   CRect *dest = NULL);
  bool DoTask(IPUTask &ipu);

  void SetFieldData(uint8_t fieldFmt);

  void Dispose();
  void MemMap(struct fb_fix_screeninfo *fb_fix = NULL);

  virtual void OnStartup();
  virtual void OnExit();
  virtual void Process();
  void Stop(bool bWait = true);

private:
  lkFIFO<IPUTask>                m_input;

  int                            m_fbHandle;
  int                            m_fbCurrentPage;
  int                            m_fbWidth;
  int                            m_fbHeight;
  bool                           m_fbInterlaced;
  int                            m_fbLineLength;
  int                            m_fbPageSize;
  int                            m_fbPhysSize;
  int                            m_fbPhysAddr;
  uint8_t                       *m_fbVirtAddr;
  struct fb_var_screeninfo       m_fbVar;
  int                            m_ipuHandle;
  uint8_t                        m_currentFieldFmt;
  bool                           m_vsync;
  CRect                          m_srcRect;
  CRect                          m_dstRect;
  CRectInt                       m_inputRect;
  CRectInt                       m_outputRect;
  CRectInt                      *m_pageCrops;
  bool                           m_bFbIsConfigured;

  CCriticalSection               m_pageSwapLock;

  void                           *m_g2dHandle;
  struct g2d_buf                 *m_bufferCapture;
  bool                           m_CaptureDone;

  std::string                    m_deviceName;
  int                            m_speed;
};


extern CIMXContext g_IMXContext;

/*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/

typedef std::map<GLvoid*, GLuint> ptrToTexMap_t;

class CDecMemInfo
{
public:
  CDecMemInfo()
    : nVirtNum(0)
    , virtMem(NULL)
    , nPhyNum(0)
    , phyMem(NULL)
  {}

  //virtual mem info
  int         nVirtNum;
  void      **virtMem;

  //phy mem info
  int         nPhyNum;
  VpuMemDesc *phyMem;
};


// Base class of IMXVPU and IMXIPU buffer
class CDVDVideoCodecIMXBuffer : public CIMXBuffer
{
friend class CDVDVideoCodecIMX;
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXBuffer(int idx);
#else
  CDVDVideoCodecIMXBuffer();
#endif

  // reference counting
  virtual void Lock();
  virtual long Release();
  virtual bool IsValid() { return m_frameBuffer; }

  virtual void BeginRender() { m_renderLock.lock(); }
  virtual void EndRender()   { m_renderLock.unlock(); }

  void SetPts(double pts) { m_pts = pts; };
  double GetPts() const { return m_pts; }

  void SetDts(double dts) { m_dts = dts; };
  double GetDts() const { return m_dts; }

  void Queue(VpuDecOutFrameInfo *frameInfo);
  VpuDecRetCode ReleaseFramebuffer(VpuDecHandle *handle);
  VpuFieldType GetFieldType() const { return m_fieldType; }

private:
  // private because we are reference counted
  virtual ~CDVDVideoCodecIMXBuffer();

protected:
#ifdef TRACE_FRAMES
  int                      m_idx;
#endif

private:
  double                   m_pts;
  double                   m_dts;
  VpuFieldType             m_fieldType;
  VpuFrameBuffer          *m_frameBuffer;
  CCriticalSection         m_renderLock;     // Lock to protect buffers being rendered
  unsigned int             iFlags;
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Methods from CDVDVideoCodec which require overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose();
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void SetSkipMode(VpuDecSkipMode skip);
  virtual void Reset() { reset(); }
  virtual bool ClearPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName() { return (const char*)m_pFormatName; }
  virtual unsigned GetAllowedReferences();
  virtual bool GetInterlaced() { return m_initInfo.nInterlace; }

  static ptrToTexMap_t ptrToTexMap;

  virtual bool GetCodecStats(double &pts, int &droppedPics);
  virtual void SetCodecControl(int flags);
  virtual void SetSpeed(int iSpeed) { g_IMXContext.SetSpeed(iSpeed); m_speed = iSpeed; }

protected:
  bool VpuOpen();
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers(bool dispose = true);
  bool VpuAllocFrameBuffers();

  void SetVPUParams(VpuDecConfig InDecConf, void* pInParam);
  void SetDrainMode(VpuDecInputType drop);
  void SetCodecParam(VpuBufferNode *bn, unsigned char * data, unsigned int size);

  void reset(bool dispose = true);
  bool getOutputFrame(VpuDecOutFrameInfo *frm);

  static const int             m_extraVpuBuffers;   // Number of additional buffers for VPU
                                                    // by both decoding and rendering threads
  VpuMemInfo                   m_memInfo;
  CDVDStreamInfo               m_hints;             // Hints from demuxer at stream opening
  CDVDCodecOptions             m_options;
  const char                  *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam              m_decOpenParam;      // Parameters required to call VPU_DecOpen
  CDecMemInfo                  m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle                 m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo               m_initInfo;          // Initial info returned from VPU at decoding start
  VpuDecSkipMode               m_dropRequest;       // Current drop state
  int                          m_dropped;
  VpuDecInputType              m_drainMode;
  int                          m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer              *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  std::unordered_map<unsigned char*,CDVDVideoCodecIMXBuffer*>
                               m_outputBuffers;     // Table of VPU output buffers
  CDVDVideoCodecIMXBuffer     *m_currentBuffer;
  VpuMemDesc                  *m_extraMem;          // Table of allocated extra Memory
  bool                         m_usePTS;            // State whether pts out of decoding process should be used
  VpuDecOutFrameInfo           m_frameInfo;         // Store last VPU output frame info
  CBitstreamConverter         *m_converter;         // H264 annex B converter
  bool                         m_convert_bitstream; // State whether bitstream conversion is required
  int                          m_bytesToBeConsumed; // Remaining bytes in VPU
  bool                         m_frameReported;     // State whether the frame consumed event will be reported by libfslvpu
  bool                         m_warnOnce;          // Track warning messages to only warn once
  int                          m_codecControlFlags;
  int                          m_speed;
#ifdef DUMP_STREAM
  FILE                        *m_dump;
#endif
};
