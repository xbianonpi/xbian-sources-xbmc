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

#include <linux/mxcfb.h>
#include <vpu_lib.h>

#include "DVDVideoCodecIMX.h"

#include "settings/AdvancedSettings.h"
#include "threads/SingleLock.h"
#include "threads/SharedSection.h"
#include "utils/log.h"
#include "windowing/WindowingFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "guilib/GraphicContext.h"
#include "utils/StringUtils.h"
#include "settings/MediaSettings.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "utils/Environment.h"

#include "linux/imx/IMX.h"
#include <vpu_io.h>
#include <linux/ipu.h>
#include "libavcodec/avcodec.h"

#include "guilib/LocalizeStrings.h"

#include <cassert>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <list>
#include <algorithm>

#define FRAME_ALIGN             16
#define MEDIAINFO               1
#define RENDER_QUEUE_SIZE       3
#define DECODE_OUTPUT_SIZE      8
#define IN_DECODER_SET          -1

#define BIT(nr) (1UL << (nr))

#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS) || defined(TRACE_FRAMES)
unsigned char CDVDVideoCodecIMXBuffer::i = 0;
#endif

CIMXContext   g_IMXContext;
std::shared_ptr<CIMXCodec> g_IMXCodec;

std::list<VpuFrameBuffer*> m_recycleBuffers;

// Number of fb pages used for paning
const int CIMXContext::m_fbPages = 3;

// Experiments show that we need at least one more (+1) VPU buffer than the min value returned by the VPU
const unsigned int CIMXCodec::m_extraVpuBuffers = RENDER_QUEUE_SIZE + 1;

CDVDVideoCodecIMX::~CDVDVideoCodecIMX()
{
  m_IMXCodec.reset();
  if (g_IMXCodec.use_count() == 1)
    g_IMXCodec.reset();
}

bool CDVDVideoCodecIMX::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (!g_IMXCodec)
  {
    m_IMXCodec.reset(new CIMXCodec);
    g_IMXCodec = m_IMXCodec;
  }
  else
    m_IMXCodec = g_IMXCodec;

  return g_IMXCodec->Open(hints, options, m_pFormatName, &m_processInfo);
}

unsigned CDVDVideoCodecIMX::GetAllowedReferences()
{
  return RENDER_QUEUE_SIZE;
}

bool CDVDVideoCodecIMX::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (pDvdVideoPicture)
    SAFE_RELEASE(pDvdVideoPicture->IMXBuffer);

  return true;
}

bool CIMXCodec::VpuAllocBuffers(VpuMemInfo *pMemBlock)
{
  for(int i=0; i<pMemBlock->nSubBlockNum; i++)
  {
    if (pMemBlock->MemSubBlock[i].MemType == VPU_MEM_VIRT)
    { // Allocate standard virtual memory
      if (posix_memalign((void**)&pMemBlock->MemSubBlock[i].pVirtAddr, PAGE_SIZE, pMemBlock->MemSubBlock[i].nSize))
      {
        ExitError("%s - Unable to malloc %d bytes.\n", pMemBlock->MemSubBlock[i].nSize);
        return false;
      }
    }
    else
    { // Allocate contigous mem for DMA
      VpuMemDesc vpuMem = {};
      vpuMem.nSize = pMemBlock->MemSubBlock[i].nSize + PAGE_SIZE;
      if(!VpuAlloc(&vpuMem))
        return false;

      vpuMem.nSize = pMemBlock->MemSubBlock[i].nSize;
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(vpuMem.nVirtAddr, PAGE_SIZE);
      pMemBlock->MemSubBlock[i].pPhyAddr = (unsigned char*)Align(vpuMem.nPhyAddr, PAGE_SIZE);
    }
  }

  return true;
}

bool CIMXCodec::VpuFreeBuffers(bool dispose)
{
  m_decOutput.for_each(Release);
  g2d_buf *preserveVpuRing = m_allocated.pop();

  m_allocated.for_each(Release);

  if (dispose)
  {
    for(int i=0; i<m_memInfo.nSubBlockNum; i++)
    {
      if (m_memInfo.MemSubBlock[i].MemType == VPU_MEM_VIRT && m_memInfo.MemSubBlock[i].pVirtAddr)
      {
        free((void*)m_memInfo.MemSubBlock[i].pVirtAddr);
        m_memInfo.MemSubBlock[i] = {};
      }
    }

    g2d_free(preserveVpuRing);
  }
  else
  {
    m_allocated.push(preserveVpuRing);
  }

  m_vpuFrameBuffers.clear();
  return true;
}


