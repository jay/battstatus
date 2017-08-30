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

To build using Visual Studio 2008 (with TR1 support) or later:
cl /W4 /wd4127 battstatus.cpp user32.lib powrprof.lib setupapi.lib

To build using MinGW or MinGW-w64:
g++ -Wall -std=gnu++11 -o battstatus battstatus.cpp -lpowrprof -lsetupapi -luuid

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

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#ifdef __MINGW32__
#include <_mingw.h>
#ifdef __MINGW64_VERSION_MAJOR
#include <batclass.h>
#include <ntstatus.h>
#else
#include <ddk/batclass.h>
#include <ddk/ntddk.h>
#endif
#else
#include <batclass.h>
#include <ntstatus.h>
#endif

#include <devguid.h>
#include <powrprof.h>
#include <setupapi.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

#include <deque>
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

// Bool macros for SYSTEM_POWER_STATUS
#define BATTSAVER(status)   ((status).Reserved1 == 1)
#define CHARGING(status)    (!!((status).BatteryFlag & SPSF_BATTERYCHARGING))
#define NO_BATTERY(status)  (!!((status).BatteryFlag & SPSF_BATTERYNOBATTERY))
#define PLUGGED_IN(status)  ((status).ACLineStatus == 1)

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

/* In Visual Studio 2008 we need the TR1 functions in std::tr1, which is only
   available if its optional feature pack or SP1 is installed. */
#if _MSC_VER == 1500
using namespace std::tr1;
#endif

unsigned verbose;
bool prevent_sleep;
RTL_OSVERSIONINFOW os;

/* There are certain times when the battery charge state should be suppressed,
   such as when it continually changes in a short period of time and verbose
   mode is disabled. */
bool suppress_charge_state;

/* There are certain times when the battery lifetime should be suppressed, such
   as when the computer just woke up. */
bool suppress_lifetime;

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
  "\n--- " << TimeToLocalTimeStr(time(NULL)).c_str() << " ---\n"

/* The timestamp style in default mode: [Sun May 28 07:00:27 PM]: text */
#define TIMESTAMPED_PREFIX \
  "[" << TimeToLocalTimeStr(time(NULL)).c_str() << "]: "

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
  // can't use conditional class here since VS2008 doesn't have it
  if(is_signed<char>::value)
    return UndocumentedValueStr<signed>(undocumented_value);
  else
    return UndocumentedValueStr<unsigned>(undocumented_value);
}

string UndocumentedValueStr(unsigned char undocumented_value)
{
  return UndocumentedValueStr<unsigned>(undocumented_value);
}

string UndocumentedValueStr(signed char undocumented_value)
{
  return UndocumentedValueStr<signed>(undocumented_value);
}

/* Relative capacity and rate:
   According to BATTERY_INFORMATION documentation the capacity and rate
   information reported by a battery may be relative, with all rate information
   reported in units per hour.
   "If [flag BATTERY_CAPACITY_RELATIVE] is set, all references to units in
   the other battery documentation can be ignored."
   That flag is set per battery, however most of this program looks at overall
   battery use in structures where that information is not available, so in
   those cases we treat it as unknown.
   https://msdn.microsoft.com/en-us/library/windows/desktop/aa372661.aspx */

enum CapacityType
{
  CAPACITY_TYPE_UNKNOWN,
  CAPACITY_TYPE_RELATIVE,
  CAPACITY_TYPE_MILLIWATT_HOUR
};

enum RateType
{
  RATE_TYPE_UNKNOWN,
  RATE_TYPE_RELATIVE,
  RATE_TYPE_MILLIWATT
};

string CapacityStr(DWORD unit, enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  stringstream ss;

  ss << unit;
  if(ct == CAPACITY_TYPE_RELATIVE)
    ss << " (relative)";
  else if(ct == CAPACITY_TYPE_MILLIWATT_HOUR)
    ss << "mWh";
  else
    ss << "mWh (or relative)";

  return ss.str();
}

