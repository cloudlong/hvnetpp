#pragma once

#include <atomic>

namespace hvnetpp {

// 侵入式无界 MPSC 队列（Dmitry Vyukov 节点式算法）
// - push：任意线程并发调用；一次原子 exchange，永不失败、不分配内存
// - pop：只允许单一消费者线程调用
// - 同一生产者线程 push 的节点，在消费端严格按 push 顺序出队（FIFO）
// 节点需继承 MpscNode；内存由使用方管理，push 之后、被 pop 出来之前不得释放或再次 push。
struct MpscNode {
    std::atomic<MpscNode*> mpscNext{nullptr};
};

class MpscQueue {
public:
    MpscQueue() : head_(&stub_), tail_(&stub_) {}
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    void push(MpscNode* node) {
        node->mpscNext.store(nullptr, std::memory_order_relaxed);
        // acq_rel：与其他生产者的 exchange 串行化，并把节点内容发布给后续链接它的生产者
        MpscNode* prev = head_.exchange(node, std::memory_order_acq_rel);
        // release：消费者 acquire 读到该指针时，节点内容（任务数据）可见
        prev->mpscNext.store(node, std::memory_order_release);
    }

    // 取出最早的节点。队列为空，或某个生产者正处于 exchange 之后、链接之前的中间态时返回 nullptr；
    // 后一种情况下该生产者完成 push 后会自行唤醒消费者（见 EventLoop::enqueue），不会丢任务。
    // 返回的节点已与队列完全脱离，调用方可以复用它的 mpscNext 字段。
    MpscNode* pop() {
        MpscNode* tail = tail_;
        MpscNode* next = tail->mpscNext.load(std::memory_order_acquire);
        if (tail == &stub_) {
            if (next == nullptr) {
                return nullptr;
            }
            tail_ = next;
            tail = next;
            next = next->mpscNext.load(std::memory_order_acquire);
        }
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        MpscNode* head = head_.load(std::memory_order_acquire);
        if (tail != head) {
            return nullptr;  // 有生产者正在 push
        }
        // 只剩最后一个真实节点：补入 stub，使其成为可摘除的非尾节点
        push(&stub_);
        next = tail->mpscNext.load(std::memory_order_acquire);
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

private:
    // head_ 被所有生产者争用，tail_ 只有消费者访问，用填充隔开避免伪共享
    // （C++14 的 new 不保证 alignas(64) 生效，这里用显式填充）
    std::atomic<MpscNode*> head_;
    char pad_[64 - sizeof(std::atomic<MpscNode*>)];
    MpscNode* tail_;
    MpscNode stub_;
};

} // namespace hvnetpp
