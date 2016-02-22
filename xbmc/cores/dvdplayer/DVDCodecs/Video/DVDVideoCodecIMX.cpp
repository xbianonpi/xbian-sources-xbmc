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

#include "DVDVideoCodecIMX.h"

#include "Application.h"
#include "settings/AdvancedSettings.h"
#include "threads/SingleLock.h"
#include "threads/Atomics.h"
#include "utils/log.h"
#include "DVDClock.h"
#include "windowing/WindowingFactory.h"
#include "guilib/GraphicContext.h"
#include "cores/VideoRenderers/BaseRenderer.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "utils/SysfsUtils.h"

#include <cassert>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <algorithm>
#include <linux/fb.h>
#include <forward_list>

#define IMX_VDI_MAX_WIDTH 968
#define FRAME_ALIGN 16
#define MEDIAINFO 1
#define RENDER_QUEUE_SIZE 3
#define IN_DECODER_SET -1
#define _4CC(c1,c2,c3,c4) (((uint32_t)(c4)<<24)|((uint32_t)(c3)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c1))
#define Align(ptr,align)  (((unsigned int)ptr + (align) - 1)/(align)*(align))
#define Align2(ptr,align)  (((unsigned int)ptr)/(align)*(align))

#define BIT(nr) (1UL << (nr))

// Global instance
CIMXContext g_IMXContext;
ptrToTexMap_t CDVDVideoCodecIMX::ptrToTexMap;
std::forward_list<CDVDVideoCodecIMXBuffer*> m_recycleBuffers;

// Number of fb pages used for paning
const int CIMXContext::m_fbPages = 3;

// Experiments show that we need at least one more (+1) VPU buffer than the min value returned by the VPU
const int CDVDVideoCodecIMX::m_extraVpuBuffers = 1+RENDER_QUEUE_SIZE+CIMXContext::m_fbPages;

bool CDVDVideoCodecIMX::VpuAllocBuffers(VpuMemInfo *pMemBlock)
{
  int i, size;
  void* ptr;
  VpuMemDesc vpuMem;
  VpuDecRetCode ret;

  for(i=0; i<pMemBlock->nSubBlockNum; i++)
  {
    size = pMemBlock->MemSubBlock[i].nAlignment + pMemBlock->MemSubBlock[i].nSize;
    if (pMemBlock->MemSubBlock[i].MemType == VPU_MEM_VIRT)
    { // Allocate standard virtual memory
      ptr = malloc(size);
      if(ptr == NULL)
      {
        CLog::Log(LOGERROR, "%s - Unable to malloc %d bytes.\n", __FUNCTION__, size);
        goto AllocFailure;
      }
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(ptr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.nVirtNum++;
      m_decMemInfo.virtMem = (void**)realloc(m_decMemInfo.virtMem, m_decMemInfo.nVirtNum*sizeof(void*));
      m_decMemInfo.virtMem[m_decMemInfo.nVirtNum-1] = ptr;
    }
    else
    { // Allocate contigous mem for DMA
      vpuMem.nSize = size;
      ret = VPU_DecGetMem(&vpuMem);
      if(ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Unable alloc %d bytes of physical memory (%d).\n", __FUNCTION__, size, ret);
        goto AllocFailure;
      }
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(vpuMem.nVirtAddr, pMemBlock->MemSubBlock[i].nAlignment);
      pMemBlock->MemSubBlock[i].pPhyAddr = (unsigned char*)Align(vpuMem.nPhyAddr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.nPhyNum++;
      m_decMemInfo.phyMem = (VpuMemDesc*)realloc(m_decMemInfo.phyMem, m_decMemInfo.nPhyNum*sizeof(VpuMemDesc));
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nPhyAddr = vpuMem.nPhyAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nVirtAddr = vpuMem.nVirtAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nCpuAddr = vpuMem.nCpuAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nSize = size;
    }
  }

  return true;

AllocFailure:
  VpuFreeBuffers();
  return false;
}

bool CDVDVideoCodecIMX::VpuFreeBuffers(bool dispose)
{
  VpuMemDesc vpuMem;
  VpuDecRetCode vpuRet;
  int freePhyNum = dispose ? m_decMemInfo.nPhyNum : m_vpuFrameBufferNum;
  bool ret = true;

  for (auto buf : m_outputBuffers)
    buf.second->Release();

  if (m_decMemInfo.virtMem && dispose)
  {
    //free virtual mem
    for(int i=0; i<m_decMemInfo.nVirtNum; i++)
    {
      if (m_decMemInfo.virtMem[i])
        free((void*)m_decMemInfo.virtMem[i]);
    }
    free(m_decMemInfo.virtMem);
    m_decMemInfo.virtMem = NULL;
    m_decMemInfo.nVirtNum = 0;
  }

  if (m_decMemInfo.nPhyNum)
  {
    int released = 0;
    //free physical mem
    for(int i=m_decMemInfo.nPhyNum - 1; i>=m_decMemInfo.nPhyNum - freePhyNum; i--)
    {
      vpuMem.nPhyAddr = m_decMemInfo.phyMem[i].nPhyAddr;
      vpuMem.nVirtAddr = m_decMemInfo.phyMem[i].nVirtAddr;
      vpuMem.nCpuAddr = m_decMemInfo.phyMem[i].nCpuAddr;
      vpuMem.nSize = m_decMemInfo.phyMem[i].nSize;
      vpuRet = VPU_DecFreeMem(&vpuMem);
      if(vpuRet != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Error while trying to free physical memory (%d).\n", __FUNCTION__, ret);
        ret = false;
        break;
      }
      else
        released++;
    }

    m_decMemInfo.nPhyNum -= released;
    if (!m_decMemInfo.nPhyNum)
    {
      free(m_decMemInfo.phyMem);
      m_decMemInfo.phyMem = NULL;
    }
  }

  m_outputBuffers.clear();

  m_vpuFrameBufferNum = 0;
  if (m_vpuFrameBuffers != NULL)
  {
    delete m_vpuFrameBuffers;
    m_vpuFrameBuffers = NULL;
  }

  return ret;
}


bool CDVDVideoCodecIMX::VpuOpen()
{
  VpuDecRetCode  ret;
  VpuVersionInfo vpuVersion;
  VpuMemInfo     memInfo;
  int            param;

  memset(&memInfo, 0, sizeof(VpuMemInfo));
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
  else
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "VPU Lib version : major.minor.rel=%d.%d.%d.\n", vpuVersion.nLibMajor, vpuVersion.nLibMinor, vpuVersion.nLibRelease);
  }

  ret = VPU_DecQueryMem(&memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
          CLog::Log(LOGERROR, "%s - iMX VPU query mem error (%d).\n", __FUNCTION__, ret);
          goto VpuOpenError;
  }

  if (!VpuAllocBuffers(&memInfo))
    goto VpuOpenError;

  m_decOpenParam.nReorderEnable = 1;
#ifdef IMX_INPUT_FORMAT_I420
  m_decOpenParam.nChromaInterleave = 0;
#else
  m_decOpenParam.nChromaInterleave = 1;
#endif
  m_decOpenParam.nMapType = 0;
  m_decOpenParam.nTiled2LinearEnable = 0;
  m_decOpenParam.nEnableFileMode = 0;

  ret = VPU_DecOpen(&m_vpuHandle, &m_decOpenParam, &memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU open failed (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  SetSkipMode(VPU_DEC_SKIPNONE);
  SetDrainMode(VPU_DEC_IN_NORMAL);

  param = 0;
  SetVPUParams(VPU_DEC_CONF_BUFDELAY, &param);

  return true;

VpuOpenError:
  Dispose();
  return false;
}

bool CDVDVideoCodecIMX::VpuAllocFrameBuffers()
{
  int totalSize = 0;
  int ySize     = 0;
  int uSize     = 0;
  int vSize     = 0;
  int mvSize    = 0;
  int yStride   = 0;
  int uvStride  = 0;

  VpuDecRetCode ret;
  VpuMemDesc vpuMem;
  unsigned char* ptr;
  unsigned char* ptrVirt;
  int nAlign;

  m_vpuFrameBufferNum = m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers;
  m_vpuFrameBuffers = new VpuFrameBuffer[m_vpuFrameBufferNum];

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
    CLog::Log(LOGERROR, "%s: invalid source format in init info\n",__FUNCTION__,ret);
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

  for (int i=0 ; i < m_vpuFrameBufferNum; i++)
  {
    totalSize = ySize + uSize + vSize + mvSize + nAlign;

    vpuMem.nSize = totalSize;
    ret = VPU_DecGetMem(&vpuMem);
    if(ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: vpu malloc frame buf failure: ret=%d \r\n",__FUNCTION__,ret);
      return false;
    }

    //record memory info for release
    m_decMemInfo.nPhyNum++;
    m_decMemInfo.phyMem = (VpuMemDesc*)realloc(m_decMemInfo.phyMem, m_decMemInfo.nPhyNum*sizeof(VpuMemDesc));
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nPhyAddr = vpuMem.nPhyAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nVirtAddr = vpuMem.nVirtAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nCpuAddr = vpuMem.nCpuAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nSize = vpuMem.nSize;

    //fill frameBuf
    ptr = (unsigned char*)vpuMem.nPhyAddr;
    ptrVirt = (unsigned char*)vpuMem.nVirtAddr;

    //align the base address
    if(nAlign>1)
    {
      ptr = (unsigned char*)Align(ptr,nAlign);
      ptrVirt = (unsigned char*)Align(ptrVirt,nAlign);
    }

    // fill stride info
    m_vpuFrameBuffers[i].nStrideY           = yStride;
    m_vpuFrameBuffers[i].nStrideC           = uvStride;

    // fill phy addr
    m_vpuFrameBuffers[i].pbufY              = ptr;
    m_vpuFrameBuffers[i].pbufCb             = ptr + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufCr             = ptr + ySize + uSize;
#else
    m_vpuFrameBuffers[i].pbufCr             = 0;
#endif
    m_vpuFrameBuffers[i].pbufMvCol          = ptr + ySize + uSize + vSize;

    // fill virt addr
    m_vpuFrameBuffers[i].pbufVirtY          = ptrVirt;
    m_vpuFrameBuffers[i].pbufVirtCb         = ptrVirt + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufVirtCr         = ptrVirt + ySize + uSize;
#else
    m_vpuFrameBuffers[i].pbufVirtCr         = 0;
#endif
    m_vpuFrameBuffers[i].pbufVirtMvCol      = ptrVirt + ySize + uSize + vSize;

    m_vpuFrameBuffers[i].pbufY_tilebot      = 0;
    m_vpuFrameBuffers[i].pbufCb_tilebot     = 0;
    m_vpuFrameBuffers[i].pbufVirtY_tilebot  = 0;
    m_vpuFrameBuffers[i].pbufVirtCb_tilebot = 0;

#ifdef TRACE_FRAMES
    m_outputBuffers[m_vpuFrameBuffers[i].pbufY] = new CDVDVideoCodecIMXBuffer(i);
#else
    m_outputBuffers[m_vpuFrameBuffers[i].pbufY] = new CDVDVideoCodecIMXBuffer();
#endif
  }

  return true;
}

CDVDVideoCodecIMX::CDVDVideoCodecIMX()
{
  m_pFormatName = "iMX-xxx";
  m_vpuHandle = 0;
  m_vpuFrameBuffers = NULL;
  m_currentBuffer = NULL;
  m_extraMem = NULL;
  m_vpuFrameBufferNum = 0;
  m_usePTS = true;
  if (getenv("IMX_NOPTS") != NULL)
  {
    m_usePTS = false;
  }
  m_converter = NULL;
#ifdef DUMP_STREAM
  m_dump = NULL;
#endif
  memset(&m_frameInfo, 0, sizeof(m_frameInfo));
  m_drainMode = (VpuDecInputType)-2;
  m_dropRequest = (VpuDecSkipMode)-2;
  m_codecControlFlags = 0;
}

CDVDVideoCodecIMX::~CDVDVideoCodecIMX()
{
  Dispose();
}

bool CDVDVideoCodecIMX::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
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

  m_hints = hints;
  CLog::Log(LOGDEBUG | LOGVIDEO, "Let's decode with iMX VPU\n");

#ifdef MEDIAINFO
  {
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: CodecID %d \n", m_hints.codec);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: StreamType %d \n", m_hints.type);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: Level %d \n", m_hints.level);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: Profile %d \n", m_hints.profile);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: PTS_invalid %d \n", m_hints.ptsinvalid);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: Tag %d \n", m_hints.codec_tag);
    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: %dx%d \n", m_hints.width,  m_hints.height);
  }
  { uint8_t *pb = (uint8_t*)&m_hints.codec_tag;
    if ((isalnum(pb[0]) && isalnum(pb[1]) && isalnum(pb[2]) && isalnum(pb[3])))
      CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: Tag fourcc %c%c%c%c\n", pb[0], pb[1], pb[2], pb[3]);
  }
  if (m_hints.extrasize)
  {
    char buf[4096];

    for (unsigned int i=0; i < m_hints.extrasize; i++)
      sprintf(buf+i*2, "%02x", ((uint8_t*)m_hints.extradata)[i]);

    CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: extradata %d %s\n", m_hints.extrasize, buf);
  }
  CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: MEDIAINFO: %d / %d \n", m_hints.width,  m_hints.height);
  CLog::Log(LOGDEBUG | LOGVIDEO, "Decode: aspect %f - forced aspect %d\n", m_hints.aspect, m_hints.forced_aspect);
