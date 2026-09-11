#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <string>

// ============================================================
//  Quick Menu Hotkeys
//
//  Opens the game's menu panels directly from a hotkey or controller
//  button. OpenPanel, FindPanelTop and the game's own menu-close call are
//  located by cross-referencing panel-name strings at startup, so no
//  fixed addresses are carried between game updates.
//
//  See CHANGELOG.txt for version history.
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
static uintptr_t g_findPanelTopAddr = 0;  // top-level panel lookup (pm[0x30ec8] array iterator)
static uintptr_t g_findPanelTopTramp = 0; // trampoline for the pm-seeding hook (see DetourFindPanelTop)
static uint32_t  g_pmArrayOff = 0;        // pm+N: panel entry array   (read out of FindPanelTop's prolog)
static uint32_t  g_pmCountOff = 0;        // pm+N: panel entry count
static uintptr_t g_setSubTabAddr    = 0;  // sub-tab switcher
static uintptr_t g_r8DataAddr     = 0;

// PanelManager global pointer chain (for proactive resolution)
static uintptr_t g_pmGlobalAddr = 0;
static int g_pmOffset1 = 0;
static int g_pmOffset2 = 0;

// PanelManager-chain state. The chain is derived statically from the binary at
// boot (FindPmChainStatic); if that fails it is learned from the first live pm
// (LearnPmChainCandidate/ValidatePmChain). Either way it stays live-validated
// and nothing is persisted — each launch re-derives it from the current binary,
// so a game update that shifts the offsets is handled automatically.
static int  g_pmChainCandA   = -1;
static int  g_pmChainCandB   = -1;
static int  g_pmChainAgree   = 0;
static int  g_pmChainZeroReads = 0;    // consecutive null reads on an unproven chain
static bool g_pmChainLearned = false;  // g_pmOffset1/2 came from static scan or learner

static char g_iniPath[MAX_PATH] = {};

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
#define WM_CLOSE_PANEL (WM_USER + 502)

// ============================================================
//  Config
// ============================================================

struct PanelBinding {
    const char* section;
    const char* defaultPanelName;  // built-in default
    char        customPanelName[64]; // optional INI override (empty = use default)
    const char* panelName;         // resolved at LoadConfig (-> custom or default)
    // Optional execute-event string passed as OpenPanel's 3rd parameter.
    // Selects a sub-tab inside the panel (e.g. PetView accepts "" for the
    // Pets tab and "SpecialVehicle" for the Special-Mount tab).
    //   hasExecuteEvent=false -> pass nullptr (game uses panel default,
    //                            which post May-11 = SpecialVehicle for PetView)
    //   hasExecuteEvent=true  -> pass executeEvent (may be "" / "Skill" / etc.)
    bool        defaultHasExecuteEvent;
    const char* defaultExecuteEvent;
    bool        hasExecuteEvent;
    char        executeEvent[64];
    DWORD       defaultKey;
    DWORD       gameDefaultKey;  // Game-internal hardcoded key (0 = none)
    DWORD       key;
    DWORD       modifierKey;       // Keyboard modifier (Shift/Ctrl/Alt), 0 = none
    WORD        controllerButton;  // XInput button bitmask (0 = disabled)
    int         psButtonByteOff;   // HID button byte offset (0-3), -1 = disabled
    BYTE        psButtonBitMask;   // HID button bit mask
};

// Verified panels — default keybinds for core menus, 0x00 = user-configurable.
// "PetView" still exists in the May-11 2026 build (verified in Ghidra after a
// full background analysis — initial list_strings missed it). The pet panel
// is opened via OpenPanel("PetView", ...) just like before.
// PetView default execute-event "" hits the Pets sub-tab (per
// mainmenuview2.html: SubTabbarHeader7 open-panel="PetView" execute-event="").
// Without it the game treats the call as default-event = "SpecialVehicle"
// (Special Mount sub-tab) — verified live by user.
static PanelBinding g_panels[] = {
    // section,         defaultPanelName,          customPanelName, panelName,                  defaultHasExecuteEvent, defaultExecuteEvent, hasExecuteEvent, executeEvent, defaultKey, gameDefaultKey, key, modifierKey, controllerButton, psByteOff, psBitMask
    // --- Core panels (default keybinds) ---
    { "Inventory",     "InventoryEquipmentPanel", "", "InventoryEquipmentPanel", false, nullptr, false, "", 0x49, 0x49, 0, 0, 0, -1, 0 },  // I
    { "QuestBook",     "QuestMenuPanel",          "", "QuestMenuPanel",          false, nullptr, false, "", 0x4A, 0x4A, 0, 0, 0, -1, 0 },  // J
    { "SkillBook",     "SkillTreePanel",          "", "SkillTreePanel",          false, nullptr, false, "", 0x4B, 0x4B, 0, 0, 0, -1, 0 },  // K
    { "Knowledge",     "KnowledgePanel2",         "", "KnowledgePanel2",         false, nullptr, false, "", 0x4C, 0x00, 0, 0, 0, -1, 0 },  // L
    { "Options",       "LogoutView",              "", "LogoutView",              false, nullptr, false, "", 0x4F, 0x00, 0, 0, 0, -1, 0 },  // O
    { "Map",           "WorldMapView",            "", "WorldMapView",            false, nullptr, false, "", 0x4D, 0x4D, 0, 0, 0, -1, 0 },  // M
    // --- Extra panels (no default keybind — configure in INI) ---
    { "Challenge",     "ChallengeMenuPanel2",     "", "ChallengeMenuPanel2",     false, nullptr, false, "", 0x00, 0x00, 0, 0, 0, -1, 0 },
    { "FactionQuest",  "FactionQuestMenuPanel",   "", "FactionQuestMenuPanel",   false, nullptr, false, "", 0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Guides",        "PlayGuideView",           "", "PlayGuideView",           false, nullptr, false, "", 0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Notifications", "AlertHistoryView",        "", "AlertHistoryView",        false, nullptr, false, "", 0x00, 0x00, 0, 0, 0, -1, 0 },
    { "Pet",           "PetView",                 "", "PetView",                 true,  "",      true,  "", 0x00, 0x00, 0, 0, 0, -1, 0 },
};

static const int NUM_PANELS = sizeof(g_panels) / sizeof(g_panels[0]);

static bool g_enabled  = true;
static bool g_debugLog = true;
static bool g_overrideGameKeys = true;
static DWORD g_reloadKey = 0;

// Text-modal gate. Set ModalGate=0 in INI to bypass entirely (escape hatch).
static bool g_modalGate = true;

// Modal hook state. Set when the TextEditModalMessage open function fires.
// Cleared by either a polled liveness check (mouse-close path) or an ESC/
// Enter key edge (keyboard-close path, fastest reaction).
static volatile bool      g_textModalActive = false;
static volatile uintptr_t g_modalObj        = 0;  // Modal object captured from hook
static volatile uintptr_t g_modalVftable    = 0;  // vftable at hook time (liveness)
static uintptr_t g_modalTrampoline = 0;

// OpenPanel detour: trampoline to original function
static uintptr_t g_openPanelTrampoline = 0;
static volatile bool g_ourCall = false;
static volatile bool g_ourCallCrashed = false;  // set by the detour's SEH when
                                                // a mod-initiated open crashed

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

// --- Input-block hooks (XInputGetState + GetRawInputData) ---
// When a controller modifier is configured AND held, the corresponding panel
// buttons are masked out of the data the GAME sees, so the same press doesn't
// trigger both the panel hotkey and the default in-game action.
static bool g_blockOverlappingInputs = true;

// The game's own menu-close call, recovered from its own toggle code.
// See ResolveMenuClose() for the signature this is derived from.
static uintptr_t g_menuCloseFn    = 0;   // f(menuObj, 0, 0)
static uintptr_t g_menuRootGlobal = 0;   // menuObj = *( *(global) + g_menuObjOff )
static uint32_t  g_menuObjOff     = 0;
static uint32_t  g_panelStateOff  = 0;   // panelObj+N: view state byte
static uintptr_t g_questMenuFlag  = 0;   // game byte: 0 = old quest journal (QuestMenuPanel), else QuestMenuPanel2
static uintptr_t g_xinputTrampoline = 0;
static uintptr_t g_griTrampoline    = 0;

typedef UINT (WINAPI *PFN_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
// Mod's HID parser routes through this pointer so it always reads the
// unfiltered original — set to the trampoline once GetRawInputData is hooked.
static PFN_GetRawInputData g_pGetRawInputData =
    (PFN_GetRawInputData)&GetRawInputData;

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
    g_blockOverlappingInputs = GetPrivateProfileIntA(
        "Settings", "BlockOverlappingInputs", 1, iniPath) != 0;
    g_reloadKey = ReadHexValue("Settings", "ReloadKey", 0, iniPath);

    // Text-modal gate (escape hatch only — set ModalGate=0 to bypass)
    g_modalGate = GetPrivateProfileIntA("Settings", "ModalGate", 1, iniPath) != 0;

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

        // Optional per-panel PanelName override — useful when a game update
        // renames a panel string (e.g. "PetView" -> "VehicleEquipmentView").
        // Empty value or "default" means use the built-in default.
        char nameBuf[64];
        GetPrivateProfileStringA(g_panels[i].section, "PanelName", "",
                                 nameBuf, sizeof(nameBuf), iniPath);
        if (nameBuf[0] != '\0' && _stricmp(nameBuf, "default") != 0) {
            strncpy(g_panels[i].customPanelName, nameBuf,
                    sizeof(g_panels[i].customPanelName) - 1);
            g_panels[i].customPanelName[sizeof(g_panels[i].customPanelName) - 1] = '\0';
            g_panels[i].panelName = g_panels[i].customPanelName;
        } else {
            g_panels[i].customPanelName[0] = '\0';
            g_panels[i].panelName = g_panels[i].defaultPanelName;
        }

        // Optional per-panel ExecuteEvent override — selects a sub-tab inside
        // the panel (e.g. PetView "" = Pets tab, "SpecialVehicle" = Special
        // Mount tab; WorldMapView "Skill" = Skill tab). Special values:
        //   missing / "default" -> use built-in default
        //   "none"              -> pass nullptr (game picks its own default)
        //   "empty" or "" tag   -> pass empty string ""  (== tabbar event="")
        //   anything else       -> pass that literal string
        char eeBuf[64];
        GetPrivateProfileStringA(g_panels[i].section, "ExecuteEvent", "__unset__",
                                 eeBuf, sizeof(eeBuf), iniPath);
        if (strcmp(eeBuf, "__unset__") == 0 || _stricmp(eeBuf, "default") == 0) {
            // Use built-in default
            g_panels[i].hasExecuteEvent = g_panels[i].defaultHasExecuteEvent;
            if (g_panels[i].defaultHasExecuteEvent && g_panels[i].defaultExecuteEvent) {
                strncpy(g_panels[i].executeEvent, g_panels[i].defaultExecuteEvent,
                        sizeof(g_panels[i].executeEvent) - 1);
                g_panels[i].executeEvent[sizeof(g_panels[i].executeEvent) - 1] = '\0';
            } else {
                g_panels[i].executeEvent[0] = '\0';
            }
        } else if (_stricmp(eeBuf, "none") == 0) {
            g_panels[i].hasExecuteEvent = false;
            g_panels[i].executeEvent[0] = '\0';
        } else if (_stricmp(eeBuf, "empty") == 0) {
            g_panels[i].hasExecuteEvent = true;
            g_panels[i].executeEvent[0] = '\0';
        } else {
            g_panels[i].hasExecuteEvent = true;
            strncpy(g_panels[i].executeEvent, eeBuf,
                    sizeof(g_panels[i].executeEvent) - 1);
            g_panels[i].executeEvent[sizeof(g_panels[i].executeEvent) - 1] = '\0';
        }
    }
}

// --- XInput ---
static bool InitXInput() {
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };

    // Prefer a DLL the game has ALREADY loaded — guarantees that any hook we
    // install later sits on the same module the game's calls go through.
    for (int i = 0; i < 3; i++) {
        HMODULE h = GetModuleHandleA(dlls[i]);
        if (!h) continue;
        PFN_XInputGetState fn = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
        if (!fn) continue;
        g_hXInput = h;
        g_pXInputGetState = fn;
        Log("XInput: reusing already-loaded %s", dlls[i]);
        return true;
    }
    // Game hasn't loaded XInput yet — load 1_4 ourselves
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
    g_pGetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
    if (dwSize == 0 || dwSize > 1024) return;

    BYTE buf[1024];
    if (g_pGetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &dwSize, sizeof(RAWINPUTHEADER)) == (UINT)-1)
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

// mainChar mode/sub-mode/subtype-array offsets, re-derived at init by
// FindModeOffsets() from the ModeSwitcher function. They are only ever read
// when the root global (g_pmGlobalAddr) resolved as well. On CD 2.01.00 the
// mode state no longer lives in mainChar and the flag-array anchor is gone, so
// g_pmGlobalAddr stays 0 and IsGameplayState() cannot evaluate the state — it
// then reports "safe" and the gate is effectively off.
static uint32_t g_offModeByte = 0xCA8;  // mainChar+N: u8 current mode (4 = ingame)
static uint32_t g_offSubByte  = 0xCA9;  // mainChar+N: u8 current sub-mode
static uint32_t g_offSubtypes = 0xCB8;  // mainChar+N: 16-slot panel-state flag array