string RateStr(DWORD unit, enum RateType rt = RATE_TYPE_UNKNOWN)
{
  stringstream ss;

  if(rt == RATE_TYPE_RELATIVE)
    ss << unit << " (relative)";
  else {
    /* Rate as described by SYSTEM_BATTERY_STATE (other rates may differ):
       "The current rate of discharge of the battery, in mW. A nonzero,
       positive rate indicates charging; a negative rate indicates discharging.
       Some batteries report only discharging rates. This value should be
       treated as a LONG as it can contain negative values (with the high bit
       set)."
       However when some of my batteries charge the Rate is:
       0x80000000 == -2147483648 (LONG) == 2147483648 (DWORD)
       When my batteries are removed the Rate is 0. */
    if(unit == 0 || unit == 0x80000000)
      return "Unknown";

    ss << showpos << (LONG)unit;
    if(rt == RATE_TYPE_MILLIWATT)
      ss << "mW";
    else
      ss << "mW (or relative)";
  }

  return ss.str();
}

string RateStr(LONG Rate, enum RateType rt = RATE_TYPE_UNKNOWN)
{
  return RateStr((DWORD)Rate, rt);
}

string CapabilitiesStr(ULONG Capabilities)
{
  if(Capabilities == 0)
    return "<none>";

  stringstream ss;
#define EXTRACT_CAPABILITIES(flag) \
  if((Capabilities & flag)) { \
    if(ss.tellp()) \
      ss << " | "; \
    ss << #flag; \
    Capabilities &= ~flag; \
  }
  EXTRACT_CAPABILITIES(BATTERY_CAPACITY_RELATIVE);
  EXTRACT_CAPABILITIES(BATTERY_IS_SHORT_TERM);
  EXTRACT_CAPABILITIES(BATTERY_SET_CHARGE_SUPPORTED);
  EXTRACT_CAPABILITIES(BATTERY_SET_DISCHARGE_SUPPORTED);
  EXTRACT_CAPABILITIES(BATTERY_SYSTEM_BATTERY);
  if(Capabilities) {
    if(ss.tellp())
      ss << " | ";
    ss << UndocumentedValueStr(Capabilities);
  }
  return ss.str();
}

string TechnologyStr(UCHAR Technology)
{
  switch(Technology)
  {
  case 0: return "Nonrechargeable";
  case 1: return "Rechargeable";
  }
  return UndocumentedValueStr(Technology);
}

string ChemistryStr(const UCHAR Chemistry[4])
{
  return string((char *)Chemistry, sizeof Chemistry);
}

string DesignedCapacityStr(ULONG DesignedCapacity,
                           enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  return CapacityStr(DesignedCapacity, ct);
}

string FullChargedCapacityStr(ULONG FullChargedCapacity,
                              enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  return CapacityStr(FullChargedCapacity, ct);
}

/* DefaultAlert1:
"The manufacturer's suggestion of a capacity, in mWh, at which a low battery
alert should occur."
*/
string DefaultAlert1Str(DWORD DefaultAlert1,
                        enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  return CapacityStr(DefaultAlert1, ct);
}

/* DefaultAlert2:
"The manufacturer's suggestion of a capacity, in mWh, at which a warning
battery alert should occur."
*/
string DefaultAlert2Str(DWORD DefaultAlert2,
                        enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  return CapacityStr(DefaultAlert2, ct);
}

string CriticalBiasStr(ULONG CriticalBias,
                        enum CapacityType ct = CAPACITY_TYPE_UNKNOWN)
{
  return CapacityStr(CriticalBias, ct);
}

string CycleCountStr(ULONG CycleCount)
{
  stringstream ss;
  ss << CycleCount;
  return ss.str();
}

