/**
 * @file main.c
 * @brief WSL COM Port Swapper — Win32 GUI for usbipd-win USB forwarding.
 *
 * Layout: two tabs, each with two listboxes side by side.
 *
 * "Serial (COM)" tab:
 *   Left  — Windows COM ports read from the system registry
 *            (HKLM\HARDWARE\DEVICEMAP\SERIALCOMM).  Always populated,
 *            no usbipd dependency for the listing step.
 *   Right — WSL /dev/ttyUSB* and /dev/ttyACM* ports.
 *
 * A COM-port number spinner and "-> Attach to WSL" button below the left
 * panel bind and forward the chosen COM port into WSL via usbipd.
 * A ttyUSB/ACM number spinner and "<- Return to Windows" button below
 * the right panel detach the device back to Windows.
 *
 * "Bluetooth" tab:
 *   Left  — USB Bluetooth adapters from `usbipd list`.
 *   Right — Bluetooth controllers the WSL kernel sees (/sys/class/bluetooth).
 *
 * "-> Attach to WSL" force-binds the selected adapter (Windows loses it),
 * attaches it and loads the btusb module; "<- Return to Windows" unbinds it.
 *
 * usbipd is still needed for bind/attach/detach, but only a background
 * `usbipd list` run is needed for BUSID lookup; the visible COM-port list
 * comes directly from the registry so it is always accurate.
 *
 * Build
 * -----
 * MSVC (from a Developer Command Prompt):
 *   cl /nologo /O2 /W3 main.c /link /SUBSYSTEM:WINDOWS \
 *      user32.lib advapi32.lib comctl32.lib /out:WslComPortSwapper.exe
 *
 * MinGW:
 *   gcc -O2 -Wall main.c manifest_res.o -o WslComPortSwapper.exe \
 *       -luser32 -ladvapi32 -lcomctl32 -mwindows
 *
 * Privilege note
 * --------------
 * usbipd bind and attach require Administrator rights.
 * The status bar shows a warning if not elevated.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _MSC_VER
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "advapi32.lib")
#  pragma comment(lib, "comctl32.lib")
#endif

/* ── control IDs ─────────────────────────────────────────────────────────── */

#define IDC_WIN_LIST  101  /**< LISTBOX — Windows COM ports (from registry)   */
#define IDC_WSL_LIST  102  /**< LISTBOX — WSL /dev/tty* ports                 */
#define IDC_REFRESH   103  /**< Push-button — refresh both lists              */
#define IDC_TO_WSL    104  /**< Push-button — bind + attach chosen COM port   */
#define IDC_TO_WIN    105  /**< Push-button — detach chosen WSL tty          */
#define IDC_COM_EDIT  106  /**< EDIT buddy for the COM-number UPDOWN          */
#define IDC_COM_SPIN  107  /**< UPDOWN — COM port number (default 4)          */
#define IDC_TTY_EDIT  108  /**< EDIT buddy for the tty-number UPDOWN          */
#define IDC_TTY_SPIN  109  /**< UPDOWN — ttyUSB/ACM suffix number (default 0) */
#define IDC_STATUS    110  /**< STATIC — status and error messages            */
#define IDC_PRIV      111  /**< STATIC — privilege / elevation indicator      */
#define IDC_USBIPD_ST 112  /**< STATIC — usbipd install / found indicator     */
#define IDC_CREDIT    113  /**< STATIC — author credit line                   */
#define IDC_TABS      114  /**< TAB — Serial (COM) / Bluetooth pages          */
#define IDC_BT_LIST   115  /**< LISTBOX — Windows USB Bluetooth adapters      */
#define IDC_BT_WSL    116  /**< LISTBOX — WSL Bluetooth controllers (hci*)    */
#define IDC_BT_TO_WSL 117  /**< Push-button — force-bind + attach adapter     */
#define IDC_BT_TO_WIN 118  /**< Push-button — unbind adapter back to Windows  */
#define IDC_UNSTICK   119  /**< Push-button — free stuck WSL-side usbip ports */

#define TIMER_DETACH_REFRESH 1  /**< WM_TIMER id: delayed refresh after detach */
#define APP_VERSION "1.0"       /**< Shown in the window title bar             */
#define IDI_APPICON  101        /**< Icon resource ID — usb-port.ico           */
#define IDM_ABOUT    201        /**< Menu command — Help → About               */

/* ── constants ───────────────────────────────────────────────────────────── */

#define MAX_DEV    64   /**< Max USB entries from usbipd list               */
#define BUSID_SZ   16   /**< Max BUSID string ("3-2")                       */
#define VIDPID_SZ  16   /**< Max VID:PID string ("10c4:ea60")               */
#define CMD_SZ     640  /**< Process command-line buffer                    */
#define RAW_SZ     8192 /**< stdout capture buffer                          */

/** WSL usbip virtual host controller (sysfs status / detach files).
 *  ponytail: first controller only; vhci_hcd.1+ appear only past 8 devices. */
#define VHCI_SYSFS "/sys/devices/platform/vhci_hcd.0"

/** Device present in Windows; not bound to usbipd. */
#define ST_WINDOWS 0
/** Device bound (shareable) but not yet attached to WSL. */
#define ST_SHARED  1
/** Device currently attached to WSL (/dev/ttyUSBx etc.). */
#define ST_WSL     2

/** Fallback usbipd.exe locations when PATH lookup fails. */
static const char * const USBIPD_LOCS[] = {
    "C:\\Program Files\\usbipd-win\\usbipd.exe",
    "C:\\Program Files (x86)\\usbipd-win\\usbipd.exe",
    NULL
};

/* ── data ────────────────────────────────────────────────────────────────── */

/**
 * @brief One USB device entry parsed from `usbipd list`.
 *
 * Used only for BUSID lookup during bind/attach/detach.
 * The visible Windows COM port list comes from the registry instead.
 */
typedef struct {
    char busid[BUSID_SZ];   /**< usbipd BUSID, e.g. "3-2"             */
    char vidpid[VIDPID_SZ]; /**< Vendor:product IDs, e.g. "10c4:ea60" */
    char display[512];       /**< Full usbipd output line (trimmed)    */
    int  state;              /**< ST_WINDOWS, ST_SHARED, or ST_WSL     */
} Dev;

/** @brief Resolved path to usbipd.exe, set by find_usbipd(). */
static char g_usbipd[MAX_PATH] = "";

/** @brief USB devices from the last `usbipd list` run (for BUSID lookup). */
static Dev  g_devs[MAX_DEV];
/** @brief Number of valid entries in g_devs. */
static int  g_ndev = 0;

/** @brief WSL tty paths populated by refresh(). */
static char g_tty[MAX_DEV][64];
/** @brief Number of valid entries in g_tty. */
static int  g_ntty = 0;

/** @brief Number of COM ports shown in the Windows listbox. */
static int  g_ncom = 0;

/* Control handles — set in WM_CREATE */
static HWND g_hwnd       = NULL; /**< Main window.                        */
static HWND g_win_list   = NULL; /**< Windows COM listbox.                */
static HWND g_wsl_list   = NULL; /**< WSL tty listbox.                    */
static HWND g_status     = NULL; /**< Status bar label.                   */
static HWND g_com_spin   = NULL; /**< COM-number UPDOWN.                  */
static HWND g_tty_spin   = NULL; /**< tty-suffix UPDOWN.                  */
static HWND g_priv_label   = NULL; /**< Privilege / elevation indicator.        */
static HWND g_usbipd_label = NULL; /**< usbipd install / found indicator.       */
static HWND g_tabs         = NULL; /**< Serial / Bluetooth tab control.         */
static HWND g_bt_list      = NULL; /**< Windows Bluetooth adapter listbox.      */
static HWND g_bt_wsl       = NULL; /**< WSL Bluetooth controller listbox.       */
/** @brief Controls shown on each tab page (index = tab); unused slots NULL. */
static HWND g_pages[2][12];
/** @brief Number of Bluetooth adapters shown in g_bt_list. */
static int  g_nbt = 0;
/** @brief Cached elevation state (set once at startup, shown in g_priv_label). */
static int  g_elevated_cache = 0;

/* ── process helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Spawn @p cmdline and drain its stdout+stderr into @p out.
 *
 * An anonymous pipe is attached to the child's stdout and stderr.
 * The call blocks for up to @p ms milliseconds, then reads available output.
 *
 * @param cmdline  Full Windows command-line string (a mutable copy is made).
 * @param out      Buffer that receives NUL-terminated captured output.
 * @param size     Byte size of @p out.
 * @param ms       Maximum wait time in milliseconds.
 * @return         Non-zero if the child process launched successfully.
 */
