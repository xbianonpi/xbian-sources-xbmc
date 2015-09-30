#pragma once

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

#include "threads/CriticalSection.h"
#include "threads/SingleLock.h"
#include <string>
#include "guilib/GraphicContext.h"

#define EDID_STRUCT_DISPLAY             0x14
#define EDID_DTM_START                  0x36
#define EDID_DTM_OFFSET_DIMENSION       0x0c
#define EDID_EXTENSION_BLOCK_START      0x7e
#define EDID_MAXSIZE                    512

class CEGLEdid
{
  struct dt_dim {
    uint8_t Width;
    uint8_t Height;
    uint8_t msbits;
  };

public:
  CEGLEdid();
  virtual ~CEGLEdid();

  bool    ReadEdidData();
  const uint8_t *GetRawEdid() { Lock(); if (m_edidEmpty) CalcSAR(); return m_edidEmpty ? NULL : &m_edid[0]; }

  float   GetSAR() const { CSingleLock lk(m_lock); return m_fSar; }
  void    CalcSAR();

  void Lock() { m_lock.lock(); }
  void Unlock() { m_lock.unlock(); }

  uint8_t      m_edid[EDID_MAXSIZE];

protected:
  float   ValidateSAR(struct dt_dim *dtm, bool mb = false);

  float        m_fSar;
  bool         m_edidEmpty;

  CCriticalSection m_lock;
};

extern CEGLEdid g_EGLEdid;
