#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <algorithm>
#include <psapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include "WindowFilter.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define WM_APP_SWITCH_TO_WINDOW (WM_APP + 1)
#define WM_APP_TRAY             (WM_APP + 2)   // уведомления от иконки в трее
#define WM_APP_ICON_READY       (WM_APP + 3)

// Идентификаторы меню трея
#define IDM_TRAY_ABOUT          101
#define IDM_TRAY_HOTKEYS        102
#define IDM_TRAY_MINIMIZE_ON    103
#define IDM_TRAY_MINIMIZE_OFF   104
#define IDM_TRAY_EXIT           199

#define MAIN_ICON               101
#define TRAY_UID                1   // уникальный ID иконки в трее (NOTIFYICONDATA)

// --- Data Structures ---
struct WindowInfo {
    HWND hwnd;
    DWORD processId;
    std::wstring title;
    std::wstring processPath;
    Gdiplus::Bitmap* pIconBitmap;
};

struct EnumData {
    std::vector<WindowInfo>* list;
    DWORD targetPID;
};

struct AppSettings {
    BYTE alpha = 220;
    bool minimizeOthers = true;
    Gdiplus::Color bgColor = Gdiplus::Color(220, 40, 40, 45);
    Gdiplus::Color selectionColor = Gdiplus::Color(80, 120, 120, 140);
    Gdiplus::Color textColor = Gdiplus::Color(255, 255, 255, 255);
    int windowRadius = 12;
    int selectionRadius = 6;
    int iconTimeoutMs = 50;
};

class TabWindSwitcherApp {
public:
    ULONG_PTR gdiplusToken = 0;
    HHOOK keyboardHook = NULL;
    HWND mainWnd = NULL;
    HWND switcherWnd = NULL;
    HANDLE instanceMutex = NULL;
    NOTIFYICONDATAW trayIcon = {};
    std::vector<WindowInfo> windowList;
    std::vector<HWND> windowsToMinimize;
    AppSettings settings;
    size_t selectedIndex = 0;
    size_t iconsPerRow = 0;
    size_t rows = 0;
    size_t visibleRows = 0;
    size_t currentPage = 0;
    size_t pageCount = 0;
    UINT dpi = 96;
    unsigned int iconGeneration = 0;
    bool altDown = false;
    std::mutex stateMutex;
    std::thread iconWorker;

    int Scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi), 96);
    }

    void ClearWindowListLocked() {
        for (const auto& window : windowList) {
            if (window.pIconBitmap) delete window.pIconBitmap;
        }
        windowList.clear();
    }

    void CancelIconLoadingLocked() {
        ++iconGeneration;
    }

    void ResetSwitcherStateLocked() {
        ClearWindowListLocked();
        switcherWnd = NULL;
        selectedIndex = 0;
        iconsPerRow = 0;
        rows = 0;
        visibleRows = 0;
        currentPage = 0;
        pageCount = 0;
        CancelIconLoadingLocked();
    }

    void ClampCurrentPageLocked() {
        if (pageCount == 0) {
            currentPage = 0;
        } else if (currentPage >= pageCount) {
            currentPage = pageCount - 1;
        }
    }

    void RecalculatePageForSelectionLocked() {
        if (iconsPerRow == 0 || visibleRows == 0 || windowList.empty()) {
            currentPage = 0;
            return;
        }

        size_t selectedRow = selectedIndex / iconsPerRow;
        size_t firstVisibleRow = currentPage * visibleRows;
        size_t lastVisibleRow = firstVisibleRow + visibleRows - 1;

        if (selectedRow < firstVisibleRow) {
            currentPage = selectedRow / visibleRows;
        } else if (selectedRow > lastVisibleRow) {
            currentPage = selectedRow / visibleRows;
        }
        ClampCurrentPageLocked();
    }
};

// --- Global Variables & Constants ---
TabWindSwitcherApp g_app;

const wchar_t MAIN_CLASS_NAME[]    = L"TabWindSwitcherMainClass";
const wchar_t SWITCHER_CLASS_NAME[] = L"TabWindSwitcherUIClass";
const wchar_t MUTEX_NAME[]         = L"TabWindSwitcher_InstanceMutex";

// INI Configuration
const wchar_t INI_FILENAME[]            = L"TabWindSwitcher.ini";
const wchar_t INI_SECTION_SETTINGS[]    = L"Settings";
const wchar_t INI_KEY_TRANSPARENCY[]    = L"Transparency";
const wchar_t INI_KEY_MINIMIZE_OTHERS[] = L"MinimizeOthers";  // 0 = выкл, 1 = вкл
const wchar_t INI_KEY_BG_R[]            = L"BgColorR";
const wchar_t INI_KEY_BG_G[]            = L"BgColorG";
const wchar_t INI_KEY_BG_B[]            = L"BgColorB";
const wchar_t INI_KEY_BG_A[]            = L"BgColorA";
const wchar_t INI_KEY_SEL_R[]           = L"SelColorR";
const wchar_t INI_KEY_SEL_G[]           = L"SelColorG";
const wchar_t INI_KEY_SEL_B[]           = L"SelColorB";
const wchar_t INI_KEY_SEL_A[]           = L"SelColorA";
const wchar_t INI_KEY_TEXT_R[]          = L"TextColorR";
const wchar_t INI_KEY_TEXT_G[]          = L"TextColorG";
const wchar_t INI_KEY_TEXT_B[]          = L"TextColorB";
const wchar_t INI_KEY_WINDOW_RADIUS[]   = L"WindowRadius";
const wchar_t INI_KEY_SELECTION_RADIUS[]= L"SelectionRadius";
const wchar_t INI_KEY_ICON_TIMEOUT_MS[] = L"IconTimeoutMs";
const int DEFAULT_TRANSPARENCY    = 220; // 0-255
const int DEFAULT_MINIMIZE_OTHERS = 1;   // включено по умолчанию

// Layout Constants
const int ICON_SIZE        = 64;
const int PADDING          = 20;
const int ICON_PADDING_TOP = 10;
const int TITLE_HEIGHT     = 40;


// --- Forward Declarations ---
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SwitcherWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
BOOL   CALLBACK EnumWindowsProc(HWND, LPARAM);
Gdiplus::Bitmap* GetBestIconForProcess(const wchar_t* exePath, HWND associatedHwnd);
void UpdateSwitcherLayeredWindow(HWND hwnd);
void AddRoundRectToPath(Gdiplus::GraphicsPath&, Gdiplus::RectF, Gdiplus::REAL);
void ShowSwitcher(bool filterByProcess, bool backwards);
void MoveSelection(int delta);
void ChangePage(int delta);
void StartIconLoading(unsigned int generation);
void StopIconWorker();
void EnableDpiAwareness();
UINT GetDpiForMonitorOrSystem(HMONITOR monitor);
std::wstring GetProcessImagePath(DWORD processId);
int ReadIniIntClamped(const wchar_t* iniPath, const wchar_t* key, int defaultValue, int minValue, int maxValue);
void ReadSettings();
void AddTrayIcon(HWND hwnd, HICON hIcon);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hwnd);
HBITMAP CreateMenuIconBitmap(HDC hdcRef, const wchar_t* glyph, COLORREF color, int size);


