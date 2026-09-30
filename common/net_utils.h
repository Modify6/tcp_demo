#pragma once

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
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

// Windows 下必须初始化 Winsock
inline bool init_network() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 失败\n");
        return false;
    }
#endif
    return true;
}

inline void cleanup_network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// 循环收，直到收满 len 字节
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
        printf("[recv] 返回 %d，本次收到 %d 字节，累计 %zu/%zu\n",
               (int)n, (int)n, got + n, len);
        got += n;
    }
    return true;
}

// 循环发，直到发满 len 字节
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
        printf("[send] 返回 %d，本次发送 %d 字节，累计 %zu/%zu\n",
               (int)n, (int)n, sent + n, len);
        sent += n;
    }
    return true;
}

// 发一条完整消息：4 字节长度头 + 内容
inline bool send_message(socket_t fd, const std::string& body) {
    uint32_t net_len = htonl((uint32_t)body.size());
    printf("[send] 准备发送一条消息，长度 %zu\n", body.size());
    if (!send_all(fd, &net_len, 4)) return false;
    if (!send_all(fd, body.data(), body.size())) return false;
    printf("[send] 一条消息发送完毕\n\n");
    return true;
}

// 收一条完整消息：先收 4 字节长度，再收内容
inline bool recv_message(socket_t fd, std::string& body) {
    uint32_t net_len;
    if (!recv_all(fd, &net_len, 4)) return false;
    uint32_t len = ntohl(net_len);
    printf("[recv] 收到长度头：%u 字节\n", len);
    body.resize(len);
    if (!recv_all(fd, body.data(), len)) return false;
    return true;
}