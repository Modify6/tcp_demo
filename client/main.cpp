#include "../common/net_utils.h"
#include <iostream>

// 与接收方约定的数据结构
struct Player {
    int32_t id;         // 4 字节整数
    float x;            // 4 字节浮点数
    float y;            // 4 字节浮点数
    char name[32];      // 定长 32 字节字符数组
};

// 手动序列化：把 Player 对象变成字节流
std::string serialize(const Player& p) {
    std::string buf;
    
    // 1. 写入 id (4字节，转网络序)
    uint32_t net_id = htonl(static_cast<uint32_t>(p.id));
    buf.append(reinterpret_cast<const char*>(&net_id), 4);
    
    // 2. 写入 x (4字节，浮点 -> 内存 -> 网络序)
    uint32_t temp_x;
    memcpy(&temp_x, &p.x, 4);          // 把 float 的内存原样拷进 uint32_t
    uint32_t net_x = htonl(temp_x);    // 转网络序
    buf.append(reinterpret_cast<const char*>(&net_x), 4);
    
    // 3. 写入 y (同上)
    uint32_t temp_y;
    memcpy(&temp_y, &p.y, 4);
    uint32_t net_y = htonl(temp_y);
    buf.append(reinterpret_cast<const char*>(&net_y), 4);
    
    // 4. 写入 name (定长 32 字节，不需要转端序，原样追加)
    // 注意：这里固定发满 32 字节，即使 name 后面有空字符，也一起发过去，保证结构对齐
    buf.append(p.name, 32); 
    
    return buf;
}

// 发送一条完整消息：4 字节长度头 + 实际 payload
bool send_packet(socket_t fd, const std::string& payload) {
    uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
    if (!send_all(fd, &net_len, 4)) return false;
    if (!send_all(fd, payload.data(), payload.size())) return false;
    return true;
}

int main() {
#ifdef _WIN32
    system("chcp 65001 > nul"); // 解决 Windows 控制台中文乱码
#endif
    if (!init_network()) return 1;

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) { printf("socket 失败\n"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("connect 失败，错误码 %d\n", WSAGetLastError());
        return 1;
    }
    printf("[client] 已连接服务器\n\n");

    // 构建结构体数据
    Player p;
    p.id = 1001;
    p.x = 12.5f;
    p.y = 30.2f;
    // 安全地填充字符串，防止内存垃圾
    strncpy(p.name, "Alice", sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0'; // 强制结尾

    // 1. 手动序列化
    std::string payload = serialize(p);
    printf("[client] 序列化后长度: %zu 字节\n", payload.size());

    // 2. 发送
    if (send_packet(fd, payload)) {
        printf("[client] 发送成功\n");
    } else {
        printf("[client] 发送失败\n");
    }

    printf("\n[client] 按回车键退出客户端...\n");
    std::cin.get(); // 防止程序直接退出

    CLOSE_SOCKET(fd);
    cleanup_network();
    return 0;
}