// --- Entry Point ---
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PSTR /*lpCmdLine*/, INT /*nCmdShow*/) {
    EnableDpiAwareness();

    g_app.instanceMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (g_app.instanceMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Another instance of TabWindSwitcher is already running.", L"TabWindSwitcher", MB_OK | MB_ICONINFORMATION);
        if (g_app.instanceMutex) {
            ReleaseMutex(g_app.instanceMutex);
            CloseHandle(g_app.instanceMutex);
        }
        return 1;
    }

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    if (!coInitialized && hrCo != RPC_E_CHANGED_MODE) {
        MessageBoxW(NULL, L"Cannot initialize COM.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&g_app.gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Ok) {
        MessageBoxW(NULL, L"Cannot initialize GDI+.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        if (coInitialized) CoUninitialize();
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCE(MAIN_ICON));

    WNDCLASSEXW wcMain = {};
    wcMain.cbSize        = sizeof(WNDCLASSEXW);
    wcMain.lpfnWndProc   = MainWndProc;
    wcMain.hInstance     = hInstance;
    wcMain.lpszClassName = MAIN_CLASS_NAME;
    wcMain.hIcon         = hAppIcon;
    wcMain.hIconSm       = hAppIcon;
    if (!RegisterClassExW(&wcMain)) {
        MessageBoxW(NULL, L"Cannot register main window class.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
        if (coInitialized) CoUninitialize();
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    WNDCLASSEXW wcSwitcher = {};
    wcSwitcher.cbSize        = sizeof(WNDCLASSEXW);
    wcSwitcher.lpfnWndProc   = SwitcherWndProc;
    wcSwitcher.hInstance     = hInstance;
    wcSwitcher.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wcSwitcher.lpszClassName = SWITCHER_CLASS_NAME;
    wcSwitcher.hIcon         = hAppIcon;
    wcSwitcher.hIconSm       = hAppIcon;
    if (!RegisterClassExW(&wcSwitcher)) {
        MessageBoxW(NULL, L"Cannot register switcher window class.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
        if (coInitialized) CoUninitialize();
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    g_app.mainWnd = CreateWindowExW(0, MAIN_CLASS_NAME, L"TabWindSwitcher Main", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_app.mainWnd) {
        MessageBoxW(NULL, L"Cannot create main message window.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
        if (coInitialized) CoUninitialize();
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    g_app.keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_app.keyboardHook) {
        MessageBoxW(NULL, L"Cannot install keyboard hook.", L"TabWindSwitcher", MB_OK | MB_ICONERROR);
        DestroyWindow(g_app.mainWnd);
        Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
        if (coInitialized) CoUninitialize();
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
        return 1;
    }

    ReadSettings();

    // Добавляем иконку в системный трей
    AddTrayIcon(g_app.mainWnd, hAppIcon);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_app.keyboardHook);
    RemoveTrayIcon();

    StopIconWorker();
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.ResetSwitcherStateLocked();
    }

    Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
    if (coInitialized) CoUninitialize();

    if (g_app.instanceMutex) {
        ReleaseMutex(g_app.instanceMutex);
        CloseHandle(g_app.instanceMutex);
    }

    return (int)msg.wParam;
}

// --- Window Procedures ---

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Когда Проводник перезапускается (крэш/обновление), трей-иконки пропадают.
    // Shell посылает RegisterWindowMessage("TaskbarCreated") - перерегистрируем иконку.
    static const UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    if (uMsg == WM_TASKBARCREATED) {
        // Иконка пропала - добавляем заново
        HICON hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
        AddTrayIcon(hwnd, hIcon);
        return 0;
    }

    switch (uMsg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_APP_ICON_READY:
        {
            HWND switcherWnd = NULL;
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                switcherWnd = g_app.switcherWnd;
            }
            if (switcherWnd)
                UpdateSwitcherLayeredWindow(switcherWnd);
            return 0;
        }

        // --- Системный трей ---
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu(hwnd);
                    break;
                case WM_LBUTTONDBLCLK:
                    // Двойной клик - показать диалог About
                    MessageBoxW(hwnd,
                        L"TabWindSwitcher v1.0\n\n"
                        L"Alt + Tab       - переключатель всех окон\n"
                        L"Alt + `         - переключатель окон текущего приложения\n"
                        L"Escape          - закрыть переключатель",
                        L"TabWindSwitcher", MB_OK | MB_ICONINFORMATION);
                    break;
            }
            return 0;

        case WM_APP_SWITCH_TO_WINDOW: {
            HWND hwndToActivate = (HWND)wParam;

            // Сначала активируем нужное окно, потом сворачиваем остальные.
            // Порядок принципиален: если сворачивать до активации, фокус
            // достанется случайному окну и SetForegroundWindow может не сработать.
            if (hwndToActivate) {
                DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
                DWORD ourThreadId        = GetCurrentThreadId();

                if (foregroundThreadId != ourThreadId)
                    AttachThreadInput(foregroundThreadId, ourThreadId, TRUE);

                LockSetForegroundWindow(LSFW_UNLOCK);
                AllowSetForegroundWindow(ASFW_ANY);

                if (IsIconic(hwndToActivate))
                    ShowWindow(hwndToActivate, SW_RESTORE);

                SetWindowPos(hwndToActivate, HWND_TOPMOST,    0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(hwndToActivate);
                SetActiveWindow(hwndToActivate);
                SetFocus(hwndToActivate);
                SetWindowPos(hwndToActivate, HWND_NOTOPMOST,  0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

                if (foregroundThreadId != ourThreadId)
                    AttachThreadInput(foregroundThreadId, ourThreadId, FALSE);
            }

            // Сворачиваем все остальные окна из списка (если фича включена в INI).
            // Список уже готов: он был заполнен в хуке до DestroyWindow(switcherWnd).
            std::vector<HWND> windowsToMinimize;
            bool minimizeOthers = false;
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                minimizeOthers = g_app.settings.minimizeOthers;
                windowsToMinimize.swap(g_app.windowsToMinimize);
            }

            if (minimizeOthers) {
                for (HWND hwndMin : windowsToMinimize) {
                    // IsWindow - защита от окон, закрытых за время переключения.
                    if (IsWindow(hwndMin) && !IsIconic(hwndMin))
                        ShowWindow(hwndMin, SW_MINIMIZE);
                }
            }

            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK SwitcherWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_KILLFOCUS:
            // Потеря фокуса - закрываем переключатель без активации какого-либо окна.
            // Типичный случай: пользователь кликнул мимо окна.
            DestroyWindow(hwnd);
            return 0;

        case WM_DPICHANGED: {
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                g_app.dpi = HIWORD(wParam);
            }
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, NULL,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateSwitcherLayeredWindow(hwnd);
            return 0;
        }

        case WM_NCDESTROY: {
            StopIconWorker();
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            g_app.ResetSwitcherStateLocked();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// --- Drawing ---

void UpdateSwitcherLayeredWindow(HWND hwnd) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int width  = rcClient.right  - rcClient.left;
    int height = rcClient.bottom - rcClient.top;
    if (width == 0 || height == 0) return;

    RECT rcWindow;
    GetWindowRect(hwnd, &rcWindow);
    POINT ptDst = { rcWindow.left, rcWindow.top };

    // FIX: UpdateLayeredWindow требует экранный DC (NULL), а не DC окна.
    // Использование GetDC(hwnd) на layered-окне технически некорректно.
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;

    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hbmMem) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);
    if (!hbmOld) {
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    BYTE alpha = DEFAULT_TRANSPARENCY;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);

        Gdiplus::Graphics graphics(hdcMem);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetSmoothingMode(Gdiplus::SmoothingMode::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHint::TextRenderingHintAntiAlias);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        alpha = g_app.settings.alpha;

        int padding = g_app.Scale(PADDING);
        int iconSize = g_app.Scale(ICON_SIZE);
        int iconPaddingTop = g_app.Scale(ICON_PADDING_TOP);
        int titleHeight = g_app.Scale(TITLE_HEIGHT);
        int titleLineHeight = titleHeight / 2;
        int itemContentWidth = g_app.Scale(128);
        int itemContentHeight = iconPaddingTop + iconSize + titleHeight;
        int itemCellWidth = itemContentWidth + padding;
        int itemCellHeight = itemContentHeight + padding;
        int contentWidth = (g_app.iconsPerRow > 0)
                         ? static_cast<int>(g_app.iconsPerRow * itemCellWidth - padding)
                         : 0;
        int startX = (width - contentWidth) / 2;

        Gdiplus::GraphicsPath path;
        AddRoundRectToPath(path,
            Gdiplus::RectF(0.0f, 0.0f, (Gdiplus::REAL)width, (Gdiplus::REAL)height),
            (Gdiplus::REAL)g_app.Scale(g_app.settings.windowRadius));
        Gdiplus::SolidBrush bgBrush(g_app.settings.bgColor);
        graphics.FillPath(&bgBrush, &path);

        Gdiplus::Font titleFont(L"Segoe UI", (Gdiplus::REAL)g_app.Scale(13),
                                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(g_app.settings.textColor);

        if (g_app.iconsPerRow > 0 && g_app.visibleRows > 0) {
            size_t firstRow = g_app.currentPage * g_app.visibleRows;
            size_t lastRow = std::min(g_app.rows, firstRow + g_app.visibleRows);
            size_t firstIndex = firstRow * g_app.iconsPerRow;
            size_t lastIndex = std::min(g_app.windowList.size(), lastRow * g_app.iconsPerRow);

            for (size_t i = firstIndex; i < lastIndex; ++i) {
                size_t absoluteRow = i / g_app.iconsPerRow;
                size_t visibleRow = absoluteRow - firstRow;
                size_t col = i % g_app.iconsPerRow;
                int x = static_cast<int>(startX + col * itemCellWidth);
                int y = static_cast<int>(padding + visibleRow * itemCellHeight);

                if (i == g_app.selectedIndex) {
                    Gdiplus::GraphicsPath selectionPath;
                    int selectionPad = g_app.Scale(4);
                    AddRoundRectToPath(selectionPath,
                        Gdiplus::RectF((Gdiplus::REAL)(x - selectionPad), (Gdiplus::REAL)(y - selectionPad),
                                       (Gdiplus::REAL)(itemContentWidth + 2 * selectionPad),
                                       (Gdiplus::REAL)(itemContentHeight + 2 * selectionPad)),
                        (Gdiplus::REAL)g_app.Scale(g_app.settings.selectionRadius));
                    Gdiplus::SolidBrush selectionBrush(g_app.settings.selectionColor);
                    graphics.FillPath(&selectionBrush, &selectionPath);
                }

                int iconDrawX = x + (itemContentWidth - iconSize) / 2;
                if (g_app.windowList[i].pIconBitmap) {
                    graphics.DrawImage(g_app.windowList[i].pIconBitmap,
                                       iconDrawX, y + iconPaddingTop, iconSize, iconSize);
                }

                Gdiplus::StringFormat strFormat;
                strFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
                strFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
                strFormat.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
                strFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                Gdiplus::RectF textRect(
                    (Gdiplus::REAL)x,
                    (Gdiplus::REAL)(y + iconPaddingTop + iconSize),
                    (Gdiplus::REAL)itemContentWidth,
                    (Gdiplus::REAL)(titleLineHeight * 2));
                graphics.DrawString(g_app.windowList[i].title.c_str(), -1, &titleFont, textRect, &strFormat, &textBrush);
            }
        }
    }

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE  size  = { width, height };
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen); // FIX: соответствует GetDC(NULL) выше
}

// --- Main Logic ---

void ShowSwitcher(bool filterByProcess, bool backwards) {
    bool hasSwitcher = false;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        hasSwitcher = (g_app.switcherWnd != NULL);
    }

    if (hasSwitcher) {
        MoveSelection(backwards ? -1 : 1);
        return;
    }

    StopIconWorker();
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.ClearWindowListLocked();
        g_app.CancelIconLoadingLocked();
    }

    DWORD targetPID = 0;
    if (filterByProcess) {
        HWND fg_hwnd = GetForegroundWindow();
        if (fg_hwnd) GetWindowThreadProcessId(fg_hwnd, &targetPID);
    }

    std::vector<WindowInfo> windows;
    EnumData data = { &windows, targetPID };
    EnumWindows(EnumWindowsProc, (LPARAM)&data);

    if (!windows.empty()) {
        // FIX: стандартное поведение Alt+Tab - при первом показе переключателя
        // сразу выделяется следующее окно (индекс 1), а не текущее (индекс 0).
        // Если окно одно - деваться некуда, остаёмся на 0.
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        HMONITOR   hMonitor    = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        int screenWidth, screenHeight, screenX, screenY;
        if (GetMonitorInfo(hMonitor, &monitorInfo)) {
            screenWidth  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
            screenHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
            screenX      = monitorInfo.rcMonitor.left;
            screenY      = monitorInfo.rcMonitor.top;
        } else {
            screenWidth  = GetSystemMetrics(SM_CXSCREEN);
            screenHeight = GetSystemMetrics(SM_CYSCREEN);
            screenX      = 0;
            screenY      = 0;
        }

        UINT dpi = GetDpiForMonitorOrSystem(hMonitor);
        int windowWidth = 0;
        int windowHeight = 0;
        unsigned int iconGeneration = 0;

        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);

            g_app.dpi = dpi;
            g_app.windowList.swap(windows);
            ++g_app.iconGeneration;
            iconGeneration = g_app.iconGeneration;

            g_app.selectedIndex = backwards
                                ? g_app.windowList.size() - 1
                                : ((g_app.windowList.size() > 1) ? 1 : 0);

            int padding = g_app.Scale(PADDING);
            int iconSize = g_app.Scale(ICON_SIZE);
            int iconPaddingTop = g_app.Scale(ICON_PADDING_TOP);
            int titleHeight = g_app.Scale(TITLE_HEIGHT);
            int itemContentWidth = g_app.Scale(128);
            int itemContentHeight = iconPaddingTop + iconSize + titleHeight;
            int itemCellWidth = itemContentWidth + padding;
            int itemCellHeight = itemContentHeight + padding;

            int maxIconsPerRow = static_cast<int>((screenWidth * 0.9 - 2 * padding) / itemCellWidth);
            if (maxIconsPerRow < 1) maxIconsPerRow = 1;

            g_app.iconsPerRow = (g_app.windowList.size() < (size_t)maxIconsPerRow)
                              ? g_app.windowList.size()
                              : (size_t)maxIconsPerRow;
            g_app.rows = (g_app.windowList.size() + g_app.iconsPerRow - 1) / g_app.iconsPerRow;

            int maxVisibleRows = static_cast<int>((screenHeight * 0.9 - 2 * padding) / itemCellHeight);
            if (maxVisibleRows < 1) maxVisibleRows = 1;

            g_app.visibleRows = std::min(g_app.rows, (size_t)maxVisibleRows);
            if (g_app.visibleRows < 1) g_app.visibleRows = 1;
            g_app.pageCount = (g_app.rows + g_app.visibleRows - 1) / g_app.visibleRows;
            g_app.currentPage = 0;
            g_app.RecalculatePageForSelectionLocked();

            int contentWidth = static_cast<int>(g_app.iconsPerRow * itemCellWidth - padding);
            windowWidth = static_cast<int>(contentWidth + 2 * padding);
            windowHeight = static_cast<int>(g_app.visibleRows * itemCellHeight + padding);
        }

        if (windowWidth > (int)(screenWidth * 0.9))
            windowWidth = (int)(screenWidth * 0.9);

        int x = screenX + (screenWidth  - windowWidth)  / 2;
        int y = screenY + (screenHeight - windowHeight) / 2;

        HWND switcherWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            SWITCHER_CLASS_NAME, L"Switcher", WS_POPUP | WS_VISIBLE,
            x, y, windowWidth, windowHeight, NULL, NULL, GetModuleHandle(NULL), NULL);
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            g_app.switcherWnd = switcherWnd;
        }

        if (g_app.switcherWnd) {
            UpdateSwitcherLayeredWindow(g_app.switcherWnd);
            StartIconLoading(iconGeneration);
        }
    }
}

