/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AppParamParser.h"
#include "CompileInfo.h"
#include "FileItem.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/SystemInfo.h"
#include "utils/StringUtils.h"
#include <stdlib.h>

CAppParamParser::CAppParamParser()
: m_logLevel(LOG_LEVEL_NORMAL),
  m_playlist(new CFileItemList())
{
}

CAppParamParser::~CAppParamParser()
{
}

void CAppParamParser::Parse(const char* const* argv, int nArgs)
{
  if (nArgs > 1)
  {
    for (int i = 1; i < nArgs; i++)
      ParseArg(argv[i]);

    // testmode is only valid if at least one item to play was given
    if (m_playlist->IsEmpty())
      m_testmode = false;
  }
}

void CAppParamParser::DisplayVersion()
{
  printf("%s Media Center %s\n", CSysInfo::GetVersion().c_str(), CSysInfo::GetAppName().c_str());
  printf("Copyright (C) %s Team %s - http://kodi.tv\n",
         CCompileInfo::GetCopyrightYears(), CSysInfo::GetAppName().c_str());
  exit(0);
}

void CAppParamParser::DisplayHelp()
{
  std::string lcAppName = CSysInfo::GetAppName();
  StringUtils::ToLower(lcAppName);
  printf("Usage: %s [OPTION]... [FILE]...\n\n", lcAppName.c_str());
  printf("Arguments:\n");
  printf("  -fs\t\t\tRuns %s in full screen\n", CSysInfo::GetAppName().c_str());
  printf("  --standalone\t\t%s runs in a stand alone environment without a window \n", CSysInfo::GetAppName().c_str());
  printf("\t\t\tmanager and supporting applications. For example, that\n");
  printf("\t\t\tenables network settings.\n");
  printf("  -p or --portable\t%s will look for configurations in install folder instead of ~/.%s\n", CSysInfo::GetAppName().c_str(), lcAppName.c_str());
  printf("  --debug\t\tEnable debug logging\n");
  printf("  --logtype=<type>\tSet <type> of logging. type is either none|0, local|1, syslog|2 or both|3\n\t\t\tfor example --logtype=2 enables logging via syslog\n");
  printf("  --syslog\t\tEquivalent to --logtype=syslog\n");
  printf("  --version\t\tPrint version information\n");
  printf("  --test\t\tEnable test mode. [FILE] required.\n");
  printf("  --settings=<filename>\tLoads specified file after advancedsettings.xml replacing any settings specified\n");
  printf("  \t\t\tspecified file must exist in special://xbmc/system/\n");
  exit(0);
}

void CAppParamParser::ParseArg(const std::string &arg)
{
  if (const char *p = getenv("KODI_LOGTYPE"))
    m_logType = atoi(p);
  if (arg == "-fs" || arg == "--fullscreen")
    m_startFullScreen = true;
  else if (arg == "-h" || arg == "--help")
    DisplayHelp();
  else if (arg == "-v" || arg == "--version")
    DisplayVersion();
  else if (arg == "--standalone")
    m_standAlone = true;
  else if (arg == "-p" || arg  == "--portable")
    m_platformDirectories = false;
  else if (arg == "--debug")
    m_logLevel = LOG_LEVEL_DEBUG;
  else if (arg == "--test")
    m_testmode = true;
  else if (arg.substr(0, 10) == "--logtype=")
    if (arg.substr(10) == "local")
      m_logType = 1;
    else if (arg.substr(10) == "syslog")
      m_logType = 2;
    else if (arg.substr(10) == "both")
      m_logType = 3;
    else if (arg.substr(10) == "none")
      m_logType = 0;
    else
      m_logType = atoi(arg.substr(10).c_str());
  else if (arg == "--syslog")
      m_logType = 2;
  else if (arg.substr(0, 11) == "--settings=")
    m_settingsFile = arg.substr(11);
  else if (arg.length() != 0 && arg[0] != '-')
  {
    const CFileItemPtr item = std::make_shared<CFileItem>(arg);
    item->SetPath(arg);
    m_playlist->Add(item);
  }
}

void CAppParamParser::SetAdvancedSettings(CAdvancedSettings& advancedSettings) const
{
  if (m_logLevel == LOG_LEVEL_DEBUG)
  {
    advancedSettings.m_logLevel = LOG_LEVEL_DEBUG;
    advancedSettings.m_logLevelHint = LOG_LEVEL_DEBUG;
    CLog::SetLogLevel(LOG_LEVEL_DEBUG);
  }

  if (!m_settingsFile.empty())
    advancedSettings.AddSettingsFile(m_settingsFile);

  if (m_startFullScreen)
    advancedSettings.m_startFullScreen = true;

  if (m_standAlone)
    advancedSettings.m_handleMounting = true;

  CLog::SetLogType(m_logType);
}

const CFileItemList& CAppParamParser::GetPlaylist() const
{
  return *m_playlist;
}
