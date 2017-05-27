/*
This program shows the system power status and monitors it for changes.

g++ -std=c++11 -o battstatus battstatus.cpp -lntdll -lpowrprof

*//*
Copyright (C) 2017 Jay Satiro <raysatiro@yahoo.com>
All rights reserved.

This file is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This file is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

<https://www.gnu.org/licenses/#GPL>
*/

#define _WIN32_WINNT 0x0501

#include <windows.h>
#include <powrprof.h>
#include <ddk/ntddk.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#ifndef ES_AWAYMODE_REQUIRED
#define ES_AWAYMODE_REQUIRED ((DWORD)0x00000040)
#endif

#ifndef PBTF_APMRESUMEFROMFAILURE
#define PBTF_APMRESUMEFROMFAILURE 0x00000001
#endif

#ifndef PBT_POWERSETTINGCHANGE
#define PBT_POWERSETTINGCHANGE 0x8013
#endif

#ifndef SPSF_BATTERYCHARGING
#define SPSF_BATTERYCHARGING 8
#endif

#ifndef SPSF_BATTERYNOBATTERY
#define SPSF_BATTERYNOBATTERY 128
#endif

/* Sample output at field width 22:
ACLineStatus:         Offline
BatteryFlag:          Low
BatteryLifePercent:   17%
BatteryLifeTime:      42 min
BatteryFullLifeTime:  Unknown
Battery discharge:    -11433mW
*/
#define BATT_FIELD_WIDTH 22

using namespace std;

int verbose;
bool prevent_sleep;
RTL_OSVERSIONINFOW os;

template <typename T>
string UndocumentedValueStr(T undocumented_value)
{
  stringstream ss;
  ss << "Undocumented value: " << undocumented_value;
  if(is_arithmetic<T>::value)
    ss << " (hex: " << hex << undocumented_value << ")";
  return ss.str();
}

string UndocumentedValueStr(char undocumented_value)
{
  return UndocumentedValueStr<
    typename conditional<
      is_signed<char>::value, signed, unsigned
    >::type>(undocumented_value);
}

string UndocumentedValueStr(unsigned char undocumented_value)
{
  return UndocumentedValueStr<unsigned>(undocumented_value);
}

string UndocumentedValueStr(signed char undocumented_value)
{
  return UndocumentedValueStr<signed>(undocumented_value);
}

string ACLineStatusStr(unsigned ACLineStatus)
{
  switch(ACLineStatus)
  {
  case 0: return "Offline";
  case 1: return "Online";
  case 255: return "Unknown status";
  }
  return UndocumentedValueStr(ACLineStatus);
}

string BatteryFlagStr(unsigned BatteryFlag)
{
  /* BatteryFlag "value is zero if the battery is not being charged and the
     battery capacity is between low and high." ie if 33 <= percentage <= 66.
     Earlier revisions of this function showed 'Normal' instead of '<none>',
     but that was less correct since technically 'Normal' is not a flag, and
     things like 'Normal | Charging' would need to be handled. */
  if(BatteryFlag == 0)
    return "<none>";

  stringstream ss;
#define EXTRACT_BATTERYFLAG(flag, name) \
  if((BatteryFlag & flag)) { \
    if(ss.tellp()) \
      ss << " | "; \
    ss << name; \
    BatteryFlag &= ~flag; \
  }
  EXTRACT_BATTERYFLAG(1, "High");
  EXTRACT_BATTERYFLAG(2, "Low");
  EXTRACT_BATTERYFLAG(4, "Critical");
  EXTRACT_BATTERYFLAG(SPSF_BATTERYCHARGING, "Charging");
  EXTRACT_BATTERYFLAG(SPSF_BATTERYNOBATTERY, "No system battery");
  EXTRACT_BATTERYFLAG(255, "Unknown status");
  if(BatteryFlag) {
    if(ss.tellp())
      ss << " | ";
    ss << UndocumentedValueStr(BatteryFlag);
  }
  return ss.str();
}

string BatteryLifePercentStr(unsigned BatteryLifePercent)
{
  stringstream ss;
  if(0 <= BatteryLifePercent && BatteryLifePercent <= 100)
    ss << (DWORD)BatteryLifePercent << "%";
  else if(BatteryLifePercent == 255)
    ss << "Unknown status";
  else
    ss << UndocumentedValueStr(BatteryLifePercent);
  return ss.str();
}