void MoveSelection(int delta) {
    HWND hwnd = NULL;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        if (g_app.windowList.empty()) return;

        size_t count = g_app.windowList.size();
        if (delta < 0) {
            size_t step = (size_t)(-delta) % count;
            g_app.selectedIndex = (g_app.selectedIndex + count - step) % count;
        } else {
            g_app.selectedIndex = (g_app.selectedIndex + (size_t)delta) % count;
        }
        g_app.RecalculatePageForSelectionLocked();
        hwnd = g_app.switcherWnd;
    }

    if (hwnd) UpdateSwitcherLayeredWindow(hwnd);
}

void ChangePage(int delta) {
    HWND hwnd = NULL;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        if (g_app.pageCount <= 1 || g_app.iconsPerRow == 0 || g_app.visibleRows == 0)
            return;

        size_t oldPage = g_app.currentPage;
        if (delta < 0) {
            g_app.currentPage = (g_app.currentPage == 0) ? g_app.pageCount - 1 : g_app.currentPage - 1;
        } else {
            g_app.currentPage = (g_app.currentPage + 1) % g_app.pageCount;
        }

        if (g_app.currentPage != oldPage) {
            size_t firstRow = g_app.currentPage * g_app.visibleRows;
            size_t firstIndex = firstRow * g_app.iconsPerRow;
            if (firstIndex < g_app.windowList.size())
                g_app.selectedIndex = firstIndex;
            else
                g_app.selectedIndex = g_app.windowList.size() - 1;
        }
        hwnd = g_app.switcherWnd;
    }

    if (hwnd) UpdateSwitcherLayeredWindow(hwnd);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pkbhs = (KBDLLHOOKSTRUCT*)lParam;
        bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        if (pkbhs->vkCode == VK_LMENU || pkbhs->vkCode == VK_RMENU) {
            if (keyDown) {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                g_app.altDown = true;
            } else if (keyUp) {
                HWND switcherWnd = NULL;
                HWND hwndToActivate = NULL;
                HWND mainWnd = NULL;

                {
                    std::lock_guard<std::mutex> lock(g_app.stateMutex);
                    g_app.altDown = false;
                    switcherWnd = g_app.switcherWnd;
                    mainWnd = g_app.mainWnd;

                    if (switcherWnd && g_app.selectedIndex < g_app.windowList.size()) {
                        hwndToActivate = g_app.windowList[g_app.selectedIndex].hwnd;

                        // FIX: Заполняем список окон для сворачивания ДО DestroyWindow.
                        // DestroyWindow вызывает WM_NCDESTROY синхронно и немедленно
                        // очищает g_app.windowList - после него список уже недоступен.
                        // PostMessage асинхронен и будет обработан позже в главном цикле.
                        g_app.windowsToMinimize.clear();
                        for (const auto& wi : g_app.windowList) {
                            if (wi.hwnd != hwndToActivate)
                                g_app.windowsToMinimize.push_back(wi.hwnd);
                        }
                    }
                }

                if (hwndToActivate)
                    PostMessage(mainWnd, WM_APP_SWITCH_TO_WINDOW, (WPARAM)hwndToActivate, 0);
                if (switcherWnd) {
                    DestroyWindow(switcherWnd); // очищает g_app.windowList - но нам уже не нужен
                }
            }
        }

        bool altDown = false;
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            altDown = g_app.altDown;
        }

        if (altDown && keyDown) {
            if (pkbhs->vkCode == VK_PRIOR) { ChangePage(-1); return 1; }
            if (pkbhs->vkCode == VK_NEXT)  { ChangePage(1);  return 1; }

            bool backwards = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (pkbhs->vkCode == VK_TAB)   { ShowSwitcher(false, backwards); return 1; }
            if (pkbhs->vkCode == VK_OEM_3) { ShowSwitcher(true, backwards);  return 1; }
        }

        // Escape закрывает переключатель без переключения (с любым состоянием Alt)
        if (pkbhs->vkCode == VK_ESCAPE && keyDown) {
            HWND switcherWnd = NULL;
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                switcherWnd = g_app.switcherWnd;
                if (switcherWnd)
                    g_app.windowsToMinimize.clear(); // сворачивать ничего не нужно
            }
            if (switcherWnd) {
                DestroyWindow(switcherWnd);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_app.keyboardHook, nCode, wParam, lParam);
}

