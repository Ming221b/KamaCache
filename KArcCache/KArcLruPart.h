#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <mutex>

/**
 * @file KArcLruPart.h
 * @brief ARC 缓存中的 LRU 部分实现
 *
 * 管理 ARC 缓存中的 T1（最近访问一次）部分及其幽灵链表 B1。
 * 提供 LRU 缓存的基本操作，并支持将频繁访问的节点转移到 LFU 部分。
 */

namespace KamaCache
{

/**
 * @class ArcLruPart
 * @brief ARC 缓存中的 LRU 部分（T1）及其幽灵链表（B1）
 *
 * 负责管理最近访问一次的条目（T1）和最近从 T1 淘汰的键（B1）。
 * 当节点访问次数达到转换阈值时，标记为可转移到 LFU 部分。
 * 淘汰的节点不会立即删除，而是进入幽灵链表 B1，用于自适应调整容量比例。
 */
template<typename Key, typename Value>
class ArcLruPart 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    /**
     * @brief 构造一个 ARC LRU 部分对象
     * @param capacity 主链表（T1）容量
     * @param transformThreshold 转换阈值，节点访问次数达到此值后可转移到 LFU 部分
     * @note 幽灵链表（B1）容量与主链表相同，初始化时创建虚拟头尾节点
     */
    explicit ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
    {
        initializeLists();
    }

    /**
     * @brief 向 LRU 部分添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @return true 操作成功，false 容量为 0 无法添加
     * @note 如果键已存在，则更新其值并移到链表头部；
     *       如果键不存在且缓存已满，则淘汰最近最少使用的节点到幽灵链表。
     */
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) return false;
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    /**
     * @brief 从 LRU 部分获取指定键的值
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @param shouldTransform 传出参数，指示该节点是否应转移到 LFU 部分
     * @return true 如果键存在，value 被设置为对应的值
     * @return false 如果键不存在，value 和 shouldTransform 保持不变
     * @note 如果键存在，会将其移到链表头部并增加访问计数。
     *       shouldTransform 为 true 表示节点访问次数达到转换阈值。
     */
    bool get(Key key, Value& value, bool& shouldTransform) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            shouldTransform = updateNodeAccess(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    void increaseCapacity() { ++capacity_; }
    
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) {
            evictLeastRecent();
        }
        --capacity_;
        return true;
    }

private:
    void initializeLists() 
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToFront(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {   
            evictLeastRecent(); // 驱逐最近最少访问
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        addToFront(newNode);
        return true;
    }

    bool updateNodeAccess(NodePtr node) 
    {
        moveToFront(node);
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    void moveToFront(NodePtr node) 
    {
        // 先从当前位置移除
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
        
        // 添加到头部
        addToFront(node);
    }

    void addToFront(NodePtr node) 
    {
        node->next_ = mainHead_->next_;
        node->prev_ = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_ = node;
    }

    void evictLeastRecent() 
    {
        NodePtr leastRecent = mainTail_->prev_.lock();
        if (!leastRecent || leastRecent == mainHead_) 
            return;

        // 从主链表中移除
        removeFromMain(leastRecent);

        // 添加到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            removeOldestGhost();
        }
        addToGhost(leastRecent);

        // 从主缓存映射中移除
        mainCache_.erase(leastRecent->getKey());
    }

    void removeFromMain(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void removeFromGhost(NodePtr node) 
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void addToGhost(NodePtr node) 
    {
        // 重置节点的访问计数
        node->accessCount_ = 1;
        
        // 添加到幽灵缓存的头部
        node->next_ = ghostHead_->next_;
        node->prev_ = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_ = node;
        
        // 添加到幽灵缓存映射
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost() 
    {
        // 使用lock()方法，并添加null检查
        NodePtr oldestGhost = ghostTail_->prev_.lock();
        if (!oldestGhost || oldestGhost == ghostHead_) 
            return;

        removeFromGhost(oldestGhost);
        ghostCache_.erase(oldestGhost->getKey());
    }
    

private:
    size_t capacity_;
    size_t ghostCapacity_;
    size_t transformThreshold_; // 转换门槛值
    std::mutex mutex_;

    NodeMap mainCache_; // key -> ArcNode
    NodeMap ghostCache_;
    
    // 主链表
    NodePtr mainHead_;
    NodePtr mainTail_;
    // 淘汰链表
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace KamaCache