bool CIMXCodec::VpuOpen()
{
  VpuDecRetCode  ret;
  VpuVersionInfo vpuVersion;
  int            param;

  memset(&m_memInfo, 0, sizeof(VpuMemInfo));
  ret = VPU_DecLoad();
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU load failed with error code %d.\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  ret = VPU_DecGetVersionInfo(&vpuVersion);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU version cannot be read (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  CLog::Log(LOGVIDEO, "VPU Lib version : major.minor.rel=%d.%d.%d.\n", vpuVersion.nLibMajor, vpuVersion.nLibMinor, vpuVersion.nLibRelease);

  ret = VPU_DecQueryMem(&m_memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
          CLog::Log(LOGERROR, "%s - iMX VPU query mem error (%d).\n", __FUNCTION__, ret);
          goto VpuOpenError;
  }

  if (!VpuAllocBuffers(&m_memInfo))
    goto VpuOpenError;

  m_decOpenParam.nReorderEnable = 1;
#ifdef IMX_INPUT_FORMAT_I420
  m_decOpenParam.nChromaInterleave = 0;
#else
  m_decOpenParam.nChromaInterleave = 1;
#endif
  m_decOpenParam.nTiled2LinearEnable = 0;
  m_decOpenParam.nEnableFileMode = 0;

  ret = VPU_DecOpen(&m_vpuHandle, &m_decOpenParam, &m_memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU open failed (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  param = 0;
  SetVPUParams(VPU_DEC_CONF_BUFDELAY, &param);

  return true;

VpuOpenError:
  Dispose();
  return false;
}

g2d_buf *CIMXCodec::VpuAlloc(VpuMemDesc *vpuMem)
{
  g2d_buf *g;
  if ((g = g2d_alloc(vpuMem->nSize, 0)))
  {
    vpuMem->nPhyAddr = g->buf_paddr;
    vpuMem->nVirtAddr = (unsigned long)g->buf_vaddr;
    m_allocated.push(g);
  }
  else
    CLog::Log(LOGERROR, "%s: vpu malloc frame buf of size %d\r\n",__FUNCTION__, vpuMem->nSize);

  return g;
}

bool CIMXCodec::VpuAllocFrameBuffers()
{
  int totalSize = 0;
  int ySize     = 0;
  int uSize     = 0;
  int vSize     = 0;
  int mvSize    = 0;
  int yStride   = 0;
  int uvStride  = 0;

  unsigned char* ptr;
  unsigned char* ptrVirt;
  int nAlign;

  int nrBuf = std::min(m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers + DECODE_OUTPUT_SIZE, (unsigned int)30);

  yStride = Align(m_initInfo.nPicWidth,FRAME_ALIGN);
  if(m_initInfo.nInterlace)
  {
    ySize = Align(m_initInfo.nPicWidth,FRAME_ALIGN)*Align(m_initInfo.nPicHeight,(2*FRAME_ALIGN));
  }
  else
  {
    ySize = Align(m_initInfo.nPicWidth,FRAME_ALIGN)*Align(m_initInfo.nPicHeight,FRAME_ALIGN);
  }

#ifdef IMX_INPUT_FORMAT_I420
  switch (m_initInfo.nMjpgSourceFormat)
  {
  case 0: // I420 (4:2:0)
    uvStride = yStride / 2;
    uSize = vSize = mvSize = ySize / 4;
    break;
  case 1: // Y42B (4:2:2 horizontal)
    uvStride = yStride / 2;
    uSize = vSize = mvSize = ySize / 2;
    break;
  case 3: // Y444 (4:4:4)
    uvStride = yStride;
    uSize = vSize = mvSize = ySize;
    break;
  default:
    CLog::Log(LOGERROR, "%s: invalid source format in init info\n",__FUNCTION__);
    return false;
  }

#else
  // NV12
  uvStride = yStride;
  uSize    = ySize/2;
  mvSize   = uSize/2;
#endif

  nAlign = m_initInfo.nAddressAlignment;
  if(nAlign>1)
  {
    ySize = Align(ySize, nAlign);
    uSize = Align(uSize, nAlign);
    vSize = Align(vSize, nAlign);
    mvSize = Align(mvSize, nAlign);
  }

  totalSize = ySize + uSize + vSize + mvSize + nAlign;
  for (int i=0 ; i < nrBuf; i++)
  {
    VpuMemDesc vpuMem = {};

    vpuMem.nSize = totalSize;
    if(!VpuAlloc(&vpuMem))
    {
      if (m_vpuFrameBuffers.size() < m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers)
        return false;

      CLog::Log(LOGWARNING, "%s: vpu can't allocate sufficient extra buffers. specify bigger CMA e.g. cma=320M.\r\n",__FUNCTION__);
      break;
    }

    //fill frameBuf
    ptr = (unsigned char*)vpuMem.nPhyAddr;
    ptrVirt = (unsigned char*)vpuMem.nVirtAddr;

    //align the base address
    if(nAlign>1)
    {
      ptr = (unsigned char*)Align(ptr,nAlign);
      ptrVirt = (unsigned char*)Align(ptrVirt,nAlign);
    }

    VpuFrameBuffer vpuFrameBuffer = {};
    m_vpuFrameBuffers.push_back(vpuFrameBuffer);

    // fill stride info
    m_vpuFrameBuffers[i].nStrideY           = yStride;
    m_vpuFrameBuffers[i].nStrideC           = uvStride;

    // fill phy addr
    m_vpuFrameBuffers[i].pbufY              = ptr;
    m_vpuFrameBuffers[i].pbufCb             = ptr + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufCr             = ptr + ySize + uSize;
#endif
    m_vpuFrameBuffers[i].pbufMvCol          = ptr + ySize + uSize + vSize;

    // fill virt addr
    m_vpuFrameBuffers[i].pbufVirtY          = ptrVirt;
    m_vpuFrameBuffers[i].pbufVirtCb         = ptrVirt + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufVirtCr         = ptrVirt + ySize + uSize;
#endif
    m_vpuFrameBuffers[i].pbufVirtMvCol      = ptrVirt + ySize + uSize + vSize;
  }

  if (VPU_DEC_RET_SUCCESS != VPU_DecRegisterFrameBuffer(m_vpuHandle, &m_vpuFrameBuffers[0], m_vpuFrameBuffers.size()))
    return false;

  m_processInfo->SetVideoDAR((double)m_initInfo.nQ16ShiftWidthDivHeightRatio/0x10000);

  m_decOutput.setquotasize(m_vpuFrameBuffers.size() - m_initInfo.nMinFrameBufferCount - m_extraVpuBuffers);
  return true;
}

CIMXCodec::CIMXCodec()
  : CThread("iMX VPU")
  , m_vpuHandle(0)
  , m_dropped(0)
  , m_lastPTS(DVD_NOPTS_VALUE)
  , m_codecControlFlags(0)
  , m_decSignal(0)
  , m_threadID(0)
  , m_decRet(VPU_DEC_INPUT_NOT_USED)
  , m_fps(-1)
  , m_burst(0)
{
  m_decOpenParam = {};
  m_nrOut.store(0);

  m_ring.reset(new CIMXCircular(5*1024*1024));

#ifdef DUMP_STREAM
  m_dump = NULL;
#endif
  m_decOutput.setquotasize(1);
  m_decInput.setquotasize(25);
  Reinit();
}

CIMXCodec::~CIMXCodec()
{
  g_IMXContext.SetProcessInfo(nullptr);
  StopThread(false);
  ProcessSignals(SIGNAL_SIGNAL);
  SetDrainMode(VPU_DEC_IN_DRAIN);
  StopThread();
}

void CIMXCodec::DisposeDecQueues()
{
  m_ring->reset();
  m_decInput.signal();
  m_decInput.for_each(Release);
  m_decOutput.signal();
  m_decOutput.for_each(Release);
  m_rebuffer = true;
}

void CIMXCodec::Reset()
{
  m_queuesLock.lock();
  DisposeDecQueues();
  ProcessSignals(SIGNAL_FLUSH);
  m_dtsDiff = 0.0;
  CLog::Log(LOGVIDEO, "iMX VPU : queues cleared ===== in/out %d/%d =====\n", m_decInput.size(), m_decOutput.size());
}

void CIMXCodec::Reinit()
{
  m_drainMode = VPU_DEC_IN_NORMAL;
  m_skipMode = VPU_DEC_SKIPNONE;

  m_fps = -1;
  m_ring->reset();
  m_decInput.setquotasize(60);
  m_loaded.Reset();
  m_dtsDiff = 0.0;
  CLog::Log(LOGVIDEO, "iMX VPU : Reinit()");
}

bool CIMXCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, std::string &m_pFormatName, CProcessInfo *m_pProcessInfo)
{
  CSingleLock lk(m_openLock);

  if (hints.software)
  {
    CLog::Log(LOGNOTICE, "iMX VPU : software decoding requested.\n");
    return false;
  }
  else if (hints.width > 1920)
  {
    CLog::Log(LOGNOTICE, "iMX VPU : software decoding forced - video dimensions out of spec: %d %d.", hints.width, hints.height);
    return false;
  }
  else if (hints.stills && hints.dvd)
    return false;

#ifdef DUMP_STREAM
  m_dump = fopen("stream.dump", "wb");
  if (m_dump != NULL)
  {
    fwrite(&hints.software, sizeof(hints.software), 1, m_dump);
    fwrite(&hints.codec, sizeof(hints.codec), 1, m_dump);
    fwrite(&hints.profile, sizeof(hints.profile), 1, m_dump);
    fwrite(&hints.codec_tag, sizeof(hints.codec_tag), 1, m_dump);
    fwrite(&hints.extrasize, sizeof(hints.extrasize), 1, m_dump);
    CLog::Log(LOGNOTICE, "Dump: HEADER: %d  %d  %d  %d  %d\n",
              hints.software, hints.codec, hints.profile,
              hints.codec_tag, hints.extrasize);
    if (hints.extrasize > 0)
      fwrite(hints.extradata, 1, hints.extrasize, m_dump);
  }
#endif

  if (m_hints != hints && g_IMXCodec->IsRunning())
  {
    StopThread(false);
    ProcessSignals(SIGNAL_FLUSH);
    m_decInput.for_each(Release);
    Reinit();
  }

  m_hints = hints;
  CLog::Log(LOGVIDEO, "Let's decode with iMX VPU\n");

  int param = 0;
  SetVPUParams(VPU_DEC_CONF_INPUTTYPE, &param);
  SetVPUParams(VPU_DEC_CONF_SKIPMODE, &param);

#ifdef MEDIAINFO
  {
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: CodecID %d \n", m_hints.codec);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: StreamType %d \n", m_hints.type);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: Level %d \n", m_hints.level);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: Profile %d \n", m_hints.profile);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: PTS_invalid %d \n", m_hints.ptsinvalid);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: Tag %d \n", m_hints.codec_tag);
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: %dx%d \n", m_hints.width,  m_hints.height);
  }
  { char str_tag[128]; av_get_codec_tag_string(str_tag, sizeof(str_tag), m_hints.codec_tag);
      CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: Tag fourcc %s\n", str_tag);
  }
  if (m_hints.extrasize)
  {
    char *buf = new char[m_hints.extrasize * 2 + 1];

    for (unsigned int i=0; i < m_hints.extrasize; i++)
      sprintf(buf+i*2, "%02x", ((uint8_t*)m_hints.extradata)[i]);

    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: extradata %d %s\n", m_hints.extrasize, buf);
    delete [] buf;
  }
  CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: %d / %d \n", m_hints.width,  m_hints.height);
  CLog::Log(LOGVIDEO, "Decode: aspect %f - forced aspect %d\n", m_hints.aspect, m_hints.forced_aspect);
