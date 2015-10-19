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
#include "utils/Screen.h"
#include "guilib/GraphicContext.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "xbmc/utils/SysfsUtils.h"
#include "messaging/ApplicationMessenger.h"
#include "Application.h"
#include <fstream>

using namespace PERIPHERALS;
using namespace KODI::MESSAGING;

int CPeripheralVideo::m_cableState = 0;

CPeripheralVideo::CPeripheralVideo(const PeripheralScanResult& scanResult)
  : CPeripheral(scanResult)
  , m_timer(this)
{
  m_features.push_back(FEATURE_CABLESTATE);
}

CPeripheralVideo::~CPeripheralVideo()
{
}

void CPeripheralVideo::OnDeviceChanged(int state)
{
  if (!GetSettingBool("pass_events"))
    return;

  CLog::Log(LOGDEBUG, "%s - state %s over %s, timer %s", __FUNCTION__, stateToStr(state), stateToStr(m_cableState), !m_timer.IsRunning() ? "will be started" : "is already running");

  m_cableState = state;

  if (m_timer.IsRunning())
    m_timer.Restart();
  else
    m_timer.Start(5000);
}

bool CPeripheralVideo::IsQuantRangeLimited()
{
#ifdef HAS_IMXVPU
  std::string value;
  std::string from = "/sys/devices/soc0/soc/20e0000.hdmi_video/rgb_quant_range";

  std::ifstream file(from);
  if (!file.is_open())
  {
    from = "/sys/devices/soc0/soc.1/20e0000.hdmi_video/rgb_quant_range";
  }
  file.close();

  SysfsUtils::GetString(from, value);
  if (value.find("limited") != std::string::npos)
    return true;
#endif
  return false;
}

void CPeripheralVideo::OnSettingChanged(const std::string &strChangedSetting)
{
  bool configSet = false;

  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "VIDEO", g_localizeStrings.Get(configSet ? 36023 : 36024));
}

bool CPeripheralVideo::InitialiseFeature(const PeripheralFeature feature)
{
  return CPeripheral::InitialiseFeature(feature);
}

void CPeripheralVideo::OnTimeout()
{
  switch (m_cableState)
  {
    case CABLE_CONNECTED:
      g_screen.SetOn();

      CSettings::Get().SetBool("videoscreen.limitedrange", IsQuantRangeLimited());
      if (CSettings::Get().GetBool("videoscreen.updateresolutions"))
      {
        CApplicationMessenger::Get().SetupDisplayReconfigure();
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "VIDEO", g_localizeStrings.Get(13288));
      }

      break;
    case CABLE_DISCONNECTED:
      g_screen.SetOff();

    default:
      ;
  }
}
