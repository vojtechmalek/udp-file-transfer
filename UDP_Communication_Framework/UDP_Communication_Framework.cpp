// ============================================================================
// INCLUDES
// ============================================================================

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#include "stdafx.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include "UDPCore.h"
#include <stdarg.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <set>
#include <vector>
#include <random>
#include <chrono>
#include <string>
#include <cstdio>
#include <algorithm>

// ============================================================================
// CONSTANTS AND DEFINITIONS
// ============================================================================

#define BUFFERS_LEN 1024
#define HEADER_SIZE 21
#define DATA_SIZE (BUFFERS_LEN - HEADER_SIZE)
#define DEFAULT_TIMEOUT_MS 500

#define FILE_CHAR 'f'
#define MESSAGE_CHAR 'm'
#define ACK_CHAR 'a'
#define NAK_CHAR 'n'

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static LogCallback g_logCallback = nullptr;
static std::thread* g_receiveThread = nullptr;
static bool g_receiveThreadRunning = false;
static SOCKET g_globalSocket = INVALID_SOCKET;
static char g_bufferRx[BUFFERS_LEN];
static char g_bufferTx[BUFFERS_LEN];
static sockaddr_in g_from = {};
static int g_fromLen = sizeof(g_from);

static std::string g_remoteIp;
static int g_remotePort;

// ACK/NAK waiting mechanism - store multiple ACKs and NAKs per transfer ID
static std::map<uint32_t, std::set<uint32_t>> g_pendingACKs; // transferId -> set of ACKed packet indices
static std::map<uint32_t, std::set<uint32_t>> g_pendingNAKs; // transferId -> set of NAKed packet indices
static std::mutex g_ackMutex;
static std::condition_variable g_ackCondition;

static uint32_t g_windowSize = DEFAULT_WINDOW_SIZE;

// ============================================================================
// LOGGING SYSTEM
// ============================================================================

static void LogMessage(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (g_logCallback) {
        g_logCallback(buffer);
    } else {
        printf("%s\n", buffer);
    }
}

// ============================================================================
// DATA STRUCTURES
// ============================================================================

#pragma pack(push, 1)
struct Packet {
    uint32_t transferId;  // ID identifying the file/message transfer
    char dataType;
    uint32_t totalPackets;
    uint32_t packetIndex;  // Index of this packet within the transfer
    uint32_t dataLength;
    uint32_t crc32;
    char data[DATA_SIZE];
};
#pragma pack(pop)

struct UnackedPacket {
    Packet packet;
    std::chrono::steady_clock::time_point sendTime;  // When packet was sent (for timeout)
    bool isNew;  // True if just added and needs to be sent immediately
};

struct SenderWindow {
    uint32_t windowSize;
    uint32_t nextSeqNum;      // Next sequence number to send (for new packets)
    std::map<uint32_t, UnackedPacket> unackedPackets;  // Map of unacknowledged packets with timers
};

struct ReceiverBuffer {
    std::map<uint32_t, Packet> bufferedPackets;
    uint32_t nextExpectedSeq;
    std::set<uint32_t> receivedPackets;
};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void CheckReceivedMessage(SOCKET socketS, char bufferRx[BUFFERS_LEN], sockaddr_in& from, int& fromLen);
void SendPayload(SOCKET socketS, char bufferTx[BUFFERS_LEN], std::string targetIp, int targetPort, char command, std::string argument);
void HandleIncomingData(SOCKET socketS, char bufferRx[BUFFERS_LEN], sockaddr_in& from, int& fromLen, Packet& firstPacket);

// ============================================================================
// NETWORK INITIALIZATION
// ============================================================================

void InitWinsock() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

// ============================================================================
// GUI WRAPPER FUNCTIONS
// ============================================================================

void InitUDPCore(LogCallback logCallback) {
    g_logCallback = logCallback;
    InitWinsock();
}