// --- Icon and Window Enumeration Helpers ---

Gdiplus::Bitmap* GetBestIconForProcess(const wchar_t* exePath, HWND associatedHwnd) {
    HICON hIcon = NULL;
    bool  ownIcon = false; // TRUE только если иконка создана нами (нужен DestroyIcon)

    if (exePath && exePath[0] != L'\0') {
        // LOAD_LIBRARY_AS_IMAGE_RESOURCE нужен для PE с нестандартным выравниванием секций
        HMODULE hModule = LoadLibraryExW(exePath, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (hModule) {
            wchar_t groupName[256] = {0};
            EnumResourceNamesW(hModule, RT_GROUP_ICON,
                [](HMODULE, LPCWSTR, LPWSTR lpName, LONG_PTR lParam) -> BOOL {
                    if (IS_INTRESOURCE(lpName)) wsprintfW((wchar_t*)lParam, L"#%d", (int)(intptr_t)lpName);
                    else                         wcscpy_s((wchar_t*)lParam, 256, lpName);
                    return FALSE; // останавливаемся на первой группе
                }, (LONG_PTR)groupName);

            if (groupName[0] != L'\0') {
                HRSRC   hResource = FindResourceW(hModule, groupName, RT_GROUP_ICON);
                if (hResource) {
                    HGLOBAL hMem     = LoadResource(hModule, hResource);
                    void*   pIconDir = hMem ? LockResource(hMem) : NULL;
                    if (pIconDir) {
                        int   iconId      = LookupIconIdFromDirectoryEx((PBYTE)pIconDir, TRUE, 256, 256, LR_DEFAULTCOLOR);
                        HRSRC hIconResource = FindResourceW(hModule, MAKEINTRESOURCEW(iconId), RT_ICON);
                        if (hIconResource) {
                            HGLOBAL hIconMem  = LoadResource(hModule, hIconResource);
                            void*   pIconData = hIconMem ? LockResource(hIconMem) : NULL;
                            if (pIconData) {
                                hIcon = CreateIconFromResourceEx((PBYTE)pIconData,
                                    SizeofResource(hModule, hIconResource),
                                    TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
                                if (hIcon) ownIcon = true; // мы создали иконку - мы её и уничтожаем
                            }
                        }
                    }
                }
            }
            FreeLibrary(hModule);
        }
    }

    if (!hIcon) {
        // WM_GETICON возвращает shared-иконку - DestroyIcon вызывать нельзя (ownIcon остаётся false)
        DWORD_PTR dwResult = 0;
        int timeoutMs = 50;
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            timeoutMs = g_app.settings.iconTimeoutMs;
        }
        if (SendMessageTimeout(associatedHwnd, WM_GETICON, ICON_BIG, 0,
                               SMTO_ABORTIFHUNG, timeoutMs, &dwResult) && dwResult)
            hIcon = (HICON)(HANDLE)dwResult;
    }
    if (!hIcon) {
        // ICON_SMALL2 - иконка в реальном разрешении без масштабирования
        DWORD_PTR dwResult = 0;
        int timeoutMs = 50;
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            timeoutMs = g_app.settings.iconTimeoutMs;
        }
        if (SendMessageTimeout(associatedHwnd, WM_GETICON, ICON_SMALL2, 0,
                               SMTO_ABORTIFHUNG, timeoutMs, &dwResult) && dwResult)
            hIcon = (HICON)(HANDLE)dwResult;
    }
    if (!hIcon) {
        // Последний резерв: иконка, прописанная в классе окна (тоже shared)
        hIcon = (HICON)(HANDLE)GetClassLongPtrW(associatedHwnd, GCLP_HICON);
    }
    // ownIcon остаётся false для всех трёх путей выше - shared-иконки не уничтожаем

    if (hIcon) {
        ICONINFO ii = {0};
        if (GetIconInfo(hIcon, &ii)) {
            BITMAP bm = {0};
            if (!ii.hbmColor || GetObject(ii.hbmColor, sizeof(bm), &bm) == 0
                || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);

                const int fallbackSize = 32;
                Gdiplus::Bitmap* fallback = new Gdiplus::Bitmap(fallbackSize, fallbackSize, PixelFormat32bppARGB);
                Gdiplus::Graphics graphics(fallback);
                graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
                HDC hdc = graphics.GetHDC();
                if (hdc) {
                    DrawIconEx(hdc, 0, 0, hIcon, fallbackSize, fallbackSize, 0, NULL, DI_NORMAL);
                    graphics.ReleaseHDC(hdc);
                }

                if (ownIcon) DestroyIcon(hIcon);
                return fallback;
            }

            Gdiplus::Bitmap* result = new Gdiplus::Bitmap(bm.bmWidth, bm.bmHeight, PixelFormat32bppARGB);
            Gdiplus::Rect    rect(0, 0, bm.bmWidth, bm.bmHeight);
            Gdiplus::BitmapData bmpData;

            if (result->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
                // Копируем построчно, учитывая что stride в GDI+ и bmWidthBytes у HBITMAP
                // могут различаться (хотя для 32bpp обычно совпадают).
                std::vector<BYTE> colorBits(bm.bmWidthBytes * bm.bmHeight);
                GetBitmapBits(ii.hbmColor, (LONG)colorBits.size(), colorBits.data());

                for (int row = 0; row < bm.bmHeight; ++row) {
                    const BYTE* srcRow = colorBits.data() + row * bm.bmWidthBytes;
                    BYTE*       dstRow = (BYTE*)bmpData.Scan0 + row * bmpData.Stride;
                    memcpy(dstRow, srcRow, bm.bmWidth * sizeof(UINT32));
                }

                // Проверяем наличие альфа-канала.
                bool hasAlpha = false;
                for (int row = 0; row < bm.bmHeight && !hasAlpha; ++row) {
                    const UINT32* rowPtr = (const UINT32*)((BYTE*)bmpData.Scan0 + row * bmpData.Stride);
                    for (int col = 0; col < bm.bmWidth; ++col) {
                        if (rowPtr[col] & 0xFF000000) { hasAlpha = true; break; }
                    }
                }

                // Если альфы нет - строим маску прозрачности из маскового битмапа.
                if (!hasAlpha && ii.hbmMask) {
                    BITMAP bmMask = {0};
                    GetObject(ii.hbmMask, sizeof(bmMask), &bmMask);
                    std::vector<BYTE> maskBits(bmMask.bmWidthBytes * bmMask.bmHeight);
                    GetBitmapBits(ii.hbmMask, (LONG)maskBits.size(), maskBits.data());

                    for (int row = 0; row < bm.bmHeight; ++row) {
                        UINT32* dstRow = (UINT32*)((BYTE*)bmpData.Scan0 + row * bmpData.Stride);
                        for (int col = 0; col < bm.bmWidth; ++col) {
                            // FIX: правильное индексирование с учётом bmWidthBytes (выравнивание строк).
                            // Старый код: maskBits[i/8] - игнорировал padding в конце каждой строки.
                            int byteIdx = row * bmMask.bmWidthBytes + col / 8;
                            int bitIdx  = 7 - (col % 8);
                            if ((maskBits[byteIdx] >> bitIdx) & 1)
                                dstRow[col] &= 0x00FFFFFF; // прозрачный
                            else
                                dstRow[col] |= 0xFF000000; // непрозрачный
                        }
                    }
                }

                result->UnlockBits(&bmpData);
            }

            DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ownIcon) DestroyIcon(hIcon); // shared-иконки (WM_GETICON, GCLP_HICON) не трогаем
            return result;
        }
        if (ownIcon) DestroyIcon(hIcon); // GetIconInfo не удался, но иконка наша - освобождаем
    }

    // Пустая заглушка, если иконку добыть не удалось.
    return new Gdiplus::Bitmap(32, 32, PixelFormat32bppARGB);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD currentPID = 0;
    GetWindowThreadProcessId(hwnd, &currentPID);

    EnumData* pData = (EnumData*)lParam;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    wchar_t className[64] = {0};
    GetClassNameW(hwnd, className, 64);
    bool isApplicationFrameWindow = (wcscmp(className, L"ApplicationFrameWindow") == 0);

    WindowFilterInput filterInput = {};
    filterInput.isSelfWindow = (hwnd == g_app.switcherWnd || hwnd == g_app.mainWnd);
    filterInput.isVisible = IsWindowVisible(hwnd) != FALSE;
    filterInput.hasTitle = GetWindowTextLengthW(hwnd) > 0;
    filterInput.hasOwner = GetWindow(hwnd, GW_OWNER) != 0;
    filterInput.isToolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;
    filterInput.isCloaked = (cloaked != 0);
    filterInput.isApplicationFrameWindow = isApplicationFrameWindow;
    filterInput.hasApplicationFrameCoreWindow =
        !isApplicationFrameWindow || FindWindowExW(hwnd, NULL, L"Windows.UI.Core.CoreWindow", NULL) != NULL;
    filterInput.targetPID = pData->targetPID;
    filterInput.currentPID = currentPID;

    if (!ShouldIncludeWindowInSwitcher(filterInput))
        return TRUE;

    std::wstring processPath = GetProcessImagePath(currentPID);

    // Динамический буфер: длина уже проверена (> 0), но может быть > 255
    int titleLen = GetWindowTextLengthW(hwnd);
    std::wstring title(titleLen + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], titleLen + 1);
    title.resize(titleLen);

    pData->list->push_back({ hwnd, currentPID, title, processPath, NULL });

    return TRUE;
}

