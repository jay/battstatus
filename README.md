battstatus
==========

battstatus - Monitor the Windows battery status for changes in state

### Usage

Usage: `battstatus [-a <minutes>] [-n] [-p] [-v[vv]]`

battstatus monitors your laptop battery for changes in state. By default it
monitors
[WM_POWERBROADCAST](https://msdn.microsoft.com/en-us/library/windows/desktop/aa373247.aspx)
messages and relevant changes in
[power status](https://msdn.microsoft.com/en-us/library/windows/desktop/aa373232.aspx).

~~~
  -v    Monitor and show all power status variables on any change.

  -vv   .. and show ??? (unused).

  -vvv  .. and show all window messages received by the monitor window.
        Window messages other than WM_POWERBROADCAST are shown by hex.

  -a    Average Lifetime: Show lifetime as an average of the last <minutes>.

  -n    No Monitoring: Show the current status and then quit.

  -p    Prevent Sleep: Prevent the computer from sleeping while monitoring.
        This option changes the monitor thread's power request state so that
        the system can stay in a working state (aka 'SYSTEM') and enter away
        mode (aka 'AWAYMODE') instead of true sleep. Note it doesn't seem to
        prevent a manual sleep initiated by the user when unplugged and running
        on battery power.
~~~

Options combined into a single argument are the same as separate options, for
example -pvv is the same as -p -v -v.

### Sample output

~~~
[Wed Aug 02 12:15:55 PM]: 5 hr 30 min (99%) remaining
[Wed Aug 02 12:17:38 PM]: WM_POWERBROADCAST: PBT_APMPOWERSTATUSCHANGE
[Wed Aug 02 12:17:38 PM]: 99% available (plugged in, not charging)
[Wed Aug 02 12:17:44 PM]: Fully charged (100%)
[Wed Aug 02 12:41:50 PM]: WM_POWERBROADCAST: PBT_APMPOWERSTATUSCHANGE
[Wed Aug 02 12:41:50 PM]: 99% remaining
[Wed Aug 02 12:45:14 PM]: 8 hr 13 min (98%) remaining
[Wed Aug 02 12:49:39 PM]: 7 hr 37 min (97%) remaining
~~~

### The 'Average Lifetime' option explained

Windows calculates battery lifetime from the immediate values that are reported
by the battery. If there is a higher than normal drain on the battery then the
lifetime will be lower. Over several status reports you may notice lifetime
discrepancies of up to several hours, but typically an hour or less. For
example:

~~~
[Tue Sep 19 05:00:50 PM]: 1 hr 45 min (20%) remaining
[Tue Sep 19 05:05:44 PM]: 41 min (19%) remaining
[Tue Sep 19 05:09:48 PM]: 1 hr 32 min (18%) remaining
[Tue Sep 19 05:15:04 PM]: 1 hr 32 min (17%) remaining
[Tue Sep 19 05:19:49 PM]: 58 min (16%) remaining
[Tue Sep 19 05:24:13 PM]: 1 hr 25 min (15%) remaining
[Tue Sep 19 05:29:08 PM]: 54 min (14%) remaining
[Tue Sep 19 05:33:33 PM]: 1 hr 07 min (13%) remaining
~~~

All of those lifetimes are technically accurate since they're based on what the
battery was reporting at the time. However you may find averaging the lifetime
gives more accurate readings. For example, `-a 30` would show each lifetime as
an average of the last 30 minutes of received lifetimes (adjusted for when they
were received) and you'd see output similar to this:

~~~
[Tue Sep 19 05:00:50 PM]: 1 hr 34 min (20%) remaining
[Tue Sep 19 05:05:44 PM]: 1 hr 24 min (19%) remaining
[Tue Sep 19 05:09:48 PM]: 1 hr 12 min (18%) remaining
[Tue Sep 19 05:15:04 PM]: 1 hr 15 min (17%) remaining
[Tue Sep 19 05:19:49 PM]: 1 hr 11 min (16%) remaining
[Tue Sep 19 05:24:13 PM]: 1 hr 05 min (15%) remaining
[Tue Sep 19 05:29:08 PM]: 1 hr 04 min (14%) remaining
[Tue Sep 19 05:33:33 PM]: 57 min (13%) remaining
~~~

Other
-----

### Resources

[PassMark BatteryMon](https://www.passmark.com/products/batmon.htm)
is a battery monitor that has a GUI and can graph the battery charge level.

[Microsoft Powercfg](https://docs.microsoft.com/en-us/windows-hardware/design/device-experiences/powercfg-command-line-options)
can make a battery status report.

[Microsoft pwrtest](https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/pwrtest)
has a /battery option that allows polling the battery status like battstatus.
It comes with the Windows Driver Kit (WDK) or can be downloaded
[here](https://github.com/jay/battstatus/releases/download/v1.0/pwrtest.zip).

### License

battstatus is free software and it is licensed under the
[GNU General Public License version 3 (GPLv3)](https://github.com/jay/battstatus/blob/master/License_GPLv3.txt),
a license that will keep it free. You may not remove my copyright or the
copyright of any contributors under the terms of the license. The source code
for battstatus cannot be used in proprietary software, but you can for example
execute a free software application from a proprietary software application.
**In any case please review the GPLv3 license, which is designed to protect
freedom, not take it away.**

### Source

The source can be found on
[GitHub](https://github.com/jay/battstatus).
Since you're reading this maybe you're already there?

### Send me any questions you have

Jay Satiro `<raysatiro$at$yahoo{}com>` and put 'battstatus' in the subject.
