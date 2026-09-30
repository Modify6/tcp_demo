#include "../common/net_utils.h"
#include <iostream>
#include <thread>
#include <vector>
#include <string>

//整体架构图
//┌──────────────────────────────────────────────┐
//│               主线程(main)                   │
//│  1. 创建监听 Socket                           │
//│  2. 创建 IOCP 完成端口                         │
//│  3. 把监听 Socket 绑定到 IOCP                  │
//│  4. 预投递 4 个 AcceptEx                       │
//│  5. 创建 N 个工作线程                          │
//│  6. 阻塞等待退出                              │
//└──────────────────────────────────────────────┘
//                                            │
//                                            │ 创建 N 个
//                                            ▼
//┌──────────────────────────────────────────────┐
//│      工作线程池(WorkerThread × N)             │
//│  循环 : GetQueuedCompletionStatus              │
//│        ├─ IO_ACCEPT → 绑定新客户端 + PostRecv │
//│        └─ IO_RECV   → 切包 + 反序列化 + PostRecv│
//└──────────────────────────────────────────────┘
//                                             ▲
//                                             │ 完成事件
//                                             │
//┌──────────────────────────────────────────────┐
//│          内核 IOCP 完成队列                    │
//│  由 Windows 内核维护，保存所有 I / O 完成事件     │
//└──────────────────────────────────────────────┘
//                                              ▲
//                                              │ 异步投递
//                                              │
//┌──────────────────────────────────────────────┐
//│  AcceptEx / WSARecv 异步操作                  │
//└──────────────────────────────────────────────┘

#define MAX_BUFFER_SIZE 4096
#define SERVER_PORT 9000

// ================= 上下文结构 =================
enum IO_TYPE { IO_ACCEPT, IO_RECV };

struct ClientContext;

struct IOContext {
    OVERLAPPED overlapped;       // 必须放在首位
    WSABUF wsabuf;
    char buffer[MAX_BUFFER_SIZE];
    IO_TYPE ioType;
    SOCKET clientSocket;
    ClientContext* clientCtx;    // 指向该客户端的上下文（Recv 时使用）

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

struct ClientContext {
    SOCKET socket;
    std::string recvBuffer;  // 每个客户端独立的粘包/拆包缓冲区

    ClientContext(SOCKET s) : socket(s) {}
};

// ================= 全局变量 =================
HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
SOCKET g_listenSocket = INVALID_SOCKET;

// ================= 从 recvBuffer 里切出一条完整消息 =================
// 协议: [4字节长度头(网络序)] [payload]
// 返回 true 说明切出一条完整消息，outPayload 里是 payload
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

// ================= 投递异步 Accept =================
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
            std::cerr << "AcceptEx 失败: " << err << std::endl;
            closesocket(ioCtx->clientSocket);
            delete ioCtx;
            return false;
        }
    }
    return true;
}

// ================= 投递异步 Recv =================
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
            std::cerr << "WSARecv 失败: " << err << std::endl;
            delete ioCtx;
            return false;
        }
    }
    return true;
}

// ================= 工作线程 =================
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
            std::cerr << "GetQueuedCompletionStatus 失败: " << GetLastError() << std::endl;
            continue;
        }

        // 反推 IOContext
        IOContext* ioCtx = CONTAINING_RECORD(lpOverlapped, IOContext, overlapped);

        switch (ioCtx->ioType) {
        case IO_ACCEPT: {
            // 新客户端连接
            ClientContext* clientCtx = new ClientContext(ioCtx->clientSocket);

            // 绑定到 IOCP，completionKey 用 clientCtx 指针
            CreateIoCompletionPort(
                (HANDLE)ioCtx->clientSocket,
                g_hIOCP,
                (ULONG_PTR)clientCtx,
                0
            );

            std::cout << "[IOCP] 接受新客户端连接" << std::endl;

            // 为该客户端投递第一次 Recv
            PostRecv(clientCtx);

            // 重新投递 Accept，等待下一个客户端
            PostAccept();

            // 释放本次 Accept 的 IOContext（注意：clientSocket 已交给 clientCtx，不要 close）
            delete ioCtx;
            break;
        }
        case IO_RECV: {
            ClientContext* clientCtx = ioCtx->clientCtx;

            // 客户端断开
            if (bytesTransferred == 0) {
                std::cout << "[IOCP] 客户端断开连接" << std::endl;
                closesocket(clientCtx->socket);
                delete clientCtx;
                delete ioCtx;
                continue;
            }

            // 1. 把本次收到的数据追加到该客户端的 recvBuffer
            clientCtx->recvBuffer.append(ioCtx->buffer, bytesTransferred);

            // 2. 循环切出所有完整消息
            std::string payload;
            while (TryExtractMessage(clientCtx, payload)) {
                // 3. 反序列化（和阻塞版本完全一样！）
                Player p = deserialize(payload);

                // 4. 处理业务
                std::cout << "[IOCP] 收到一条 Player 消息:" << std::endl;
                std::cout << "  -> ID:    " << p.id << std::endl;
                std::cout << "  -> Level: " << p.level << std::endl;
                std::cout << "  -> X:     " << p.x << std::endl;
                std::cout << "  -> Y:     " << p.y << std::endl;
                std::cout << "  -> HP:    " << p.hp << std::endl;
                std::cout << "  -> MP:    " << p.mp << std::endl;
                std::cout << "  -> Name:  " << p.name << std::endl;
                std::cout << "  -> Guild: " << p.guild << std::endl;
                std::cout << "----------------------------------------" << std::endl;
            }

            // 5. 继续投递下一次 Recv
            PostRecv(clientCtx);

            // 6. 释放本次 Recv 的 IOContext
            delete ioCtx;
            break;
        }
        }
    }
    return 0;
}

// ================= 主函数 =================
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
        std::cerr << "bind 失败: " << WSAGetLastError() << std::endl;
        return 1;
    }
    listen(g_listenSocket, SOMAXCONN);
    std::cout << "[IOCP] 服务器监听端口 " << SERVER_PORT << std::endl;

    // 2. 创建完成端口
    g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_hIOCP == NULL) {
        std::cerr << "CreateIoCompletionPort 失败" << std::endl;
        return 1;
    }

    // 3. 将监听 Socket 绑定到完成端口（completionKey 传监听 Socket）
    CreateIoCompletionPort((HANDLE)g_listenSocket, g_hIOCP, (ULONG_PTR)g_listenSocket, 0);

    // 4. 预投递多个 Accept
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
    std::cout << "[IOCP] 启动 " << threadCount << " 个工作线程" << std::endl;

    // 6. 主线程等待
    std::cout << "[IOCP] 按回车键退出..." << std::endl;
    std::cin.get();

    // 7. 清理（简化处理）
    for (auto& t : threads) {
        if (t.joinable()) t.detach();
    }
    closesocket(g_listenSocket);
    CloseHandle(g_hIOCP);
    cleanup_network();
    return 0;
}