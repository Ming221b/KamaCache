#pragma once

/**
 * @file KLruCache.h
 * @brief LRU（最近最少使用）缓存及其变体实现
 *
 * 该文件包含多种 LRU 缓存实现：
 * 1. KLruCache: 基础 LRU 缓存，使用双向链表和哈希表
 * 2. KLruKCache: LRU-K 缓存，考虑访问历史频率
 * 3. KHashLruCaches: 分片 LRU 缓存，提高并发性能
 *
 * 所有实现均线程安全（使用互斥锁），并支持任意类型的键和值。
 */

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "KICachePolicy.h"

namespace KamaCache
{

// 前向声明
template<typename Key, typename Value> class KLruCache;

/**
 * @class LruNode
 * @brief LRU 缓存中的节点类
 *
 * 表示 LRU 缓存中的一个条目，存储键、值、访问次数以及前后节点的指针。
 * 使用 weak_ptr 指向前驱节点以避免循环引用，使用 shared_ptr 指向后继节点。
 */
template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // 访问次数
    std::weak_ptr<LruNode<Key, Value>> prev_;  // 改为weak_ptr打破循环引用
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1) 
    {}

    // 提供必要的访问器
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class KLruCache<Key, Value>;
};


/**
 * @class KLruCache
 * @brief 基础 LRU（最近最少使用）缓存实现
 *
 * 实现经典的 LRU 缓存策略：当缓存满时，淘汰最近最少使用的项目。
 * 使用双向链表维护访问顺序，哈希表提供 O(1) 的查找效率。
 * 线程安全：所有公共操作均使用互斥锁保护。
 */
template<typename Key, typename Value>
class KLruCache : public KICachePolicy<Key, Value>
{
public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    /**
     * @brief 构造一个 LRU 缓存对象
     * @param capacity 缓存容量，必须大于 0
     * @note 容量为 0 或负数的缓存将拒绝所有添加操作
     */
    KLruCache(int capacity)
        : capacity_(capacity)
    {
        initializeList();
    }

    ~KLruCache() override = default;

    /**
     * @brief 向缓存中添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @note 如果键已存在，则更新其值并将节点移到最近使用位置；
     *       如果键不存在且缓存已满，则淘汰最近最少使用的节点后添加新节点。
     *       线程安全。
     */
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;
    
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果在当前容器中,则更新value,并调用get方法，代表该数据刚被访问
            updateExistingNode(it->second, value);
            return ;
        }

        addNewNode(key, value);
    }

    /**
     * @brief 从缓存中获取指定键的值（传出参数版本）
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在，value 被设置为对应的值，且节点被移到最近使用位置
     * @return false 如果键不存在，value 保持不变
     * @note 线程安全。如果键存在，会更新节点的访问时间（移到链表头部）
     */
    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    /**
     * @brief 从缓存中获取指定键的值（返回值版本）
     * @param key 要查找的键
     * @return Value 如果键存在则返回对应的值，否则返回默认构造的 Value 对象
     * @note 内部调用 bool get(Key, Value&) 版本，如果键不存在会返回默认值
     * @warning 对于非平凡类型，返回默认构造值可能有性能开销
     */
    Value get(Key key) override
    {
        Value value{};
        // memset(&value, 0, sizeof(value));   // memset 是按字节设置内存的，对于复杂类型（如 string）使用 memset 可能会破坏对象的内部结构
        get(key, value);
        return value;
    }

    /**
     * @brief 从缓存中删除指定键的条目
     * @param key 要删除的键
     * @note 如果键存在，则从链表和哈希表中移除对应节点；如果键不存在，则不执行任何操作。
     *       线程安全。
     */
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

private:
    void initializeList()
    {
        // 创建首尾虚拟节点
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value) 
    {
       if (nodeMap_.size() >= capacity_) 
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);
       nodeMap_[key] = newNode;
    }

    // 将该节点移动到最新的位置
    void moveToMostRecent(NodePtr node) 
    {
        removeNode(node);
        insertNode(node);
    }

    void removeNode(NodePtr node) 
    {
        if(!node->prev_.expired() && node->next_) 
        {
            auto prev = node->prev_.lock(); // 使用lock()获取shared_ptr
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // 清空next_指针，彻底断开节点与链表的连接
        }
    }

    // 从尾部插入结点
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_.lock()->next_ = node; // 使用lock()获取shared_ptr
        dummyTail_->prev_ = node;
    }

    // 驱逐最近最少访问
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
    }

private:
    int           capacity_; // 缓存容量
    NodeMap       nodeMap_; // key -> Node 
    std::mutex    mutex_;
    NodePtr       dummyHead_; // 虚拟头结点
    NodePtr       dummyTail_;
};

