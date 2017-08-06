/* battstatus - Monitor the Windows battery status for changes in state

Sample output:

[Wed Aug 02 12:15:55 PM]: 5 hr 30 min (99%) remaining
[Wed Aug 02 12:17:38 PM]: WM_POWERBROADCAST: PBT_APMPOWERSTATUSCHANGE
[Wed Aug 02 12:17:38 PM]: 99% available (plugged in, not charging)
[Wed Aug 02 12:17:44 PM]: Fully charged (100%)
[Wed Aug 02 12:41:50 PM]: WM_POWERBROADCAST: PBT_APMPOWERSTATUSCHANGE
[Wed Aug 02 12:41:50 PM]: 99% remaining
[Wed Aug 02 12:45:14 PM]: 8 hr 13 min (98%) remaining
[Wed Aug 02 12:49:39 PM]: 7 hr 37 min (97%) remaining

It can optionally show verbose information and prevent sleep. Use option --help
to see the usage information.

g++ -std=c++11 -o battstatus battstatus.cpp -lntdll -lpowrprof

https://github.com/jay/battstatus
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

unsigned verbose;
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
  if(BatteryLifePercent <= 100)
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
1 hr 01 min; 1 hr 00 min; 1 min or "Unknown" if BatteryLifeTime is -1.
*/
string BatteryLifeTimeStr(DWORD BatteryLifeTime)
{
  if(BatteryLifeTime == (DWORD)-1)
    return "Unknown";

  DWORD hours = BatteryLifeTime / 3600;
  DWORD minutes = (BatteryLifeTime % 3600) / 60;

  stringstream ss;
  if(hours) {
    ss << hours << " hr " << setw(2);
    ss.fill('0');
  }
  ss << minutes << " min";
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

/* The timestamp style in verbose mode:
--- Sun May 28 07:00:55 PM ---
text
*/
#define TIMESTAMPED_HEADER \
  "\n--- " << TimeToLocalTimeStr(time(NULL)) << " ---\n"

/* The timestamp style in default mode: [Sun May 28 07:00:27 PM]: text */
#define TIMESTAMPED_PREFIX \
  "[" << TimeToLocalTimeStr(time(NULL)) << "]: "

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

#define FUNC_SHOW_BOOL(item) \
string item##Str(BOOL item) \
{ \
  stringstream ss; \
  ss << (item == TRUE ? "TRUE" : \
         (item == FALSE ? "FALSE" : \
          UndocumentedValueStr(item))); \
  return ss.str(); \
}

FUNC_SHOW_BOOL(AcOnLine);
FUNC_SHOW_BOOL(BatteryPresent);
FUNC_SHOW_BOOL(Charging);
FUNC_SHOW_BOOL(Discharging);

string mWhStr(DWORD mWh)
{
  stringstream ss;
  ss << mWh << "mWh";;
  return ss.str();
}

string MaxCapacityStr(DWORD MaxCapacity)
{
  return mWhStr(MaxCapacity);
}

string RemainingCapacityStr(DWORD RemainingCapacity)
{
  return mWhStr(RemainingCapacity);
}

string RateStr(DWORD Rate)
{
  /* Rate: "The current rate of discharge of the battery, in mW. A nonzero,
     positive rate indicates charging; a negative rate indicates discharging.
     Some batteries report only discharging rates. This value should be treated
     as a LONG as it can contain negative values (with the high bit set)."
     However when some of my batteries charge the Rate is:
     0x80000000 == -2147483648 (LONG) == 2147483648 (DWORD)
     When my batteries are removed the Rate is 0. */
  if(!Rate || Rate == 0x80000000)
    return "Unknown";

  stringstream ss;
  ss << showpos << (LONG)Rate << "mW";
  return ss.str();
}

string RateStr(LONG Rate)
{
  return RateStr((DWORD)Rate);
}

string EstimatedTimeStr(DWORD EstimatedTime)
{
  return BatteryLifeTimeStr(EstimatedTime);
}

/* DefaultAlert1:
"The manufacturer's suggestion of a capacity, in mWh, at which a low battery
alert should occur."
*/
string DefaultAlert1Str(DWORD DefaultAlert1)
{
  return mWhStr(DefaultAlert1);
}

/* DefaultAlert2:
"The manufacturer's suggestion of a capacity, in mWh, at which a warning
battery alert should occur."
*/
string DefaultAlert2Str(DWORD DefaultAlert2)
{
  return mWhStr(DefaultAlert2);
}

void ShowBatteryState(const SYSTEM_BATTERY_STATE *state)
{
#define SHOW_STATE(item) \
  cout << left << setw(BATT_FIELD_WIDTH) << #item ": " \
       << right << item##Str(state->item) << "\n";

  SHOW_STATE(AcOnLine);
  SHOW_STATE(BatteryPresent);
  SHOW_STATE(Charging);
  SHOW_STATE(Discharging);
  SHOW_STATE(MaxCapacity);
  SHOW_STATE(RemainingCapacity);
  SHOW_STATE(Rate);
  SHOW_STATE(EstimatedTime);
  SHOW_STATE(DefaultAlert1);
  SHOW_STATE(DefaultAlert2);
  cout << flush;
}

/* Return the battery power rate in mW.
A negative rate means discharging and a positive rate means charging.
0 means neither charging nor discharging.
Ignore errors: Don't show them and return 0.
*/
LONG GetBatteryPowerRate()
{
  /* Note SYSTEM_BATTERY_STATE seems to be updated by the OS at the same
     frequency as SYSTEM_POWER_STATUS, which is not necessarily that often. */
  SYSTEM_BATTERY_STATE sbs;
  if(CallNtPowerInformation(SystemBatteryState, NULL, 0, &sbs, sizeof sbs))
    return 0;
  /* As described in RateStr(), 0x80000000 is an invalid value and any other
     value should be converted to LONG. */
  return ((DWORD)sbs.Rate != 0x80000000) ? (LONG)sbs.Rate : 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if(verbose >= 3)
  {
    /* show all window messages */
    cout << TIMESTAMPED_PREFIX
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
    cout << TIMESTAMPED_PREFIX << "WM_POWERBROADCAST: ";
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
        cout << "Bit 0 is on, User prompting/interaction is allowed.";
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
    cout << TIMESTAMPED_HEADER
         << "Monitor window created.\n"
         << "hwnd: " << hex << hwnd << dec << "\n"
         << "name: " << window_class_name << "\n" << endl;
  }

  return hwnd;
}

void ShowUsage()
{
cerr <<
"\nUsage: battstatus [-p] [-v[vv]]\n"
"\n"
"battstatus monitors your laptop battery for changes in state. By default it "
"monitors WM_POWERBROADCAST messages and relevant changes in power status.\n"
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
"The battstatus source can be found at https://github.com/jay/battstatus\n"
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
    if(*p != '-') {
      cerr << "Error: Option parsing failed, expected '-' : " << p << endl;
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
    if(!SetThreadExecutionState(ES_AWAYMODE_REQUIRED | ES_CONTINUOUS |
                                ES_SYSTEM_REQUIRED)) {
      cerr << "SetThreadExecutionState failed to prevent sleep." << endl;
      exit(1);
    }
  }

  /* in verbose mode show all SYSTEM_BATTERY_STATE members */
  if(verbose) {
    SYSTEM_BATTERY_STATE sbs = { 0, };
    NTSTATUS status = CallNtPowerInformation(SystemBatteryState,
                                             NULL, 0, &sbs, sizeof sbs);
    if(status == STATUS_SUCCESS) {
      cout << TIMESTAMPED_HEADER;
      ShowBatteryState(&sbs);
      if(verbose >= 3) {
        cout << "DefaultAlert1 is the manufacturer's suggested alert level "
                "for 'Low'.\n"
                "DefaultAlert2 is the manufacturer's suggested alert level "
                "for 'Warning'." << endl;
      }
    }
    else {
      cout << "Warning: CallNtPowerInformation failed to retrieve "
              "SystemBatteryState with error code ";
      switch(status) {
      case STATUS_BUFFER_TOO_SMALL:
        cout << "STATUS_BUFFER_TOO_SMALL";
        break;
      case STATUS_ACCESS_DENIED:
        cout << "STATUS_ACCESS_DENIED";
        break;
      default:
        cout << hex << "0x" << status << dec;
        break;
      }
      cout << "." << endl;
    }
    cout << endl;
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

#define PROCESS_WINDOW_MESSAGES() \
  for(MSG msg; PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);) { \
    if(msg.message == WM_QUIT) \
      exit((int)msg.wParam); \
    TranslateMessage(&msg); \
    DispatchMessage(&msg); \
  }

  SYSTEM_POWER_STATUS prev_status = { 0, };
  SYSTEM_POWER_STATUS status = { 0, };
  for(;; Sleep(100), prev_status = status) {
    PROCESS_WINDOW_MESSAGES();

    if(!GetSystemPowerStatus(&status)) {
      DWORD gle = GetLastError();
      cerr << "GetSystemPowerStatus() failed, GetLastError(): " << gle << endl;
      status = prev_status;
      for(int i = 0; i < 10; ++i) {
        Sleep(100);
        PROCESS_WINDOW_MESSAGES();
      }
      continue;
    }

    PROCESS_WINDOW_MESSAGES();

    bool full_status_shown = false;

    /* in verbose mode if SYSTEM_POWER_STATUS has changed show it in full and
       show the battery power rate */
    if(verbose &&
       ComparePowerStatus(&prev_status, &status) == CPS_NOTEQUAL) {
      cout << TIMESTAMPED_HEADER;
      ShowPowerStatus(&status);
      cout << left << setw(BATT_FIELD_WIDTH) << "Battery Power Rate: "
           << right << RateStr(GetBatteryPowerRate()) << "\n";
      cout << endl;
      full_status_shown = true;
    }

    /* Default monitor mode.
       Compare a subset of SYSTEM_POWER_STATUS to determine when the relevant
       state has changed, in order to show an updated power status.
       Note battery percent remaining is compared instead of time remaining
       since the latter is volatile and could cause a lot of updates. */

#define BATTSAVER(status)   ((status).Reserved1 == 1)
#define CHARGING(status)    (!!((status).BatteryFlag & SPSF_BATTERYCHARGING))
#define NO_BATTERY(status)  (!!((status).BatteryFlag & SPSF_BATTERYNOBATTERY))
#define PLUGGED_IN(status)  ((status).ACLineStatus == 1)

    /* Check if the battery saver status has changed.
       The battery saver status is available since Windows 10. It's stored in
       member SystemStatus but older headers use the name Reserved1. */
    if(os.dwMajorVersion >= 10 &&
       BATTSAVER(status) != BATTSAVER(prev_status)) {
      cout << TIMESTAMPED_PREFIX
           << SystemStatusFlagStr(status.Reserved1) << endl;
    }

    if(!full_status_shown &&
       status.BatteryLifePercent == prev_status.BatteryLifePercent &&
       CHARGING(status) == CHARGING(prev_status) &&
       NO_BATTERY(status) == NO_BATTERY(prev_status) &&
       PLUGGED_IN(status) == PLUGGED_IN(prev_status))
      continue;

    cout << TIMESTAMPED_PREFIX;
    // Show the status in the same formats that the battery systray uses
    if(NO_BATTERY(status)) {
      // eg: No battery is detected
      cout << "No battery is detected";
    }
    else if(status.BatteryLifePercent == 100 &&
            status.BatteryLifeTime == (DWORD)-1 &&
            PLUGGED_IN(status) &&
            !CHARGING(status) &&
            !GetBatteryPowerRate()) {
      // eg: Fully charged (100%)
      cout << "Fully charged (" << BatteryLifePercentStr(100) << ")";
    }
    else if(CHARGING(status) || PLUGGED_IN(status)) {
      // eg: 100% available (plugged in, charging)
      // eg: 99% available (plugged in, not charging)
      cout << BatteryLifePercentStr(status.BatteryLifePercent)
           << (GetBatteryPowerRate() < 0 ? " remaining" : " available") << " ("
           << (PLUGGED_IN(status) ? "" : "not ") << "plugged in, "
           << (CHARGING(status) ? "" : "not ") << "charging)";
    }
    /* BatteryLifeTime is "-1 if remaining seconds are unknown or if the
       device is connected to AC power." */
    else if(status.BatteryLifeTime == (DWORD)-1) {
      // eg: 100% remaining
      cout << BatteryLifePercentStr(status.BatteryLifePercent) << " remaining";
    }
    else {
      // eg: 27 min (15%) remaining
      cout << BatteryLifeTimeStr(status.BatteryLifeTime) << " ("
           << BatteryLifePercentStr(status.BatteryLifePercent)
           << ") remaining";
    }
    cout << endl;
  }
}
