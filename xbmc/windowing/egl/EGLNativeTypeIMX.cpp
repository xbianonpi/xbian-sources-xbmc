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

#include <linux/mxcfb.h>
#include "system.h"
#include <EGL/egl.h>

#include "Application.h"
#include "EGLNativeTypeIMX.h"
#include <math.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "utils/log.h"
#include "utils/RegExp.h"
#include "utils/StringUtils.h"
#include "utils/Environment.h"
#include "guilib/gui3d.h"
#include "windowing/WindowingFactory.h"
#include "cores/AudioEngine/AEFactory.h"
#include <fstream>

CEGLNativeTypeIMX::CEGLNativeTypeIMX()
  : m_display(NULL)
  , m_window(NULL)
{
  m_show = true;
  m_readonly = true;
}

CEGLNativeTypeIMX::~CEGLNativeTypeIMX()
{
}

bool CEGLNativeTypeIMX::CheckCompatibility()
{
  std::ifstream file("/sys/class/graphics/fb0/fsl_disp_dev_property");
  return file;
}

void CEGLNativeTypeIMX::Initialize()
{
  int fd;

  fd = open("/dev/fb0",O_RDWR);
  if (fd < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while opening /dev/fb0.\n", __FUNCTION__);
    return;
  }

  // Check if we can change the framebuffer resolution
  if ((fd = open("/sys/class/graphics/fb0/mode", O_RDWR)) >= 0)
  {
    m_readonly = false;
    close(fd);
    GetNativeResolution(&m_init); m_init.fPixelRatio = (float)m_init.iScreenWidth/m_init.iScreenHeight;
  }

  ShowWindow(false);
  return;
}

void CEGLNativeTypeIMX::Destroy()
{
  CLog::Log(LOGDEBUG, "%s\n", __FUNCTION__);
  struct fb_fix_screeninfo fixed_info;
  void *fb_buffer;
  int fd;

  fd = open("/dev/fb0",O_RDWR);
  if (fd < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while opening /dev/fb0.\n", __FUNCTION__);
    return;
  }

  ioctl( fd, FBIOGET_FSCREENINFO, &fixed_info);
  // Black fb0
  fb_buffer = mmap(NULL, fixed_info.smem_len, PROT_WRITE, MAP_SHARED, fd, 0);
  if (fb_buffer == MAP_FAILED)
  {
    CLog::Log(LOGERROR, "%s - fb mmap failed %s.\n", __FUNCTION__, strerror(errno));
  }
  else
  {
    memset(fb_buffer, 0x0, fixed_info.smem_len);
    munmap(fb_buffer, fixed_info.smem_len);
  }
  close(fd);

  if (!m_readonly)
  {
    CLog::Log(LOGDEBUG, "%s changing mode to %s\n", __FUNCTION__, m_init.strId.c_str());
    set_sysfs_str("/sys/class/graphics/fb0/mode", m_init.strId.c_str());
  }

  system("/usr/bin/splash --force -i -m 'stopping xbmc...'");
  return;
}

bool CEGLNativeTypeIMX::CreateNativeDisplay()
{
  CLog::Log(LOGDEBUG,": %s", __FUNCTION__);
#ifdef HAS_IMXVPU
  if (m_display)
    return true;

  // Force double-buffering
  CEnvironment::setenv("FB_MULTI_BUFFER", "2", 0);
  // EGL will be rendered on fb0
  if (!(m_display = fbGetDisplayByIndex(0)))
    return false;
  m_nativeDisplay = &m_display;
  return true;
#else
  return false;
#endif
}

bool CEGLNativeTypeIMX::CreateNativeWindow()
{
  CLog::Log(LOGDEBUG,": %s", __FUNCTION__);
#ifdef HAS_IMXVPU
  if (m_window)
    return true;

  if (!(m_window = fbCreateWindow(m_display, 0, 0, 0, 0)))
    return false;

  m_nativeWindow = &m_window;
  return true;
#else
  return false;
#endif
}

