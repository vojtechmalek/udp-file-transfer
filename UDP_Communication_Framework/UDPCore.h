// Header for UI to be able to communicate with the core

#pragma once

#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#define DEFAULT_WINDOW_SIZE 10 // Default window size for Selective Repeat
#define MIN_WINDOW_SIZE 1      // Minimum window size for Selective Repeat
#define MAX_WINDOW_SIZE 1000   // Maximum window size for Selective Repeat

typedef void (*LogCallback)(const char* message);

// Initialize UDP core with logging callback (NULL = use console)
void InitUDPCore(LogCallback logCallback = nullptr);

// Network setup functions
void SetupUDPSocket(const std::string& targetIp, int targetPort, int localPort, SOCKET* outSocket);
void CleanupUDPCore(SOCKET socket);

// Sending functions
void SendMessage(SOCKET socket, const std::string& targetIp, int targetPort, const std::string& message);
void SendFile(SOCKET socket, const std::string& targetIp, int targetPort, const std::string& filePath);

// Start receiving thread (runs in background)
void StartReceivingThread(SOCKET socket);

// Stop receiving thread
void StopReceivingThread();

// Selective Repeat Protocol Configuration
void EnableSelectiveRepeat(bool enable, uint32_t windowSize = DEFAULT_WINDOW_SIZE);

