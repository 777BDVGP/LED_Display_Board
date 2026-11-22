# LED Display Board — User Guide

How to set up and use the clock once it is built and the firmware is installed
(see the [README](README.md) for installing, and
[Build_Files/README.md](Build_Files/README.md) for building).

Everything is done from the clock's own web pages: open `http://<the clock's IP>/`
in a browser on the same WiFi (phone or PC). The pages need no login — anyone
on your WiFi can open them.

---

## 1. First setup

1. **WiFi** — a new clock opens the hotspot **LEDClock-Setup** (blue LED
   blinking fast). Connect a phone, pick your network, type the password,
   choose a **fixed IP** (recommended) and press **SAVE & RESTART**.
2. **Time zone** — SETTINGS → **TIME ZONE**. The clock starts on Greenwich
   (GMT); pick your zone and press **UPDATE**. The time changes at once.
3. **Name** — SETTINGS → **Device Name** (shown at the top of every page and
   to the other clocks).
4. **Brightness** — SETTINGS → BRIGHTNESS (see section 9).
5. **Phone notifications** (optional) — NTFY page (section 7).
6. **More clocks** (optional) — ABOUT → CLOCK NETWORK (section 8).
7. Add your **birthdays, anniversaries and reminders** (sections 4–5).

## 2. The pages

The menu at the top has **HOME**, **QUICK** and **OTHER**. OTHER leads to
everything else:

| Button | What it is for |
|---|---|
| REMINDERS | reminders that repeat or happen once (section 5) |
| REMINDER VALID PERIODS | date ranges that limit when a reminder may fire (section 5.3) |
| BIRTHDAYS / ANNIVERSARYS | yearly dates (section 4) |
| DISK TOOLS | backups to / from CSV files (section 12) |
| SETTINGS | display, brightness, time zone, buzzer, … (section 9) |
| ABOUT | WiFi info, firmware updates, clock network (sections 8, 10) |
| NTFY | phone notifications (section 7) |
| FILES | the files stored on the clock (section 12) |
| EXERCISES | guided exercise programmes (section 6) |
| REBOOT / RESET ASP | restart / forget all WiFi networks (section 11) |
| WIFI NETWORKS | saved WiFi networks (section 11) |

## 3. HOME — today's events

HOME lists everything that fires **today**: birthdays, anniversaries and
reminders, with their time and status.

| Status | Meaning | Tap it to … |
|---|---|---|
| **PENDING** | still to come today (extras shown: HUMAN, ONE TIME, NTFY, RADAR) | **disable** it for today |
| **DISABLED** | will not show today | enable it again |
| **FIRED** | already shown today | — |

- **Change the time for today only:** tap the time and pick a new one. Only
  today's occurrence moves; the next one is at the normal time again.
- **RESET TIME/STATUS** undoes all of today's changed times and disabled
  events.
- Disabling or retiming for today is copied to the other clocks.

### What happens when an event fires

1. The **buzzer** plays the event's pattern (after the *Buzzer Delay*, if set).
2. The **message scrolls** across the display in the event's colour:
   the label and message, plus the age for birthdays / anniversaries.
3. It scrolls **Repeat Times** times. **0 = it keeps scrolling until the
   push-button is pressed.**
4. **Press the push-button** (front) to stop the message and the buzzer.
5. **Detect Human** ticked: the event waits until the radar sees someone,
   then fires — so the message is not shown to an empty room.
6. **Send NTFY** ticked: a push message goes to your phone (section 7).
7. **One Time** reminders are deleted after they fired.

## 4. Birthdays and anniversaries

BIRTHDAYS / ANNIVERSARYS → **+ Create New**, or tap an entry to edit it.

| Field | Meaning |
|---|---|
| Label | the name, e.g. "Anna" |
| Message | shown after the label, e.g. "Verjaar" / "Anniversary" |
| Date | the date it happened — the **year is used for the age**; it fires every year on the day and month |
| Time | when it fires (or one time for all of them — SETTINGS → Birthdays & Anniversaries Override) |
| Show Age | adds "NN years" |
| Text Color / Buzzer Sound / Repeat Times / Detect Human / NTFY / Event Active | as described above |

- **Public holidays:** an anniversary with the message **Public Holiday** fires
  only on the exact date including the year — update it each year for
  holidays that move (Easter, …).
- The lists have a **search box**; **Delete List** lets you tap entries to
  delete them (no confirmation — tap carefully).

## 5. Reminders

### 5.1 Creating a reminder

REMINDERS → **+ Create New Reminder**:

| Field | Meaning |
|---|---|
| Label / Message | the text that scrolls |
| Repeat | **Once on Date**, **Once per Week** (one weekday), **Multiple per Week** (e.g. Mon, Wed, Fri — or every day), **First Week of Month** / **Last Week of Month** (e.g. first Monday — of every month or of one chosen month = yearly) |
| Date | for *Once on Date* |
| Time | when it fires |
| Valid Period | **None**, or a period from REMINDER VALID PERIODS — the reminder then only fires on dates inside it (section 5.3) |
| One Time | delete the reminder after it fired |
| Text Color, Buzzer Sound, Repeat Times, Detect Human, NTFY, Event Active | as in section 3 |

**QUICK** (top menu) makes a one-time reminder in two taps: type a label, set
"in 40 minutes" (or any number) with **Set**, then **SAVE**.

### 5.2 The reminders list

Reminders are grouped — **Daily, Weekly, Monthly, Yearly, Once** and
**Once – Expired** (one-time dates that have passed; good candidates to
delete). Tap a group header to fold it; the clock remembers which groups you
folded. The **search box** finds a reminder by name, day, month, "NTFY", etc.
The badge shows e.g. `16:15 · Mon, Wed · NTFY · School Terms`.

### 5.3 Reminder Valid Periods

A Valid Period is a named list of date ranges — e.g. *School Terms*:
15 Jan – 28 Mar, 8 Apr – 20 Jun, … A reminder linked to it only fires on
dates inside one of the ranges.

- OTHER → REMINDER VALID PERIODS → **+ Create New Valid Period**, give it a
  name and add ranges (**+ Add Range**).
- **Every Year** ticked: the year is ignored and the range repeats every year
  (and may run over New Year, e.g. 1 Dec – 31 Jan). Unticked: exact dates
  including the year. Both end dates count.
- Deleting a period sets the reminders that used it back to **None**.
- Limits: 20 periods, 30 ranges each.

## 6. Exercises

EXERCISES holds exercise programmes that are counted out on the display:
name, number of movements with their instructions and times, sets, an
instruction between sets (e.g. "Now Other Leg"), repetitions. **Select & Run →
START** runs one; the same button (now **STOP**) stops it. Exercises are kept on each clock
and are **not** copied to the other clocks.

## 7. Phone notifications (NTFY)

The clock can send a push message to your phone through the free
[ntfy](https://ntfy.sh) app.

1. Install the ntfy app and **subscribe** to a topic name of your own (make
   it unusual — anyone who knows the name can read it).
2. NTFY page: **Enable NTFY Push Messages**, enter that name as **NTFY Topic
   Option 1** (and a second one, e.g. for a partner, as **Option 2**), choose
   the **priority**, press **UPDATE**.
3. In each event choose which topic gets it (Option 1, Option 2 or
   **BOTH_NTFYS**) and tick **Send NTFY**.

**NTFY Topic Name** is this clock's own switch: with several clocks, set it to
**NO_NTFY** on all clocks except one — otherwise every clock sends the same
message.

## 8. Clock network — several clocks

Up to **4 clocks** share their **birthdays, anniversaries, reminders and valid
periods**:

1. Give every clock a fixed IP.
2. On each clock: ABOUT → CLOCK NETWORK → **Clocks in Use**, type the IPs of
   the other clocks (their names appear), **SAVE NETWORK**.

- Adding, changing or deleting on **any** clock is copied to the others within
  seconds.
- A clock that was off sends / receives everything again when it comes back
  (adding only — something **deleted** while a clock was off stays on that
  clock; delete it again).
- **Not** shared: settings, time zone, WiFi, NTFY settings, exercises — set
  those on each clock.

## 9. Settings

SETTINGS → change values → **UPDATE** at the bottom.

| Section | Setting | What it does |
|---|---|---|
| SYSTEM | OTA Hostname | name for firmware updates over WiFi |
| | Device Name | the clock's name (page header, clock network) |
| | Save Log Active | keep a log file on the clock |
| TIME ZONE | Time Zone | 26 zones, daylight saving automatic; default Greenwich |
| BRIGHTNESS | Display LDR Data | shows the light-sensor reading on the display, to calibrate |
| | Raw LDR Minimum / Maximum | sensor readings for dark / bright room |
| | Min / Max Display Brightness | display brightness in the dark / in bright light |
| HUMAN DETECT COLOUR | On Human Detect DIFFERENT Colour + RGB | time in another colour while someone is near |
| DISPLAY ON / OFF | Time on only when Human Detect | display off when nobody is near (radar) |
| | Day / Night Detect-Off | from what time "day" and "night" start, and how long the display stays on after the last movement (day in minutes, night in seconds) |
| BUZZER DELAY | Delay Before Message Display (MS) + colour | pause (and colour) between the buzzer and the message |
| TIME DISPLAY | Weekday Display (Sec) | the display alternates between *weekday + time* and the date: seconds for the weekday + time |
| | YearDate Display (Sec) + style | seconds for the date (0 = never show it) — style *Full Date* or *Date/Time* |
| DAY / NIGHT COLOURS | start times + RGB colours | colour of the time by day and by night |
| BUZZER PATTERNS | on / off times (ms) + Test | tune the four beep patterns; tick Test to hear one |
| MIDNIGHT REBOOT | Time + Enable | a daily restart (default 00:05) to keep the clock fresh |
| BIRTHDAYS & ANNIVERSARIES | Display Time + Override | show all birthdays and anniversaries at one time instead of each one's own |
| WEB DISPLAY THEME | Dark / Light | look of the web pages |

Colours are entered as `R,G,B` (0–255 each), e.g. `255,0,0` for red.

## 10. ABOUT page

- **FIRMWARE** — **CHECK FOR UPDATE** / **INSTALL FROM GITHUB** (the clock
  restarts, downloads the new version and restarts again; everything you
  entered is kept), or **UPLOAD & INSTALL** a `.bin` file.
- **WIFI** — network, IP, gateway, number of saved networks.
- **CLOCK NETWORK** — section 8.
- REBOOT / RESET ASP.

## 11. WiFi networks, reboot, reset

- OTHER → **WIFI NETWORKS** lists the saved networks (up to 8, passwords never
  shown) with **DELETE** and **ADD NETWORK** (fixed IP or automatic). Changes
  are used at the next start. The clock tries the networks in range,
  strongest first.
- If no saved network can be reached for about 7 minutes, the
  **LEDClock-Setup** hotspot opens for 5 minutes, then the clock restarts and
  tries again — so a router that is down for a while does not need any action.
- **REBOOT** restarts the clock. **RESET ASP** forgets **all** WiFi networks
  and restarts into the setup hotspot — you then need a phone to set it up
  again.

## 12. Backups — Disk Tools and Files

- **DISK TOOLS → EXPORT** writes all birthdays, anniversaries, reminders,
  exercises or valid periods to a CSV file on the clock. Download it on the
  **FILES** page and keep it on your PC.
- **IMPORT** replaces **all** entries of that kind on this clock with the CSV
  file (upload it on FILES first), and then copies the same set to the other
  clocks (exercises excepted). Entries that exist only on another clock are
  removed there.
- CSV files can be edited in Excel (one line per entry).
- **FILES** lists, views, downloads, uploads and deletes the clock's files.
  The WiFi file is never shown.

## 13. What the clock does by itself

- Takes the time from the internet and keeps it right (daylight saving too).
- Rebuilds the list of today's events every midnight.
- Dims and brightens with the room light; switches the display off when
  nobody is near (if set).
- Reconnects to WiFi after a dropout, and tries other saved networks.
- Restarts once a day (MIDNIGHT REBOOT, if enabled).

## 14. Limits

| What | Limit |
|---|---|
| Clocks in one network | 4 |
| Entries shown per list (birthdays, reminders, …) | 64 |
| Events firing on one day | 40 |
| Valid periods / ranges per period | 20 / 30 |
| Saved WiFi networks | 8 |
| Display | 48 × 8 LEDs (six 8 × 8 panels) |
| Web pages / updates over WiFi | no password — keep the clock on a WiFi you trust |
