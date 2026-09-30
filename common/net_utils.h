#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET close
#define INVALID_SOCK INVALID_SOCKET
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ================= 网络初始化 =================
inline bool init_network() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    return true;
}

inline void cleanup_network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ================= 阻塞式循环收发（客户端使用） =================
inline bool send_all(socket_t fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(fd, p + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(fd, p + sent, len - sent, 0);
#endif
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

inline bool recv_all(socket_t fd, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t got = 0;
    while (got < len) {
#ifdef _WIN32
        int n = recv(fd, p + got, (int)(len - got), 0);
#else
        ssize_t n = recv(fd, p + got, len - got, 0);
#endif
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

// ================= 字节流写入器 =================
class ByteWriter {
    std::string buffer;
public:
    void write_int32(int32_t val) {
        uint32_t net_val = htonl(static_cast<uint32_t>(val));
        buffer.append(reinterpret_cast<const char*>(&net_val), 4);
    }
    void write_float(float val) {
        uint32_t temp;
        memcpy(&temp, &val, 4);
        uint32_t net_val = htonl(temp);
        buffer.append(reinterpret_cast<const char*>(&net_val), 4);
    }
    void write_cstr(const char* str, size_t fixed_len) {
        buffer.append(str, fixed_len);
    }
    const std::string& data() const { return buffer; }
};

// ================= 字节流读取器 =================
class ByteReader {
    const std::string& buffer;
    size_t offset = 0;
public:
    ByteReader(const std::string& buf) : buffer(buf) {}
    int32_t read_int32() {
        uint32_t net_val;
        memcpy(&net_val, buffer.data() + offset, 4);
        offset += 4;
        return static_cast<int32_t>(ntohl(net_val));
    }
    float read_float() {
        uint32_t net_val;
        memcpy(&net_val, buffer.data() + offset, 4);
        offset += 4;
        uint32_t host_val = ntohl(net_val);
        float val;
        memcpy(&val, &host_val, 4);
        return val;
    }
    void read_cstr(char* out, size_t fixed_len) {
        memcpy(out, buffer.data() + offset, fixed_len);
        out[fixed_len - 1] = '\0';
        offset += fixed_len;
    }
};

// ================= 共享的 Player 结构体 =================
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

// ================= Player 的序列化与反序列化 =================
inline std::string serialize(const Player& p) {
    ByteWriter writer;
    writer.write_int32(p.id);
    writer.write_int32(p.level);
    writer.write_float(p.x);
    writer.write_float(p.y);
    writer.write_float(p.hp);
    writer.write_float(p.mp);
    writer.write_cstr(p.name, 32);
    writer.write_cstr(p.guild, 32);
    return writer.data();
}

inline Player deserialize(const std::string& buf) {
    ByteReader reader(buf);
    Player p;
    p.id = reader.read_int32();
    p.level = reader.read_int32();
    p.x = reader.read_float();
    p.y = reader.read_float();
    p.hp = reader.read_float();
    p.mp = reader.read_float();
    reader.read_cstr(p.name, 32);
    reader.read_cstr(p.guild, 32);
    return p;
}