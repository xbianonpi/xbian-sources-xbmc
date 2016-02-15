/*
 *      Copyright (C) 2011-2013 Team XBMC
 *      http://www.xbmc.org
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
#include "EGLEdid.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "settings/DisplaySettings.h"

CEGLEdid g_EGLEdid;

CEGLEdid::CEGLEdid()
  : m_fSar(0.0f)
  , m_edidEmpty(true)
{
  memset(&m_edid, 0, EDID_MAXSIZE);
}

CEGLEdid::~CEGLEdid()
{
}

float CEGLEdid::ValidateSAR(struct dt_dim *dtm, bool mb)
{
  int Height = dtm->Height | (mb ? (dtm->msbits & 0x0f) << 8 : 0);
  if (Height < 1)
    return .0f;

  int Width = dtm->Width | (mb ? (dtm->msbits & 0xf0) << 4 : 0);
  float t_sar = (float) Width / Height;

  if (t_sar < 0.33 || t_sar > 3.00)
    t_sar = .0f;
  else
    CLog::Log(LOGINFO, "%s: Screen SAR: %.3f (from detailed: %s, %dx%d)",__FUNCTION__, t_sar, mb ? "yes" : "no", Width, Height);

  return t_sar;
}

void CEGLEdid::CalcSAR()
{
  CSingleLock lk(m_lock);

  m_fSar = .0f;
  m_edidEmpty = true;
  ReadEdidData();

  // enumerate through (max four) detailed timing info blocks
  // specs and lookup WxH [mm / in]. W and H are in 3 bytes,
  // where 1st = W, 2nd = H, 3rd byte is 4bit/4bit.
  //
  // if DTM block starts with 0 - it is not DTM, skip
  for (int i = EDID_DTM_START; i < 126 && m_fSar == 0 && *(m_edid +i); i += 18)
    m_fSar = ValidateSAR((struct dt_dim *)(m_edid +i +EDID_DTM_OFFSET_DIMENSION), true);

  // fallback - info related to 'Basic display parameters.' is at offset 0x14-0x18.
  // where W is 2nd byte, H 3rd.
  if (m_fSar == 0)
    m_fSar = ValidateSAR((struct dt_dim *)(m_edid +EDID_STRUCT_DISPLAY +1));

  // if m_sar != 0, final SAR is usefull
  // if it is 0, EDID info was missing or calculated
  // SAR value wasn't sane
  if (m_fSar == 0)
  {
    RESOLUTION_INFO res = CDisplaySettings::GetInstance().GetCurrentResolutionInfo();

    CLog::Log(LOGWARNING, "%s: Screen SAR - not usable info",__FUNCTION__);

    if (res.iScreenWidth != 0)
      m_fSar = res.iScreenHeight / res.iScreenWidth;
    else
      m_fSar = .0f;
  }

  m_edidEmpty = false;
}
