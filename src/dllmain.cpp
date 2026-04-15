#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <string>

// ============================================================
//  Quick Menu Hotkeys v1.10 — Cross-Reference Pattern Scanner
//
//  Finds the OpenPanel function by cross-referencing multiple
//  known panel name strings. The common CALL target across
//  multiple panel string references = OpenPanel function.
//
//  PanelManager is resolved two ways:
//   1. Global pointer chain (immediate, no user action needed)
//   2. Hook fallback (captures RCX when game calls OpenPanel)
// ============================================================

// --- Globals ---
static uintptr_t g_gameBase = 0;
static DWORD     g_imageSize = 0;
static bool g_ready = false;

static volatile LONGLONG g_panelManager = 0;
static volatile LONG g_hookCounter = 0;

// Resolved addresses
static uintptr_t g_openPanelAddr  = 0;
static uintptr_t g_findPanelAddr  = 0;
static uintptr_t g_r8DataAddr     = 0;

// PanelManager global pointer chain (for proactive resolution)
static uintptr_t g_pmGlobalAddr = 0;
static int g_pmOffset1 = 0;
static int g_pmOffset2 = 0;

// Debug: capture panel names
static volatile LONGLONG g_lastRDX = 0;
static volatile LONG g_nameLogIndex = 0;
#define NAME_LOG_SIZE 64
static char g_nameLog[NAME_LOG_SIZE][128];

static FILE* g_logFile = nullptr;
static HMODULE g_hModule = nullptr;
static HWND g_gameWindow = nullptr;
static WNDPROC g_originalWndProc = nullptr;

// Toggle tracking: which panel was last opened via hotkey (-1 = none)
static volatile int g_currentPanel = -1;
static volatile bool g_panelConfirmedOpen = false;  // true once flag1 seen non-zero after open

#define WM_OPEN_PANEL  (WM_USER + 501)
#define WM_CLEAR_FLAGS (WM_USER + 502)

// ============================================================
//  Config
// ============================================================

struct PanelBinding {
    const char* section;
    const char* panelName;
    DWORD       defaultKey;
    DWORD       gameDefaultKey;  // Game-internal hardcoded key (0 = none)
    DWORD       key;
    DWORD       modifierKey;       // Keyboard modifier (Shift/Ctrl/Alt), 0 = none
    WORD        controllerButton;  // XInput button bitmask (0 = disabled)
    int         psButtonByteOff;   // HID button byte offset (0-3), -1 = disabled
    BYTE        psButtonBitMask;   // HID button bit mask
};

// Verified panels — default keybinds for core menus, 0x00 = user-configurable
//                                                          default  gameKey  key  mod  ctrl  psOff psMask
static PanelBinding g_panels[] = {
    // --- Core panels (default keybinds) ---
    { "Inventory",          "InventoryEquipmentPanel",          0x49, 0x49, 0, 0, 0, -1, 0 },  // I
    { "QuestBook",          "QuestMenuPanel",                   0x4A, 0x4A, 0, 0, 0, -1, 0 },  // J
    { "SkillBook",          "SkillTreePanel",                   0x4B, 0x4B, 0, 0, 0, -1, 0 },  // K
    { "Knowledge",          "KnowledgePanel2",                  0x4C, 0x00, 0, 0, 0, -1, 0 },  // L
    { "Options",            "LogoutView",                       0x4F, 0x00, 0, 0, 0, -1, 0 },  // O
    { "Map",                "WorldMapView",                     0x4D, 0x4D, 0, 0, 0, -1, 0 },  // M
    // --- Extra panels (no default keybind — configure in INI) ---
    { "Challenge",          "ChallengeMenuPanel2",              0x00, 0x00, 0, 0, 0, -1, 0 },
    { "FactionQuest",       "FactionQuestMenuPanel",            0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Guides",             "PlayGuideView",                    0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Notifications",      "AlertHistoryView",                 0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Pet",                "PetView",                          0x00, 0x00, 0, 0, 0, -1, 0 },
};

static const int NUM_PANELS = sizeof(g_panels) / sizeof(g_panels[0]);

static bool g_enabled  = true;
static bool g_debugLog = true;
static bool g_overrideGameKeys = true;
static DWORD g_reloadKey = 0;

// OpenPanel detour: trampoline to original function
static uintptr_t g_openPanelTrampoline = 0;
static volatile bool g_ourCall = false;

// --- XInput (dynamic loading) ---
struct XINPUT_GAMEPAD_LOCAL {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY;
    SHORT sThumbRX, sThumbRY;
};
struct XINPUT_STATE_LOCAL {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD_LOCAL Gamepad;
};
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE_LOCAL*);
static PFN_XInputGetState g_pXInputGetState = nullptr;
static HMODULE g_hXInput = nullptr;
static bool g_controllerEnabled = true;
static WORD g_controllerModifier = 0;
static WORD g_prevButtons = 0;

// --- PS Controller (HID Raw Input) ---
#define SONY_VID    0x054C
#define DS4_PID_1   0x05C4  // DualShock 4 v1
#define DS4_PID_2   0x09CC  // DualShock 4 v2
#define DS5_PID     0x0CE6  // DualSense
#define DS5_EDGE    0x0DF2  // DualSense Edge

static HANDLE   g_cachedHidDevice    = nullptr;
static bool     g_cachedIsSony       = false;
static int      g_cachedReportOffset = -1;
static volatile LONG g_hidButtonsPacked = 0;  // [buttons1 | buttons2<<8 | buttons3<<16]
static volatile bool g_hidConnected  = false;

static int  g_psModifierByteOff = -1;   // -1 = no modifier
static BYTE g_psModifierBitMask = 0;
static bool g_psModifierEnabled = false;

// --- Logging ---
static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

// CRC32 of a file (for detecting modded game files in logs)
static uint32_t FileCRC32(const char* path) {
    static const uint32_t table[256] = {
        0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
        0x0EDB8832,0x79DCB8A4,0xE0D5E91B,0x97D2D988,0x09B64C2B,0x7EB17CBF,0xE7B82D09,0x90BF1D9F,
        0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
        0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
        0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
        0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
        0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F0B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
        0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
        0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
        0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0D6B,0x086D3D2D,0x91646C97,0xE6635C01,
        0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
        0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
        0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
        0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7822,
        0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
        0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
        0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
        0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
        0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
        0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
        0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
        0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
        0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
        0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
        0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
        0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
        0x86D3D2D4,0xF1D4E242,0x68DDB3F6,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
        0x88085AE6,0xFF0F6B70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
        0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
        0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
        0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD706FF,0x54DE5729,0x23D967BF,
        0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
    };
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (size_t i = 0; i < n; i++)
            crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    fclose(f);
    return crc ^ 0xFFFFFFFF;
}

// --- INI ---
static DWORD ReadHexValue(const char* section, const char* key,
                           DWORD defaultVal, const char* iniPath) {
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath);
    if (buf[0] == '\0') return defaultVal;
    return (DWORD)strtoul(buf, nullptr, 16);
}

static bool ParsePSButtonName(const char* name, int* outByteOff, BYTE* outBitMask);  // forward decl

