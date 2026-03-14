#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

/**
 * @file KArcLfuPart.h
 * @brief ARC 缓存中的 LFU 部分实现
 *
 * 管理 ARC 缓存中的 T2（频繁访问）部分及其幽灵链表 B2。
 * 使用频率哈希表（map<频率, 链表>）组织节点，支持快速找到最小频率节点。
 */

namespace KamaCache
{

/**
 * @class ArcLfuPart
 * @brief ARC 缓存中的 LFU 部分（T2）及其幽灵链表（B2）
 *
 * 负责管理频繁访问的条目（T2）和最近从 T2 淘汰的键（B2）。
 * 使用频率映射（FreqMap）组织节点，每个频率对应一个链表。
 * 维护最小频率（minFreq_）以快速找到淘汰候选。
 * 淘汰的节点进入幽灵链表 B2，用于自适应调整容量比例。
 */
template<typename Key, typename Value>
class ArcLfuPart 
{
public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqMap = std::map<size_t, std::list<NodePtr>>;

    /**
     * @brief 构造一个 ARC LFU 部分对象
     * @param capacity 主链表（T2）容量
     * @param transformThreshold 转换阈值（保留参数，与 LRU 部分保持一致）
     * @note 幽灵链表（B2）容量与主链表相同，初始化时创建虚拟头尾节点。
     *       最小频率（minFreq_）初始化为 0，添加第一个节点后更新为 1。
     */
    explicit ArcLfuPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
        , minFreq_(0)
    {
        initializeLists();
    }

    /**
     * @brief 向 LFU 部分添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @return true 操作成功，false 容量为 0 无法添加
     * @note 如果键已存在，则更新其值并增加频率；
     *       如果键不存在且缓存已满，则淘汰频率最低的节点到幽灵链表。
     */
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) 
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value);
    }

    /**
     * @brief 从 LFU 部分获取指定键的值
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在，value 被设置为对应的值
     * @return false 如果键不存在，value 保持不变
     * @note 如果键存在，会增加节点的访问频率并调整在频率链表中的位置。
     */
    bool get(Key key, Value& value) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    bool contain(Key key)
    {
        return mainCache_.find(key) != mainCache_.end();
    }

    bool checkGhost(Key key) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) 
        {
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
        if (mainCache_.size() == capacity_) 
        {
            evictLeastFrequent();
        }
        --capacity_;
        return true;
    }

private:
    void initializeLists() 
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {
            evictLeastFrequent();
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;
        
        // 将新节点添加到频率为1的列表中
        if (freqMap_.find(1) == freqMap_.end()) 
        {
            freqMap_[1] = std::list<NodePtr>();
        }
        freqMap_[1].push_back(newNode);
        minFreq_ = 1;
        
        return true;
    }

    void updateNodeFrequency(NodePtr node) 
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();
        size_t newFreq = node->getAccessCount();

        // 从旧频率列表中移除
        auto& oldList = freqMap_[oldFreq];
        oldList.remove(node);
        if (oldList.empty()) 
        {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_) 
            {
                minFreq_ = newFreq;
            }
        }

        // 添加到新频率列表
        if (freqMap_.find(newFreq) == freqMap_.end()) 
        {
            freqMap_[newFreq] = std::list<NodePtr>();
        }
        freqMap_[newFreq].push_back(node);
    }

    void evictLeastFrequent() 
    {
        if (freqMap_.empty()) 
            return;

        // 获取最小频率的列表
        auto& minFreqList = freqMap_[minFreq_];
        if (minFreqList.empty()) 
            return;

        // 移除最少使用的节点
        NodePtr leastNode = minFreqList.front();
        minFreqList.pop_front();

        // 如果该频率的列表为空，则删除该频率项
        if (minFreqList.empty()) 
        {
            freqMap_.erase(minFreq_);
            // 更新最小频率
            if (!freqMap_.empty()) 
            {
                minFreq_ = freqMap_.begin()->first;
            }
        }

        // 将节点移到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            removeOldestGhost();
        }
        addToGhost(leastNode);
        
        // 从主缓存中移除
        mainCache_.erase(leastNode->getKey());
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
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        if (!ghostTail_->prev_.expired()) {
            ghostTail_->prev_.lock()->next_ = node;
        }
        ghostTail_->prev_ = node;
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost() 
    {
        NodePtr oldestGhost = ghostHead_->next_;
        if (oldestGhost != ghostTail_) 
        {
            removeFromGhost(oldestGhost);
            ghostCache_.erase(oldestGhost->getKey());
        }
    }

private:
    size_t capacity_;
    size_t ghostCapacity_;
    size_t transformThreshold_;
    size_t minFreq_;
    std::mutex mutex_;

    NodeMap mainCache_;
    NodeMap ghostCache_;
    FreqMap freqMap_;
    
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace KamaCache