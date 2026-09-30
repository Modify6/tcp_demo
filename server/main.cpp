#include "../common/net_utils.h"
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>

#define MAX_BUFFER_SIZE 4096
#define SERVER_PORT 9000

// =========================================================================
// 全局统计（多线程共享，必须用 atomic 或 mutex 保护）
// =========================================================================
std::atomic<int> g_clientIdCounter{ 0 };   // 客户端 ID 递增计数器
std::atomic<int> g_onlineClients{ 0 };     // 当前在线客户端数
std::atomic<long long> g_totalMessages{ 0 }; // 服务器累计收到的消息数
std::atomic<long long> g_totalBytes{ 0 };    // 服务器累计收到的字节数

// 用于保护 std::cout，防止多线程打印交错
std::mutex g_logMutex;

// =========================================================================
// 获取当前时间字符串，格式: 2026-10-01 12:34:56
// =========================================================================
std::string NowString() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

// 加锁打印，防止多线程输出混乱
template<typename... Args>
void LogPrint(Args&&... args) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    (std::cout << ... << args) << std::endl;
}

// =========================================================================
// IO 上下文（每次异步操作使用）
// =========================================================================
enum IO_TYPE { IO_ACCEPT, IO_RECV };

struct ClientContext; // 前向声明

struct IOContext {
    OVERLAPPED overlapped;       // 必须放在首位
    WSABUF wsabuf;
    char buffer[MAX_BUFFER_SIZE];
    IO_TYPE ioType;
    SOCKET clientSocket;
    ClientContext* clientCtx;

    IOContext() {
        ZeroMemory(&overlapped, sizeof(overlapped));
        ZeroMemory(buffer, MAX_BUFFER_SIZE);
        wsabuf.buf = buffer;
        wsabuf.len = MAX_BUFFER_SIZE;
        ioType = IO_RECV;
        clientSocket = INVALID_SOCKET;
        clientCtx = nullptr;
    }
};

// =========================================================================
// 客户端上下文（每个客户端长期存在）
// =========================================================================
struct ClientContext {
    int id;                     // 客户端唯一 ID
    SOCKET socket;              // 客户端 Socket
    std::string ip;             // 客户端 IP
    int port;                   // 客户端端口
    std::string connectTime;    // 连接建立时间
    std::string recvBuffer;     // 粘包/拆包缓冲区

    // 客户端统计
    long long msgCount = 0;     // 该客户端累计消息数
    long long byteCount = 0;    // 该客户端累计字节数

    ClientContext(SOCKET s) : socket(s) {
        id = ++g_clientIdCounter;
        connectTime = NowString();
    }
};

// =========================================================================
// 全局 IOCP 与监听 Socket
// =========================================================================
HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
SOCKET g_listenSocket = INVALID_SOCKET;

// =========================================================================
// 从 recvBuffer 切出一条完整消息
// =========================================================================
bool TryExtractMessage(ClientContext* ctx, std::string& outPayload) {
    if (ctx->recvBuffer.size() < 4) return false;

    uint32_t net_len;
    memcpy(&net_len, ctx->recvBuffer.data(), 4);
    uint32_t len = ntohl(net_len);

    if (ctx->recvBuffer.size() < 4 + len) return false;

    outPayload.assign(ctx->recvBuffer.data() + 4, len);
    ctx->recvBuffer.erase(0, 4 + len);
    return true;
}

// =========================================================================
// 投递异步 Accept
// =========================================================================
bool PostAccept() {
    IOContext* ioCtx = new IOContext();
    ioCtx->ioType = IO_ACCEPT;
    ioCtx->clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ioCtx->clientSocket == INVALID_SOCKET) {
        delete ioCtx;
        return false;
    }

    DWORD bytesReceived = 0;
    BOOL ok = AcceptEx(
        g_listenSocket,
        ioCtx->clientSocket,
        ioCtx->buffer,
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytesReceived,
        &ioCtx->overlapped
    );

    if (ok == FALSE) {
        int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            LogPrint("[ERROR] AcceptEx 失败: ", err);
            closesocket(ioCtx->clientSocket);
            delete ioCtx;
            return false;
        }
    }
    return true;
}

// =========================================================================
// 投递异步 Recv
// =========================================================================
bool PostRecv(ClientContext* clientCtx) {
    IOContext* ioCtx = new IOContext();
    ioCtx->ioType = IO_RECV;
    ioCtx->clientSocket = clientCtx->socket;
    ioCtx->clientCtx = clientCtx;

    DWORD flags = 0;
    DWORD bytesReceived = 0;

    int ret = WSARecv(
        clientCtx->socket,
        &ioCtx->wsabuf,
        1,
        &bytesReceived,
        &flags,
        &ioCtx->overlapped,
        NULL
    );

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            LogPrint("[ERROR] WSARecv 失败 (Client#", clientCtx->id, "): ", err);
            delete ioCtx;
            return false;
        }
    }
    return true;
}

