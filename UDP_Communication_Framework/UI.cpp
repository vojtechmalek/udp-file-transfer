#include "stdafx.h"
#include "UI.h"
#include "UDPCore.h"
#include <commdlg.h>
#include <sstream>
#include <thread>

// ============================================================================
// CONSTANTS AND DEFINITIONS
// ============================================================================

static const int UI_WIDTH = 560;
static const int UI_HEIGHT = 560;
static const int UI_BOTTOM_PADDING = 10;
static const int UI_TITLE_BAR_HEIGHT = 30;
static const int UI_PROTOCOL_SECTION_HEIGHT = 50;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

// Window handles
static HWND g_hwnd = NULL;
static HWND g_hLog = NULL;
static HWND g_hTargetIP = NULL;
static HWND g_hTargetPort = NULL;
static HWND g_hLocalPort = NULL;
static HWND g_hMessage = NULL;
static HWND g_hFilePath = NULL;
static HWND g_hConnect = NULL;
static HWND g_hSelectFile = NULL;
static HWND g_hSend = NULL;
static HWND g_hSREnable = NULL;
static HWND g_hSRWindowSize = NULL;

// Application state
static SOCKET g_socket = INVALID_SOCKET;
static std::string g_targetIp;
static int g_targetPort = 0;
static int g_localPort = 0;
static bool g_connected = false;
static std::string g_selectedFile;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

// String conversion utilities
std::wstring StringToWString(const std::string& str);
std::string WStringToString(const std::wstring& wstr);

// Window text utilities
std::string GetWindowTextString(HWND hwnd);
void SetWindowTextString(HWND hwnd, const std::string& text);

// Event handlers
void OnConnect();
void OnSelectFile();
void OnSend();
void OnSelectiveRepeatToggle();
void OnWindowSizeChanged();

// ============================================================================
// STRING CONVERSION UTILITIES
// ============================================================================

std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// ============================================================================
// WINDOW TEXT UTILITIES
// ============================================================================

std::string GetWindowTextString(HWND hwnd) {
    int length = GetWindowTextLength(hwnd) + 1;
    std::wstring wstr(length, 0);
    GetWindowText(hwnd, &wstr[0], length);
    return WStringToString(wstr);
}

void SetWindowTextString(HWND hwnd, const std::string& text) {
    std::wstring wtext = StringToWString(text);
    SetWindowText(hwnd, wtext.c_str());
}

// ============================================================================
// LOGGING SYSTEM
// ============================================================================

