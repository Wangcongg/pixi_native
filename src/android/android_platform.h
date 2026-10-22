// Android 宿主的调度原语:替代 mac/iOS 宿主的 GCD。
// - MainQueue:跨线程投递到 JS/GL 线程(Choreographer 驱动的 UI 线程),宿主在每帧 drawFrame 开头 drain。
//   对应 dispatch_async(dispatch_get_main_queue()),延迟 ≤1 帧。
// - WorkPool:后台并发队列,对应 gLoadQ()/ResourceManager 的 DISPATCH_QUEUE_CONCURRENT。
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aplat {

class MainQueue {
    std::mutex mu_;
    std::vector<std::function<void()>> jobs_;
public:
    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.push_back(std::move(fn));
    }
    // 只能在 JS/GL 线程调用。drain 时新投递的任务留到下一帧(快照交换,防自增殖任务饿死渲染)。
    void drain() {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mu_);
            batch.swap(jobs_);
        }
        for (auto& fn : batch) if (fn) fn();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.clear();
    }
};

class WorkPool {
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&]{ return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            if (job) job();
        }
    }
public:
    void start(int threads = 4) {
        if (!threads_.empty()) return;
        stop_ = false;
        for (int i = 0; i < threads; ++i) threads_.emplace_back([this]{ loop(); });
    }
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            jobs_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.clear();
    }
    ~WorkPool() { shutdown(); }
};

class CpuGate {
    std::mutex mu_;
    std::condition_variable cv_;
    int slots_;
public:
    explicit CpuGate(int slots = 2) : slots_(slots) {}
    void acquire() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&]{ return slots_ > 0; });
        --slots_;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++slots_;
        }
        cv_.notify_one();
    }
    struct Scoped {
        CpuGate& gate;
        explicit Scoped(CpuGate& g) : gate(g) { gate.acquire(); }
        ~Scoped() { gate.release(); }
    };
};

}   // namespace aplat