// =========================================================================
// 工作线程
// =========================================================================
DWORD WINAPI WorkerThread(LPVOID) {
    while (true) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED lpOverlapped = NULL;

        BOOL ok = GetQueuedCompletionStatus(
            g_hIOCP,
            &bytesTransferred,
            &completionKey,
            &lpOverlapped,
            INFINITE
        );

        if (!ok) {
            LogPrint("[ERROR] GetQueuedCompletionStatus 失败: ", GetLastError());
            continue;
        }

        IOContext* ioCtx = CONTAINING_RECORD(lpOverlapped, IOContext, overlapped);

        switch (ioCtx->ioType) {
            // =============================================================
            // Accept 完成：新客户端接入
            // =============================================================
        case IO_ACCEPT: {
            ClientContext* clientCtx = new ClientContext(ioCtx->clientSocket);

            // 获取客户端 IP 和端口
            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            getpeername(ioCtx->clientSocket, (sockaddr*)&clientAddr, &addrLen);
            char ipBuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
            clientCtx->ip = ipBuf;
            clientCtx->port = ntohs(clientAddr.sin_port);

            // 绑定新客户端到 IOCP
            CreateIoCompletionPort(
                (HANDLE)ioCtx->clientSocket,
                g_hIOCP,
                (ULONG_PTR)clientCtx,
                0
            );

            g_onlineClients++;

            LogPrint("============================================================");
            LogPrint("[CONNECT] 新客户端接入");
            LogPrint("  -> 客户端 ID  : #", clientCtx->id);
            LogPrint("  -> IP 地址    : ", clientCtx->ip);
            LogPrint("  -> 端口       : ", clientCtx->port);
            LogPrint("  -> 连接时间   : ", clientCtx->connectTime);
            LogPrint("  -> 当前在线数 : ", g_onlineClients.load());
            LogPrint("============================================================");

            PostRecv(clientCtx);
            PostAccept();

            delete ioCtx;
            break;
        }

                      // =============================================================
                      // Recv 完成：收到数据
                      // =============================================================
        case IO_RECV: {
            ClientContext* clientCtx = ioCtx->clientCtx;

            // 客户端断开
            if (bytesTransferred == 0) {
                g_onlineClients--;

                LogPrint("============================================================");
                LogPrint("[DISCONNECT] 客户端断开");
                LogPrint("  -> 客户端 ID  : #", clientCtx->id);
                LogPrint("  -> IP 地址    : ", clientCtx->ip, ":", clientCtx->port);
                LogPrint("  -> 累计消息数 : ", clientCtx->msgCount);
                LogPrint("  -> 累计字节数 : ", clientCtx->byteCount);
                LogPrint("  -> 当前在线数 : ", g_onlineClients.load());
                LogPrint("============================================================");

                closesocket(clientCtx->socket);
                delete clientCtx;
                delete ioCtx;
                continue;
            }

            // 更新字节统计
            clientCtx->byteCount += bytesTransferred;
            g_totalBytes += bytesTransferred;

            // 追加到该客户端的 recvBuffer
            clientCtx->recvBuffer.append(ioCtx->buffer, bytesTransferred);

            // 循环切出所有完整消息
            std::string payload;
            while (TryExtractMessage(clientCtx, payload)) {
                Player p = deserialize(payload);

                clientCtx->msgCount++;
                g_totalMessages++;

                LogPrint("------------------------------------------------------------");
                LogPrint("[MESSAGE] 收到来自 Client#", clientCtx->id,
                    " (", clientCtx->ip, ":", clientCtx->port, ") 的消息");
                LogPrint("  -> 消息序号   : 第 ", clientCtx->msgCount, " 条");
                LogPrint("  -> ID         : ", p.id);
                LogPrint("  -> Level      : ", p.level);
                LogPrint("  -> X          : ", p.x);
                LogPrint("  -> Y          : ", p.y);
                LogPrint("  -> HP         : ", p.hp);
                LogPrint("  -> MP         : ", p.mp);
                LogPrint("  -> Name       : ", p.name);
                LogPrint("  -> Guild      : ", p.guild);
                LogPrint("  -> 时间       : ", NowString());
                LogPrint("  -> 全局统计   : 在线 ", g_onlineClients.load(),
                    " 个客户端, 累计消息 ", g_totalMessages.load(),
                    " 条, 累计字节 ", g_totalBytes.load());
                LogPrint("------------------------------------------------------------");
            }

            PostRecv(clientCtx);
            delete ioCtx;
            break;
        }
        }
    }
    return 0;
}

// =========================================================================
// 主函数
// =========================================================================
int main() {
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif
    if (!init_network()) return 1;

    // 1. 创建监听 Socket
    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(g_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LogPrint("[ERROR] bind 失败: ", WSAGetLastError());
        return 1;
    }
    listen(g_listenSocket, SOMAXCONN);

    // 2. 创建完成端口
    g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_hIOCP == NULL) {
        LogPrint("[ERROR] CreateIoCompletionPort 失败");
        return 1;
    }

    // 3. 绑定监听 Socket 到 IOCP
    CreateIoCompletionPort((HANDLE)g_listenSocket, g_hIOCP, (ULONG_PTR)g_listenSocket, 0);

    // 4. 预投递 Accept
    for (int i = 0; i < 4; ++i) {
        PostAccept();
    }

    // 5. 创建工作线程池
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int threadCount = sysInfo.dwNumberOfProcessors * 2;
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(WorkerThread, nullptr);
    }

    LogPrint("============================================================");
    LogPrint("[IOCP] 多客户端服务器已启动");
    LogPrint("  -> 监听端口   : ", SERVER_PORT);
    LogPrint("  -> 工作线程数 : ", threadCount);
    LogPrint("  -> 启动时间   : ", NowString());
    LogPrint("  -> 等待客户端连接...");
    LogPrint("============================================================");

    // 6. 主线程等待
    std::cin.get();

    // 7. 清理
    for (auto& t : threads) {
        if (t.joinable()) t.detach();
    }
    closesocket(g_listenSocket);
    CloseHandle(g_hIOCP);
    cleanup_network();
    return 0;
}