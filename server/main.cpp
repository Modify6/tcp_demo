#include "../common/net_utils.h"

int main() {
	
#ifdef _WIN32
    system("chcp 65001 > nul");
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
	
    if (!init_network()) return 1;

    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCK) {
        printf("socket 失败\n");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    // 故意把接收缓冲区调小，方便看到拆分
    int rcvbuf = 4096;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind 失败，错误码 %d\n", WSAGetLastError());
        return 1;
    }
    listen(listen_fd, 1);
    printf("[server] 监听 9000 ...\n");

    socket_t fd = accept(listen_fd, nullptr, nullptr);
    if (fd == INVALID_SOCK) {
        printf("accept 失败\n");
        return 1;
    }
    printf("[server] 客户端已连接\n\n");

    while (true) {
        std::string body;
        if (!recv_message(fd, body)) {
            printf("[server] 连接关闭\n");
            break;
        }
        std::string preview = body.size() > 40 ? body.substr(0, 40) + "..." : body;
        printf("[server] 完整消息到达，长度 %zu，内容前 40 字节：%s\n\n",
               body.size(), preview.c_str());
    }

    CLOSE_SOCKET(fd);
    CLOSE_SOCKET(listen_fd);
    cleanup_network();
    return 0;
}