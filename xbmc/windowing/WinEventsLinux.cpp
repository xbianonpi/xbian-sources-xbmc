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
#include "system.h"

#if defined(HAS_LINUX_EVENTS)

#include "WinEventsLinux.h"
#include "WinEvents.h"
#include "XBMC_events.h"
#include "input/XBMC_keysym.h"
#include "Application.h"
#include "input/MouseStat.h"
#include "utils/log.h"
#include "powermanagement/PowerManager.h"

#ifdef TARGET_RASPBERRY_PI
#include "utils/TimeUtils.h"
#include "guilib/Resolution.h"
#include "addons/Skin.h"
#include "utils/XMLUtils.h"
#include "utils/StringUtils.h"
#include "filesystem/File.h"
#include "guilib/iimage.h"
#include "guilib/XBTF.h"
#include "guilib/imagefactory.h"
#include "guilib/TextureManager.h"
#include "linux/RBP.h"
#include "input/InputManager.h"
#endif

bool CWinEventsLinux::m_initialized = false;
CLinuxInputDevices CWinEventsLinux::m_devices;

CWinEventsLinux::CWinEventsLinux()
{
#ifdef TARGET_RASPBERRY_PI
  m_last_mouse_move_time = 0;
  m_mouse_state = -1;
  memset(m_cursors, 0, sizeof m_cursors);
#endif
}

void CWinEventsLinux::RefreshDevices()
{
  m_devices.InitAvailable();
}

bool CWinEventsLinux::IsRemoteLowBattery()
{
  return m_devices.IsRemoteLowBattery();
  return false;
}

#ifdef TARGET_RASPBERRY_PI
void *CWinEventsLinux::LoadImage(const std::string texturePath, int &width, int &height)
{
  void *pixels = NULL;
  // Read image into memory to use our vfs
  XFILE::CFile file;
  XFILE::auto_buffer buf;

  if (file.LoadFile(texturePath, buf) <= 0)
    return NULL;

  IImage *pImage = ImageFactory::CreateLoader(texturePath);
  if (pImage != NULL && pImage->LoadImageFromMemory((unsigned char *)buf.get(), buf.size(), width, height))
  {
    width = pImage->Width();
    height = pImage->Height();
    if (width > 0 && height > 0)
    {
      pixels = malloc(width * height * 4);
      if (!pixels)
        return NULL;
      if (!pImage->Decode((unsigned char *)pixels, width * 4, XB_FMT_A8R8G8B8))
      {
        free(pixels);
        return NULL;
      }
    }
  }
  return pixels;
}

