#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace lob {

// A tiny, single-size-class free-list pool for std::list nodes.
//
// std::list allocates exactly one node at a time, and every node has the same
// size, so a single free list is all we need. Freed nodes are recycled instead
// of returned to the global allocator, which removes malloc/free traffic from
// the hot add/cancel path. Chunks are freed in bulk on destruction.
//
// NOT thread-safe (matches the OrderBook's single-writer contract).
class NodePool {
public:
    NodePool() = default;
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;
    NodePool(NodePool&&) = delete;
    NodePool& operator=(NodePool&&) = delete;

    ~NodePool() {
        for (void* chunk : chunks_) {
            ::operator delete(chunk);
        }
    }

    void* allocate(std::size_t bytes) {
        const std::size_t need = bytes < sizeof(FreeNode) ? sizeof(FreeNode) : bytes;
        if (node_size_ == 0) {
            node_size_ = need;
        }
        // Fall back to the global allocator for any off-size request (never
        // happens for std::list nodes, but keeps the allocator fully general).
        if (need != node_size_) {
            return ::operator new(bytes);
        }
        if (free_ == nullptr) {
            grow();
        }
        FreeNode* node = free_;
        free_ = free_->next;
        return static_cast<void*>(node);
    }

    void deallocate(void* p, std::size_t bytes) noexcept {
        const std::size_t used = bytes < sizeof(FreeNode) ? sizeof(FreeNode) : bytes;
        if (used != node_size_) {
            ::operator delete(p);
            return;
        }
        auto* node = static_cast<FreeNode*>(p);
        node->next = free_;
        free_ = node;
    }

private:
    struct FreeNode {
        FreeNode* next;
    };

    void grow() {
        constexpr std::size_t kNodesPerChunk = 4096;
        const std::size_t bytes = node_size_ * kNodesPerChunk;
        auto* base = static_cast<std::uint8_t*>(::operator new(bytes));
        chunks_.push_back(base);
        for (std::size_t i = 0; i < kNodesPerChunk; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(base + i * node_size_);
            node->next = free_;
            free_ = node;
        }
    }

    std::size_t node_size_{0};
    FreeNode* free_{nullptr};
    std::vector<void*> chunks_{};
};

// Stateful allocator that routes single-object allocations to a NodePool.
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(NodePool* pool) noexcept : pool_(pool) {}

    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 1 && pool_ != nullptr) {
            return static_cast<T*>(pool_->allocate(sizeof(T)));
        }
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n == 1 && pool_ != nullptr) {
            pool_->deallocate(p, sizeof(T));
            return;
        }
        ::operator delete(p);
    }

    template <typename U>
    bool operator==(const PoolAllocator<U>& o) const noexcept {
        return pool_ == o.pool_;
    }
    template <typename U>
    bool operator!=(const PoolAllocator<U>& o) const noexcept {
        return pool_ != o.pool_;
    }

    NodePool* pool_;
};

} // namespace lob