#endif

  m_warnOnce = true;
  switch(m_hints.codec)
  {
  case AV_CODEC_ID_MPEG1VIDEO:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg1";
    break;
  case AV_CODEC_ID_MPEG2VIDEO:
  case AV_CODEC_ID_MPEG2VIDEO_XVMC:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg2";
    break;
  case AV_CODEC_ID_H263:
    m_decOpenParam.CodecFormat = VPU_V_H263;
    m_pFormatName = "iMX-h263";
    break;
  case AV_CODEC_ID_H264:
  {
    // Test for VPU unsupported profiles to revert to sw decoding
    if (m_hints.profile == 110)
    {
      CLog::Log(LOGNOTICE, "i.MX6 VPU is not able to decode AVC profile %d level %d", m_hints.profile, m_hints.level);
      return false;
    }
    m_decOpenParam.CodecFormat = VPU_V_AVC;
    m_pFormatName = "iMX-h264";
    break;
  }
  case AV_CODEC_ID_VC1:
    m_decOpenParam.CodecFormat = VPU_V_VC1_AP;
    m_pFormatName = "iMX-vc1";
    break;
  case AV_CODEC_ID_CAVS:
  case AV_CODEC_ID_AVS:
    m_decOpenParam.CodecFormat = VPU_V_AVS;
    m_pFormatName = "iMX-AVS";
    break;
  case AV_CODEC_ID_RV10:
  case AV_CODEC_ID_RV20:
  case AV_CODEC_ID_RV30:
  case AV_CODEC_ID_RV40:
    m_decOpenParam.CodecFormat = VPU_V_RV;
    m_pFormatName = "iMX-RV";
    break;
  case AV_CODEC_ID_KMVC:
    m_decOpenParam.CodecFormat = VPU_V_AVC_MVC;
    m_pFormatName = "iMX-MVC";
    break;
  case AV_CODEC_ID_VP8:
    m_decOpenParam.CodecFormat = VPU_V_VP8;
    m_pFormatName = "iMX-vp8";
    break;
  case AV_CODEC_ID_MPEG4:
    switch(m_hints.codec_tag)
    {
    case fourcc('D','I','V','X'):
      // Test for VPU unsupported profiles to revert to sw decoding
      if (m_hints.profile == -99 && m_hints.level == -99)
      {
        CLog::Log(LOGNOTICE, "i.MX6 iMX-divx4 profile %d level %d - sw decoding", m_hints.profile, m_hints.level);
        return false;
      }
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX4
      m_pFormatName = "iMX-divx4";
      break;
    case fourcc('D','X','5','0'):
    case fourcc('D','I','V','5'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX56
      m_pFormatName = "iMX-divx5";
      break;
    case fourcc('X','V','I','D'):
    case fourcc('x','v','i','d'):
    case fourcc('M','P','4','V'):
    case fourcc('P','M','P','4'):
    case fourcc('F','M','P','4'):
      m_decOpenParam.CodecFormat = VPU_V_XVID;
      m_pFormatName = "iMX-xvid";
      break;
    default:
      CLog::Log(LOGERROR, "iMX VPU : MPEG4 codec tag %d is not (yet) handled.\n", m_hints.codec_tag);
      return false;
    }
    break;
  default:
    CLog::Log(LOGERROR, "iMX VPU : codecid %d is not (yet) handled.\n", m_hints.codec);
    return false;
  }

  std::list<EINTERLACEMETHOD> deintMethods({ EINTERLACEMETHOD::VS_INTERLACEMETHOD_AUTO,
                                             EINTERLACEMETHOD::VS_INTERLACEMETHOD_RENDER_BOB });

  for(int i = EINTERLACEMETHOD::VS_INTERLACEMETHOD_IMX_FASTMOTION; i <= EINTERLACEMETHOD::VS_INTERLACEMETHOD_IMX_WEAVE; ++i)
    deintMethods.push_back(static_cast<EINTERLACEMETHOD>(i));

  m_processInfo = m_pProcessInfo;
  m_processInfo->SetVideoDecoderName(m_pFormatName, true);
  m_processInfo->SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo->SetVideoDeintMethod("none");
  m_processInfo->SetVideoPixelFormat("Y/CbCr 4:2:0");
  m_processInfo->UpdateDeinterlacingMethods(deintMethods);
  g_IMXContext.SetProcessInfo(m_processInfo);

  return true;
}

