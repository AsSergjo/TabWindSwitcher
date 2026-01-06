#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <map>
#include <psapi.h>
#include <shlwapi.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

#define WM_APP_SWITCH_TO_WINDOW (WM_APP + 1)
#define IDM_ABOUT 101
#define IDM_EXIT  102
#define MAIN_ICON 101

// --- Data Structures ---
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    Gdiplus::Bitmap* pIconBitmap;
};

struct EnumData {
    std::vector<WindowInfo>* list;
    DWORD targetPID;
};

// --- Global Variables & Constants ---
ULONG_PTR gdiplusToken;
HHOOK hKeyboardHook;
HWND g_hMainWnd = NULL;
HWND g_hSwitcherWnd = NULL;
HANDLE g_hMutex = NULL; // Mutex for single instance
std::vector<WindowInfo> g_windowList;
size_t g_selectedIndex = 0;
bool g_isSingleAppMode = false;

const wchar_t MAIN_CLASS_NAME[] = L"TabWindSwitcherMainClass";
const wchar_t SWITCHER_CLASS_NAME[] = L"TabWindSwitcherUIClass";
const wchar_t MUTEX_NAME[] = L"TabWindSwitcher_InstanceMutex";

// INI Configuration
const wchar_t INI_FILENAME[] = L"TabWindSwitcher.ini";
const wchar_t INI_SECTION_SETTINGS[] = L"Settings";
const wchar_t INI_KEY_TRANSPARENCY[] = L"Transparency";
const int DEFAULT_TRANSPARENCY = 220; // 0-255

// Layout Constants
const int ICON_SIZE = 64;
const int PADDING = 20;
const int TITLE_HEIGHT = 40;

// Global Transparency Value
BYTE g_alpha = DEFAULT_TRANSPARENCY;


// --- Forward Declarations ---
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SwitcherWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
BOOL CALLBACK EnumWindowsProc(HWND, LPARAM);
Gdiplus::Bitmap* GetBestIconForProcess(const wchar_t* exePath, HWND associatedHwnd);
void UpdateSwitcherLayeredWindow(HWND hwnd);
void AddRoundRectToPath(Gdiplus::GraphicsPath&, Gdiplus::RectF, Gdiplus::REAL);
void ShowSwitcher(bool filterByProcess);
void ReadTransparencySetting();


// --- Entry Point ---
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PSTR /*lpCmdLine*/, INT /*nCmdShow*/) {
    // Prevent multiple instances from running
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (g_hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Another instance of TabWindSwitcher is already running.", L"TabWindSwitcher", MB_OK | MB_ICONINFORMATION);
        if (g_hMutex) {
            ReleaseMutex(g_hMutex);
            CloseHandle(g_hMutex);
        }
        return 1; // Exit
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCE(MAIN_ICON));

    WNDCLASSEXW wcMain = {};
    wcMain.cbSize = sizeof(WNDCLASSEXW);
    wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInstance;
    wcMain.lpszClassName = MAIN_CLASS_NAME;
    wcMain.hIcon = hAppIcon;
    wcMain.hIconSm = hAppIcon;
    RegisterClassExW(&wcMain);

    WNDCLASSEXW wcSwitcher = {};
    wcSwitcher.cbSize = sizeof(WNDCLASSEXW);
    wcSwitcher.lpfnWndProc = SwitcherWndProc;
    wcSwitcher.hInstance = hInstance;
    wcSwitcher.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSwitcher.lpszClassName = SWITCHER_CLASS_NAME;
    wcSwitcher.hIcon = hAppIcon;
    wcSwitcher.hIconSm = hAppIcon;
    RegisterClassExW(&wcSwitcher);

    g_hMainWnd = CreateWindowExW(0, MAIN_CLASS_NAME, L"TabWindSwitcher Main", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    ReadTransparencySetting(); // Initialize transparency from INI

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hKeyboardHook);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();

    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
    }

    return (int)msg.wParam;
}