void SetupUDPSocket(const std::string& targetIp, int targetPort, int localPort, SOCKET* outSocket) {
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(localPort);
    local.sin_addr.s_addr = INADDR_ANY;

    SOCKET socketS = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketS == INVALID_SOCKET) {
        LogMessage("Socket creation failed: %d", WSAGetLastError());
        *outSocket = INVALID_SOCKET;
        return;
    }

    if (bind(socketS, (sockaddr*)&local, sizeof(local)) != 0) {
        int error = WSAGetLastError();
        LogMessage("Binding error on port %d: %d", localPort, error);
        closesocket(socketS);
        *outSocket = INVALID_SOCKET;
        return;
    }

    g_globalSocket = socketS;
    *outSocket = socketS;
    g_remoteIp = targetIp;
    g_remotePort = targetPort;
}

void CleanupUDPCore(SOCKET socket) {
    StopReceivingThread();
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
    }
    WSACleanup();
}

void SendMessage(SOCKET socket, const std::string& targetIp, int targetPort, const std::string& message) {
    SendPayload(socket, g_bufferTx, targetIp, targetPort, MESSAGE_CHAR, message);
}

void SendFile(SOCKET socket, const std::string& targetIp, int targetPort, const std::string& filePath) {
    SendPayload(socket, g_bufferTx, targetIp, targetPort, FILE_CHAR, filePath);
}

void StartReceivingThread(SOCKET socket) {
    if (g_receiveThreadRunning) {
        return;
    }
    g_receiveThreadRunning = true;
    g_globalSocket = socket;
    g_receiveThread = new std::thread([]() {
        CheckReceivedMessage(g_globalSocket, g_bufferRx, std::ref(g_from), std::ref(g_fromLen));
    });
    g_receiveThread->detach();
}

void StopReceivingThread() {
    g_receiveThreadRunning = false;
}

void EnableSelectiveRepeat(bool enable, uint32_t windowSize) {
    if (enable) {
        g_windowSize = (windowSize < MIN_WINDOW_SIZE) ? MIN_WINDOW_SIZE :
                       (windowSize > MAX_WINDOW_SIZE) ? MAX_WINDOW_SIZE : windowSize;
    } else {
        g_windowSize = 1; // When "disabled", use window size 1 (stop-and-wait behavior)
    }
    LogMessage("Window size set to %u", g_windowSize);
}

// ============================================================================
// PACKET UTILITIES
// ============================================================================

uint32_t GenerateTransferId() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);
    return dist(rng);
}

void SendPacket(SOCKET socketS, char bufferTx[BUFFERS_LEN], const Packet& packet, const std::string& targetIp, int targetPort) {
    sockaddr_in addrDest = {};
    addrDest.sin_family = AF_INET;
    addrDest.sin_port = htons(targetPort);
    inet_pton(AF_INET, targetIp.c_str(), &addrDest.sin_addr);

    memcpy(bufferTx, &packet, sizeof(Packet));
    if (sendto(socketS, bufferTx, sizeof(Packet), 0, (sockaddr*)&addrDest, sizeof(addrDest)) == SOCKET_ERROR) {
        LogMessage("Send error: %d", WSAGetLastError());
    } else {
        LogMessage("SENT PACKET: TransferID=%u, Type=%c, Index=%u/%u, DataLen=%u -> %s:%d", 
                   packet.transferId, packet.dataType, packet.packetIndex, packet.totalPackets - 1, packet.dataLength, targetIp.c_str(), targetPort);
    }
}

// ============================================================================
// CRC FUNCTIONS
// ============================================================================

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

