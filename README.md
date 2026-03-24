# Quick Menu Hotkeys

Source code for my Quick Menu Hotkeys mod for Crimson Desert.

This repository is public so anyone can review the source code or compile the mod manually.

## Build

Requires:
- Visual Studio or Visual C++ Build Tools
- Windows SDK
- x64 Developer Command Prompt for Visual Studio

Build command:
```bat
cl /O2 /LD /EHsc src\dllmain.cpp user32.lib psapi.lib /link /DLL /OUT:QuickMenuHotkeys.asi