bool CEGLNativeTypeIMX::GetNativeDisplay(XBNativeDisplayType **nativeDisplay) const
{
  if (!m_nativeDisplay)
    return false;

  *nativeDisplay = (XBNativeDisplayType*)m_nativeDisplay;
  return true;
}

bool CEGLNativeTypeIMX::GetNativeWindow(XBNativeWindowType **nativeWindow) const
{
  if (!m_nativeWindow)
    return false;

  *nativeWindow = (XBNativeWindowType*)m_nativeWindow;
  return true;
}

bool CEGLNativeTypeIMX::DestroyNativeDisplay()
{
  CLog::Log(LOGDEBUG,": %s", __FUNCTION__);
#ifdef HAS_IMXVPU
  if (m_display)
    fbDestroyDisplay(m_display);

  m_display = NULL;
  m_nativeDisplay = NULL;
  return true;
#else
  return false;
#endif
}

bool CEGLNativeTypeIMX::DestroyNativeWindow()
{
  CLog::Log(LOGDEBUG,": %s", __FUNCTION__);
#ifdef HAS_IMXVPU
  if (m_window)
    fbDestroyWindow(m_window);

  m_window = NULL;
  m_nativeWindow = NULL;
  return true;
#else
  return false;
#endif
}

bool CEGLNativeTypeIMX::GetNativeResolution(RESOLUTION_INFO *res) const
{
  std::string mode;
  get_sysfs_str("/sys/class/graphics/fb0/mode", mode);
  CLog::Log(LOGDEBUG,": %s, %s", __FUNCTION__, mode.c_str());

  return ModeToResolution(mode, res);
}

bool CEGLNativeTypeIMX::SetNativeResolution(const RESOLUTION_INFO &res)
{
  if (m_readonly || !g_application.GetRenderGUI())
    return false;

  std::string mode;
  get_sysfs_str("/sys/class/graphics/fb0/mode", mode);
  if (res.strId == mode)
  {
    CLog::Log(LOGDEBUG,": %s - not changing res (%s vs %s)", __FUNCTION__, res.strId.c_str(), mode.c_str());
    return true;
  }

  DestroyNativeWindow();
  DestroyNativeDisplay();

  ShowWindow(false);
  CLog::Log(LOGDEBUG,": %s - changing resolution to %s", __FUNCTION__, res.strId.c_str());
  set_sysfs_str("/sys/class/graphics/fb0/mode", res.strId.c_str());

  CreateNativeDisplay();
  CreateNativeWindow();

  return true;
}

bool CEGLNativeTypeIMX::FindMatchingResolution(const RESOLUTION_INFO &res, const std::vector<RESOLUTION_INFO> &resolutions)
{
  for (int i = 0; i < (int)resolutions.size(); i++)
  {
    if(resolutions[i].iScreenWidth == res.iScreenWidth &&
       resolutions[i].iScreenHeight == res.iScreenHeight &&
       resolutions[i].fRefreshRate == res.fRefreshRate && (resolutions[i].dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK))
    {
       return true;
    }
  }
  return false;
}

bool CEGLNativeTypeIMX::ProbeResolutions(std::vector<RESOLUTION_INFO> &resolutions)
{
  if (m_readonly)
    return false;

  std::string valstr;
  get_sysfs_str("/sys/class/graphics/fb0/modes", valstr);

  std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");
  std::sort(probe_str.begin(), probe_str.end());

  resolutions.clear();
  RESOLUTION_INFO res;
  for (size_t i = 0; i < probe_str.size(); i++)
  {
    if(!StringUtils::StartsWith(probe_str[i], "S:") && !StringUtils::StartsWith(probe_str[i], "U:") &&
       !StringUtils::StartsWith(probe_str[i], "H:") && !StringUtils::StartsWith(probe_str[i], "T:"))
      continue;

    if(ModeToResolution(probe_str[i], &res))
      if(!FindMatchingResolution(res, resolutions))
         resolutions.push_back(res);
  }

  return resolutions.size() > 0;
}

bool CEGLNativeTypeIMX::GetPreferredResolution(RESOLUTION_INFO *res) const
{
  return GetNativeResolution(res);
}

