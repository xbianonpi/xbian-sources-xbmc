/*
 *  Copyright (C) 2014-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayer.h"
#include "ServiceBroker.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "cores/omxplayer/OMXPlayerAudio.h"
#include "cores/omxplayer/OMXPlayerVideo.h"
#include "threads/SystemClock.h"

#ifndef FF_BUG_GMC_UNSUPPORTED
#define FF_BUG_GMC_UNSUPPORTED 0
#endif

#define PREDICATE_RETURN(lh, rh) \
  do { \
    if((lh) != (rh)) \
      return (lh) > (rh); \
  } while(0)

bool OMXPlayerUnsuitable(bool m_HasVideo, bool m_HasAudio, CDVDDemux* m_pDemuxer, std::shared_ptr<CDVDInputStream> m_pInputStream, CSelectionStreams &m_SelectionStreams)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  // if no OMXPlayer acceleration then it is not suitable
  if (!settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEOMXPLAYER))
    return true;
  // if no MMAL acceleration stick with omxplayer regardless
  if (!settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEMMAL))
    return false;

  // omxplayer only handles Pi sink
  const std::string audioDevice = settings->GetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE);
  if (audioDevice != "PI:Analogue" &&
      audioDevice != "PI:HDMI" &&
      audioDevice != "PI:Both" &&
      audioDevice != "Default")
  {
    CLog::Log(LOGNOTICE, "%s OMXPlayer unsuitable due to audio sink", __func__);
    return true;
  }
  // omxplayer doesn't handle ac3 transcode
  if (settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE))
  {
    CLog::Log(LOGNOTICE, "%s OMXPlayer unsuitable due to ac3transcode", __func__);
    return true;
  }
  if (m_pDemuxer)
  {
    // find video stream
    int num_supported = 0, num_unsupported = 0;
    AVCodecID codec = AV_CODEC_ID_NONE;
    for (const auto &it : m_SelectionStreams.Get(STREAM_VIDEO))
    {
      CDemuxStream *stream = m_pDemuxer->GetStream(it.demuxerId, it.id);
      if(!stream || stream->disabled || stream->flags & AV_DISPOSITION_ATTACHED_PIC)
        continue;
      CDVDStreamInfo hint(*stream, true);

      bool supported = false;
      if (hint.workaround_bugs & FF_BUG_GMC_UNSUPPORTED)
        ;
      else if ((hint.codec == AV_CODEC_ID_MPEG1VIDEO || hint.codec == AV_CODEC_ID_MPEG2VIDEO) && g_RBP.GetCodecMpg2())
        supported = true;
      else if ((hint.codec == AV_CODEC_ID_VC1 || hint.codec == AV_CODEC_ID_WMV3) && g_RBP.GetCodecWvc1())
        supported = true;
      else if (hint.codec == AV_CODEC_ID_H264 || hint.codec == AV_CODEC_ID_MPEG4 || hint.codec == AV_CODEC_ID_H263 ||
          hint.codec == AV_CODEC_ID_VP6 || hint.codec == AV_CODEC_ID_VP6F || hint.codec == AV_CODEC_ID_VP6A || hint.codec == AV_CODEC_ID_VP8 ||
          hint.codec == AV_CODEC_ID_THEORA || hint.codec == AV_CODEC_ID_MJPEG || hint.codec == AV_CODEC_ID_MJPEGB)
        supported = true;
      codec = hint.codec;
      if (supported)
        num_supported++;
      else
        num_unsupported++;
    }
    if (num_unsupported > 0 && num_supported == 0)
    {
      CLog::Log(LOGNOTICE, "%s OMXPlayer unsuitable due to video codec (%x:%d/%d)", __func__, codec, num_supported, num_unsupported);
      return true;
    }
  }
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    CLog::Log(LOGNOTICE, "%s OMXPlayer unsuitable due to dvd menus", __func__);
    return true;
  }
  return false;
}

bool OMXDoProcessing(struct SOmxPlayerState &m_OmxPlayerState, int m_playSpeed, IDVDStreamPlayerVideo *m_VideoPlayerVideo, IDVDStreamPlayerAudio *m_VideoPlayerAudio,
                     CCurrentStream m_CurrentAudio, CCurrentStream m_CurrentVideo, bool m_HasVideo, bool m_HasAudio, CProcessInfo &processInfo)
{
  bool reopen_stream = false;
  unsigned int now = XbmcThreads::SystemClockMillis();
  if (m_OmxPlayerState.last_check_time == 0.0 || m_OmxPlayerState.last_check_time + 20 <= now)
  {
    m_OmxPlayerState.last_check_time = now;
    m_OmxPlayerState.stamp = m_OmxPlayerState.av_clock.OMXMediaTime();
    const bool m_Pause = m_playSpeed == DVD_PLAYSPEED_PAUSE;
    const bool not_accepts_data = (!m_VideoPlayerAudio->AcceptsData() && m_HasAudio) ||
        (!m_VideoPlayerVideo->AcceptsData() && m_HasVideo);
    /* when the video/audio fifos are low, we pause clock, when high we resume */
    double audio_pts = floor(m_VideoPlayerAudio->GetCurrentPts());
    double video_pts = floor(m_VideoPlayerVideo->GetCurrentPts());

    float audio_fifo = audio_pts / DVD_TIME_BASE - m_OmxPlayerState.stamp * 1e-6;
    float video_fifo = video_pts / DVD_TIME_BASE - m_OmxPlayerState.stamp * 1e-6;
    float threshold = 0.1f;
    bool audio_fifo_low = false, video_fifo_low = false, audio_fifo_high = false, video_fifo_high = false;

    if (m_OmxPlayerState.interlace_method == VS_INTERLACEMETHOD_MAX)
      m_OmxPlayerState.interlace_method = processInfo.GetVideoSettings().m_InterlaceMethod;

    // if deinterlace setting has changed, we should close and open video
    if (m_OmxPlayerState.interlace_method != processInfo.GetVideoSettings().m_InterlaceMethod)
    {
      CLog::Log(LOGNOTICE, "%s - Reopen stream due to interlace change (%d,%d)", __FUNCTION__,
        m_OmxPlayerState.interlace_method, processInfo.GetVideoSettings().m_InterlaceMethod);

      m_OmxPlayerState.interlace_method    = processInfo.GetVideoSettings().m_InterlaceMethod;
      reopen_stream = true;
    }

    if (audio_pts != DVD_NOPTS_VALUE)
    {
      audio_fifo_low = m_HasAudio && audio_fifo < threshold;
      audio_fifo_high = audio_pts != DVD_NOPTS_VALUE && audio_fifo >= m_OmxPlayerState.threshold;
    }
    if (video_pts != DVD_NOPTS_VALUE)
    {
      video_fifo_low = m_HasVideo && video_fifo < threshold;
      video_fifo_high = video_pts != DVD_NOPTS_VALUE && video_fifo >= m_OmxPlayerState.threshold;
    }
    if (!m_HasAudio && m_HasVideo)
      audio_fifo_high = true;
    if (!m_HasVideo && m_HasAudio)
      video_fifo_high = true;

    #ifdef _DEBUG
    CLog::Log(LOGDEBUG, "%s::%s M:%.6f-%.6f (A:%.6f V:%.6f) PEF:%d%d%d S:%.2f A:%.2f V:%.2f/T:%.2f (A:%d%d V:%d%d) A:%d%% V:%d%%", "CVideoPlayer", __FUNCTION__,
      m_OmxPlayerState.stamp*1e-6, m_OmxPlayerState.av_clock.OMXClockAdjustment()*1e-6, audio_pts*1e-6, video_pts*1e-6,
      m_OmxPlayerState.av_clock.OMXIsPaused(), m_OmxPlayerState.bOmxSentEOFs, not_accepts_data, m_playSpeed * (1.0f/DVD_PLAYSPEED_NORMAL),
      audio_pts == DVD_NOPTS_VALUE ? 0.0:audio_fifo, video_pts == DVD_NOPTS_VALUE ? 0.0:video_fifo, m_OmxPlayerState.threshold,
      audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high,
      m_VideoPlayerAudio->GetLevel(), 0);
    #endif

    if(!m_Pause && (m_OmxPlayerState.bOmxSentEOFs || not_accepts_data || (audio_fifo_high && video_fifo_high) || m_playSpeed != DVD_PLAYSPEED_NORMAL))
    {
      if (m_OmxPlayerState.av_clock.OMXIsPaused())
      {
        CLog::Log(LOGDEBUG, "%s::%s Resume %.2f,%.2f (A:%d%d V:%d%d) EOF:%d FULL:%d T:%.2f", "CVideoPlayer", __FUNCTION__, audio_fifo, video_fifo,
          audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high, m_OmxPlayerState.bOmxSentEOFs, not_accepts_data, m_OmxPlayerState.threshold);
        m_OmxPlayerState.av_clock.OMXResume();
      }
    }
    else if ((m_Pause || audio_fifo_low || video_fifo_low) && m_playSpeed == DVD_PLAYSPEED_NORMAL)
    {
      if (!m_OmxPlayerState.av_clock.OMXIsPaused())
      {
        if (!m_Pause)
          m_OmxPlayerState.threshold = std::min(2.0f*m_OmxPlayerState.threshold, 16.0f);
        CLog::Log(LOGDEBUG, "%s::%s Pause %.2f,%.2f (A:%d%d V:%d%d) EOF:%d FULL:%d T:%.2f", "CVideoPlayer", __FUNCTION__, audio_fifo, video_fifo,
          audio_fifo_low, audio_fifo_high, video_fifo_low, video_fifo_high, m_OmxPlayerState.bOmxSentEOFs, not_accepts_data, m_OmxPlayerState.threshold);
        m_OmxPlayerState.av_clock.OMXPause();
      }
    }
  }
  return reopen_stream;
}

bool OMXStillPlaying(bool waitVideo, bool waitAudio, bool eosVideo, bool eosAudio)
{
  // wait for omx components to finish
  if (waitVideo && !eosVideo)
    return true;
  if (waitAudio && !eosAudio)
    return true;
  return false;
}