void CIMXCodec::Dispose()
{
#ifdef DUMP_STREAM
  if (m_dump)
  {
    fclose(m_dump);
    m_dump = NULL;
  }
#endif

  VpuDecRetCode  ret;
  bool VPU_loaded = m_vpuHandle;

  RecycleFrameBuffers();

  if (m_vpuHandle)
  {
    ret = VPU_DecClose(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
      CLog::Log(LOGERROR, "%s - VPU close failed with error code %d.\n", __FUNCTION__, ret);
    else
      CLog::Log(LOGVIDEO, "%s - VPU closed.", __FUNCTION__);

    m_vpuHandle = 0;
  }

  VpuFreeBuffers();

  if (VPU_loaded)
  {
    ret = VPU_DecUnLoad();
    if (ret != VPU_DEC_RET_SUCCESS)
      CLog::Log(LOGERROR, "%s - VPU unload failed with error code %d.\n", __FUNCTION__, ret);
  }
}

void CIMXCodec::SetVPUParams(VpuDecConfig InDecConf, void* pInParam)
{
  if (m_vpuHandle)
    if (VPU_DEC_RET_SUCCESS != VPU_DecConfig(m_vpuHandle, InDecConf, pInParam))
      CLog::Log(LOGERROR, "%s - iMX VPU set dec param failed (%d).\n", __FUNCTION__, (int)InDecConf);
}

void CIMXCodec::SetDrainMode(VpuDecInputType drain)
{
  if (m_drainMode == drain)
    return;

  m_drainMode = drain;
  VpuDecInputType config = drain == IN_DECODER_SET ? VPU_DEC_IN_DRAIN : drain;
  SetVPUParams(VPU_DEC_CONF_INPUTTYPE, &config);
  if (drain == VPU_DEC_IN_DRAIN && !EOS())
    ProcessSignals(SIGNAL_SIGNAL);
}

void CIMXCodec::SetSkipMode(VpuDecSkipMode skip)
{
  if (m_skipMode == skip)
    return;

  m_skipMode = skip;
  VpuDecSkipMode config = skip == IN_DECODER_SET ? VPU_DEC_SKIPB : skip;
  SetVPUParams(VPU_DEC_CONF_SKIPMODE, &config);
}

bool CIMXCodec::GetCodecStats(double &pts, int &droppedFrames, int &skippedPics)
{
  droppedFrames = m_dropped;
  skippedPics = -1;
  m_dropped = 0;
  pts = m_lastPTS;
  return true;
}

void CIMXCodec::SetCodecControl(int flags)
{
  if (!FBRegistered())
    return;

  m_codecControlFlags = flags;
  if (m_codecControlFlags & DVD_CODEC_CTRL_HURRY && !m_burst)
    m_burst = RENDER_QUEUE_SIZE;

  SetDrainMode(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN && !m_ring->used() ? VPU_DEC_IN_DRAIN : VPU_DEC_IN_NORMAL);
}

bool CIMXCodec::getOutputFrame(VpuDecOutFrameInfo *frm)
{
  VpuDecRetCode ret = VPU_DecGetOutputFrame(m_vpuHandle, frm);
  if(VPU_DEC_RET_SUCCESS != ret)
    CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
  return ret == VPU_DEC_RET_SUCCESS;
}

int CIMXCodec::Decode(BYTE *pData, int iSize, double dts, double pts)
{
  static class CIMXFps ptrn;
  static unsigned char mmm;

  if (EOS() && m_drainMode && !m_decOutput.size())
    return VC_BUFFER;

  int ret = 0;
  if (!g_IMXCodec->IsRunning())
  {
    if (!m_decInput.full() && (float)m_ring->used()/m_ring->size() < 0.9)
    {
      if (pts != DVD_NOPTS_VALUE)
        ptrn.Add(pts);
      else if (dts != DVD_NOPTS_VALUE)
        ptrn.Add(dts);

      if (m_decInput.size() < m_decInput.getquotasize() - 1)
        ret |= VC_BUFFER;
    }
    else
    {
      double fd = ptrn.GetFrameDuration(true);
      if (fd > 80000 && m_hints.fpsscale != 0)
        m_fps = (double)m_hints.fpsrate / m_hints.fpsscale;
      else if (fd != 0)
        m_fps = (double)DVD_TIME_BASE / fd;

      m_decInput.setquotasize(std::max((unsigned int)m_fps, (unsigned int)25));

      m_decOpenParam.nMapType = TILED_FRAME_MB_RASTER_MAP;

      ptrn.Flush();
      g_IMXCodec->Create();
      g_IMXCodec->WaitStartup();
    }
  }

  if (pData)
  {
    VPUTask *vpu = new VPUTask({ pData, iSize, 0, 0, 0, pts, dts, 0, 0 }, m_ring);
    m_decInput.push(VPUTaskPtr(vpu));
  }

  if (!IsDraining() && (float)m_ring->used()/m_ring->size() < 0.7 && m_decInput.size() < m_decInput.getquotasize() - 1)
  {
    ret |= VC_BUFFER;
    if (!m_burst && mmm++ % 3)
      goto log;
  }

  if (m_rebuffer && m_decInput.size() > m_decInput.getquotasize() /2)
    m_rebuffer = false;

  if ((m_decOutput.size() >= m_decOutput.getquotasize() /3 || m_drainMode || m_burst || !ret)
    && m_decOutput.size() && !m_rebuffer)
    ret |= VC_PICTURE;

  if (m_burst && ret & VC_PICTURE)
  {
    if (m_decInput.size() > m_decInput.getquotasize() /2)
      ret &= ~VC_BUFFER;
    --m_burst;
  }

log:
#ifdef IMX_PROFILE
  CLog::Log(LOGVIDEO, "%s - demux size: %d  dts : %f - pts : %f - addr : 0x%x, return %d ===== in/out %d(%.0f)/%d ===== dr: %d, drm: %d\n",
                       __FUNCTION__, iSize, recalcPts(dts), recalcPts(pts), (uint)pData, ret, m_decInput.size(), (float)m_ring->used()*100/m_ring->size(), m_decOutput.size(), IsDraining(), m_drainMode);
#endif

  if (!ret || m_drainMode)
    Sleep(5);

  return ret;
}

void CIMXCodec::ReleaseFramebuffer(VpuFrameBuffer* fb)
{
  m_recycleBuffers.push_back(fb);
}

void CIMXCodec::RecycleFrameBuffers()
{
  for(unsigned int i = 0; i < std::min(m_recycleBuffers.size(), (unsigned int)2); i++)
  {
    m_pts[m_recycleBuffers.front()] = DVD_NOPTS_VALUE;
    VPU_DecOutFrameDisplayed(m_vpuHandle, m_recycleBuffers.front());
    m_recycleBuffers.pop_front();
  --m_nrOut;
  }
}

inline
void CIMXCodec::AddExtraData(VpuBufferNode *bn, bool force)
{
  if ((m_decOpenParam.CodecFormat == VPU_V_MPEG2) ||
      (m_decOpenParam.CodecFormat == VPU_V_VC1_AP)||
      (m_decOpenParam.CodecFormat == VPU_V_XVID)  ||
      (force))
    bn->sCodecData = { (unsigned char *)m_hints.extradata, m_hints.extrasize };
  else
    bn->sCodecData = { nullptr, 0 };
}

void CIMXCodec::Process()
{
  VpuDecFrameLengthInfo         frameLengthInfo;
  VpuBufferNode                 inData;
  VpuBufferNode                 dummy = {};
  VpuDecRetCode                 ret;
  int                           retStatus;
  VpuDecOutFrameInfo            frameInfo;
#ifdef IMX_PROFILE
  int                           freeInfo;
#endif
  double                        fps = 60.0;
  VPUTaskPtr                    task;

  m_threadID = GetCurrentThreadId();
  SetPriority(GetPriority()+1);

  m_recycleBuffers.clear();
  m_pts.clear();
  m_loaded.Set();

  m_burst = 0;
  m_dropped = 0;
  m_dropRequest = false;
  m_lastPTS = DVD_NOPTS_VALUE;

  AddExtraData(&dummy, m_decOpenParam.CodecFormat == VPU_V_AVC && m_hints.extradata);
  inData = dummy;
  if (dummy.sCodecData.pData)
    CLog::Log(LOGVIDEO, "Decode: MEDIAINFO: adding extra codec data\n");

  if (!VpuOpen())
    return;

  while (!m_bStop && m_vpuHandle)
  {
    RecycleFrameBuffers();

    if (!(task = m_decInput.pop()))
      task.reset(new VPUTask());

    inData.pVirAddr = m_ring->pop(task->demux.iSize);
    inData.nSize = task->demux.iSize;

    // some streams have problem with getting intial info after seek into (during playback start).
    // feeding VPU with extra data helps
    if (!m_vpuFrameBuffers.size() && !task->IsEmpty() && m_decRet & VPU_DEC_NO_ENOUGH_INBUF)
      AddExtraData(&inData, true);

    while (!m_bStop) // Decode as long as the VPU consumes data
    {
      RecycleFrameBuffers();
      ProcessSignals();

      retStatus = m_decRet & VPU_DEC_SKIP ? VC_USERDATA : 0;

#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
      unsigned long long before_dec = XbmcThreads::SystemClockMillis();
#endif
      ret = VPU_DecDecodeBuf(m_vpuHandle, &inData, &m_decRet);

#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
      unsigned long long dec_time = XbmcThreads::SystemClockMillis()-before_dec;
#endif
#ifdef IMX_PROFILE
      VPU_DecGetNumAvailableFrameBuffers(m_vpuHandle, &freeInfo);
      CLog::Log(LOGVIDEO, "%s - VPU ret %d dec 0x%x decode takes : %lld (%db), free: %d\n\n", __FUNCTION__, ret, m_decRet,  dec_time, inData.nSize, freeInfo);
#endif

      if (m_skipMode == IN_DECODER_SET)
        SetSkipMode(VPU_DEC_SKIPNONE);

      if (m_drainMode == VPU_DEC_IN_KICK)
        SetDrainMode(VPU_DEC_IN_NORMAL);

      if (EOS())
        break;

      if (ret != VPU_DEC_RET_SUCCESS && ret != VPU_DEC_RET_FAILURE_TIMEOUT)
        ExitError("VPU decode failed with error code %d (0x%x).\n", ret, m_decRet);

      if (m_decRet & VPU_DEC_INIT_OK || m_decRet & VPU_DEC_RESOLUTION_CHANGED)
      // VPU decoding init OK : We can retrieve stream info
      {
        if (m_decRet & VPU_DEC_RESOLUTION_CHANGED && !(m_decSignal & SIGNAL_NOWAIT))
        {
          while (m_nrOut.load() && !m_bStop)
          {
            RecycleFrameBuffers();
            std::this_thread::yield();
          }
        }

        if (VPU_DecGetInitialInfo(m_vpuHandle, &m_initInfo) != VPU_DEC_RET_SUCCESS)
          ExitError("VPU get initial info failed");

        if (!VpuFreeBuffers(false))
          ExitError("VPU error while freeing frame buffers");
        else if (!VpuAllocFrameBuffers())
          ExitError("VPU error while registering frame buffers");

        if (m_initInfo.nInterlace && m_fps >= 49 && m_decOpenParam.nMapType == TILED_FRAME_MB_RASTER_MAP)
        {
          m_decOpenParam.nMapType = LINEAR_FRAME_MAP;
          Dispose();
          VpuOpen();
          continue;
        }


        fps = m_fps;
        if (m_initInfo.nInterlace && fps <= 30)
          fps *= 2;

        m_processInfo->SetVideoFps(m_fps);

        CLog::Log(LOGDEBUG, "%s - VPU Init Stream Info : %dx%d (interlaced : %d - Minframe : %d)"\
                  " - Align : %d bytes - crop : %d %d %d %d - Q16Ratio : %x, fps: %.3f\n", __FUNCTION__,
          m_initInfo.nPicWidth, m_initInfo.nPicHeight, m_initInfo.nInterlace, m_initInfo.nMinFrameBufferCount,
          m_initInfo.nAddressAlignment, m_initInfo.PicCropRect.nLeft, m_initInfo.PicCropRect.nTop,
          m_initInfo.PicCropRect.nRight, m_initInfo.PicCropRect.nBottom, m_initInfo.nQ16ShiftWidthDivHeightRatio, m_fps);

        if (m_decOpenParam.CodecFormat == VPU_V_AVC || dummy.sCodecData.pData)
        {
          SetDrainMode((VpuDecInputType)VPU_DEC_IN_KICK);
          inData = dummy;
          continue;
        }
      }

      if (m_hints.ptsinvalid && m_decRet & VPU_DEC_ONE_FRM_CONSUMED && m_decRet & CLASS_NOBUF)
        m_dtsDiff += DVD_TIME_BASE / m_fps;

      if (m_decRet & VPU_DEC_ONE_FRM_CONSUMED)
        if (!VPU_DecGetConsumedFrameInfo(m_vpuHandle, &frameLengthInfo) && frameLengthInfo.pFrame)
          m_pts[frameLengthInfo.pFrame] = task->demux.pts;

      if (m_decRet & CLASS_PICTURE && getOutputFrame(&frameInfo))
      {
        ++m_nrOut;
        CDVDVideoCodecIMXBuffer *buffer = new CDVDVideoCodecIMXBuffer(&frameInfo, fps, m_decOpenParam.nMapType);

        /* quick & dirty fix to get proper timestamping for VP8 codec */
        if (m_decOpenParam.CodecFormat == VPU_V_VP8)
          buffer->SetPts(task->demux.pts);
        else
          buffer->SetPts(m_pts[frameInfo.pDisplayFrameBuf]);

        buffer->SetDts(task->demux.dts - m_dtsDiff);
#ifdef IMX_PROFILE_BUFFERS
        CLog::Log(LOGVIDEO, "+D  %f/%f  %lld\n", recalcPts(buffer->GetDts()), recalcPts(buffer->GetPts()), dec_time);
#endif
#ifdef TRACE_FRAMES
        CLog::Log(LOGVIDEO, "+  0x%x dts %f pts %f  (VPU)\n", buffer->GetIdx(), recalcPts(buffer->GetDts()), recalcPts(buffer->GetPts()));
#endif

        if (m_decRet & VPU_DEC_OUTPUT_MOSAIC_DIS)
          buffer->SetFlags(DVP_FLAG_DROPPED);

        if (!m_decOutput.push(buffer))
          SAFE_RELEASE(buffer);
        else
          m_lastPTS = buffer->GetPts();

      }
      else if (m_decRet & CLASS_DROP)
        ++m_dropped;

      if (m_decRet & VPU_DEC_SKIP)
        ++m_dropped;

      if (m_decRet & VPU_DEC_NO_ENOUGH_BUF)
      {
        CLog::Log(LOGVIDEO, "iMX : VPU_DEC_NO_ENOUGH_BUF !");
        inData = dummy;
        continue;
      }

      if (retStatus & VC_USERDATA)
        continue;

      if (m_decRet & CLASS_FORCEBUF)
        break;

      if (!(m_drainMode ||
            m_decRet & (CLASS_NOBUF | CLASS_DROP)))
        break;

      inData = dummy;
    } // Decode loop
  } // Process() main loop

  ProcessSignals(SIGNAL_FLUSH | SIGNAL_RESET | SIGNAL_DISPOSE);
}

void CIMXCodec::ProcessSignals(int signal)
{
  if (!(m_decSignal | signal))
    return;

  if (signal & SIGNAL_SIGNAL)
  {
    m_decInput.signal();
    m_decOutput.signal();
    m_ring->signal();
  }

  CSingleLock lk(m_signalLock);
  m_decSignal |= signal & ~(SIGNAL_SIGNAL | SIGNAL_NOWAIT);

  if (!IsCurrentThread())
    return;

  int process = m_decSignal;
  m_decSignal = 0;

  if (process & SIGNAL_FLUSH)
  {
    FlushVPU();
    m_queuesLock.unlock();
    m_decSignal = SIGNAL_NOWAIT;
  }
  if (process & SIGNAL_RESET)
    DisposeDecQueues();
  if (process & SIGNAL_DISPOSE)
    Dispose();
}

void CIMXCodec::FlushVPU()
{
  CLog::Log(LOGVIDEO, "%s: VPU flush.\n", __FUNCTION__);
  int ret = VPU_DecFlushAll(m_vpuHandle);

  if (ret == VPU_DEC_RET_FAILURE_TIMEOUT)
    VPU_DecReset(m_vpuHandle);

  if (ret != VPU_DEC_RET_SUCCESS && ret != VPU_DEC_RET_INVALID_HANDLE)
    CLog::Log(LOGDEBUG, "%s: VPU flush failed with error code %d.\n", __FUNCTION__, ret);
}

inline
void CIMXCodec::ExitError(const char *msg, ...)
{
  va_list va;
  va_start(va, msg);
  CLog::Log(LOGERROR, "%s: %s", __FUNCTION__, StringUtils::FormatV(msg, va).c_str());
  va_end(va);

  StopThread(false);
}

bool CIMXCodec::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  auto buffer = m_decOutput.pop();
  assert(buffer);

#ifdef IMX_PROFILE
  static unsigned int previous = 0;
  unsigned int current;

  current = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "+G 0x%x %f/%f tm:%03d : Interlaced 0x%x\n", buffer->GetIdx(),
                            recalcPts(buffer->GetDts()), recalcPts(buffer->GetPts()), current - previous,
                            m_initInfo.nInterlace ? buffer->GetFieldType() : 0);
  previous = current;
