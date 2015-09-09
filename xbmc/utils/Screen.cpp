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

#include "Application.h"
#include "utils/log.h"
#include "settings/Settings.h"
#include "video/VideoReferenceClock.h"
#include "guilib/GUIWindowManager.h"
#include "utils/Screen.h"
#include "windowing/WindowingFactory.h"
#include "threads/SingleLock.h"
#ifdef HAS_IMXVPU
#include "cores/dvdplayer/DVDCodecs/Video/DVDVideoCodecIMX.h"
#endif

using namespace ANNOUNCEMENT;

CScreen g_screen;

CScreen::CScreen()
  : m_state(false)
  , m_changedBlank(false)
  , m_timer(this)
{
  CAnnouncementManager::Get().AddAnnouncer(this);
}

CScreen::~CScreen()
{
  CAnnouncementManager::Get().RemoveAnnouncer(this);
}

void CScreen::Announce(AnnouncementFlag flag, const char *sender, const char *message, const CVariant &data)
{
  if (flag == System && !strcmp(sender, "xbmc") && !strcmp(message, "OnQuit"))
    g_application.SetRenderGUI(true);
  else if (flag == GUI && !strcmp(sender, "xbmc") && !strcmp(message, "OnScreensaverDeactivated"))
    g_screen.SetOn();
  else if (flag == GUI && !strcmp(sender, "xbmc") && !strcmp(message, "OnScreensaverActivated") && CSettings::Get().GetString("screensaver.mode") == "screensaver.xbmc.builtin.black")
    g_screen.SetOff();
#ifdef HAS_IMXVPU
  else if (flag == Player && !strcmp(sender, "xbmc") && !strcmp(message, "OnPlay"))
    g_VideoReferenceClock.Start();
  else if (flag == Player && !strcmp(sender, "xbmc") && !strcmp(message, "OnPause"))
    g_VideoReferenceClock.Stop();
#endif
}

void CScreen::ScreenPowerOff(bool doBlank)
{
  if (!doBlank || !CSettings::Get().GetBool("videoscreen.blankcurrent"))
    return;

#ifdef HAS_IMXVPU
  m_changedBlank = true;
  // calling CIMXContext::Blank() tells CodecIMX
  // fb1 is not ready
  g_IMXContext.Blank();
  g_Windowing.Hide();
#endif
}

void CScreen::ScreenPowerOn(bool doBlank)
{
  if (!doBlank || !m_changedBlank)
    return;

#ifdef HAS_IMXVPU
  m_changedBlank = false;
  g_Windowing.Show();
  g_IMXContext.Unblank();
#endif
}

void CScreen::SetState(bool state, bool doBlank)
{
  if (g_application.m_bStop)
  {
    g_application.SetRenderGUI(true);
    return;
  }

  CSingleLock lock(m_critSection);
  if (state == m_state)
    return;

  CLog::Log(LOGDEBUG, "%s - set standby %d, screensaver is %s", __FUNCTION__, (int)state, g_application.IsInScreenSaver() ? "active" : "inactive");

  m_state = state;

  switch (state)
  {
  case true:

    g_VideoReferenceClock.Stop();
    if (!g_application.IsInScreenSaver())
      g_application.ActivateScreenSaver();
    ScreenPowerOff(doBlank);

    break;
  case false:
    g_application.WakeUpScreenSaverAndDPMS();
    ScreenPowerOn(doBlank);
    if (g_application.m_pPlayer->IsPlayingVideo())
      g_VideoReferenceClock.Start();

  default:
    ;
  }

  if (!state)
    OnTimeout();
  else if (m_timer.IsRunning())
    m_timer.Restart();
  else
    m_timer.Start(2500);
}

void CScreen::OnTimeout()
{
  g_application.SetRenderGUI(!m_state);
}