#endif

  m_warnOnce = true;
  m_convert_bitstream = false;
  switch(m_hints.codec)
  {
  case CODEC_ID_MPEG1VIDEO:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg1";
    break;
  case CODEC_ID_MPEG2VIDEO:
  case CODEC_ID_MPEG2VIDEO_XVMC:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg2";
    break;
  case CODEC_ID_H263:
    m_decOpenParam.CodecFormat = VPU_V_H263;
    m_pFormatName = "iMX-h263";
    break;
  case CODEC_ID_H264:
  {
    // Test for VPU unsupported profiles to revert to sw decoding
    if ((m_hints.profile == 110) || //hi10p
        (m_hints.profile == 578 && m_hints.level == 30))   //quite uncommon h264 profile with Main 3.0
    {
      CLog::Log(LOGNOTICE, "i.MX6 VPU is not able to decode AVC profile %d level %d", m_hints.profile, m_hints.level);
      return false;
    }
    m_decOpenParam.CodecFormat = VPU_V_AVC;
    m_pFormatName = "iMX-h264";
    if (hints.extradata)
    {
      if ( *(char*)hints.extradata == 1 )
      {
        m_converter         = new CBitstreamConverter();
        m_convert_bitstream = m_converter->Open(hints.codec, (uint8_t *)m_hints.extradata, m_hints.extrasize, true);
        free(m_hints.extradata);
        m_hints.extrasize = m_converter->GetExtraSize();
        m_hints.extradata = malloc(m_hints.extrasize);
        memcpy(m_hints.extradata, m_converter->GetExtraData(), m_hints.extrasize);
      }
    }
    break;
  }
  case CODEC_ID_VC1:
    m_decOpenParam.CodecFormat = VPU_V_VC1_AP;
    m_pFormatName = "iMX-vc1";
    break;
  case CODEC_ID_CAVS:
  case CODEC_ID_AVS:
    m_decOpenParam.CodecFormat = VPU_V_AVS;
    m_pFormatName = "iMX-AVS";
    break;
  case CODEC_ID_RV10:
  case CODEC_ID_RV20:
  case CODEC_ID_RV30:
  case CODEC_ID_RV40:
    m_decOpenParam.CodecFormat = VPU_V_RV;
    m_pFormatName = "iMX-RV";
    break;
  case CODEC_ID_KMVC:
    m_decOpenParam.CodecFormat = VPU_V_AVC_MVC;
    m_pFormatName = "iMX-MVC";
    break;
  case CODEC_ID_VP8:
    m_decOpenParam.CodecFormat = VPU_V_VP8;
    m_pFormatName = "iMX-vp8";
    break;
  case CODEC_ID_MPEG4:
    switch(m_hints.codec_tag)
    {
    case _4CC('D','I','V','X'):
      // Test for VPU unsupported profiles to revert to sw decoding
      if (m_hints.profile == -99 && m_hints.level == -99)
      {
        CLog::Log(LOGNOTICE, "i.MX6 iMX-divx4 profile %d level %d - sw decoding", m_hints.profile, m_hints.level);
        return false;
      }
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX4
      m_pFormatName = "iMX-divx4";
      break;
    case _4CC('D','X','5','0'):
    case _4CC('D','I','V','5'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX56
      m_pFormatName = "iMX-divx5";
      break;
    case _4CC('X','V','I','D'):
    case _4CC('M','P','4','V'):
    case _4CC('P','M','P','4'):
    case _4CC('F','M','P','4'):
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

  return true;
}

void CDVDVideoCodecIMX::Dispose()
{
#ifdef DUMP_STREAM
  if (m_dump)
  {
    fclose(m_dump);
    m_dump = NULL;
  }
#endif

  g_IMXContext.Clear();

  VpuDecRetCode  ret;
  bool VPU_loaded = m_vpuHandle;

  reset(true);

  if (m_vpuHandle)
  {
    ret = VPU_DecClose(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU close failed with error code %d.\n", __FUNCTION__, ret);
    }
    m_vpuHandle = 0;
  }

  VpuFreeBuffers();

  if (VPU_loaded)
  {
    ret = VPU_DecUnLoad();
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU unload failed with error code %d.\n", __FUNCTION__, ret);
    }
  }

  if (m_converter)
  {
    m_converter->Close();
    SAFE_DELETE(m_converter);
  }
}

inline
void CDVDVideoCodecIMX::SetVPUParams(VpuDecConfig InDecConf, void* pInParam)
{
  if (VPU_DEC_RET_SUCCESS != VPU_DecConfig(m_vpuHandle, InDecConf, pInParam))
    CLog::Log(LOGERROR, "%s - iMX VPU set dec param failed.\n", __FUNCTION__);
}

void CDVDVideoCodecIMX::SetDrainMode(VpuDecInputType drain)
{
  if (!m_vpuHandle || m_drainMode == drain)
    return;

  m_drainMode = drain;
  VpuDecInputType config = drain == IN_DECODER_SET ? VPU_DEC_IN_DRAIN : drain;
  SetVPUParams(VPU_DEC_CONF_INPUTTYPE, &config);
  CLog::Log(LOGDEBUG | LOGVIDEO, ">>>>>>>>>>>>>>>>>>>>> %s - iMX VPU set drain mode %d.\n", __FUNCTION__, config);
}

inline
void CDVDVideoCodecIMX::SetSkipMode(VpuDecSkipMode skip)
{
  if (!m_vpuHandle || m_dropRequest == skip)
    return;

  m_dropRequest = skip;
  VpuDecSkipMode config = skip == IN_DECODER_SET ? VPU_DEC_SKIPB : skip;
  SetVPUParams(VPU_DEC_CONF_SKIPMODE, &config);
  CLog::Log(LOGDEBUG | LOGVIDEO, ">>>>>>>>>>>>>>>>>>>>> %s - iMX VPU set skip mode %d.\n", __FUNCTION__, config);
}

inline
void CDVDVideoCodecIMX::SetCodecParam(VpuBufferNode *bn, unsigned char *data, unsigned int size)
{
  switch (m_drainMode)
  {
    case VPU_DEC_IN_NORMAL: bn->sCodecData.pData = data; bn->sCodecData.nSize = size; break;
                   default: bn->sCodecData.pData = NULL; bn->sCodecData.nSize =    0;
                            bn->pVirAddr         = NULL; bn->nSize            =    0;
  }
}

bool CDVDVideoCodecIMX::GetCodecStats(double &pts, int &droppedPics)
{
  droppedPics = m_dropped;
  m_dropped = 0;
  pts = DVD_NOPTS_VALUE;
  return true;
}

void CDVDVideoCodecIMX::SetCodecControl(int flags)
{
  m_codecControlFlags = flags;
#ifdef DVD_CODEC_CTRL_DROP
  if (m_drainMode != IN_DECODER_SET)
    SetDrainMode(flags & DVD_CODEC_CTRL_DRAIN && m_vpuFrameBufferNum ? VPU_DEC_IN_DRAIN : VPU_DEC_IN_NORMAL);
#endif
}

bool CDVDVideoCodecIMX::getOutputFrame(VpuDecOutFrameInfo *frm)
{
  VpuDecRetCode ret = VPU_DecGetOutputFrame(m_vpuHandle, frm);
  if(VPU_DEC_RET_SUCCESS != ret)
    CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
  return ret == VPU_DEC_RET_SUCCESS;
}

int CDVDVideoCodecIMX::Decode(BYTE *pData, int iSize, double dts, double pts)
{
  VpuDecFrameLengthInfo frameLengthInfo;
  VpuBufferNode inData;
  VpuDecRetCode ret;
  static int decRet;
  int retStatus = 0;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;

#ifdef IMX_PROFILE
  static unsigned long long previous, current;
#endif
#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
  unsigned long long before_dec;
#endif

#ifdef DUMP_STREAM
  if (m_dump != NULL)
  {
    if (pData)
    {
      fwrite(&dts, sizeof(double), 1, m_dump);
      fwrite(&pts, sizeof(double), 1, m_dump);
      fwrite(&iSize, sizeof(int), 1, m_dump);
      fwrite(pData, 1, iSize, m_dump);
    }
  }
#endif

  if (!m_vpuHandle)
  {
    VpuOpen();
    if (!m_vpuHandle)
      return VC_ERROR;
  }

  while(!m_recycleBuffers.empty())
  {
    m_recycleBuffers.front()->ReleaseFramebuffer(&m_vpuHandle);
    m_recycleBuffers.pop_front();
  }

#ifdef IMX_PROFILE
  current = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "%s - delta time decode : %llu - demux size : %d  dts : %f - pts : %f - addr : 0x%x\n",
                                    __FUNCTION__, current - previous, iSize, recalcPts(dts), recalcPts(pts), (uint)pData);
  previous = current;
#endif

  {
    //printf("D   %f  %d\n", pts, iSize);
    if ((m_convert_bitstream) && (iSize))
    {
      // convert demuxer packet from bitstream to bytestream (AnnexB)
      if (m_converter->Convert(demuxer_content, demuxer_bytes))
      {
        demuxer_content = m_converter->GetConvertBuffer();
        demuxer_bytes = m_converter->GetConvertSize();
      }
      else
      {
        CLog::Log(LOGERROR,"%s - bitstream_convert error", __FUNCTION__);
        if (!m_vpuFrameBufferNum)
          m_convert_bitstream = false;
      }
    }

    inData.nSize = demuxer_bytes;
    inData.pPhyAddr = NULL;
    inData.pVirAddr = demuxer_content;
    if ((m_decOpenParam.CodecFormat == VPU_V_MPEG2) ||
        (m_decOpenParam.CodecFormat == VPU_V_VC1_AP)||
        (m_decOpenParam.CodecFormat == VPU_V_XVID)  ||
        (m_convert_bitstream && iSize && !m_vpuFrameBufferNum && decRet & (VPU_DEC_NO_ENOUGH_INBUF | VPU_DEC_OUTPUT_NODIS)))
      SetCodecParam(&inData, (unsigned char *)m_hints.extradata, m_hints.extrasize);
    else
      SetCodecParam(&inData, NULL, 0);

#ifdef IMX_PROFILE_BUFFERS
    static unsigned long long dec_time = 0;
#endif

    while (true) // Decode as long as the VPU consumes data
    {
#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
      before_dec = XbmcThreads::SystemClockMillis();
#endif
      ret = VPU_DecDecodeBuf(m_vpuHandle, &inData, &decRet);
#ifdef IMX_PROFILE_BUFFERS
      unsigned long long dec_single_call = XbmcThreads::SystemClockMillis()-before_dec;
      dec_time += dec_single_call;
#endif
#ifdef IMX_PROFILE
      CLog::Log(LOGDEBUG | LOGVIDEO, "%s - VPU ret %d dec 0x%x decode takes : %lld\n\n", __FUNCTION__, ret, decRet,  XbmcThreads::SystemClockMillis() - before_dec);
#endif

      if (ret != VPU_DEC_RET_SUCCESS && ret != VPU_DEC_RET_FAILURE_TIMEOUT)
      {
        CLog::Log(LOGERROR, "%s - VPU decode failed with error code %d (0x%x).\n", __FUNCTION__, ret, decRet);
        retStatus = VC_ERROR;
        break;
      }

      if (decRet & VPU_DEC_INIT_OK || decRet & VPU_DEC_RESOLUTION_CHANGED)
      // VPU decoding init OK : We can retrieve stream info
      {
        ret = VPU_DecGetInitialInfo(m_vpuHandle, &m_initInfo);
        if (ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU get initial info failed (%d).\n", __FUNCTION__, ret);
          break;
        }

        CLog::Log(LOGDEBUG, "%s - VPU Init Stream Info : %dx%d (interlaced : %d - Minframe : %d)"\
                  " - Align : %d bytes - crop : %d %d %d %d - Q16Ratio : %x\n", __FUNCTION__,
          m_initInfo.nPicWidth, m_initInfo.nPicHeight, m_initInfo.nInterlace, m_initInfo.nMinFrameBufferCount,
          m_initInfo.nAddressAlignment, m_initInfo.PicCropRect.nLeft, m_initInfo.PicCropRect.nTop,
          m_initInfo.PicCropRect.nRight, m_initInfo.PicCropRect.nBottom, m_initInfo.nQ16ShiftWidthDivHeightRatio);

        reset(false);

        if (!VpuFreeBuffers(false)   ||
           (!VpuAllocFrameBuffers()) ||
           (VPU_DEC_RET_SUCCESS != VPU_DecRegisterFrameBuffer(m_vpuHandle, m_vpuFrameBuffers, m_vpuFrameBufferNum)))
        {
          CLog::Log(LOGERROR, "%s - VPU error while registering frame buffers (%d).\n", __FUNCTION__, ret);
          return VC_REOPEN;
        }

        SetDrainMode((VpuDecInputType)IN_DECODER_SET);
        return VC_USERDATA;
      } //VPU_DEC_INIT_OK

      if (decRet & VPU_DEC_ONE_FRM_CONSUMED)
      {
        ret = VPU_DecGetConsumedFrameInfo(m_vpuHandle, &frameLengthInfo);
        if (ret != VPU_DEC_RET_SUCCESS)
          CLog::Log(LOGERROR, "%s - VPU error retireving info about consummed frame (%d).\n", __FUNCTION__, ret);

        if (frameLengthInfo.pFrame && !(decRet & VPU_DEC_OUTPUT_DROPPED))
          m_outputBuffers[frameLengthInfo.pFrame->pbufY]->SetPts(pts);
        if (m_dropRequest > 0)
          SetSkipMode(VPU_DEC_SKIPNONE);
      } //VPU_DEC_ONE_FRM_CONSUMED

      // VPU_DEC_OUTPUT_MOSAIC_DIS - According to libfslvpuwrap: If this flag
      // is set then the frame should be dropped. It is just returned to gather
      // decoder information but not for display
      if (decRet & (VPU_DEC_OUTPUT_DIS | VPU_DEC_OUTPUT_MOSAIC_DIS) && getOutputFrame(&m_frameInfo))
      // Frame ready to be displayed
      {
        // Some codecs (VC1?) lie about their frame size (mod 16). Adjust...
        m_frameInfo.pExtInfo->nFrmWidth  = (((m_frameInfo.pExtInfo->nFrmWidth) + 15) & ~15);
        m_frameInfo.pExtInfo->nFrmHeight = (((m_frameInfo.pExtInfo->nFrmHeight) + 15) & ~15);

        CDVDVideoCodecIMXBuffer *buffer = m_outputBuffers[m_frameInfo.pDisplayFrameBuf->pbufY];
        {
          /* quick & dirty fix to get proper timestamping for VP8 codec */
          if (m_decOpenParam.CodecFormat == VPU_V_VP8)
            buffer->SetPts(pts);

          buffer->Lock();
          buffer->SetDts(dts);
          buffer->Queue(&m_frameInfo);

#ifdef IMX_PROFILE_BUFFERS
          CLog::Log(LOGDEBUG | LOGVIDEO, "+D  %f  %lld\n", recalcPts(buffer->GetPts()), dec_time);
          dec_time = 0;
#endif

#ifdef TRACE_FRAMES
          CLog::Log(LOGDEBUG | LOGVIDEO, "+  %02d dts %f pts %f  (VPU)\n", buffer->m_idx, recalcPts(buffer->GetDts()), recalcPts(buffer->GetPts()));
#endif

          if (!m_usePTS)
          {
            buffer->SetPts(DVD_NOPTS_VALUE);
            buffer->SetDts(DVD_NOPTS_VALUE);
          }

#ifdef IMX_PROFILE_BUFFERS
          static unsigned long long lastD = 0;
          unsigned long long current = XbmcThreads::SystemClockMillis(), tmp;
          CLog::Log(LOGDEBUG | LOGVIDEO, "+V  %f  %lld\n", recalcPts(buffer->GetPts()), current-lastD);
          lastD = current;
#endif
          if (decRet & VPU_DEC_OUTPUT_DIS)
            buffer->iFlags = DVP_FLAG_ALLOCATED;

          m_currentBuffer = buffer;
          retStatus |= VC_PICTURE;
        }
      } //VPU_DEC_OUTPUT_DIS
      else if (decRet & (VPU_DEC_OUTPUT_REPEAT  |
                         VPU_DEC_OUTPUT_DROPPED |
                         VPU_DEC_SKIP))
      {
#ifdef TRACE_FRAMES
        CLog::Log(LOGDEBUG | LOGVIDEO, "%s - Frame drop/repeat/skip (0x%x).\n", __FUNCTION__, decRet);
#endif
        m_dropped++;
        if (decRet & VPU_DEC_SKIP)
          m_dropped++;
        retStatus |= VC_USERDATA;
      }

      if (decRet & VPU_DEC_NO_ENOUGH_INBUF)
      {
        if (iSize)
          SetSkipMode(VPU_DEC_SKIPNONE);
        break;
      }

      if (decRet & VPU_DEC_FLUSH)
        retStatus |= VC_FLUSHED;

      if (decRet & VPU_DEC_NO_ENOUGH_BUF)
      {
        retStatus |= VC_USERDATA;
        Sleep(5);
      }

      break;
    } // Decode loop
  } //(pData && iSize)

  if (!(retStatus & VC_USERDATA) && m_drainMode != VPU_DEC_IN_DRAIN)
    retStatus |= VC_BUFFER;

  if (m_drainMode == IN_DECODER_SET)
    SetDrainMode(VPU_DEC_IN_NORMAL);
#ifdef IMX_PROFILE
  CLog::Log(LOGDEBUG | LOGVIDEO, "%s - returns %x - duration %lld extinf %d\n", __FUNCTION__, retStatus, XbmcThreads::SystemClockMillis() - previous, (int)m_frameInfo.pExtInfo);
#endif
  return retStatus;
}