#endif

  if (!m_dropRequest)
    pDvdVideoPicture->iFlags = buffer->GetFlags();
  else
    pDvdVideoPicture->iFlags = DVP_FLAG_DROPPED;

  if (m_initInfo.nInterlace)
  {
    if (buffer->GetFieldType() == VPU_FIELD_NONE && m_warnOnce)
    {
      m_warnOnce = false;
      CLog::Log(LOGWARNING, "Interlaced content reported by VPU, but full frames detected - Please turn off deinterlacing manually.");
    }
    else if (buffer->GetFieldType() == VPU_FIELD_TB || buffer->GetFieldType() == VPU_FIELD_TOP)
      pDvdVideoPicture->iFlags |= DVP_FLAG_TOP_FIELD_FIRST;

    pDvdVideoPicture->iFlags |= DVP_FLAG_INTERLACED;
  }

  pDvdVideoPicture->format = RENDER_FMT_IMXMAP;
  pDvdVideoPicture->iWidth = buffer->m_pctWidth;
  pDvdVideoPicture->iHeight = buffer->m_pctHeight;

  pDvdVideoPicture->iDisplayWidth = ((pDvdVideoPicture->iWidth * buffer->nQ16ShiftWidthDivHeightRatio) + 32767) >> 16;
  pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;

  pDvdVideoPicture->pts = buffer->GetPts();
  pDvdVideoPicture->dts = buffer->GetDts();

  if (pDvdVideoPicture->iFlags & DVP_FLAG_DROPPED)
    SAFE_RELEASE(buffer);

  pDvdVideoPicture->IMXBuffer = buffer;
  return true;
}

void CIMXCodec::SetDropState(bool bDrop)
{
  m_dropRequest = false;
}

bool CIMXCodec::IsCurrentThread() const
{
  return CThread::IsCurrentThread(m_threadID);
}

std::string CIMXCodec::GetPlayerInfo()
{
  std::ostringstream s;
  if (g_IMXCodec && m_ring->size())
    s << "buf In/Out: " << m_ring->used() * 100 / m_ring->size() << "%/" << m_decOutput.size();
  return s.str();
}

/*******************************************/
CDVDVideoCodecIMXBuffer::CDVDVideoCodecIMXBuffer(VpuDecOutFrameInfo *frameInfo, double fps, int map)
  : m_dts(DVD_NOPTS_VALUE)
  , m_fieldType(frameInfo->eFieldType)
  , m_frameBuffer(frameInfo->pDisplayFrameBuf)
  , m_iFlags(DVP_FLAG_ALLOCATED)
  , m_convBuffer(0)
{
  m_pctWidth  = frameInfo->pExtInfo->FrmCropRect.nRight - frameInfo->pExtInfo->FrmCropRect.nLeft;
  m_pctHeight = frameInfo->pExtInfo->FrmCropRect.nBottom - frameInfo->pExtInfo->FrmCropRect.nTop;

  // Some codecs (VC1?) lie about their frame size (mod 16). Adjust...
  iWidth      = (((frameInfo->pExtInfo->nFrmWidth) + 15) & ~15);
  iHeight     = (((frameInfo->pExtInfo->nFrmHeight) + 15) & ~15);

  pVirtAddr   = m_frameBuffer->pbufVirtY;
  pPhysAddr   = (int)m_frameBuffer->pbufY;

  nQ16ShiftWidthDivHeightRatio = frameInfo->pExtInfo->nQ16ShiftWidthDivHeightRatio;

#ifdef IMX_INPUT_FORMAT_I420
  iFormat     = fourcc('I', '4', '2', '0');
#else
  iFormat     = map == TILED_FRAME_MB_RASTER_MAP ? fourcc('T', 'N', 'V', 'P') :
                map == TILED_FIELD_MB_RASTER_MAP ? fourcc('T', 'N', 'V', 'F') : fourcc('N', 'V', '1', '2');
#endif
  m_fps       = fps;
#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS) || defined(TRACE_FRAMES)
  m_idx       = i++;
#endif
  Lock();
}

void CDVDVideoCodecIMXBuffer::Recycle()
{
  if (!m_frameBuffer)
    return;

  CIMXCodec::ReleaseFramebuffer(m_frameBuffer);
  m_frameBuffer = nullptr;
}

void CDVDVideoCodecIMXBuffer::Lock()
{
  ++m_iRefs;
}

long CDVDVideoCodecIMXBuffer::Release()
{
  long count = --m_iRefs;

  if (count)
    return count;

  Recycle();
  g_IMXContext.ReturnDma(m_convBuffer);

  delete this;
  return 0;
}

CDVDVideoCodecIMXBuffer::~CDVDVideoCodecIMXBuffer()
{
#ifdef TRACE_FRAMES
  CLog::Log(LOGVIDEO, "~  0x%x  (VPU)\n", m_idx);
#endif
}

CIMXContext::CIMXContext()
  : CThread("iMX IPU")
  , m_fbHandle(0)
  , m_fbCurrentPage(0)
  , m_fbPhysAddr(0)
  , m_fbVirtAddr(NULL)
  , m_ipuHandle(0)
  , m_pageCrops(nullptr)
  , m_bFbIsConfigured(false)
  , m_deviceName("/dev/fb1")
{
  m_showBuffer.setquotasize(1);
  m_dmaBuffer.setquotasize(0);
  OpenDevices();
}

CIMXContext::~CIMXContext()
{
  Stop();
  Dispose();

  CloseDevices();
}

bool CIMXContext::AdaptScreen(bool allocate)
{
  {
    CExclusiveLock lk(m_fbMapLock);
    MemMap();
  }

  struct fb_var_screeninfo fbVar;
  if (!GetFBInfo("/dev/fb0", &fbVar))
    return false;

  m_fbWidth = allocate ? 1920 : fbVar.xres;
  m_fbHeight = allocate ? 1080 : fbVar.yres;

  m_fbInterlaced = g_graphicsContext.GetResInfo().dwFlags & D3DPRESENTFLAG_INTERLACED;

  if (!GetFBInfo(m_deviceName, &m_fbVar))
    return false;

  m_fbVar.xoffset = 0;
  m_fbVar.yoffset = 0;

  if (!allocate && (fbVar.bits_per_pixel == 16 || (m_fps >= 49 && m_fbHeight == 1080)))
  {
    m_fbVar.nonstd = fourcc('Y', 'U', 'Y', 'V');
    m_fbVar.bits_per_pixel = 16;
  }
  else
  {
    m_fbVar.nonstd = fourcc('R', 'G', 'B', '4');
    m_fbVar.bits_per_pixel = 32;
  }

  m_fbVar.xres = m_fbWidth;
  m_fbVar.yres = m_fbHeight;

  if (m_fbInterlaced)
    m_fbVar.vmode |= FB_VMODE_INTERLACED;
  else
    m_fbVar.vmode &= ~FB_VMODE_INTERLACED;

  m_fbVar.yres_virtual = (m_fbVar.yres + 1) * m_fbPages;
  m_fbVar.xres_virtual = m_fbVar.xres;

  struct fb_fix_screeninfo fb_fix;

  bool err = false;
  {
    CExclusiveLock lk(m_fbMapLock);

    Blank();

    m_fbVar.activate = FB_ACTIVATE_NOW;
    if (ioctl(m_fbHandle, FBIOPUT_VSCREENINFO, &m_fbVar) == -1)
    {
      CLog::Log(LOGWARNING, "iMX : Failed to setup %s (%s)\n", m_deviceName.c_str(), strerror(errno));
      err = true;
    }
    else if (ioctl(m_fbHandle, FBIOGET_FSCREENINFO, &fb_fix) == -1)
    {
      CLog::Log(LOGWARNING, "iMX : Failed to query fixed screen info at %s (%s)\n", m_deviceName.c_str(), strerror(errno));
      err = true;
    }

    MemMap(&fb_fix);
    Unblank();
    m_bFbIsConfigured = true;
  }

  if (err)
  {
    TaskRestart();
    return false;
  }

  return true;
}

