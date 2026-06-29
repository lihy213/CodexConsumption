# CodexAssumptionNative

This folder contains the native Win32 implementation of CodexConsumption.

Build:

```bat
build-native.bat
```

Output:

```text
..\dist-native\CodexAssumptionNative.exe
```

Architecture:

- Hidden owner window: timers, refresh, commands, Explorer restart handling.
- Custom status window: draws remaining 5h and 1w quota.
- Floating mode: topmost desktop widget near the taskbar.
- Pinned mode: taskbar-area widget with placement maintenance and occupied-area avoidance.
- Tooltip: hover shows next refresh/reset time for 5h and 1w windows.