void StartIconLoading(unsigned int generation) {
    std::vector<WindowInfo> jobs;
    HWND mainWnd = NULL;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        jobs = g_app.windowList;
        mainWnd = g_app.mainWnd;
    }

    std::thread worker([jobs, generation, mainWnd]() {
        for (const auto& job : jobs) {
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                if (generation != g_app.iconGeneration)
                    return;
            }

            Gdiplus::Bitmap* icon = GetBestIconForProcess(job.processPath.c_str(), job.hwnd);
            HWND redrawWnd = NULL;
            bool accepted = false;

            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                if (generation == g_app.iconGeneration) {
                    for (auto& window : g_app.windowList) {
                        if (window.hwnd == job.hwnd && window.processId == job.processId) {
                            if (window.pIconBitmap) delete window.pIconBitmap;
                            window.pIconBitmap = icon;
                            icon = NULL;
                            redrawWnd = g_app.switcherWnd;
                            accepted = true;
                            break;
                        }
                    }
                }
            }

            if (icon) delete icon;
            if (accepted && redrawWnd && mainWnd)
                PostMessageW(mainWnd, WM_APP_ICON_READY, 0, 0);
        }
    });

    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.iconWorker = std::move(worker);
    }
}

void StopIconWorker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.CancelIconLoadingLocked();
        if (g_app.iconWorker.joinable()
            && g_app.iconWorker.get_id() != std::this_thread::get_id())
            worker = std::move(g_app.iconWorker);
    }

    if (worker.joinable())
        worker.join();
}


void AddRoundRectToPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, Gdiplus::REAL radius) {
    if (radius <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    Gdiplus::REAL diameter = radius * 2.0f;
    path.AddArc(rect.X,                         rect.Y,                          diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight()  - diameter,    rect.Y,                          diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight()  - diameter,    rect.GetBottom() - diameter,     diameter, diameter,   0, 90);
    path.AddArc(rect.X,                         rect.GetBottom() - diameter,     diameter, diameter,  90, 90);
    path.CloseFigure();
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
        SetProcessDpiAwarenessContextFn setContext =
            (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;
    }

    SetProcessDPIAware();
}

UINT GetDpiForMonitorOrSystem(HMONITOR monitor) {
    UINT dpiX = 96;
    UINT dpiY = 96;

    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
        GetDpiForMonitorFn getDpiForMonitor =
            (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor");
        if (getDpiForMonitor && SUCCEEDED(getDpiForMonitor(monitor, 0, &dpiX, &dpiY))) {
            FreeLibrary(shcore);
            return dpiX;
        }
        FreeLibrary(shcore);
    }

    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
        dpiX = GetDeviceCaps(hdcScreen, LOGPIXELSX);
        ReleaseDC(NULL, hdcScreen);
    }
    return dpiX;
}

std::wstring GetProcessImagePath(DWORD processId) {
    std::wstring path(MAX_PATH, L'\0');

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (hProcess) {
        DWORD size = (DWORD)path.size();
        if (QueryFullProcessImageNameW(hProcess, 0, &path[0], &size)) {
            path.resize(size);
            CloseHandle(hProcess);
            return path;
        }
        CloseHandle(hProcess);
    }

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (hProcess) {
        DWORD copied = GetModuleFileNameExW(hProcess, NULL, &path[0], (DWORD)path.size());
        CloseHandle(hProcess);
        if (copied > 0) {
            path.resize(copied);
            return path;
        }
    }

    return L"";
}