bool CIMXContext::GetFBInfo(const std::string &fbdev, struct fb_var_screeninfo *fbVar)
{
  int fb = open(fbdev.c_str(), O_RDWR | O_NONBLOCK);
  if (fb < 0)
  {
    CLog::Log(LOGWARNING, "iMX : Failed to open /dev/fb0\n");
    return false;
  }

  int err = ioctl(fb, FBIOGET_VSCREENINFO, fbVar);
  if (err < 0)
    CLog::Log(LOGWARNING, "iMX : Failed to query variable screen info at %s\n", fbdev.c_str());

  close(fb);
  return err >= 0;
}

void CIMXContext::MemMap(struct fb_fix_screeninfo *fb_fix)
{
  if (m_fbVirtAddr && m_fbPhysSize)
  {
    Clear();
    munmap(m_fbVirtAddr, m_fbPhysSize);
    m_fbVirtAddr = nullptr;
    m_fbPhysAddr = 0;
  }
  else if (fb_fix)
  {
    m_fbLineLength = fb_fix->line_length;
    m_fbPhysSize = fb_fix->smem_len;
    m_fbPageSize = m_fbLineLength * m_fbVar.yres_virtual / m_fbPages;
    m_fbPhysAddr = fb_fix->smem_start;
    m_fbVirtAddr = (uint8_t*)mmap(0, m_fbPhysSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbHandle, 0);
    Clear();
  }
}

void CIMXContext::OnLostDisplay()
{
  CSingleLock lk(m_pageSwapLock);
  m_bFbIsConfigured = false;
}

void CIMXContext::OnResetDisplay()
{
  CSingleLock lk(m_pageSwapLock);
  if (m_bFbIsConfigured)
    return;

  CLog::Log(LOGDEBUG, "iMX : %s - going to change screen parameters\n", __FUNCTION__);
  AdaptScreen();
}

bool CIMXContext::TaskRestart()
{
  CLog::Log(LOGINFO, "iMX : %s - restarting IMX rendererer\n", __FUNCTION__);
  // Stop the ipu thread
  Stop();
  MemMap();
  CloseDevices();

  Create();
  return true;
}

void CIMXContext::Dispose()
{
  if (!m_pageCrops)
    return;

  delete[] m_pageCrops;
  m_pageCrops = nullptr;
}

bool CIMXContext::OpenDevices()
{
  if ((m_fbHandle = open(m_deviceName.c_str(), O_RDWR | O_NONBLOCK)) < 0)
    m_fbHandle = 0;
  if ((m_ipuHandle = open("/dev/mxc_ipu", O_RDWR | O_SYNC | O_NONBLOCK, 0)) < 0)
    m_ipuHandle = 0;

  bool opened = m_fbHandle > 0 && m_ipuHandle > 0;
  if (!opened)
    CLog::Log(LOGWARNING, "iMX : Failed to open framebuffer: %s\n", m_deviceName.c_str());

  return opened;
}

void CIMXContext::CloseDevices()
{
  CLog::Log(LOGINFO, "iMX : Closing devices\n");

  if (m_fbHandle)
  {
    close(m_fbHandle);
    m_fbHandle = 0;
  }

  m_showBuffer.for_each(Release);

  if (m_ipuHandle)
  {
    close(m_ipuHandle);
    m_ipuHandle = 0;
  }
}

bool CIMXContext::Blank()
{
  if (!m_fbHandle) return false;
  return ioctl(m_fbHandle, FBIOBLANK, FB_BLANK_NORMAL) == 0;
}

bool CIMXContext::Unblank()
{
  if (!m_fbHandle) return false;
  return ioctl(m_fbHandle, FBIOBLANK, FB_BLANK_UNBLANK) == 0;
}

inline
void CIMXContext::SetFieldData(uint8_t fieldFmt, double fps)
{
  if (m_bStop || !IsRunning() || !m_bFbIsConfigured)
    return;

  static EINTERLACEMETHOD imPrev;
  fieldFmt &= -!m_fbInterlaced;

  bool dr = IsDoubleRate();
  bool deint = !!m_currentFieldFmt;
  m_currentFieldFmt = fieldFmt;

  if (!!fieldFmt != deint ||
      dr != IsDoubleRate()||
      fps != m_fps        ||
      imPrev != CMediaSettings::GetInstance().GetCurrentVideoSettings().m_InterlaceMethod)
    m_bFbIsConfigured = false;

  if (m_bFbIsConfigured)
    return;

  if (IsDoubleRate())
    m_showBuffer.setquotasize(2);
  else
    m_showBuffer.setquotasize(1);

  m_fps = fps;
  imPrev = CMediaSettings::GetInstance().GetCurrentVideoSettings().m_InterlaceMethod;
  CLog::Log(LOGDEBUG, "iMX : Output parameters changed - deinterlace %s%s, fps: %.3f\n", !!fieldFmt ? "active" : "not active", IsDoubleRate() ? " DR" : "", m_fps);
  SetIPUMotion(imPrev);

  CSingleLock lk(m_pageSwapLock);
  AdaptScreen();
}

#define MASK1 (IPU_DEINTERLACE_RATE_FRAME1 | RENDER_FLAG_TOP)
#define MASK2 (IPU_DEINTERLACE_RATE_FRAME1 | RENDER_FLAG_BOT)
#define VAL1  MASK1
#define VAL2  RENDER_FLAG_BOT

inline
bool checkIPUStrideOffset(struct ipu_deinterlace *d, bool DR)
{
  switch (d->motion)
  {
  case HIGH_MOTION:
    return ((d->field_fmt & MASK1) == VAL1) || ((d->field_fmt & MASK2) == VAL2);
  case MED_MOTION:
    return DR && (((d->field_fmt & MASK1) == VAL1) || ((d->field_fmt & MASK2) != VAL2));
  default:
    return true;
  }
}

inline
void CIMXContext::SetIPUMotion(EINTERLACEMETHOD imethod)
{
  std::string strImethod;

  if (m_processInfo)
    m_processInfo->SetVideoDeintMethod("none");

  if (!m_currentFieldFmt || imethod == VS_INTERLACEMETHOD_NONE)
    return;

  switch (imethod)
  {
  case VS_INTERLACEMETHOD_IMX_WEAVE_HALF:
    strImethod = g_localizeStrings.Get(16336);
    m_motion   = LOW_MOTION;
    break;

  case VS_INTERLACEMETHOD_IMX_WEAVE:
    strImethod = g_localizeStrings.Get(16338);
    m_motion   = LOW_MOTION;
    break;

  case VS_INTERLACEMETHOD_IMX_ADVMOTION:
    strImethod = g_localizeStrings.Get(16337);
    m_motion   = MED_MOTION;
    break;

  case VS_INTERLACEMETHOD_AUTO:
  case VS_INTERLACEMETHOD_IMX_ADVMOTION_HALF:
    strImethod = g_localizeStrings.Get(16335);
    m_motion   = MED_MOTION;
    break;

  case VS_INTERLACEMETHOD_IMX_FASTMOTION:
    strImethod = g_localizeStrings.Get(16334);
    m_motion   = HIGH_MOTION;
    break;

  default:
    strImethod = g_localizeStrings.Get(16021);
    m_motion   = HIGH_MOTION;
    break;
  }

  if (m_processInfo)
    m_processInfo->SetVideoDeintMethod(strImethod);
}

unsigned int CIMXContext::Blit(CIMXBuffer *source_p, CIMXBuffer *source, const CRect &srcRect,
                       const CRect &dstRect, uint8_t fieldFmt, int page)
{
  static unsigned char pg;

  if (page == RENDER_TASK_AUTOPAGE)
    page = pg;

  if (page < 0 && page >= m_fbPages)
    return -1;

  CIMXIPUTask *task = new CIMXIPUTask(source, source_p, page);
  IPUTaskPtr ipu(task);

#ifdef IMX_PROFILE_BUFFERS
  unsigned long long before = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGVIDEO, "+p 0x%x@%d  - (buf: %d/%d)", ipu->cb->GetIdx(), ipu->page, m_showBuffer.size(), m_dmaBuffer.size());
#endif
  SetFieldData(fieldFmt, source->m_fps);

  if (!PrepareTask(ipu, srcRect, dstRect))
    return -1;

  if (!TileTask(ipu))
  {
    m_waitVSync.Set();
    return -1;
  }

  DeintTask(ipu);

  m_showBuffer.push(ipu);
  pg = (1 + pg) % m_fbPages;

#ifdef IMX_PROFILE_BUFFERS
  unsigned long long after = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGVIDEO, "+P 0x%x@%d  %d (buf: %d/%d)", ipu->cb->GetIdx(), ipu->page, (int)(after-before), m_showBuffer.size(), m_dmaBuffer.size());
#endif

  if (g_IMXCodec)
    return g_IMXCodec->GetLockedBuffers();

  return -1;
}