// --- Window Procedures ---

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_APP_SWITCH_TO_WINDOW: {
            HWND hwndToActivate = (HWND)wParam;
            if (hwndToActivate) {
                DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
                DWORD ourThreadId = GetCurrentThreadId();
                if (foregroundThreadId != ourThreadId) AttachThreadInput(foregroundThreadId, ourThreadId, TRUE);
                
                LockSetForegroundWindow(LSFW_UNLOCK);
                AllowSetForegroundWindow(ASFW_ANY);
                if (IsIconic(hwndToActivate)) ShowWindow(hwndToActivate, SW_RESTORE);
                
                SetWindowPos(hwndToActivate, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(hwndToActivate);
                SetActiveWindow(hwndToActivate);
                SetFocus(hwndToActivate);
                SetWindowPos(hwndToActivate, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

                if (foregroundThreadId != ourThreadId) AttachThreadInput(foregroundThreadId, ourThreadId, FALSE);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK SwitcherWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_RBUTTONUP: {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About");
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

            POINT cursorPos;
            GetCursorPos(&cursorPos);

            SetForegroundWindow(hwnd);

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, cursorPos.x, cursorPos.y, 0, hwnd, NULL);
            
            DestroyMenu(hMenu);

            switch (cmd) {
                case IDM_ABOUT:
                    MessageBoxW(hwnd, L"TabWindSwitcher v1.0", L"About", MB_OK | MB_ICONINFORMATION);
                    break;
                case IDM_EXIT:
                    PostMessage(g_hMainWnd, WM_CLOSE, 0, 0);
                    break;
            }
            return 0;
        }
        case WM_NCDESTROY: {
            for (const auto& window : g_windowList) {
                if (window.pIconBitmap) delete window.pIconBitmap;
            }
            g_windowList.clear();
            g_hSwitcherWnd = NULL;
            g_isSingleAppMode = false;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// --- Drawing ---

void UpdateSwitcherLayeredWindow(HWND hwnd) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int width = rcClient.right - rcClient.left;
    int height = rcClient.bottom - rcClient.top;
    if (width == 0 || height == 0) return;

    RECT rcWindow;
    GetWindowRect(hwnd, &rcWindow);
    POINT ptDst = { rcWindow.left, rcWindow.top };

    HDC hdcScreen = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

    Gdiplus::Graphics graphics(hdcMem);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHint::TextRenderingHintAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    
    Gdiplus::GraphicsPath path;
    AddRoundRectToPath(path, Gdiplus::RectF(0.0f, 0.0f, (Gdiplus::REAL)width, (Gdiplus::REAL)height), 12.0f);
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(220, 40, 40, 45));
    graphics.FillPath(&bgBrush, &path);
    
    Gdiplus::Font titleFont(L"Segoe UI", 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));

    int itemContentWidth = 128;
    int itemCellWidth = itemContentWidth + PADDING;
    int contentWidth = static_cast<int>(itemCellWidth * g_windowList.size() - PADDING);
    int startX = (width - contentWidth) / 2;

    for (size_t i = 0; i < g_windowList.size(); ++i) {
        int x = static_cast<int>(startX + i * itemCellWidth);
        int y = PADDING;

        if (i == g_selectedIndex) {
            Gdiplus::GraphicsPath selectionPath;
            AddRoundRectToPath(selectionPath, Gdiplus::RectF((Gdiplus::REAL)x - 4, (Gdiplus::REAL)y - 4, (Gdiplus::REAL)itemContentWidth + 8, (Gdiplus::REAL)(ICON_SIZE + TITLE_HEIGHT) + 8), 6.0f);
            Gdiplus::SolidBrush selectionBrush(Gdiplus::Color(80, 120, 120, 140));
            graphics.FillPath(&selectionBrush, &selectionPath);
        }

        int iconDrawX = x + (itemContentWidth - ICON_SIZE) / 2;
        if (g_windowList[i].pIconBitmap) {
            graphics.DrawImage(g_windowList[i].pIconBitmap, iconDrawX, y, ICON_SIZE, ICON_SIZE);
        }

        Gdiplus::StringFormat strFormat;
        strFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
        strFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        Gdiplus::RectF textRect((Gdiplus::REAL)x, (Gdiplus::REAL)(y + ICON_SIZE), (Gdiplus::REAL)itemContentWidth, (Gdiplus::REAL)TITLE_HEIGHT);
        graphics.DrawString(g_windowList[i].title.c_str(), -1, &titleFont, textRect, &strFormat, &textBrush);
    }
    
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, g_alpha, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE size = { width, height };
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcScreen);
}

// --- Main Logic ---