static void LoadConfig(const char* iniPath) {
    g_enabled  = GetPrivateProfileIntA("Settings", "Enabled",  1, iniPath) != 0;
    g_debugLog = GetPrivateProfileIntA("Settings", "DebugLog", 1, iniPath) != 0;
    g_overrideGameKeys = GetPrivateProfileIntA("Settings", "OverrideGameKeys", 1, iniPath) != 0;
    g_controllerEnabled = GetPrivateProfileIntA("Settings", "ControllerEnabled", 1, iniPath) != 0;
    g_controllerModifier = (WORD)ReadHexValue("Settings", "ControllerModifier", 0, iniPath);
    g_reloadKey = ReadHexValue("Settings", "ReloadKey", 0, iniPath);

    // PSModifier — global modifier for PS controller buttons
    char psMod[32];
    GetPrivateProfileStringA("Settings", "PSModifier", "none", psMod, sizeof(psMod), iniPath);
    g_psModifierEnabled = false;
    g_psModifierByteOff = -1;
    g_psModifierBitMask = 0;
    if (_stricmp(psMod, "none") != 0) {
        if (ParsePSButtonName(psMod, &g_psModifierByteOff, &g_psModifierBitMask))
            g_psModifierEnabled = true;
    }

    for (int i = 0; i < NUM_PANELS; i++) {
        g_panels[i].key = ReadHexValue(g_panels[i].section, "Hotkey",
                                        g_panels[i].defaultKey, iniPath);
        g_panels[i].modifierKey = ReadHexValue(g_panels[i].section, "ModifierKey", 0, iniPath);
        g_panels[i].controllerButton = (WORD)ReadHexValue(
            g_panels[i].section, "ControllerButton", 0, iniPath);

        // Per-panel PSButton (native HID)
        char psBtn[32];
        GetPrivateProfileStringA(g_panels[i].section, "PSButton", "none", psBtn, sizeof(psBtn), iniPath);
        g_panels[i].psButtonByteOff = -1;
        g_panels[i].psButtonBitMask = 0;
        if (_stricmp(psBtn, "none") != 0)
            ParsePSButtonName(psBtn, &g_panels[i].psButtonByteOff, &g_panels[i].psButtonBitMask);
    }
}

// --- XInput ---
static bool InitXInput() {
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3; i++) {
        g_hXInput = LoadLibraryA(dlls[i]);
        if (g_hXInput) {
            g_pXInputGetState = (PFN_XInputGetState)GetProcAddress(g_hXInput, "XInputGetState");
            if (g_pXInputGetState) {
                Log("XInput loaded: %s", dlls[i]);
                return true;
            }
            FreeLibrary(g_hXInput);
            g_hXInput = nullptr;
        }
    }
    Log("WARNING: XInput not available — controller support disabled");
    return false;
}

// --- PS Controller (HID Raw Input) ---

static bool ParsePSButtonName(const char* name, int* outByteOff, BYTE* outBitMask) {
    if (_stricmp(name, "Circle")   == 0) { *outByteOff = 0; *outBitMask = 0x40; return true; }
    if (_stricmp(name, "Triangle") == 0) { *outByteOff = 0; *outBitMask = 0x80; return true; }
    if (_stricmp(name, "Square")   == 0) { *outByteOff = 0; *outBitMask = 0x10; return true; }
    if (_stricmp(name, "Cross")    == 0) { *outByteOff = 0; *outBitMask = 0x20; return true; }
    if (_stricmp(name, "L1")       == 0) { *outByteOff = 1; *outBitMask = 0x01; return true; }
    if (_stricmp(name, "R1")       == 0) { *outByteOff = 1; *outBitMask = 0x02; return true; }
    if (_stricmp(name, "Share")    == 0) { *outByteOff = 1; *outBitMask = 0x10; return true; }
    if (_stricmp(name, "Options")  == 0) { *outByteOff = 1; *outBitMask = 0x20; return true; }
    if (_stricmp(name, "L3")       == 0) { *outByteOff = 1; *outBitMask = 0x40; return true; }
    if (_stricmp(name, "R3")       == 0) { *outByteOff = 1; *outBitMask = 0x80; return true; }
    if (_stricmp(name, "PS")       == 0) { *outByteOff = 2; *outBitMask = 0x01; return true; }
    if (_stricmp(name, "Touchpad") == 0) { *outByteOff = 2; *outBitMask = 0x02; return true; }
    if (_stricmp(name, "Mute")     == 0) { *outByteOff = 2; *outBitMask = 0x04; return true; }
    // D-Pad via hat-switch (virtual bits in byte 3)
    if (_stricmp(name, "DPadUp")    == 0) { *outByteOff = 3; *outBitMask = 0x01; return true; }
    if (_stricmp(name, "DPadDown")  == 0) { *outByteOff = 3; *outBitMask = 0x02; return true; }
    if (_stricmp(name, "DPadLeft")  == 0) { *outByteOff = 3; *outBitMask = 0x04; return true; }
    if (_stricmp(name, "DPadRight") == 0) { *outByteOff = 3; *outBitMask = 0x08; return true; }
    return false;
}

static void IdentifyHidDevice(HANDLE hDevice) {
    if (hDevice == g_cachedHidDevice) return;
    g_cachedHidDevice = hDevice;
    g_cachedIsSony = false;
    g_cachedReportOffset = -1;

    RID_DEVICE_INFO info = {};
    info.cbSize = sizeof(RID_DEVICE_INFO);
    UINT sz = sizeof(info);
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) return;
    if (info.dwType != RIM_TYPEHID) return;
    if (info.hid.dwVendorId != SONY_VID) return;

    g_cachedIsSony = true;
    WORD pid = (WORD)info.hid.dwProductId;
    if (pid == DS4_PID_1 || pid == DS4_PID_2) {
        g_cachedReportOffset = 5;  // DualShock 4 USB
    } else if (pid == DS5_PID || pid == DS5_EDGE) {
        g_cachedReportOffset = 8;  // DualSense USB
    }
    Log("HID device identified: VID=%04X PID=%04X → buttons offset=%d",
        SONY_VID, pid, g_cachedReportOffset);
}

static void ParseSonyHidReport(LPARAM lParam) {
    UINT dwSize = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
    if (dwSize == 0 || dwSize > 1024) return;

    BYTE buf[1024];
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &dwSize, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;

    RAWINPUT* raw = (RAWINPUT*)buf;
    if (raw->header.dwType != RIM_TYPEHID) return;
    if (raw->data.hid.dwCount == 0 || raw->data.hid.dwSizeHid == 0) return;

    IdentifyHidDevice(raw->header.hDevice);
    if (!g_cachedIsSony || g_cachedReportOffset < 0) return;

    BYTE* report = raw->data.hid.bRawData;
    DWORD reportLen = raw->data.hid.dwSizeHid;

    // Handle Bluetooth reports with report ID prefix
    int offset = g_cachedReportOffset;
    if (reportLen > 40 && report[0] == 0x31) {
        offset = 9;   // DualSense Bluetooth
    } else if (reportLen > 40 && report[0] == 0x11) {
        offset = 7;   // DualShock 4 Bluetooth
    }

    if ((DWORD)offset >= reportLen) return;

    // Cache all 3 button bytes + virtual D-Pad bits in a packed DWORD (atomic write)
    BYTE b0 = report[offset];
    BYTE b1 = ((DWORD)(offset + 1) < reportLen) ? report[offset + 1] : 0;
    BYTE b2 = ((DWORD)(offset + 2) < reportLen) ? report[offset + 2] : 0;
    // D-Pad: lower nibble of b0 is a hat-switch (0-7 = directions, 8 = neutral)
    BYTE hat = b0 & 0x0F;
    static const BYTE hatToDpad[9] = {
        0x01, 0x09, 0x08, 0x0A, 0x02, 0x06, 0x04, 0x05, 0x00
    //  Up    UpR   Right DnR   Down  DnL   Left  UpL   Neutral
    };
    BYTE dpad = (hat <= 8) ? hatToDpad[hat] : 0;
    LONG packed = (LONG)(b0 | (b1 << 8) | (b2 << 16) | (dpad << 24));
    InterlockedExchange(&g_hidButtonsPacked, packed);
    g_hidConnected = true;
}

