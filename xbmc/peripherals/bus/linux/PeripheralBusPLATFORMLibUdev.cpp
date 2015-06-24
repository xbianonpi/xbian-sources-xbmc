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

#include "PeripheralBusPLATFORMLibUdev.h"
#include "peripherals/Peripherals.h"
extern "C" {
#include <libudev.h>
}
#include <poll.h>
#include "utils/log.h"

using namespace PERIPHERALS;

CPeripheralBusPLATFORM::CPeripheralBusPLATFORM(CPeripherals *manager, const std::string &threadname, PeripheralBusType type) :
    CPeripheralBusUSB(manager, "PeripBusPLATFORMUdev", type)
{
  udev_monitor_filter_add_match_subsystem_devtype(m_udevMon, "platform", NULL);
  udev_monitor_filter_update(m_udevMon);
}

void CPeripheralBusPLATFORM::Clear(void)
{
  StopThread(false);
}

bool CPeripheralBusPLATFORM::PerformDeviceScan(PeripheralScanResults &results)
{
  struct udev_enumerate *enumerate;
  struct udev_list_entry *devices, *dev_list_entry;
  struct udev_device *dev(NULL);
  enumerate = udev_enumerate_new(m_udev);
  udev_enumerate_scan_devices(enumerate);
  devices = udev_enumerate_get_list_entry(enumerate);

  bool bContinue(true);
  std::string strPath, t;
  udev_list_entry_foreach(dev_list_entry, devices)
  {
    strPath = udev_list_entry_get_name(dev_list_entry);
    dev = udev_device_new_from_syspath(m_udev, strPath.c_str());
    if (strPath.empty() || !dev)
      bContinue = false;

    if (bContinue)
    {
      bContinue = false;
      if (GetCableState(udev_device_get_syspath(dev)))
        bContinue = true;
    }

    if (bContinue)
    {
      PeripheralScanResult result(m_type);
      result.m_strLocation = udev_device_get_syspath(dev);
      result.m_iSequence   = GetNumberOfPeripheralsWithFeature(FEATURE_CABLESTATE);
      result.m_type        = PERIPHERAL_VIDEO;

      t = udev_device_get_sysattr_value(dev, "uevent");
      PeripheralTypeTranslator::UeventToName(t, result.m_strDeviceName);
      if (result.m_strDeviceName.empty())
        result.m_strDeviceName += "generic_video";

      result.m_iVendorId   = 0;
      result.m_iProductId  = 0;

      if (!results.ContainsResult(result))
        results.m_results.push_back(result);
    }

    bContinue = true;
  }
  /* Free the enumerator object */
  udev_enumerate_unref(enumerate);

  return true;
}

int CPeripheralBusPLATFORM::GetCableState(const std::string &strLocation)
{
  struct udev_device         *dev = udev_device_new_from_syspath(m_udev, strLocation.c_str());
  std::string                 files[] = { "cable_state", "status", "state" };
  std::vector<std::string>    cableState(files, files + 3);

  std::string t;
  int        state = CABLE_UNKNOWN;

  if (!dev)
    return state;

  for (std::vector<std::string>::iterator f = cableState.begin() ; f != cableState.end(); ++f)
  {
    if (udev_device_get_sysattr_value(dev, f->c_str()))
      t = udev_device_get_sysattr_value(dev, f->c_str());

    if (!t.empty() && (t.find("connected") != std::string::npos || t.find("plugout") != std::string::npos))
      state = CABLE_DISCONNECTED;
    if (!t.empty() && (t.find("disconnected") != std::string::npos || t.find("plugin") != std::string::npos))
      state = CABLE_CONNECTED;

    if (state)
      break;
  }

  return state;
}

void CPeripheralBusPLATFORM::Process(void)
{
  while (!m_bStop)
    WaitForUpdate();

  m_bIsStarted = false;
}

void CPeripheralBusPLATFORM::OnDeviceChanged(const std::string &strLocation)
{
  CSingleLock lock(m_critSection);
  CPeripheral *peripheral = GetPeripheral(strLocation);
  if (peripheral)
    peripheral->OnDeviceChanged(GetCableState(strLocation));
}