/* SystemStatusFlag:
"This flag and the GUID_POWER_SAVING_STATUS GUID were introduced in Windows 10.
This flag was previously reserved, named Reserved1, and had a value of 0."
*/
string SystemStatusFlagStr(unsigned SystemStatusFlag)
{
  switch(SystemStatusFlag)
  {
  case 0: return "Battery saver is off";
  case 1: return "Battery saver is on";
  }
  return UndocumentedValueStr(SystemStatusFlag);
}

/* Format the number of battery life seconds in the same format as the systray:
[1 hr ]01 min or "Unknown" if BatteryLifeTime is -1.
*/
string BatteryLifeTimeStr(DWORD BatteryLifeTime)
{
  if(BatteryLifeTime == (DWORD)-1)
    return "Unknown";

  DWORD hours = BatteryLifeTime / 3600;
  DWORD minutes = (BatteryLifeTime % 3600) / 60;

  stringstream ss;
  if(hours)
    ss << hours << " hr ";
  ss.fill('0');
  ss << setw(2) << minutes << " min";
  return ss.str();
}

/* BatteryFullLifeTime:
"The system is only capable of estimating BatteryFullLifeTime based on
calculations on BatteryLifeTime and BatteryLifePercent. Without smart battery
subsystems, this value may not be accurate enough to be useful."
*/
string BatteryFullLifeTimeStr(DWORD BatteryFullLifeTime)
{
  return BatteryLifeTimeStr(BatteryFullLifeTime);
}

/* time as local time string in format: Tue May 16 03:24:31 PM */
string TimeToLocalTimeStr(time_t t)
{
  char buf[100];
  /* localtime is thread-safe in Windows but the per-thread structure that's
     returned is shared by gmtime, mktime, mkgmtime. */
  struct tm *lt = localtime(&t);
  // old format: "%Y-%m-%d %H:%M:%S" 2017-05-16 15:10:08
  if(!lt || !strftime(buf, sizeof buf, "%a %b %d %I:%M:%S %p", lt))
    return "Unknown time";
  return buf;
}

void ShowPowerStatus(const SYSTEM_POWER_STATUS *status)
{
#define SHOW_STATUS(item) \
  cout << left << setw(BATT_FIELD_WIDTH) << #item ": " \
       << right << item##Str(status->item) << "\n";

  SHOW_STATUS(ACLineStatus);
  SHOW_STATUS(BatteryFlag);
  SHOW_STATUS(BatteryLifePercent);
  if(os.dwMajorVersion >= 10) { /* SystemStatusFlag was added in Windows 10 */
    cout << left << setw(BATT_FIELD_WIDTH) << "SystemStatusFlag: "
         << right << SystemStatusFlagStr(status->Reserved1) << "\n";
  }
  SHOW_STATUS(BatteryLifeTime);
  SHOW_STATUS(BatteryFullLifeTime);
  cout << flush;
}

enum cpstype { CPS_EQUAL, CPS_NOTEQUAL };
enum cpstype ComparePowerStatus(const SYSTEM_POWER_STATUS *a,
                                const SYSTEM_POWER_STATUS *b)
{
#define COMPARE_STATUS(item) \
  if(a->item != b->item) \
    return CPS_NOTEQUAL;

  COMPARE_STATUS(ACLineStatus);
  COMPARE_STATUS(BatteryFlag);
  COMPARE_STATUS(BatteryLifePercent);
  COMPARE_STATUS(Reserved1); // aka SystemStatusFlag
  COMPARE_STATUS(BatteryLifeTime);
  COMPARE_STATUS(BatteryFullLifeTime);
  return CPS_EQUAL;
}

