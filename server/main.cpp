#include "../common/net_utils.h"
#include <iostream>

// =========================================================================
// 1. 定义与发送方完全一致的数据结构
// =========================================================================
struct Player {
    int32_t id;
    int32_t level;
    float x;
    float y;
    float hp;
    float mp;
    char name[32];
    char guild[32];
};

// =========================================================================
// 2. 手动反序列化：从字节流还原 Player 对象
// =========================================================================
Player deserialize(const std::string& buf) {
    // 把字节流交给 ByteReader 处理
    ByteReader reader(buf);
    Player p;

    // 严格按照发送方的写入顺序读取！顺序绝不能错！
    p.id = reader.read_int32();  // 读 4 字节
    p.level = reader.read_int32();  // 读 4 字节
    p.x = reader.read_float();  // 读 4 字节
    p.y = reader.read_float();  // 读 4 字节
    p.hp = reader.read_float();  // 读 4 字节
    p.mp = reader.read_float();  // 读 4 字节
    reader.read_cstr(p.name, 32);   // 读 32 字节
    reader.read_cstr(p.guild, 32);  // 读 32 字节

    return p;
}

// =========================================================================
// 3. 接收一条完整消息：先读长度头，再读 payload
// =========================================================================
bool recv_packet(socket_t fd, std::string& payload) {
    uint32_t net_len;

    // 1. 先收 4 字节长度头（这一步必定能收满，因为 send 端保证了）
    if (!recv_all(fd, &net_len, 4)) return false;

    // 2. 将长度头从网络序转回主机序
    uint32_t len = ntohl(net_len);

    // 3. 根据长度，预先分配好接收缓冲区的大小
    payload.resize(len);

    // 4. 精确收取 len 个字节的 payload
    return recv_all(fd, payload.data(), len);
}

// =========================================================================
// 4. 主函数
// =========================================================================
int main() {
#ifdef _WIN32
    system("chcp 65001 > nul"); // 解决中文乱码
#endif

    if (!init_network()) return 1;

    // 创建监听 socket
    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    // 设置端口复用，防止服务器重启时提示“地址已被占用”
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    // 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 监听本机所有网卡
    addr.sin_port = htons(9000);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind 失败\n");
        return 1;
    }

    // 开始监听，等待队列长度为 1
    listen(listen_fd, 1);
    printf("[server] 监听 9000 端口，等待连接...\n");

    // 阻塞等待客户端连接（程序会停在这里，直到 client 启动）
    socket_t fd = accept(listen_fd, nullptr, nullptr);
    printf("[server] 客户端已连接\n\n");

    // ===== 接收并反序列化 =====
    std::string payload;
    if (recv_packet(fd, payload)) {
        printf("[server] 收到字节流长度: %zu\n", payload.size());

        // 反序列化还原成结构体
        Player p = deserialize(payload);

        // 打印结果，验证数据正确性
        printf("  -> ID:    %d\n", p.id);
        printf("  -> Level: %d\n", p.level);
        printf("  -> X:     %.2f\n", p.x);
        printf("  -> Y:     %.2f\n", p.y);
        printf("  -> HP:    %.2f\n", p.hp);
        printf("  -> MP:    %.2f\n", p.mp);
        printf("  -> Name:  %s\n", p.name);
        printf("  -> Guild: %s\n", p.guild);
    }
    else {
        printf("[server] 接收失败或客户端断开连接\n");
    }

    printf("\n[server] 按回车键退出服务端...\n");
    std::cin.get();

    // 清理资源
    CLOSE_SOCKET(fd);
    CLOSE_SOCKET(listen_fd);
    cleanup_network();
    return 0;
}