bool CEGLNativeTypeIMX::ShowWindow(bool show)
{
  if (m_show == show)
    return true;

  CLog::Log(LOGDEBUG, ": %s %s", __FUNCTION__, show?"show":"hide");
  {
    if (!show)
    {
      int fd = open("/dev/fb0", O_WRONLY | O_NONBLOCK);
      ioctl(fd, FBIO_WAITFORVSYNC, 0);
      close(fd);
    }
    set_sysfs_str("/sys/class/graphics/fb0/blank", show?"0":"1");
  }

  m_show = show;
  return true;
}

bool CEGLNativeTypeIMX::get_sysfs_str(std::string path, std::string& valstr) const
{
  int len;
  char buf[256] = {0};

  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
  {
    CLog::Log(LOGERROR, "%s: error reading %s",__FUNCTION__, path.c_str());
    valstr = "fail";
    return false;
  }

  while ((len = read(fd, buf, 255)) > 0)
    valstr.append(buf, len);

  StringUtils::Trim(valstr);
  close(fd);

  return true;
}

bool CEGLNativeTypeIMX::set_sysfs_str(std::string path, std::string val) const
{
  int fd = open(path.c_str(), O_WRONLY);
  if (fd < 0)
  {
    CLog::Log(LOGERROR, "%s: error writing %s",__FUNCTION__, path.c_str());
    return false;
  }

  val += '\n';
  write(fd, val.c_str(), val.size());
  close(fd);

  return true;
}

bool CEGLNativeTypeIMX::ModeToResolution(std::string mode, RESOLUTION_INFO *res) const
{
  if (!res)
    return false;

  res->iWidth = 0;
  res->iHeight= 0;

  if(mode.empty())
    return false;

  std::string fromMode = StringUtils::Mid(mode, 2);
  StringUtils::Trim(fromMode);

  res->dwFlags = 0;
  res->fPixelRatio = 1.0f;

  if (StringUtils::StartsWith(mode, "U:")) {
    res->dwFlags |= D3DPRESENTFLAG_WIDESCREEN;
  } else if (StringUtils::StartsWith(mode, "H:")) {
    res->dwFlags |= D3DPRESENTFLAG_MODE3DSBS;
    res->fPixelRatio = 2.0f;
  } else if (StringUtils::StartsWith(mode, "T:")) {
    res->dwFlags |= D3DPRESENTFLAG_MODE3DTB;
    res->fPixelRatio = 0.5f;
  } else if (StringUtils::StartsWith(mode, "F:")) {
    return false;
  }

  CRegExp split(true);
  split.RegComp("([0-9]+)x([0-9]+)([pi])-([0-9]+)");
  if (split.RegFind(fromMode) < 0)
    return false;

  int w = atoi(split.GetMatch(1).c_str());
  int h = atoi(split.GetMatch(2).c_str());
  std::string p = split.GetMatch(3);
  int r = atoi(split.GetMatch(4).c_str());

  res->iWidth = w;
  res->iHeight= h;
  res->iScreenWidth = w;
  res->iScreenHeight= h;
  res->fRefreshRate = r;
  res->dwFlags |= p[0] == 'p' ? D3DPRESENTFLAG_PROGRESSIVE : D3DPRESENTFLAG_INTERLACED;

  res->iScreen       = 0;
  res->bFullScreen   = true;
  res->iSubtitles    = (int)(0.965 * res->iHeight);
  res->fPixelRatio  *= (float)m_init.fPixelRatio / ((float)res->iScreenWidth/(float)res->iScreenHeight);
  res->strMode       = StringUtils::Format("%dx%d @ %.2f%s %s- Full Screen (%.3f)", res->iScreenWidth, res->iScreenHeight, res->fRefreshRate,
                                           res->dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
                                           res->dwFlags & D3DPRESENTFLAG_MODE3DSBS ? "3DSBS " : res->dwFlags & D3DPRESENTFLAG_MODE3DTB ? "3DTB " : "",
                                           res->fPixelRatio);
  res->strId         = mode;

  return res->iWidth > 0 && res->iHeight> 0;
}