bool CWinEventsLinux::LoadXML(const std::string strFileName)
{
  RESOLUTION_INFO m_coordsRes; // resolution that the window coordinates are in.
  // Find appropriate skin folder + resolution to load from
  std::string strFileNameLower = strFileName;
  StringUtils::ToLower(strFileNameLower);
  std::string strLowerPath = g_SkinInfo->GetSkinPath(strFileNameLower, &m_coordsRes);
  std::string strPath = g_SkinInfo->GetSkinPath(strFileName, &m_coordsRes);

  TiXmlElement* pRootElement = NULL;
  CXBMCTinyXML xmlDoc;
  std::string strPathLower = strPath;
  StringUtils::ToLower(strPathLower);
  if (!xmlDoc.LoadFile(strPath) && !xmlDoc.LoadFile(strPathLower) && !xmlDoc.LoadFile(strLowerPath))
  {
    CLog::Log(LOGERROR, "unable to load:%s, Line %d\n%s", strPath.c_str(), xmlDoc.ErrorRow(), xmlDoc.ErrorDesc());
    return false;
  }
  pRootElement = (TiXmlElement*)xmlDoc.RootElement()->Clone();
  //printf("%s: load:%s,%s,%s\n", __func__, strPath.c_str(), strPathLower.c_str(), strLowerPath.c_str());

  if (!pRootElement)
    return false;

  if (strcmpi(pRootElement->Value(), "window"))
  {
    CLog::Log(LOGERROR, "file : XML file doesnt contain <window>");
    return false;
  }

  TiXmlElement *pChild = pRootElement->FirstChildElement();
  while (pChild)
  {
    if (strcmpi(pChild->Value(), "controls") == 0)
    {
      TiXmlElement *pControl = pChild->FirstChildElement();
      while (pControl)
      {
        //printf("%s:2 %s\n", __func__, pControl->Value());
        if (strcmpi(pControl->Value(), "control") == 0)
        {
          std::string strStringValue;
          if (XMLUtils::GetString(pControl, "texture", strStringValue))
          {
            const char* idAttr = pControl->Attribute("id");
            int index = idAttr ? atoi(idAttr)-1 : -1;
            if (index >= 0 && index < (int)(sizeof m_cursors/sizeof *m_cursors))
            {
              if (m_cursors[index].pixels)
                free(m_cursors[index].pixels);
              std::string path = g_TextureManager.GetTexturePath(strStringValue);
              m_cursors[index].width = m_cursors[index].height = 64; // max
              m_cursors[index].pixels = LoadImage(path, m_cursors[index].width, m_cursors[index].height);
              //printf("%s: texture: %d %s %dx%d %p\n", __func__, index, path.c_str(), m_cursors[index].width, m_cursors[index].height, m_cursors[index].pixels);
            }
          }
        }
        pControl = pControl->NextSiblingElement();
      }
    }
    pChild = pChild->NextSiblingElement();
  }
  delete pRootElement;
  return true;
}
#endif

bool CWinEventsLinux::MessagePump()
{
  if (!m_initialized)
  {
    m_devices.InitAvailable();
    m_initialized = true;
#ifdef TARGET_RASPBERRY_PI
    LoadXML("Pointer.xml");
#endif
  }

  bool ret = false;
  XBMC_Event event = {0};
#ifdef TARGET_RASPBERRY_PI
  bool active = CInputManager::Get().IsMouseActive();
  int64_t Now = CurrentHostCounter();
  if (!active)
  {
    if (m_mouse_state != -1)
    {
      g_RBP.update_cursor(0, 0, 0);
      m_mouse_state = -1;
    }
  }
  else
  {
    int state = CInputManager::Get().GetMouseState() - 1;
    if (m_mouse_state != state)
    {
      //printf("%s: %d->%d\n", __func__, m_mouse_state, state);
      if (state >= 0 && state < (int)(sizeof m_cursors/sizeof *m_cursors) && m_cursors[state].pixels)
      {
        g_RBP.set_cursor(m_cursors[state].pixels, m_cursors[state].width, m_cursors[state].height, 0, 0);
      }
      m_mouse_state = state;
    }
  }
#endif
  while (1)
  {
    event = m_devices.ReadEvent();
#ifdef TARGET_RASPBERRY_PI
    if (active && (event.type == XBMC_MOUSEMOTION || event.type == XBMC_MOUSEBUTTONDOWN || event.type == XBMC_MOUSEBUTTONUP))
    {
      if (event.type == XBMC_MOUSEMOTION)
        g_RBP.update_cursor(event.motion.x, event.motion.y, 1);
      m_last_mouse_move_time = Now;
      //printf("%s: %d,%d %d %d,%d (%d,%d) act:%d\n", __func__, event.motion.type, event.motion.which, event.motion.state, event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel, CInputManager::Get().IsMouseActive());
    }
#endif
    if (event.type != XBMC_NOEVENT)
    {
      ret |= g_application.OnEvent(event);
    }
    else
    {
      break;
    }
  }

#ifdef TARGET_RASPBERRY_PI
  if (active && Now - m_last_mouse_move_time > 5 * 1000000000LL)
  {
    g_RBP.update_cursor(0, 0, 0);
    m_mouse_state = -1;
  }
#endif
  return ret;
}

size_t CWinEventsLinux::GetQueueSize()
{
  return m_devices.Size();
}

void CWinEventsLinux::MessagePush(XBMC_Event *ev)
{
  g_application.OnEvent(*ev);
}

#endif