void CDVDVideoCodecIMX::reset(bool dispose)
{
  if (m_vpuFrameBufferNum && dispose)
    SetSkipMode(VPU_DEC_ISEARCH);

  if (dispose)
    CLog::Log(LOGDEBUG | LOGVIDEO, "%s - called\n", __FUNCTION__);

  // Invalidate all buffers
  for(auto buf : m_outputBuffers)
    buf.second->ReleaseFramebuffer(&m_vpuHandle);
  m_recycleBuffers.clear();

  if (!dispose)
    return;

  if (!m_vpuHandle || !m_vpuFrameBufferNum)
    return;

  // Flush VPU
  int ret = VPU_DecFlushAll(m_vpuHandle);
  if (ret != VPU_DEC_RET_SUCCESS)
    CLog::Log(LOGERROR, "%s - VPU flush failed with error code %d.\n", __FUNCTION__, ret);
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

bool CDVDVideoCodecIMX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
#ifdef IMX_PROFILE
  static unsigned int previous = 0;
  unsigned int current;

  current = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "%s  tm:%03d : Interlaced %d\n", __FUNCTION__, current - previous, m_initInfo.nInterlace);
  previous = current;
#endif

  pDvdVideoPicture->iFlags = m_currentBuffer->iFlags;

  if (m_initInfo.nInterlace)
  {
    if (m_currentBuffer->GetFieldType() == VPU_FIELD_NONE && m_warnOnce && m_currentBuffer->iFlags & DVP_FLAG_ALLOCATED)
    {
      m_warnOnce = false;
      CLog::Log(LOGWARNING, "Interlaced content reported by VPU, but full frames detected - Please turn off deinterlacing manually.");
    }
    else if (m_currentBuffer->GetFieldType() == VPU_FIELD_TB || m_currentBuffer->GetFieldType() == VPU_FIELD_TOP)
      pDvdVideoPicture->iFlags |= DVP_FLAG_TOP_FIELD_FIRST;

    pDvdVideoPicture->iFlags |= DVP_FLAG_INTERLACED;
  }

  pDvdVideoPicture->format = RENDER_FMT_IMXMAP;
  pDvdVideoPicture->iWidth = m_frameInfo.pExtInfo->FrmCropRect.nRight - m_frameInfo.pExtInfo->FrmCropRect.nLeft;
  pDvdVideoPicture->iHeight = m_frameInfo.pExtInfo->FrmCropRect.nBottom - m_frameInfo.pExtInfo->FrmCropRect.nTop;

  pDvdVideoPicture->iDisplayWidth = ((pDvdVideoPicture->iWidth * m_frameInfo.pExtInfo->nQ16ShiftWidthDivHeightRatio) + 32767) >> 16;
  pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;

  // Current buffer is locked already -> hot potato
  pDvdVideoPicture->pts = m_currentBuffer->GetPts();
  pDvdVideoPicture->dts = m_currentBuffer->GetDts();

