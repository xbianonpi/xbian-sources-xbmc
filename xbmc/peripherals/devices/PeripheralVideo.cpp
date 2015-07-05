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

#include "PeripheralVideo.h"
#include "utils/log.h"
#include "windowing/WindowingFactory.h"
#include "guilib/GraphicContext.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "ApplicationMessenger.h"

using namespace PERIPHERALS;

CPeripheralVideo::CPeripheralVideo(const PeripheralScanResult& scanResult)
  : CPeripheral(scanResult)
  , m_cableState(0)
  , m_timer(this)
{
  m_features.push_back(FEATURE_CABLESTATE);
}

void CPeripheralVideo::OnDeviceChanged(int state)
{
  if (!GetSettingBool("enabled"))
    return;

  m_cableState = state;

  switch (m_cableState) {
    case CABLE_CONNECTED:
      if (m_timer.IsRunning())
        m_timer.Restart();
      else
        m_timer.Start(5000);
      break;
    case CABLE_DISCONNECTED:
      m_timer.Stop();
    default:
      ;
  }
}

void CPeripheralVideo::OnTimeout()
{
  if (!GetSettingBool("probe_resolutions"))
    return;

  CApplicationMessenger::Get().SetupDisplayReconfigure();
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "VIDEO", g_localizeStrings.Get(13289));
}
