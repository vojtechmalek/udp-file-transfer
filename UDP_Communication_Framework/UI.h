#pragma once
#include <windows.h>
#include <commdlg.h>
#include "UDPCore.h"

// Control IDs
#define IDC_TARGET_IP        1001
#define IDC_TARGET_PORT      1002
#define IDC_LOCAL_PORT       1003
#define IDC_CONNECT          1004
#define IDC_MESSAGE          1005
#define IDC_SELECT_FILE      1006
#define IDC_FILE_PATH        1007
#define IDC_SEND             1008
#define IDC_LOG              1009
#define IDC_SR_ENABLE        1010
#define IDC_SR_WINDOW_SIZE   1011

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Window creation
bool CreateMainWindow(HINSTANCE hInstance, int width, int height, const wchar_t* title);

// Logging function for UDP core
void AddLogMessage(const char* message);

// Helper functions
std::string GetWindowTextString(HWND hwnd);
void SetWindowTextString(HWND hwnd, const std::string& text);
std::wstring StringToWString(const std::string& str);
std::string WStringToString(const std::wstring& wstr);

