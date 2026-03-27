#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <string>

// ============================================================
//  Quick Menu Hotkeys v1.6 — Cross-Reference Pattern Scanner
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

#define WM_OPEN_PANEL (WM_USER + 501)

// ============================================================
//  Config
// ============================================================

struct PanelBinding {
    const char* section;
    const char* panelName;
    DWORD       defaultKey;
    DWORD       gameDefaultKey;  // Game-internal hardcoded key (0 = none)
    DWORD       key;
    WORD        controllerButton;  // XInput button bitmask (0 = disabled)
};

// Verified panels — default keybinds for core menus, 0x00 = user-configurable
//                                                          default  gameKey  key  ctrl
static PanelBinding g_panels[] = {
    // --- Core panels (default keybinds) ---
    { "Inventory",          "InventoryEquipmentPanel",          0x49, 0x49, 0, 0 },  // I
    { "QuestBook",          "QuestMenuPanel",                   0x4A, 0x4A, 0, 0 },  // J
    { "SkillBook",          "SkillTreePanel",                   0x4B, 0x4B, 0, 0 },  // K
    { "Knowledge",          "KnowledgePanel2",                  0x4C, 0x00, 0, 0 },  // L
    { "Options",            "LogoutView",                       0x4F, 0x00, 0, 0 },  // O
    { "Map",                "WorldMapView",                     0x4D, 0x4D, 0, 0 },  // M
    // --- Extra panels (no default keybind — configure in INI) ---
    { "Challenge",          "ChallengeMenuPanel2",              0x00, 0x00, 0, 0 },
    { "FactionQuest",       "FactionQuestMenuPanel",            0x00, 0x00, 0, 0 },
    { "Guides",             "PlayGuideView",                    0x00, 0x00, 0, 0 },
    { "Notifications",      "AlertHistoryView",                 0x00, 0x00, 0, 0 },
};

static const int NUM_PANELS = sizeof(g_panels) / sizeof(g_panels[0]);

static bool g_enabled  = true;
static bool g_debugLog = true;
static bool g_overrideGameKeys = true;

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

// --- Logging ---
static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

// --- INI ---
static DWORD ReadHexValue(const char* section, const char* key,
                           DWORD defaultVal, const char* iniPath) {
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), iniPath);
    if (buf[0] == '\0') return defaultVal;
    return (DWORD)strtoul(buf, nullptr, 16);
}