uint32_t CalculateCRC32(const char* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ (uint8_t)data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

bool VerifyPacketCRC(const Packet& packet) {
    return CalculateCRC32(packet.data, packet.dataLength) == packet.crc32;
}

// ============================================================================
// HASH FUNCTIONS
// ============================================================================

static std::string ComputeSHA256(const BYTE* data, size_t dataLength) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE rgbHash[32];
    DWORD cbHash = 32;
    std::string hashString;
    const char hexDigits[] = "0123456789abcdef";

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    if (!CryptHashData(hHash, data, (DWORD)dataLength, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    if (!CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    for (DWORD i = 0; i < cbHash; i++) {
        hashString += hexDigits[rgbHash[i] >> 4];
        hashString += hexDigits[rgbHash[i] & 0xf];
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return hashString;
}

std::string CalculateFileHash(const std::string& filepath) {
    FILE* file = fopen(filepath.c_str(), "rb");
    if (!file) {
        return "";
    }

    BYTE buffer[4096];
    std::vector<BYTE> fileData;
    size_t bytesRead;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        fileData.insert(fileData.end(), buffer, buffer + bytesRead);
    }
    fclose(file);

    if (fileData.empty()) {
        return "";
    }

    return ComputeSHA256(fileData.data(), fileData.size());
}

// ============================================================================
// ACK/NAK FUNCTIONS
// ============================================================================

void SendACKToAddress(SOCKET socketS, char bufferTx[BUFFERS_LEN], sockaddr_in& targetAddr, uint32_t transferId, uint32_t packetIndex) {
    Packet ackPacket = {};
    ackPacket.transferId = transferId;
    ackPacket.dataType = ACK_CHAR;
    ackPacket.totalPackets = 1;
    ackPacket.packetIndex = packetIndex;
    ackPacket.dataLength = 0;
    ackPacket.crc32 = CalculateCRC32(ackPacket.data, ackPacket.dataLength);

    if (g_remotePort != 0) {
        targetAddr.sin_port = htons(g_remotePort);
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &targetAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
    int port = ntohs(targetAddr.sin_port);

    memcpy(bufferTx, &ackPacket, sizeof(Packet));
    sendto(socketS, bufferTx, sizeof(Packet), 0, (sockaddr*)&targetAddr, sizeof(targetAddr));
    LogMessage("SENT ACK: TransferID=%u, PacketIndex=%u -> %s:%d", transferId, packetIndex, ipStr, port);
}

void SendNAKToAddress(SOCKET socketS, char bufferTx[BUFFERS_LEN], sockaddr_in& targetAddr, uint32_t transferId, uint32_t packetIndex) {
    Packet nakPacket = {};
    nakPacket.transferId = transferId;
    nakPacket.dataType = NAK_CHAR;
    nakPacket.totalPackets = 1;
    nakPacket.packetIndex = packetIndex;
    nakPacket.dataLength = 0;
    nakPacket.crc32 = CalculateCRC32(nakPacket.data, nakPacket.dataLength);

    if (g_remotePort != 0) {
        targetAddr.sin_port = htons(g_remotePort);
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &targetAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
    int port = ntohs(targetAddr.sin_port);

    memcpy(bufferTx, &nakPacket, sizeof(Packet));
    sendto(socketS, bufferTx, sizeof(Packet), 0, (sockaddr*)&targetAddr, sizeof(targetAddr));
    LogMessage("SENT NAK: TransferID=%u, PacketIndex=%u -> %s:%d", transferId, packetIndex, ipStr, port);
}

// ============================================================================
// PACKET PREPARATION
// ============================================================================

static bool PrepareFilePackets(const std::string& filePath, uint32_t transferId, char dataType, std::vector<Packet>& outPackets) {
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        LogMessage("File not found: %s", filePath.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string filename = filePath.substr(filePath.find_last_of("/\\") + 1);
    std::string fileHash = CalculateFileHash(filePath);
    if (fileHash.empty()) {
        LogMessage("Failed to calculate file hash");
        fclose(f);
        return false;
    }

    std::string filenameWithHash = filename + "|" + fileHash;
    if (filenameWithHash.size() > DATA_SIZE) {
        LogMessage("Filename + hash too long (max %d bytes)", DATA_SIZE);
        fclose(f);
        return false;
    }

    uint32_t totalPackets = (uint32_t)((fileSize + DATA_SIZE - 1) / DATA_SIZE) + 1;
    outPackets.clear();
    outPackets.reserve(totalPackets);

    // Filename packet
    Packet filenamePacket = {};
    filenamePacket.transferId = transferId;
    filenamePacket.dataType = dataType;
    filenamePacket.totalPackets = totalPackets;
    filenamePacket.packetIndex = 0;
    filenamePacket.dataLength = (uint32_t)filenameWithHash.size();
    memcpy(filenamePacket.data, filenameWithHash.data(), filenamePacket.dataLength);
    filenamePacket.crc32 = CalculateCRC32(filenamePacket.data, filenamePacket.dataLength);
    outPackets.push_back(filenamePacket);

    // Data packets
    for (uint32_t i = 1; i < totalPackets; i++) {
        Packet dataPacket = {};
        dataPacket.transferId = transferId;
        dataPacket.dataType = dataType;
        dataPacket.totalPackets = totalPackets;
        dataPacket.packetIndex = i;
        dataPacket.dataLength = (uint32_t)fread(dataPacket.data, 1, DATA_SIZE, f);
        dataPacket.crc32 = CalculateCRC32(dataPacket.data, dataPacket.dataLength);
        outPackets.push_back(dataPacket);
    }

    fclose(f);
    return true;
}

static void PrepareMessagePackets(const std::string& message, uint32_t transferId, char dataType, std::vector<Packet>& outPackets) {
    // Calculate hash of entire message for integrity checking
    std::string messageHash = ComputeSHA256((const BYTE*)message.data(), message.length());
    if (messageHash.empty()) {
        LogMessage("Failed to calculate message hash");
        return;
    }
    
    // Calculate number of data packets needed (packet 0 is hash, data starts at packet 1)
    uint32_t dataPackets = (uint32_t)((message.length() + DATA_SIZE - 1) / DATA_SIZE);
    uint32_t totalPackets = 1 + dataPackets;  // 1 for hash packet + data packets
    
    outPackets.clear();
    outPackets.reserve(totalPackets);

    // Packet 0: contains message hash (similar to files having filename|hash)
    Packet packet0 = {};
    packet0.transferId = transferId;
    packet0.dataType = dataType;
    packet0.totalPackets = totalPackets;
    packet0.packetIndex = 0;
    packet0.dataLength = (uint32_t)messageHash.size();
    if (packet0.dataLength > DATA_SIZE) {
        LogMessage("Message hash too long (max %d bytes)", DATA_SIZE);
        return;
    }
    memcpy(packet0.data, messageHash.data(), packet0.dataLength);
    packet0.crc32 = CalculateCRC32(packet0.data, packet0.dataLength);
    outPackets.push_back(packet0);

    // Data packets (starting from index 1)
    for (uint32_t i = 1; i < totalPackets; i++) {
        Packet msgPacket = {};
        msgPacket.transferId = transferId;
        msgPacket.dataType = dataType;
        msgPacket.totalPackets = totalPackets;
        msgPacket.packetIndex = i;
        size_t offset = (i - 1) * DATA_SIZE;  // Adjust offset since packet 0 is hash
        msgPacket.dataLength = (uint32_t)(std::min)(message.size() - offset, (size_t)DATA_SIZE);
        memcpy(msgPacket.data, message.data() + offset, msgPacket.dataLength);
        msgPacket.crc32 = CalculateCRC32(msgPacket.data, msgPacket.dataLength);
        outPackets.push_back(msgPacket);
    }
}

// ============================================================================
// SENDING FUNCTIONS
// ============================================================================

// Implements selective repeat
static void ExecuteSlidingWindowLoop(SOCKET socketS, char bufferTx[BUFFERS_LEN], char bufferRx[BUFFERS_LEN],
    const std::string& targetIp, int targetPort, sockaddr_in& from, int& fromLen,
    const std::vector<Packet>& allPackets, uint32_t transferId, uint32_t windowSize) {
    
    SenderWindow window;
    window.windowSize = windowSize;
    window.nextSeqNum = 0;

    uint32_t totalPackets = (uint32_t)allPackets.size();
    LogMessage("Starting transfer of %u packets (window size: %u)", totalPackets, windowSize);

    const int RESPONSE_TIMEOUT_MS = DEFAULT_TIMEOUT_MS;
    auto now = std::chrono::steady_clock::now();
    auto timeoutTime = now - std::chrono::milliseconds(RESPONSE_TIMEOUT_MS); // Set to past so new packets send immediately
    
    // Send packet 0 with stop-and-wait (critical metadata packet)
    LogMessage("Sending packet 0 (stop-and-wait for critical metadata)");
    UnackedPacket packet0;
    packet0.packet = allPackets[0];
    packet0.isNew = true;
    window.unackedPackets[0] = packet0;
    window.nextSeqNum = 1;
    
    // Wait for packet 0 to be ACKed before starting window
    bool packet0Acked = false;
    while (!packet0Acked) {
        now = std::chrono::steady_clock::now();
        
        // Check for NAK for packet 0
        std::set<uint32_t> receivedNaks;
        {
            std::lock_guard<std::mutex> lock(g_ackMutex);
            if (g_pendingNAKs.find(transferId) != g_pendingNAKs.end() && !g_pendingNAKs[transferId].empty()) {
                receivedNaks = g_pendingNAKs[transferId];
                g_pendingNAKs.erase(transferId);
            }
        }
        
        if (receivedNaks.find(0) != receivedNaks.end()) {
            LogMessage("NAK received for packet 0 - retransmitting");
            SendPacket(socketS, bufferTx, window.unackedPackets[0].packet, targetIp, targetPort);
            window.unackedPackets[0].sendTime = now;
        }
        
        // Check for ACK for packet 0
        std::set<uint32_t> receivedAcks;
        {
            std::lock_guard<std::mutex> lock(g_ackMutex);
            if (g_pendingACKs.find(transferId) != g_pendingACKs.end() && !g_pendingACKs[transferId].empty()) {
                receivedAcks = g_pendingACKs[transferId];
                g_pendingACKs.erase(transferId);
            }
        }
        
        if (receivedAcks.find(0) != receivedAcks.end()) {
            window.unackedPackets.erase(0);
            LogMessage("Packet 0 ACKed - starting selective repeat window");
            packet0Acked = true;
        } else {
            // Retransmit packet 0 if new or timeout
            if (window.unackedPackets[0].isNew || 
                (std::chrono::duration_cast<std::chrono::milliseconds>(now - window.unackedPackets[0].sendTime).count() >= RESPONSE_TIMEOUT_MS)) {
                LogMessage("Retransmitting packet 0 (%s)", window.unackedPackets[0].isNew ? "initial send" : "timeout");
                SendPacket(socketS, bufferTx, window.unackedPackets[0].packet, targetIp, targetPort);
                window.unackedPackets[0].sendTime = now;
                window.unackedPackets[0].isNew = false;
            }
            Sleep(10);
        }
    }
    
    // Initialize window with packets 1 to windowSize
    for (uint32_t i = 1; i <= windowSize && i < totalPackets; i++) {
        UnackedPacket unacked;
        unacked.packet = allPackets[i];
        unacked.isNew = true;
        window.unackedPackets[i] = unacked;
        window.nextSeqNum = i + 1;
    }
    if (window.nextSeqNum > 1) {
        LogMessage("Window initialized with packets 1-%u (%zu packets)", 
                   window.nextSeqNum - 1, window.unackedPackets.size());
    } else {
        LogMessage("Window initialized - no additional packets (only packet 0)");
    }
    
    // Continue until all packets are ACKed
    while (window.nextSeqNum < totalPackets || !window.unackedPackets.empty()) {
        now = std::chrono::steady_clock::now();
        
        // Process NAKs first
        std::set<uint32_t> receivedNaks;
        {
            std::lock_guard<std::mutex> lock(g_ackMutex);
            if (g_pendingNAKs.find(transferId) != g_pendingNAKs.end() && !g_pendingNAKs[transferId].empty()) {
                receivedNaks = g_pendingNAKs[transferId];
                g_pendingNAKs.erase(transferId);
            }
        }
        
        // Handle NAKs: send and reset timer
        for (uint32_t nakIdx : receivedNaks) {
            auto it = window.unackedPackets.find(nakIdx);
            if (it != window.unackedPackets.end()) {
                LogMessage("NAK received for packet %u - retransmitting immediately", nakIdx);
                SendPacket(socketS, bufferTx, it->second.packet, targetIp, targetPort);
                it->second.sendTime = now;
            } else {
                LogMessage("NAK received for packet %u (already ACKed) - ignoring", nakIdx);
            }
        }
        
        // Process ACKs
        std::set<uint32_t> receivedAcks;
        {
            std::lock_guard<std::mutex> lock(g_ackMutex);
            if (g_pendingACKs.find(transferId) != g_pendingACKs.end() && !g_pendingACKs[transferId].empty()) {
                receivedAcks = g_pendingACKs[transferId];
                g_pendingACKs.erase(transferId);
            }
        }
        
        // Handle ACKs: remove from unacked, add next packet if available
        for (uint32_t ackIdx : receivedAcks) {
            auto it = window.unackedPackets.find(ackIdx);
            if (it != window.unackedPackets.end()) {
                // Remove from unacked
                window.unackedPackets.erase(it);
                LogMessage("Packet %u ACKed (removed from unacked, %zu remaining)", 
                           ackIdx, window.unackedPackets.size());
                
                // Add next packet if available and window has space
                if (window.nextSeqNum < totalPackets && window.unackedPackets.size() < windowSize) {
                    UnackedPacket unacked;
                    unacked.packet = allPackets[window.nextSeqNum];
                    unacked.isNew = true;
                    window.unackedPackets[window.nextSeqNum] = unacked;
                    LogMessage("Added packet %u to window (next=%u)", 
                               window.nextSeqNum, window.nextSeqNum + 1);
                    window.nextSeqNum++;
                }
            } else {
                // Already ACKed - ignore duplicate ACK
                LogMessage("ACK for packet %u (already ACKed) - ignoring", ackIdx);
            }
        }
        
        // Loop over all unacked packets: send if new or timer expired
        for (auto& pair : window.unackedPackets) {
            if (pair.second.isNew || 
                (std::chrono::duration_cast<std::chrono::milliseconds>(now - pair.second.sendTime).count() >= RESPONSE_TIMEOUT_MS)) {
                LogMessage("Sending packet %u (%s)", pair.first, 
                           pair.second.isNew ? "new" : "timeout");
                SendPacket(socketS, bufferTx, pair.second.packet, targetIp, targetPort);
                pair.second.sendTime = now;  // Reset timer
                pair.second.isNew = false;
            }
        }
        
        // If all packets are sent and all are ACKed, we're done
        if (window.nextSeqNum >= totalPackets && window.unackedPackets.empty()) {
            LogMessage("All packets sent and ACKed - completing transfer");
            break;
        }
        
        // Small sleep to prevent busy-waiting
        Sleep(10);
    }
    
    LogMessage("All %u packets sent and ACKed successfully", totalPackets);
}

void SendPayload(SOCKET socketS, char bufferTx[BUFFERS_LEN], std::string targetIp, int targetPort, char command, std::string argument) {
    // Use selective repeat protocol with configured window size
    uint32_t transferId = GenerateTransferId();
    char bufferRx[BUFFERS_LEN];
    sockaddr_in from = {};
    int fromLen = sizeof(from);

    std::vector<Packet> allPackets;
    if (command == FILE_CHAR) {
        if (!PrepareFilePackets(argument, transferId, command, allPackets)) {
            return;
        }
    } else if (command == MESSAGE_CHAR) {
        PrepareMessagePackets(argument, transferId, command, allPackets);
    } else {
        return;
    }

    ExecuteSlidingWindowLoop(socketS, bufferTx, bufferRx, targetIp, targetPort, from, fromLen, allPackets, transferId, g_windowSize);
    LogMessage("%s transfer completed successfully", (command == FILE_CHAR) ? "File" : "Message");
}

// ============================================================================
// RECEIVING FUNCTIONS
// ============================================================================

void CheckReceivedMessage(SOCKET socketS, char bufferRx[BUFFERS_LEN], sockaddr_in& from, int& fromLen) {
    while (g_receiveThreadRunning) {
        int bytes = recvfrom(socketS, bufferRx, BUFFERS_LEN, 0, (sockaddr*)&from, &fromLen);
        if (bytes == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != 10054 && error != WSAETIMEDOUT && error != 10038 && error != 10093) {
                LogMessage("recvfrom error: %d", error);
            }
            if (error == 10038 || error == 10093) {
                break;
            }
            Sleep(10);
            continue;
        }

        if (bytes > 0 && bytes >= (int)sizeof(Packet)) {
            Packet* packet = reinterpret_cast<Packet*>(bufferRx);

            switch (packet->dataType) {
            case MESSAGE_CHAR:
            case FILE_CHAR:
                HandleIncomingData(socketS, bufferRx, from, fromLen, *packet);
                break;
            case ACK_CHAR:
            {
                // Store ACK in the set
                std::lock_guard<std::mutex> lock(g_ackMutex);
                if (VerifyPacketCRC(*packet)) {
                    g_pendingACKs[packet->transferId].insert(packet->packetIndex);
                    g_ackCondition.notify_all();
                }
            }
            break;
            case NAK_CHAR:
            {
                // Store NAK in the set
                std::lock_guard<std::mutex> lock(g_ackMutex);
                g_pendingNAKs[packet->transferId].insert(packet->packetIndex);
                g_ackCondition.notify_all();
            }
            break;
            default:
                LogMessage("Unknown packet type: %c", packet->dataType);
                break;
            }
        }
    }
}

// Receives packets, sends ACKs and NAKs, and handles out-of-order packets
void HandleIncomingData(SOCKET socketS, char bufferRx[BUFFERS_LEN], sockaddr_in& from, int& fromLen, Packet& firstPacket) {
    char bufferTx[BUFFERS_LEN];
    ReceiverBuffer buffer;
    buffer.nextExpectedSeq = 0;

    // Validate first packet (must be index 0 to start transfer)
    if (!VerifyPacketCRC(firstPacket)) {
        LogMessage("CRC verification failed for packet %u - sending NAK immediately", firstPacket.packetIndex);
        SendNAKToAddress(socketS, bufferTx, from, firstPacket.transferId, firstPacket.packetIndex);
        return;
    }

    if (firstPacket.packetIndex != 0) {
        LogMessage("Expected packet 0 first, got %u - ignoring", firstPacket.packetIndex);
        return;
    }

    // Initialize receiver state
    uint32_t transferId = firstPacket.transferId;
    uint32_t totalPackets = firstPacket.totalPackets;
    buffer.receivedPackets.insert(0);
    buffer.nextExpectedSeq = 1;

    bool isFile = (firstPacket.dataType == FILE_CHAR);
    std::string filename;
    std::string expectedHash;
    FILE* outFile = nullptr;

    // Process first packet (packet 0) - contains metadata (filename|hash for files, hash for messages)
    if (isFile) {
        std::string filenameWithHash(firstPacket.data, firstPacket.dataLength);
        size_t separatorPos = filenameWithHash.find('|');
        if (separatorPos == std::string::npos) {
            LogMessage("Invalid filename packet format");
            return;
        }
        filename = filenameWithHash.substr(0, separatorPos);
        expectedHash = filenameWithHash.substr(separatorPos + 1);
        outFile = fopen(filename.c_str(), "wb");
        if (!outFile) {
            LogMessage("Failed to create file: %s", filename.c_str());
            return;
        }
        LogMessage("Receiving file '%s' (%u packets)", filename.c_str(), totalPackets);
    } else {
        expectedHash = std::string(firstPacket.data, firstPacket.dataLength);
        LogMessage("Receiving message (%u packets, hash: %s)", totalPackets, expectedHash.c_str());
    }

    std::string fullMessage;
    if (!isFile) { // for messages, reserve space for message
        fullMessage.reserve(totalPackets * DATA_SIZE);
    }

    // Send ACK  for first packet
    SendACKToAddress(socketS, bufferTx, from, transferId, 0);

    // Timing for overall timeout
    auto startTime = std::chrono::steady_clock::now();
    const int COLLECTION_TIMEOUT_MS = 300000; // Overall timeout for receiving all packets

    LogMessage("Received initial packet 0/%u", totalPackets - 1);

    // Main receiving loop - continue until all packets received
    while (buffer.receivedPackets.size() < totalPackets) {
        auto now = std::chrono::steady_clock::now();
        
        // Check overall timeout
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count() > COLLECTION_TIMEOUT_MS) {
            LogMessage("Overall timeout waiting for all packets (%zu/%u received)", 
                       buffer.receivedPackets.size(), totalPackets);
            break;
        }

        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(socketS, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout for responsive ACKs

        if (select(0, &readfds, NULL, NULL, &timeout) > 0 && FD_ISSET(socketS, &readfds)) {
            int bytes = recvfrom(socketS, bufferRx, BUFFERS_LEN, 0, (sockaddr*)&from, &fromLen);
            if (bytes <= 0 || bytes < (int)sizeof(Packet)) {
                continue;
            }

            Packet* packet = reinterpret_cast<Packet*>(bufferRx);
            
            // Verify packet belongs to this transfer
            if (packet->transferId != transferId) {
                continue; // Different transfer, ignore
            }

            // Validate packet index
            if (packet->packetIndex >= totalPackets) {
                LogMessage("Invalid packet index %u (max: %u) - ignoring", 
                           packet->packetIndex, totalPackets - 1);
                continue;
            }

            // CRC check - send NAK immediately if failed
            if (!VerifyPacketCRC(*packet)) {
                LogMessage("CRC verification failed for packet %u - sending NAK immediately", 
                           packet->packetIndex);
                SendNAKToAddress(socketS, bufferTx, from, packet->transferId, packet->packetIndex);
                continue;
            }

            // Check if duplicate
            bool isDuplicate = (buffer.receivedPackets.find(packet->packetIndex) != buffer.receivedPackets.end());
            
            if (isDuplicate) {
                // Duplicate packet - still ACK it
                LogMessage("Duplicate packet %u received - sending ACK", packet->packetIndex);
                SendACKToAddress(socketS, bufferTx, from, packet->transferId, packet->packetIndex);
            } else {
                // New packet received
                buffer.receivedPackets.insert(packet->packetIndex);
                LogMessage("Received new packet %u/%u", packet->packetIndex, totalPackets - 1);
                
                // Send ACK for this packet
                SendACKToAddress(socketS, bufferTx, from, packet->transferId, packet->packetIndex);

                if (packet->packetIndex == buffer.nextExpectedSeq) {
                    // In-order packet - process immediately
                    if (isFile) {
                        fwrite(packet->data, 1, packet->dataLength, outFile);
                    } else {
                        fullMessage.append(packet->data, packet->dataLength);
                    }
                    buffer.nextExpectedSeq++;

                    // Process any buffered packets that are now in order
                    while (buffer.bufferedPackets.find(buffer.nextExpectedSeq) != buffer.bufferedPackets.end()) {
                        Packet& buffered = buffer.bufferedPackets[buffer.nextExpectedSeq];
                        if (isFile) {
                            fwrite(buffered.data, 1, buffered.dataLength, outFile);
                        } else {
                            fullMessage.append(buffered.data, buffered.dataLength);
                        }
                        LogMessage("Processed buffered packet %u (now in order)", buffer.nextExpectedSeq);
                        buffer.bufferedPackets.erase(buffer.nextExpectedSeq);
                        buffer.nextExpectedSeq++;
                    }
                } else {
                    // Out of order - buffer it
                    buffer.bufferedPackets[packet->packetIndex] = *packet;
                    LogMessage("Buffered out-of-order packet %u (expected %u)", 
                               packet->packetIndex, buffer.nextExpectedSeq);
                }
            }
        }
    }

    // Cleanup
    if (outFile) {
        fclose(outFile);
    }

    // Verify completion
    if (buffer.receivedPackets.size() == totalPackets) {
        LogMessage("All %u packets received successfully", totalPackets);
        if (isFile) {
            std::string receivedHash = CalculateFileHash(filename);
            if (!receivedHash.empty() && receivedHash == expectedHash) {
                LogMessage("File hash verification SUCCESS");
                char fullPath[MAX_PATH];
                if (GetFullPathNameA(filename.c_str(), MAX_PATH, fullPath, NULL)) {
                    LogMessage("File saved: %s", fullPath);
                }
            } else {
                LogMessage("File hash verification FAILED");
            }
        } else {
            // Verify message hash
            std::string receivedHash = ComputeSHA256((const BYTE*)fullMessage.data(), fullMessage.length());
            if (!receivedHash.empty() && receivedHash == expectedHash) {
                LogMessage("Message hash verification SUCCESS");
                LogMessage("Message: %s", fullMessage.c_str());
            } else {
                LogMessage("Message hash verification FAILED");
            }
        }
    } else {
        LogMessage("Incomplete transfer (%zu/%u packets received)", 
                   buffer.receivedPackets.size(), totalPackets);
    }
}