void ShowSwitcher(bool filterByProcess) {
    if (g_hSwitcherWnd) {
        if (!g_windowList.empty()) g_selectedIndex = (g_selectedIndex + 1) % g_windowList.size();
        UpdateSwitcherLayeredWindow(g_hSwitcherWnd);
        return;
    }
    
    for (const auto& window : g_windowList) {
        if(window.pIconBitmap) delete window.pIconBitmap;
    }
    g_windowList.clear();
    
    DWORD targetPID = 0;
    g_isSingleAppMode = filterByProcess;
    if (filterByProcess) {
        HWND fg_hwnd = GetForegroundWindow();
        if(fg_hwnd) GetWindowThreadProcessId(fg_hwnd, &targetPID);
    }

    EnumData data = { &g_windowList, targetPID };
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    
    if (g_windowList.size() > 0) {
        g_selectedIndex = 0;
        
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        HMONITOR hMonitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        int screenWidth, screenHeight, screenX, screenY;
        if (GetMonitorInfo(hMonitor, &monitorInfo)) {
            screenWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
            screenHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
            screenX = monitorInfo.rcMonitor.left;
            screenY = monitorInfo.rcMonitor.top;
        } else {
            screenWidth = GetSystemMetrics(SM_CXSCREEN);
            screenHeight = GetSystemMetrics(SM_CYSCREEN);
            screenX = 0;
            screenY = 0;
        }
        
        int itemContentWidth = 128;
        int itemCellWidth = itemContentWidth + PADDING;
        int contentWidth = static_cast<int>(itemCellWidth * g_windowList.size() - PADDING);
        int windowWidth = static_cast<int>(contentWidth + 2 * PADDING);
        int windowHeight = ICON_SIZE + TITLE_HEIGHT + PADDING * 2;
        
        if (windowWidth > screenWidth * 0.9) windowWidth = (int)(screenWidth * 0.9);

        int x = screenX + (screenWidth - windowWidth) / 2;
        int y = screenY + (screenHeight - windowHeight) / 2;

        g_hSwitcherWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            SWITCHER_CLASS_NAME, L"Switcher", WS_POPUP | WS_VISIBLE,
            x, y, windowWidth, windowHeight, NULL, NULL, GetModuleHandle(NULL), NULL);
        UpdateSwitcherLayeredWindow(g_hSwitcherWnd);
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    static bool altDown = false;
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pkbhs = (KBDLLHOOKSTRUCT*)lParam;

        if (pkbhs->vkCode == VK_LMENU || pkbhs->vkCode == VK_RMENU) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                altDown = true;
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                altDown = false;
                if (g_hSwitcherWnd) {
                    if(g_selectedIndex < g_windowList.size()) {
                        HWND hwndToActivate = g_windowList[g_selectedIndex].hwnd;
                        PostMessage(g_hMainWnd, WM_APP_SWITCH_TO_WINDOW, (WPARAM)hwndToActivate, 0);
                    }
                    DestroyWindow(g_hSwitcherWnd);
                }
            }
        }

        if (altDown && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            if (pkbhs->vkCode == VK_TAB) { ShowSwitcher(false); return 1; }
            if (pkbhs->vkCode == VK_OEM_3) { ShowSwitcher(true); return 1; }
        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// --- Icon and Window Enumeration Helpers ---

Gdiplus::Bitmap* GetBestIconForProcess(const wchar_t* exePath, HWND associatedHwnd) {
    HICON hIcon = NULL;

    if (exePath && exePath[0] != L'\0') {
        HMODULE hModule = LoadLibraryExW(exePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (hModule) {
            wchar_t groupName[256] = {0};
            EnumResourceNamesW(hModule, RT_GROUP_ICON, 
                [](HMODULE, LPCWSTR, LPWSTR lpName, LONG_PTR lParam) -> BOOL {
                    if (IS_INTRESOURCE(lpName)) { wsprintfW((wchar_t*)lParam, L"#%d", (int)(intptr_t)lpName); }
                    else { wcscpy_s((wchar_t*)lParam, 256, lpName); }
                    return FALSE;
                }, (LONG_PTR)groupName);

            if (groupName[0] != L'\0') {
                HRSRC hResource = FindResourceW(hModule, groupName, RT_GROUP_ICON);
                if (hResource) {
                    HGLOBAL hMem = LoadResource(hModule, hResource);
                    void* pIconDir = hMem ? LockResource(hMem) : NULL;
                    if (pIconDir) {
                        int iconId = LookupIconIdFromDirectoryEx((PBYTE)pIconDir, TRUE, 256, 256, LR_DEFAULTCOLOR);
                        HRSRC hIconResource = FindResourceW(hModule, MAKEINTRESOURCEW(iconId), RT_ICON);
                        if (hIconResource) {
                            HGLOBAL hIconMem = LoadResource(hModule, hIconResource);
                            void* pIconData = hIconMem ? LockResource(hIconMem) : NULL;
                            if (pIconData) {
                                hIcon = CreateIconFromResourceEx((PBYTE)pIconData, SizeofResource(hModule, hIconResource), TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
                            }
                        }
                    }
                }
            }
            FreeLibrary(hModule);
        }
    }
    
    if (!hIcon) {
        SendMessageTimeout(associatedHwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
    }
    
    if (hIcon) {
        ICONINFO ii = {0};
        if (GetIconInfo(hIcon, &ii)) {
            BITMAP bm = {0};
            GetObject(ii.hbmColor, sizeof(bm), &bm);
            Gdiplus::Bitmap* result = new Gdiplus::Bitmap(bm.bmWidth, bm.bmHeight, PixelFormat32bppARGB);
            Gdiplus::Rect rect(0, 0, bm.bmWidth, bm.bmHeight);
            Gdiplus::BitmapData bmpData;
            if(result->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
                GetBitmapBits(ii.hbmColor, bm.bmWidthBytes * bm.bmHeight, bmpData.Scan0);
                bool hasAlpha = false;
                for (int i = 0; i < bm.bmWidth * bm.bmHeight; ++i) {
                    if (((UINT32*)bmpData.Scan0)[i] & 0xFF000000) { hasAlpha = true; break; }
                }
                if (!hasAlpha && ii.hbmMask) {
                    BITMAP bmMask;
                    GetObject(ii.hbmMask, sizeof(bmMask), &bmMask);
                    std::vector<BYTE> maskBits(bmMask.bmWidthBytes * bmMask.bmHeight);
                    GetBitmapBits(ii.hbmMask, bmMask.bmWidthBytes * bmMask.bmHeight, maskBits.data());
                    for (int i = 0; i < bm.bmWidth * bm.bmHeight; ++i) {
                        if ((maskBits[i / 8] >> (7 - (i % 8))) & 1) {
                             ((UINT32*)bmpData.Scan0)[i] &= 0x00FFFFFF;
                        } else {
                             ((UINT32*)bmpData.Scan0)[i] |= 0xFF000000;
                        }
                    }
                }
                result->UnlockBits(&bmpData);
            }
            DeleteObject(ii.hbmColor);
            if(ii.hbmMask) DeleteObject(ii.hbmMask);
            DestroyIcon(hIcon);
            return result;
        }
        DestroyIcon(hIcon);
    }
    return new Gdiplus::Bitmap(32, 32, PixelFormat32bppARGB);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (hwnd == g_hSwitcherWnd || hwnd == g_hMainWnd) return TRUE;
    if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0 || GetWindow(hwnd, GW_OWNER) != 0) return TRUE;
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD currentPID = 0;
    GetWindowThreadProcessId(hwnd, &currentPID);

    EnumData* pData = (EnumData*)lParam;
    if (pData->targetPID != 0 && currentPID != pData->targetPID) {
        return TRUE;
    }
    
    wchar_t processPath[MAX_PATH] = {0};
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, currentPID);
    if (hProcess) {
        if (GetModuleFileNameExW(hProcess, NULL, processPath, MAX_PATH) == 0) {
            processPath[0] = L'\0';
        }
        CloseHandle(hProcess);
    }

    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    
    pData->list->push_back({hwnd, title, GetBestIconForProcess(processPath, hwnd)});
    
    return TRUE;
}

void AddRoundRectToPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, Gdiplus::REAL radius) {
    if (radius <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    Gdiplus::REAL diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void ReadTransparencySetting() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    PathRemoveFileSpecW(iniPath);
    PathAppendW(iniPath, INI_FILENAME);

    // Read transparency, defaulting to DEFAULT_TRANSPARENCY if not found
    int transparency = GetPrivateProfileIntW(
        INI_SECTION_SETTINGS,
        INI_KEY_TRANSPARENCY,
        DEFAULT_TRANSPARENCY,
        iniPath
    );

    // Ensure transparency is within valid range [0, 255]
    if (transparency < 0) transparency = 0;
    if (transparency > 255) transparency = 255;

    g_alpha = (BYTE)transparency;
}
