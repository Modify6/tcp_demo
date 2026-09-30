#include "../common/net_utils.h"
#include <iostream>

// 与发送方完全一致的数据结构
struct Player {
    int32_t id;
    float x;
    float y;
    char name[32];
};

// 手动反序列化：从字节流还原 Player 对象
Player deserialize(const std::string& buf) {
    Player p;
    size_t offset = 0; // 偏移量，记录我们读到哪了
    
    // 1. 读 id
    uint32_t net_id;
    memcpy(&net_id, buf.data() + offset, 4);
    p.id = static_cast<int32_t>(ntohl(net_id));
    offset += 4;
    
    // 2. 读 x
    uint32_t net_x;
    memcpy(&net_x, buf.data() + offset, 4);
    uint32_t host_x = ntohl(net_x);
    memcpy(&p.x, &host_x, 4); // 把主机序的 uint32_t 内存原样拷回 float
    offset += 4;
    
    // 3. 读 y
    uint32_t net_y;
    memcpy(&net_y, buf.data() + offset, 4);
    uint32_t host_y = ntohl(net_y);
    memcpy(&p.y, &host_y, 4);
    offset += 4;
    
    // 4. 读 name (定长 32 字节，原样拷贝)
    memcpy(p.name, buf.data() + offset, 32);
    p.name[31] = '\0'; // 安全起见，强制加上字符串结束符，防止越界读取
    offset += 32;
    
    return p;
}

// 接收一条完整消息：先读 4 字节长度，再读 payload
bool recv_packet(socket_t fd, std::string& payload) {
    uint32_t net_len;
    if (!recv_all(fd, &net_len, 4)) return false;
    uint32_t len = ntohl(net_len);
    
    payload.resize(len);
    return recv_all(fd, payload.data(), len);
}

int main() {
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif
    if (!init_network()) return 1;

    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCK) { printf("socket 失败\n"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind 失败，错误码 %d\n", WSAGetLastError());
        return 1;
    }
    listen(listen_fd, 1);
    printf("[server] 监听 9000 端口，等待连接...\n");

    socket_t fd = accept(listen_fd, nullptr, nullptr);
    printf("[server] 客户端已连接\n\n");

    std::string payload;
    if (recv_packet(fd, payload)) {
        printf("[server] 收到字节流长度: %zu\n", payload.size());

        // 手动反序列化
        Player p = deserialize(payload);

        printf("  -> ID:   %d\n", p.id);
        printf("  -> X:    %.2f\n", p.x);
        printf("  -> Y:    %.2f\n", p.y);
        printf("  -> Name: %s\n", p.name);
    } else {
        printf("[server] 接收失败或客户端断开连接\n");
    }

    printf("\n[server] 按回车键退出服务端...\n");
    std::cin.get();

    CLOSE_SOCKET(fd);
    CLOSE_SOCKET(listen_fd);
    cleanup_network();
    return 0;
}