void CIMXContext::WaitVSync()
{
  m_waitVSync.WaitMSec(1300 / g_graphicsContext.GetFPS());
}

inline
bool CIMXContext::ShowPage()
{
  IPUTaskPtr ipu = m_showBuffer.pop();

  if (!ipu)
    return false;

#if defined(TRACE_FRAMES) || defined(IMX_PROFILE_BUFFERS)
  static unsigned long long pgprev;
  unsigned long long pgstart = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGVIDEO, "np(@%d) - fbiopan: %d\n", m_fbCurrentPage, (int)(pgstart - pgprev));
#endif

  CSharedLock lock(m_fbMapLock);
  {

    CSingleLock lk(m_pageSwapLock);
    {
      if (!m_bFbIsConfigured)
        return false;

      if (DoTask(ipu))
        m_fbVar.yoffset = (m_fbVar.yres + 1) * ipu->page + !checkIPUStrideOffset(&ipu->task.input.deinterlace, IsDoubleRate());
      else
        return false;
    }

    if (m_bStop)
      return true;

#if defined(TRACE_FRAMES) || defined(IMX_PROFILE_BUFFERS)
    unsigned long long pgend = XbmcThreads::SystemClockMillis();
    CLog::Log(LOGVIDEO, "NP(@%d) - pgswap: %d (%d)\n", ipu->page, (int)(pgend - pgstart), (int)(pgend - pgprev));
    pgprev = pgend;
#endif

    m_fbVar.activate = FB_ACTIVATE_VBL;
    if (ioctl(m_fbHandle, FBIOPAN_DISPLAY, &m_fbVar) < 0)
      CLog::Log(LOGWARNING, "Panning failed: %s\n", strerror(errno));

    m_fbCurrentPage = ipu->page;
    m_waitVSync.Set();

    if (ioctl(m_fbHandle, FBIO_WAITFORVSYNC, nullptr) < 0)
      CLog::Log(LOGWARNING, "Vsync failed: %s\n", strerror(errno));

  }

  if (g_IMXCodec && !g_IMXCodec->GetBurst() && g_graphicsContext.GetResInfo().fRefreshRate <= 24.0)
    CThread::Sleep(std::rand() % 7 + 7);

  return true;
}

void CIMXContext::SetProcessInfo(CProcessInfo *m_pProcessInfo)
{
  m_processInfo = m_pProcessInfo;
  if (!m_processInfo)
    return;

  SetIPUMotion(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_InterlaceMethod);
}

void CIMXContext::Clear(int page)
{
  if (!m_fbVirtAddr) return;

  uint16_t clr = 128 << 8 | 16;
  uint32_t clr32 = (clr << 16) | clr;
  uint8_t *tmp_buf;
  int bytes;

  if (page < 0)
  {
    tmp_buf = m_fbVirtAddr;
    bytes = m_fbPageSize*m_fbPages;
  }
  else if (page < m_fbPages)
  {
    tmp_buf = m_fbVirtAddr + page*m_fbPageSize;
    bytes = m_fbPageSize;
  }
  else
    // out of range
    return;

  if (m_fbVar.nonstd == fourcc('R', 'G', 'B', '4'))
    memset(tmp_buf, 0, bytes);
  else if (m_fbVar.nonstd == fourcc('Y', 'U', 'Y', 'V'))
    for (int i = 0; i < bytes / 4; ++i, tmp_buf += 4)
      memcpy(tmp_buf, &clr32, 4);
  else
    CLog::Log(LOGERROR, "iMX Clear fb error : Unexpected format");

  SetProcessInfo(m_processInfo);
}

bool CIMXContext::PrepareTask(IPUTaskPtr &ipu, CRect srcRect, CRect dstRect)
{
  CSharedLock lock(m_fbMapLock);

  if (srcRect.IsEmpty() || dstRect.IsEmpty())
    return false;

  CRectInt iSrcRect, iDstRect;

  float srcWidth = srcRect.Width();
  float srcHeight = srcRect.Height();
  float dstWidth = dstRect.Width();
  float dstHeight = dstRect.Height();

  // Project coordinates outside the target buffer rect to
  // the source rect otherwise the IPU task will fail
  // This is under the assumption that the srcRect is always
  // inside the input buffer rect. If that is not the case
  // it needs to be projected to the ouput buffer rect as well
  if (dstRect.x1 < 0)
  {
    srcRect.x1 -= dstRect.x1*srcWidth / dstWidth;
    dstRect.x1 = 0;
  }
  if (dstRect.x2 > m_fbWidth)
  {
    srcRect.x2 -= (dstRect.x2-m_fbWidth)*srcWidth / dstWidth;
    dstRect.x2 = m_fbWidth;
  }
  if (dstRect.y1 < 0)
  {
    srcRect.y1 -= dstRect.y1*srcHeight / dstHeight;
    dstRect.y1 = 0;
  }
  if (dstRect.y2 > m_fbHeight)
  {
    srcRect.y2 -= (dstRect.y2-m_fbHeight)*srcHeight / dstHeight;
    dstRect.y2 = m_fbHeight;
  }

  iSrcRect.x1 = Align((int)srcRect.x1,8);
  iSrcRect.y1 = Align((int)srcRect.y1,8);
  iSrcRect.x2 = Align2((int)srcRect.x2,8);
  iSrcRect.y2 = Align2((int)srcRect.y2,16);

  iDstRect.x1 = Align((int)dstRect.x1,8);
  iDstRect.y1 = Align((int)dstRect.y1,8);
  iDstRect.x2 = Align2((int)dstRect.x2,8);
  iDstRect.y2 = std::min(Align2((int)dstRect.y2,16)+8, m_fbHeight);

  if (srcRect.IsEmpty() || dstRect.IsEmpty())
    return false;

  ipu->task.input.crop.pos.x  = iSrcRect.x1;
  ipu->task.input.crop.pos.y  = iSrcRect.y1;
  ipu->task.input.crop.w      = iSrcRect.Width();
  ipu->task.input.crop.h      = iSrcRect.Height();

  ipu->task.output.crop.pos.x = iDstRect.x1;
  ipu->task.output.crop.pos.y = iDstRect.y1;
  ipu->task.output.crop.w     = iDstRect.Width();
  ipu->task.output.crop.h     = iDstRect.Height();

  ipu->task.input.width   = ipu->cb->iWidth;
  ipu->task.input.height  = ipu->cb->iHeight;
  ipu->task.input.format  = ipu->cb->iFormat;
  ipu->task.input.paddr   = ipu->cb->pPhysAddr;

  return true;
}

bool CIMXContext::TileTask(IPUTaskPtr &ipu)
{
  m_zoomAllowed = true;

  // on double rate deinterlacing this is reusing pb already rasterised frame
  if (ipu->cb->iFormat != fourcc('T', 'N', 'V', 'F') && ipu->cb->iFormat != fourcc('T', 'N', 'V', 'P'))
    return true;

  // Use band mode directly to FB, as no transformations needed (eg cropping)
  if (m_fps >= 49 && m_fbWidth == 1920 && ipu->task.input.width == 1920 && !ipu->task.input.deinterlace.enable && CEnvironment::getenv("IPU_RESIZE").empty())
  {
    m_zoomAllowed = false;
    ipu->task.output.crop.pos.x = ipu->task.input.crop.pos.x = 0;
    ipu->task.output.crop.pos.y = ipu->task.input.crop.pos.y = 0;
    ipu->task.output.crop.h     = ipu->task.input.height     = ipu->task.input.crop.h = ipu->cb->iHeight;
    ipu->task.output.crop.w     = ipu->task.input.width      = ipu->task.input.crop.w = ipu->cb->iWidth;
    if (ipu->task.input.crop.h < m_fbHeight)
      ipu->task.output.paddr     += m_fbLineLength * (m_fbHeight - ipu->task.input.crop.h)/2;
    return true;
  }
/*
  // check for 3-field deinterlace (no HIGH_MOTION allowed) from tile field format
  if (ipu->cb->iFormat == fourcc('T', 'N', 'V', 'F') && ipu->pb)
  {
    ipu->task.input.paddr     = ipu->pb->pPhysAddr;
    ipu->task.input.paddr_n   = ipu->cb->pPhysAddr;
    ipu->task.input.deinterlace.field_fmt = IPU_DEINTERLACE_FIELD_TOP;
    ipu->task.input.deinterlace.enable = true;

    ipu->task.output.crop.pos.x = ipu->task.input.crop.pos.x = 0;
    ipu->task.output.crop.pos.y = ipu->task.input.crop.pos.y = 0;
    ipu->task.output.crop.h     = ipu->task.input.height     = ipu->task.input.crop.h = ipu->cb->iHeight;
    ipu->task.output.crop.w     = ipu->task.input.width      = ipu->task.input.crop.w = ipu->cb->iWidth;

    return CheckTask(ipu) == 0;
  }
*/
  // rasterize from tile (frame)
  struct ipu_task vdoa = {};

  vdoa.input.width   = vdoa.output.width  = vdoa.output.crop.w = vdoa.input.crop.w = ipu->cb->iWidth;
  vdoa.input.height  = vdoa.output.height = vdoa.output.crop.h = vdoa.input.crop.h = ipu->cb->iHeight;
  vdoa.input.format  = ipu->cb->iFormat;
  vdoa.input.paddr   = ipu->cb->pPhysAddr;
  vdoa.output.format = ipu->task.input.format = m_fbVar.bits_per_pixel == 16 ? fourcc('Y', 'U', 'Y', 'V') : fourcc('N', 'V', '1', '2');

  dma_addr_t buffer = AllocateBuffer(vdoa.input.width, vdoa.input.height);
  if (buffer < 0)
    return false;

  vdoa.output.paddr = ipu->cb->SetConvBuffer(buffer);

  if (!RunIoctl(m_ipuHandle, IPU_QUEUE_TASK, &vdoa))
    return false;

  ipu->cb->Recycle();

  ipu->task.input.format = vdoa.output.format;
  ipu->task.input.paddr  = vdoa.output.paddr;

  ipu->cb->iFormat   = vdoa.output.format;
  ipu->cb->pPhysAddr = vdoa.output.paddr;

  return true;
}

