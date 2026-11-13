# Changelog

Each version's first list is what the Sticky shows on its update screen
(short lines, plain characters); web/build.sh puts it in the update manifest.

## 0.7.1

- Updating shows a progress bar, then "Update done" before it restarts.
- An update started from Home Assistant shows on the Sticky too.
- Looks for updates at most every 6 hours, never while asleep.

## 0.7.0

- Updates from the Sticky: tap "Update" in the status bar, read what's new, install.
- Asleep on USB, any button wakes it again.
- A clock while asleep, with the date (can be turned off).
- Wi-Fi and settings are kept across updates.
- Quicker taps: Wi-Fi no longer naps between presses.
- Setup is done from the setup page only (no hotspot).