#ifdef DVD_CODEC_CTRL_DROP
  if (m_codecControlFlags & DVD_CODEC_CTRL_DROP)
    m_currentBuffer->Release();
  else
#endif
  if (m_currentBuffer->iFlags & DVP_FLAG_ALLOCATED)
    pDvdVideoPicture->IMXBuffer = m_currentBuffer;
  else if (m_currentBuffer->iFlags & DVP_FLAG_DROPPED)
    SAFE_RELEASE(m_currentBuffer);

  return true;
}

void CDVDVideoCodecIMX::SetDropState(bool bDrop)
{
  // We are fast enough to continue to really decode every frames
  // and avoid artefacts...
  // (Of course these frames won't be rendered but only decoded)
  if (m_dropRequest < 1 && m_vpuFrameBufferNum && m_speed != DVD_PLAYSPEED_PAUSE)
    SetSkipMode((VpuDecSkipMode)(-1*bDrop));
}

/*******************************************/
#ifdef TRACE_FRAMES
CDVDVideoCodecIMXBuffer::CDVDVideoCodecIMXBuffer(int idx)
  : m_idx(idx)
  ,
#else
CDVDVideoCodecIMXBuffer::CDVDVideoCodecIMXBuffer()
  :
#endif
    m_pts(DVD_NOPTS_VALUE)
  , m_dts(DVD_NOPTS_VALUE)
  , m_fieldType(VPU_FIELD_NONE)
  , m_frameBuffer(NULL)
  , iFlags(DVP_FLAG_DROPPED)
{
  Lock();
}

