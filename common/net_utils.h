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

// 网络初始化 (Windows 下必须调用 WSAStartup)
inline bool init_network() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
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

// 循环接收，直到收满 len 字节。解决 TCP 拆包问题。
inline bool recv_all(socket_t fd, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t got = 0;
    while (got < len) {
#ifdef _WIN32
        int n = recv(fd, p + got, (int)(len - got), 0);
#else
        ssize_t n = recv(fd, p + got, len - got, 0);
#endif
        if (n <= 0) return false; // 连接断开或出错
        got += n;
    }
    return true;
}

// 循环发送，直到发满 len 字节。解决 TCP 粘包/缓冲区限制问题。
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