/* Try to get the battery mW, ignore errors.
Battery mW seems to only change when the power status changes.
Rate: "The current rate of discharge of the battery, in mW. A nonzero, positive
rate indicates charging; a negative rate indicates discharging. Some batteries
report only discharging rates. This value should be treated as a LONG as it can
contain negative values (with the high bit set)."
However when some of my batteries charge the Rate is:
0x80000000 == -2147483648 (LONG) == 2147483648 (DWORD)
.. so I'm treating that value as 0.
When my batteries are removed the Rate is 0.
*/
LONG GetBatteryMilliwatts()
{
  SYSTEM_BATTERY_STATE sbs;
  if(CallNtPowerInformation(SystemBatteryState, NULL, 0, &sbs, sizeof sbs))
    return 0;
  return (sbs.Rate != 0x80000000) ? (LONG)sbs.Rate : 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if(verbose >= 3)
  {
    /* show all window messages */
    cout << "[" << TimeToLocalTimeStr(time(NULL)) << "]: "
         << hex << "WindowProc: msg 0x" << msg << ", wparam 0x" << wParam
         << ", lparam 0x" << lParam << dec << endl;
  }
  switch(msg) {
  /* WM_POWERBROADCAST:
     "Notifies applications of a change in the power status of the computer,
     such as a switch from battery power to A/C. The system also broadcasts
     this event when remaining battery power slips below the threshold
     specified by the user or if the battery power changes by a specified
     percentage."
     ...and in remarks:
     "This event can occur when battery life drops to less than 5 minutes, or
     when the percentage of battery life drops below 10 percent, or if the
     battery life changes by 3 percent."
     Note this is a broadcast message and therefore not received by
     message-only windows. */
#define CASE_PBT(item) \
  case item: cout << #item; break;
  case WM_POWERBROADCAST:
    cout << "[" << TimeToLocalTimeStr(time(NULL)) << "]: "
         << "WM_POWERBROADCAST: ";
    switch(wParam) {
    CASE_PBT(PBT_APMQUERYSUSPEND);        /* 0x0000 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMQUERYSTANDBY);        /* 0x0001 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMQUERYSUSPENDFAILED);  /* 0x0002 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMQUERYSTANDBYFAILED);  /* 0x0003 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMSUSPEND);             /* 0x0004 */
    CASE_PBT(PBT_APMSTANDBY);             /* 0x0005 */
    CASE_PBT(PBT_APMRESUMECRITICAL);      /* 0x0006 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMRESUMESUSPEND);       /* 0x0007 */
    CASE_PBT(PBT_APMRESUMESTANDBY);       /* 0x0008 */
    CASE_PBT(PBT_APMBATTERYLOW);          /* 0x0009 */  /* Win2k & XP only */
    CASE_PBT(PBT_APMPOWERSTATUSCHANGE);   /* 0x000A */
    CASE_PBT(PBT_APMOEMEVENT);            /* 0x000B */  /* Win2k & XP only */
    CASE_PBT(PBT_APMRESUMEAUTOMATIC);     /* 0x0012 */
    CASE_PBT(PBT_POWERSETTINGCHANGE);     /* 0x8013 */
    default: cout << UndocumentedValueStr(wParam);
    }

    if(lParam == 0 &&
       wParam != PBT_APMQUERYSUSPEND &&
       wParam != PBT_APMQUERYSTANDBY) {
      /* lParam in this case has no significance so skip showing it */
      cout << endl;
      return TRUE;
    }

    cout << " (lParam: ";
    if(wParam == PBT_APMQUERYSUSPEND ||
       wParam == PBT_APMQUERYSTANDBY) {
      LPARAM unknown = (LPARAM)(lParam & ~1);
      if(lParam & 1) {
        cout << "Bit 0 is on, user prompting/interaction is allowed.";
        if(unknown)
          cout << " | ";
      }
      else {
        cout << "Bit 0 is off, User prompting/interaction is not allowed.";
        if(unknown)
          cout << " | ";
      }
      if(unknown)
        cout << UndocumentedValueStr(lParam);
    }
#if 0 // this is unreachable now since lParam 0 is skipped earlier
    else if(lParam == 0) {
      cout << "0";
    }
#endif
    else if(wParam == PBT_APMRESUMECRITICAL ||
            wParam == PBT_APMRESUMESUSPEND ||
            wParam == PBT_APMRESUMESTANDBY ||
            wParam == PBT_APMRESUMEAUTOMATIC) {
      LPARAM unknown = (LPARAM)(lParam & ~PBTF_APMRESUMEFROMFAILURE);
      if(lParam & PBTF_APMRESUMEFROMFAILURE) {
        cout << "PBTF_APMRESUMEFROMFAILURE";
        if(unknown)
          cout << " | ";
      }
      if(unknown)
        cout << UndocumentedValueStr(unknown);
    }
    else
      cout << UndocumentedValueStr(lParam);
    cout << ")" << endl;
    return TRUE;

  default:
    break;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

HWND InitMonitorWindow()
{
  const char * window_class_name =
    "battstatus window {4A6A9339-FA17-4324-93FC-EC92656FF058}";

  WNDCLASS wc;
  wc.style         = CS_NOCLOSE;
  wc.lpfnWndProc   = WindowProc;
  wc.cbClsExtra    = 0;
  wc.cbWndExtra    = 0;
  wc.hInstance     = GetModuleHandle(NULL);
  wc.hIcon         = NULL;
  wc.hCursor       = NULL;
  wc.hbrBackground = NULL;
  wc.lpszMenuName  = NULL;
  wc.lpszClassName = window_class_name;

  ATOM atomWindowClass = RegisterClass(&wc);
  if(!atomWindowClass) {
    DWORD gle = GetLastError();
    cerr << "RegisterClass() failed to make window class "
         << "\"" << window_class_name << "\""
         << " with error code " << gle << "." << endl;
    return NULL;
  }

  HWND hwnd = CreateWindowEx(0, window_class_name, window_class_name,
                             0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
  if(!hwnd) {
    DWORD gle = GetLastError();
    cerr << "CreateWindowEx() failed to make window "
         << "\"" << window_class_name << "\""
         << " with error code " << gle << "." << endl;
    return NULL;
  }

  if(verbose >= 3) {
    cout << "\n--- " << TimeToLocalTimeStr(time(NULL)) << " ---\n"
         << "Monitor window created.\n"
         << "hwnd: " << hex << hwnd << dec << "\n"
         << "name: \"" << window_class_name << "\"\n" << endl;
  }

  return hwnd;
}

void ShowUsage()
{
cerr <<
"\nUsage: battstatus [-p] [-v[vv]]\n"
"\n"
"battstatus monitors your laptop battery for changes in state. By default it "
"monitors WM_POWERBROADCAST messages and changes in the system power status "
"to the battery charge status and percentage remaining.\n"
"\n"
"  -v\tMonitor and show all power status variables on any change.\n"
"\n"
"  -vv\t.. and show ??? (unused).\n"
"\n"
"  -vvv\t.. and show all window messages received by the monitor window.\n"
"\tWindow messages other than WM_POWERBROADCAST are shown by hex.\n"
"\n"
"  -p\tPrevent Sleep: Prevent the computer from sleeping while monitoring.\n"
"\tThis option changes the monitor thread's power request state so that the "
"system can stay in a working state (aka 'SYSTEM') and enter away mode "
"(aka 'AWAYMODE') instead of true sleep. Note it doesn't seem to prevent a "
"manual sleep initiated by the user when unplugged and running on battery "
"power.\n"
"\n"
"Options combined into a single argument are the same as separate options, "
"for example -pvv is the same as -p -v -v.\n"
"\n"
"Also, Windows has its own diagnostics tool that is helpful: powercfg /?\n"
;
}

int main(int argc, char *argv[])
{
  os.dwOSVersionInfoSize = sizeof os;
  RtlGetVersion(&os);

  for(int i = 1; i < argc; ++i) {
    char *p = argv[i];
    if(!strcmp(p, "--help")) {
        ShowUsage();
        exit(1);
    }
    if(*p != '-' && *p != '/') {
      cerr << "Error: Option parsing failed, expected - or / : " << p << endl;
      exit(1);
    }
    while(*++p) {
      switch(*p) {
      case 'h':
      case '?':
        ShowUsage();
        exit(1);
      case 'p':
        prevent_sleep = true;
        break;
      case 'v':
        ++verbose;
        break;
      default:
        cerr << "Error: Option parsing failed, unknown option: " << *p << endl;
        exit(1);
      }
    }
  }

  if(prevent_sleep) {
    /* "The SetThreadExecutionState function cannot be used to prevent the user
       from putting the computer to sleep." However these flags below get us
       pretty close. It's still possible if on battery power for the user to
       manually initiate a true sleep though.
       ES_AWAYMODE_REQUIRED - Use away mode (instead of true sleep).
       ES_CONTINUOUS - Flags set here should remain in effect until next call.
       ES_SYSTEM_REQUIRED - Reset system idle timer to force a working state.
       */
    SetThreadExecutionState(ES_AWAYMODE_REQUIRED | ES_CONTINUOUS |
                            ES_SYSTEM_REQUIRED);
  }

  HWND hwnd = InitMonitorWindow();
  if(!hwnd) {
    cerr << "InitMonitorWindow() failed." << endl;
    exit(1);
  }

#if 0 // testing purposes
  WindowProc(hwnd, WM_POWERBROADCAST,
             PBT_APMQUERYSUSPEND,
             0x44);//PBTF_APMRESUMEFROMFAILURE);
#endif

  SYSTEM_POWER_STATUS prev_status = { 0, };
  for(;; Sleep(100)) {
    MSG msg;
    while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if(msg.message == WM_QUIT)
        exit((int)msg.wParam);
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    SYSTEM_POWER_STATUS status;
    if(!GetSystemPowerStatus(&status)) {
      DWORD gle = GetLastError();
      cerr << "GetSystemPowerStatus() failed, GetLastError(): " << gle << endl;
      Sleep(1000);
      continue;
    }

    if(verbose) {
      if(ComparePowerStatus(&prev_status, &status) == CPS_EQUAL)
        continue;

      cout << "\n--- " << TimeToLocalTimeStr(time(NULL)) << " ---\n";
      ShowPowerStatus(&status);
      LONG mW = GetBatteryMilliwatts();
      if(mW) {
        stringstream ss;
        ss << left << setw(BATT_FIELD_WIDTH)
           << (mW < 0 ? "Battery discharge: " : "Battery charge: ")
           << showpos << mW << "mW";
        cout << ss.str() << endl;
      }
      cout << endl;
      prev_status = status;
      continue;
    }

    bool nobatt = !!(status.BatteryFlag & SPSF_BATTERYNOBATTERY);
    bool prev_nobatt = !!(prev_status.BatteryFlag & SPSF_BATTERYNOBATTERY);
    bool battsaver = status.Reserved1;
    bool prev_battsaver = prev_status.Reserved1;
    bool charging = !!(status.BatteryFlag & SPSF_BATTERYCHARGING);
    bool prev_charging = !!(prev_status.BatteryFlag & SPSF_BATTERYCHARGING);

    if(os.dwMajorVersion >= 10 && battsaver != prev_battsaver) {
      cout << "[" << TimeToLocalTimeStr(time(NULL)) << "]: "
           << SystemStatusFlagStr(status.Reserved1) << endl;
    }

    /* continue if state is the same. note that battery time remaining isn't
       checked here since it's much more volatile than percentage remaining.
       it is checked in verbose mode though. */
    if(nobatt == prev_nobatt &&
       charging == prev_charging &&
       status.BatteryLifePercent == prev_status.BatteryLifePercent)
      continue;

    cout << "[" << TimeToLocalTimeStr(time(NULL)) << "]: ";
    // Show the status in the same formats that the battery systray uses
    if(nobatt) {
      // eg: No battery is detected
      cout << "No battery is detected";
    }
    else if(charging) {
      // eg: 100% available (plugged in, charging)
      cout << BatteryLifePercentStr(status.BatteryLifePercent)
           << " available ("
           << (status.ACLineStatus == 1 ? "plugged in, " : "")
           << "charging)";
    }
    /* BatteryLifeTime is "–1 if remaining seconds are unknown or if the
       device is connected to AC power." */
    else if(status.BatteryLifeTime == (DWORD)-1) {
      if(status.BatteryLifePercent == 100 && !GetBatteryMilliwatts()) {
        // eg: Fully charged (100%)
        cout << "Fully charged ("
             << BatteryLifePercentStr(status.BatteryLifePercent) << ")";
      }
      else {
        // eg: 100% remaining
        cout << BatteryLifePercentStr(status.BatteryLifePercent)
             << " remaining";
      }
    }
    else {
      // eg: 27 min (15%) remaining
      cout << BatteryLifeTimeStr(status.BatteryLifeTime) << " ("
           << BatteryLifePercentStr(status.BatteryLifePercent)
           << ") remaining";
    }
    cout << endl;

    prev_status = status;
  }
}