bool CIMXContext::DeintTask(IPUTaskPtr &ipu)
{
  if (!m_currentFieldFmt || !ipu->pb)
    return false;

  ipu->task.input.deinterlace.enable = 1;
  ipu->task.input.deinterlace.motion = m_motion;
  ipu->task.input.deinterlace.field_fmt = m_currentFieldFmt;

  if (ipu->task.input.deinterlace.motion != HIGH_MOTION)
    ipu->task.input.paddr_n = ipu->cb->pPhysAddr;

  ipu->task.input.format  = ipu->cb->iFormat;
  ipu->task.input.paddr   = ipu->task.input.paddr_n ? ipu->pb->pPhysAddr : ipu->cb->pPhysAddr;

  return true;
}

int CIMXContext::CheckTask(struct ipu_task *task)
{
  int ret = IPU_CHECK_ERR_INPUT_CROP;
  while (ret != IPU_CHECK_OK && ret > IPU_CHECK_ERR_MIN)
  {
    ret = ioctl(m_ipuHandle, IPU_CHECK_TASK, task);
    switch (ret)
    {
      case IPU_CHECK_OK:
        break;
      case IPU_CHECK_ERR_SPLIT_INPUTW_OVER:
        task->input.crop.w -= 8;
        break;
      case IPU_CHECK_ERR_SPLIT_INPUTH_OVER:
        task->input.crop.h -= 8;
        break;
      case IPU_CHECK_ERR_SPLIT_OUTPUTW_OVER:
        task->output.width -= 8;
        task->output.crop.w = task->output.width;
        break;
      case IPU_CHECK_ERR_SPLIT_OUTPUTH_OVER:
        task->output.height -= 8;
        task->output.crop.h = task->output.height;
        break;
      // no IPU processing needed
      case IPU_CHECK_ERR_PROC_NO_NEED:
        return ret;
      default:
        CLog::Log(LOGWARNING, "iMX : unhandled IPU check error: %d", ret);
        return ret;
      }
  }

  return 0;
}

bool CIMXContext::DoTask(IPUTaskPtr &ipu)
{
  CSharedLock lock(m_fbMapLock);

  ipu->task.output.width  = m_fbWidth;
  ipu->task.output.height = m_fbHeight;
  ipu->task.output.format = m_fbVar.nonstd;
  ipu->task.output.paddr  = m_fbPhysAddr + ipu->page*m_fbPageSize;

  if (CheckTask(ipu))
    return false;

  // Clear page if cropping changes
  CRectInt dstRect(ipu->task.output.crop.pos.x, ipu->task.output.crop.pos.y,
                   ipu->task.output.crop.pos.x + ipu->task.output.crop.w,
                   ipu->task.output.crop.pos.y + ipu->task.output.crop.h);

  if (m_pageCrops[ipu->page] != dstRect)
  {
    m_pageCrops[ipu->page] = dstRect;
    Clear(ipu->page);
  }

  return RunIoctl(m_ipuHandle, IPU_QUEUE_TASK, &ipu->task);
}

inline
bool CIMXContext::RunIoctl(int handle, int cmd, struct ipu_task *task)
{
  if (ioctl(handle, cmd, task) < 0)
  {
    CLog::Log(LOGERROR, "IPU task failed: %s at #%d\n", strerror(errno), __LINE__);
    return false;
  }

  return true;
}

bool CIMXContext::CaptureDisplay(unsigned char *&buffer, int iWidth, int iHeight, bool blend)
{
  void *g2dHandle;
  int size = iWidth * iHeight * 4;
  struct g2d_buf *bufferCapture = g2d_alloc(size, 0);

  if (!bufferCapture || g2d_open(&g2dHandle))
  {
    CLog::Log(LOGERROR, "%s : Error while trying open/allocate G2D\n", __FUNCTION__);
    return false;
  }

  if (!buffer)
    buffer = new uint8_t[size];
  else if (blend)
    std::memcpy(bufferCapture->buf_vaddr, buffer, bufferCapture->buf_size);

  CSingleLock lk(m_pageSwapLock);
  if (buffer)
  {
    struct g2d_surface src = {};
    struct g2d_surface dst = {};

    {
      src.planes[0] = m_fbPhysAddr + m_fbCurrentPage * m_fbPageSize;
      dst.planes[0] = bufferCapture->buf_paddr;
      if (m_fbVar.bits_per_pixel == 16)
      {
        src.format = G2D_YUYV;
        src.planes[1] = src.planes[0] + Align(m_fbWidth * m_fbHeight, 64);
        src.planes[2] = src.planes[1] + Align((m_fbWidth * m_fbHeight) / 2, 64);
      }
      else
      {
        src.format = G2D_RGBX8888;
      }

      dst.left = dst.top = src.top = src.left = 0;
      src.stride = src.right = src.width = m_fbWidth;
      src.bottom = src.height = m_fbHeight;

      dst.right = dst.stride = dst.width = iWidth;
      dst.bottom = dst.height = iHeight;

      dst.rot = src.rot = G2D_ROTATION_0;
      dst.format = G2D_BGRA8888;

      if (blend)
      {
        src.blendfunc = G2D_ONE_MINUS_DST_ALPHA;
        dst.blendfunc = G2D_ONE;
        g2d_enable(g2dHandle, G2D_BLEND);
      }

      g2d_blit(g2dHandle, &src, &dst);
      g2d_finish(g2dHandle);
      g2d_disable(g2dHandle, G2D_BLEND);
    }

    std::memcpy(buffer, bufferCapture->buf_vaddr, bufferCapture->buf_size);
  }
  else
    CLog::Log(LOGERROR, "iMX : Error allocating capture buffer\n");

  if (g2d_free(bufferCapture))
    CLog::Log(LOGERROR, "iMX : Error while freeing capture buuffer\n");

  g2d_close(g2dHandle);
  return true;
}

dma_addr_t CIMXContext::AllocateBuffer(unsigned int x, unsigned int y)
{
  int dmaPhy;

  if (!m_dmaBuffer.size())
  {
    dmaPhy = x * y * 4;

    if (ioctl(m_ipuHandle, IPU_ALLOC, &dmaPhy) < 0)
      dmaPhy = -1;

    m_dmaBuffer.setquotasize(m_dmaBuffer.getquotasize() + 1);
  }
  else
  {
    dmaPhy = m_dmaBuffer.pop();
  }

  if (dmaPhy < 0)
    CLog::Log(LOGERROR, "iMX : Error allocating crop buffer (%d)", dmaPhy);

  return dmaPhy;
}

void CIMXContext::DeallocateBuffer()
{
  CLog::Log(LOGVIDEO, "iMX : Deallocating crop buffers (%d)", m_dmaBuffer.getquotasize());
  
  while (m_dmaBuffer.size() < m_dmaBuffer.getquotasize())
    Sleep(10);

  while (m_dmaBuffer.size() && m_ipuHandle)
    ioctl(m_ipuHandle, IPU_FREE, m_dmaBuffer.pop());

  m_dmaBuffer.setquotasize(0);
  CLog::Log(LOGVIDEO, "iMX : Deallocating crop buffers done");
}

void CIMXContext::Allocate()
{
  m_pageCrops = new CRectInt[m_fbPages];

  CSingleLock lk(m_pageSwapLock);
  AdaptScreen(true);
  AdaptScreen();
  Create();
}

void CIMXContext::OnStartup()
{
  CSingleLock lk(m_pageSwapLock);
  g_Windowing.Register(this);
  CLog::Log(LOGNOTICE, "iMX : IPU thread started");
}

void CIMXContext::OnExit()
{
  CSingleLock lk(m_pageSwapLock);
  g_Windowing.Unregister(this);
  CLog::Log(LOGNOTICE, "iMX : IPU thread terminated");
}

void CIMXContext::Stop(bool bWait /*= true*/)
{
  if (!IsRunning())
    return;

  CThread::StopThread(false);
  m_showBuffer.signal();
  m_dmaBuffer.signal();
  if (bWait && IsRunning())
    CThread::StopThread(true);
}

void CIMXContext::Process()
{
  while (!m_bStop)
  {
    if (!ShowPage())
      m_waitVSync.Set();
  }

  m_showBuffer.for_each(Release);
}
