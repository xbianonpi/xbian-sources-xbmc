/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"

#include <memory>

extern "C"
{
#include <libavfilter/avfilter.h>
}

class CDVDVideoCodecDRMPRIME : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecDRMPRIME(CProcessInfo& processInfo);
  ~CDVDVideoCodecDRMPRIME() override;

  static CDVDVideoCodec* Create(CProcessInfo& processInfo);
  static void Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return m_name.c_str(); }
  unsigned GetAllowedReferences() override { return 5; }
  void SetCodecControl(int flags) override;

protected:
  void Drain();
  bool SetPictureParams(VideoPicture* pVideoPicture);
  void UpdateProcessInfo(struct AVCodecContext* avctx, const enum AVPixelFormat fmt);
  CDVDVideoCodec::VCReturn ProcessFilterIn();
  CDVDVideoCodec::VCReturn ProcessFilterOut();
  static enum AVPixelFormat GetFormat(struct AVCodecContext* avctx, const enum AVPixelFormat* fmt);
  static int GetBuffer(struct AVCodecContext* avctx, AVFrame* frame, int flags);
  bool FilterOpen(const std::string& filters, bool test);
  void FilterClose();
  void FilterTest();
  std::string GetFilterChain(bool interlaced);

  std::string m_name;
  std::string m_deintFilterName;
  std::string m_filters;
  int m_codecControlFlags = 0;
  CDVDStreamInfo m_hints;
  AVCodecContext* m_pCodecContext = nullptr;
  AVFrame* m_pFrame = nullptr;
  AVFrame* m_pFilterFrame = nullptr;
  AVFilterGraph* m_pFilterGraph = nullptr;
  AVFilterContext* m_pFilterIn = nullptr;
  AVFilterContext* m_pFilterOut = nullptr;
  std::shared_ptr<IVideoBufferPool> m_videoBufferPool;
};