// Check if the game is in a safe state for opening panels.
// Blocks during cutscenes, QTEs, minigames, etc.
// Subtypes: 0x06=cinema, 0x07=qte, 0x08=minigame, 0x0D/0x0E=normal gameplay
static bool IsGameplayState() {
    if (g_pmGlobalAddr == 0) return true;  // can't check, assume safe
    __try {
        uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
        if (!root) return true;
        uintptr_t mc = *(uintptr_t*)(root + 0x48);
        if (!mc) return true;
        uint8_t subtype = *(uint8_t*)(mc + 0xCA9);
        if (subtype >= 0x0D) return true;   // normal gameplay
        if (subtype == 0x0C) return true;    // main menu (ESC menu)
        Log("[Safety] Blocked: unsafe state (subtype=0x%02X)", subtype);
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return true;
}

// Menu state flag offsets (scanned dynamically from game code)
static int g_flagOffset1 = 0;  // first byte-write-zero offset (near "LogoutView")
static int g_flagOffset2 = 0;  // second byte-write-zero offset

static void ClearMenuStateFlags() {
    if (g_pmGlobalAddr != 0 && g_flagOffset1 != 0) {
        __try {
            uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
            if (root) {
                uintptr_t uiCtrl = *(uintptr_t*)(root + 0x48);
                if (uiCtrl) {
                    *(uint8_t*)(uiCtrl + g_flagOffset1) = 0;
                    *(uint8_t*)(uiCtrl + g_flagOffset2) = 0;
                    Log("Menu state flags cleared (0x%X/0x%X)", g_flagOffset1, g_flagOffset2);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("WARNING: Failed to clear menu flags");
        }
    }
}

// Check if flag1 (mc+0xCC5) is currently set — indicates a panel is open.
// Used at key-press time to verify the panel is still open before toggle-close.
static bool IsPanelFlagSet() {
    if (g_pmGlobalAddr == 0 || g_flagOffset1 == 0) return false;
    __try {
        uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
        if (!root) return false;
        uintptr_t mc = *(uintptr_t*)(root + 0x48);
        if (!mc) return false;
        return *(volatile uint8_t*)(mc + g_flagOffset1) != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ============================================================
//  Pattern Scanner
// ============================================================

static uintptr_t FindString(const char* str) {
    BYTE* base = (BYTE*)g_gameBase;
    int len = (int)strlen(str);
    for (DWORD i = 0; i + len + 1 < g_imageSize; i++) {
        if (base[i] == (BYTE)str[0] &&
            memcmp(base + i, str, len + 1) == 0) {
            return (uintptr_t)(base + i);
        }
    }
    return 0;
}

static uintptr_t FindLEA(uintptr_t targetAddr, uintptr_t afterAddr = 0) {
    BYTE* base = (BYTE*)g_gameBase;
    DWORD start = afterAddr > g_gameBase ? (DWORD)(afterAddr - g_gameBase) : 0;

    for (DWORD i = start; i + 7 < g_imageSize; i++) {
        BYTE* p = base + i;
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D) {
            BYTE modrm = p[2];
            if ((modrm & 0xC7) == 0x05) {
                int32_t disp = *(int32_t*)(p + 3);
                uintptr_t resolved = (uintptr_t)(p + 7) + disp;
                if (resolved == targetAddr)
                    return (uintptr_t)p;
            }
        }
    }
    return 0;
}

// Collect ALL CALL rel32 targets within range after a given address
#define MAX_CALLS_PER_LEA 16

static int FindAllCALLsAfter(uintptr_t from, int maxRange,
                              uintptr_t* targets, int maxTargets) {
    BYTE* start = (BYTE*)from;
    int count = 0;
    for (int i = 0; i < maxRange && count < maxTargets; i++) {
        if (start[i] == 0xE8) {
            int32_t rel = *(int32_t*)(start + i + 1);
            uintptr_t target = (uintptr_t)(start + i + 5) + rel;
            if (target > g_gameBase && target < g_gameBase + 0x10000000) {
                targets[count++] = target;
                i += 4;
            }
        }
    }
    return count;
}

// Cross-reference strategy:
// 1. Find multiple known panel strings
// 2. For each string, find LEA references → collect ALL CALL targets nearby
// 3. Best match = OpenPanel, second best = FindPanel
static bool FindOpenPanelFunction() {
    const char* probeStrings[] = {
        "InventoryEquipmentPanel",
        "QuestMenuPanel",
        "SkillTreePanel",
        "KnowledgePanel2",
    };
    const int NUM_PROBES = sizeof(probeStrings) / sizeof(probeStrings[0]);

    uintptr_t callSets[4][MAX_CALLS_PER_LEA * 10];
    int       callCounts[4] = {0};

    for (int p = 0; p < NUM_PROBES; p++) {
        uintptr_t strAddr = FindString(probeStrings[p]);
        if (!strAddr) {
            Log("WARNING: String '%s' not found", probeStrings[p]);
            continue;
        }
        Log("String '%s' at base+0x%llX", probeStrings[p],
            (unsigned long long)(strAddr - g_gameBase));

        uintptr_t leaAddr = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            leaAddr = FindLEA(strAddr, leaAddr ? leaAddr + 1 : 0);
            if (!leaAddr) break;

            Log("  LEA at base+0x%llX", (unsigned long long)(leaAddr - g_gameBase));

            uintptr_t targets[MAX_CALLS_PER_LEA];
            int n = FindAllCALLsAfter(leaAddr, 200, targets, MAX_CALLS_PER_LEA);

            for (int c = 0; c < n; c++) {
                if (callCounts[p] < MAX_CALLS_PER_LEA * 10) {
                    callSets[p][callCounts[p]++] = targets[c];
                    Log("    CALL -> base+0x%llX",
                        (unsigned long long)(targets[c] - g_gameBase));
                }
            }
        }
    }

    // Score all candidates from probe set 0
    uintptr_t bestTarget = 0, secondTarget = 0;
    int bestScore = 0, secondScore = 0;

    for (int i = 0; i < callCounts[0]; i++) {
        uintptr_t candidate = callSets[0][i];
        int score = 1;

        for (int p = 1; p < NUM_PROBES; p++) {
            for (int j = 0; j < callCounts[p]; j++) {
                if (callSets[p][j] == candidate) {
                    score++;
                    break;
                }
            }
        }

        if (score > bestScore) {
            secondScore = bestScore; secondTarget = bestTarget;
            bestScore = score; bestTarget = candidate;
        } else if (score > secondScore && candidate != bestTarget) {
            secondScore = score; secondTarget = candidate;
        }
    }

    if (bestScore < 2) {
        Log("ERROR: No common CALL target found (best score: %d)", bestScore);
        return false;
    }

    g_openPanelAddr = bestTarget;
    Log("OpenPanel found at base+0x%llX (matched %d/%d)",
        (unsigned long long)(g_openPanelAddr - g_gameBase), bestScore, NUM_PROBES);

    if (secondScore >= 2) {
        g_findPanelAddr = secondTarget;
        Log("FindPanel found at base+0x%llX (matched %d/%d)",
            (unsigned long long)(g_findPanelAddr - g_gameBase), secondScore, NUM_PROBES);
    }

    // Find R8 data pointer near one of the LEA sites
    uintptr_t strAddr = FindString("InventoryEquipmentPanel");
    uintptr_t leaAddr = FindLEA(strAddr);
    if (leaAddr) {
        BYTE* scan = (BYTE*)leaAddr;
        for (int j = -60; j < 80; j++) {
            BYTE* p = scan + j;
            if (p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x05) {
                int32_t disp = *(int32_t*)(p + 3);
                uintptr_t r8val = (uintptr_t)(p + 7) + disp;
                if (r8val > g_gameBase && r8val < g_gameBase + 0x10000000) {
                    g_r8DataAddr = r8val;
                    Log("R8 data at base+0x%llX",
                        (unsigned long long)(r8val - g_gameBase));
                    break;
                }
            }
        }
    }

    return true;
}

// ============================================================
//  PanelManager Global Pointer Chain Scanner
// ============================================================

// Scans callers of FindPanel for the pattern:
//   MOV RCX, [rip+disp32]   ; 48 8B 0D xx xx xx xx  (load global)
//   MOV reg, [RCX+off1]     ; 48 8B xx xx            (first offset)
//   MOV reg, [reg+off2]     ; 48 8B xx xx            (second offset)
//   MOV RCX, [RCX]          ; 48 8B 09               (final deref)
//   CALL FindPanel           ; E8 xx xx xx xx

// Scan for menu state flag offsets by finding two consecutive
// MOV BYTE PTR [reg+disp32], 0 instructions near a "LogoutView" reference.
// Known pattern: C6 xx CC_0C_00_00 00  C6 xx BC_0C_00_00 00
static bool FindMenuStateFlagOffsets() {
    uintptr_t strAddr = FindString("LogoutView");
    if (!strAddr) return false;

    BYTE* base = (BYTE*)g_gameBase;

    // Find all LEA references to "LogoutView" and check nearby code
    uintptr_t leaAddr = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        leaAddr = FindLEA(strAddr, leaAddr ? leaAddr + 1 : 0);
        if (!leaAddr) break;

        Log("FindMenuStateFlags: checking LEA at base+0x%llX",
            (unsigned long long)(leaAddr - g_gameBase));

        // Scan -80 to +80 bytes around the LEA for back-to-back MOV BYTE writes
        BYTE* scan = (BYTE*)leaAddr;
        for (int k = -80; k < 80; k++) {
            BYTE* p = scan + k;
            // Check: C6 [modrm] [disp32] 00 — 7 bytes, MOV BYTE [reg+disp32], 0
            if (p[0] != 0xC6) continue;
            if ((p[1] & 0xF8) != 0x80 || p[1] == 0x84) continue;
            int disp1 = *(int*)(p + 2);
            if (p[6] != 0x00) continue;
            if (disp1 < 0xC00 || disp1 > 0xD00) continue;

            // Check second MOV BYTE right after (7 bytes later)
            BYTE* p2 = p + 7;
            if (p2[0] != 0xC6) continue;
            if ((p2[1] & 0xF8) != 0x80 || p2[1] == 0x84) continue;
            int disp2 = *(int*)(p2 + 2);
            if (p2[6] != 0x00) continue;
            if (disp2 < 0xC00 || disp2 > 0xD00) continue;

            g_flagOffset1 = disp1;
            g_flagOffset2 = disp2;
            Log("FindMenuStateFlags: OK (0x%X, 0x%X) at LEA%+d",
                g_flagOffset1, g_flagOffset2, k);
            return true;
        }
    }

    Log("WARNING: Menu state flag offsets not found — toggle-close disabled");
    return false;
}

static bool FindPanelManagerChain() {
    if (g_findPanelAddr == 0) return false;

    BYTE* base = (BYTE*)g_gameBase;

    for (DWORD i = 5; i + 5 < g_imageSize; i++) {
        if (base[i] != 0xE8) continue;

        int32_t rel = *(int32_t*)(base + i + 1);
        uintptr_t target = g_gameBase + i + 5 + rel;
        if (target != g_findPanelAddr) continue;

        // Found a CALL to FindPanel at base+i
        // Look backwards (up to 30 bytes) for MOV RCX, [rip+disp32]: 48 8B 0D
        for (int j = (int)i - 30; j < (int)i - 6; j++) {
            if (j < 0) continue;
            if (base[j] != 0x48 || base[j+1] != 0x8B || base[j+2] != 0x0D)
                continue;

            int32_t disp = *(int32_t*)(base + j + 3);
            uintptr_t globalAddr = g_gameBase + j + 7 + disp;
            if (globalAddr < g_gameBase || globalAddr >= g_gameBase + g_imageSize)
                continue;

            // Extract disp8 offsets from subsequent MOV [reg+disp8] instructions
            int offsets[4];
            int nOffsets = 0;
            for (int k = j + 7; k < (int)i && k < j + 25 && nOffsets < 2; k++) {
                if (base[k] == 0x48 && base[k+1] == 0x8B) {
                    BYTE modrm = base[k+2];
                    int mod = (modrm >> 6) & 3;
                    if (mod == 1) { // [reg + disp8]
                        offsets[nOffsets++] = (int)(signed char)base[k+3];
                        k += 3;
                    }
                }
            }

            if (nOffsets < 2) continue;
            if (offsets[0] <= 0 || offsets[1] <= 0) continue;

            // Validate by trying the pointer chain
            __try {
                uintptr_t ptr = *(uintptr_t*)globalAddr;
                if (!ptr) continue;
                ptr = *(uintptr_t*)(ptr + offsets[0]);
                if (!ptr) continue;
                ptr = *(uintptr_t*)(ptr + offsets[1]);
                if (!ptr) continue;
                uintptr_t pm = *(uintptr_t*)ptr;

                // Store chain for later use (even if pm is 0 now)
                g_pmGlobalAddr = globalAddr;
                g_pmOffset1 = offsets[0];
                g_pmOffset2 = offsets[1];
                Log("PanelManager chain found: [base+0x%llX] -> +0x%X -> +0x%X -> deref",
                    (unsigned long long)(globalAddr - g_gameBase), offsets[0], offsets[1]);

                if (pm) {
                    g_panelManager = (LONGLONG)pm;
                    Log("PanelManager resolved immediately: 0x%llX", (unsigned long long)pm);
                }
                return true;

            } __except(EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
    }

    Log("WARNING: Could not find PanelManager global chain");
    return false;
}

// Re-read PanelManager from the known pointer chain
static bool ReadPanelManagerFromGlobal() {
    if (g_pmGlobalAddr == 0) return false;
    __try {
        uintptr_t ptr = *(uintptr_t*)g_pmGlobalAddr;
        if (!ptr) return false;
        ptr = *(uintptr_t*)(ptr + g_pmOffset1);
        if (!ptr) return false;
        ptr = *(uintptr_t*)(ptr + g_pmOffset2);
        if (!ptr) return false;
        ptr = *(uintptr_t*)ptr;
        if (ptr) {
            g_panelManager = (LONGLONG)ptr;
            return true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ============================================================
//  x64 Instruction Length Decoder
// ============================================================

static int DecodeModRM(BYTE* op, int baseLen) {
    BYTE modrm = op[0];
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    int len = baseLen;
    if (mod == 3) return len;
    if (rm == 4) len++;
    if (mod == 0 && rm == 5) len += 4;
    if (mod == 1) len += 1;
    if (mod == 2) len += 4;
    return len;
}

static int InstrLen(BYTE* ip) {
    bool hasRex = (ip[0] >= 0x40 && ip[0] <= 0x4F);
    BYTE* op = hasRex ? ip + 1 : ip;
    int base = hasRex ? 1 : 0;

    if (op[0] >= 0x50 && op[0] <= 0x5F) return base + 1;
    if (op[0] == 0x83) return DecodeModRM(op + 1, base + 2) + 1;
    if (op[0] == 0x81) return DecodeModRM(op + 1, base + 2) + 4;

    if (op[0] == 0x88 || op[0] == 0x89 || op[0] == 0x8A ||
        op[0] == 0x8B || op[0] == 0x8D || op[0] == 0x3B ||
        op[0] == 0x3A || op[0] == 0x2B || op[0] == 0x2A ||
        op[0] == 0x03 || op[0] == 0x01) {
        return DecodeModRM(op + 1, base + 2);
    }

    if (op[0] == 0x33 || op[0] == 0x85 || op[0] == 0x31 ||
        op[0] == 0x29 || op[0] == 0x39) {
        return DecodeModRM(op + 1, base + 2);
    }

    if (op[0] == 0xC7) return DecodeModRM(op + 1, base + 2) + 4;
    if (op[0] == 0xC6) return DecodeModRM(op + 1, base + 2) + 1;
    if (op[0] == 0xF7 && ((op[1] >> 3) & 7) == 0) return DecodeModRM(op + 1, base + 2) + 4;
    if (op[0] == 0xF6 && ((op[1] >> 3) & 7) == 0) return DecodeModRM(op + 1, base + 2) + 1;
    if (op[0] == 0xFF) return DecodeModRM(op + 1, base + 2);

    if (op[0] >= 0xB8 && op[0] <= 0xBF) {
        return base + 1 + (hasRex && (ip[0] & 0x08) ? 8 : 4);
    }
    if (op[0] >= 0xB0 && op[0] <= 0xB7) return base + 2;
    if (op[0] == 0x90) return base + 1;
    if (op[0] == 0xC3) return base + 1;
    if (op[0] == 0xE8 || op[0] == 0xE9) return base + 5;
    if (op[0] >= 0x70 && op[0] <= 0x7F) return base + 2;
    if (op[0] == 0xEB) return base + 2;

    if (op[0] == 0x0F) {
        BYTE op2 = op[1];
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF)
            return DecodeModRM(op + 2, base + 3);
        if (op2 == 0x1F)
            return DecodeModRM(op + 2, base + 3);
        if (op2 >= 0x80 && op2 <= 0x8F) return base + 6;
        if (op2 >= 0x40 && op2 <= 0x4F)
            return DecodeModRM(op + 2, base + 3);
    }

    return 0;
}

// ============================================================
//  Hook Installation
// ============================================================

// C detour for OpenPanel — blocks game-triggered opens for remapped panels
static void __fastcall DetourOpenPanel(LONGLONG pm, const char* panelName, void* data) {
    // Capture PanelManager + panel name (same as old assembly trampoline)
    g_panelManager = pm;
    g_lastRDX = (LONGLONG)panelName;
    InterlockedIncrement(&g_hookCounter);

    // Block game-triggered opens ONLY when the user is pressing the game's default key
    // (or a modifier combo on that key). This prevents the game's keybind from interfering
    // while still allowing internal game UI calls (settlement, NPCs, etc.) to work normally.
    if (!g_ourCall && g_ready && g_overrideGameKeys) {
        __try {
            for (int i = 0; i < NUM_PANELS; i++) {
                if (g_panels[i].gameDefaultKey == 0) continue;
                if (strcmp(panelName, g_panels[i].panelName) != 0) continue;

                // Only block if the game's default key is physically pressed right now
                if (GetAsyncKeyState(g_panels[i].gameDefaultKey) & 0x8000) {
                    Log("[Blocked] Game tried to open '%s' (key 0x%02X pressed)",
                        panelName, g_panels[i].gameDefaultKey);
                    return;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Forward to original function (SEH protected — disables mod on crash)
    typedef void (__fastcall* OrigFunc)(LONGLONG, const char*, void*);
    __try {
        ((OrigFunc)g_openPanelTrampoline)(pm, panelName, data);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ready = false;
        Log("FATAL: Trampoline crashed (0x%08X) — mod disabled to prevent further crashes",
            GetExceptionCode());
    }
}

static bool InstallHook(uintptr_t targetAddr) {
    BYTE* target = (BYTE*)targetAddr;
    BYTE stolenBytes[32];

    int stolenLen = 0;
    while (stolenLen < 14) {
        int len = InstrLen(target + stolenLen);
        if (len == 0) {
            Log("ERROR: Unknown instruction at +%d: %02X %02X %02X %02X",
                stolenLen, target[stolenLen], target[stolenLen+1],
                target[stolenLen+2], target[stolenLen+3]);
            return false;
        }
        stolenLen += len;
    }
    memcpy(stolenBytes, target, stolenLen);

    // Safety: reject stolen bytes with RIP-relative addressing — they would
    // reference wrong memory when relocated to the trampoline.
    // Generic check: any instruction with a ModRM byte where mod=00, r/m=101
    // uses RIP-relative addressing in x64 mode.
    for (int j = 0; j < stolenLen; ) {
        BYTE* ip = stolenBytes + j;
        bool hasRex = (ip[0] >= 0x40 && ip[0] <= 0x4F);
        BYTE* op = hasRex ? ip + 1 : ip;
        int modrmOffset = 1;  // ModRM byte position after opcode

        bool hasModRM = false;
        if (op[0] == 0x0F) {
            // Two-byte opcode — ModRM follows second byte
            modrmOffset = 2;
            BYTE op2 = op[1];
            if ((op2 >= 0x10 && op2 <= 0x1F) || (op2 >= 0x28 && op2 <= 0x2F) ||
                (op2 >= 0x40 && op2 <= 0x4F) || (op2 >= 0x80 && op2 <= 0x8F) ||
                (op2 >= 0xB0 && op2 <= 0xBF) || op2 == 0xAF || op2 == 0xA3 ||
                op2 == 0xA5 || op2 == 0xAB || op2 == 0xAD)
                hasModRM = true;
        } else {
            // Single-byte opcodes with ModRM (common ranges)
            BYTE b = op[0];
            if ((b >= 0x00 && b <= 0x03) || (b >= 0x08 && b <= 0x0B) ||
                (b >= 0x10 && b <= 0x13) || (b >= 0x18 && b <= 0x1B) ||
                (b >= 0x20 && b <= 0x23) || (b >= 0x28 && b <= 0x2B) ||
                (b >= 0x30 && b <= 0x33) || (b >= 0x38 && b <= 0x3B) ||
                b == 0x63 || (b >= 0x80 && b <= 0x8D) ||
                b == 0x8F || b == 0xC6 || b == 0xC7 ||
                b == 0xF6 || b == 0xF7 || b == 0xFE || b == 0xFF ||
                b == 0x69 || b == 0x6B || b == 0x85)
                hasModRM = true;
        }

        if (hasModRM && (j + modrmOffset) < stolenLen) {
            BYTE modrm = op[modrmOffset];
            if ((modrm & 0xC7) == 0x05) {
                Log("ERROR: RIP-relative instruction in stolen bytes at +%d (op=%02X) — hook aborted",
                    j, op[0]);
                return false;
            }
        }

        int len = InstrLen(target + j);
        if (len == 0) break;
        j += len;
    }

    Log("Stealing %d bytes from base+0x%llX",
        stolenLen, (unsigned long long)(targetAddr - g_gameBase));

    // --- Trampoline: stolen bytes + JMP back to original ---
    void* trampMem = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (!trampMem) {
        Log("ERROR: VirtualAlloc (trampoline) failed!");
        return false;
    }

    BYTE* p = (BYTE*)trampMem;
    memcpy(p, stolenBytes, stolenLen);
    p += stolenLen;
    *p++ = 0xFF; *p++ = 0x25;
    *(uint32_t*)p = 0; p += 4;
    *(uintptr_t*)p = targetAddr + stolenLen;

    g_openPanelTrampoline = (uintptr_t)trampMem;

    // Lock trampoline to execute-only (no longer needs write access)
    DWORD trampOldProt;
    VirtualProtect(trampMem, 64, PAGE_EXECUTE_READ, &trampOldProt);

    // --- Patch original: JMP to DetourOpenPanel ---
    DWORD oldProt;
    VirtualProtect((void*)targetAddr, stolenLen, PAGE_EXECUTE_READWRITE, &oldProt);

    BYTE* h = (BYTE*)targetAddr;
    *h++ = 0xFF; *h++ = 0x25;
    *(uint32_t*)h = 0; h += 4;
    *(uintptr_t*)h = (uintptr_t)DetourOpenPanel; h += 8;
    while (h < target + stolenLen) *h++ = 0x90;

    VirtualProtect((void*)targetAddr, stolenLen, oldProt, &oldProt);
    Log("Hook installed at base+0x%llX -> DetourOpenPanel, trampoline at 0x%p",
        (unsigned long long)(targetAddr - g_gameBase), trampMem);
    return true;
}

// ============================================================
//  Open Panel
// ============================================================

static void OpenPanelOnGameThread(int panelIndex) {
    if (panelIndex < 0 || panelIndex >= NUM_PANELS) return;

    // Always refresh PanelManager from global chain before use
    // (protects against stale pointer after UI rebuild / scene change)
    if (g_pmGlobalAddr != 0)
        ReadPanelManagerFromGlobal();

    LONGLONG pm = g_panelManager;
    if (pm == 0) {
        Log("ERROR: PanelManager not available!");
        return;
    }

    if (g_openPanelAddr == 0) {
        Log("ERROR: OpenPanel function address not resolved!");
        return;
    }

    typedef void (__fastcall* OpenPanelFunc)(LONGLONG, const char*, void*);
    OpenPanelFunc openPanel = (OpenPanelFunc)g_openPanelAddr;
    // Pass nullptr — OpenPanel looks up the default execute-event per panel name
    // (verified in Ghidra: param_3==NULL → FUN_140aa5010 resolves default)
    void* dataPtr = nullptr;

    const char* panelName = g_panels[panelIndex].panelName;
    Log("Opening panel: %s (index %d)", panelName, panelIndex);

    __try {
        g_ourCall = true;
        openPanel(pm, panelName, dataPtr);
        g_ourCall = false;
        g_currentPanel = panelIndex;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ourCall = false;
        Log("ERROR: Panel call crashed (0x%08X) for '%s'",
            GetExceptionCode(), panelName);
        g_panelManager = 0;
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
    }
}

// ============================================================
//  WndProc Handler
// ============================================================

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    if (msg == WM_OPEN_PANEL) {
        OpenPanelOnGameThread((int)wParam);
        return 0;
    }
    if (msg == WM_CLEAR_FLAGS) {
        ClearMenuStateFlags();
        return 0;
    }
    // Reset panel tracking when user closes a menu with ESC
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
    }
    // Parse PS controller HID reports (don't consume — let game process it too)
    if (msg == WM_INPUT) {
        ParseSonyHidReport(lParam);
    }
    return CallWindowProcA(g_originalWndProc, hwnd, msg, wParam, lParam);
}

// ============================================================
//  Panel Name Logger Thread
// ============================================================

static DWORD WINAPI NameLogThread(LPVOID) {
    LONG lastCounter = 0;
    while (true) {
        Sleep(50);
        LONG cur = g_hookCounter;
        if (cur != lastCounter) {
            lastCounter = cur;
            LONGLONG rdx = g_lastRDX;
            if (rdx != 0) {
                __try {
                    const char* name = (const char*)(uintptr_t)rdx;
                    if (name[0] >= 0x20 && name[0] < 0x7F) {
                        int idx = (g_nameLogIndex++) % NAME_LOG_SIZE;
                        strncpy(g_nameLog[idx], name, 127);
                        g_nameLog[idx][127] = '\0';
                        Log("[Hook] Panel name: \"%s\" (call #%d)",
                            g_nameLog[idx], (int)cur);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }
    return 0;
}

// ============================================================
//  Input Thread
// ============================================================

static bool g_keyWasDown[32] = {};  // track previous key state per panel
static bool g_reloadWasDown = false;
static DWORD g_lastActionTime = 0;  // cooldown between panel switches
static const DWORD PANEL_COOLDOWN_MS = 400;  // 400ms between actions
static char g_iniPath[MAX_PATH] = {};

// Debounce: delay plain key when modifier bindings exist on the same key
static int   g_pendingPanel = -1;
static DWORD g_pendingTime = 0;
static const DWORD MODIFIER_DEBOUNCE_MS = 60;

// Check if any panel has a modifier binding on the given key
static bool HasModifierBindings(DWORD key) {
    for (int i = 0; i < NUM_PANELS; i++) {
        if (g_panels[i].key == key && g_panels[i].modifierKey != 0)
            return true;
    }
    return false;
}

// Check if g_currentPanel is a modifier-bound panel on the given base key.
// When true, pressing the plain key is unambiguous (user wants to switch away
// from the modifier panel), so debounce can be skipped.
static bool IsModifierPanelActiveOnKey(DWORD key) {
    if (g_currentPanel < 0 || g_currentPanel >= NUM_PANELS) return false;
    return g_panels[g_currentPanel].key == key &&
           g_panels[g_currentPanel].modifierKey != 0;
}

// Helper: handle panel action (toggle close or open) — shared by keyboard and controller
static bool HandlePanelAction(int i) {
    DWORD now = GetTickCount();
    if (now - g_lastActionTime < PANEL_COOLDOWN_MS) return false;

    if (!IsGameplayState()) return false;

    g_lastActionTime = now;

    if (g_panelManager == 0) {
        Log("ERROR: PanelManager not available yet.");
        return false;
    }

    // Verify panel is actually still open before toggle-close.
    // flag1 (mc+0xCC5) is non-zero when a panel is open, 0 when closed.
    // If flag1 is 0 but we think a panel is open → game closed it → open fresh.
    if (g_currentPanel == i && !IsPanelFlagSet()) {
        Log("Panel '%s' was already closed by game (flag1=0), opening fresh",
            g_panels[i].panelName);
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
    }

    if (g_currentPanel == i) {
        Log("Closing panel: %s (toggle)", g_panels[i].panelName);
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
        PostMessageA(g_gameWindow, WM_CLEAR_FLAGS, 0, 0);
    } else {
        PostMessageA(g_gameWindow, WM_OPEN_PANEL, i, 0);
    }
    return true;
}

static DWORD WINAPI InputThread(LPVOID) {
    while (true) {
        Sleep(16);
        if (!g_enabled || !g_ready || !g_gameWindow) continue;
        if (GetForegroundWindow() != g_gameWindow) continue;

        // Keep PanelManager fresh from global chain
        if (g_panelManager == 0 && g_pmGlobalAddr != 0)
            ReadPanelManagerFromGlobal();

        // Panel close detection is now handled in HandlePanelAction via
        // IsAnyPanelStateActive() — checks the full state-flag array (mc+0xCB8,
        // 16 slots) instead of only flag1 (mc+0xCC5 = slot 0x0D).

        // --- INI Reload hotkey ---
        if (g_reloadKey != 0) {
            bool rkDown = (GetAsyncKeyState(g_reloadKey) & 0x8000) != 0;
            if (rkDown && !g_reloadWasDown) {
                g_reloadWasDown = true;
                LoadConfig(g_iniPath);
                Log("--- INI reloaded (0x%02X pressed) ---", g_reloadKey);
                for (int i = 0; i < NUM_PANELS; i++) {
                    if (g_panels[i].key != 0x00)
                        Log("  [%s] Hotkey=0x%02X Mod=0x%02X Ctrl=0x%04X",
                            g_panels[i].section, g_panels[i].key,
                            g_panels[i].modifierKey, g_panels[i].controllerButton);
                }
            } else if (!rkDown) {
                g_reloadWasDown = false;
            }
        }

        // --- Keyboard polling ---
        // Pass 1: panels WITH modifier (always runs, even during debounce)
        bool actionTaken = false;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].key == 0x00 || g_panels[i].modifierKey == 0) continue;

            bool keyDown = (GetAsyncKeyState(g_panels[i].key) & 0x8000) != 0;
            bool modDown = (GetAsyncKeyState(g_panels[i].modifierKey) & 0x8000) != 0;
            bool isDown = keyDown && modDown;

            if (isDown && !g_keyWasDown[i]) {
                g_keyWasDown[i] = true;
                // Cancel any pending plain key on the same base key
                if (g_pendingPanel >= 0 && g_panels[g_pendingPanel].key == g_panels[i].key) {
                    Log("Debounce: cancelled plain '%s', modifier combo wins",
                        g_panels[g_pendingPanel].panelName);
                    g_pendingPanel = -1;
                }
                // Also mark plain-key panels on the same key as "seen"
                // so they don't trigger when modifier is released
                for (int k = 0; k < NUM_PANELS; k++) {
                    if (g_panels[k].key == g_panels[i].key && g_panels[k].modifierKey == 0)
                        g_keyWasDown[k] = true;
                }
                HandlePanelAction(i);
                actionTaken = true;
                break;
            } else if (!isDown) {
                g_keyWasDown[i] = false;
            }
        }

        // Debounce: fire pending plain key after delay
        if (!actionTaken && g_pendingPanel >= 0) {
            if (GetTickCount() - g_pendingTime >= MODIFIER_DEBOUNCE_MS) {
                HandlePanelAction(g_pendingPanel);
                g_pendingPanel = -1;
            }
            actionTaken = true;  // block further input while debouncing
        }

        // Pass 2: panels WITHOUT modifier
        if (!actionTaken) {
            for (int i = 0; i < NUM_PANELS; i++) {
                if (g_panels[i].key == 0x00 || g_panels[i].modifierKey != 0) continue;

                bool isDown = (GetAsyncKeyState(g_panels[i].key) & 0x8000) != 0;

                // Guard: skip if ANY modifier for the same key is currently held
                if (isDown) {
                    for (int k = 0; k < NUM_PANELS; k++) {
                        if (g_panels[k].key == g_panels[i].key &&
                            g_panels[k].modifierKey != 0 &&
                            (GetAsyncKeyState(g_panels[k].modifierKey) & 0x8000)) {
                            isDown = false;
                            break;
                        }
                    }
                }
                // Guard: skip if any global modifier is held but panel has no modifier
                // Prevents conflict with other mods using Shift+I, Ctrl+I etc.
                if (isDown && g_panels[i].modifierKey == 0) {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                        (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                        (GetAsyncKeyState(VK_MENU) & 0x8000)) {
                        isDown = false;
                    }
                }

                if (isDown && !g_keyWasDown[i]) {
                    g_keyWasDown[i] = true;
                    if (HasModifierBindings(g_panels[i].key) && !IsModifierPanelActiveOnKey(g_panels[i].key)) {
                        // Delay: give modifier time to register
                        g_pendingPanel = i;
                        g_pendingTime = GetTickCount();
                    } else {
                        HandlePanelAction(i);
                    }
                    actionTaken = true;
                    break;
                } else if (!isDown) {
                    g_keyWasDown[i] = false;
                }
            }
        }

        // --- Controller polling ---
        if (!actionTaken && g_controllerEnabled && g_pXInputGetState) {
            XINPUT_STATE_LOCAL state;
            memset(&state, 0, sizeof(state));
            if (g_pXInputGetState(0, &state) == 0) {
                WORD buttons = state.Gamepad.wButtons;
                WORD pressed = buttons & ~g_prevButtons;
                g_prevButtons = buttons;

                // Modifier check: must be held if configured
                if (g_controllerModifier != 0) {
                    if (!(buttons & g_controllerModifier)) pressed = 0;
                    else pressed &= ~g_controllerModifier;
                }

                if (pressed != 0) {
                    for (int i = 0; i < NUM_PANELS; i++) {
                        if (g_panels[i].controllerButton == 0) continue;
                        if (!(pressed & g_panels[i].controllerButton)) continue;
                        HandlePanelAction(i);
                        actionTaken = true;
                        break;
                    }
                }
            }
        }

        // --- PS Controller polling (HID Raw Input) ---
        if (!actionTaken && g_controllerEnabled && g_hidConnected) {
            static bool g_psWasDown[32] = {};

            LONG packed = InterlockedCompareExchange(&g_hidButtonsPacked, 0, 0);
            BYTE hb[4] = {
                (BYTE)(packed & 0xFF),
                (BYTE)((packed >> 8) & 0xFF),
                (BYTE)((packed >> 16) & 0xFF),
                (BYTE)((packed >> 24) & 0xFF)
            };

            // Modifier check: must be held if configured
            bool psModOk = true;
            if (g_psModifierEnabled) {
                psModOk = (hb[g_psModifierByteOff] & g_psModifierBitMask) != 0;
            }

            if (psModOk) {
                for (int i = 0; i < NUM_PANELS; i++) {
                    if (g_panels[i].psButtonByteOff < 0) continue;
                    bool isDown = (hb[g_panels[i].psButtonByteOff] & g_panels[i].psButtonBitMask) != 0;

                    if (isDown && !g_psWasDown[i]) {
                        g_psWasDown[i] = true;
                        HandlePanelAction(i);
                        break;
                    } else if (!isDown) {
                        g_psWasDown[i] = false;
                    }
                }
            } else {
                // Modifier not held — reset all edge states
                for (int i = 0; i < NUM_PANELS; i++)
                    g_psWasDown[i] = false;
            }
        }
    }
    return 0;
}

// ============================================================
//  Find Game Window
// ============================================================

static HWND FindGameWindow(){
    DWORD myPid=GetCurrentProcessId();
    HWND h=nullptr;
    while((h=FindWindowExW(nullptr,h,L"WindowsLauncherClassName",L"Crimson Desert"))!=nullptr){
        DWORD pid=0;GetWindowThreadProcessId(h,&pid);
        if(pid==myPid)return h;
    }
    return nullptr;
}

// ============================================================
//  Mod Thread
// ============================================================

static DWORD WINAPI ModThread(LPVOID) {
    for (int i = 0; i < 120; i++) {
        Sleep(1000);
        g_gameWindow = FindGameWindow();
        if (g_gameWindow) break;
    }
    if (!g_gameWindow) return 0;
    {char cls[256]={};char ttl[256]={};GetClassNameA(g_gameWindow,cls,256);GetWindowTextA(g_gameWindow,ttl,256);
    Log("Game window: class='%s' title='%s'",cls,ttl);}

    Sleep(10000);

    char dllPath[MAX_PATH];
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
    std::string iniPath(dllPath);
    size_t dot = iniPath.rfind('.');
    if (dot != std::string::npos) iniPath = iniPath.substr(0, dot);
    iniPath += ".ini";

    LoadConfig(iniPath.c_str());
    strncpy(g_iniPath, iniPath.c_str(), MAX_PATH - 1);
    if (!g_enabled) return 0;

    if (g_debugLog) {
        std::string logPath = iniPath.substr(0, iniPath.rfind('.')) + ".log";
        g_logFile = fopen(logPath.c_str(), "w");
    }

    if (g_controllerEnabled) InitXInput();

    Log("=== Quick Menu Hotkeys v1.10 ===");

    g_gameBase = (uintptr_t)GetModuleHandleA("CrimsonDesert.exe");
    if (!g_gameBase) { Log("ERROR: CrimsonDesert.exe not found"); return 0; }

    MODULEINFO mi;
    GetModuleInformation(GetCurrentProcess(), (HMODULE)g_gameBase, &mi, sizeof(mi));
    g_imageSize = mi.SizeOfImage;

    Log("Game base: 0x%llX  Size: 0x%X", (unsigned long long)g_gameBase, g_imageSize);

    // Hash meta/0.papgt to detect modded game files (JSON mods etc.)
    {
        std::string metaPath(dllPath);
        size_t bs = metaPath.rfind('\\');
        if (bs != std::string::npos) {
            metaPath = metaPath.substr(0, bs);           // strip filename
            bs = metaPath.rfind('\\');
            if (bs != std::string::npos)
                metaPath = metaPath.substr(0, bs);       // strip bin64
        }
        metaPath += "\\meta\\0.papgt";
        uint32_t crc = FileCRC32(metaPath.c_str());
        if (crc) Log("meta/0.papgt CRC32: %08X", crc);
        else     Log("meta/0.papgt: NOT FOUND");
    }

    // Pattern scan: find OpenPanel + FindPanel
    if (!FindOpenPanelFunction()) {
        Log("ERROR: Pattern scan failed!");
        return 0;
    }

    // Install hook on OpenPanel (fallback PanelManager capture)
    if (!InstallHook(g_openPanelAddr)) {
        Log("ERROR: Hook installation failed!");
        return 0;
    }

    // Find menu state flag offsets (for toggle-close)
    FindMenuStateFlagOffsets();

    // Find PanelManager global pointer chain (proactive resolution)
    FindPanelManagerChain();

    // If chain found but PM not yet available, retry until gameplay starts
    if (g_pmGlobalAddr != 0 && g_panelManager == 0) {
        Log("Waiting for PanelManager from global chain...");
        for (int retry = 0; retry < 60 && g_panelManager == 0; retry++) {
            Sleep(1000);
            ReadPanelManagerFromGlobal();
        }
        if (g_panelManager != 0)
            Log("PanelManager resolved from global: 0x%llX",
                (unsigned long long)g_panelManager);
    }

    // Re-validate window handle before subclassing
    if(!IsWindow(g_gameWindow)){
        g_gameWindow=FindGameWindow();
        if(!g_gameWindow||!IsWindow(g_gameWindow)){Log("FATAL: game window invalid before WndProc hook");return 0;}
    }

    // Window subclassing (retry up to 5 times — some overlays/mods delay window init)
    for (int retry = 0; retry < 5; retry++) {
        SendMessageTimeoutA(g_gameWindow, WM_NULL, 0, 0, SMTO_BLOCK, 1000, nullptr);
        g_originalWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWindow, GWLP_WNDPROC,
                                                         (LONG_PTR)HookedWndProc);
        if (g_originalWndProc) {
            if (retry > 0) Log("Window subclassing succeeded on retry %d", retry);
            break;
        }
        Log("WARNING: Window subclassing attempt %d failed, retrying...", retry + 1);
        Sleep(2000);
    }
    if (!g_originalWndProc) {
        Log("ERROR: Window subclassing failed after 5 attempts — disabling OverrideGameKeys");
        g_overrideGameKeys = false;
        return 0;
    }

    // Register for HID Gamepad raw input (PS5/PS4 native support)
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage     = 0x05;  // Game Pad
    rid[0].dwFlags     = RIDEV_INPUTSINK;
    rid[0].hwndTarget  = g_gameWindow;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x04;  // Joystick
    rid[1].dwFlags     = RIDEV_INPUTSINK;
    rid[1].hwndTarget  = g_gameWindow;
    if (RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
        Log("Raw Input: registered for HID GamePad + Joystick");
    else
        Log("Raw Input: RegisterRawInputDevices FAILED (error=%lu)", GetLastError());

    g_ready = true;
    CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
    if (g_debugLog)
        CreateThread(nullptr, 0, NameLogThread, nullptr, 0, nullptr);

    Log("--- Configured Keybinds ---");
    for (int i = 0; i < NUM_PANELS; i++) {
        if (g_panels[i].key != 0x00) {
            Log("  [%s] %s = 0x%02X", g_panels[i].section,
                g_panels[i].panelName, g_panels[i].key);
        }
    }
    Log("OverrideGameKeys: %s", g_overrideGameKeys ? "ON" : "OFF");
    if (g_controllerEnabled && g_pXInputGetState) {
        Log("--- Controller Bindings ---");
        if (g_controllerModifier != 0) {
            Log("  Modifier: 0x%04X", g_controllerModifier);
            Log("  NOTE: If the modifier button has a game function (e.g. LB=block),");
            Log("        it will trigger BOTH the game action and the mod hotkey.");
            Log("        Use an unused button as modifier to avoid conflicts.");
        }
        bool hasAnyButton = false;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].controllerButton != 0) { hasAnyButton = true; break; }
        }
        if (!hasAnyButton) {
            Log("  WARNING: No ControllerButton assigned to any panel!");
            Log("           Add e.g. ControllerButton=0001 (D-Pad Up) to a panel section.");
        }
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].controllerButton != 0)
                Log("  [%s] %s = 0x%04X", g_panels[i].section,
                    g_panels[i].panelName, g_panels[i].controllerButton);
        }
    }
    // PS Controller binding summary
    {
        bool hasAnyPS = false;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].psButtonByteOff >= 0) { hasAnyPS = true; break; }
        }
        if (hasAnyPS) {
            Log("--- PS Controller Bindings (native HID) ---");
            if (g_psModifierEnabled)
                Log("  PSModifier: byteOff=%d mask=0x%02X", g_psModifierByteOff, g_psModifierBitMask);
            for (int i = 0; i < NUM_PANELS; i++) {
                if (g_panels[i].psButtonByteOff >= 0)
                    Log("  [%s] PSButton: byteOff=%d mask=0x%02X",
                        g_panels[i].section, g_panels[i].psButtonByteOff, g_panels[i].psButtonBitMask);
            }
        }
    }
    Log("--- Mod ready! PanelManager: %s ---",
        g_panelManager ? "AVAILABLE" : "waiting for first panel interaction");

    return 0;
}

// ============================================================
//  DLL Entry Point
// ============================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_gameWindow && g_originalWndProc)
            SetWindowLongPtrA(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        if (g_hXInput) { FreeLibrary(g_hXInput); g_hXInput = nullptr; }
        if (g_logFile) { Log("=== Mod unloaded ==="); fclose(g_logFile); }
    }
    return TRUE;
}
