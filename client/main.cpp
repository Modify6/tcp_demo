#include "../common/net_utils.h"
#include <iostream>

// =========================================================================
// 1. 定义与接收方完全一致的数据结构
// =========================================================================
struct Player {
    int32_t id;         // 4 字节整数
    int32_t level;      // 4 字节整数
    float x;            // 4 字节浮点数
    float y;            // 4 字节浮点数
    float hp;           // 4 字节浮点数
    float mp;           // 4 字节浮点数
    char name[32];      // 定长 32 字节字符数组
    char guild[32];     // 定长 32 字节字符数组
};

// =========================================================================
// 2. 手动序列化：把 Player 对象变成字节流
// =========================================================================
std::string serialize(const Player& p) {
    ByteWriter writer;

    // 严格按照顺序写入，顺序必须与接收方的读取顺序完全一致！
    writer.write_int32(p.id);
    writer.write_int32(p.level);
    writer.write_float(p.x);
    writer.write_float(p.y);
    writer.write_float(p.hp);
    writer.write_float(p.mp);
    writer.write_cstr(p.name, 32);  // 定长写入，不需要长度前缀
    writer.write_cstr(p.guild, 32);

    // 返回拼好的字节流
    return writer.data();
}

// =========================================================================
// 3. 发送一条完整消息：4 字节长度头 + 实际 payload
// =========================================================================
bool send_packet(socket_t fd, const std::string& payload) {
    // 关键：先告诉接收方，这条消息总共有多少字节
    // 这样接收方才知道该收多少，解决粘包问题
    uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));

    // 1. 先发 4 字节长度头
    if (!send_all(fd, &net_len, 4)) return false;

    // 2. 再发真正的数据体
    if (!send_all(fd, payload.data(), payload.size())) return false;

    return true;
}

// =========================================================================
// 4. 主函数
// =========================================================================
int main() {
#ifdef _WIN32
    // 切换控制台代码页为 UTF-8，解决中文乱码
    system("chcp 65001 > nul");
#endif

    // 初始化网络库（Windows 下必须）
    if (!init_network()) return 1;

    // 创建 TCP socket
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);

    // 配置服务器地址
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);        // 端口 9000，转网络序
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // 本地回环地址

    // 连接服务器
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("connect 失败\n");
        return 1;
    }
    printf("[client] 已连接服务器\n\n");

    // ===== 构建要发送的数据 =====
    Player p;
    p.id = 1001;
    p.level = 99;
    p.x = 12.5f;
    p.y = 30.2f;
    p.hp = 100.0f;
    p.mp = 50.0f;

    // 安全地填充字符串：strncpy 最多拷贝 31 字节，第 32 字节强制放 '\0'
    // 防止没有结束符导致内存越界
    strncpy(p.name, "Alice", sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
    strncpy(p.guild, "DragonSlayer", sizeof(p.guild) - 1);
    p.guild[sizeof(p.guild) - 1] = '\0';

    // ===== 序列化并发送 =====
    std::string payload = serialize(p);
    printf("[client] 序列化后长度: %zu 字节\n", payload.size());

    if (send_packet(fd, payload)) {
        printf("[client] 发送成功\n");
    }
    else {
        printf("[client] 发送失败\n");
    }

    // 暂停，防止窗口一闪而过
    printf("\n[client] 按回车键退出客户端...\n");
    std::cin.get();

    // 清理资源
    CLOSE_SOCKET(fd);
    cleanup_network();
    return 0;
}