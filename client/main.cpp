#include "../common/net_utils.h"

int main() {
    if (!init_network()) return 1;

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) {
        printf("socket 失败\n");
        return 1;
    }

    // 故意把发送缓冲区调小，方便看到拆分
    int sndbuf = 4096;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("connect 失败，错误码 %d\n", WSAGetLastError());
        return 1;
    }
    printf("[client] 已连接\n\n");

    // 1. 发一条大消息，制造多次 send / 多次 recv
    std::string big(1024 * 1024, 'A');
    send_message(fd, big);

    // 2. 再连发三条小消息
    send_message(fd, "hello");
    send_message(fd, "world");
    send_message(fd, "protobuf");

    CLOSE_SOCKET(fd);
    cleanup_network();
    return 0;
}