static int proc_capture(const char *cmdline, char *out, int size, DWORD ms) {
    HANDLE r, w;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&r, &w, &sa, 0)) return 0;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput  = w;
    si.hStdError   = w;
    si.wShowWindow = SW_HIDE;

    char buf[CMD_SZ];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(r); CloseHandle(w); return 0;
    }
    CloseHandle(w);
    WaitForSingleObject(pi.hProcess, ms);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DWORD n; int total = 0;
    while (total < size - 1 &&
           ReadFile(r, out + total, size - total - 1, &n, NULL) && n)
        total += (int)n;
    out[total] = '\0';
    CloseHandle(r);
    return 1;
}

/**
 * @brief Run a usbipd command with @p args and capture its output.
 *
 * Calls usbipd directly via CreateProcessA — no cmd.exe wrapper — to
 * avoid cmd.exe quote stripping issues with paths containing spaces.
 * g_usbipd must have been resolved by find_usbipd() before calling.
 *
 * @param args  Argument string appended after the usbipd path (e.g. "list").
 * @param out   Output buffer.
 * @param size  Size of @p out.
 * @return      Non-zero if usbipd was launched.
 */
static int usbipd_capture(const char *args, char *out, int size) {
    if (!g_usbipd[0]) { *out = '\0'; return 0; }
    char cmd[CMD_SZ];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", g_usbipd, args);
    return proc_capture(cmd, out, size, 8000);
}

/**
 * @brief Run a usbipd command silently, waiting up to 15 seconds.
 *
 * Used for bind, attach, and detach operations where no output is needed.
 *
 * @param args  Arguments passed to usbipd (e.g. "bind --busid 3-2").
 */
static void usbipd_run(const char *args) {
    if (!g_usbipd[0]) return;
    char cmd[CMD_SZ];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", g_usbipd, args);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/**
 * @brief Run a bash command inside WSL and capture its output.
 *
 * Launches `wsl.exe sh -c "<bash_cmd>"` directly via CreateProcessA so
 * that shell metacharacters are interpreted by the WSL shell.
 *
 * @param bash_cmd  sh command; must not contain embedded double-quotes.
 * @param out       Output buffer.
 * @param size      Size of @p out.
 * @return          Non-zero if wsl.exe launched.
 */
static int wsl_capture(const char *bash_cmd, char *out, int size) {
    char cmd[CMD_SZ];
    snprintf(cmd, sizeof(cmd), "wsl.exe sh -c \"%s\"", bash_cmd);
    return proc_capture(cmd, out, size, 5000);
}

/* ── elevation ───────────────────────────────────────────────────────────── */

/**
 * @brief Return non-zero if the process is running with Administrator privileges.
 *
 * Queries the process token for TOKEN_ELEVATION. usbipd bind and attach
 * require elevation; a warning is shown in the status bar if absent.
 */
static int is_elevated(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    TOKEN_ELEVATION e = {0};
    DWORD sz = sizeof(e);
    BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sz, &sz);
    CloseHandle(tok);
    return ok ? (int)e.TokenIsElevated : 0;
}

/* ── usbipd path resolution ──────────────────────────────────────────────── */

/**
 * @brief Resolve the usbipd.exe path into g_usbipd.
 *
 * Tries (in order):
 *  1. `where.exe usbipd` via cmd.exe — finds usbipd on the session PATH.
 *     GUI apps launched from Explorer inherit the full user PATH via
 *     cmd.exe, even if the System PATH omits it.
 *  2. Each path in USBIPD_LOCS (hard-coded install locations).
 *  3. Leaves g_usbipd empty if not found; usbipd operations will show
 *     a "not found" error in the status bar at click time.
 */
static void find_usbipd(void) {
    /* Strategy 1: query via cmd.exe so user PATH additions are visible */
    char out[MAX_PATH] = {0};
    char cmd[] = "cmd.exe /C where usbipd";
    if (proc_capture(cmd, out, sizeof(out), 4000) && out[0]) {
        char *nl = strpbrk(out, "\r\n");
        if (nl) *nl = '\0';
        if (out[0]) { snprintf(g_usbipd, MAX_PATH, "%s", out); return; }
    }
    /* Strategy 2: hard-coded install locations */
    for (int i = 0; USBIPD_LOCS[i]; i++) {
        if (GetFileAttributesA(USBIPD_LOCS[i]) != INVALID_FILE_ATTRIBUTES) {
            snprintf(g_usbipd, MAX_PATH, "%s", USBIPD_LOCS[i]);
            return;
        }
    }
    /* Not found: g_usbipd stays empty */
}

/* ── string helpers ──────────────────────────────────────────────────────── */

/**
 * @brief NUL-terminate @p s at @p len, then strip trailing CR, LF, and spaces.
 *
 * @param s    Mutable buffer of at least @p len + 1 bytes (need not be
 *             NUL-terminated yet, e.g. right after a memcpy).
 * @param len  Current string length; updated to the new length on return.
 */
static void rtrim(char *s, int *len) {
    s[*len] = '\0';
    while (*len > 0 && (s[*len-1] == '\r' || s[*len-1] == '\n' ||
                         s[*len-1] == ' '))
        s[--(*len)] = '\0';
}

/**
 * @brief Return non-zero if @p s starts with a usbipd BUSID.
 *
 * BUSID format: `<digits>-<digits>` (e.g. "3-2", "1-14").
 * Lines beginning with letters (section headers) are rejected.
 */
static int is_busid_line(const char *s) {
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    if (*s++ != '-') return 0;
    return isdigit((unsigned char)*s);
}

/**
 * @brief Extract the VID:PID token (second field) from a usbipd list line.
 *
 * usbipd format: `BUSID  VID:PID  description  STATE`
 *
 * @param line  One trimmed usbipd output line.
 * @param out   Buffer of at least VIDPID_SZ bytes.
 * @return      Non-zero if a colon was found in the extracted token.
 */
static int parse_vidpid(const char *line, char *out) {
    const char *p = line;
    while (*p && !isspace((unsigned char)*p)) p++;  /* skip BUSID  */
    while (*p &&  isspace((unsigned char)*p)) p++;  /* skip spaces */
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < VIDPID_SZ - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return strchr(out, ':') != NULL;
}

/**
 * @brief Return non-zero if @p s contains exactly the token "COM<num>".
 *
 * Checks that the character after the digit string is not a digit, so
 * COM4 does not match COM40.
 *
 * @param s    String to search (usbipd device description line).
 * @param num  COM port number to match.
 */
static int has_com(const char *s, int num) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "COM%d", num);
    int plen = (int)strlen(prefix);
    const char *p = s;
    while ((p = strstr(p, prefix)) != NULL) {
        if (!isdigit((unsigned char)p[plen])) return 1;
        p++;
    }
    return 0;
}

/* ── device finders ──────────────────────────────────────────────────────── */

/**
 * @brief Get the VID:PID string for a COM port from the Windows registry.
 *
 * Walks HKLM\SYSTEM\CurrentControlSet\Enum\USB looking for a device whose
 * Device Parameters\PortName value equals "COMn".  The VID and PID are
 * extracted from the key name (format "VID_XXXX&PID_YYYY[&MI_ZZ]") and
 * returned as the lowercase string "xxxx:yyyy".
 *
 * This is the reliable fallback when usbipd's device description does not
 * contain "(COMn)" — some drivers omit the port number from the friendly name.
 *
 * @param n    COM port number (e.g. 4 for COM4).
 * @param out  Buffer of at least VIDPID_SZ bytes.
 * @return     Non-zero if a matching device was found.
 */