string BatteryInformationStr(const BATTERY_INFORMATION *bi)
{
  stringstream ss;

  enum CapacityType ct = ((bi->Capabilities & BATTERY_CAPACITY_RELATIVE) ?
                          CAPACITY_TYPE_RELATIVE :
                          CAPACITY_TYPE_MILLIWATT_HOUR);

#define BATTINFO_PRELUDE(item) \
  ss << left << setw(BATT_FIELD_WIDTH) << #item ": " << right;

#define SHOW_BATTINFO(item) \
  BATTINFO_PRELUDE(item); \
  ss << item##Str(bi->item) << "\n";

#define SHOW_BATTINFO_CAPACITY_MEMBER(item) \
  BATTINFO_PRELUDE(item); \
  ss << item##Str(bi->item, ct) << "\n";

  SHOW_BATTINFO(Capabilities);
  SHOW_BATTINFO(Technology);
  SHOW_BATTINFO(Chemistry);
  SHOW_BATTINFO_CAPACITY_MEMBER(DesignedCapacity);
  SHOW_BATTINFO_CAPACITY_MEMBER(FullChargedCapacity);
  SHOW_BATTINFO_CAPACITY_MEMBER(DefaultAlert1);
  SHOW_BATTINFO_CAPACITY_MEMBER(DefaultAlert2);
  SHOW_BATTINFO_CAPACITY_MEMBER(CriticalBias);
  SHOW_BATTINFO(CycleCount);

  return ss.str();
}

void ShowBatteryInformation(const BATTERY_INFORMATION *bi)
{
  cout << BatteryInformationStr(bi) << flush;
}

string ManufactureDateStr(BATTERY_MANUFACTURE_DATE *mnfctr_date)
{
  stringstream ss;

  if(!mnfctr_date->Year)
    return "Unknown";

  ss.fill('0');
  ss << setw(4) << (unsigned)mnfctr_date->Year << "-"
     << setw(2) << (unsigned)mnfctr_date->Month << "-"
     << setw(2) << (unsigned)mnfctr_date->Day;

  return ss.str();
}

// this is the input for EnumBattInterfacesProc
struct device {
  /* slot always has a valid number. Any other member may be valid. */
  unsigned slot;  // battery interface number  (starts at 0, sequential)
  HANDLE handle;  // battery interface handle  (invalid: INVALID_HANDLE_VALUE)
  wchar_t *path;  // battery interface path  (opens handle)
};

/* this is the output for EnumBattInterfacesProc

MSDN says battery tags are not unique between battery device interfaces
(slots), therefore more than one slot may have a battery with the same tag.
Furthermore the tag may change even if the battery hasn't, and when that
happens "all cached data should be re-read".

To see if the same physical battery is present compare unique_id, not tag.

https://msdn.microsoft.com/en-us/library/windows/desktop/aa372659.aspx
*/
struct battery {
  /* 'success' signifies that all requested information was obtained from the
     battery. Unless otherwise noted any member may be valid even if success is
     false. To determine if a battery is present in the slot check if
     tag != BATTERY_TAG_INVALID. */
  bool success;
  ULONG tag;                 // battery tag  (invalid: BATTERY_TAG_INVALID)
  wchar_t *unique_id;        // string that uniquely identifies the battery
  wchar_t *path;             // battery interface path
  // battery manufacture date (unknown: 0000-00-00)
  BATTERY_MANUFACTURE_DATE mnfctr_date;
  BATTERY_INFORMATION info;  // battery info  (invalid: success is false)
  double health;             // percentage of full capacity vs design capacity
};

typedef BOOL (CALLBACK* BATTINTENUMPROC)(const struct device *device,
                                         void *cbdata);

