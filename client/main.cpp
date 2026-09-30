#include "../common/net_utils.h"
#include <iostream>

bool send_packet(socket_t fd, const std::string& payload) {
    uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
    if (!send_all(fd, &net_len, 4)) return false;
    if (!send_all(fd, payload.data(), payload.size())) return false;
    return true;
}

int main() {
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif
    if (!init_network()) return 1;

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("connect 失败\n");
        return 1;
    }
    printf("[client] 已连接服务器\n\n");

    // 构建数据
    Player p{};
    p.id = 1001;
    p.level = 99;
    p.x = 12.5f;
    p.y = 30.2f;
    p.hp = 100.0f;
    p.mp = 50.0f;
    strncpy(p.name, "Alice", 31); p.name[31] = '\0';
    strncpy(p.guild, "DragonSlayer", 31); p.guild[31] = '\0';

    // 序列化 + 发送
    std::string payload = serialize(p);
    printf("[client] 序列化后长度: %zu 字节\n", payload.size());

    // 连发 3 条，用来验证 IOCP 服务器能否处理粘包
    for (int i = 0; i < 3; ++i) {
        p.id = 1001 + i;
        payload = serialize(p);
        send_packet(fd, payload);
        printf("[client] 发送第 %d 条消息，ID = %d\n", i + 1, p.id);
    }

    printf("\n[client] 按回车键退出...\n");
    std::cin.get();

    CLOSE_SOCKET(fd);
    cleanup_network();
    return 0;
}