#pragma once

#include <memory>

/**
 * @file KArcCacheNode.h
 * @brief ARC 缓存节点定义
 *
 * 定义 ARC 缓存中使用的节点结构，包含键、值、访问计数和前驱/后继指针。
 * 使用 weak_ptr 指向前驱节点以避免循环引用。
 */

namespace KamaCache
{

/**
 * @class ArcNode
 * @brief ARC 缓存节点类
 *
 * 表示 ARC 缓存中的一个条目，存储键、值、访问次数以及前后节点的指针。
 * 访问次数用于决定节点是否应从 LRU 部分转移到 LFU 部分。
 * 使用 weak_ptr 指向前驱节点以避免循环引用，使用 shared_ptr 指向后继节点。
 */
template<typename Key, typename Value>
class ArcNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;
    std::weak_ptr<ArcNode> prev_;
    std::shared_ptr<ArcNode> next_;

public:
    ArcNode() : accessCount_(1), next_(nullptr) {}
    
    ArcNode(Key key, Value value) 
        : key_(key)
        , value_(value)
        , accessCount_(1)
        , next_(nullptr) 
    {}

    // Getters
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    size_t getAccessCount() const { return accessCount_; }
    
    // Setters
    void setValue(const Value& value) { value_ = value; }
    void incrementAccessCount() { ++accessCount_; }

    template<typename K, typename V> friend class ArcLruPart;
    template<typename K, typename V> friend class ArcLfuPart;
};

} // namespace KamaCache