static int vidpid_for_com(int n, char *out) {
    char want[16];
    snprintf(want, sizeof(want), "COM%d", n);

    /* FTDI's VCP driver enumerates the serial port under a child
     * "FTDIBUS" bus node (key names use '+' separators, e.g.
     * "VID_0403+PID_6001+<serial>") instead of directly under Enum\USB,
     * so PortName never shows up there for FTDI-based adapters. */
    static const char *roots[] = {
        "SYSTEM\\CurrentControlSet\\Enum\\USB",
        "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS",
    };

    int found = 0;
    for (size_t r = 0; !found && r < sizeof(roots) / sizeof(roots[0]); r++) {
        HKEY hUsb;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, roots[r],
                          0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hUsb) != ERROR_SUCCESS)
            continue;

        char dev_key[128];
        DWORD dev_idx = 0, dev_len;

        while (!found) {
            dev_len = sizeof(dev_key);
            if (RegEnumKeyExA(hUsb, dev_idx++, dev_key, &dev_len,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            /* Key name format: "VID_10C4&PID_EA60" or "VID_10C4&PID_EA60&MI_00" */
            const char *vp = strstr(dev_key, "VID_");
            const char *pp = strstr(dev_key, "PID_");
            if (!vp || !pp) continue;

            char vid[8] = {0}, pid_s[8] = {0};
            int  i;
            for (i = 0; i < 4 && isxdigit((unsigned char)vp[4+i]); i++)
                vid[i]   = (char)tolower((unsigned char)vp[4+i]);
            for (i = 0; i < 4 && isxdigit((unsigned char)pp[4+i]); i++)
                pid_s[i] = (char)tolower((unsigned char)pp[4+i]);
            if (!vid[0] || !pid_s[0]) continue;

            /* Enumerate instance sub-keys (serial number or "0000") */
            HKEY hDev;
            if (RegOpenKeyExA(hUsb, dev_key, 0,
                              KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hDev) != ERROR_SUCCESS)
                continue;

            char inst[128];
            DWORD inst_idx = 0, inst_len;
            while (!found) {
                inst_len = sizeof(inst);
                if (RegEnumKeyExA(hDev, inst_idx++, inst, &inst_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;

                /* Check Device Parameters\PortName under this instance */
                char param_path[256];
                snprintf(param_path, sizeof(param_path), "%s\\Device Parameters", inst);
                HKEY hParam;
                if (RegOpenKeyExA(hDev, param_path, 0, KEY_READ, &hParam) != ERROR_SUCCESS)
                    continue;

                char port_name[16];
                DWORD plen = sizeof(port_name), type;
                if (RegQueryValueExA(hParam, "PortName", NULL, &type,
                                      (BYTE *)port_name, &plen) == ERROR_SUCCESS
                    && type == REG_SZ
                    && strcmp(port_name, want) == 0) {
                    snprintf(out, VIDPID_SZ, "%s:%s", vid, pid_s);
                    found = 1;
                }
                RegCloseKey(hParam);
            }
            RegCloseKey(hDev);
        }
        RegCloseKey(hUsb);
    }
    return found;
}

/**
 * @brief Find the usbipd BUSID for a Windows COM port number.
 *
 * Two strategies, tried in order:
 *  1. String match — searches each g_devs entry for "COM<n>" in the
 *     usbipd device description (works when the driver includes the
 *     port number in the friendly name, e.g. "CP210x (COM4)").
 *  2. VID:PID match — calls vidpid_for_com() to get the device's VID:PID
 *     from the Windows registry, then matches that against g_devs.
 *     Works even when usbipd omits the COM port from the description.
 *
 * @param n    COM port number (e.g. 4 for COM4).
 * @param out  Buffer of at least BUSID_SZ bytes.
 * @return     Non-zero if found by either strategy.
 */
static int busid_for_com(int n, char *out) {
    /* Strategy 1: "(COMn)" substring in usbipd description */
    for (int i = 0; i < g_ndev; i++) {
        if (g_devs[i].state != ST_WSL && has_com(g_devs[i].display, n)) {
            strncpy(out, g_devs[i].busid, BUSID_SZ - 1);
            out[BUSID_SZ - 1] = '\0';
            return 1;
        }
    }
    /* Strategy 2: VID:PID from registry → match against g_devs */
    char vidpid[VIDPID_SZ] = {0};
    if (vidpid_for_com(n, vidpid)) {
        for (int i = 0; i < g_ndev; i++) {
            if (g_devs[i].state != ST_WSL &&
                strcmp(g_devs[i].vidpid, vidpid) == 0) {
                strncpy(out, g_devs[i].busid, BUSID_SZ - 1);
                out[BUSID_SZ - 1] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/**
 * @brief Read the VID:PID for a WSL ttyUSB/ACM device from sysfs.
 *
 * Tries ttyUSBn first, then ttyACMn.  For each, walks up three levels of
 * the sysfs device tree because the depth from the tty class entry to the
 * USB device node (which holds idVendor/idProduct) varies by driver:
 *   depth 1 (device/..)      — some composite/ACM drivers
 *   depth 2 (device/../..)   — most USB serial drivers (cp210x, pl2303…)
 *   depth 3 (device/../../..)— usbipd vhci_hcd attachment on some kernels
 *
 * @param n     Numeric suffix (0 → ttyUSB0 / ttyACM0).
 * @param out   Buffer of at least VIDPID_SZ bytes; set to "" on failure.
 * @param out_sz Byte size of @p out.
 * @return      Non-zero if a non-empty VID:PID was obtained.
 */
static int tty_vidpid(int n, char *out, int out_sz) {
    static const char * const types[] = { "ttyUSB", "ttyACM", NULL };
    for (int t = 0; types[t]; t++) {
        char bash[512];
        snprintf(bash, sizeof(bash),
            "v=$(cat /sys/class/tty/%s%d/device/../idVendor 2>/dev/null"
            " || cat /sys/class/tty/%s%d/device/../../idVendor 2>/dev/null"
            " || cat /sys/class/tty/%s%d/device/../../../idVendor 2>/dev/null);"
            " p=$(cat /sys/class/tty/%s%d/device/../idProduct 2>/dev/null"
            " || cat /sys/class/tty/%s%d/device/../../idProduct 2>/dev/null"
            " || cat /sys/class/tty/%s%d/device/../../../idProduct 2>/dev/null);"
            " echo $v:$p",
            types[t], n, types[t], n, types[t], n,
            types[t], n, types[t], n, types[t], n);

        char raw[128] = {0};
        if (!wsl_capture(bash, raw, sizeof(raw))) continue;

        int len = (int)strlen(raw);
        rtrim(raw, &len);
        /* Require at least "a:b" and a non-empty vendor (not just ":pid") */
        if (len >= 3 && strchr(raw, ':') && raw[0] != ':') {
            strncpy(out, raw, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 1;
        }
    }
    out[0] = '\0';
    return 0;
}

/**
 * @brief Find the usbipd BUSID for a WSL ttyUSB/ACM device.
 *
 * Two strategies, tried in order:
 *  1. sysfs VID:PID — calls tty_vidpid() to read idVendor/idProduct from
 *     the WSL kernel sysfs tree, then matches against ST_WSL entries in
 *     g_devs[].  Works when the sysfs device symlink is resolvable.
 *  2. Single-device fallback — if exactly one device is currently Attached
 *     (ST_WSL) in g_devs[] and sysfs lookup failed or found no match, use
 *     that entry directly.  Handles WSL environments where sysfs paths for
 *     usbipd-attached devices are not standard.
 *
 * g_devs[] must be current (call usbipd_list_devices() first).
 *
 * @param n    Numeric suffix (0 → ttyUSB0 / ttyACM0, used for sysfs only).
 * @param out  Buffer of at least BUSID_SZ bytes.
 * @return     Non-zero if a BUSID was found by either strategy.
 */
static int busid_for_tty(int n, char *out) {
    /* Strategy 1: sysfs VID:PID matching */
    char vidpid[VIDPID_SZ] = {0};
    if (tty_vidpid(n, vidpid, sizeof(vidpid))) {
        for (int i = 0; i < g_ndev; i++) {
            if (g_devs[i].state == ST_WSL &&
                strcmp(g_devs[i].vidpid, vidpid) == 0) {
                strncpy(out, g_devs[i].busid, BUSID_SZ - 1);
                out[BUSID_SZ - 1] = '\0';
                return 1;
            }
        }
    }
    /* Strategy 2: single attached device — no ambiguity, detach it */
    int attached = 0, last = -1;
    for (int i = 0; i < g_ndev; i++) {
        if (g_devs[i].state == ST_WSL) { attached++; last = i; }
    }
    if (attached == 1) {
        strncpy(out, g_devs[last].busid, BUSID_SZ - 1);
        out[BUSID_SZ - 1] = '\0';
        return 1;
    }
    return 0;
}

/* ── refresh ─────────────────────────────────────────────────────────────── */

/**
 * @brief Populate the Windows COM listbox from the system registry.
 *
 * Reads HKLM\HARDWARE\DEVICEMAP\SERIALCOMM. Each registry value has:
 *   name  = device path, e.g. "\\Device\\USBSER000"
 *   data  = COM port name, e.g. "COM4"
 *
 * Display format: "COM4    \\Device\\USBSER000"
 *
 * This is independent of usbipd so it always shows currently-attached
 * COM ports even if usbipd is not installed or not running.
 *
 * Updates g_ncom with the count of entries added.
 */
static void list_registry_com_ports(void) {
    SendMessage(g_win_list, LB_RESETCONTENT, 0, 0);
    g_ncom = 0;

    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;

    char vname[128], vdata[64];
    DWORD idx = 0, vnlen, vdlen, vtype;
    while (1) {
        vnlen = sizeof(vname);
        vdlen = sizeof(vdata);
        LONG r = RegEnumValueA(hk, idx++, vname, &vnlen, NULL,
                                &vtype, (BYTE *)vdata, &vdlen);
        if (r != ERROR_SUCCESS) break;
        if (vtype != REG_SZ) continue;
        /*
         * vdata = "COM4" (the port name the user cares about)
         * vname = "\Device\USBSER000" (the kernel device path)
         */
        char disp[256];
        snprintf(disp, sizeof(disp), "%-8s  %s", vdata, vname);
        SendMessageA(g_win_list, LB_ADDSTRING, 0, (LPARAM)disp);
        g_ncom++;
    }
    RegCloseKey(hk);
}

/**
 * @brief Attempt to start the usbipd Windows service if it is stopped.
 *
 * Opens the Service Control Manager, queries the "usbipd" service state,
 * and calls StartService() if the service is in the STOPPED state.
 * Waits up to 4 seconds for the service to enter RUNNING state.
 * Silently returns if the service is already running, not installed,
 * or if the process lacks the required SERVICE_START privilege.
 *
 * advapi32.lib is already in the link line; winsvc.h is pulled in by windows.h.
 */
static void try_start_usbipd_service(void) {
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) return;

    SC_HANDLE hSvc = OpenServiceA(hSCM, "usbipd",
                                   SERVICE_START | SERVICE_QUERY_STATUS);
    if (hSvc) {
        SERVICE_STATUS ss = {0};
        if (QueryServiceStatus(hSvc, &ss) &&
            ss.dwCurrentState == SERVICE_STOPPED) {
            StartServiceA(hSvc, 0, NULL);
            /* Poll until running or timeout (~4 s) */
            for (int i = 0; i < 8; i++) {
                Sleep(500);
                if (QueryServiceStatus(hSvc, &ss) &&
                    ss.dwCurrentState == SERVICE_RUNNING)
                    break;
            }
        }
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hSCM);
}

/**
 * @brief Run `usbipd list` and populate g_devs[] for BUSID lookup.
 *
 * Not used for the visible Windows list (that comes from the registry).
 * Required to map a COM port number to a usbipd BUSID so bind/attach work.
 *
 * If usbipd list returns no output, the usbipd service is likely stopped.
 * try_start_usbipd_service() is called automatically and the list is
 * retried once.
 */
static void usbipd_list_devices(void) {
    g_ndev = 0;
    if (!g_usbipd[0]) return;

    char raw[RAW_SZ] = {0};
    usbipd_capture("list", raw, sizeof(raw));

    /* No output → service probably stopped; try to start it and retry once */
    if (!raw[0]) {
        try_start_usbipd_service();
        usbipd_capture("list", raw, sizeof(raw));
    }

    for (char *line = raw; *line; ) {
        char *end = strchr(line, '\n');
        char  tmp[512];
        int   len = end ? (int)(end - line) : (int)strlen(line);
        if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
        memcpy(tmp, line, len);
        rtrim(tmp, &len);

        if (is_busid_line(tmp) && g_ndev < MAX_DEV) {
            Dev *d = &g_devs[g_ndev];
            /* Extract BUSID (first non-space token) */
            int i = 0;
            while (i < BUSID_SZ - 1 && tmp[i] && !isspace((unsigned char)tmp[i]))
                { d->busid[i] = tmp[i]; i++; }
            d->busid[i] = '\0';

            parse_vidpid(tmp, d->vidpid);

            /*
             * Classify by STATE column keyword.
             * "Attached" → ST_WSL.  "Shared" (capital S) → ST_SHARED.
             * "Not shared" has lowercase 's', so strstr("Shared") misses it → ST_WINDOWS.
             */
            d->state = strstr(tmp, "Attached") ? ST_WSL
                     : strstr(tmp, "Shared")   ? ST_SHARED
                                               : ST_WINDOWS;

            snprintf(d->display, sizeof(d->display), "%s", tmp);
            g_ndev++;
        }
        if (!end) break;
        line = end + 1;
    }
}

/**
 * @brief Run `wsl.exe sh -c "ls /dev/ttyUSB* /dev/ttyACM*"` and populate
 *        the WSL tty listbox.
 *
 * Updates g_tty[] and g_ntty with the returned paths.
 * If WSL is not running or no tty devices exist, the listbox is left empty.
 */
static void list_wsl_tty(void) {
    SendMessage(g_wsl_list, LB_RESETCONTENT, 0, 0);
    g_ntty = 0;

    char raw[RAW_SZ] = {0};
    wsl_capture("ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null", raw, sizeof(raw));

    for (char *wl = raw; *wl && g_ntty < MAX_DEV; ) {
        char *we  = strchr(wl, '\n');
        char  tmp[64];
        int   len = we ? (int)(we - wl) : (int)strlen(wl);
        if (len < 1 || len > (int)sizeof(tmp) - 1) { if (!we) break; wl = we+1; continue; }
        memcpy(tmp, wl, len);
        rtrim(tmp, &len);
        if (tmp[0] == '/') {
            snprintf(g_tty[g_ntty++], sizeof(g_tty[0]), "%s", tmp);
            SendMessageA(g_wsl_list, LB_ADDSTRING, 0, (LPARAM)tmp);
        }
        if (!we) break;
        wl = we + 1;
    }
}

/**
 * @brief Populate both Bluetooth-tab listboxes.
 *
 * Left  — g_devs[] entries whose usbipd description contains "Bluetooth",
 *         shown as "BUSID  VID:PID  STATE".  The first row is pre-selected
 *         so the attach/return buttons work without a click.
 *         usbipd_list_devices() must have run first.
 * Right — `ls /sys/class/bluetooth` inside WSL (hci0, hci1, ...), i.e. the
 *         controllers the WSL kernel has bound with btusb.
 *
 * Updates g_nbt with the count of adapters added.
 */
static void list_bluetooth(void) {
    SendMessage(g_bt_list, LB_RESETCONTENT, 0, 0);
    g_nbt = 0;
    for (int i = 0; i < g_ndev; i++) {
        /* ponytail: name match only; relies on the driver's friendly name,
         * e.g. "Intel(R) Wireless Bluetooth(R)" */
        if (!strstr(g_devs[i].display, "Bluetooth")) continue;

        /* STATE is the last column; usbipd separates columns with 2+ spaces */
        const char *st = g_devs[i].display, *p;
        for (p = st; (p = strstr(p, "  ")) != NULL; p++) st = p;
        while (*st == ' ') st++;

        char disp[128];
        snprintf(disp, sizeof(disp), "%-6s %s  %s",
                 g_devs[i].busid, g_devs[i].vidpid, st);
        SendMessageA(g_bt_list, LB_ADDSTRING, 0, (LPARAM)disp);
        g_nbt++;
    }
    SendMessage(g_bt_list, LB_SETCURSEL, 0, 0);

    SendMessage(g_bt_wsl, LB_RESETCONTENT, 0, 0);
    char raw[RAW_SZ] = {0};
    wsl_capture("ls /sys/class/bluetooth 2>/dev/null", raw, sizeof(raw));
    for (char *tok = strtok(raw, " \r\n"); tok; tok = strtok(NULL, " \r\n"))
        if (strncmp(tok, "hci", 3) == 0)
            SendMessageA(g_bt_wsl, LB_ADDSTRING, 0, (LPARAM)tok);
}

/**
 * @brief Read the BUSID of the selected Bluetooth adapter row.
 *
 * Rows are formatted by list_bluetooth() with the BUSID as first token.
 *
 * @param out  Buffer of at least BUSID_SZ bytes.
 * @return     Non-zero if a row is selected.
 */
static int bt_busid(char *out) {
    int sel = (int)SendMessage(g_bt_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return 0;
    char text[128] = {0};
    SendMessageA(g_bt_list, LB_GETTEXT, sel, (LPARAM)text);
    return sscanf(text, "%15s", out) == 1;
}

/**
 * @brief Refresh all listboxes and the status bar.
 *
 * 1. Reads Windows COM ports from the registry into the left listbox.
 * 2. Runs `usbipd list` in the background to populate g_devs[]
 *    (needed for BUSID lookup; not shown directly).
 * 3. Queries WSL for /dev/tty* devices; populates the right listbox.
 * 4. Fills the Bluetooth tab from g_devs[] and WSL /sys/class/bluetooth.
 * 5. Updates the status bar with counts and any warnings.
 *
 * The Refresh button is disabled during the operation.
 */
static void refresh(void) {
    SetWindowTextA(g_status, "Refreshing...");
    EnableWindow(GetDlgItem(g_hwnd, IDC_REFRESH), FALSE);

    list_registry_com_ports();
    usbipd_list_devices();
    list_wsl_tty();
    list_bluetooth();

    /* Build status message */
    char st[320];
    if (g_ncom == 0 && g_ntty == 0 && g_nbt == 0) {
        snprintf(st, sizeof(st),
            "No COM ports, WSL tty devices or Bluetooth adapters found."
            "%s",
            g_usbipd[0] ? "" : "  usbipd not found (attach/detach unavailable).");
    } else {
        const char *elev = is_elevated() ? "" : "  [Run as Administrator for attach/detach]";
        const char *usb  = g_usbipd[0] ? "" : "  [usbipd not found]";
        snprintf(st, sizeof(st), "%d COM port(s)   %d WSL tty(s)   %d Bluetooth adapter(s)%s%s",
                 g_ncom, g_ntty, g_nbt, elev, usb);
    }
    SetWindowTextA(g_status, st);
    EnableWindow(GetDlgItem(g_hwnd, IDC_REFRESH), TRUE);
}

/* ── spinner factory ─────────────────────────────────────────────────────── */

/**
 * @brief Create an EDIT + UPDOWN buddy pair (a number spinner).
 *
 * The EDIT shows and accepts the numeric value; the UPDOWN overlays the
 * right edge of the EDIT via UDS_ALIGNRIGHT and provides arrow buttons.
 * UDS_SETBUDDYINT keeps the EDIT in sync automatically.
 *
 * @param hwnd      Parent window.
 * @param hInst     Module instance.
 * @param edit_id   Control ID for the EDIT.
 * @param spin_id   Control ID for the UPDOWN.
 * @param ex        EDIT x position in client coordinates.
 * @param ey        EDIT y position in client coordinates.
 * @param ew        EDIT width (the UPDOWN arrow buttons overlay the right ~18px).
 * @param def_val   Initial numeric value.
 * @param hfont     Font applied to the EDIT.
 * @return          Handle to the UPDOWN control (NULL if class not registered).
 */
static HWND make_spinner(HWND hwnd, HINSTANCE hInst,
                          int edit_id, int spin_id,
                          int ex, int ey, int ew,
                          int def_val, HFONT hfont) {
    HWND hEdit = CreateWindowA("EDIT", "",
        WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER|ES_CENTER|ES_AUTOHSCROLL,
        ex, ey, ew, 22, hwnd, (HMENU)(INT_PTR)edit_id, hInst, NULL);
    SendMessage(hEdit, WM_SETFONT, (WPARAM)hfont, TRUE);

    HWND hSpin = CreateWindowA(UPDOWN_CLASS, NULL,
        WS_CHILD|WS_VISIBLE|
        UDS_SETBUDDYINT|UDS_ALIGNRIGHT|UDS_NOTHOUSANDS|UDS_ARROWKEYS,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)spin_id, hInst, NULL);
    if (!hSpin) return NULL;

    SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit,     0);
    SendMessage(hSpin, UDM_SETRANGE, 0, MAKELPARAM(255, 0));
    SendMessage(hSpin, UDM_SETPOS,   0, (LPARAM)def_val);
    return hSpin;
}

/**
 * @brief Read the current integer value from an UPDOWN spinner.
 *
 * @param h  Handle to the UPDOWN; NULL-safe (returns 0 for NULL).
 * @return   Current spinner value.
 */
static int spin_val(HWND h) {
    if (!h) return 0;
    return (int)(short)LOWORD(SendMessage(h, UDM_GETPOS, 0, 0));
}

/**
 * @brief Show the controls of tab page @p tab and hide the others.
 *
 * @param tab  Tab index (0 = Serial, 1 = Bluetooth).
 */
static void show_tab(int tab) {
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 12; i++)
            if (g_pages[p][i])
                ShowWindow(g_pages[p][i], p == tab ? SW_SHOW : SW_HIDE);
}

/* ── window procedure ────────────────────────────────────────────────────── */

/**
 * @brief Main window message procedure.
 *
 * **WM_CREATE** — creates a tab control holding two-column pages:
 *   Serial tab, left (x=14, w=320):
 *     "Windows COM Ports" label, COM listbox, "COM port #:" label,
 *     COM spinner (default 4), "-> Attach to WSL" button.
 *   Serial tab, right (x=346, w=320):
 *     "WSL tty Ports" label, tty listbox, "ttyUSB/ACM #:" label,
 *     tty spinner (default 0), "<- Return to Windows" button.
 *   Bluetooth tab: adapter listbox + "-> Attach to WSL" on the left,
 *     WSL hci listbox + "<- Return to Windows" on the right.
 *   Bottom row (below the tabs): "Refresh" button, status label.
 *   Calls refresh() on startup.
 *
 * **WM_COMMAND**:
 *   IDC_REFRESH   — calls refresh().
 *   IDC_TO_WSL    — reads COM spinner → busid_for_com() → usbipd bind + attach.
 *   IDC_TO_WIN    — re-runs usbipd list → reads tty spinner → busid_for_tty()
 *                   (WSL sysfs VID:PID lookup) → usbipd detach.
 *   IDC_BT_TO_WSL — selected adapter → usbipd bind --force + attach,
 *                   then `modprobe btusb` in WSL.
 *   IDC_BT_TO_WIN — selected adapter → usbipd unbind.
 *   IDC_UNSTICK   — in-use vhci ports in WSL → echo port > vhci_hcd.0/detach.
 *
 * **WM_NOTIFY** — TCN_SELCHANGE switches the visible tab page.
 *
 * **WM_DESTROY** — posts WM_QUIT.
 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;
        HINSTANCE hInst = GetModuleHandleA(NULL);
        HFONT mono = (HFONT)GetStockObject(ANSI_FIXED_FONT);
        HFONT gui  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        /* ── Serial tab, left column (x=14, panel width=320) ── */

        /** Panel label */
        HWND lbl_w = CreateWindowA("STATIC", "Windows COM Ports",
            WS_CHILD|WS_VISIBLE, 14, 38, 320, 16, hwnd, NULL, hInst, NULL);

        /**
         * COM port listbox — populated from HKLM\HARDWARE\DEVICEMAP\SERIALCOMM.
         * Shows every COM port currently registered in the OS, regardless
         * of usbipd status.
         */
        g_win_list = CreateWindowA("LISTBOX", NULL,
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_HSCROLL|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY,
            14, 56, 320, 168, hwnd, (HMENU)IDC_WIN_LIST, hInst, NULL);
        SendMessage(g_win_list, WM_SETFONT, (WPARAM)mono, TRUE);

        /** "COM port #:" label aligned with the spinner */
        HWND lbl_cn = CreateWindowA("STATIC", "COM port #:",
            WS_CHILD|WS_VISIBLE, 14, 236, 82, 16, hwnd, NULL, hInst, NULL);

        /**
         * COM number spinner.  EDIT at x=100, width 58 (arrows overlay right
         * ~18px).  Default value 4 — COM4 is the typical ESP32 port.
         */
        g_com_spin = make_spinner(hwnd, hInst,
                                   IDC_COM_EDIT, IDC_COM_SPIN,
                                   100, 232, 58, 4, gui);

        /** Attach button — runs usbipd bind then usbipd attach --wsl */
        HWND btn_wsl = CreateWindowA("BUTTON", "-> Attach to WSL",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            166, 230, 164, 28, hwnd, (HMENU)IDC_TO_WSL, hInst, NULL);

        /* ── Serial tab, right column (x=346, panel width=320) ── */

        /** Panel label */
        HWND lbl_t = CreateWindowA("STATIC", "WSL tty Ports",
            WS_CHILD|WS_VISIBLE, 346, 38, 320, 16, hwnd, NULL, hInst, NULL);

        /**
         * WSL tty listbox — populated from `ls /dev/ttyUSB* /dev/ttyACM*`
         * inside WSL.  Shows which tty devices are currently available in WSL.
         */
        g_wsl_list = CreateWindowA("LISTBOX", NULL,
            WS_CHILD|WS_VISIBLE|WS_BORDER|WS_HSCROLL|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY,
            346, 56, 320, 168, hwnd, (HMENU)IDC_WSL_LIST, hInst, NULL);
        SendMessage(g_wsl_list, WM_SETFONT, (WPARAM)mono, TRUE);

        /** "ttyUSB/ACM #:" label aligned with the tty spinner */
        HWND lbl_tn = CreateWindowA("STATIC", "ttyUSB/ACM #:",
            WS_CHILD|WS_VISIBLE, 346, 236, 100, 16, hwnd, NULL, hInst, NULL);

        /**
         * tty number spinner.  EDIT at x=450, default 0 (ttyUSB0 / ttyACM0).
         * busid_for_tty() tries ttyUSBn first, then ttyACMn.
         */
        g_tty_spin = make_spinner(hwnd, hInst,
                                   IDC_TTY_EDIT, IDC_TTY_SPIN,
                                   450, 232, 58, 0, gui);

        /** Detach button — runs usbipd detach to return device to Windows */
        HWND btn_win = CreateWindowA("BUTTON", "<- Return to Windows",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            516, 230, 150, 28, hwnd, (HMENU)IDC_TO_WIN, hInst, NULL);

        /* ── Bluetooth tab (same grid as the Serial tab) ── */

        HWND lbl_bw = CreateWindowA("STATIC", "Windows Bluetooth Adapters (usbipd)",
            WS_CHILD, 14, 38, 320, 16, hwnd, NULL, hInst, NULL);

        /** Adapter listbox — "BUSID  VID:PID  STATE" rows from list_bluetooth() */
        g_bt_list = CreateWindowA("LISTBOX", NULL,
            WS_CHILD|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY,
            14, 56, 320, 168, hwnd, (HMENU)IDC_BT_LIST, hInst, NULL);
        SendMessage(g_bt_list, WM_SETFONT, (WPARAM)mono, TRUE);

        /** Attach button — usbipd bind --force + attach --wsl + modprobe btusb */
        HWND btn_bt_wsl = CreateWindowA("BUTTON", "-> Attach to WSL",
            WS_CHILD|BS_PUSHBUTTON,
            166, 230, 164, 28, hwnd, (HMENU)IDC_BT_TO_WSL, hInst, NULL);

        HWND lbl_bl = CreateWindowA("STATIC", "WSL Bluetooth Controllers",
            WS_CHILD, 346, 38, 320, 16, hwnd, NULL, hInst, NULL);

        /** WSL controller listbox — hci* entries from /sys/class/bluetooth */
        g_bt_wsl = CreateWindowA("LISTBOX", NULL,
            WS_CHILD|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,
            346, 56, 320, 168, hwnd, (HMENU)IDC_BT_WSL, hInst, NULL);
        SendMessage(g_bt_wsl, WM_SETFONT, (WPARAM)mono, TRUE);

        /** Return button — usbipd unbind (also detaches) */
        HWND btn_bt_win = CreateWindowA("BUTTON", "<- Return to Windows",
            WS_CHILD|BS_PUSHBUTTON,
            516, 230, 150, 28, hwnd, (HMENU)IDC_BT_TO_WIN, hInst, NULL);

        /* ── Bottom row (below the tabs) ── */

        /** Refresh — re-reads registry COM ports, re-runs usbipd list, re-scans WSL */
        HWND btn_ref = CreateWindowA("BUTTON", "Refresh",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            8, 274, 90, 28, hwnd, (HMENU)IDC_REFRESH, hInst, NULL);

        /** Clear stuck — detaches orphaned usbip ports from the WSL side */
        HWND btn_unstick = CreateWindowA("BUTTON", "Clear stuck USB",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            104, 274, 110, 28, hwnd, (HMENU)IDC_UNSTICK, hInst, NULL);

        /** Status label — two-line area for counts, usbipd path, errors */
        g_status = CreateWindowA("STATIC", "",
            WS_CHILD|WS_VISIBLE,
            222, 278, 450, 36, hwnd, (HMENU)IDC_STATUS, hInst, NULL);

        /*
         * ── Status strip (y=316): privilege left, usbipd right ──
         * Both coloured via WM_CTLCOLORSTATIC: green = ok, red = problem.
         * Width 326 each; 8px left margin, 12px gap, 8px right margin = 680.
         */
        g_elevated_cache = is_elevated();
        g_priv_label = CreateWindowA("STATIC",
            g_elevated_cache
                ? "Administrator: OK  \x97  attach/detach enabled"
                : "Administrator: NO  \x97  run as Administrator",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            8, 316, 326, 18, hwnd, (HMENU)IDC_PRIV, hInst, NULL);

        g_usbipd_label = CreateWindowA("STATIC",
            g_usbipd[0]
                ? "usbipd: installed  \x97  found on PATH"
                : "usbipd: NOT FOUND  \x97  install from github.com/dorssel/usbipd-win",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            346, 316, 326, 18, hwnd, (HMENU)IDC_USBIPD_ST, hInst, NULL);

        /* ── Author credit ── */
        HWND lbl_credit = CreateWindowA("STATIC",
            "by Matteo Vittorio Ricciutelli  \xb7  2026",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            8, 336, 664, 14, hwnd, (HMENU)IDC_CREDIT, hInst, NULL);

        /*
         * ── Tab control (frames both pages) ──
         * Moved to the bottom of the Z-order and given WS_CLIPSIBLINGS so it
         * never paints over the page controls that sit on top of it.
         */
        g_tabs = CreateWindowA(WC_TABCONTROLA, NULL,
            WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
            4, 4, 672, 262, hwnd, (HMENU)IDC_TABS, hInst, NULL);
        SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SendMessage(g_tabs, WM_SETFONT, (WPARAM)gui, TRUE);
        TCITEMA ti = { TCIF_TEXT };
        ti.pszText = "Serial (COM)";
        SendMessageA(g_tabs, TCM_INSERTITEMA, 0, (LPARAM)&ti);
        ti.pszText = "Bluetooth";
        SendMessageA(g_tabs, TCM_INSERTITEMA, 1, (LPARAM)&ti);

        HWND pages[2][12] = {
            { lbl_w, g_win_list, lbl_cn, GetDlgItem(hwnd, IDC_COM_EDIT), g_com_spin, btn_wsl,
              lbl_t, g_wsl_list, lbl_tn, GetDlgItem(hwnd, IDC_TTY_EDIT), g_tty_spin, btn_win },
            { lbl_bw, g_bt_list, btn_bt_wsl, lbl_bl, g_bt_wsl, btn_bt_win },
        };
        memcpy(g_pages, pages, sizeof(pages));

        /* Apply GUI font to all non-listbox controls */
        HWND gfx[] = { lbl_w, lbl_t, lbl_cn, lbl_tn,
                       btn_wsl, btn_win, btn_ref, btn_unstick, g_status,
                       g_priv_label, g_usbipd_label, lbl_credit,
                       lbl_bw, lbl_bl, btn_bt_wsl, btn_bt_win };
        for (int i = 0; i < (int)(sizeof(gfx) / sizeof(gfx[0])); i++)
            SendMessage(gfx[i], WM_SETFONT, (WPARAM)gui, TRUE);

        refresh();
        break;
    }

    case WM_COMMAND: {
        int  id = LOWORD(wp);
        char busid[BUSID_SZ] = {0};
        char args[CMD_SZ];
        char st[320];

        if (id == IDM_ABOUT) {
            MessageBoxA(hwnd,
                "WSL COM Port Swapper v" APP_VERSION "\r\n\r\n"
                "Forward USB serial devices between Windows and WSL2\r\n"
                "via usbipd-win \x97 no command line required.\r\n\r\n"
                "Author:  Matteo Vittorio Ricciutelli\r\n"
                "Icon:    Icon made by Freepik from www.flaticon.com",
                "About WSL COM Port Swapper",
                MB_OK | MB_ICONINFORMATION);

        } else if (id == IDC_WIN_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            /*
             * Sync COM spinner to the selected Windows listbox entry.
             * Entry format: "COM4    \\Device\\USBSER000"
             */
            int sel = (int)SendMessage(g_win_list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[256] = {0};
                SendMessageA(g_win_list, LB_GETTEXT, sel, (LPARAM)text);
                int n = 0;
                if (sscanf(text, "COM%d", &n) == 1)
                    SendMessage(g_com_spin, UDM_SETPOS, 0, (LPARAM)n);
            }

        } else if (id == IDC_WSL_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            /*
             * Sync tty spinner to the selected WSL listbox entry.
             * Entry format: "/dev/ttyUSB0" or "/dev/ttyACM0"
             * Walk back from end of string to find trailing digits.
             */
            int sel = (int)SendMessage(g_wsl_list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char text[64] = {0};
                SendMessageA(g_wsl_list, LB_GETTEXT, sel, (LPARAM)text);
                const char *p = text + strlen(text);
                while (p > text && isdigit((unsigned char)p[-1])) p--;
                int n = 0;
                if (*p && sscanf(p, "%d", &n) == 1)
                    SendMessage(g_tty_spin, UDM_SETPOS, 0, (LPARAM)n);
            }

        } else if (id == IDC_REFRESH) {
            refresh();

        } else if (id == IDC_TO_WSL) {
            /*
             * Forward a Windows COM port into WSL.
             *
             * 1. Read COM number from the left spinner.
             * 2. Re-run usbipd list fresh (state may have changed since refresh).
             * 3. Look up BUSID via busid_for_com() — tries description string
             *    match first, then VID:PID-from-registry fallback.
             * 4. Run `usbipd bind --busid X` — makes the device shareable.
             * 5. Run `usbipd attach --wsl --busid X` — forwards into WSL.
             *    Steps 4–5 require Administrator privileges.
             */
            if (!g_usbipd[0]) {
                SetWindowTextA(g_status, "usbipd not found. Install from github.com/dorssel/usbipd-win");
                break;
            }
            int cn = spin_val(g_com_spin);
            SetWindowTextA(g_status, "Querying usbipd device list...");
            usbipd_list_devices();   /* always refresh before lookup */
            if (!busid_for_com(cn, busid)) {
                /*
                 * Build a diagnostic message so the user can see what
                 * usbipd actually returned and why the match failed.
                 */
                char diag[1024] = {0};
                usbipd_capture("list", diag, sizeof(diag));
                if (!diag[0]) strncpy(diag, "(no output — is the usbipd service running?)", sizeof(diag)-1);

                char mb[2048];
                if (g_ndev == 0) {
                    snprintf(mb, sizeof(mb),
                        "COM%d not found.\r\n\r\n"
                        "usbipd list returned no device entries.\r\n\r\n"
                        "Raw output:\r\n%.800s",
                        cn, diag);
                } else {
                    /* Show what BUSIDs ARE in the list */
                    char devlist[512] = {0};
                    int  pos = 0;
                    for (int i = 0; i < g_ndev && pos < 490; i++) {
                        pos += snprintf(devlist + pos, 510 - pos,
                                        "  %s  %s\r\n",
                                        g_devs[i].busid, g_devs[i].display);
                    }
                    snprintf(mb, sizeof(mb),
                        "COM%d not matched in %d usbipd device(s).\r\n\r\n"
                        "Devices usbipd sees:\r\n%s\r\n"
                        "Tip: the VID:PID fallback also failed — check that "
                        "COM%d is a USB device (not Bluetooth or virtual).",
                        cn, g_ndev, devlist, cn);
                }
                MessageBoxA(hwnd, mb, "BUSID Lookup Failed", MB_OK | MB_ICONWARNING);
                snprintf(st, sizeof(st), "COM%d: BUSID lookup failed — see dialog.", cn);
                SetWindowTextA(g_status, st); break;
            }
            snprintf(st, sizeof(st),
                "Binding and attaching COM%d (BUSID %s) to WSL...", cn, busid);
            SetWindowTextA(g_status, st);

            snprintf(args, sizeof(args), "bind --busid %s", busid);
            usbipd_run(args);
            snprintf(args, sizeof(args), "attach --wsl --busid %s", busid);
            usbipd_run(args);
            snprintf(st, sizeof(st),
                "Attached COM%d (BUSID %s) to WSL \x97 waiting for tty to appear...", cn, busid);
            SetWindowTextA(g_status, st);
            SetTimer(hwnd, TIMER_DETACH_REFRESH, 2000, NULL);

        } else if (id == IDC_TO_WIN) {
            /*
             * Return a WSL tty device back to Windows.
             *
             * 1. Read tty number from the right spinner.
             * 2. Re-run usbipd list (state may have changed since refresh —
             *    device must show "Attached" for busid_for_tty to match).
             * 3. Query WSL sysfs for the device's VID:PID (busid_for_tty()).
             * 4. Match VID:PID against ST_WSL entries in g_devs to find BUSID.
             * 5. Run `usbipd detach --busid X` — device reappears in Windows.
             */
            if (!g_usbipd[0]) {
                SetWindowTextA(g_status, "usbipd not found. Install from github.com/dorssel/usbipd-win");
                break;
            }
            int tn = spin_val(g_tty_spin);
            SetWindowTextA(g_status, "Querying usbipd device list...");
            usbipd_list_devices();   /* always refresh — device must show Attached */
            SetWindowTextA(g_status, "Querying WSL sysfs for device VID:PID...");
            if (!busid_for_tty(tn, busid)) {
                /*
                 * Diagnostic: show what sysfs returned and what usbipd sees
                 * so the user can tell whether it's a sysfs path issue or a
                 * state mismatch (device not yet Attached in usbipd).
                 * Re-use tty_vidpid() — same query busid_for_tty() already ran.
                 */
                char sysfs_vidpid[VIDPID_SZ] = {0};
                tty_vidpid(tn, sysfs_vidpid, sizeof(sysfs_vidpid));

                char devlist[512] = {0};
                int  pos = 0;
                for (int i = 0; i < g_ndev && pos < 490; i++) {
                    const char *state_s =
                        g_devs[i].state == ST_WSL    ? "Attached" :
                        g_devs[i].state == ST_SHARED ? "Shared"   : "Windows";
                    pos += snprintf(devlist + pos, 510 - pos,
                        "  %s  %s  [%s]\r\n",
                        g_devs[i].busid, g_devs[i].vidpid, state_s);
                }

                char mb[2048];
                snprintf(mb, sizeof(mb),
                    "ttyUSB%d / ttyACM%d: cannot identify which device to detach.\r\n\r\n"
                    "sysfs VID:PID: %s\r\n\r\n"
                    "usbipd devices (%d):\r\n%s\r\n"
                    "Multiple devices show [Attached] and sysfs lookup failed,\r\n"
                    "so the correct device cannot be determined automatically.\r\n\r\n"
                    "Fix options:\r\n"
                    "  - Detach all but one device first, then retry\r\n"
                    "  - Run 'usbipd detach --busid X' manually for the correct BUSID",
                    tn, tn,
                    sysfs_vidpid[0] ? sysfs_vidpid : "(empty — sysfs path not found)",
                    g_ndev, g_ndev ? devlist : "  (none)\r\n");
                MessageBoxA(hwnd, mb, "WSL Device Lookup Failed", MB_OK | MB_ICONWARNING);
                snprintf(st, sizeof(st), "ttyUSB/ACM%d: not found — see dialog.", tn);
                SetWindowTextA(g_status, st); break;
            }
            snprintf(st, sizeof(st),
                "Detaching tty*%d (BUSID %s) from WSL...", tn, busid);
            SetWindowTextA(g_status, st);

            snprintf(args, sizeof(args), "detach --busid %s", busid);
            usbipd_run(args);
            snprintf(st, sizeof(st),
                "Detached BUSID %s \x97 waiting for Windows to enumerate device...", busid);
            SetWindowTextA(g_status, st);
            /* Delay the refresh so Windows has time to re-enumerate the COM port */
            SetTimer(hwnd, TIMER_DETACH_REFRESH, 2000, NULL);

        } else if (id == IDC_BT_TO_WSL || id == IDC_BT_TO_WIN) {
            /*
             * Move the selected USB Bluetooth adapter between Windows and WSL.
             *
             * To WSL:
             *   1. `usbipd bind --busid X --force` — Bluetooth radios need a
             *      forced bind, which takes the adapter away from Windows.
             *   2. `usbipd attach --wsl --busid X`.
             *   3. `modprobe btusb` as root — the WSL kernel ships the USB
             *      Bluetooth driver as a loadable module.
             * To Windows:
             *   `usbipd unbind --busid X` — detaches from WSL and drops the
             *   forced bind so Windows reloads its Bluetooth driver.
             */
            if (!g_usbipd[0]) {
                SetWindowTextA(g_status, "usbipd not found. Install from github.com/dorssel/usbipd-win");
                break;
            }
            if (!bt_busid(busid)) {
                SetWindowTextA(g_status, "No Bluetooth adapter selected \x97 click Refresh.");
                break;
            }
            if (id == IDC_BT_TO_WSL) {
                char mb[512];
                snprintf(mb, sizeof(mb),
                    "Attach Bluetooth adapter (BUSID %s) to WSL?\r\n\r\n"
                    "This force-binds the adapter: Windows loses Bluetooth "
                    "(paired mice, keyboards, headsets) until you click "
                    "\"<- Return to Windows\".",
                    busid);
                if (MessageBoxA(hwnd, mb, "Attach Bluetooth to WSL",
                                MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                    break;

                snprintf(st, sizeof(st),
                    "Binding and attaching Bluetooth (BUSID %s) to WSL...", busid);
                SetWindowTextA(g_status, st);
                snprintf(args, sizeof(args), "bind --busid %s --force", busid);
                usbipd_run(args);
                snprintf(args, sizeof(args), "attach --wsl --busid %s", busid);
                usbipd_run(args);

                /* Marker instead of exit code: proc_capture only returns output,
                 * and wsl.exe may prepend its own warnings to it */
                char mp[RAW_SZ] = {0};
                proc_capture("wsl.exe -u root sh -c \"modprobe btusb && echo btusb-ok\"",
                             mp, sizeof(mp), 5000);
                snprintf(st, sizeof(st),
                    strstr(mp, "btusb-ok")
                        ? "Attached Bluetooth (BUSID %s) to WSL \x97 waiting for hci to appear..."
                        : "Attached Bluetooth (BUSID %s) to WSL, but 'modprobe btusb' failed "
                          "\x97 run 'sudo modprobe btusb' in WSL.",
                    busid);
            } else {
                snprintf(args, sizeof(args), "unbind --busid %s", busid);
                usbipd_run(args);
                snprintf(st, sizeof(st),
                    "Returned Bluetooth (BUSID %s) to Windows \x97 waiting for driver to reload...",
                    busid);
            }
            SetWindowTextA(g_status, st);
            SetTimer(hwnd, TIMER_DETACH_REFRESH, 2000, NULL);

        } else if (id == IDC_UNSTICK) {
            /*
             * Free "phantom" devices stuck on the WSL side of usbip.
             *
             * If Windows drops the link (unplug, usbipd restart) without WSL
             * noticing, the vhci port stays in use and the stale /dev/tty*
             * remains.  usbipd detach can't help — Windows no longer lists
             * the device as Attached — so detach it from the WSL side:
             *   echo <port> > /sys/devices/platform/vhci_hcd.0/detach
             *
             * Status table row: "hub port sta spd dev sockfd local_busid";
             * sta 6 = in use, 7 = error.  Every such port is detached, so
             * devices that still work are dropped too and need re-attaching.
             */
            char raw[RAW_SZ] = {0};
            wsl_capture("cat " VHCI_SYSFS "/status 2>/dev/null", raw, sizeof(raw));

            unsigned ports[MAX_DEV];
            int  np = 0, pos = 0;
            char list[1024] = {0};
            for (char *ln = strtok(raw, "\r\n"); ln && np < MAX_DEV; ln = strtok(NULL, "\r\n")) {
                char hub[4], lbus[BUSID_SZ];
                unsigned port, sta;
                if (sscanf(ln, "%3s %u %u %*s %*s %*s %15s", hub, &port, &sta, lbus) == 4 &&
                    (sta == 6 || sta == 7)) {
                    ports[np++] = port;
                    if (pos < (int)sizeof(list) - 64)
                        pos += snprintf(list + pos, sizeof(list) - pos,
                                        "  port %u  (WSL busid %s)\r\n", port, lbus);
                }
            }
            if (np == 0) {
                SetWindowTextA(g_status, "No usbip ports in use in WSL \x97 nothing to clear.");
                break;
            }

            char mb[1400];
            snprintf(mb, sizeof(mb),
                "Detach %d USB device(s) from the WSL side?\r\n\r\n%s\r\n"
                "Use this when a device is stuck in WSL (stale /dev/tty*) but "
                "usbipd no longer shows it as Attached.  Devices that still "
                "work in WSL are disconnected too; re-attach them afterwards.",
                np, list);
            if (MessageBoxA(hwnd, mb, "Clear stuck USB devices",
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                break;

            /* Marker per port: proc_capture gives output, not exit codes */
            int ok = 0;
            for (int i = 0; i < np; i++) {
                char cmd[CMD_SZ], out[512] = {0};
                snprintf(cmd, sizeof(cmd),
                    "wsl.exe -u root sh -c \"echo %u > " VHCI_SYSFS "/detach && echo detach-ok\"",
                    ports[i]);
                proc_capture(cmd, out, sizeof(out), 5000);
                if (strstr(out, "detach-ok")) ok++;
            }
            snprintf(st, sizeof(st),
                "Cleared %d of %d stuck usbip port(s) in WSL \x97 refreshing...", ok, np);
            SetWindowTextA(g_status, st);
            SetTimer(hwnd, TIMER_DETACH_REFRESH, 2000, NULL);
        }
        break;
    }

    case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == IDC_TABS &&
            ((LPNMHDR)lp)->code == TCN_SELCHANGE)
            show_tab((int)SendMessage(g_tabs, TCM_GETCURSEL, 0, 0));
        break;

    case WM_TIMER:
        if (wp == TIMER_DETACH_REFRESH) {
            KillTimer(hwnd, TIMER_DETACH_REFRESH);
            refresh();
        }
        break;

    /**
     * @brief Colour the privilege indicator label.
     *
     * Returns a green text colour when elevated, red when not.
     * The background matches the dialog face colour so the control
     * blends into the window.  Other STATIC controls fall through to
     * DefWindowProcA for default handling.
     */
    case WM_CTLCOLORSTATIC: {
        HWND hCtrl = (HWND)lp;
        if (hCtrl == g_priv_label || hCtrl == g_usbipd_label) {
            HDC hdc = (HDC)wp;
            int ok = (hCtrl == g_priv_label) ? g_elevated_cache : (g_usbipd[0] != '\0');
            SetTextColor(hdc, ok ? RGB(0, 140, 0) : RGB(180, 0, 0));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ── entry point ─────────────────────────────────────────────────────────── */

/**
 * @brief Windows GUI application entry point.
 *
 * 1. Calls InitCommonControlsEx(ICC_UPDOWN_CLASS) so UPDOWN_CLASS is
 *    registered before WM_CREATE creates spinner controls.
 * 2. Calls find_usbipd() to resolve the usbipd.exe path and sets
 *    WSL_UTF8=1 so wsl.exe output stays parseable.
 * 3. Registers the "WslUsbHelper" window class.
 * 4. Uses AdjustWindowRect to compute the outer window size so the
 *    client area is exactly 680 × 354 pixels — wide enough for the tab
 *    control, both columns and their controls.
 * 5. Creates the main window (no resize handle, no maximise) and runs
 *    the Win32 message loop.
 *
 * @param hInst  Current process instance handle.
 * @param hPrev  Always NULL on Win32 (legacy parameter).
 * @param lpCmd  Command-line string (not used).
 * @param nShow  Initial show state.
 * @return       wParam from the WM_QUIT message.
 */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    /* Register all common controls (including UPDOWN_CLASS) before CreateWindow */
    InitCommonControls();

    find_usbipd();

    /* wsl.exe writes its own messages (e.g. wsl.conf warnings) in UTF-16;
     * the first NUL byte truncates the captured output, emptying the WSL
     * lists.  WSL_UTF8=1 makes those messages plain UTF-8 lines instead. */
    SetEnvironmentVariableA("WSL_UTF8", "1");

    WNDCLASSA wc     = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "WslComPortSwapper";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconA(hInst, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassA(&wc);

    /*
     * Use AdjustWindowRect so the client area is exactly the size needed.
     * Rightmost control: tab x=4+672=676, plus 4px padding = 680 client width.
     * Bottom control:    credit y=336+14=350, plus 4px padding = 354 client height.
     */
    /* Build the menu bar before AdjustWindowRect so bMenu=TRUE is accurate */
    HMENU hMenuBar = CreateMenu();
    HMENU hHelp    = CreatePopupMenu();
    AppendMenuA(hHelp,    MF_STRING, IDM_ABOUT, "&About");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "&Help");

    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT  r     = {0, 0, 680, 354};
    AdjustWindowRect(&r, style, TRUE);  /* TRUE = has menu bar */

    HWND hwnd = CreateWindowA("WslComPortSwapper", "WSL COM Port Swapper v" APP_VERSION,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, hMenuBar, hInst, NULL);

    /* Set small taskbar / title-bar icon separately */
    HICON hSmall = (HICON)LoadImageA(hInst, MAKEINTRESOURCE(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (hSmall) SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    if (!g_usbipd[0]) {
        MessageBoxA(hwnd,
            "usbipd-win was not found on this system.\r\n\r\n"
            "Attach and detach operations will be unavailable until it is installed.\r\n\r\n"
            "Download and install from:\r\n"
            "  https://github.com/dorssel/usbipd-win/releases",
            "usbipd Not Found", MB_OK | MB_ICONWARNING);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
