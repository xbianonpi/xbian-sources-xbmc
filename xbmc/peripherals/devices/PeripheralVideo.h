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

#include "Peripheral.h"
#include "peripherals/Peripherals.h"
#include "threads/Timer.h"

namespace PERIPHERALS
{
  class CPeripheralVideo : public CPeripheral, protected ITimerCallback
  {
  public:
    CPeripheralVideo(const PeripheralScanResult& scanResult);
    virtual ~CPeripheralVideo(void);

    virtual void OnDeviceChanged(int state);
    virtual void OnTimeout();

    void OnSettingChanged(const std::string &strChangedSetting);
    bool InitialiseFeature(const PeripheralFeature feature);

    static bool IsQuantRangeLimited();

    static const char *stateToStr(const int state)
    {
      switch (state)
      {
      case CABLE_CONNECTED:
        return "connected";
      case CABLE_DISCONNECTED:
        return "disconnected";
      default:
        return "unknown";
      }
    }

  public:
    static int                    m_cableState;
  protected:
    CTimer                        m_timer;

  };
}