/* Get battery info for each battery.

Pass a pointer to an _empty_ vector<battery> as cbdata.

This is called by EnumBattInterfaces once for each battery interface without
skipping any inaccessible devices, starting at 0 until the last interface.

After this function returns the device resources are freed. If you'll need to
reopen the battery interface later, make a copy of the path.

TRUE:  Continue on to the next interface.
FALSE: Stop enumerating interfaces; do not call this function again.
*/
BOOL CALLBACK EnumBattInterfacesProc(const struct device *device,
                                     void *cbdata)
{
  DWORD bytes_written;

  vector<battery> *batteries = (vector<battery> *)cbdata;

  batteries->push_back(battery());
  struct battery *battery = &batteries->back();
  battery->tag = BATTERY_TAG_INVALID;

  if(!device->path || device->handle == INVALID_HANDLE_VALUE)
    return TRUE; // interface inaccessible, continue on

  battery->path = _wcsdup(device->path);
  if(!battery->path) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }

  // How long to wait for the interface to return a battery tag
  ULONG wait = 0;

  if(!DeviceIoControl(device->handle, IOCTL_BATTERY_QUERY_TAG,
                      &wait, sizeof(wait),
                      &battery->tag, sizeof(battery->tag),
                      &bytes_written, NULL)) {
    return TRUE; // battery not found, continue on
  }

  BATTERY_QUERY_INFORMATION bqi = { battery->tag };

  bqi.InformationLevel = BatteryUniqueID;

  wchar_t buffer[1024];
  if(!DeviceIoControl(device->handle, IOCTL_BATTERY_QUERY_INFORMATION,
                      &bqi, sizeof(bqi),
                      buffer, sizeof(buffer),
                      &bytes_written, NULL)) {
    return TRUE; // unique id string not found or too long, continue on
  }

  battery->unique_id = _wcsdup(buffer);
  if(!battery->unique_id) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }

  bqi.InformationLevel = BatteryManufactureDate;

  if(!DeviceIoControl(device->handle, IOCTL_BATTERY_QUERY_INFORMATION,
                      &bqi, sizeof(bqi),
                      &battery->mnfctr_date, sizeof(battery->mnfctr_date),
                      &bytes_written, NULL)) {
    // assume manufacture date unknown (0000-00-00)
    memset(&battery->mnfctr_date, 0, sizeof battery->mnfctr_date);
  }

  bqi.InformationLevel = BatteryInformation;

  if(!DeviceIoControl(device->handle, IOCTL_BATTERY_QUERY_INFORMATION,
                      &bqi, sizeof(bqi),
                      &battery->info, sizeof(battery->info),
                      &bytes_written, NULL)) {
    memset(&battery->info, 0, sizeof battery->info);
    return TRUE; // battery info isn't accessible, continue on
  }

  if(!battery->info.FullChargedCapacity ||
     battery->info.FullChargedCapacity == (ULONG)-1)
    battery->health = 0;
  else if(!battery->info.DesignedCapacity ||
          battery->info.DesignedCapacity == (ULONG)-1 ||
          battery->info.FullChargedCapacity >= battery->info.DesignedCapacity)
    battery->health = 100;
  else {
    battery->health = 100 * ((double)battery->info.FullChargedCapacity /
                             battery->info.DesignedCapacity);
  }

  battery->success = true;
  return TRUE;
}

