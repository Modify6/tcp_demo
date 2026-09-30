#include "../common/net_utils.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

// =========================================================================
// 1. 数据结构（与发送方一致）
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
// 2. 反序列化（只在接收线程里调用）
// =========================================================================
Player deserialize(const std::string& buf) {
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

// =========================================================================
// 3. 线程安全队列（生产者-消费者模型）
// =========================================================================
template<typename T>
class SafeQueue {
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stopped_ = false;

public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cond_.notify_one(); // 有数据了，唤醒消费者
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待：队列有数据，或者被要求停止
        cond_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false; // 被停止且没数据了
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cond_.notify_all(); // 唤醒所有等待者，让它们退出
    }
};

// =========================================================================
// 4. 接收线程函数
// =========================================================================
void recv_thread_func(socket_t fd, SafeQueue<Player>& queue, std::atomic<bool>& running) {
    printf("[recv_thread] 接收线程启动\n");

    while (running) {
        // 4.1 先收 4 字节长度头
        uint32_t net_len;
        if (!recv_all(fd, &net_len, 4)) {
            printf("[recv_thread] 对端关闭或出错，退出接收循环\n");
            break;
        }
        uint32_t len = ntohl(net_len);
        // 4.2 根据长度收 payload
        std::string payload(len, '\0');
        if (!recv_all(fd, payload.data(), len)) {
            printf("[recv_thread] payload 接收失败\n");
            break;
        }

        // 4.3 反序列化
        Player p = deserialize(payload);

        // 4.4 塞进队列，唤醒主线程处理
        queue.push(p);
        printf("[recv_thread] 收到一条消息，已放入队列\n");
    }

    // 4.5 通知主线程：接收结束了
    running = false;
    queue.stop();
    printf("[recv_thread] 接收线程退出\n");
}

// =========================================================================
// 5. 主函数
// =========================================================================
int main() {
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif
    if (!init_network()) return 1;

    // -------- 5.1 建立监听、接受连接 --------
    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind 失败\n");
        return 1;
    }
    listen(listen_fd, 1);
    printf("[server] 监听 9000，等待连接...\n");

    socket_t fd = accept(listen_fd, nullptr, nullptr);
    printf("[server] 客户端已连接\n\n");

    // -------- 5.2 启动接收线程 --------
    SafeQueue<Player> queue;             // 线程安全队列
    std::atomic<bool> running{ true };     // 标志位：接收线程是否继续

    // 创建线程：传入 socket 描述符、队列引用、running 标志引用
    std::thread recv_thread(recv_thread_func, fd, std::ref(queue), std::ref(running));

    // -------- 5.3 主线程：消费队列 --------
    printf("[main] 主线程开始消费队列...\n");
    Player p;
    while (queue.pop(p)) {
        // 这里就是主线程的业务逻辑
        printf("[main] 处理一条消息:\n");
        printf("  -> ID:    %d\n", p.id);
        printf("  -> Level: %d\n", p.level);
        printf("  -> X:     %.2f\n", p.x);
        printf("  -> Y:     %.2f\n", p.y);
        printf("  -> HP:    %.2f\n", p.hp);
        printf("  -> MP:    %.2f\n", p.mp);
        printf("  -> Name:  %s\n", p.name);
        printf("  -> Guild: %s\n", p.guild);
        printf("----------------------------------------\n");
    }

    printf("[main] 队列已停止，主线程退出\n");

    // -------- 5.4 清理 --------
    running = false;
    if (recv_thread.joinable()) {
        recv_thread.join(); // 等待接收线程退出
    }

    CLOSE_SOCKET(fd);
    CLOSE_SOCKET(listen_fd);
    cleanup_network();
    return 0;
}