void CDVDVideoCodecIMXBuffer::Lock()
{
#ifdef TRACE_FRAMES
  long count = AtomicIncrement(&m_iRefs);
  CLog::Log(LOGDEBUG | LOGVIDEO, "R+ %02d  -  ref : %ld  (VPU)\n", m_idx, count);
#else
  AtomicIncrement(&m_iRefs);
#endif
}

long CDVDVideoCodecIMXBuffer::Release()
{
  long count = AtomicDecrement(&m_iRefs);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG | LOGVIDEO, "R- %02d  -  ref : %ld  (VPU)\n", m_idx, count);
#endif
  if (count == 1)
  {
    // If count drops to 1 then the only reference is being held by the codec
    // that it can be released in the next Decode call.
    m_recycleBuffers.insert_after(m_recycleBuffers.before_begin(), this);
#ifdef TRACE_FRAMES
    CLog::Log(LOGDEBUG | LOGVIDEO, "R  %02d  (VPU)\n", m_idx);
#endif
  }
  else if (count == 0)
  {
    delete this;
  }

  return count;
}

void CDVDVideoCodecIMXBuffer::Queue(VpuDecOutFrameInfo *frameInfo)
{
  // No lock necessary because at this time there is definitely no
  // thread still holding a reference
  m_frameBuffer = frameInfo->pDisplayFrameBuf;

#ifdef IMX_INPUT_FORMAT_I420
  iFormat     = _4CC('I', '4', '2', '0');
#else
  iFormat     = _4CC('N', 'V', '1', '2');
#endif
  iWidth      = frameInfo->pExtInfo->nFrmWidth;
  iHeight     = frameInfo->pExtInfo->nFrmHeight;
  pVirtAddr   = m_frameBuffer->pbufVirtY;
  pPhysAddr   = (int)m_frameBuffer->pbufY;
  m_fieldType = frameInfo->eFieldType;
}

VpuDecRetCode CDVDVideoCodecIMXBuffer::ReleaseFramebuffer(VpuDecHandle *handle)
{
  // Again no lock required because this is only issued after the last
  // external reference was released
  VpuDecRetCode ret = VPU_DEC_RET_SUCCESS;

  m_renderLock.lock();
  if(m_frameBuffer)
  {
    ret = VPU_DecOutFrameDisplayed(*handle, m_frameBuffer);
    if(ret != VPU_DEC_RET_SUCCESS)
      CLog::Log(LOGERROR, "%s: vpu clear frame display failure: ret=%d \r\n",__FUNCTION__,ret);
    else
      m_frameBuffer = NULL;
  }
  m_renderLock.unlock();

#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG | LOGVIDEO, "-  %02d  (VPU) | ret: %d\n", m_idx, ret);
#endif
  SetPts(DVD_NOPTS_VALUE);
  iFlags = DVP_FLAG_DROPPED;
  return ret;
}

CDVDVideoCodecIMXBuffer::~CDVDVideoCodecIMXBuffer()
{
  assert(m_iRefs == 0);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG | LOGVIDEO, "~  %02d  (VPU)\n", m_idx);
#endif
}

CIMXContext::CIMXContext()
  : CThread("iMX IPU")
  , m_fbHandle(0)
  , m_fbCurrentPage(0)
  , m_fbPhysAddr(0)
  , m_fbVirtAddr(NULL)
  , m_ipuHandle(0)
  , m_vsync(true)
  , m_pageCrops(NULL)
  , m_bFbIsConfigured(false)
  , m_g2dHandle(NULL)
  , m_bufferCapture(NULL)
  , m_deviceName("/dev/fb1")
{
  m_pageCrops = new CRectInt[m_fbPages];
  CLog::Log(LOGDEBUG | LOGVIDEO, "iMX : Allocated %d render buffers\n", m_fbPages);

  SetBlitRects(CRectInt(), CRectInt());

  g2dOpenDevices();
  // Start the ipu thread
  Create();
}

CIMXContext::~CIMXContext()
{
  Stop(false);
  Dispose();
  CloseDevices();
  g2dCloseDevices();
}

bool CIMXContext::GetFBInfo(const std::string &fbdev, struct fb_var_screeninfo *fbVar)
{
  int fb = open(fbdev.c_str(), O_RDONLY, 0);
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
    munmap(m_fbVirtAddr, m_fbPhysSize);
    m_fbVirtAddr = NULL;
    m_fbPhysAddr = 0;
  }
  else if (fb_fix)
  {
    m_fbLineLength = fb_fix->line_length;
    m_fbPhysSize = fb_fix->smem_len;
    m_fbPageSize = m_fbLineLength * m_fbVar.yres_virtual / m_fbPages;
    m_fbPhysAddr = fb_fix->smem_start;
    m_fbVirtAddr = (uint8_t*)mmap(0, m_fbPhysSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbHandle, 0);
    m_fbCurrentPage = 0;
    Clear();
  }
}