/**
 * @class KLruKCache
 * @brief LRU-K 缓存实现
 *
 * LRU-K 算法是 LRU 的改进，不仅考虑最近访问时间，还考虑访问频率。
 * 维护一个访问历史记录，只有访问次数达到 K 次的项才会进入主缓存。
 * 适用于访问模式具有“频率特征”的场景，能更好地抵抗突发流量干扰。
 *
 * 工作原理：
 * 1. 每个键的访问次数被记录在单独的 LRU 缓存中（历史记录）
 * 2. 当访问次数达到 K 时，该键及其值被提升到主 LRU 缓存
 * 3. 主缓存满时仍按 LRU 策略淘汰
 */
template<typename Key, typename Value>
class KLruKCache : public KLruCache<Key, Value>
{
public:
    /**
     * @brief 构造一个 LRU-K 缓存对象
     * @param capacity 主缓存容量
     * @param historyCapacity 历史记录缓存容量（存储访问次数）
     * @param k 阈值 K，访问次数达到此值后条目才进入主缓存
     */
    KLruKCache(int capacity, int historyCapacity, int k)
        : KLruCache<Key, Value>(capacity) // 调用基类构造
        , historyList_(std::make_unique<KLruCache<Key, size_t>>(historyCapacity))
        , k_(k)
    {}

    Value get(Key key) 
    {
        // 首先尝试从主缓存获取数据
        Value value{};
        bool inMainCache = KLruCache<Key, Value>::get(key, value);

        // 获取并更新访问历史计数
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);

        // 如果数据在主缓存中，直接返回
        if (inMainCache) 
        {
            return value;
        }

        // 如果数据不在主缓存，但访问次数达到了k次
        if (historyCount >= k_) 
        {
            // 检查是否有历史值记录
            auto it = historyValueMap_.find(key);
            if (it != historyValueMap_.end()) 
            {
                // 有历史值，将其添加到主缓存
                Value storedValue = it->second;
                
                // 从历史记录移除
                historyList_->remove(key);
                historyValueMap_.erase(it);
                
                // 添加到主缓存
                KLruCache<Key, Value>::put(key, storedValue);
                
                return storedValue;
            }
            // 没有历史值记录，无法添加到缓存，返回默认值
        }

        // 数据不在主缓存且不满足添加条件，返回默认值
        return value;
    }

    void put(Key key, Value value) 
    {
        // 检查是否已在主缓存
        Value existingValue{};
        bool inMainCache = KLruCache<Key, Value>::get(key, existingValue);
        
        if (inMainCache) 
        {
            // 已在主缓存，直接更新
            KLruCache<Key, Value>::put(key, value);
            return;
        }
        
        // 获取并更新访问历史
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);
        
        // 保存值到历史记录映射，供后续get操作使用
        historyValueMap_[key] = value;
        
        // 检查是否达到k次访问阈值
        if (historyCount >= k_) 
        {
            // 达到阈值，添加到主缓存
            historyList_->remove(key);
            historyValueMap_.erase(key);
            KLruCache<Key, Value>::put(key, value);
        }
    }

private:
    int                                     k_; // 进入缓存队列的评判标准
    std::unique_ptr<KLruCache<Key, size_t>> historyList_; // 访问数据历史记录(value为访问次数)
    std::unordered_map<Key, Value>          historyValueMap_; // 存储未达到k次访问的数据值
};

/**
 * @class KHashLruCaches
 * @brief 分片哈希 LRU 缓存
 *
 * 将总缓存容量分成多个独立的 LRU 缓存分片，每个分片有自己的锁。
 * 通过键的哈希值决定路由到哪个分片，从而减少锁竞争，提高并发性能。
 * 适用于多线程高并发场景，但可能因哈希不均匀导致分片负载不平衡。
 */
template<typename Key, typename Value>
class KHashLruCaches
{
public:
    KHashLruCaches(size_t capacity, int sliceNum)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 获取每个分片的大小
        for (int i = 0; i < sliceNum_; ++i)
        {
            lruSliceCaches_.emplace_back(new KLruCache<Key, Value>(sliceSize)); 
        }
    }

    void put(Key key, Value value)
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        memset(&value, 0, sizeof(value));
        get(key, value);
        return value;
    }

private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t                                              capacity_;  // 总容量
    int                                                 sliceNum_;  // 切片数量
    std::vector<std::unique_ptr<KLruCache<Key, Value>>> lruSliceCaches_; // 切片LRU缓存
};

} // namespace KamaCache