/* Enumerate the battery device interfaces.

EnumProc should be called by this function once for each battery interface
without skipping any inaccessible devices, starting at 0 until the last
interface.

TRUE:  EnumProc was called and returned TRUE for all interfaces.
FALSE: No interfaces found, out of memory or EnumProc returned FALSE.
*/
BOOL WINAPI EnumBattInterfaces(BATTINTENUMPROC EnumProc, void *cbdata)
{
  HDEVINFO hdev = SetupDiGetClassDevs(&GUID_DEVCLASS_BATTERY, 0, 0,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if(hdev == INVALID_HANDLE_VALUE)
    return FALSE;

  DWORD idev = 0;
  for(; idev < 100; ++idev) {
    struct device device;

    device.slot = idev;
    device.handle = INVALID_HANDLE_VALUE;
    device.path = NULL;

    SP_DEVICE_INTERFACE_DATA did = { sizeof did, };

    if(SetupDiEnumDeviceInterfaces(hdev, NULL, &GUID_DEVCLASS_BATTERY, idev,
                                   &did)) {
      DWORD cbRequired = 0;

      if(!SetupDiGetDeviceInterfaceDetailW(hdev, &did, NULL, 0,
                                           &cbRequired, 0) &&
         GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pdidd =
          (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, cbRequired);

        if(!pdidd) {
          SetLastError(ERROR_NOT_ENOUGH_MEMORY);
          return FALSE;
        }

        pdidd->cbSize = sizeof(*pdidd);

        if(SetupDiGetDeviceInterfaceDetailW(hdev, &did, pdidd, cbRequired,
                                            &cbRequired, 0)) {
          device.path = _wcsdup(pdidd->DevicePath);

          if(!device.path) {
            free(pdidd);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
          }

          device.handle = CreateFileW(device.path,
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      NULL);
        }

        free(pdidd);
      }
    }
    else if(GetLastError() == ERROR_NO_MORE_ITEMS)
      break;

    BOOL rc = EnumProc(&device, cbdata);
    DWORD gle = GetLastError();

    free(device.path);

    if(device.handle != INVALID_HANDLE_VALUE)
      CloseHandle(device.handle);

    if(!rc) {
      SetLastError(gle);
      return FALSE;
    }
  }

  return TRUE;
}

void ShowIndividualBatteryHealth()
{
  const char *borderline = "========================================="
                           "======================================\n";
  const char *sepline =    "-----------------------------------------"
                           "--------------------------------------\n";

  wstringstream wss;

wss << borderline <<
"Individual Battery Health:\n"
"\n"
"This program is designed to monitor the overall combined battery capacity,\n"
"however what follows is the percentage of how much capacity each individual\n"
"battery is currently able to store (full capacity) versus how much capacity\n"
"it was initially able to store (design capacity), also known as health. A\n"
"battery will lose health the more charge cycles it is put through. Other\n"
"factors affect health such as the method of charging. For example, in my\n"
"experience working on a number of Dell Latitudes, the ExpressCharge feature\n"
"can reduce health faster than normal.\n";

  vector<battery> batteries;
  EnumBattInterfaces(EnumBattInterfacesProc, &batteries);

  unsigned batteries_present = 0;

  for(DWORD i = 0; i < batteries.size(); ++i, wss << sepline) {
    wss << "\n" << sepline;
    wss << "Slot #" << i << ": "
        << (batteries[i].path ? batteries[i].path : L"(inaccessible)") << "\n";

    if(batteries[i].tag == BATTERY_TAG_INVALID) {
      wss << "(empty)\n";
      continue;
    }

    ++batteries_present;

    if(!batteries[i].success) {
      wss << "(inaccessible)\n";
      continue;
    }

    wss << "\n\"" << batteries[i].unique_id << "\" is at "
        << std::fixed << setprecision(2)
        << batteries[i].health << "% health\n";

    wss << "\n" << BatteryInformationStr(&batteries[i].info).c_str();

    wss << left << setw(BATT_FIELD_WIDTH) << "Manufacture Date: "
        << right << ManufactureDateStr(&batteries[i].mnfctr_date).c_str()
        << "\n";
  }

  wss << "\nCounted " << batteries_present << " "
      << (batteries_present == 1 ? "battery" : "batteries") << " "
      << "and " << batteries.size() << " battery interfaces. "
      << "(" << TimeToLocalTimeStr(time(NULL)).c_str() << ")\n";

  wss << "\n" << borderline;
  wcout << endl << wss.str() << endl;
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

/* BatteryLifePercent is "255 if status is unknown." */
#define PERCENT_UNKNOWN ((BYTE)255)

string BatteryLifePercentStr(unsigned BatteryLifePercent)
{
  stringstream ss;
  if(BatteryLifePercent <= 100)
    ss << (DWORD)BatteryLifePercent << "%";
  else if(BatteryLifePercent == PERCENT_UNKNOWN)
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

/* BatteryLifeTime is "-1 if remaining seconds are unknown or if the device
   is connected to AC power." */
#define LIFETIME_UNKNOWN ((DWORD)-1)

/* Format the number of battery life seconds in the same format as the systray:
1 hr 01 min; 1 hr 00 min; 1 min or "Unknown" if LIFETIME_UNKNOWN.
*/
string BatteryLifeTimeStr(DWORD BatteryLifeTime)
{
  if(BatteryLifeTime == LIFETIME_UNKNOWN)
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

string MaxCapacityStr(DWORD MaxCapacity)
{
  return CapacityStr(MaxCapacity);
}

string RemainingCapacityStr(DWORD RemainingCapacity)
{
  return CapacityStr(RemainingCapacity);
}

string EstimatedTimeStr(DWORD EstimatedTime)
{
  return BatteryLifeTimeStr(EstimatedTime);
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
     PBT_APMPOWERSTATUSCHANGE:
     "This event can occur when battery life drops to less than 5 minutes, or
     when the percentage of battery life drops below 10 percent, or if the
     battery life changes by 3 percent."
     Note this is a broadcast message and therefore not received by
     message-only windows. */
  case WM_POWERBROADCAST:

    if(wParam == PBT_APMPOWERSTATUSCHANGE) {
      static SYSTEM_POWER_STATUS status, prev_status;
      prev_status = status;

      if(GetSystemPowerStatus(&status)) {
#if 0
        cout << "PBT_APMPOWERSTATUSCHANGE DEBUG: \n---\n(prev_status)\n";
        ShowPowerStatus(&prev_status);
        cout << "---\n(status)\n";
        ShowPowerStatus(&status);
        cout << "---" << endl;
#endif
        /* If the charge state is being suppressed but only it or members
           affected by it have changed then don't show anything. */
        if(suppress_charge_state &&
           status.BatteryLifePercent == prev_status.BatteryLifePercent &&
           ((status.BatteryFlag & ~SPSF_BATTERYCHARGING) ==
            (prev_status.BatteryFlag & ~SPSF_BATTERYCHARGING)))
          return TRUE;
      }
      else
        status = prev_status;
    }

#define CASE_PBT(item) \
  case item: cout << #item; break;

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
    cerr << "Error: RegisterClass() failed to make window class "
         << "\"" << window_class_name << "\""
         << " with error code " << gle << "." << endl;
    return NULL;
  }

  HWND hwnd = CreateWindowEx(0, window_class_name, window_class_name,
                             0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
  if(!hwnd) {
    DWORD gle = GetLastError();
    cerr << "Error: CreateWindowEx() failed to make window "
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
  NTSTATUS ntstatus;

  /* RtlGetVersion retrieves the real OS info */
  NTSTATUS (NTAPI *RtlGetVersion)(RTL_OSVERSIONINFOW *lpVersionInformation) =
    (NTSTATUS (NTAPI *)(RTL_OSVERSIONINFOW *))
    GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion");

  os.dwOSVersionInfoSize = sizeof os;
  ntstatus = RtlGetVersion(&os);
  if(ntstatus != STATUS_SUCCESS) {
    cerr << "Error: RtlGetVersion failed, error 0x" << hex << ntstatus << endl;
    exit(1);
  }

  /* NtQueryTimerResolution retrieves the OS timer resolutions as 100ns units
     of interrupt. Note that the minimum timer resolution is equal to the
     maximum timer interval, and vice versa. */
  NTSTATUS (NTAPI *NtQueryTimerResolution)(ULONG *MinimumResolution,
                                           ULONG *MaximumResolution,
                                           ULONG *ActualResolution) =
    (NTSTATUS (NTAPI *)(ULONG *, ULONG *, ULONG *))
    GetProcAddress(GetModuleHandleW(L"ntdll"), "NtQueryTimerResolution");

  ULONG MaximumTimerInterval, unused, unused2;
  ntstatus = NtQueryTimerResolution(&MaximumTimerInterval, &unused, &unused2);
  if(ntstatus != STATUS_SUCCESS) {
    cerr << "Error: NtQueryTimerResolution failed, error 0x" << hex << ntstatus
         << endl;
    exit(1);
  }

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
      cerr << "Error: SetThreadExecutionState failed to prevent sleep."
           << endl;
      exit(1);
    }
    if(verbose) {
      cout << "The thread execution state has been changed to prevent sleep."
           << endl;
    }
  }

  if(verbose)
    ShowIndividualBatteryHealth();

  /* in verbose mode show all SYSTEM_BATTERY_STATE members */
  if(verbose) {
    SYSTEM_BATTERY_STATE sbs = { 0, };
    NTSTATUS ntstatus = CallNtPowerInformation(SystemBatteryState,
                                               NULL, 0, &sbs, sizeof sbs);
    if(ntstatus == STATUS_SUCCESS) {
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
      switch(ntstatus) {
      case STATUS_BUFFER_TOO_SMALL:
        cout << "STATUS_BUFFER_TOO_SMALL";
        break;
      case STATUS_ACCESS_DENIED:
        cout << "STATUS_ACCESS_DENIED";
        break;
      default:
        cout << hex << "0x" << ntstatus << dec;
        break;
      }
      cout << "." << endl;
    }
    cout << endl;
  }

  HWND hwnd = InitMonitorWindow();
  if(!hwnd) {
    cerr << "Error: InitMonitorWindow() failed." << endl;
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
      cerr << "Error: GetSystemPowerStatus() failed, GetLastError(): " << gle
           << endl;
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

    /* Detect a battery revival.
       If a battery is in a really bad state then it's possible that the
       battery, the device or the charger will cycle the charger on and off in
       an attempt to slowly revive the battery. A full revival may take a day.
       That can create a lot of noise in the log, so suppress revival
       charges when not verbose.

       In order to detect a revival charge, record the current tick count
       each time the charge state changes and then assume revival if
       'max_changes' number of changes occurred within 'span_minutes'. */
    {
      const unsigned max_changes = 20;
      const unsigned span_minutes = 30;
      DWORD now = GetTickCount();

      // Used like a FIFO for each change's tick count, up to max_changes
      static deque<DWORD> ticks;

      /* Clear all the stored ticks if more than span_minutes has passed since
         the last charge state change. */
      if(ticks.size() && (now - ticks.back()) >= (span_minutes * 60 * 1000))
        ticks.clear();

      if(CHARGING(status) != CHARGING(prev_status)) {
        if(ticks.size() == max_changes)
          ticks.pop_front();

        ticks.push_back(now);
      }

      /* If there's a battery revival taking place then warn. If not verbose
         then also temporarily suppress future charge state changes while the
         revival is taking place so that it won't fill the log with noise. */
      if(ticks.size() == max_changes) {
        DWORD elapsed_minutes = (ticks.back() - ticks.front()) / 1000 / 60;

        if(elapsed_minutes < span_minutes) {
          if(!suppress_charge_state) {
            suppress_charge_state = !verbose;

            if(!verbose || full_status_shown) {
              stringstream ss;
              ss << TIMESTAMPED_PREFIX << "WARNING: ";
              const string &warn = ss.str();

              cout << warn << "Frequent on/off charges are occurring." << endl
                   << warn << "Possible battery revival or bad battery."
                   << endl;

              if(suppress_charge_state)
                cout << warn << "Temporarily ignoring charge state." << endl;
            }
          }
        }
        else
          suppress_charge_state = false;
      }
      else
        suppress_charge_state = false;
    }

    /* Suppress the battery lifetime if less than 'span_minutes' has passed
       since the computer woke up. The battery systray behaves similar but its
       logic for this appears to be more complex. For example, sometimes it can
       wait just a minute to show the lifetime and sometimes it can wait five
       minutes. And those variations do not appear be dependent on percentage,
       which may not have changed in the interim. */
    {
      const unsigned span_minutes = 3;

      ULONGLONG lastwake;
      NTSTATUS ntstatus = CallNtPowerInformation(LastWakeTime, NULL, 0,
                                                 &lastwake, sizeof lastwake);
      if(ntstatus == STATUS_SUCCESS) {
        static ULONGLONG ignore_this_waketime = (ULONGLONG)-1;

        /* Ignore the first retrieved waketime since it most likely occurred
           before this program was started. */
        if(ignore_this_waketime == (ULONGLONG)-1)
          ignore_this_waketime = lastwake;

        if(ignore_this_waketime != lastwake) {
          /* Convert lastwake to milliseconds.  mt is a generous number of
             100ns units of interrupt to remove from lastwake before the
             conversion, without which GetTickCount could come before it:
             GetTickCount: 76815265   <--- less granularity, it hasn't updated
             Unadjusted lastwake in milliseconds: 76815269
             For now put aside the issue of GetTickCount wraparound at
             49d17h2m47s which has to be handled some other way. */
          ULONGLONG mt = (MaximumTimerInterval * 2) + 10000;
          DWORD waketick =
            (DWORD)((lastwake > mt ? lastwake - mt : 0) / 10000);
          DWORD elapsed_minutes = (GetTickCount() - waketick) / 1000 / 60;

          if(elapsed_minutes < span_minutes) {
            static ULONGLONG prev_lastwake = (ULONGLONG)-1;

            suppress_lifetime = !verbose;

            if(full_status_shown || prev_lastwake != lastwake) {
              prev_lastwake = lastwake;

              stringstream ss;
              ss << TIMESTAMPED_PREFIX;
              const string &timestamp = ss.str();

              cout << timestamp
                   << "Recently resumed, battery lifetime is inaccurate."
                   << endl;

              if(suppress_lifetime) {
                cout << timestamp
                     << "Temporarily ignoring lifetime." << endl;
              }
            }
          }
          else {
            ignore_this_waketime = lastwake;
            suppress_lifetime = false;
          }
        }
        else
          suppress_lifetime = false;
      }
      else
        suppress_lifetime = false;
    }

    /* Default monitor mode.
       Compare a subset of SYSTEM_POWER_STATUS to determine when the relevant
       state has changed, in order to show updated power status one-liners.
       Note battery percent remaining is compared instead of lifetime
       since the latter is volatile and could cause a lot of updates. */

    /* Check if the battery saver status has changed.
       The battery saver status is available since Windows 10. It's stored in
       member SystemStatus but older headers use the name Reserved1. */
    if(!suppress_charge_state &&
       os.dwMajorVersion >= 10 &&
       BATTSAVER(status) != BATTSAVER(prev_status)) {
      cout << TIMESTAMPED_PREFIX
           << SystemStatusFlagStr(status.Reserved1) << endl;
    }

    if(!full_status_shown &&
       status.BatteryLifePercent == prev_status.BatteryLifePercent &&
       (suppress_charge_state ||
        (CHARGING(status) == CHARGING(prev_status))) &&
       NO_BATTERY(status) == NO_BATTERY(prev_status) &&
       PLUGGED_IN(status) == PLUGGED_IN(prev_status))
      continue;

    /* The status has changed enough to show the one-liner output. */

    cout << TIMESTAMPED_PREFIX;
    // Show the status in the same formats that the battery systray uses
    if(NO_BATTERY(status)) {
      // eg: No battery is detected
      cout << "No battery is detected";
    }
    else if(suppress_charge_state) {
      // eg: 100% remaining
      cout << BatteryLifePercentStr(status.BatteryLifePercent) << " remaining";
    }
    else if(status.BatteryLifePercent == 100 &&
            (suppress_lifetime ||
             status.BatteryLifeTime == LIFETIME_UNKNOWN) &&
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
    else if(!suppress_lifetime &&
            status.BatteryLifeTime != LIFETIME_UNKNOWN) {
      // eg: 27 min (15%) remaining
      cout << BatteryLifeTimeStr(status.BatteryLifeTime) << " ("
           << BatteryLifePercentStr(status.BatteryLifePercent)
           << ") remaining";
    }
    else {
      // eg: 100% remaining
      cout << BatteryLifePercentStr(status.BatteryLifePercent) << " remaining";
    }
    cout << endl;
  }
}
