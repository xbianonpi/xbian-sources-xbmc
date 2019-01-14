/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CECBuiltins.h"

#include "messaging/ApplicationMessenger.h"
#include "utils/log.h"

using namespace KODI::MESSAGING;

/*! \brief Wake up device through CEC.
 *  \param params (ignored)
 */
static int ActivateSource(const std::vector<std::string>& params)
{
  bool forceType(false);

  if (params.size())
  {
    if (params[0] == "force" || params[0] == "1")
      forceType = true;
  }
  else
    CLog::Log(LOGNOTICE, "CECActivateSource called without parameter, assuming 'unforced'");
  CApplicationMessenger::GetInstance().PostMsg(TMSG_CECACTIVATESOURCE, forceType);

  return 1; // Don't wake up screensaver
}

/*! \brief Put device in standby through CEC.
 *  \param params (ignored)
 */
static int Standby(const std::vector<std::string>& params)
{
  bool forceType(false);

  if (params.size())
  {
    if (params[0] == "force" || params[0] == "1")
      forceType = true;
  }
  else
    CLog::Log(LOGNOTICE, "CECStandby called without parameter, assuming 'unforced'");
  CApplicationMessenger::GetInstance().PostMsg(TMSG_CECSTANDBY, forceType);

  return 1; // Don't wake up screensaver
}

/*! \brief Toggle device state through CEC.
 *  \param params (ignored)
 */
static int ToggleState(const std::vector<std::string>& params)
{
  bool result;
  bool forceType(false);

  if (params.size())
  {
    if (params[0] == "force" || params[0] == "1")
      forceType = true;
  }
  else
    CLog::Log(LOGNOTICE, "CECToggleState called without parameter, assuming 'unforced'");
  CApplicationMessenger::GetInstance().SendMsg(TMSG_CECTOGGLESTATE, forceType, 0, static_cast<void*>(&result));

  return 1; // Don't wake up screensaver
}

// Note: For new Texts with comma add a "\" before!!! Is used for table text.
//
/// \page page_List_of_built_in_functions
/// \section built_in_functions_4 CEC built-in's
///
/// -----------------------------------------------------------------------------
///
/// \table_start
///   \table_h2_l{
///     Function,
///     Description }
///   \table_row2_l{
///     <b>`CECActivateSource`</b>
///     ,
///     Wake up playing device via a CEC peripheral
///   }
///   \table_row2_l{
///     <b>`CECStandby`</b>
///     ,
///     Put playing device on standby via a CEC peripheral
///   }
///   \table_row2_l{
///     <b>`CECToggleState`</b>
///     ,
///     Toggle state of playing device via a CEC peripheral
///   }
///  \table_end
///

CBuiltins::CommandMap CCECBuiltins::GetOperations() const
{
  return {
           {"cectogglestate",    {"Toggle state of playing device via a CEC peripheral", 0, ToggleState}},
           {"cecactivatesource", {"Wake up playing device via a CEC peripheral", 0, ActivateSource}},
           {"cecstandby",        {"Put playing device on standby via a CEC peripheral", 0, Standby}}
         };
}