int ReadIniIntClamped(const wchar_t* iniPath, const wchar_t* key, int defaultValue, int minValue, int maxValue) {
    int value = GetPrivateProfileIntW(INI_SECTION_SETTINGS, key, defaultValue, iniPath);
    if (value < minValue) value = minValue;
    if (value > maxValue) value = maxValue;
    return value;
}

void ReadSettings() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    PathRemoveFileSpecW(iniPath);
    PathAppendW(iniPath, INI_FILENAME);

    AppSettings settings;
    settings.alpha = (BYTE)ReadIniIntClamped(iniPath, INI_KEY_TRANSPARENCY, DEFAULT_TRANSPARENCY, 0, 255);
    settings.minimizeOthers =
        GetPrivateProfileIntW(INI_SECTION_SETTINGS, INI_KEY_MINIMIZE_OTHERS, DEFAULT_MINIMIZE_OTHERS, iniPath) != 0;

    int bgR = ReadIniIntClamped(iniPath, INI_KEY_BG_R, 40, 0, 255);
    int bgG = ReadIniIntClamped(iniPath, INI_KEY_BG_G, 40, 0, 255);
    int bgB = ReadIniIntClamped(iniPath, INI_KEY_BG_B, 45, 0, 255);
    int bgA = ReadIniIntClamped(iniPath, INI_KEY_BG_A, 220, 0, 255);
    settings.bgColor = Gdiplus::Color((BYTE)bgA, (BYTE)bgR, (BYTE)bgG, (BYTE)bgB);

    int selR = ReadIniIntClamped(iniPath, INI_KEY_SEL_R, 120, 0, 255);
    int selG = ReadIniIntClamped(iniPath, INI_KEY_SEL_G, 120, 0, 255);
    int selB = ReadIniIntClamped(iniPath, INI_KEY_SEL_B, 140, 0, 255);
    int selA = ReadIniIntClamped(iniPath, INI_KEY_SEL_A, 80, 0, 255);
    settings.selectionColor = Gdiplus::Color((BYTE)selA, (BYTE)selR, (BYTE)selG, (BYTE)selB);

    int textR = ReadIniIntClamped(iniPath, INI_KEY_TEXT_R, 255, 0, 255);
    int textG = ReadIniIntClamped(iniPath, INI_KEY_TEXT_G, 255, 0, 255);
    int textB = ReadIniIntClamped(iniPath, INI_KEY_TEXT_B, 255, 0, 255);
    settings.textColor = Gdiplus::Color(255, (BYTE)textR, (BYTE)textG, (BYTE)textB);

    settings.windowRadius = ReadIniIntClamped(iniPath, INI_KEY_WINDOW_RADIUS, 12, 0, 64);
    settings.selectionRadius = ReadIniIntClamped(iniPath, INI_KEY_SELECTION_RADIUS, 6, 0, 64);
    settings.iconTimeoutMs = ReadIniIntClamped(iniPath, INI_KEY_ICON_TIMEOUT_MS, 50, 1, 2000);

    std::lock_guard<std::mutex> lock(g_app.stateMutex);
    g_app.settings = settings;
}

// ---------------------------------------------------------------------------
// Системный трей
// ---------------------------------------------------------------------------

// Создаёт 16×16 HBITMAP с одним Unicode-символом из Segoe MDL2 Assets
// (или Segoe UI Symbol как резерв) в заданном цвете.
// Возвращает pre-multiplied ARGB bitmap, пригодный для SetMenuItemBitmaps.
// Вызывающий обязан вызвать DeleteObject() по окончании.
HBITMAP CreateMenuIconBitmap(HDC hdcRef, const wchar_t* glyph, COLORREF color, int size) {
    HDC     hdcMem  = CreateCompatibleDC(hdcRef);
    HBITMAP hBitmap = NULL;
    if (!hdcMem) return NULL;

    // Создаём 32bpp DIB с альфа-каналом
    BITMAPINFO bmi  = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    hBitmap = CreateDIBSection(hdcRef, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) { DeleteDC(hdcMem); return NULL; }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);
    if (!hOldBmp) {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        return NULL;
    }

    if (pBits)
        ZeroMemory(pBits, size * size * sizeof(DWORD));

    // Пробуем Segoe MDL2 Assets (Win10+), потом Segoe UI Symbol (Win8/8.1)
    const wchar_t* fontNames[] = { L"Segoe MDL2 Assets", L"Segoe UI Symbol" };
    HFONT hFont = NULL;
    for (int fi = 0; fi < 2 && !hFont; ++fi) {
        hFont = CreateFontW(
            size - 2, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            fontNames[fi]);
    }
    if (!hFont) {
        SelectObject(hdcMem, hOldBmp);
        DeleteDC(hdcMem);
        DeleteObject(hBitmap);
        return NULL;
    }

    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));

    RECT rcText = { 0, 1, size, size };
    DrawTextW(hdcMem, glyph, -1, &rcText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);

    // GDI рисует без alpha, поэтому сначала рисуем белую маску,
    // затем перекрашиваем её в нужный системный цвет меню.
    if (pBits) {
        DWORD* px = (DWORD*)pBits;
        BYTE targetR = GetRValue(color);
        BYTE targetG = GetGValue(color);
        BYTE targetB = GetBValue(color);

        for (int i = 0; i < size * size; ++i) {
            BYTE b = (BYTE)(px[i] & 0xFF);
            BYTE g = (BYTE)((px[i] >> 8) & 0xFF);
            BYTE r = (BYTE)((px[i] >> 16) & 0xFF);
            // Яркость как alpha (max канала - лучший вариант для иконочных шрифтов)
            BYTE a = std::max(r, std::max(g, b));

            BYTE premulR = (BYTE)((targetR * a + 127) / 255);
            BYTE premulG = (BYTE)((targetG * a + 127) / 255);
            BYTE premulB = (BYTE)((targetB * a + 127) / 255);

            px[i] = ((DWORD)a << 24)
                  | ((DWORD)premulR << 16)
                  | ((DWORD)premulG << 8)
                  | ((DWORD)premulB);
        }
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    return hBitmap;
}

// Регистрирует иконку в области уведомлений.
void AddTrayIcon(HWND hwnd, HICON hIcon) {
    ZeroMemory(&g_app.trayIcon, sizeof(g_app.trayIcon));
    g_app.trayIcon.cbSize           = sizeof(NOTIFYICONDATAW);
    g_app.trayIcon.hWnd             = hwnd;
    g_app.trayIcon.uID              = TRAY_UID;
    g_app.trayIcon.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.trayIcon.uCallbackMessage = WM_APP_TRAY;
    g_app.trayIcon.hIcon            = hIcon;
    // Подсказка при наведении
    wcscpy_s(g_app.trayIcon.szTip, L"TabWindSwitcher\n"
                                    L"Alt+Tab - все окна\n"
                                    L"Alt+` - окна приложения");
    Shell_NotifyIconW(NIM_ADD, &g_app.trayIcon);
}