static void LoadConfig(const char* iniPath) {
    g_enabled  = GetPrivateProfileIntA("Settings", "Enabled",  1, iniPath) != 0;
    g_debugLog = GetPrivateProfileIntA("Settings", "DebugLog", 1, iniPath) != 0;
    g_overrideGameKeys = GetPrivateProfileIntA("Settings", "OverrideGameKeys", 1, iniPath) != 0;
    g_controllerEnabled = GetPrivateProfileIntA("Settings", "ControllerEnabled", 1, iniPath) != 0;
    g_controllerModifier = (WORD)ReadHexValue("Settings", "ControllerModifier", 0, iniPath);

    for (int i = 0; i < NUM_PANELS; i++) {
        g_panels[i].key = ReadHexValue(g_panels[i].section, "Hotkey",
                                        g_panels[i].defaultKey, iniPath);
        g_panels[i].controllerButton = (WORD)ReadHexValue(
            g_panels[i].section, "ControllerButton", 0, iniPath);
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

static void ClearMenuStateFlags() {
    if (g_pmGlobalAddr != 0) {
        __try {
            uintptr_t root = *(uintptr_t*)g_pmGlobalAddr;
            if (root) {
                uintptr_t uiCtrl = *(uintptr_t*)(root + 0x48);
                if (uiCtrl) {
                    *(uint8_t*)(uiCtrl + 0xcc4) = 0;
                    *(uint8_t*)(uiCtrl + 0xcbc) = 0;
                    Log("Menu state flags cleared (0xcc4/0xcbc)");
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("WARNING: Failed to clear menu flags");
        }
    }
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

    // Block game-triggered opens for panels the user has remapped
    if (!g_ourCall && g_overrideGameKeys) {
        __try {
            for (int i = 0; i < NUM_PANELS; i++) {
                if (g_panels[i].gameDefaultKey != 0 &&
                    g_panels[i].key != 0 &&
                    g_panels[i].key != g_panels[i].gameDefaultKey &&
                    strcmp(panelName, g_panels[i].panelName) == 0) {
                    Log("[Blocked] Game tried to open '%s' (remapped to 0x%02X)",
                        panelName, g_panels[i].key);
                    return;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Forward to original function
    typedef void (__fastcall* OrigFunc)(LONGLONG, const char*, void*);
    ((OrigFunc)g_openPanelTrampoline)(pm, panelName, data);
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
//  Sub-Panel Navigation (for panels inside ESC menu)
// ============================================================

// ============================================================
//  Open Panel
// ============================================================

static void OpenPanelOnGameThread(int panelIndex) {
    if (panelIndex < 0 || panelIndex >= NUM_PANELS) return;

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
    void* dataPtr = g_r8DataAddr ? (void*)g_r8DataAddr : nullptr;

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
    // Reset panel tracking when user closes a menu with ESC
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        g_currentPanel = -1;
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
static DWORD g_lastActionTime = 0;  // cooldown between panel switches
static const DWORD PANEL_COOLDOWN_MS = 400;  // 400ms between actions

// Helper: handle panel action (toggle close or open) — shared by keyboard and controller
static bool HandlePanelAction(int i) {
    DWORD now = GetTickCount();
    if (now - g_lastActionTime < PANEL_COOLDOWN_MS) return false;
    g_lastActionTime = now;

    if (g_panelManager == 0) {
        Log("ERROR: PanelManager not available yet.");
        return false;
    }

    if (g_currentPanel == i) {
        Log("Closing panel: %s (toggle)", g_panels[i].panelName);
        g_currentPanel = -1;
        ClearMenuStateFlags();
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

        // --- Keyboard polling ---
        bool actionTaken = false;
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].key == 0x00) continue;

            bool isDown = (GetAsyncKeyState(g_panels[i].key) & 0x8000) != 0;
            if (isDown && !g_keyWasDown[i]) {
                g_keyWasDown[i] = true;
                HandlePanelAction(i);
                actionTaken = true;
                break;
            } else if (!isDown) {
                g_keyWasDown[i] = false;
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
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

// ============================================================
//  Find Game Window
// ============================================================

struct FindWindowData { DWORD processId; HWND result; };

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    FindWindowData* data = (FindWindowData*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->processId && IsWindowVisible(hwnd)) {
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));
        if (title[0] != '\0') {
            data->result = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindGameWindow() {
    FindWindowData data;
    data.processId = GetCurrentProcessId();
    data.result = nullptr;
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    return data.result;
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

    Sleep(10000);

    char dllPath[MAX_PATH];
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
    std::string iniPath(dllPath);
    size_t dot = iniPath.rfind('.');
    if (dot != std::string::npos) iniPath = iniPath.substr(0, dot);
    iniPath += ".ini";

    LoadConfig(iniPath.c_str());
    if (!g_enabled) return 0;

    if (g_controllerEnabled) InitXInput();

    if (g_debugLog) {
        std::string logPath = iniPath.substr(0, iniPath.rfind('.')) + ".log";
        g_logFile = fopen(logPath.c_str(), "w");
    }

    Log("=== Quick Menu Hotkeys v1.6 ===");

    g_gameBase = (uintptr_t)GetModuleHandleA("CrimsonDesert.exe");
    if (!g_gameBase) { Log("ERROR: CrimsonDesert.exe not found"); return 0; }

    MODULEINFO mi;
    GetModuleInformation(GetCurrentProcess(), (HMODULE)g_gameBase, &mi, sizeof(mi));
    g_imageSize = mi.SizeOfImage;

    Log("Game base: 0x%llX  Size: 0x%X", (unsigned long long)g_gameBase, g_imageSize);

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

    // Window subclassing
    g_originalWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWindow, GWLP_WNDPROC,
                                                     (LONG_PTR)HookedWndProc);
    if (!g_originalWndProc) {
        Log("ERROR: Window subclassing failed!");
        return 0;
    }

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
        if (g_controllerModifier != 0)
            Log("  Modifier: 0x%04X", g_controllerModifier);
        for (int i = 0; i < NUM_PANELS; i++) {
            if (g_panels[i].controllerButton != 0)
                Log("  [%s] %s = 0x%04X", g_panels[i].section,
                    g_panels[i].panelName, g_panels[i].controllerButton);
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
