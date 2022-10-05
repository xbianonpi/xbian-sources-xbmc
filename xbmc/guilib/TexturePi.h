/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "Texture.h"

#include "system_gl.h"

/************************************************************************/
/*    CPiTexture                                                       */
/************************************************************************/
class CPiTexture : public CTexture
{
public:
  CPiTexture(unsigned int width = 0, unsigned int height = 0, unsigned int format = XB_FMT_A8R8G8B8);
  ~CPiTexture() override;

  void CreateTextureObject() override;
  void LoadToGPU() override;
  void Update(unsigned int width, unsigned int height, unsigned int pitch, unsigned int format, const unsigned char *pixels, bool loadToGPU) override;
  void Allocate(unsigned int width, unsigned int height, unsigned int format) override;
  bool LoadFromFileInternal(const std::string& texturePath, unsigned int maxWidth, unsigned int maxHeight, bool requirePixels, const std::string& strMimeType = "") override;

protected:
  void *m_egl_image = NULL;
};