// Check if the game is in a safe state for opening panels.
// Blocks during cutscenes, QTEs, minigames, and the in-game menu.
// Sub-mode values (stable across 1.0.4.1 .. 1.13, only their OFFSET moves):
//   0x06=cinema, 0x07=qte, 0x08=minigame  → UNSAFE
//   0x0D = gameplay variant (also seen while a mod-opened panel is active) → SAFE
//   0x0E = ingamemenu (pre-Apr-23 this was gameplay)  → UNSAFE
//   0x0F = hud-info + hud-play + quickslot (gameplay, no interaction target) → SAFE
//   0x10 = hud-info + hud-play + interaction + quickslot (interaction target) → SAFE
// 0x0E is the single problematic gameplay-looking value after the update, so
// whitelist gameplay values explicitly rather than using a single threshold.
// The sub-mode byte is only meaningful while mode==4 (ingame-global); without
// the mode check a loading screen passes (mode=0 but sub still holds a stale
// 0x0F from the previous gameplay frame).
static bool IsGameplayState() {
    if (g_pmGlobalAddr == 0) return true;  // can't check, assume safe
    __try {
        uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
        if (!root) return true;
        uintptr_t mc = *(uintptr_t*)(root + 0x48);
        if (!mc) return true;
        uint8_t mode    = *(uint8_t*)(mc + g_offModeByte);
        uint8_t subtype = *(uint8_t*)(mc + g_offSubByte);
        if (mode == 4 &&
            (subtype == 0x0D || subtype == 0x0F || subtype == 0x10)) return true;
        Log("[Safety] Blocked: unsafe state (mode=0x%02X subtype=0x%02X)", mode, subtype);
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return true;
}

// Menu state flag offsets (scanned dynamically from game code)
static int g_flagOffset1 = 0;  // first byte-write-zero offset (near "LogoutView")
static int g_flagOffset2 = 0;  // second byte-write-zero offset

// ============================================================
//  Panel close
// ============================================================
// Closing used to mean "clear two menu-state bytes" in mainChar. That anchor
// has been gone since CD 1.13, which turned the close into a silent no-op.
// Since 2.01.00 the mod calls the game's own menu-close function instead
// (ResolveMenuClose / CloseMenuNow); the open/closed test reads the per-panel
// view state byte. Menu-state bytes, a synthetic ESC and the "PrevMenu" event
// were all tried and dropped again.

// ------------------------------------------------------------------
//  The game's own "is this panel open?" test and menu-close call
// ------------------------------------------------------------------
// Recovered from the game's own panel toggle code, which reads:
//
//     call    [vtable+0x110]            ; -> PanelManager
//     lea     rdx, [panel name]
//     call    FindPanelTop              ; -> panel object
//     test    rax, rax        / je      skip
//     movzx   eax, byte [rax + STATE]   ; 0F B6 80 <disp32>
//     and     al, 0x60                  ; 24 60
//     cmp     al, 0x40                  ; 3C 40
//     jne     skip                      ; not open -> nothing to close
//     mov     rcx, [rip + GLOBAL]
//     mov     rcx, [rcx + OBJOFF]       ; 48 8B 89 <disp32>
//     xor     edx, edx / xor r8d, r8d
//     call    MENUCLOSE
//
// Everything the mod needs is in that one shape, and none of it is a
// per-caller context object: the close takes a menu object reachable from a
// plain global. Anchoring the scan on FindPanelTop (which the mod resolves
// precisely) plus the exact AND 0x60 / CMP 0x40 test makes the match
// self-validating. On build 25116796 exactly two sites match and both agree.
//
// This replaces the menu-state flag array (unresolvable since CD 1.13, which
// made toggle-close a no-op).
static bool ResolveMenuClose() {
    BYTE* base = (BYTE*)g_gameBase;

    // Pass 1: the state offset on its own. The open-test idiom appears all over
    // the UI code, so even if the close-call shape ever changes, IsPanelOpen()
    // keeps working. Majority vote over every occurrence.
    {
        uint32_t cand[8] = {}; int votes[8] = {}; int nc = 0;
        for (DWORD i = 0; (size_t)i + 11 < g_imageSize; i++) {
            if (base[i] != 0x0F || base[i+1] != 0xB6 || base[i+2] != 0x80) continue;
            if (base[i+7] != 0x24 || base[i+8] != 0x60) continue;
            if (base[i+9] != 0x3C || base[i+10] != 0x40) continue;
            uint32_t off = *(uint32_t*)(base + i + 3);
            if (off < 0x10 || off > 0x2000) continue;
            int k = 0;
            for (; k < nc; k++) if (cand[k] == off) { votes[k]++; break; }
            if (k == nc && nc < 8) { cand[nc] = off; votes[nc] = 1; nc++; }
        }
        int best = 0;
        for (int k = 0; k < nc; k++)
            if (votes[k] > best) { best = votes[k]; g_panelStateOff = cand[k]; }
        if (g_panelStateOff)
            Log("MenuClose: panel state byte at +0x%X (%d sites)", g_panelStateOff, best);
        else
            Log("MenuClose: panel open-test idiom not found — open detection disabled");
    }

    if (!g_findPanelTopAddr) {
        Log("MenuClose: FindPanelTop unresolved — close call cannot be derived");
        return false;
    }

    // Pass 2: the full shape, anchored on FindPanelTop.
    uintptr_t fn = 0, glob = 0;
    uint32_t  objOff = 0;
    int matches = 0, disagreements = 0;
    for (DWORD i = 0; (size_t)i + 0x80 < g_imageSize; i++) {
        if (base[i] != 0x0F || base[i+1] != 0xB6 || base[i+2] != 0x80) continue;
        if (base[i+7] != 0x24 || base[i+8] != 0x60) continue;
        if (base[i+9] != 0x3C || base[i+10] != 0x40) continue;

        bool anchored = false;
        for (int back = 5; back <= 0x30 && (DWORD)back <= i; back++) {
            BYTE* q = base + i - back;
            if (q[0] != 0xE8) continue;
            if ((uintptr_t)(q + 5) + *(int32_t*)(q + 1) == g_findPanelTopAddr) {
                anchored = true; break;
            }
        }
        if (!anchored) continue;

        // mov rcx,[rcx+disp32]  = 48 8B 89 disp32, then a CALL shortly after
        for (int f = 11; f < 0x60; f++) {
            BYTE* q = base + i + f;
            if (q[0] != 0x48 || q[1] != 0x8B || q[2] != 0x89) continue;
            uint32_t oo = *(uint32_t*)(q + 3);
            if (oo < 0x10 || oo > 0x2000) break;
            uintptr_t g = 0;
            for (int back = 7; back <= 0x20; back++) {
                BYTE* r = q - back;
                if (r[0] == 0x48 && r[1] == 0x8B && r[2] == 0x0D) {
                    uintptr_t cand = (uintptr_t)(r + 7) + *(int32_t*)(r + 3);
                    if (cand > g_gameBase && cand < g_gameBase + g_imageSize) g = cand;
                }
            }
            uintptr_t tgt = 0;
            for (int f2 = 7; f2 < 0x20; f2++) {
                BYTE* r = q + f2;
                if (r[0] != 0xE8) continue;
                uintptr_t t = (uintptr_t)(r + 5) + *(int32_t*)(r + 1);
                if (t > g_gameBase && t < g_gameBase + g_imageSize) tgt = t;
                break;
            }
            if (!g || !tgt) break;
            matches++;
            if (!fn) { fn = tgt; glob = g; objOff = oo; }
            else if (fn != tgt || glob != g || objOff != oo) disagreements++;
            break;
        }
    }

    if (!fn || disagreements) {
        Log("MenuClose: NOT resolved (%d match(es), %d disagreement(s))",
            matches, disagreements);
        return false;
    }
    g_menuCloseFn    = fn;
    g_menuRootGlobal = glob;
    g_menuObjOff     = objOff;
    Log("MenuClose: OK fn=base+0x%llX  menuObj=*(*(base+0x%llX)+0x%X)  (%d agreeing sites)",
        (unsigned long long)(fn - g_gameBase),
        (unsigned long long)(glob - g_gameBase), objOff, matches);
    return true;
}

// View state of one panel object: 0 = not found, else the state byte.
static bool ReadPanelState(const char* panelName, BYTE* out) {
    if (!panelName || !panelName[0]) return false;
    if (!g_findPanelTopAddr || !g_panelStateOff) return false;
    LONGLONG pm = g_panelManager;
    if (pm == 0) return false;
    typedef LONGLONG (__fastcall* FindPanelTopFn2)(LONGLONG, const char*);
    __try {
        LONGLONG panel = ((FindPanelTopFn2)g_findPanelTopAddr)(pm, panelName);
        if (!panel) return false;
        *out = *(BYTE*)(panel + g_panelStateOff);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Ask the game whether a panel is currently on screen. Far better than the old
// global flag array: it is per panel, and it stays correct when the player
// closes a panel with ESC behind the mod's back.
//
// The menu panels (Inventory, Quest, Skill, Knowledge, ...) are tabs inside
// MainMenuView2. Most of them carry the "open" state themselves, but the
// quest panel's own object never does (it reads 0x90 while visibly open on
// 2.02.00). So the container counts too: if the panel's own byte does not say
// "open" but MainMenuView2 is open, the panel is treated as open.
static bool IsPanelOpenByName(const char* panelName) {
    BYTE st = 0;
    if (!ReadPanelState(panelName, &st)) return false;
    if ((st & 0x60) == 0x40) return true;
    BYTE mm = 0;
    bool mmOpen = ReadPanelState("MainMenuView2", &mm) && (mm & 0x60) == 0x40;
    Log("Panel '%s' state byte +0x%X = 0x%02X, MainMenuView2 = 0x%02X -> %s",
        panelName, g_panelStateOff, st, mm, mmOpen ? "open (via container)" : "not open");
    return mmOpen;
}

static bool PanelOpenCheckAvailable() {
    return g_findPanelTopAddr != 0 && g_panelStateOff != 0 && g_panelManager != 0;
}

static void CloseMenuNow() {
    if (!g_menuCloseFn || !g_menuRootGlobal) {
        Log("Close: menu-close unresolved");
        return;
    }
    typedef void (__fastcall* MenuCloseFn)(LONGLONG, LONGLONG, LONGLONG);
    __try {
        LONGLONG root = *(LONGLONG*)g_menuRootGlobal;
        if (!root) { Log("Close: menu root global is null"); return; }
        LONGLONG obj = *(LONGLONG*)(root + g_menuObjOff);
        if (!obj) { Log("Close: menu object is null"); return; }
        ((MenuCloseFn)g_menuCloseFn)(obj, 0, 0);
        Log("Close: menu closed via the game's own call");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("Close: menu-close call raised an exception");
    }
}

// Check if any panel-state flag in the 16-byte slot array at mc+g_offSubtypes
// is set (0xCB8 pre-1.13, 0xCB0 on 1.13 — resolved by FindModeOffsets).
// Used at key-press time to verify some panel is still open before toggle-close.
// Post Apr-23 2026 update: single-byte check on flag1 (slot 0x0D) became unreliable
// (transient slot). The scanned flag offsets (g_flagOffset1/g_flagOffset2) both
// fall inside this 16-byte array — scanning the whole array covers every panel.
// True only when the flag array is actually reachable. IsPanelFlagSet() returns
// false both for "no panel open" and for "cannot tell", so callers that would
// act on a negative MUST check this first.
static bool PanelFlagsAvailable() {
    return g_pmGlobalAddr != 0 && g_flagOffset1 != 0;
}

static bool IsPanelFlagSet() {
    if (!PanelFlagsAvailable()) return false;
    __try {
        uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
        if (!root) return false;
        uintptr_t mc = *(uintptr_t*)(root + 0x48);
        if (!mc) return false;
        volatile uint8_t* arr = (volatile uint8_t*)(mc + g_offSubtypes);
        for (int i = 0; i < 16; i++) {
            if (arr[i] != 0) return true;
        }
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Detour for the TextEditModalMessage open function (FUN_140b85bf0 in
// build 1.0.4.1, identified via "TextEditModalMessage" string xref). Calls
// original, then sets g_textModalActive when the modal was created.
typedef LONGLONG* (__fastcall *TextEditModalFn)(LONGLONG, LONGLONG, LONGLONG, UINT);

static LONGLONG* __fastcall DetourTextEditModal(LONGLONG p1, LONGLONG p2,
                                                 LONGLONG p3, UINT p4) {
    LONGLONG* result = ((TextEditModalFn)g_modalTrampoline)(p1, p2, p3, p4);
    if (result) {
        // Capture vftable for the liveness check below; reading the modal
        // object's first 8 bytes is safe right after construction.
        __try {
            g_modalVftable = *(uintptr_t*)result;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_modalVftable = 0;
        }
        g_modalObj = (uintptr_t)result;
        g_textModalActive = true;
        Log("[ModalHook] TextEdit modal opened (obj=0x%p, vft=0x%llX)",
            (void*)result, (unsigned long long)g_modalVftable);
    }
    return result;
}

// Probes the captured modal object for liveness. Used in the InputThread
// poll loop so the flag clears when the modal is dismissed by mouse click
// (no ESC/Enter edge to drive the keyboard path).
//
// Two-tier check:
//  1. vftable still equals what we captured at hook time → object likely alive
//  2. state field at *(obj + 0x140) + 0xd4 still == 0xf (set by the opener
//     to mark TextEdit type; reset/freed when the modal is destroyed)
// Either failure → flag cleared.
static bool ProbeModalAlive() {
    if (g_modalObj == 0 || g_modalVftable == 0) return false;
    __try {
        if (*(uintptr_t*)g_modalObj != g_modalVftable) return false;
        uintptr_t sub = *(uintptr_t*)(g_modalObj + 0x140);
        if (!sub) return false;
        return *(int*)(sub + 0xd4) == 0xf;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static bool IsModalActive() {
    if (!g_modalGate) return false;
    return g_textModalActive;
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

// ============================================================
//  Precise OpenPanel / FindPanelTop resolution
// ============================================================
// The scoring resolver further down collects EVERY call in a 200-byte window
// after a panel-name LEA. On the 2026-09 build that lets the generic
// name->panel lookup outscore OpenPanel itself — the lookup is reachable from
// far more sites — so the mod ended up hooking the lookup and no hotkey did
// anything. Both functions have a much tighter signature than "a call nearby":
//
//   FindPanelTop(pm, name)    is the callee of the FIRST call within 40 bytes
//                             of a LEA RDX,[panel name].
//   OpenPanel(pm, name, ...)  CONTAINS "LEA RDX,[MainMenuView2]; CALL
//                             FindPanelTop" inside its first 128 bytes, and is
//                             itself called with panel-name strings in RDX.
//
// Verified on build 25116796: FindPanelTop wins 21 of 21 MainMenuView2 sites,
// and OpenPanel is the only MainMenuView2-carrying function that is invoked
// with three different panel names. Falls back to the old scoring resolver.

// Defined further down with the other image-walking helpers.
static uintptr_t FindFunctionStart(uintptr_t midAddr);

// First CALL rel32 within `span` bytes after `from`; 0 if the next call does
// not resolve into the image.
static uintptr_t FirstCallAfter(uintptr_t from, int span) {
    BYTE* p = (BYTE*)from;
    for (int j = 7; j < span; j++) {
        if (p[j] != 0xE8) continue;
        int32_t rel = *(int32_t*)(p + j + 1);
        uintptr_t t = (uintptr_t)(p + j + 5) + rel;
        if (t > g_gameBase && t < g_gameBase + g_imageSize) return t;
        return 0;
    }
    return 0;
}

// LEA RDX, [rip+disp32] — the callee's 2nd argument, i.e. the panel name.
static inline bool IsLeaRdx(uintptr_t lea) {
    BYTE* p = (BYTE*)lea;
    return p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15;
}

static bool FindOpenPanelPrecise() {
    uintptr_t mainMenuStr = FindString("MainMenuView2");
    if (!mainMenuStr) {
        Log("Precise resolver: 'MainMenuView2' string not found");
        return false;
    }

    // --- 1) FindPanelTop: most-voted callee of LEA RDX,[MainMenuView2] ---
    const int MAXC = 16;
    uintptr_t cand[MAXC] = {};
    int       votes[MAXC] = {};
    int       nc = 0;
    uintptr_t lea = 0;
    for (int guard = 0; guard < 64; guard++) {
        lea = FindLEA(mainMenuStr, lea ? lea + 1 : 0);
        if (!lea) break;
        if (!IsLeaRdx(lea)) continue;
        uintptr_t t = FirstCallAfter(lea, 40);
        if (!t) continue;
        int k = 0;
        for (; k < nc; k++) if (cand[k] == t) { votes[k]++; break; }
        if (k == nc && nc < MAXC) { cand[nc] = t; votes[nc] = 1; nc++; }
    }
    uintptr_t panelTop = 0;
    int topVotes = 0;
    for (int k = 0; k < nc; k++)
        if (votes[k] > topVotes) { topVotes = votes[k]; panelTop = cand[k]; }
    if (!panelTop || topVotes < 2) {
        Log("Precise resolver: no clear FindPanelTop (best %d votes)", topVotes);
        return false;
    }
    Log("Precise resolver: FindPanelTop = base+0x%llX (%d/%d MainMenuView2 sites)",
        (unsigned long long)(panelTop - g_gameBase), topVotes, nc ? nc : 1);

    // --- 2) OpenPanel: MainMenuView2-carrying function called with panel names ---
    uintptr_t owners[MAXC] = {};
    int       nOwners = 0;
    lea = 0;
    for (int guard = 0; guard < 64; guard++) {
        lea = FindLEA(mainMenuStr, lea ? lea + 1 : 0);
        if (!lea) break;
        if (!IsLeaRdx(lea)) continue;
        if (FirstCallAfter(lea, 40) != panelTop) continue;
        uintptr_t fn = FindFunctionStart(lea);
        if (!fn || lea - fn > 128) continue;
        bool dup = false;
        for (int k = 0; k < nOwners; k++) if (owners[k] == fn) { dup = true; break; }
        if (!dup && nOwners < MAXC) owners[nOwners++] = fn;
    }
    if (!nOwners) {
        Log("Precise resolver: no function carries MainMenuView2 near its start");
        return false;
    }

    static const char* const probes[] = {
        "InventoryEquipmentPanel", "SkillTreePanel",
        "KnowledgePanel2", "QuestMenuPanel",
    };
    const int nProbes = (int)(sizeof(probes) / sizeof(probes[0]));
    int hits[MAXC] = {};
    for (int pi = 0; pi < nProbes; pi++) {
        uintptr_t str = FindString(probes[pi]);
        if (!str) continue;
        bool seen[MAXC] = {};
        uintptr_t l = 0;
        for (int guard = 0; guard < 64; guard++) {
            l = FindLEA(str, l ? l + 1 : 0);
            if (!l) break;
            if (!IsLeaRdx(l)) continue;
            uintptr_t t = FirstCallAfter(l, 40);
            if (!t) continue;
            for (int k = 0; k < nOwners; k++)
                if (owners[k] == t && !seen[k]) { seen[k] = true; hits[k]++; }
        }
    }
    int bestK = -1, bestHits = 0;
    for (int k = 0; k < nOwners; k++)
        if (hits[k] > bestHits) { bestHits = hits[k]; bestK = k; }
    if (bestK < 0 || bestHits < 2) {
        Log("Precise resolver: %d MainMenuView2 owner(s), none called with 2+ panel names",
            nOwners);
        return false;
    }

    g_openPanelAddr    = owners[bestK];
    g_findPanelTopAddr = panelTop;
    Log("OpenPanel found at base+0x%llX (precise: %d/%d panel names, %d owner candidates)",
        (unsigned long long)(g_openPanelAddr - g_gameBase),
        bestHits, nProbes, nOwners);
    return true;
}

// Cross-reference strategy:
// 1. Find multiple known panel strings
// 2. For each string, find LEA references -> collect ALL CALL targets nearby
// 3. Best match = OpenPanel, second best = FindPanel
static bool FindOpenPanelFunction() {
    if (FindOpenPanelPrecise()) return true;
    Log("Precise resolver failed — falling back to the legacy CALL-window scoring");

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

    // Disambiguate OpenPanel vs FindPanel when both candidates have the same
    // score. OpenPanel calls FindPanel internally as part of its lookup-then-
    // open flow; FindPanel is a small leaf helper. If secondTarget contains
    // a CALL to bestTarget (but not the other way round), then secondTarget
    // is actually OpenPanel and bestTarget is FindPanel — swap their roles.
    if (secondScore >= 2) {
        BYTE* base = (BYTE*)g_gameBase;
        auto containsCallTo = [&](uintptr_t funcAddr, uintptr_t target) -> bool {
            if (funcAddr < g_gameBase) return false;
            DWORD funcOff = (DWORD)(funcAddr - g_gameBase);
            const int range = 4096;  // OpenPanel body is large
            for (int i = 0; i < range && funcOff + i + 5 <= g_imageSize; i++) {
                if (base[funcOff + i] != 0xE8) continue;
                int32_t rel = *(int32_t*)(base + funcOff + i + 1);
                uintptr_t callTarget = funcAddr + i + 5 + rel;
                if (callTarget == target) return true;
            }
            return false;
        };

        bool bestCallsSecond  = containsCallTo(bestTarget, secondTarget);
        bool secondCallsBest  = containsCallTo(secondTarget, bestTarget);
        if (secondCallsBest && !bestCallsSecond) {
            uintptr_t tmp = bestTarget;
            bestTarget = secondTarget;
            secondTarget = tmp;
            int tmpS = bestScore;
            bestScore = secondScore;
            secondScore = tmpS;
            Log("Heuristic swap: bestTarget calls smaller target -> swapped roles");
        }
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
//  Sub-Tab Helper Scanner (FindPanelTop + SetSubTab)
// ============================================================
//
// After the new game build (1.07.00) shifted internal addresses, the
// hardcoded FindPanelTop (g_gameBase + 0x3349660) and SetSubTab
// (g_gameBase + 0xb6f840) used by the post-open sub-tab switch crashed
// the game. Both functions are called from inside OpenPanel, so we scan
// OpenPanel's body to extract their addresses.
//
// FindPanelTop is the FIRST CALL OpenPanel makes, immediately following
// the prolog and a LEA RDX, [rip+disp] that loads the "MainMenuView2"
// string pointer (used by every panel-open to locate the main view).
//
// SetSubTab is identified by its argument-setup signature:
//   XOR R9D, R9D     ; 45 33 C9   (4th arg = 0)
//   XOR R8D, R8D     ; 45 33 C0   (3rd arg = 0)
//   MOV EDX, [rXX+0x1AC]          (2nd arg = secondary tab index from
//                                  the MainMenuView2 state struct)
// followed by a CALL within ~24 bytes.
static bool FindSubTabHelpers() {
    if (g_openPanelAddr == 0) return false;
    BYTE* base = (BYTE*)g_gameBase;
    BYTE* op   = (BYTE*)g_openPanelAddr;
    const int maxScan = 4096;
    DWORD openOff = (DWORD)(g_openPanelAddr - g_gameBase);
    if (openOff + maxScan > g_imageSize) return false;

    uintptr_t mainMenuStr = FindString("MainMenuView2");
    if (!mainMenuStr) {
        Log("WARNING: 'MainMenuView2' string not found — sub-tab helpers unresolved");
        return false;
    }

    // 1) FindPanelTop: first LEA RDX, [rip+disp]=MainMenuView2 then next E8 CALL
    for (int i = 0; i < 128; i++) {
        BYTE* p = op + i;
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;
        int32_t disp = *(int32_t*)(p + 3);
        uintptr_t resolved = (uintptr_t)(p + 7) + disp;
        if (resolved != mainMenuStr) continue;

        // Found LEA RDX, [MainMenuView2] — scan forward up to 32 bytes
        // for the immediately-following E8 CALL.
        for (int j = 7; j < 40; j++) {
            BYTE* q = p + j;
            if (q[0] != 0xE8) continue;
            int32_t rel = *(int32_t*)(q + 1);
            uintptr_t target = (uintptr_t)(q + 5) + rel;
            if (target <= g_gameBase || target >= g_gameBase + g_imageSize) continue;
            g_findPanelTopAddr = target;
            Log("FindPanelTop found at base+0x%llX (first CALL after LEA RDX=MainMenuView2)",
                (unsigned long long)(target - g_gameBase));
            break;
        }
        break;
    }
    if (g_findPanelTopAddr == 0)
        Log("WARNING: FindPanelTop not located inside OpenPanel — sub-tab switch disabled");

    // 2) SetSubTab: XOR R9D,R9D ; XOR R8D,R8D ; MOV EDX,[rXX+0x1AC] ; ... CALL E8
    // The XOR pair can also appear in reverse order (XOR R8D first, XOR R9D
    // second) depending on register pressure — accept either ordering.
    for (int i = 64; i < maxScan - 32; i++) {
        BYTE* p = op + i;
        bool pairOK = false;
        int after = 0;
        // Order A: 45 33 C9   45 33 C0
        if (p[0] == 0x45 && p[1] == 0x33 && p[2] == 0xC9 &&
            p[3] == 0x45 && p[4] == 0x33 && p[5] == 0xC0) {
            pairOK = true; after = 6;
        }
        // Order B: 45 33 C0   45 33 C9
        else if (p[0] == 0x45 && p[1] == 0x33 && p[2] == 0xC0 &&
                 p[3] == 0x45 && p[4] == 0x33 && p[5] == 0xC9) {
            pairOK = true; after = 6;
        }
        if (!pairOK) continue;

        // Within the next 24 bytes, require a MOV EDX, [rXX+0x1AC] sequence
        // (8B XX AC 01 00 00 — modrm with disp32=0x000001AC, /r=2 (EDX)).
        bool sawEdxLoad = false;
        for (int j = after; j < after + 24 && (i + j + 6) < maxScan; j++) {
            BYTE* q = p + j;
            if (q[0] != 0x8B) continue;
            if (((q[1] >> 3) & 0x07) != 2) continue;       // /r = EDX
            if ((q[1] & 0xC0) != 0x80) continue;           // mod=10 -> disp32
            if (*(uint32_t*)(q + 2) != 0x000001AC) continue;
            sawEdxLoad = true; break;
        }
        if (!sawEdxLoad) continue;

        // Find the next E8 CALL within 40 bytes after the XOR pair
        for (int j = after; j < after + 40 && (i + j + 5) < maxScan; j++) {
            BYTE* q = p + j;
            if (q[0] != 0xE8) continue;
            int32_t rel = *(int32_t*)(q + 1);
            uintptr_t target = (uintptr_t)(q + 5) + rel;
            if (target <= g_gameBase || target >= g_gameBase + g_imageSize) continue;
            g_setSubTabAddr = target;
            Log("SetSubTab found at base+0x%llX (after XOR R8/R9 + MOV EDX,[+0x1AC])",
                (unsigned long long)(target - g_gameBase));
            break;
        }
        if (g_setSubTabAddr) break;
    }
    if (g_setSubTabAddr == 0)
        Log("WARNING: SetSubTab not located inside OpenPanel — sub-tab switch disabled");

    return g_findPanelTopAddr != 0 && g_setSubTabAddr != 0;
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

            // The flag writes run against a register loaded as:
            //   MOV reg, qword ptr [rOther + 0x48]   (3-4 bytes, disp8)
            // and rOther was loaded from a global:
            //   MOV rOther, qword ptr [rip+disp32]   (7 bytes, REX.W[+R] 8B xx)
            // Scan backwards up to 64 bytes for this pair so we can resolve
            // mc = *(global + 0x48) without relying on the (now-broken)
            // FindPanel chain scanner.
            int chainMcOffset = 0;
            uintptr_t chainGlobal = 0;
            for (int back = 1; back <= 64; back++) {
                BYTE* q = p - back;
                // MOV r64, [r/m64 + disp8]: REX.W (48/49/4C/4D) 8B modrm disp8
                if ((q[0] != 0x48 && q[0] != 0x49 && q[0] != 0x4C && q[0] != 0x4D) ||
                    q[1] != 0x8B)
                    continue;
                BYTE modrm = q[2];
                if ((modrm & 0xC0) != 0x40) continue;     // need mod=1 (disp8)
                if ((modrm & 0x07) == 4 || (modrm & 0x07) == 5) continue; // skip SIB/RBP
                int mcOff = (int)(signed char)q[3];
                if (mcOff <= 0 || mcOff > 0x100) continue;

                // Now scan upward up to 64 bytes for a RIP-relative load
                // into the same source register used here (r/m field of modrm).
                int srcReg = modrm & 0x07;
                bool srcIsExt = (q[0] == 0x49 || q[0] == 0x4D); // REX.B -> extended rm
                for (int back2 = 1; back2 <= 64; back2++) {
                    BYTE* r = q - back2;
                    // MOV r64, [rip+disp32]: REX.W[+R] (48/4C) 8B modrm=00_xxx_101
                    if (r[0] != 0x48 && r[0] != 0x4C) continue;
                    if (r[1] != 0x8B) continue;
                    BYTE m2 = r[2];
                    if ((m2 & 0xC7) != 0x05) continue;    // mod=0, rm=5 (RIP-rel)
                    int dstReg = (m2 >> 3) & 0x07;
                    bool dstIsExt = (r[0] == 0x4C);        // REX.R -> extended dst
                    if (dstReg != srcReg || dstIsExt != srcIsExt) continue;
                    int32_t rel = *(int32_t*)(r + 3);
                    uintptr_t global = (uintptr_t)(r + 7) + rel;
                    if (global < g_gameBase ||
                        global >= g_gameBase + g_imageSize) continue;
                    chainGlobal = global;
                    chainMcOffset = mcOff;
                    break;
                }
                if (chainGlobal) break;
            }

            if (chainGlobal && chainMcOffset) {
                g_pmGlobalAddr = chainGlobal;
                g_pmOffset1 = chainMcOffset;
                Log("FlagChain: global=base+0x%llX, mcOffset=0x%X (mc=*(*global+0x%X))",
                    (unsigned long long)(chainGlobal - g_gameBase),
                    chainMcOffset, chainMcOffset);
            } else {
                Log("FlagChain: NOT found — flag array still reachable but IsPanelFlagSet degraded");
            }
            return true;
        }
    }

    Log("Menu state flag array not found (expected since CD 1.13) — open/closed "
        "state comes from the per-panel view state instead");
    return false;
}

static bool FindPanelManagerChain() {
    BYTE* base = (BYTE*)g_gameBase;

    // Post Apr-23 2026 build 1.0.4.1: the PM-load idiom is exactly:
    //   48 8B 0D XX XX XX XX   MOV RCX, [rip+disp32]   ; root = *global
    //   48 8B ?? XX            MOV r, [RCX+disp8]      ; a = *(root + off1)
    //   48 8B ?? XX            MOV r, [r+disp8]        ; b = *(a   + off2)
    //   48 8B ??               MOV r, r                ; reg-reg move (e.g. RDX,RAX)
    //   48 8B 09               MOV RCX, [RCX]          ; final deref -> pm
    //   E8 XX XX XX XX         CALL <wrapper>          ; call into panel lookup
    // Verified with Ghidra search_bytes: exactly two matches in the binary,
    // both give the same chain root->+0x68->+0x60->deref, which matches the
    // panel-manager pointer captured live by the OpenPanel hook.
    static const BYTE pat[] = {
        0x48, 0x8B, 0x0D, 0,0,0,0,
        0x48, 0x8B, 0, 0,
        0x48, 0x8B, 0, 0,
        0x48, 0x8B, 0,
        0x48, 0x8B, 0x09,
        0xE8, 0,0,0,0,
    };
    static const BYTE mask[] = {
        0xFF, 0xFF, 0xFF, 0,0,0,0,
        0xFF, 0xFF, 0, 0,
        0xFF, 0xFF, 0, 0,
        0xFF, 0xFF, 0,
        0xFF, 0xFF, 0xFF,
        0xFF, 0,0,0,0,
    };
    const int patLen = (int)sizeof(pat);

    for (DWORD i = 0; i + patLen <= g_imageSize; i++) {
        bool ok = true;
        for (int k = 0; k < patLen; k++) {
            if (mask[k] && base[i + k] != pat[k]) { ok = false; break; }
        }
        if (!ok) continue;

        // Validate modrm for disp8 MOVs (mod=1, rm != 4 SIB, != 5 RBP).
        BYTE m1 = base[i + 9];   // first disp8 modrm
        BYTE m2 = base[i + 13];  // second disp8 modrm
        BYTE m3 = base[i + 17];  // reg-reg modrm
        if ((m1 & 0xC0) != 0x40) continue;
        if ((m2 & 0xC0) != 0x40) continue;
        if ((m3 & 0xC0) != 0xC0) continue;

        int32_t disp = *(int32_t*)(base + i + 3);
        uintptr_t globalAddr = g_gameBase + i + 7 + disp;
        if (globalAddr < g_gameBase || globalAddr >= g_gameBase + g_imageSize)
            continue;

        int off1 = (int)(signed char)base[i + 10];
        int off2 = (int)(signed char)base[i + 14];
        if (off1 <= 0 || off2 <= 0) continue;

        g_pmGlobalAddr = globalAddr;
        g_pmOffset1 = off1;
        g_pmOffset2 = off2;
        Log("PanelManager chain found: [base+0x%llX] -> +0x%X -> +0x%X -> deref",
            (unsigned long long)(globalAddr - g_gameBase), off1, off2);

        // Try to resolve now (root may still be null this early in startup —
        // that's fine, InputThread retries via ReadPanelManagerFromGlobal).
        __try {
            uintptr_t ptr = *(uintptr_t*)globalAddr;
            if (ptr) {
                ptr = *(uintptr_t*)(ptr + off1);
                if (ptr) {
                    ptr = *(uintptr_t*)(ptr + off2);
                    if (ptr) {
                        uintptr_t pm = *(uintptr_t*)ptr;
                        if (pm) {
                            g_panelManager = (LONGLONG)pm;
                            Log("PanelManager resolved immediately: 0x%llX",
                                (unsigned long long)pm);
                        }
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return true;
    }

    Log("WARNING: Could not find PanelManager global chain");
    return false;
}

// Re-read PanelManager from the known pointer chain
static bool ReadPanelManagerFromGlobal() {
    if (g_pmGlobalAddr == 0 || g_pmOffset2 == 0) return false;
    __try {
        uintptr_t ptr = *(uintptr_t*)g_pmGlobalAddr;
        if (!ptr) return false;
        ptr = *(uintptr_t*)(ptr + g_pmOffset1);
        if (!ptr) return false;
        ptr = *(uintptr_t*)(ptr + g_pmOffset2);
        if (!ptr) return false;
        ptr = *(uintptr_t*)ptr;
        if (ptr) {
            // Learned/seeded chains (unlike the static-scan chain) may be
            // stale after a game update — reject implausible values instead
            // of handing a garbage pointer to OpenPanel.
            if (g_pmChainLearned &&
                ((ptr & 0x7) || ptr < 0x10000 || ptr > 0x00007FFFFFFFFFFF))
                return false;
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
    // Operand-size prefix (0x66) — most commonly seen here as `66 90` (2-byte
    // NOP for alignment padding before a function body). Returns its own
    // length and lets the caller call InstrLen on the next byte if it isn't
    // a recognized standalone form. Handle `66 90` directly as the typical
    // case so a function whose entry is aligned with this NOP doesn't trip
    // the unknown-instruction guard.
    if (ip[0] == 0x66 && ip[1] == 0x90) return 2;

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

static bool SafeReadPtr(uintptr_t addr, uintptr_t* out) {
    __try { *out = *(uintptr_t*)addr; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The July 2026 (1.13) build dropped the static PanelManager load idiom that
// FindPanelManagerChain matches — callers now fetch pm through per-object
// virtual getters (vtable+0x108), so no image-wide byte pattern exists anymore.
// Fallback, driven entirely from the OpenPanel hook (game thread, so no
// synchronization needed): search the known root singleton (g_pmGlobalAddr,
// same global the flag chain resolved) for a root->+A->+B->deref chain that
// yields the live pm. A match is only a CANDIDATE — it could be a transient
// object that coincidentally points at pm right now. It is promoted into
// g_pmOffset1/2 (re-enabling ReadPanelManagerFromGlobal's stale-pointer
// protection) only after agreeing with the live pm on 3 separate captures,
// and demoted again on any divergence or panel-call crash. Same
// N-consecutive-sightings pattern as the container-vtable auto-relearn in
// Private Storage Anywhere. State lives in the globals block up top
// (g_pmChainCandA/B, g_pmChainAgree, g_pmChainLearned).

// --- Static PanelManager-chain derivation (byte-pattern, boot time) ---------
// 1.13 dropped the old load idiom that FindPanelManagerChain matched, but the
// chain is still derivable statically from a different anchor: FindPanelTop
// (already resolved via the "MainMenuView2" xref) is called at a handful of
// sites with its 1st argument loaded as  rcx = [[[root_global + A] + B]]  and
// a panel-name string in rdx. Scanning FindPanelTop's call sites for that
// compact 3-MOV chain (preceded by a RIP-load of the root global) recovers
// (A,B) with no runtime learning and no persistence — same "read it fresh
// from the binary every boot" model the mod used before 1.13. Verified on
// 1.13.01: exactly 4 sites, all agreeing on +0x68/+0x68.

// modrm reg/rm register numbers, folding in REX.R / REX.B extension bits.
static inline int PmModDest(BYTE rex, BYTE m) { return (((rex>>2)&1)<<3) | ((m>>3)&7); }
static inline int PmModBase(BYTE rex, BYTE m) { return ((rex&1)<<3) | (m&7); }

// mov r64,[r64+disp8]  =  REX.W(48/49/4C/4D) 8B modrm(mod=01,rm!=100) disp8.
// Reports the disp8, instruction length, and both register numbers so the
// caller can verify the load chains register-to-register.
static bool PmMovLoadDisp8(const BYTE* p, int* disp, int* len,
                           int* destReg, int* baseReg) {
    BYTE rex = p[0];
    if (rex!=0x48 && rex!=0x4C && rex!=0x49 && rex!=0x4D) return false;
    if (p[1]!=0x8B) return false;
    BYTE m = p[2];
    if ((m & 0xC0)!=0x40) return false;      // mod=01 (disp8)
    if ((m & 0x07)==0x04) return false;       // rm=100 -> SIB, skip
    *destReg = PmModDest(rex, m);
    *baseReg = PmModBase(rex, m);
    *disp = (int)(unsigned char)p[3];         // field offsets are 0..0x7F
    *len = 4;
    return true;
}
// mov r64,[r64]  =  REX.W 8B modrm(mod=00, rm!=100/101)
static bool PmMovLoadDeref(const BYTE* p, int* destReg, int* baseReg) {
    BYTE rex = p[0];
    if (rex!=0x48 && rex!=0x4C && rex!=0x49 && rex!=0x4D) return false;
    if (p[1]!=0x8B) return false;
    BYTE m = p[2];
    if ((m & 0xC0)!=0x00) return false;
    if ((m & 0x07)==0x04 || (m & 0x07)==0x05) return false;
    *destReg = PmModDest(rex, m);
    *baseReg = PmModBase(rex, m);
    return true;
}
// Is there a  mov <wantReg>,[rip+disp32]->root_global  in [lo,hi)?
// (data-flow: the chain's first base register must actually come from root.)
static bool PmHasRootLoadInto(const BYTE* base, DWORD lo, DWORD hi, int wantReg) {
    for (DWORD p = lo; p + 6 < hi; p++) {
        BYTE rex = base[p];
        if ((rex==0x48 || rex==0x4C) && base[p+1]==0x8B &&
            (base[p+2] & 0xC7)==0x05) {
            if (PmModDest(rex, base[p+2]) != wantReg) continue;
            int32_t d = *(int32_t*)(base + p + 3);
            if (g_gameBase + p + 7 + d == g_pmGlobalAddr) return true;
        }
    }
    return false;
}

// Derive g_pmOffset1/2 statically. Returns true on a confident match.
static bool FindPmChainStatic() {
    if (g_pmGlobalAddr == 0 || g_findPanelTopAddr == 0) return false;
    const BYTE* base = (const BYTE*)g_gameBase;
    const DWORD WIN = 0x60;
    struct { int a, b, count; } votes[16];
    int nv = 0;

    for (DWORD i = 0; i + 5 <= g_imageSize; i++) {
        if (base[i] != 0xE8) continue;
        int32_t rel = *(int32_t*)(base + i + 1);
        if (g_gameBase + i + 5 + rel != g_findPanelTopAddr) continue;

        DWORD lo = (i >= WIN) ? i - WIN : 0;
        for (DWORD p = lo; p + 6 < i; p++) {
            int a, la, b, lb, d1, b1, d2, b2, d3, b3;
            if (!PmMovLoadDisp8(base + p, &a, &la, &d1, &b1)) continue;
            if (!PmMovLoadDisp8(base + p + la, &b, &lb, &d2, &b2)) continue;
            if (b2 != d1) continue;                         // mov2 base == mov1 dest
            if (!PmMovLoadDeref(base + p + la + lb, &d3, &b3)) continue;
            if (b3 != d2) continue;                         // mov3 base == mov2 dest
            if (a <= 0 || b <= 0) continue;                 // real field offsets, and
                                                            // b>0 keeps off2 off the
                                                            // g_pmOffset2==0 sentinel
            if (!PmHasRootLoadInto(base, lo, p + 1, b1)) continue;  // mov1 base <- root
            // record one vote for (a,b), then move to the next call site
            int slot = -1;
            for (int v = 0; v < nv; v++)
                if (votes[v].a == a && votes[v].b == b) { slot = v; break; }
            if (slot < 0 && nv < 16) { slot = nv++; votes[slot].a = a; votes[slot].b = b; votes[slot].count = 0; }
            if (slot >= 0) votes[slot].count++;
            break;
        }
    }

    int best = -1, total = 0;
    for (int v = 0; v < nv; v++) {
        total += votes[v].count;
        if (best < 0 || votes[v].count > votes[best].count) best = v;
    }
    if (best < 0) {
        Log("[PmChain] static scan: no FindPanelTop chain anchor found");
        return false;
    }
    g_pmOffset1 = votes[best].a;
    g_pmOffset2 = votes[best].b;
    g_pmChainLearned = true;   // still live-validated by ValidatePmChain
    Log("[PmChain] static scan OK: [global]->+0x%X->+0x%X->deref (%d/%d anchor sites)",
        votes[best].a, votes[best].b, votes[best].count, total);
    return true;
}

// SEH-guarded read of the full candidate chain; 0 on any missing link.
static LONGLONG ReadChainPm(int a, int b) {
    uintptr_t root = 0, t1 = 0, t2 = 0, v = 0;
    if (!SafeReadPtr(g_pmGlobalAddr, &root) || !root) return 0;
    if (!SafeReadPtr(root + a, &t1) || !t1) return 0;
    if (!SafeReadPtr(t1 + b, &t2) || !t2) return 0;
    if (!SafeReadPtr(t2, &v)) return 0;
    return (LONGLONG)v;
}

// Drop the active chain (static-derived or live-learned) back to hook capture.
// In-memory only — nothing is persisted, so the next boot re-derives the chain
// statically from the binary again (self-correcting across game updates).
static void DemotePmChain(const char* why) {
    if (!g_pmChainLearned) return;
    Log("[PmChain] %s — demoted, back to hook capture", why);
    g_pmOffset2 = 0;
    g_pmChainLearned = false;
    g_pmChainCandA = g_pmChainCandB = -1;
    g_pmChainAgree = 0;
    g_pmChainZeroReads = 0;
}

// Scan for a new candidate chain. b starts at 8: b==0 would collide with the
// codebase-wide "no chain" sentinel g_pmOffset2==0 once promoted. Ascending
// scan finds the smallest (real) offsets first; a plausibility filter skips
// unaligned/non-canonical pointers.
static void LearnPmChainCandidate(LONGLONG pm) {
    uintptr_t root = 0;
    if (!SafeReadPtr(g_pmGlobalAddr, &root) || !root) return;
    for (int a = 0; a <= 0x180; a += 8) {
        uintptr_t t1 = 0;
        if (!SafeReadPtr(root + a, &t1)) continue;
        if (!t1 || (t1 & 0x7) || t1 < 0x10000 || t1 > 0x00007FFFFFFFFFFF) continue;
        for (int b = 8; b <= 0x180; b += 8) {
            uintptr_t t2 = 0;
            if (!SafeReadPtr(t1 + b, &t2)) break;
            if (!t2 || (t2 & 0x7) || t2 < 0x10000 || t2 > 0x00007FFFFFFFFFFF) continue;
            uintptr_t v = 0;
            if (!SafeReadPtr(t2, &v)) continue;
            if ((LONGLONG)v == pm) {
                g_pmChainCandA = a;
                g_pmChainCandB = b;
                g_pmChainAgree = 1;
                Log("[PmChain] candidate [global]->+0x%X->+0x%X->deref (1/3)", a, b);
                return;
            }
        }
    }
    static bool s_loggedNoChain = false;
    if (!s_loggedNoChain) {
        s_loggedNoChain = true;
        Log("[PmChain] no static pm chain found near root — hook capture only");
    }
}

// Called from the OpenPanel hook on every game-initiated capture.
static void ValidatePmChain(LONGLONG pm) {
    if (g_pmGlobalAddr == 0 || pm == 0) return;
    if (g_pmOffset2 != 0) {
        // Active chain. Static-scan chains (pre-1.13 builds) are trusted as
        // before; learned/seeded chains must keep matching the live pm.
        if (g_pmChainLearned) {
            LONGLONG v = ReadChainPm(g_pmOffset1, g_pmOffset2);
            if (v == pm) {
                // Live confirmation of the static-derived (or learned) chain.
                if (g_pmChainAgree == 0)
                    Log("[PmChain] chain confirmed against live pm");
                g_pmChainAgree++;
                g_pmChainZeroReads = 0;
            } else if (v != 0) {
                DemotePmChain("chain diverged from live pm");
            } else if (g_pmChainAgree == 0 && ++g_pmChainZeroReads >= 5) {
                // A single null read is no verdict (chain links may not be
                // constructed during loading / UI rebuild). But an UNPROVEN
                // chain that never resolves across 5 captures — while panels
                // are demonstrably opening — is stale (a wrong static match or
                // a post-update layout change). Demote so candidate learning
                // can find the real chain from the live pm.
                DemotePmChain("chain never resolved across 5 captures");
            }
        }
        return;
    }
    if (g_pmChainCandA >= 0) {
        if (ReadChainPm(g_pmChainCandA, g_pmChainCandB) == pm) {
            if (++g_pmChainAgree >= 3) {
                g_pmOffset1 = g_pmChainCandA;
                g_pmOffset2 = g_pmChainCandB;
                g_pmChainLearned = true;
                Log("[PmChain] promoted [global]->+0x%X->+0x%X->deref (3 agreements)",
                    g_pmChainCandA, g_pmChainCandB);
            } else {
                Log("[PmChain] candidate agreed (%d/3)", g_pmChainAgree);
            }
        } else {
            g_pmChainCandA = g_pmChainCandB = -1;
            g_pmChainAgree = 0;
            LearnPmChainCandidate(pm);
        }
        return;
    }
    LearnPmChainCandidate(pm);
}

// ============================================================
//  New quest journal switch
// ============================================================
// The game keeps two quest journals. Every place that opens the journal
// builds the panel name at runtime:
//     name = "QuestMenuPanel";  if (FLAG_BYTE != 0) name = "QuestMenuPanel2";
// (Ghidra 2.01.00: DAT_146b70448, 11 sites.) With the flag set the tab in
// MainMenuView2 is QuestMenuPanel2, so opening "QuestMenuPanel" matches no
// tab and the menu lands on its last tab (usually the Inventory) — which is
// what J did since the new journal shipped. Resolve the flag byte the same
// way: at each LEA of "QuestMenuPanel" the compare  CMP byte [rip+FLAG],0
// (80 3D disp32 00) follows within a few dozen bytes; majority vote.
static void ResolveQuestMenuFlag() {
    // "QuestMenuPanel" is also the tail of "DailyQuestMenuPanel" and
    // "FactionQuestMenuPanel", so walk every occurrence that starts a string
    // (preceded by a NUL) instead of taking the first substring match.
    static const char kName[] = "QuestMenuPanel";
    const int kLen = (int)sizeof(kName) - 1;
    BYTE* base = (BYTE*)g_gameBase;
    uintptr_t cand[8] = {}; int votes[8] = {}; int nc = 0;
    int strings = 0, leas = 0;
    for (DWORD i = 1; i + kLen + 1 < g_imageSize; i++) {
        if (base[i] != 'Q' || base[i - 1] != 0) continue;
        if (memcmp(base + i, kName, kLen + 1) != 0) continue;
        strings++;
        uintptr_t str = g_gameBase + i;
        uintptr_t lea = 0;
        for (int guard = 0; guard < 32; guard++) {
            lea = FindLEA(str, lea ? lea + 1 : 0);
            if (!lea) break;
            leas++;
            for (int k = 7; k < 0x40; k++) {
                BYTE* q = (BYTE*)(lea + k);
                if (q[0] != 0x80 || q[1] != 0x3D || q[6] != 0x00) continue;
                uintptr_t g = (uintptr_t)(q + 7) + *(int32_t*)(q + 2);
                if (g <= g_gameBase || g >= g_gameBase + g_imageSize) continue;
                int c = 0;
                for (; c < nc; c++) if (cand[c] == g) { votes[c]++; break; }
                if (c == nc && nc < 8) { cand[nc] = g; votes[nc] = 1; nc++; }
                break;
            }
        }
    }
    Log("QuestMenuFlag: %d string(s), %d LEA site(s)", strings, leas);
    int best = 0, total = 0; uintptr_t bestG = 0;
    for (int c = 0; c < nc; c++) { total += votes[c]; if (votes[c] > best) { best = votes[c]; bestG = cand[c]; } }
    if (bestG && best >= 2 && best * 2 > total) {
        g_questMenuFlag = bestG;
        BYTE v = 0xFF;
        __try { v = *(BYTE*)bestG; } __except(EXCEPTION_EXECUTE_HANDLER) {}
        Log("QuestMenuFlag: OK base+0x%llX (%d/%d sites agree), value now %u -> %s",
            (unsigned long long)(bestG - g_gameBase), best, total, (unsigned)v,
            v ? "QuestMenuPanel2" : "QuestMenuPanel");
    } else {
        Log("QuestMenuFlag: not resolved (%d candidates, best %d/%d) — J uses the configured name",
            nc, best, total);
    }
}

// The panel name to use for a binding right now. Only the built-in
// QuestBook default follows the game's flag; a PanelName= override in the
// INI is always used as written.
static const char* PanelNameFor(int i) {
    const char* name = g_panels[i].panelName;
    if (g_questMenuFlag && g_panels[i].customPanelName[0] == 0 &&
        strcmp(name, "QuestMenuPanel") == 0) {
        BYTE v = 0;
        __try { v = *(BYTE*)g_questMenuFlag; } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (v) return "QuestMenuPanel2";
    }
    return name;
}

// C detour for OpenPanel — blocks game-triggered opens for remapped panels
static void __fastcall DetourOpenPanel(LONGLONG pm, const char* panelName, void* data) {
    // First-capture tracking — decides whether to let this call through
    // unblocked (so the user's very first keypress actually opens the panel).
    bool wasFirstCapture = (g_panelManager == 0 && pm != 0);

    // Capture PanelManager + panel name (same as old assembly trampoline)
    g_panelManager = pm;
    g_lastRDX = (LONGLONG)panelName;
    InterlockedIncrement(&g_hookCounter);

    // 1.13+: no static pm-chain idiom exists in the binary, so learn/validate
    // one against the live pm (candidate -> 3 agreements -> promoted; demoted
    // on divergence). Skip the mod's own re-entrant calls — their pm may come
    // from the chain itself, which would self-confirm.
    if (!g_ourCall)
        ValidatePmChain(pm);

    // First-hit pass-through: on the very first capture, do NOT block the
    // original call even if the user is holding a game-default key. Otherwise
    // the user's first I-press is swallowed and the inventory doesn't open —
    // they would have to press again. Skip the block logic here and forward.
    if (wasFirstCapture) {
        Log("[FirstCapture] pm=0x%llX via '%s' — passing through unblocked",
            (unsigned long long)pm, panelName);
    } else if (!g_ourCall && g_ready && g_overrideGameKeys) {
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

    // Forward to original function (SEH protected). A crash on a
    // mod-initiated call means WE supplied a bad pm (e.g. a stale seeded
    // chain after a game update) — demote the chain and let the caller
    // clean up, instead of bricking the mod: OpenPanelOnGameThread calls the
    // HOOKED address, so its own __except never sees this exception.
    // A crash on a game-initiated call is genuine trampoline corruption —
    // disable the mod as before.
    typedef void (__fastcall* OrigFunc)(LONGLONG, const char*, void*);
    __try {
        ((OrigFunc)g_openPanelTrampoline)(pm, panelName, data);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (g_ourCall) {
            g_ourCallCrashed = true;
            Log("ERROR: OpenPanel crashed (0x%08X) on mod-supplied pm 0x%llX",
                GetExceptionCode(), (unsigned long long)pm);
            DemotePmChain("mod-initiated OpenPanel crashed");
        } else {
            g_ready = false;
            Log("FATAL: Trampoline crashed (0x%08X) — mod disabled to prevent further crashes",
                GetExceptionCode());
        }
    }
}

// ============================================================
//  Input-block detours (XInputGetState + GetRawInputData)
// ============================================================
//
// When a controller modifier is held, mask out the user-bound panel buttons
// from the data the GAME sees, so the same physical press doesn't trigger
// both the panel hotkey and the default in-game action.
//
// The mod's own polling reads through the trampoline pointers — it always
// sees the unfiltered data, so panel hotkeys keep working.

static DWORD WINAPI DetourXInputGetState(DWORD index, XINPUT_STATE_LOCAL* state) {
    PFN_XInputGetState orig = (PFN_XInputGetState)g_xinputTrampoline;
    DWORD result = orig(index, state);
    if (!g_blockOverlappingInputs) return result;
    if (result != 0 || !state) return result;
    if (g_controllerModifier == 0) return result;

    // Per-call state. The game polls XInput from one thread; the mod's input
    // thread polls through the trampoline (different code path) so this
    // single-writer state needs no atomic.
    static WORD g_xiPrevBtns = 0;

    WORD boundMask = 0;
    for (int i = 0; i < NUM_PANELS; i++)
        boundMask |= g_panels[i].controllerButton;
    boundMask &= ~g_controllerModifier;
    if (boundMask == 0) return result;

    WORD btns    = state->Gamepad.wButtons;
    WORD modNow  = (WORD)(btns          & g_controllerModifier);
    WORD modPrev = (WORD)(g_xiPrevBtns  & g_controllerModifier);

    // Replay-with-delay: bound bits seen by the game come from the PREVIOUS
    // frame, non-bound bits pass through unchanged. This gives the modifier
    // one extra polling tick to register before a chord's panel-button bit
    // would reach the game. If the modifier is held in this OR the previous
    // frame, every bound bit is stripped.
    //
    // Why prev-frame too: catches the release-glitch where the user lifts
    // the modifier a frame before the panel button (game would otherwise
    // see one stray frame of the panel button alone after release).
    //
    // Why a delay at all: catches the press-edge race where the user pushes
    // the panel button a tick before the modifier (game would otherwise see
    // a one-frame edge of the panel button alone, which is enough for
    // edge-triggered actions like the horse whistle to fire).
    WORD outBtns = (WORD)((btns & ~boundMask) | (g_xiPrevBtns & boundMask));
    if (modNow || modPrev) {
        outBtns = (WORD)(outBtns & ~boundMask);
    }

    state->Gamepad.wButtons = outBtns;
    g_xiPrevBtns = btns;
    return result;
}

static UINT WINAPI DetourGetRawInputData(HRAWINPUT hRaw, UINT cmd, LPVOID data,
                                          PUINT pcbSize, UINT cbSizeHeader) {
    PFN_GetRawInputData orig = (PFN_GetRawInputData)g_griTrampoline;
    UINT r = orig(hRaw, cmd, data, pcbSize, cbSizeHeader);
    if (!g_blockOverlappingInputs) return r;
    if (cmd != RID_INPUT || data == nullptr || r == (UINT)-1) return r;
    if (!g_psModifierEnabled || g_psModifierByteOff < 0 || g_psModifierByteOff > 2)
        return r;

    __try {
        RAWINPUT* raw = (RAWINPUT*)data;
        if (raw->header.dwType != RIM_TYPEHID) return r;
        if (raw->data.hid.dwCount == 0 || raw->data.hid.dwSizeHid == 0) return r;

        IdentifyHidDevice(raw->header.hDevice);
        if (!g_cachedIsSony || g_cachedReportOffset < 0) return r;

        BYTE* report = raw->data.hid.bRawData;
        DWORD reportLen = raw->data.hid.dwSizeHid;
        int rOff = g_cachedReportOffset;
        if (reportLen > 40 && report[0] == 0x31)      rOff = 9;   // DualSense BT
        else if (reportLen > 40 && report[0] == 0x11) rOff = 7;   // DualShock 4 BT
        if ((DWORD)rOff >= reportLen) return r;

        // Per-call state. WM_INPUT is normally processed on the game's UI
        // thread; single-writer = no atomic needed.
        static BYTE g_hidPrevBits[3] = {0, 0, 0};
        static BYTE g_hidPrevHat     = 0x08;  // neutral

        // Snapshot raw bytes BEFORE we mutate the report.
        BYTE curBits[3];
        curBits[0] = report[rOff];
        curBits[1] = ((DWORD)(rOff + 1) < reportLen) ? report[rOff + 1] : 0;
        curBits[2] = ((DWORD)(rOff + 2) < reportLen) ? report[rOff + 2] : 0;
        BYTE curHat = (BYTE)(curBits[0] & 0x0F);

        bool modNow  = (curBits[g_psModifierByteOff]      & g_psModifierBitMask) != 0;
        bool modPrev = (g_hidPrevBits[g_psModifierByteOff] & g_psModifierBitMask) != 0;

        // Replay-with-delay on bit-bytes (byteOff 0..2). Each bound bit is
        // replaced with the previous frame's value, then masked off if the
        // modifier is held in current OR previous frame. Same motivation as
        // DetourXInputGetState — see comments there.
        for (int i = 0; i < NUM_PANELS; i++) {
            int o  = g_panels[i].psButtonByteOff;
            BYTE m = g_panels[i].psButtonBitMask;
            if (o < 0 || o > 2) continue;
            if (o == g_psModifierByteOff && (m & g_psModifierBitMask)) continue;
            DWORD idx = (DWORD)(rOff + o);
            if (idx >= reportLen) continue;
            BYTE prevBit = (BYTE)(g_hidPrevBits[o] & m);
            report[idx] = (BYTE)((report[idx] & ~m) | prevBit);
            if (modNow || modPrev) report[idx] &= ~m;
        }

        // D-Pad (hat-switch). The hat is a 4-bit enum, not a bitmask, so the
        // delay applies whenever the current OR previous hat resolves to a
        // bound direction; non-bound directions pass through immediately.
        BYTE boundDpad = 0;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].psButtonByteOff == 3)
                boundDpad |= g_panels[i].psButtonBitMask;
        }
        if (boundDpad) {
            static const BYTE hatToDpad[9] = {
                0x01, 0x09, 0x08, 0x0A, 0x02, 0x06, 0x04, 0x05, 0x00
            };
            BYTE curDir  = (curHat       <= 8) ? hatToDpad[curHat]       : 0;
            BYTE prevDir = (g_hidPrevHat <= 8) ? hatToDpad[g_hidPrevHat] : 0;
            BYTE outHat  = curHat;
            if ((curDir & boundDpad) || (prevDir & boundDpad)) {
                outHat = g_hidPrevHat;  // 1-frame delay
            }
            if (modNow || modPrev) {
                outHat = 0x08;  // neutral — preserves modifier intent across
                                // press-edge and release-glitch frames
            }
            report[rOff] = (BYTE)((report[rOff] & 0xF0) | (outHat & 0x0F));
        }

        // Update prev state with the snapshot (raw, before mutation).
        g_hidPrevBits[0] = curBits[0];
        g_hidPrevBits[1] = curBits[1];
        g_hidPrevBits[2] = curBits[2];
        g_hidPrevHat     = curHat;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// Walk backward from an address inside a function until we hit the INT3
// alignment padding (CC CC CC...) that precedes every MSVC function.
// Returns the address of the byte after the padding (= function start).
static uintptr_t FindFunctionStart(uintptr_t midAddr) {
    if (midAddr <= g_gameBase + 16) return 0;
    BYTE* base = (BYTE*)g_gameBase;
    DWORD a = (DWORD)(midAddr - g_gameBase);
    DWORD limit = (a > 4096) ? (a - 4096) : 16;
    for (DWORD i = a - 1; i > limit; i--) {
        if (base[i] == 0xCC && base[i+1] != 0xCC &&
            i >= 2 && base[i-1] == 0xCC && base[i-2] == 0xCC) {
            return (uintptr_t)(base + i + 1);
        }
    }
    return 0;
}

// Re-derive the mainChar mode/sub/subtype-array offsets from the game's
// ModeSwitcher function (the per-frame mode-string configurator). It is found
// via the unique "ingame-global" string xref and references all mainChar mode
// offsets as disp32 in the 0xC00-0xD00 range: the consecutive pair (X, X+1)
// is mode byte + sub byte, the max disp within +0x20 is the subtype array.
// Same technique as PrivateStorageAnywhere's resolver (verified on 1.13.01).
// On failure the pre-1.13 defaults stay in place.
static bool FindModeOffsets() {
    uintptr_t strIG = FindString("ingame-global");
    if (!strIG) { Log("FindModeOffsets: 'ingame-global' string not found — safety gate cannot evaluate the game state"); return false; }
    uintptr_t leaAddr = FindLEA(strIG);
    if (!leaAddr) { Log("FindModeOffsets: no LEA xref — safety gate cannot evaluate the game state"); return false; }
    uintptr_t fnStart = FindFunctionStart(leaAddr);
    if (!fnStart) { Log("FindModeOffsets: function start not found — safety gate cannot evaluate the game state"); return false; }

    // Collect all unique disp32 values in the mainChar mode-byte range.
    uint8_t* fn = (uint8_t*)fnStart;
    const int scanWindow = 0xA00;
    uint32_t found[64];
    int nFound = 0;
    for (int k = 2; k < scanWindow - 4; k++) {
        uint8_t modrmA = fn[k - 1];
        uint8_t modrmB = fn[k - 2];
        bool caseA = ((modrmA & 0xC0) == 0x80) && ((modrmA & 0x07) != 0x04);
        bool caseB = ((modrmB & 0xC0) == 0x80) && ((modrmB & 0x07) == 0x04);
        if (!caseA && !caseB) continue;
        uint32_t disp = *(uint32_t*)(fn + k);
        if (disp < 0xC00 || disp >= 0xD00) continue;
        bool dup = false;
        for (int j = 0; j < nFound; j++) if (found[j] == disp) { dup = true; break; }
        if (!dup && nFound < 64) found[nFound++] = disp;
    }
    // The consecutive pair (X, X+1) identifies mode byte + sub byte.
    uint32_t modeByte = 0;
    for (int i = 0; i < nFound && !modeByte; i++)
        for (int j = 0; j < nFound; j++)
            if (found[j] == found[i] + 1) { modeByte = found[i]; break; }
    if (!modeByte) { Log("FindModeOffsets: no mode/sub pair in ModeSwitcher — safety gate cannot evaluate the game state"); return false; }
    // Max disp within +0x20 of the mode byte = subtype array base.
    uint32_t maxDisp = 0;
    for (int i = 0; i < nFound; i++)
        if (found[i] > modeByte + 1 && found[i] <= modeByte + 0x20 && found[i] > maxDisp)
            maxDisp = found[i];
    if (!maxDisp) { Log("FindModeOffsets: no subtype array disp — safety gate cannot evaluate the game state"); return false; }

    g_offModeByte = modeByte;
    g_offSubByte  = modeByte + 1;
    g_offSubtypes = maxDisp;
    Log("FindModeOffsets: OK base+0x%llX (mode=0x%X sub=0x%X subtypes=0x%X)",
        (unsigned long long)(fnStart - g_gameBase),
        g_offModeByte, g_offSubByte, g_offSubtypes);
    return true;
}

// Returns trampoline address (for calling original) or 0 on failure.
// Installs a 14-byte JMP-thunk at targetAddr that redirects to detourFn.
// gameImageOnly=true rejects thunks that resolve outside the game image
// (default; matches the original OpenPanel hook contract). Pass false when
// hooking exports in OS DLLs (xinput, user32) — those may legitimately
// thunk-forward outside the game image.
// trampolineOut, if non-null, is written with the trampoline address BEFORE
// the JMP-thunk goes live. Use it for hot-path hooks (XInput / GetRawInputData)
// where the game can call the detour the instant the JMP is installed; reading
// a still-zero trampoline global from inside the detour is undefined behavior.
static uintptr_t InstallHookGeneric(uintptr_t targetAddr, void* detourFn,
                                     bool gameImageOnly = true,
                                     uintptr_t* trampolineOut = nullptr) {
    BYTE* target = (BYTE*)targetAddr;
    BYTE stolenBytes[32];

    // Log first 16 bytes at target so we can identify unexpected prologs/thunks.
    Log("Target base+0x%llX first 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
        (unsigned long long)(targetAddr - g_gameBase),
        target[0], target[1], target[2], target[3],
        target[4], target[5], target[6], target[7],
        target[8], target[9], target[10], target[11],
        target[12], target[13], target[14], target[15]);

    // Resolve FF 25 [rip+disp32] import/relay thunks to the real target.
    // Post Apr-23 2026 game update 1.0.4.1: OpenPanel at 0x140AF9EB0 is only
    // a 6-byte JMP thunk in the static PE. Hooking the thunk would clobber 9
    // bytes of adjacent code and corrupt the trampoline (manifests as
    // 0xC0000005 forwards). Follow thunk chains up to 4 levels.
    // Also handle E9 rel32 near-jump thunks (5 bytes) as belt-and-braces.
    __try {
        for (int hop = 0; hop < 4; hop++) {
            uintptr_t realTarget = 0;
            if (target[0] == 0xFF && target[1] == 0x25) {
                int32_t disp = *(int32_t*)(target + 2);
                uintptr_t ptrLoc = targetAddr + 6 + disp;
                realTarget = *(uintptr_t*)ptrLoc;
            } else if (target[0] == 0xE9) {
                int32_t rel = *(int32_t*)(target + 1);
                realTarget = targetAddr + 5 + rel;
            } else {
                break;
            }
            if (gameImageOnly &&
                (realTarget < g_gameBase ||
                 realTarget >= g_gameBase + g_imageSize)) {
                Log("Thunk at base+0x%llX points outside image (0x%llX) — hook aborted",
                    (unsigned long long)(targetAddr - g_gameBase),
                    (unsigned long long)realTarget);
                return 0;
            }
            Log("Thunk at base+0x%llX -> real target at base+0x%llX",
                (unsigned long long)(targetAddr - g_gameBase),
                (unsigned long long)(realTarget - g_gameBase));
            target = (BYTE*)realTarget;
            targetAddr = realTarget;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: Thunk resolution crashed — hook aborted");
        return 0;
    }

    int stolenLen = 0;
    while (stolenLen < 14) {
        int len = InstrLen(target + stolenLen);
        if (len == 0) {
            Log("ERROR: Unknown instruction at +%d: %02X %02X %02X %02X",
                stolenLen, target[stolenLen], target[stolenLen+1],
                target[stolenLen+2], target[stolenLen+3]);
            return 0;
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
                return 0;
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
        return 0;
    }

    BYTE* p = (BYTE*)trampMem;
    memcpy(p, stolenBytes, stolenLen);
    p += stolenLen;
    *p++ = 0xFF; *p++ = 0x25;
    *(uint32_t*)p = 0; p += 4;
    *(uintptr_t*)p = targetAddr + stolenLen;

    // Lock trampoline to execute-only (no longer needs write access)
    DWORD trampOldProt;
    VirtualProtect(trampMem, 64, PAGE_EXECUTE_READ, &trampOldProt);

    // Publish trampoline to caller-supplied global BEFORE patching the target.
    // Once the JMP-thunk goes live the detour can be invoked immediately on
    // any thread; reading a still-zero trampoline global there would crash.
    if (trampolineOut) *trampolineOut = (uintptr_t)trampMem;

    // --- Patch original: JMP to detourFn ---
    DWORD oldProt;
    VirtualProtect((void*)targetAddr, stolenLen, PAGE_EXECUTE_READWRITE, &oldProt);

    BYTE* h = (BYTE*)targetAddr;
    *h++ = 0xFF; *h++ = 0x25;
    *(uint32_t*)h = 0; h += 4;
    *(uintptr_t*)h = (uintptr_t)detourFn; h += 8;
    while (h < target + stolenLen) *h++ = 0x90;

    VirtualProtect((void*)targetAddr, stolenLen, oldProt, &oldProt);
    Log("Hook installed at base+0x%llX -> 0x%p, trampoline at 0x%p",
        (unsigned long long)(targetAddr - g_gameBase), detourFn, trampMem);
    return (uintptr_t)trampMem;
}

// ============================================================
//  PanelManager seeding via FindPanelTop
// ============================================================
// Until some panel is opened, g_panelManager stays 0 and every hotkey is a
// no-op — the user has to open one vanilla panel first to prime the mod.
// Before 1.13 pm came from a static load idiom; that idiom is gone, and on
// 2.01.00 callers fetch pm through a per-object virtual getter (vtable+0x110)
// hanging off the caller's own `this`, which no byte pattern can follow.
//
// FindPanelTop(pm, name) takes pm as its FIRST argument and is called from 226
// sites across the binary — including ordinary UI work that runs long before
// the user touches a panel. Hooking it purely to read RCX seeds g_panelManager
// within the first frames and changes no behaviour.
//
// The candidate is validated against the very array FindPanelTop itself walks,
// so a wrong or stale RCX is never adopted. Both offsets are read out of
// FindPanelTop's own prolog rather than hardcoded (2.01.00: entries +0x30EB8,
// count +0x30EC0; 1.13 had them 0x10 higher).

// FindPanelTop opens with two  MOV reg,[RCX+disp32]  loads: the entry array and
// the entry count. Pick the first two such loads with a struct-sized
// displacement, in address order.
static bool DeriveFindPanelTopOffsets() {
    if (!g_findPanelTopAddr) return false;
    const BYTE* fn = (const BYTE*)g_findPanelTopAddr;
    uint32_t found[2] = {0, 0};
    int n = 0;
    for (int i = 0; i < 0x40 && n < 2; i++) {
        // 8B /r with mod=10, rm=001 (RCX base, disp32); optional REX.W/R prefix.
        int k = i;
        if (fn[k] == 0x48 || fn[k] == 0x4C || fn[k] == 0x49 || fn[k] == 0x4D) k++;
        if (fn[k] != 0x8B) continue;
        if ((fn[k + 1] & 0xC7) != 0x81) continue;
        uint32_t disp = *(const uint32_t*)(fn + k + 2);
        if (disp < 0x1000 || disp > 0x100000) continue;
        found[n++] = disp;
        i = k + 5;
    }
    if (n < 2) {
        Log("PmSeed: FindPanelTop prolog offsets not derivable (%d found) — seeding disabled", n);
        return false;
    }
    g_pmArrayOff = found[0];
    g_pmCountOff = found[1];
    Log("PmSeed: pm entry array at +0x%X, count at +0x%X (from FindPanelTop prolog)",
        g_pmArrayOff, g_pmCountOff);
    return true;
}

static void TrySeedPanelManager(LONGLONG pm) {
    if (pm == 0 || g_pmArrayOff == 0) return;
    __try {
        uintptr_t entries = *(uintptr_t*)(pm + g_pmArrayOff);
        uint32_t  count   = *(uint32_t*)(pm + g_pmCountOff);
        // A real PanelManager has a heap-allocated entry array and a sane count.
        if (entries <= 0x10000 || count == 0 || count > 0x1000) return;
        if (InterlockedCompareExchange64((volatile LONG64*)&g_panelManager,
                                         (LONG64)pm, 0) == 0) {
            Log("[PmSeed] PanelManager 0x%llX captured from FindPanelTop "
                "(entries=0x%llX count=%u) — hotkeys live without a vanilla panel first",
                (unsigned long long)pm, (unsigned long long)entries, count);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// Pass all four register arguments through untouched — the exact arity of
// FindPanelTop is not certain, and clobbering R8/R9 would corrupt the call.
typedef LONGLONG (__fastcall *FindPanelTopFn)(LONGLONG, LONGLONG, LONGLONG, LONGLONG);

static LONGLONG __fastcall DetourFindPanelTop(LONGLONG pm, LONGLONG a2,
                                              LONGLONG a3, LONGLONG a4) {
    // One predictable-branch compare once pm is known; this is a hot path.
    if (InterlockedCompareExchange64((volatile LONG64*)&g_panelManager, 0, 0) == 0)
        TrySeedPanelManager(pm);
    return ((FindPanelTopFn)g_findPanelTopTramp)(pm, a2, a3, a4);
}

static bool InstallPmSeedHook() {
    if (!g_findPanelTopAddr) return false;
    if (!DeriveFindPanelTopOffsets()) return false;
    uintptr_t tramp = InstallHookGeneric(g_findPanelTopAddr,
                                         (void*)&DetourFindPanelTop,
                                         /*gameImageOnly=*/true,
                                         &g_findPanelTopTramp);
    if (!tramp) {
        Log("PmSeed: hook on FindPanelTop FAILED — falling back to first-panel capture");
        return false;
    }
    Log("PmSeed: FindPanelTop hook installed at base+0x%llX",
        (unsigned long long)(g_findPanelTopAddr - g_gameBase));
    return true;
}

// Backwards-compatible wrapper for the OpenPanel hook (writes to the
// existing trampoline global so callers in the OpenPanel detour path keep
// working unchanged).
static bool InstallHook(uintptr_t targetAddr) {
    g_openPanelTrampoline = InstallHookGeneric(targetAddr, (void*)&DetourOpenPanel);
    return g_openPanelTrampoline != 0;
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

    typedef LONGLONG (__fastcall* FindPanelFunc)(LONGLONG, const char*);
    typedef void (__fastcall* OpenPanelFunc)(LONGLONG, const char*, const char*);
    typedef void (__fastcall* SetSubTabFunc)(uintptr_t, uint32_t, uint32_t, uint32_t);

    OpenPanelFunc openPanel = (OpenPanelFunc)g_openPanelAddr;

    const char* panelName = PanelNameFor(panelIndex);
    if (!panelName || panelName[0] == '\0') {
        Log("ERROR: [%s] has no PanelName — set it in the INI to enable this hotkey",
            g_panels[panelIndex].section);
        return;
    }

    // OpenPanel's 3rd parameter is a direct C-string (the execute-event)
    // verified from the decompile of FUN_140abf8e0:
    //   if ((param_3 == NULL) || (*param_3 == '\0'))
    //       param_3 = lookup_panel_default_event(panelName)   // -> "SpecialVehicle" for PetView
    //   else
    //       process_event_string(param_3)
    // Then OpenPanel iterates the panel's sub-tab list and matches param_3
    // against each sub-tab's execute-event (at +0x200). Match -> sets that tab.
    //
    // Empty execute-event ("") can NOT be reached via param_3 because:
    //   - param_3 = NULL -> default-substitution -> always "SpecialVehicle"
    //   - param_3 = ""   -> same path (early-out treats "" as NULL)
    //   - Any non-empty param_3 -> only matches sub-tabs with that exact string
    // The HTML tabbar-click bypasses OpenPanel and calls SetSubTab directly.
    // We replicate that here: after OpenPanel, do a manual SetSubTab(petsIdx).
    const char* execEventArg = nullptr;
    if (g_panels[panelIndex].hasExecuteEvent &&
        g_panels[panelIndex].executeEvent[0] != '\0') {
        // Non-empty event -> pass it directly so OpenPanel matches that sub-tab
        execEventArg = g_panels[panelIndex].executeEvent;
        Log("Opening panel: %s (index %d, execute-event=\"%s\")",
            panelName, panelIndex, execEventArg);
    } else if (g_panels[panelIndex].hasExecuteEvent) {
        Log("Opening panel: %s (index %d, execute-event=\"\" -> post-switch to empty-event sub-tab)",
            panelName, panelIndex);
    } else {
        Log("Opening panel: %s (index %d)", panelName, panelIndex);
    }

    g_ourCallCrashed = false;
    __try {
        g_ourCall = true;
        openPanel(pm, panelName, execEventArg);
        g_ourCall = false;
        g_currentPanel = panelIndex;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_ourCall = false;
        Log("ERROR: Panel call crashed (0x%08X) for '%s'",
            GetExceptionCode(), panelName);
        // A learned chain that fed this pm may be a stale false positive —
        // drop it so the next press falls back to hook capture instead of
        // re-reading the same bad chain in a crash loop.
        DemotePmChain("panel call crashed");
        g_panelManager = 0;
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
        return;
    }

    // openPanel goes through the HOOKED address, so a crash inside the game's
    // OpenPanel body is caught by the detour's SEH (which demotes the chain
    // and sets this flag), not by the __except above. Clean up here.
    if (g_ourCallCrashed) {
        g_ourCallCrashed = false;
        g_panelManager = 0;
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
        return;
    }

    Log("Post-open: hasExecuteEvent=%d executeEvent[0]=0x%02X panelName='%s'",
        g_panels[panelIndex].hasExecuteEvent ? 1 : 0,
        (unsigned char)g_panels[panelIndex].executeEvent[0],
        panelName);

    // Post-open sub-tab override: when the user explicitly asks for an empty
    // execute-event (Pets tab on PetView), OpenPanel cannot land on that tab
    // via its parameter API. We find the sub-tab whose execute-event is "" and
    // matches our panelName, then call the game's SetSubTab function directly.
    if (!g_panels[panelIndex].hasExecuteEvent ||
        g_panels[panelIndex].executeEvent[0] != '\0') {
        Log("Post-open: skipping sub-tab switch (no empty-event override requested)");
        return;
    }

    // Both addresses are scanned dynamically from OpenPanel's body at init.
    // If the scanner couldn't resolve either, skip the switch (panel still
    // opens, just at the game-default sub-tab).
    if (g_findPanelTopAddr == 0 || g_setSubTabAddr == 0) {
        Log("Post-open: skipping sub-tab switch — helpers unresolved (findPanelTop=%p setSubTab=%p)",
            (void*)g_findPanelTopAddr, (void*)g_setSubTabAddr);
        return;
    }

    Log("Post-open: starting sub-tab switch for '%s'", panelName);

    typedef LONGLONG (__fastcall* FindPanelTopFunc)(LONGLONG, const char*);
    FindPanelTopFunc findPanelTop = (FindPanelTopFunc)g_findPanelTopAddr;
    SetSubTabFunc    setSubTab    = (SetSubTabFunc)   g_setSubTabAddr;
    Log("Post-open: findPanelTop=%p setSubTab=%p", (void*)findPanelTop, (void*)setSubTab);

    __try {
        LONGLONG mainMenu = findPanelTop(pm, "MainMenuView2");
        Log("Sub-tab: findPanelTop returned mainMenu=0x%llX", (unsigned long long)mainMenu);
        if (mainMenu == 0) {
            Log("Sub-tab switch: MainMenuView2 not found");
            return;
        }
        uint32_t primaryTab  = *(uint32_t*)(mainMenu + 0x1a8);
        uintptr_t panelsList = *(uintptr_t*)(mainMenu + 0x128);
        Log("Sub-tab: primaryTab=%u panelsList=0x%llX",
            primaryTab, (unsigned long long)panelsList);
        if (!panelsList) { Log("Sub-tab: panelsList null"); return; }
        uintptr_t topTabBase = panelsList + (uintptr_t)primaryTab * 0x38;
        uintptr_t subContainer = *(uintptr_t*)(topTabBase + 0x30);
        Log("Sub-tab: topTabBase=0x%llX subContainer=0x%llX",
            (unsigned long long)topTabBase, (unsigned long long)subContainer);
        if (!subContainer) { Log("Sub-tab: subContainer null"); return; }

        uintptr_t subList  = *(uintptr_t*)(subContainer + 0xc0);
        uint32_t  subCount = *(uint32_t*) (subContainer + 0xc8);
        Log("Sub-tab: subList=0x%llX subCount=%u",
            (unsigned long long)subList, subCount);
        if (!subList || subCount == 0) { Log("Sub-tab: subList null or empty"); return; }

        // Same dual name-resolution as FUN_140abf8e0:
        //   if state+0xa8 set AND *(state+0xa8 + 8) set -> vtable lookup
        //     name = FUN_14354e240(*(state+0x60))   (g_gameBase + 0x354e240)
        //   else                                    -> name = *(state+0x180)
        typedef const char* (__fastcall* GetNameVtFunc)(LONGLONG);
        GetNameVtFunc getNameVt = (GetNameVtFunc)(g_gameBase + 0x354e240);

        int targetIdx = -1;
        Log("Sub-tab: entering loop, subCount=%u", subCount);
        for (uint32_t j = 0; j < subCount && j < 64; j++) {
            // Layout (from OpenPanel decompile):
            //   subList[j] (8 bytes) -> intermediate ptr -> entry struct
            // Two derefs needed before reading the entry fields.
            uintptr_t indirect = *(uintptr_t*)(subList + j * 8);
            Log("Sub-tab: j=%u indirect=0x%llX", j, (unsigned long long)indirect);
            if (!indirect) continue;
            uintptr_t subEntry = *(uintptr_t*)indirect;
            if (!subEntry) continue;
            uintptr_t subState = *(uintptr_t*)(subEntry + 0xe0);
            if (!subState) continue;

            const char* subName = nullptr;
            uintptr_t a8 = *(uintptr_t*)(subState + 0xa8);
            bool useVt = (a8 != 0 && *(uintptr_t*)(a8 + 8) != 0);
            if (useVt) {
                LONGLONG arg = *(LONGLONG*)(subState + 0x60);
                subName = getNameVt(arg);
            } else {
                subName = *(const char**)(subState + 0x180);
            }
            if (!subName) continue;
            if (strcmp(subName, panelName) != 0) continue;

            // Resolve execute-event string at offset +0x200 on the sub-entry
            const char** evtPP = *(const char***)(subEntry + 0x200);
            if (!evtPP) continue;
            const char* evt = *evtPP;
            if (!evt) continue;
            Log("Sub-tab scan: j=%u name='%s' event='%s'", j, subName, evt);
            if (evt[0] == '\0') {
                targetIdx = (int)j;
                break;
            }
        }

        if (targetIdx < 0) {
            Log("Sub-tab switch: no sub-tab with name='%s' and execute-event=\"\" found",
                panelName);
            return;
        }

        *(int*)(mainMenu + 0x1ac) = targetIdx;
        setSubTab(subContainer, (uint32_t)targetIdx, 0, 0);
        Log("Sub-tab switch: '%s' -> empty-event tab at index %d",
            panelName, targetIdx);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("Sub-tab switch: exception 0x%08X — leaving default tab",
            GetExceptionCode());
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
    if (msg == WM_CLOSE_PANEL) {
        CloseMenuNow();
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
static DWORD g_lastOpenTime   = 0;  // when the mod last sent an open (see OPEN_GRACE_MS)
// Right after an open the panel's view state is still in its opening
// transition and the game's own open-test does not report "open" yet. A
// second press inside this window is therefore a toggle-close, not a
// "closed outside the mod" re-open (seen on 2.02.00: J pressed 0.7 s after
// opening reported "not on screen" and re-opened instead of closing).
static const DWORD OPEN_GRACE_MS = 1000;
static const DWORD PANEL_COOLDOWN_MS = 400;  // 400ms between actions

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

    if (IsModalActive()) {
        Log("[Safety] Blocked: a text-input modal is open");
        return false;
    }

    g_lastActionTime = now;

    if (g_panelManager == 0) {
        Log("ERROR: PanelManager not available yet.");
        return false;
    }

    // Before a toggle-close, ask the game whether the panel is really still on
    // screen (the player may have closed it with ESC behind the mod's back).
    // The per-panel view state is preferred; the old global flag array is only
    // consulted when that is unavailable. With neither available the mod's own
    // toggle state is trusted — reporting "closed" for "cannot tell" would make
    // every press an open.
    bool believedOpenButIsNot = false;
    if (g_currentPanel == i && now - g_lastOpenTime >= OPEN_GRACE_MS) {
        if (PanelOpenCheckAvailable())
            believedOpenButIsNot = !IsPanelOpenByName(PanelNameFor(i));
        else if (PanelFlagsAvailable())
            believedOpenButIsNot = !IsPanelFlagSet();
    }
    if (believedOpenButIsNot) {
        Log("Panel '%s' is not on screen any more (closed outside the mod) — opening fresh",
            PanelNameFor(i));
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
    }

    if (g_currentPanel == i) {
        Log("Closing panel: %s (toggle)", PanelNameFor(i));
        g_currentPanel = -1;
        g_panelConfirmedOpen = false;
        PostMessageA(g_gameWindow, WM_CLOSE_PANEL, (WPARAM)i, 0);
    } else {
        g_lastOpenTime = now;
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

        // Modal close detection: two paths.
        //  (1) Keyboard close (ESC / Enter): edge-detected here for instant
        //      reaction (~16 ms latency).
        //  (2) Mouse close (clicking Confirm/Cancel): no key edge fires, so
        //      we poll the modal object's liveness via ProbeModalAlive().
        if (g_textModalActive) {
            static bool g_escWasDown   = false;
            static bool g_enterWasDown = false;
            bool escDown   = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            bool enterDown = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
            if (escDown && !g_escWasDown) {
                g_escWasDown = true;
                g_textModalActive = false;
                g_modalObj = 0;
                g_modalVftable = 0;
                Log("[ModalHook] Cleared by ESC");
            } else if (!escDown) {
                g_escWasDown = false;
            }
            if (enterDown && !g_enterWasDown) {
                g_enterWasDown = true;
                g_textModalActive = false;
                g_modalObj = 0;
                g_modalVftable = 0;
                Log("[ModalHook] Cleared by Enter");
            } else if (!enterDown) {
                g_enterWasDown = false;
            }

            // Mouse-close path: probe object every poll. ~16ms latency.
            if (g_textModalActive && !ProbeModalAlive()) {
                g_textModalActive = false;
                g_modalObj = 0;
                g_modalVftable = 0;
                Log("[ModalHook] Cleared (object no longer alive — mouse close?)");
            }
        }

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

    // --- Install input-block hooks (XInputGetState + GetRawInputData) ---
    // Goal: when a controller modifier is configured AND held, prevent the
    // game from seeing the same press the mod is consuming for a panel hotkey.
    // The mod's polling reads through the returned trampolines so it always
    // gets the unfiltered original data.
    if (g_blockOverlappingInputs) {
        if (g_controllerEnabled && g_pXInputGetState) {
            uintptr_t origAddr = (uintptr_t)g_pXInputGetState;
            uintptr_t tramp = InstallHookGeneric(origAddr,
                                                  (void*)&DetourXInputGetState,
                                                  /*gameImageOnly=*/false,
                                                  &g_xinputTrampoline);
            if (tramp) {
                g_pXInputGetState = (PFN_XInputGetState)tramp;
                Log("XInputGetState hook installed (orig=0x%p, tramp=0x%p)",
                    (void*)origAddr, (void*)tramp);
            } else {
                Log("WARNING: XInputGetState hook FAILED — overlapping inputs not blocked");
            }
        }

        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            uintptr_t origAddr = (uintptr_t)GetProcAddress(hUser32, "GetRawInputData");
            if (origAddr) {
                uintptr_t tramp = InstallHookGeneric(origAddr,
                                                     (void*)&DetourGetRawInputData,
                                                     /*gameImageOnly=*/false,
                                                     &g_griTrampoline);
                if (tramp) {
                    g_pGetRawInputData = (PFN_GetRawInputData)tramp;
                    Log("GetRawInputData hook installed (orig=0x%p, tramp=0x%p)",
                        (void*)origAddr, (void*)tramp);
                } else {
                    Log("WARNING: GetRawInputData hook FAILED — overlapping HID inputs not blocked");
                }
            }
        }
    } else {
        Log("BlockOverlappingInputs=0 — input-block hooks skipped");
    }

    Log("=== Quick Menu Hotkeys v1.11.0 ===");

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

    // Scan OpenPanel's body for FindPanelTop + SetSubTab. Non-fatal: if
    // either is missing, the sub-tab post-switch is skipped (panel still
    // opens at the game-default tab).
    FindSubTabHelpers();
    ResolveQuestMenuFlag();

    // Install hook on OpenPanel (fallback PanelManager capture)
    if (!InstallHook(g_openPanelAddr)) {
        Log("ERROR: Hook installation failed!");
        return 0;
    }

    // Recover the game's own open-test and menu-close call (replaces the
    // menu-state flag array, unresolvable since 1.13).
    ResolveMenuClose();

    // Seed the PanelManager from FindPanelTop so the first hotkey works without
    // the user having to open a vanilla panel first. Non-fatal: on failure the
    // OpenPanel hook above still captures pm on the first panel interaction.
    InstallPmSeedHook();

    // Install hook on TextEditModalMessage opener (FUN_140b85bf0 in 1.0.4.1).
    // Identified by the unique LEA xref to the literal "\"TextEditModalMessage\""
    // string. Failure is non-fatal — just disables the modal gate and logs.
    {
        uintptr_t modalStrAddr = FindString("TextEditModalMessage");
        if (!modalStrAddr) {
            Log("WARNING: 'TextEditModalMessage' string not found — modal gate disabled");
            g_modalGate = false;
        } else {
            uintptr_t modalLea = FindLEA(modalStrAddr);
            if (!modalLea) {
                Log("WARNING: LEA to TextEditModalMessage not found — modal gate disabled");
                g_modalGate = false;
            } else {
                uintptr_t modalFn = FindFunctionStart(modalLea);
                if (!modalFn) {
                    Log("WARNING: function start for modal opener not found — modal gate disabled");
                    g_modalGate = false;
                } else {
                    Log("TextEditModalMessage opener resolved at base+0x%llX (LEA at base+0x%llX)",
                        (unsigned long long)(modalFn - g_gameBase),
                        (unsigned long long)(modalLea - g_gameBase));
                    g_modalTrampoline = InstallHookGeneric(modalFn, (void*)&DetourTextEditModal);
                    if (!g_modalTrampoline) {
                        Log("WARNING: modal hook install failed — modal gate disabled");
                        g_modalGate = false;
                    }
                }
            }
        }
    }

    // Find menu state flag offsets (for toggle-close) + global/mc chain.
    FindMenuStateFlagOffsets();

    // Re-derive the mainChar mode/sub/subtype offsets (they shift between
    // game updates; hardcoded 0xCA9 made the safety gate block every hotkey
    // on 1.13). Fallback if the ModeSwitcher scan fails: the two flag slots
    // FindMenuStateFlagOffsets scans independently sit at stable indices 9
    // and 0xD of the subtype array, and the sub-mode byte sits 0xF below the
    // array base (both verified on 1.12: 0xCC1/0xCC5 -> 0xCB8/0xCA9, and
    // 1.13: 0xCB9/0xCBD -> 0xCB0/0xCA1) — a second, independent derivation.
    if (!FindModeOffsets() && g_flagOffset1 != 0 && g_flagOffset2 != 0) {
        uint32_t lo = (uint32_t)((g_flagOffset1 < g_flagOffset2)
                                 ? g_flagOffset1 : g_flagOffset2);
        if (lo >= 0xC10 && lo < 0xE00) {
            g_offSubtypes = lo - 9;
            g_offSubByte  = g_offSubtypes - 0xF;
            g_offModeByte = g_offSubByte - 1;
            Log("FindModeOffsets: FALLBACK from flag offsets (mode=0x%X sub=0x%X subtypes=0x%X)",
                g_offModeByte, g_offSubByte, g_offSubtypes);
        }
    }
    // Cross-check: the scanned flag offsets must live inside the subtype
    // array, otherwise one of the two scans went stale.
    if (g_flagOffset1 != 0 &&
        ((uint32_t)g_flagOffset1 < g_offSubtypes ||
         (uint32_t)g_flagOffset1 >= g_offSubtypes + 16)) {
        Log("WARNING: flag offset 0x%X outside subtype array 0x%X..+0x10 — "
            "mode offsets may be stale", g_flagOffset1, g_offSubtypes);
    }

    // Resolve the PanelManager global pointer chain proactively from the
    // binary. Pre-1.13: the direct load idiom (FindPanelManagerChain). 1.13+:
    // that idiom is gone, so derive the chain statically from FindPanelTop's
    // call-site anchor instead (FindPmChainStatic). Either way the offsets are
    // read fresh from the binary every boot — no persistence — so hotkeys
    // (including game-less keys like O) work right after loading, and a future
    // update that shifts the offsets is picked up automatically on next launch.
    // If both static paths fail, the OpenPanel hook still learns the chain
    // from the first live capture (ValidatePmChain), so nothing is lost.
    FindPanelManagerChain();
    if (g_pmOffset2 == 0 && g_pmGlobalAddr != 0)
        FindPmChainStatic();

    // If the chain resolved pm immediately, log it. Otherwise InputThread
    // will call ReadPanelManagerFromGlobal() until root is populated.
    if (g_panelManager != 0)
        Log("PanelManager resolved from global: 0x%llX",
            (unsigned long long)g_panelManager);

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
    Log("BlockOverlappingInputs: %s%s",
        g_blockOverlappingInputs ? "ON" : "OFF",
        (g_blockOverlappingInputs && (g_xinputTrampoline || g_griTrampoline))
            ? "" : " (no input hooks active)");
    if (g_controllerEnabled && g_pXInputGetState) {
        Log("--- Controller Bindings ---");
        if (g_controllerModifier != 0) {
            Log("  Modifier: 0x%04X", g_controllerModifier);
            if (g_blockOverlappingInputs && g_xinputTrampoline) {
                Log("  Block: while modifier is held, bound panel buttons are masked");
                Log("         from the game (modifier itself is preserved).");
            } else {
                Log("  NOTE: BlockOverlappingInputs is OFF or hook failed — pressing");
                Log("        the modifier+button combo will trigger BOTH the game");
                Log("        action and the mod hotkey.");
            }
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
            if (g_psModifierEnabled) {
                Log("  PSModifier: byteOff=%d mask=0x%02X", g_psModifierByteOff, g_psModifierBitMask);
                if (g_blockOverlappingInputs && g_griTrampoline) {
                    Log("  Block: while PSModifier is held, bound PS buttons are masked");
                    Log("         from the game's HID reports (modifier itself preserved).");
                }
            }
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
