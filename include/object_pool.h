#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

// Pre-allocated memory pool. Avoids new/delete overhead per order.
// allocate() and deallocate() are O(1) — no system calls in the hot path.
template <typename T, size_t BlockSize = 4096>
class ObjectPool {
public:
    ObjectPool() {
        allocate_block();
    }

    ~ObjectPool() {
        for (auto* block : blocks_) {
            ::operator delete(block, std::align_val_t{alignof(T)});
        }
    }

    // Non-copyable, non-movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    T* allocate() {
        if (free_list_ == nullptr) [[unlikely]] {
            allocate_block();
        }
        Node* node = free_list_;
        free_list_ = node->next;
        ++allocated_;
        return reinterpret_cast<T*>(node);
    }

    void deallocate(T* ptr) {
        Node* node = reinterpret_cast<Node*>(ptr);
        node->next = free_list_;
        free_list_ = node;
        --allocated_;
    }

    size_t allocated_count() const { return allocated_; }
    size_t capacity() const { return blocks_.size() * BlockSize; }

private:
    struct Node {
        Node* next;
    };

    static_assert(sizeof(T) >= sizeof(Node),
        "Object must be at least pointer-sized for free list");

    void allocate_block() {
        // Allocate raw memory aligned for T
        char* block = static_cast<char*>(
            ::operator new(sizeof(T) * BlockSize, std::align_val_t{alignof(T)}));
        blocks_.push_back(block);

        // Chain all slots into the free list
        for (size_t i = 0; i < BlockSize; ++i) {
            Node* node = reinterpret_cast<Node*>(block + i * sizeof(T));
            node->next = free_list_;
            free_list_ = node;
        }
    }

    Node* free_list_ = nullptr;
    size_t allocated_ = 0;
    std::vector<char*> blocks_;
};

} // namespace ob
