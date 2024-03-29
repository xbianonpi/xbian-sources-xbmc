/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/DispResource.h"
#include "windowing/VideoSync.h"

class CVideoSyncPi : public CVideoSync, IDispResource
{
public:
  CVideoSyncPi(CVideoReferenceClock* clock) : CVideoSync(clock) {};
  virtual bool Setup();
  virtual void Run(CEvent& stopEvent);
  virtual void Cleanup();
  virtual float GetFps();
  virtual void OnResetDisplay();
  virtual void RefreshChanged();

private:
  volatile bool m_abort;
};
