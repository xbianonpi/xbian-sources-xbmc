/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DRMUtils.h"
#include "platform/linux/RBP.h"

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CDRMAtomic : public CDRMUtils
{
public:
  CDRMAtomic() = default;
  ~CDRMAtomic() { DestroyDrm(); };
  virtual void FlipPage(struct gbm_bo *bo, bool rendered, bool videoLayer) override;
  virtual bool SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo *bo) override;
  virtual bool SetActive(bool active) override;
  virtual bool InitDrm() override;
  virtual void DestroyDrm() override;
  virtual bool AddProperty(struct drm_object *object, const char *name, uint64_t value) override;

private:
  void DrmAtomicCommit(int fb_id, int flags, bool rendered, bool videoLayer);
  bool ResetPlanes();

  bool m_need_modeset;
  bool m_active = true;
  drmModeAtomicReq *m_req = nullptr;
  DISPMANX_ELEMENT_HANDLE_T m_dispman_display = 0;
};

}
}
}