bool CIMXContext::AdaptScreen()
{

  if(m_ipuHandle)
  {
    close(m_ipuHandle);
    m_ipuHandle = 0;
  }

  MemMap();

  if(!m_fbHandle && !OpenDevices())
    goto Err;

  struct fb_var_screeninfo fbVar;
  if (!GetFBInfo("/dev/fb0", &fbVar))
    goto Err;

  CLog::Log(LOGNOTICE, "iMX : Changing framebuffer parameters\n");

  if (SysfsUtils::Has("/sys/class/graphics/fb1/ntsc_mode"))
  {
    std::string ntsc;
    SysfsUtils::GetString("/sys/class/graphics/fb0/ntsc_mode", ntsc);
    SysfsUtils::SetInt("/sys/class/graphics/fb1/ntsc_mode", ntsc.find("active") != std::string::npos ? 1 : 0);
  }

  m_fbWidth = fbVar.xres;
  m_fbHeight = fbVar.yres;

  m_fbInterlaced = g_graphicsContext.GetResInfo().dwFlags & D3DPRESENTFLAG_INTERLACED;

  if (!GetFBInfo(m_deviceName, &m_fbVar))
    goto Err;

  m_fbVar.xoffset = 0;
  m_fbVar.yoffset = 0;

  if (m_currentFieldFmt)
  {
    m_fbVar.nonstd = _4CC('U', 'Y', 'V', 'Y');
    m_fbVar.bits_per_pixel = 16;
  }
  else
  {
    m_fbVar.nonstd = _4CC('R', 'G', 'B', '4');
    m_fbVar.bits_per_pixel = 32;
  }
  m_fbVar.activate = FB_ACTIVATE_NOW;
  m_fbVar.xres = m_fbWidth;
  m_fbVar.yres = m_fbHeight;

  if (m_fbInterlaced)
    m_fbVar.vmode |= FB_VMODE_INTERLACED;
  else
    m_fbVar.vmode &= ~FB_VMODE_INTERLACED;

  m_fbVar.yres_virtual = (m_fbVar.yres + 1) * m_fbPages;
  m_fbVar.xres_virtual = m_fbVar.xres;

  Blank();

  struct fb_fix_screeninfo fb_fix;

  if (ioctl(m_fbHandle, FBIOPUT_VSCREENINFO, &m_fbVar) == -1)
  {
    CLog::Log(LOGWARNING, "iMX : Failed to setup %s (%s)\n", m_deviceName.c_str(), strerror(errno));
    goto Err;
  }
  else if (ioctl(m_fbHandle, FBIOGET_FSCREENINFO, &fb_fix) == -1)
  {
    CLog::Log(LOGWARNING, "iMX : Failed to query fixed screen info at %s (%s)\n", m_deviceName.c_str(), strerror(errno));
    goto Err;
  }

  MemMap(&fb_fix);

  if (m_currentFieldFmt)
    m_ipuHandle = open("/dev/mxc_ipu", O_RDWR, 0);

  Unblank();

  return true;

Err:
  TaskRestart();
  return false;
}

void CIMXContext::OnResetDevice()
{
  CSingleLock lk(m_pageSwapLock);

  CLog::Log(LOGDEBUG, "iMX : %s - going to change screen parameters\n", __FUNCTION__);
  m_bFbIsConfigured = false;
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
  m_pageCrops = NULL;
}

bool CIMXContext::OpenDevices()
{
  m_fbHandle = open(m_deviceName.c_str(), O_RDWR, 0);
  if (m_fbHandle < 0)
  {
    m_fbHandle = 0;
    CLog::Log(LOGWARNING, "iMX : Failed to open framebuffer: %s\n", m_deviceName.c_str());
  }

  return m_fbHandle > 0;
}

inline
void CIMXContext::g2dOpenDevices()
{
  // open g2d here to ensure all g2d fucntions are called from the same thread
  if (unlikely(!m_g2dHandle))
  {
    if (g2d_open(&m_g2dHandle) != 0)
    {
      m_g2dHandle = NULL;
      CLog::Log(LOGERROR, "%s - Error while trying open G2D\n", __FUNCTION__);
    }
  }
}

void CIMXContext::CloseDevices()
{
  CLog::Log(LOGINFO, "iMX : Closing devices\n");

  if (m_fbHandle)
  {
    close(m_fbHandle);
    m_fbHandle = 0;
  }

  if (m_ipuHandle)
  {
    close(m_ipuHandle);
    m_ipuHandle = 0;
  }
}

void CIMXContext::g2dCloseDevices()
{
  // close g2d here to ensure all g2d fucntions are called from the same thread
  if (m_bufferCapture && !g2d_free(m_bufferCapture))
    m_bufferCapture = NULL;

  if (m_g2dHandle && !g2d_close(m_g2dHandle))
    m_g2dHandle = NULL;
}

bool CIMXContext::GetPageInfo(CIMXBuffer *info, int page)
{
  if (page < 0 || page >= m_fbPages)
    return false;

  info->iWidth    = m_fbWidth;
  info->iHeight   = m_fbHeight;
  info->iFormat   = m_fbVar.nonstd;
  info->pPhysAddr = m_fbPhysAddr + page*m_fbPageSize;
  info->pVirtAddr = m_fbVirtAddr + page*m_fbPageSize;

  return true;
}

bool CIMXContext::Blank()
{
  if (!m_fbHandle) return false;

  CSingleLock lk(m_pageSwapLock);
  if (CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF)
    SysfsUtils::SetInt("/sys/class/graphics/fb0/blank", 1);
  m_bFbIsConfigured = false;
  return ioctl(m_fbHandle, FBIOBLANK, FB_BLANK_NORMAL) == 0;
}

bool CIMXContext::Unblank()
{
  if (!m_fbHandle) return false;

  CSingleLock lk(m_pageSwapLock);
  if (CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF)
    SysfsUtils::SetInt("/sys/class/graphics/fb0/blank", 0);
  m_bFbIsConfigured = true;
  return ioctl(m_fbHandle, FBIOBLANK, FB_BLANK_UNBLANK) == 0;
}

bool CIMXContext::SetVSync(bool enable)
{
  m_vsync = enable;
  return true;
}

void CIMXContext::SetBlitRects(const CRect &srcRect, const CRect &dstRect)
{
  m_srcRect = srcRect;
  m_dstRect = dstRect;
}

inline
void CIMXContext::SetFieldData(uint8_t fieldFmt)
{
  if (m_bStop || !IsRunning())
    return;

  fieldFmt &= -!m_fbInterlaced;

  bool dr = IsDoubleRate();
  bool deint = !!m_currentFieldFmt;
  m_currentFieldFmt = fieldFmt;
  if (!!fieldFmt == deint && dr == IsDoubleRate())
    return;

  CLog::Log(LOGDEBUG, "iMX : Deinterlacing parameters changed (%s) %s\n", !!fieldFmt ? "active" : "not active", IsDoubleRate() ? "DR" : "");
  if (!!fieldFmt == deint)
    return;

  CSingleLock lk(m_pageSwapLock);
  m_bFbIsConfigured = false;
  AdaptScreen();
}

#define MASK1 (IPU_DEINTERLACE_RATE_FRAME1 | RENDER_FLAG_TOP)
#define MASK2 (IPU_DEINTERLACE_RATE_FRAME1 | RENDER_FLAG_BOT)
#define VAL1  MASK1
#define VAL2  RENDER_FLAG_BOT

inline
bool checkIPUStrideOffset(struct ipu_deinterlace *d)
{
  return ((d->field_fmt & MASK1) == VAL1) ||
         ((d->field_fmt & MASK2) == VAL2);
}

void CIMXContext::Blit(CIMXBuffer *source_p, CIMXBuffer *source, uint8_t fieldFmt, int page, CRect *dest)
{
  static int pg;

  if (likely(page == RENDER_TASK_AUTOPAGE))
    page = pg;
  else if (page == RENDER_TASK_CAPTURE)
    m_CaptureDone = false;
  else if (page < 0 && page >= m_fbPages)
    return;
#ifdef IMX_PROFILE_BUFFERS
  unsigned long long after = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "+B(%d)  %f\n", page, recalcPts(((CDVDVideoCodecIMXBuffer*)source)->GetPts()));
#endif

  pg = ++pg % m_fbPages;

  IPUTask ipu;

  SetFieldData(fieldFmt);
  PrepareTask(ipu, source_p, source, dest);

  ipu.page = page;
  ipu.shift = ipu.task.input.deinterlace.field_fmt ? checkIPUStrideOffset(&ipu.task.input.deinterlace) : 0;

  DoTask(ipu);

  if (unlikely(m_speed == DVD_PLAYSPEED_PAUSE))
    return;

  CSingleLock lk(m_pageSwapLock);
  if (ipu.task.output.width)
    m_input.push(ipu);
}

bool CIMXContext::PushCaptureTask(CIMXBuffer *source, CRect *dest)
{
  Blit(NULL, source, RENDER_TASK_CAPTURE, 0, dest);
  return true;
}

