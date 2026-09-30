#pragma once

// =========================================================================
// 1. 跨平台网络头文件与宏定义
// =========================================================================
#ifdef _WIN32
  // Windows 下必须包含 Winsock2，且必须在 windows.h 之前包含
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")  // 告诉链接器链接 ws2_32 库
using socket_t = SOCKET;            // Windows 的 socket 类型是 SOCKET
#define CLOSE_SOCKET closesocket    // Windows 下关闭 socket 用 closesocket
#define INVALID_SOCK INVALID_SOCKET // Windows 下的无效 socket 标记
#else
  // Linux / macOS 下的头文件
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;               // Linux 的 socket 类型就是 int
#define CLOSE_SOCKET close          // Linux 下关闭 socket 用 close
#define INVALID_SOCK INVALID_SOCKET
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// =========================================================================
// 2. 网络初始化与清理（Windows 特有）
// =========================================================================
inline bool init_network() {
#ifdef _WIN32
    // Windows 下使用 socket 前，必须调用 WSAStartup 初始化 Winsock 库
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
#endif
    return true;
}

inline void cleanup_network() {
#ifdef _WIN32
    // 程序结束前，必须调用 WSACleanup 释放 Winsock 库资源
    WSACleanup();
#endif
}

// =========================================================================
// 3. 循环收发（解决 TCP 粘包与拆包的核心）
// =========================================================================
// TCP 是字节流协议。一次 send 不一定能发完所有数据，一次 recv 也不一定能收全。
// 所以必须用循环，直到发满/收满指定字节数。
inline bool recv_all(socket_t fd, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t got = 0;
    while (got < len) {
        // 尝试收 remaining 个字节
        int n = recv(fd, p + got, (int)(len - got), 0);
        if (n <= 0) return false; // 返回 0 表示对端关闭，返回负数表示出错
        got += n;                 // 累加已收字节数，继续循环
    }
    return true;
}

inline bool send_all(socket_t fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t sent = 0;
    while (sent < len) {
        // 尝试发 remaining 个字节
        int n = send(fd, p + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += n; // 累加已发字节数，继续循环
    }
    return true;
}

// =========================================================================
// 4. 字节流写入器（发送方专用）
// =========================================================================
// 目标：把结构体里的各个字段，按顺序“拍扁”成无 padding 的字节流。
class ByteWriter {
    std::string buffer; // 用来存放拼好的字节流
public:
    // 写入 4 字节整数
    void write_int32(int32_t val) {
        // 关键：把主机字节序转成网络字节序（大端），保证跨平台
        uint32_t net_val = htonl(static_cast<uint32_t>(val));
        // 把这 4 个字节追加到 buffer 末尾
        buffer.append(reinterpret_cast<const char*>(&net_val), 4);
    }

    // 写入 4 字节浮点数
    void write_float(float val) {
        // 浮点数不能直接 htonl，必须先把它内存里的 4 个字节原样拷到 uint32_t 里
        uint32_t temp;
        memcpy(&temp, &val, 4);
        // 再按整数的方式转网络序
        uint32_t net_val = htonl(temp);
        buffer.append(reinterpret_cast<const char*>(&net_val), 4);
    }

    // 写入定长字符数组（例如 char name[32]）
    void write_cstr(const char* str, size_t fixed_len) {
        // char 是 1 字节，没有字节序问题，直接原样追加
        // 注意：这里固定发 fixed_len 个字节，多出的空字符也会一起发过去
        buffer.append(str, fixed_len);
    }

    // 获取最终拼好的完整字节流
    const std::string& data() const { return buffer; }
};

// =========================================================================
// 5. 字节流读取器（接收方专用）
// =========================================================================
// 目标：从字节流里，按顺序“抠”出各个字段，还原成原始类型。
class ByteReader {
    const std::string& buffer; // 待解析的字节流
    size_t offset = 0;         // 偏移量：记录当前读到哪了
public:
    ByteReader(const std::string& buf) : buffer(buf) {}

    // 读取 4 字节整数
    int32_t read_int32() {
        uint32_t net_val;
        // 从 offset 处抠出 4 字节
        memcpy(&net_val, buffer.data() + offset, 4);
        offset += 4; // 游标后移 4 字节
        // 网络序转主机序，返回给调用者
        return static_cast<int32_t>(ntohl(net_val));
    }

    // 读取 4 字节浮点数
    float read_float() {
        uint32_t net_val;
        memcpy(&net_val, buffer.data() + offset, 4);
        offset += 4;
        // 网络序转主机序
        uint32_t host_val = ntohl(net_val);
        // 把主机序的整数内存原样拷回给浮点数
        float val;
        memcpy(&val, &host_val, 4);
        return val;
    }

    // 读取定长字符数组
    void read_cstr(char* out, size_t fixed_len) {
        // 从 offset 处原样拷贝 fixed_len 个字节到 out
        memcpy(out, buffer.data() + offset, fixed_len);
        // 安全保护：强制加上字符串结束符 '\0'，防止对面没发 '\0' 导致越界读取
        out[fixed_len - 1] = '\0';
        offset += fixed_len;
    }
};