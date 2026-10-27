# WSL COM Port Swapper

A lightweight Win32 GUI for forwarding USB serial devices (COM ports) between Windows and WSL2 using [usbipd-win](https://github.com/dorssel/usbipd-win).

Designed for embedded development workflows where you need to flash or monitor a microcontroller from both Windows tools and Linux-side toolchains (ESP-IDF, OpenOCD, minicom, etc.) without touching the command line every time.

---

## Features

- **Windows COM port list** — populated directly from the Windows registry; always accurate, no usbipd dependency for the listing step
- **WSL tty port list** — live scan of `/dev/ttyUSB*` and `/dev/ttyACM*` inside WSL
- **One-click attach / detach** — forwards the selected COM port into WSL or returns it to Windows
- **Bluetooth tab** — hands the PC's USB Bluetooth adapter to WSL (and back) in one click
- **Clear stuck USB** — frees "phantom" devices WSL still holds after Windows dropped the link
- **Automatic BUSID resolution** — matches by device description string first, then falls back to VID:PID registry lookup
- **Privilege indicator** — green/red status bar shows whether the app is running with sufficient rights
- **usbipd status indicator** — shows whether usbipd-win is installed and found on PATH
- **Delayed refresh** — waits 2 seconds after attach/detach before refreshing lists, giving Windows and WSL time to enumerate the device
- **UAC manifest** — auto-prompts for Administrator on launch; no need to right-click → Run as Administrator

---

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| Windows 10 / 11 (x64) | Build 19041 or later for WSL2 |
| [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) | Any distribution |
| [usbipd-win](https://github.com/dorssel/usbipd-win/releases) | Install the `.msi`; adds the `usbipd` CLI and Windows service |

Install usbipd-win first. The app detects it automatically and shows a warning on startup if it is not found.

---

## Build

Only `main.c`, `app.manifest`, and `manifest.rc` are needed. No dependencies beyond the Windows SDK.

### Option A — MSVC (Visual Studio)

Open a **Developer Command Prompt for VS** and run:

```bat
build.bat
```

### Option B — MinGW / GCC

Make sure `gcc` and `windres` are on your PATH, then run:

```bat
build.bat
```

The script detects whichever compiler is available and picks the right flags automatically. The manifest is embedded in both cases so Windows shows the UAC elevation prompt on launch.

---

## Usage

1. **Launch** `WslComPortSwapper.exe` — Windows will prompt for Administrator rights (required for usbipd bind/attach/detach).
2. **Plug in** your USB device. Click **Refresh** if it does not appear immediately.
3. **Attach to WSL** — set the COM port number in the left spinner (e.g. `4` for COM4) and click `→ Attach to WSL`. The device appears as `/dev/ttyUSB0` (or similar) in WSL after ~2 seconds.
4. **Return to Windows** — set the tty number in the right spinner (e.g. `0` for ttyUSB0) and click `← Return to Windows`. The COM port reappears in Windows after ~2 seconds.
5. **Refresh** updates both lists at any time.

### Bluetooth tab

1. Open the **Bluetooth** tab. The left list shows USB Bluetooth adapters seen by usbipd (e.g. `1-14  8087:0026  Not shared`); the first one is pre-selected.
2. **Attach to WSL** — after a confirmation, the adapter is force-bound and attached, and `btusb` is loaded in WSL. **Windows has no Bluetooth while the adapter is in WSL** (paired mice, keyboards, headsets drop). `hci0` appears in the right list after ~2 seconds.
3. **Return to Windows** — unbinds the adapter; Windows reloads its Bluetooth driver.

### Clear stuck USB

Sometimes Windows drops the usbip link (unplug, usbipd restart) and WSL never notices: `/dev/ttyUSB0` is still listed, but `usbipd list` shows nothing Attached, so **Return to Windows** cannot detach it. **Clear stuck USB** reads `/sys/devices/platform/vhci_hcd.0/status` in WSL, lists the in-use ports, and after a confirmation detaches each one from the WSL side as root:

```sh
echo 0 | sudo tee /sys/devices/platform/vhci_hcd.0/detach   # 0 = port from the status table
```

Every in-use port is detached, so any device that still works in WSL must be re-attached afterwards.

### Status strip

| Indicator | Green | Red |
|-----------|-------|-----|
| Administrator | Running elevated — attach/detach enabled | Not elevated — operations will fail |
| usbipd | Found on PATH | Not installed or not found |

---

## How it works

### COM port listing
Read directly from `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`. No usbipd involvement — the list is always accurate even if the usbipd service is not running.

### Windows → WSL (attach)
1. `usbipd list` is run to get the current device table and BUSIDs.
2. BUSID for the selected COM port is resolved by two strategies in order:
   - **Description match** — searches the usbipd output for `COMn` in the device friendly name.
   - **VID:PID registry fallback** — walks `HKLM\SYSTEM\CurrentControlSet\Enum\USB` to find the VID:PID for the COM port, then matches that against the usbipd device table.
3. `usbipd bind --busid <id>` makes the device shareable.
4. `usbipd attach --wsl --busid <id>` forwards it into WSL.

### Bluetooth
Adapters are the `usbipd list` entries whose description contains `Bluetooth`. Equivalent commands:

```bat
:: to WSL
usbipd bind --busid 1-14 --force
usbipd attach --wsl --busid 1-14
wsl -u root modprobe btusb

:: back to Windows (also detaches)
usbipd unbind --busid 1-14
```

`--force` is required because Bluetooth radios are in use by the Windows Bluetooth stack; it takes the device away from Windows until `unbind`. The right list shows `/sys/class/bluetooth/hci*` inside WSL. Using Bluetooth from Linux still needs BlueZ (`bluetoothctl`) installed and running in your distribution.

### WSL → Windows (detach)
1. `usbipd list` is refreshed so the Attached state is current.
2. VID:PID is read from the WSL kernel sysfs tree (`/sys/class/tty/ttyUSBn/device/../idVendor` and up to three parent levels) and matched against Attached entries in the usbipd table.
3. If sysfs lookup fails and exactly one device is Attached, that device is used directly (common case when only one device is forwarded at a time).
4. `usbipd detach --busid <id>` returns the device to Windows.

---

## Known limitations

- **Multiple attached devices + sysfs failure** — if more than one device is Attached to WSL and the sysfs VID:PID lookup returns empty (can happen on some WSL2 kernel configurations), the tool cannot determine which device corresponds to which tty number. In this case detach manually with `usbipd detach --busid <id>`.
- **Non-USB serial ports** — Bluetooth COM ports and virtual serial ports (e.g. from VMs) appear in the Windows list but have no USB hardware to bind, so attach will fail with a BUSID lookup error.
- **Bluetooth detection by name** — only adapters whose usbipd description contains `Bluetooth` are listed.
- **Single WSL distribution** — `usbipd attach --wsl` attaches to the default WSL distribution. If you have multiple distributions, set the default with `wsl --set-default <distro>`.

---

## Author

by Matteo Vittorio Ricciutelli — 2026

## Credits

- Icon made by [Freepik](https://www.freepik.com) from [www.flaticon.com](https://www.flaticon.com)

---

## License

MIT License. See [LICENSE](LICENSE) if included, or use freely.