bool CIMXContext::ShowPage(int page, bool shift)
{
#ifdef IMX_PROFILE_BUFFERS
  unsigned long long pgstart = XbmcThreads::SystemClockMillis();
  unsigned long long pgflip;
#endif
  if (!m_fbHandle) return false;
  if (page < 0 || page >= m_fbPages) return false;
  if (!m_bFbIsConfigured) return false;

  // Protect page swapping from screen capturing that does read the current
  // front buffer. This is actually not done very frequently so the lock
  // does not hurt.
  CSingleLock lk(m_pageSwapLock);

  m_fbCurrentPage = page;
  m_fbVar.activate = FB_ACTIVATE_VBL;
  m_fbVar.yoffset = (m_fbVar.yres + 1) * page + !shift;
  if (ioctl(m_fbHandle, FBIOPAN_DISPLAY, &m_fbVar) < 0)
  {
    CLog::Log(LOGWARNING, "Panning failed: %s\n", strerror(errno));
    return false;
  }
#ifdef IMX_PROFILE_BUFFERS
  pgflip = XbmcThreads::SystemClockMillis();
#endif
  // Wait for flip
  if (m_vsync && ioctl(m_fbHandle, FBIO_WAITFORVSYNC, 0) < 0)
  {
    CLog::Log(LOGWARNING, "Vsync failed: %s\n", strerror(errno));
    return false;
  }
#ifdef IMX_PROFILE_BUFFERS
  unsigned long long pgend = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "NP(%d) - tot: %d - sync: %d - jflp: %d\n", m_fbCurrentPage, (int)(pgend - pgstart), (int)(pgend - pgflip), (int)(pgflip - pgstart));
#endif
  return true;
}

void CIMXContext::Clear(int page)
{
  if (!m_fbVirtAddr) return;

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


  if (m_fbVar.nonstd == _4CC('R', 'G', 'B', '4'))
    memset(tmp_buf, 0, bytes);
  else if (m_fbVar.nonstd == _4CC('U', 'Y', 'V', 'Y'))
  {
    int pixels = bytes / 2;
    for (int i = 0; i < pixels; ++i, tmp_buf += 2)
    {
      tmp_buf[0] = 128;
      tmp_buf[1] = 16;
    }
  }
  else
    CLog::Log(LOGERROR, "iMX Clear fb error : Unexpected format");
}

void CIMXContext::CaptureDisplay(unsigned char *buffer, int iWidth, int iHeight)
{
  if ((m_fbVar.nonstd != _4CC('R', 'G', 'B', '4')) &&
      (m_fbVar.nonstd != _4CC('U', 'Y', 'V', 'Y')))
  {
    CLog::Log(LOGWARNING, "iMX : Unknown screen capture format\n");
    return;
  }

  // Prevent page swaps
  CSingleLock lk(m_pageSwapLock);
  if (m_fbCurrentPage < 0 || m_fbCurrentPage >= m_fbPages)
  {
    CLog::Log(LOGWARNING, "iMX : Invalid page to capture\n");
    return;
  }
  unsigned char *display = m_fbVirtAddr + m_fbCurrentPage*m_fbPageSize;

  if (m_fbVar.nonstd == _4CC('R', 'G', 'B', '4'))
  {
    memcpy(buffer, display, iWidth * iHeight * 4);
    // BGRA is needed RGBA we get
    unsigned int size = iWidth * iHeight * 4;
    for (unsigned int i = 0; i < size; i += 4)
    {
       std::swap(buffer[i], buffer[i + 2]);
    }
  }
  else //_4CC('U', 'Y', 'V', 'Y')))
  {
    uint8_t r,g,b,a;
    int u, y0, v, y1;
    int iStride = m_fbWidth*2;
    int oStride = iWidth*4;

    int cy  =  1*(1 << 16);
    int cr1 =  1.40200*(1 << 16);
    int cr2 = -0.71414*(1 << 16);
    int cr3 =  0*(1 << 16);
    int cb1 =  0*(1 << 16);
    int cb2 = -0.34414*(1 << 16);
    int cb3 =  1.77200*(1 << 16);

    iWidth = std::min(iWidth/2, m_fbWidth/2);
    iHeight = std::min(iHeight, m_fbHeight);

    for (int y = 0; y < iHeight; ++y, display += iStride, buffer += oStride)
    {
      uint8_t *iLine = display;
      uint8_t *oLine = buffer;

      for (int x = 0; x < iWidth; ++x, iLine += 4, oLine += 8 )
      {
        u  = iLine[0]-128;
        y0 = iLine[1]-16;
        v  = iLine[2]-128;
        y1 = iLine[3]-16;

        a = 255-oLine[3];
        r = (cy*y0 + cb1*u + cr1*v) >> 16;
        g = (cy*y0 + cb2*u + cr2*v) >> 16;
        b = (cy*y0 + cb3*u + cr3*v) >> 16;

        oLine[0] = (b*a + oLine[0]*oLine[3])/255;
        oLine[1] = (g*a + oLine[1]*oLine[3])/255;
        oLine[2] = (r*a + oLine[2]*oLine[3])/255;
        oLine[3] = 255;

        a = 255-oLine[7];
        r = (cy*y0 + cb1*u + cr1*v) >> 16;
        g = (cy*y0 + cb2*u + cr2*v) >> 16;
        b = (cy*y0 + cb3*u + cr3*v) >> 16;

        oLine[4] = (b*a + oLine[4]*oLine[7])/255;
        oLine[5] = (g*a + oLine[5]*oLine[7])/255;
        oLine[6] = (r*a + oLine[6]*oLine[7])/255;
        oLine[7] = 255;
      }
    }
  }
}

void CIMXContext::WaitCapture()
{
}

void CIMXContext::PrepareTask(IPUTask &ipu, CIMXBuffer *source_p, CIMXBuffer *source,
                              CRect *dest)
{
  // Fill with zeros
  ipu.Zero();
  ipu.previous = source_p;
  ipu.current = source;

  CRect srcRect = m_srcRect;
  CRect dstRect;
  if (dest == NULL)
    dstRect = m_dstRect;
  else
    dstRect = *dest;

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
  iSrcRect.y2 = Align2((int)srcRect.y2,8);

  iDstRect.x1 = Align((int)dstRect.x1,8);
  iDstRect.y1 = Align((int)dstRect.y1,8);
  iDstRect.x2 = Align2((int)dstRect.x2,8);
  iDstRect.y2 = Align2((int)dstRect.y2,8);

  ipu.task.input.crop.pos.x  = iSrcRect.x1;
  ipu.task.input.crop.pos.y  = iSrcRect.y1;
  ipu.task.input.crop.w      = iSrcRect.Width();
  ipu.task.input.crop.h      = iSrcRect.Height();

  ipu.task.output.crop.pos.x = iDstRect.x1;
  ipu.task.output.crop.pos.y = iDstRect.y1;
  ipu.task.output.crop.w     = iDstRect.Width();
  ipu.task.output.crop.h     = iDstRect.Height();

  // If dest is set it means we do not want to blit to frame buffer
  // but to a capture buffer and we state this capture buffer dimensions
  if (dest)
  {
    // Populate partly output block
    ipu.task.output.crop.pos.x = 0;
    ipu.task.output.crop.pos.y = 0;
    ipu.task.output.crop.w     = iDstRect.Width();
    ipu.task.output.crop.h     = iDstRect.Height();
    ipu.task.output.width  = iDstRect.Width();
    ipu.task.output.height = iDstRect.Height();
  }
  else
  {
  // Setup deinterlacing if enabled
  if (m_currentFieldFmt)
  {
    ipu.task.input.deinterlace.enable = 1;
    /*
    if (source_p)
    {
      task.input.deinterlace.motion = MED_MOTION;
      task.input.paddr   = source_p->pPhysAddr;
      task.input.paddr_n = source->pPhysAddr;
    }
    else
    */
      ipu.task.input.deinterlace.motion = HIGH_MOTION;
    ipu.task.input.deinterlace.field_fmt = m_currentFieldFmt;
  }
  }
}