void AddLogMessage(const char* message) {
    if (g_hLog) {
        std::wstring wmsg = StringToWString(message);
        LRESULT result = SendMessage(g_hLog, LB_ADDSTRING, 0, (LPARAM)wmsg.c_str());
        int index = (int)result;
        SendMessage(g_hLog, LB_SETTOPINDEX, index, 0); // Auto-scroll to bottom
        
        // Calculate text width and enable horizontal scrolling if needed
        HDC hdc = GetDC(g_hLog);
        if (hdc) {
            SIZE textSize;
            GetTextExtentPoint32(hdc, wmsg.c_str(), (int)wmsg.length(), &textSize);
            ReleaseDC(g_hLog, hdc);
            
            // Set horizontal extent if text is wider than current setting
            int currentExtent = (int)SendMessage(g_hLog, LB_GETHORIZONTALEXTENT, 0, 0);
            if (textSize.cx > currentExtent) {
                SendMessage(g_hLog, LB_SETHORIZONTALEXTENT, textSize.cx + 20, 0); // Add 20px padding
            }
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void OnConnect() {
    if (g_connected) {
        // Disconnect
        CleanupUDPCore(g_socket);
        g_socket = INVALID_SOCKET;
        g_connected = false;
        SetWindowText(g_hConnect, L"Connect");
        EnableWindow(g_hTargetIP, TRUE);
        EnableWindow(g_hTargetPort, TRUE);
        EnableWindow(g_hLocalPort, TRUE);
        AddLogMessage("Disconnected");
        return;
    }

    // Get and validate input values
    g_targetIp = GetWindowTextString(g_hTargetIP);
    std::string targetPortStr = GetWindowTextString(g_hTargetPort);
    std::string localPortStr = GetWindowTextString(g_hLocalPort);

    // Validate IP address
    sockaddr_in sa;
    if (inet_pton(AF_INET, g_targetIp.c_str(), &(sa.sin_addr)) != 1) {
        MessageBox(g_hwnd, L"Invalid IP address format", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Validate ports
    try {
        g_targetPort = std::stoi(targetPortStr);
        g_localPort = std::stoi(localPortStr);
        if (g_targetPort < 1 || g_targetPort > 65535 || g_localPort < 1 || g_localPort > 65535) {
            MessageBox(g_hwnd, L"Ports must be between 1 and 65535", L"Error", MB_OK | MB_ICONERROR);
            return;
        }
    } catch (...) {
        MessageBox(g_hwnd, L"Invalid port number", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Initialize UDP core
    InitUDPCore(AddLogMessage);
    
    // Apply Selective Repeat settings
    bool srEnabled = (SendMessage(g_hSREnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::string windowStr = GetWindowTextString(g_hSRWindowSize);
    uint32_t windowSize = DEFAULT_WINDOW_SIZE;
    try {
        if (!windowStr.empty()) {
            windowSize = (uint32_t)std::stoul(windowStr);
            if (windowSize < MIN_WINDOW_SIZE) windowSize = MIN_WINDOW_SIZE;
            if (windowSize > MAX_WINDOW_SIZE) windowSize = MAX_WINDOW_SIZE;
        }
    } catch (...) {
        windowSize = DEFAULT_WINDOW_SIZE;
    }
    EnableSelectiveRepeat(srEnabled, windowSize);
    
    // Setup UDP socket
    SetupUDPSocket(g_targetIp, g_targetPort, g_localPort, &g_socket);
    if (g_socket == INVALID_SOCKET) {
        MessageBox(g_hwnd, L"Failed to bind socket. Port may be in use.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Start receiving thread
    StartReceivingThread(g_socket);

    // Update UI state
    g_connected = true;
    SetWindowText(g_hConnect, L"Disconnect");
    EnableWindow(g_hTargetIP, FALSE);
    EnableWindow(g_hTargetPort, FALSE);
    EnableWindow(g_hLocalPort, FALSE);
    AddLogMessage("Connected successfully");
}

void OnSelectFile() {
    // Save current working directory to restore it later
    char currentDir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, currentDir);
    
    OPENFILENAMEA ofn;
    char szFile[260] = { 0 };

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        g_selectedFile = szFile;
        SetWindowTextString(g_hFilePath, g_selectedFile);
        AddLogMessage(("File selected: " + g_selectedFile).c_str());
    }
    
    // Restore original working directory (GetOpenFileName may change it)
    SetCurrentDirectoryA(currentDir);
}

void OnSend() {
    if (!g_connected || g_socket == INVALID_SOCKET) {
        MessageBox(g_hwnd, L"Please connect first", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    std::string message = GetWindowTextString(g_hMessage);
    std::string fileToSend = g_selectedFile;
    
    if (!fileToSend.empty()) {
        // Send file in background thread
        AddLogMessage(("Sending file: " + fileToSend).c_str());
        SOCKET socket = g_socket;
        std::string targetIp = g_targetIp;
        int targetPort = g_targetPort;
        std::thread sendThread([socket, targetIp, targetPort, fileToSend]() {
            SendFile(socket, targetIp, targetPort, fileToSend);
        });
        sendThread.detach();
        g_selectedFile.clear();
        SetWindowTextString(g_hFilePath, "No file selected");
    } else if (!message.empty()) {
        // Send message in background thread
        AddLogMessage(("Sending message: " + message).c_str());
        SOCKET socket = g_socket;
        std::string targetIp = g_targetIp;
        int targetPort = g_targetPort;
        std::thread sendThread([socket, targetIp, targetPort, message]() {
            SendMessage(socket, targetIp, targetPort, message);
        });
        sendThread.detach();
        SetWindowText(g_hMessage, L""); // Clear message box
    } else {
        MessageBox(g_hwnd, L"Please enter a message or select a file", L"Error", MB_OK | MB_ICONERROR);
    }
}

void OnSelectiveRepeatToggle() {
    bool enabled = (SendMessage(g_hSREnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(g_hSRWindowSize, enabled ? TRUE : FALSE);
    
    std::string windowStr = GetWindowTextString(g_hSRWindowSize);
    uint32_t windowSize = DEFAULT_WINDOW_SIZE;
    try {
        if (!windowStr.empty()) {
            windowSize = (uint32_t)std::stoul(windowStr);
            if (windowSize < MIN_WINDOW_SIZE) windowSize = MIN_WINDOW_SIZE;
            if (windowSize > MAX_WINDOW_SIZE) windowSize = MAX_WINDOW_SIZE;
        }
    } catch (...) {
        windowSize = DEFAULT_WINDOW_SIZE;
    }
    EnableSelectiveRepeat(enabled, windowSize);
}

void OnWindowSizeChanged() {
    bool enabled = (SendMessage(g_hSREnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (enabled) {
        std::string windowStr = GetWindowTextString(g_hSRWindowSize);
        uint32_t windowSize = DEFAULT_WINDOW_SIZE;
        try {
            if (!windowStr.empty()) {
                windowSize = (uint32_t)std::stoul(windowStr);
                if (windowSize < MIN_WINDOW_SIZE) windowSize = MIN_WINDOW_SIZE;
                if (windowSize > MAX_WINDOW_SIZE) windowSize = MAX_WINDOW_SIZE;
            }
        } catch (...) {
            windowSize = DEFAULT_WINDOW_SIZE;
        }
        EnableSelectiveRepeat(true, windowSize);
    }
}

// ============================================================================
// WINDOW PROCEDURE
// ============================================================================

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        // Connection settings section
        CreateWindowW(L"STATIC", L"Target IP:",
            WS_VISIBLE | WS_CHILD,
            10, 10, 100, 20,
            hwnd, NULL, NULL, NULL);

        g_hTargetIP = CreateWindowW(L"EDIT", L"127.0.0.1",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            10, 30, 200, 25,
            hwnd, (HMENU)IDC_TARGET_IP, NULL, NULL);

        CreateWindowW(L"STATIC", L"Target Port:",
            WS_VISIBLE | WS_CHILD,
            220, 10, 100, 20,
            hwnd, NULL, NULL, NULL);

        g_hTargetPort = CreateWindowW(L"EDIT", L"6000",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            220, 30, 100, 25,
            hwnd, (HMENU)IDC_TARGET_PORT, NULL, NULL);

        CreateWindowW(L"STATIC", L"Local Port:",
            WS_VISIBLE | WS_CHILD,
            330, 10, 100, 20,
            hwnd, NULL, NULL, NULL);

        g_hLocalPort = CreateWindowW(L"EDIT", L"5001",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            330, 30, 100, 25,
            hwnd, (HMENU)IDC_LOCAL_PORT, NULL, NULL);

        g_hConnect = CreateWindowW(L"BUTTON", L"Connect",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            440, 28, 100, 30,
            hwnd, (HMENU)IDC_CONNECT, NULL, NULL);

        // Message input section
        CreateWindowW(L"STATIC", L"Message:",
            WS_VISIBLE | WS_CHILD,
            10, 70, 100, 20,
            hwnd, NULL, NULL, NULL);

        g_hMessage = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_MULTILINE | ES_WANTRETURN,
            10, 90, 400, 60,
            hwnd, (HMENU)IDC_MESSAGE, NULL, NULL);

        // File selection section
        g_hSelectFile = CreateWindowW(L"BUTTON", L"Select File",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            10, 160, 100, 30,
            hwnd, (HMENU)IDC_SELECT_FILE, NULL, NULL);

        CreateWindowW(L"STATIC", L"Selected:",
            WS_VISIBLE | WS_CHILD,
            120, 165, 60, 20,
            hwnd, NULL, NULL, NULL);

        g_hFilePath = CreateWindowW(L"STATIC", L"No file selected",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            180, 165, 300, 20,
            hwnd, (HMENU)IDC_FILE_PATH, NULL, NULL);

        g_hSend = CreateWindowW(L"BUTTON", L"Send",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            UI_WIDTH - 140, 90, 120, 100,
            hwnd, (HMENU)IDC_SEND, NULL, NULL);

        // Activity log section
        CreateWindowW(L"STATIC", L"Activity Log:",
            WS_VISIBLE | WS_CHILD,
            10, 200, 200, 20,
            hwnd, NULL, NULL, NULL);

        int logStartY = 220;
        int logHeight = UI_HEIGHT - UI_TITLE_BAR_HEIGHT - logStartY - UI_PROTOCOL_SECTION_HEIGHT - UI_BOTTOM_PADDING;
        g_hLog = CreateWindowW(L"LISTBOX", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            10, logStartY, UI_WIDTH - 20, logHeight,
            hwnd, (HMENU)IDC_LOG, NULL, NULL);

        // Protocol settings section
        int protocolY = UI_HEIGHT - UI_TITLE_BAR_HEIGHT - UI_PROTOCOL_SECTION_HEIGHT - UI_BOTTOM_PADDING;
        CreateWindowW(L"STATIC", L"Protocol Settings:",
            WS_VISIBLE | WS_CHILD,
            10, protocolY, 150, 20,
            hwnd, NULL, NULL, NULL);

        g_hSREnable = CreateWindowW(L"BUTTON", L"Use Selective Repeat",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            10, protocolY + 20, 200, 20,
            hwnd, (HMENU)IDC_SR_ENABLE, NULL, NULL);

        CreateWindowW(L"STATIC", L"Window Size:",
            WS_VISIBLE | WS_CHILD,
            220, protocolY + 22, 90, 20,
            hwnd, NULL, NULL, NULL);

        g_hSRWindowSize = CreateWindowW(L"EDIT", L"10",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            315, protocolY + 20, 60, 20,
            hwnd, (HMENU)IDC_SR_WINDOW_SIZE, NULL, NULL);

        // Initialize Selective Repeat (disabled by default)
        EnableSelectiveRepeat(false, DEFAULT_WINDOW_SIZE);
        EnableWindow(g_hSRWindowSize, FALSE);

        AddLogMessage("UDP Communication Framework - Ready");
        break;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_CONNECT:
            OnConnect();
            break;
        case IDC_SELECT_FILE:
            OnSelectFile();
            break;
        case IDC_SEND:
            OnSend();
            break;
        case IDC_SR_ENABLE:
            OnSelectiveRepeatToggle();
            break;
        case IDC_SR_WINDOW_SIZE:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                OnWindowSizeChanged();
            }
            break;
        }
        break;
    }

    case WM_DESTROY:
        if (g_connected) {
            CleanupUDPCore(g_socket);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// WINDOW CREATION
// ============================================================================

bool CreateMainWindow(HINSTANCE hInstance, int width, int height, const wchar_t* title) {
    const wchar_t CLASS_NAME[] = L"UDPCommWindowClass";

    WNDCLASSW wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassW(&wc))
        return false;

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, title,
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd)
        return false;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!CreateMainWindow(hInstance, UI_WIDTH, UI_HEIGHT, L"UDP Communication Framework")) {
        return 1;
    }

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
