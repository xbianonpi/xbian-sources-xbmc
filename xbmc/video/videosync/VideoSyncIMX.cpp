/*
 *      Copyright (C) 2005-2014 Team XBMC
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

#include "system.h"

#if defined(HAS_IMXVPU)

#include "video/videosync/VideoSyncIMX.h"
#include "guilib/GraphicContext.h"
#include "windowing/WindowingFactory.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include <linux/mxcfb.h>
#include <linux/mxc_dcic.h>
#include <sys/ioctl.h>
#include "threads/Thread.h"

bool CVideoSyncIMX::Setup(PUPDATECLOCK func)
{
  struct fb_var_screeninfo screen_info;

  UpdateClock = func;
  m_abort = false;

  m_fddcic = open("/dev/mxc_dcic0", O_RDWR);
  if (m_fddcic < 0)
    return false;

  int fb0 = open("/dev/fb0", O_RDONLY | O_NONBLOCK);
  if (fb0 < 0)
    return false;

  bool bContinue = !ioctl(fb0, FBIOGET_VSCREENINFO, &screen_info);
  if (bContinue) {
    bContinue = !ioctl(m_fddcic, DCIC_IOC_CONFIG_DCIC, &screen_info.sync);

    if (bContinue)
      bContinue = !ioctl(m_fddcic, DCIC_IOC_START_VSYNC, 0);
  }

  close(fb0);
  if (!bContinue)
  {
    if (m_fddcic > 0)
      close(m_fddcic);
    return false;
  }

  g_Windowing.Register(this);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up IMX");
  return true;
}

void CVideoSyncIMX::Run(volatile bool& stop)
{
  unsigned long counter;
  unsigned long last = 0;
  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(CThread::GetCurrentThread()->GetPriority()+1);

  while (!stop && !m_abort)
  {
    read(m_fddcic, &counter, sizeof(unsigned long));
    uint64_t now = CurrentHostCounter();

    UpdateClock((unsigned int)(counter - last), now);
    last = counter;
  }
}

void CVideoSyncIMX::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up IMX");
  ioctl(m_fddcic, DCIC_IOC_STOP_VSYNC, 0);
  close(m_fddcic);
  g_Windowing.Unregister(this);
}

float CVideoSyncIMX::GetFps()
{
  RESOLUTION_INFO info = g_graphicsContext.GetResInfo();
  m_fps = info.fRefreshRate;
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: fps: %.3f", m_fps);
  return m_fps;
}

void CVideoSyncIMX::OnResetDevice()
{
  m_abort = true;
}

void CVideoSyncIMX::RefreshChanged()
{
  if (m_fps != g_graphicsContext.GetFPS())
    m_abort = true;
}

#endif