bool CIMXContext::DoTask(IPUTask &ipu)
{
  bool swapColors = false;

  // Clear page if cropping changes
  CRectInt dstRect(ipu.task.output.crop.pos.x, ipu.task.output.crop.pos.y,
                   ipu.task.output.crop.pos.x + ipu.task.output.crop.w,
                   ipu.task.output.crop.pos.y + ipu.task.output.crop.h);

  // Populate input block
  ipu.task.input.width   = ipu.current->iWidth;
  ipu.task.input.height  = ipu.current->iHeight;
  ipu.task.input.format  = ipu.current->iFormat;
  ipu.task.input.paddr   = ipu.current->pPhysAddr;

  // Populate output block if it has not already been filled
  if (ipu.task.output.width == 0)
  {
    ipu.task.output.width  = m_fbWidth;
    ipu.task.output.height = m_fbHeight;
    ipu.task.output.format = m_fbVar.nonstd;
    ipu.task.output.paddr  = m_fbPhysAddr + ipu.page*m_fbPageSize;

    if (m_pageCrops[ipu.page] != dstRect)
    {
      m_pageCrops[ipu.page] = dstRect;
      Clear(ipu.page);
    }
  }
  else
  {
    // If we have already set dest dimensions we want to use capture buffer
    // Note we allocate this capture buffer as late as this function because
    // all g2d functions have to be called from the same thread
    int size = ipu.task.output.width * ipu.task.output.height * 4;
    if ((m_bufferCapture) && (size != m_bufferCapture->buf_size))
    {
      if (g2d_free(m_bufferCapture))
        CLog::Log(LOGERROR, "iMX : Error while freeing capture buuffer\n");
      m_bufferCapture = NULL;
    }

    if (m_bufferCapture == NULL)
    {
      m_bufferCapture = g2d_alloc(size, 0);
      if (m_bufferCapture == NULL)
        CLog::Log(LOGERROR, "iMX : Error allocating capture buffer\n");
    }
    ipu.task.output.paddr = m_bufferCapture->buf_paddr;
    swapColors = true;
  }

  if ((ipu.task.input.crop.w <= 0) || (ipu.task.input.crop.h <= 0)
  ||  (ipu.task.output.crop.w <= 0) || (ipu.task.output.crop.h <= 0))
    return false;

#ifdef IMX_PROFILE_BUFFERS
  unsigned long long before = XbmcThreads::SystemClockMillis();
#endif

  if (ipu.task.input.deinterlace.enable)
  {
    //We really use IPU only if we have to deinterlace (using VDIC)
    int ret = IPU_CHECK_ERR_INPUT_CROP;
    while (ret > IPU_CHECK_ERR_MIN)
    {
        ret = ioctl(m_ipuHandle, IPU_CHECK_TASK, &ipu.task);
        switch (ret)
        {
        case IPU_CHECK_OK:
            break;
        case IPU_CHECK_ERR_SPLIT_INPUTW_OVER:
            ipu.task.input.crop.w -= 8;
            break;
        case IPU_CHECK_ERR_SPLIT_INPUTH_OVER:
            ipu.task.input.crop.h -= 8;
            break;
        case IPU_CHECK_ERR_SPLIT_OUTPUTW_OVER:
            ipu.task.output.crop.w -= 8;
            break;
        case IPU_CHECK_ERR_SPLIT_OUTPUTH_OVER:
            ipu.task.output.crop.h -= 8;
            break;
        // deinterlacing setup changing, m_ipuHandle is closed
        case -1:
            return true;
        default:
            CLog::Log(LOGWARNING, "iMX : unhandled IPU check error: %d\n", ret);
            return false;
        }
    }

    // Need to find another interface to protect ipu.current from disposing
    // in CDVDVideoCodecIMX::Dispose. CIMXContext must not have knowledge
    // about CDVDVideoCodecIMX.
    ipu.current->BeginRender();
    if (ipu.current->IsValid())
        ret = ioctl(m_ipuHandle, IPU_QUEUE_TASK, &ipu.task);
    else
        ret = 0;
#ifdef IMX_PROFILE_BUFFERS
  unsigned long long after = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "+P(%d)  %f  %d\n", ipu.page, recalcPts(((CDVDVideoCodecIMXBuffer*)ipu.current)->GetPts()), (int)(after-before));
#endif
    ipu.current->EndRender();
    if (ret < 0)
    {
        CLog::Log(LOGERROR, "IPU task failed: %s\n", strerror(errno));
        return false;
    }
  }
  else
  {
    // deinterlacing is not required, let's use g2d instead of IPU

    struct g2d_surface src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    ipu.current->BeginRender();
    if (ipu.current->IsValid())
    {
      if (ipu.current->iFormat == _4CC('I', '4', '2', '0'))
      {
        src.format = G2D_I420;
        src.planes[0] = ipu.current->pPhysAddr;
        src.planes[1] = src.planes[0] + Align(ipu.current->iWidth * ipu.current->iHeight, 64);
        src.planes[2] = src.planes[1] + Align((ipu.current->iWidth * ipu.current->iHeight) / 2, 64);
      }
      else //_4CC('N', 'V', '1', '2');
      {
        src.format = G2D_NV12;
        src.planes[0] = ipu.current->pPhysAddr;
        src.planes[1] =  src.planes[0] + Align(ipu.current->iWidth * ipu.current->iHeight, 64);
      }

      src.left = ipu.task.input.crop.pos.x;
      src.right = ipu.task.input.crop.w + src.left ;
      src.top  = ipu.task.input.crop.pos.y;
      src.bottom = ipu.task.input.crop.h + src.top;
      src.stride = ipu.current->iWidth;
      src.width  = ipu.current->iWidth;
      src.height = ipu.current->iHeight;
      src.rot = G2D_ROTATION_0;
      /*printf("src planes :%x -%x -%x \n",src.planes[0], src.planes[1], src.planes[2] );
      printf("src left %d right %d top %d bottom %d stride %d w : %d h %d rot : %d\n",
           src.left, src.right, src.top, src.bottom, src.stride, src.width, src.height, src.rot);*/

      dst.planes[0] = ipu.task.output.paddr;
      dst.left = ipu.task.output.crop.pos.x;
      dst.top = ipu.task.output.crop.pos.y;
      dst.right = ipu.task.output.crop.w + dst.left;
      dst.bottom = ipu.task.output.crop.h + dst.top;

      dst.stride = ipu.task.output.width;
      dst.width = ipu.task.output.width;
      dst.height = ipu.task.output.height;
      dst.rot = G2D_ROTATION_0;
      dst.format = swapColors ? G2D_BGRA8888 : G2D_RGBA8888;
      /*printf("dst planes :%x -%x -%x \n",dst.planes[0], dst.planes[1], dst.planes[2] );
      printf("dst left %d right %d top %d bottom %d stride %d w : %d h %d rot : %d format %d\n",
           dst.left, dst.right, dst.top, dst.bottom, dst.stride, dst.width, dst.height, dst.rot, dst.format);*/

      // Launch synchronous blit
      g2d_blit(m_g2dHandle, &src, &dst);
      g2d_finish(m_g2dHandle);
      if ((m_bufferCapture) && (ipu.task.output.paddr == m_bufferCapture->buf_paddr))
        m_CaptureDone = true;
    }
#ifdef IMX_PROFILE_BUFFERS
  unsigned long long after = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG | LOGVIDEO, "+P(%d)  %f  %d\n", ipu.page, recalcPts(((CDVDVideoCodecIMXBuffer*)ipu.current)->GetPts()), (int)(after-before));
#endif
    ipu.current->EndRender();
  }

  return true;
}

void CIMXContext::RendererAllowed(bool yes)
{
  if (IsRunning() == yes)
    return;
  CLog::Log(LOGNOTICE, "iMX : changing to %s", yes ? "enabled" : "disabled");
  if (yes)
    TaskRestart();
  else
    Stop(true);
}

void CIMXContext::OnStartup()
{
  OpenDevices();

  AdaptScreen();
  g_Windowing.Register(this);
  CLog::Log(LOGNOTICE, "iMX : IPU thread started");
}

void CIMXContext::OnExit()
{
  g_Windowing.Unregister(this);
  CLog::Log(LOGNOTICE, "iMX : IPU thread terminated");
}

void CIMXContext::Stop(bool bWait /*= true*/)
{
  if (!IsRunning())
    return;

  CThread::StopThread(false);
  m_input.signal_stop();
  Blank();
  if (bWait && IsRunning())
    CThread::StopThread(true);
}

void CIMXContext::Process()
{
  while (!m_bStop)
  {
    IPUTask task = m_input.pop();

    if (m_bStop)
      break;

    // Show back buffer
    ShowPage(task.page, task.shift);
  }

  m_input.for_each(callDone);
  return;
}