// Удаляет иконку из области уведомлений.
void RemoveTrayIcon() {
    if (g_app.trayIcon.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.trayIcon);
        g_app.trayIcon.hWnd = NULL;
    }
}

// Строит и показывает контекстное меню трея с пиктограммами из Segoe MDL2 Assets.
// Глифы подобраны по таблице MDL2 (docs.microsoft.com/en-us/windows/apps/design/style/segoe-ui-symbol-font):
//   U+E946  Information (ⓘ)   → About
//   U+E8C8  Key               → Горячие клавиши
//   U+E8A7  CheckMark/Accept  → Сворачивать: Вкл
//   U+E711  Cancel/No         → Сворачивать: Выкл
//   U+E7E8  Leave / Exit      → Выход
void ShowTrayMenu(HWND hwnd) {
    // Получаем DC экрана для создания совместимых битмапов
    HDC hdcScreen = GetDC(NULL);

    // Цвет иконок: берём системный цвет текста меню
    COLORREF clrIcon = GetSysColor(COLOR_MENUTEXT);

    const int ICON_SZ = 16;

    // Глифы Segoe MDL2 Assets
    HBITMAP hbmAbout    = CreateMenuIconBitmap(hdcScreen, L"\xE946", clrIcon, ICON_SZ); // Information
    HBITMAP hbmHotkeys  = CreateMenuIconBitmap(hdcScreen, L"\xE8C8", clrIcon, ICON_SZ); // Key
    HBITMAP hbmMinOn    = CreateMenuIconBitmap(hdcScreen, L"\xE8A7", clrIcon, ICON_SZ); // Accept / Check
    HBITMAP hbmMinOff   = CreateMenuIconBitmap(hdcScreen, L"\xE711", clrIcon, ICON_SZ); // Cancel
    HBITMAP hbmExit     = CreateMenuIconBitmap(hdcScreen, L"\xE7E8", clrIcon, ICON_SZ); // Leave

    ReleaseDC(NULL, hdcScreen);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        if (hbmAbout)   DeleteObject(hbmAbout);
        if (hbmHotkeys) DeleteObject(hbmHotkeys);
        if (hbmMinOn)   DeleteObject(hbmMinOn);
        if (hbmMinOff)  DeleteObject(hbmMinOff);
        if (hbmExit)    DeleteObject(hbmExit);
        return;
    }

    // --- Заголовок (не кликабельный, серый) ---
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                L"TabWindSwitcher v1.0");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // --- About ---
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_ABOUT, L"О программе");
    if (hbmAbout) SetMenuItemBitmaps(hMenu, IDM_TRAY_ABOUT, MF_BYCOMMAND, hbmAbout, hbmAbout);

    // --- Горячие клавиши ---
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HOTKEYS, L"Горячие клавиши…");
    if (hbmHotkeys) SetMenuItemBitmaps(hMenu, IDM_TRAY_HOTKEYS, MF_BYCOMMAND, hbmHotkeys, hbmHotkeys);

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // --- Сворачивать остальные окна: переключатель ---
    bool minimizeOthers = false;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        minimizeOthers = g_app.settings.minimizeOthers;
    }

    if (minimizeOthers) {
        // Сейчас включено - предлагаем выключить
        AppendMenuW(hMenu, MF_STRING | MF_CHECKED, IDM_TRAY_MINIMIZE_ON,
                    L"Сворачивать остальные окна");
        if (hbmMinOn)
            SetMenuItemBitmaps(hMenu, IDM_TRAY_MINIMIZE_ON, MF_BYCOMMAND, hbmMinOn, hbmMinOn);
    } else {
        // Сейчас выключено - предлагаем включить
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_MINIMIZE_OFF,
                    L"Сворачивать остальные окна");
        if (hbmMinOff)
            SetMenuItemBitmaps(hMenu, IDM_TRAY_MINIMIZE_OFF, MF_BYCOMMAND, hbmMinOff, hbmMinOff);
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // --- Выход ---
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Выход");
    if (hbmExit) SetMenuItemBitmaps(hMenu, IDM_TRAY_EXIT, MF_BYCOMMAND, hbmExit, hbmExit);

    // Трей-меню требует SetForegroundWindow + PostMessage(WM_NULL) для корректного закрытия
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    int cmd = TrackPopupMenuEx(hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
        pt.x, pt.y, hwnd, NULL);

    DestroyMenu(hMenu);

    // Освобождаем битмапы ПОСЛЕ DestroyMenu - меню уже не рисует их
    if (hbmAbout)   DeleteObject(hbmAbout);
    if (hbmHotkeys) DeleteObject(hbmHotkeys);
    if (hbmMinOn)   DeleteObject(hbmMinOn);
    if (hbmMinOff)  DeleteObject(hbmMinOff);
    if (hbmExit)    DeleteObject(hbmExit);

    PostMessageW(hwnd, WM_NULL, 0, 0); // сбрасываем фокус после TrackPopupMenu

    // --- Обработка выбора ---
    switch (cmd) {
        case IDM_TRAY_ABOUT:
            MessageBoxW(hwnd,
                L"TabWindSwitcher v1.0\n\n"
                L"Переключатель окон с тонкой настройкой.\n\n"
                L"Alt + Tab    - все окна\n"
                L"Alt + `      - окна текущего приложения\n"
                L"Escape       - отмена",
                L"О программе", MB_OK | MB_ICONINFORMATION);
            break;

        case IDM_TRAY_HOTKEYS:
            MessageBoxW(hwnd,
                L"Горячие клавиши TabWindSwitcher:\n\n"
                L"  Alt + Tab       Переключатель всех открытых окон\n"
                L"  Alt + `         Переключатель окон текущего приложения\n"
                L"  Escape          Закрыть переключатель без переключения\n\n"
                L"Удерживайте Alt - переключатель остаётся открытым.\n"
                L"Отпустите Alt   - активируется выбранное окно.",
                L"Горячие клавиши", MB_OK | MB_ICONINFORMATION);
            break;

        case IDM_TRAY_MINIMIZE_ON:
        case IDM_TRAY_MINIMIZE_OFF: {
            // Переключаем флаг и сохраняем в INI
            bool newMinimizeOthers = false;
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                g_app.settings.minimizeOthers = !g_app.settings.minimizeOthers;
                newMinimizeOthers = g_app.settings.minimizeOthers;
            }

            wchar_t iniPath[MAX_PATH];
            GetModuleFileNameW(NULL, iniPath, MAX_PATH);
            PathRemoveFileSpecW(iniPath);
            PathAppendW(iniPath, INI_FILENAME);
            WritePrivateProfileStringW(INI_SECTION_SETTINGS, INI_KEY_MINIMIZE_OTHERS,
                                       newMinimizeOthers ? L"1" : L"0", iniPath);
            break;
        }

        case IDM_TRAY_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
    }
}
