#pragma once
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

#include "interfaces/AnnouncementManager.h"
#include "threads/CriticalSection.h"
#include "threads/Timer.h"

class CScreen : public ANNOUNCEMENT::IAnnouncer, protected ITimerCallback
{
public:
  CScreen();
  ~CScreen();

  void         Announce(ANNOUNCEMENT::AnnouncementFlag flag, const char *sender, const char *message, const CVariant &data);

  void         SetOff(bool doBlank = true) { SetState(true, doBlank); };
  void         SetOn() { SetState(false); };
  void         SwitchState() { SetState(!m_state, m_changedBlank); };

  bool         GetScreenState() { return m_state; }

protected:
  void         OnTimeout();

private:
  void         SetState(bool status, bool doBlank = true);
  void         ScreenPowerOn(bool doBlank);
  void         ScreenPowerOff(bool doBlank);

  bool             m_state;
  bool             m_changedBlank;
  CCriticalSection m_critSection;
  CTimer           m_timer;
};

extern CScreen g_screen;
