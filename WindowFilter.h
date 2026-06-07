#pragma once

#include <windows.h>

struct WindowFilterInput {
    bool isSelfWindow;
    bool isVisible;
    bool hasTitle;
    bool hasOwner;
    bool isToolWindow;
    bool isCloaked;
    bool isApplicationFrameWindow;
    bool hasApplicationFrameCoreWindow;
    DWORD targetPID;
    DWORD currentPID;
};

inline bool ShouldIncludeWindowInSwitcher(const WindowFilterInput& window) {
    if (window.isSelfWindow) return false;
    if (!window.isVisible || !window.hasTitle) return false;
    if (window.hasOwner) return false;
    if (window.isToolWindow) return false;
    if (window.isCloaked) return false;
    if (window.isApplicationFrameWindow && !window.hasApplicationFrameCoreWindow) return false;
    if (window.targetPID != 0 && window.currentPID != window.targetPID) return false;
    return true;
}
