#pragma once

#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "KICachePolicy.h"

/**
 * @file KLfuCache.h
 * @brief LFU（最不经常使用）缓存及其变体实现
 *
 * 该文件包含多种 LFU 缓存实现：
 * 1. KLfuCache: 基础 LFU 缓存，使用频率哈希表和双向链表
 * 2. KHashLfuCache: 分片 LFU 缓存，提高并发性能
 *
 * 支持频率衰减机制：当平均访问频率超过阈值时，所有节点的频率衰减一半。
 * 所有实现均线程安全（使用互斥锁），并支持任意类型的键和值。
 */

namespace KamaCache
{

template<typename Key, typename Value> class KLfuCache;

/**
 * @class FreqList
 * @brief 频率链表，用于管理同一访问频率的所有节点
 *
 * 每个 FreqList 对应一个特定的访问频率，包含该频率下的所有缓存节点。
 * 使用双向链表组织节点，支持快速添加、删除和获取第一个节点。
 * 是 LFU 缓存的核心数据结构之一。
 */
template<typename Key, typename Value>
class FreqList
{
private:
    struct Node
    {
        int freq; // 访问频次
        Key key;
        Value value;
        std::weak_ptr<Node> pre; // 上一结点改为weak_ptr打破循环引用
        std::shared_ptr<Node> next;

        Node() 
        : freq(1), next(nullptr) {}
        Node(Key key, Value value) 
        : freq(1), key(key), value(value), next(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    int freq_; // 访问频率
    NodePtr head_; // 假头结点
    NodePtr tail_; // 假尾结点

public:
    explicit FreqList(int n) 
     : freq_(n) 
    {
      head_ = std::make_shared<Node>();
      tail_ = std::make_shared<Node>();
      head_->next = tail_;
      tail_->pre = head_;
    }

    bool isEmpty() const
    {
      return head_->next == tail_;
    }

    // 提那家结点管理方法
    void addNode(NodePtr node) 
    {
        if (!node || !head_ || !tail_) 
            return;

        node->pre = tail_->pre;
        node->next = tail_;
        tail_->pre.lock()->next = node; // 使用lock()获取shared_ptr
        tail_->pre = node;
    }

    void removeNode(NodePtr node)
    {
        if (!node || !head_ || !tail_)
            return;
        if (node->pre.expired() || !node->next) 
            return;

        auto pre = node->pre.lock(); // 使用lock()获取shared_ptr
        pre->next = node->next;
        node->next->pre = pre;
        node->next = nullptr; // 确保显式置空next指针，彻底断开节点与链表的连接
    }

    NodePtr getFirstNode() const { return head_->next; }
    
    friend class KLfuCache<Key, Value>;
};

/**
 * @class KLfuCache
 * @brief 基础 LFU（最不经常使用）缓存实现
 *
 * 实现经典的 LFU 缓存策略：当缓存满时，淘汰访问频率最低的项目。
 * 使用多个频率链表（FreqList）组织节点，每个链表包含相同频率的节点。
 * 支持频率衰减机制：当平均访问频率超过阈值时，所有节点的频率减半，
 * 防止高频节点永久占据缓存。
 *
 * 特性：
 * 1. 维护最小频率（minFreq_）以快速找到淘汰候选
 * 2. 计算平均访问频率，超过阈值时触发衰减
 * 3. 线程安全，所有公共操作使用互斥锁保护
 */
template <typename Key, typename Value>
class KLfuCache : public KICachePolicy<Key, Value>
{
public:
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    /**
     * @brief 构造一个 LFU 缓存对象
     * @param capacity 缓存容量，必须大于 0
     * @param maxAverageNum 最大平均访问频率阈值，超过此值会触发频率衰减
     * @note 容量为 0 的缓存将拒绝所有添加操作
     */
    KLfuCache(int capacity, int maxAverageNum = 1000000)
    : capacity_(capacity), minFreq_(INT8_MAX), maxAverageNum_(maxAverageNum),
      curAverageNum_(0), curTotalNum_(0)
    {}

    ~KLfuCache() override = default;

    /**
     * @brief 向缓存中添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @note 如果键已存在，则更新其值并增加访问频率；
     *       如果键不存在且缓存已满，则淘汰频率最低的节点后添加新节点。
     *       线程安全。
     */
    void put(Key key, Value value) override
    {
        if (capacity_ == 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 重置其value值
            it->second->value = value;
            // 找到了直接调整就好了，不用再去get中再找一遍，但其实影响不大
            getInternal(it->second, value);
            return;
        }

        putInternal(key, value);
    }

    /**
     * @brief 从缓存中获取指定键的值（传出参数版本）
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在，value 被设置为对应的值，且节点的访问频率增加
     * @return false 如果键不存在，value 保持不变
     * @note 线程安全。如果键存在，会调用 getInternal 增加节点频率并调整位置
     */
    bool get(Key key, Value& value) override
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
          getInternal(it->second, value);
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
      Value value;
      get(key, value);
      return value;
    }

    /**
     * @brief 清空缓存，释放所有资源
     * @note 移除所有节点，清空哈希表和频率链表。线程安全。
     */
    void purge()
    {
      nodeMap_.clear();
      freqToFreqList_.clear();
    }

private:
    void putInternal(Key key, Value value); // 添加缓存
    void getInternal(NodePtr node, Value& value); // 获取缓存

    void kickOut(); // 移除缓存中的过期数据

    void removeFromFreqList(NodePtr node); // 从频率列表中移除节点
    void addToFreqList(NodePtr node); // 添加到频率列表

    void addFreqNum(); // 增加平均访问等频率
    void decreaseFreqNum(int num); // 减少平均访问等频率
    void handleOverMaxAverageNum(); // 处理当前平均访问频率超过上限的情况
    void updateMinFreq();

private:
    int                                            capacity_; // 缓存容量
    int                                            minFreq_; // 最小访问频次(用于找到最小访问频次结点)
    int                                            maxAverageNum_; // 最大平均访问频次
    int                                            curAverageNum_; // 当前平均访问频次
    int                                            curTotalNum_; // 当前访问所有缓存次数总数 
    std::mutex                                     mutex_; // 互斥锁
    NodeMap                                        nodeMap_; // key 到 缓存节点的映射
    std::unordered_map<int, FreqList<Key, Value>*> freqToFreqList_;// 访问频次到该频次链表的映射
};

template<typename Key, typename Value>
void KLfuCache<Key, Value>::getInternal(NodePtr node, Value& value)
{
    // 找到之后需要将其从低访问频次的链表中删除，并且添加到+1的访问频次链表中，
    // 访问频次+1, 然后把value值返回
    value = node->value;
    // 从原有访问频次的链表中删除节点
    removeFromFreqList(node); 
    node->freq++;
    addToFreqList(node);
    // 如果当前node的访问频次如果等于minFreq+1，并且其前驱链表为空，则说明
    // freqToFreqList_[node->freq - 1]链表因node的迁移已经空了，需要更新最小访问频次
    if (node->freq - 1 == minFreq_ && freqToFreqList_[node->freq - 1]->isEmpty())
        minFreq_++;

    // 总访问频次和当前平均访问频次都随之增加
    addFreqNum();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::putInternal(Key key, Value value)
{   
    // 如果不在缓存中，则需要判断缓存是否已满
    if (nodeMap_.size() == capacity_)
    {
        // 缓存已满，删除最不常访问的结点，更新当前平均访问频次和总访问频次
        kickOut();
    }
    
    // 创建新结点，将新结点添加进入，更新最小访问频次
    NodePtr node = std::make_shared<Node>(key, value);
    nodeMap_[key] = node;
    addToFreqList(node);
    addFreqNum();
    minFreq_ = std::min(minFreq_, 1);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::kickOut()
{
    NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();
    removeFromFreqList(node);
    nodeMap_.erase(node->key);
    decreaseFreqNum(node->freq);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) 
        return;
    
    auto freq = node->freq;
    freqToFreqList_[freq]->removeNode(node);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addToFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) 
        return;

    // 添加进入相应的频次链表前需要判断该频次链表是否存在
    auto freq = node->freq;
    if (freqToFreqList_.find(node->freq) == freqToFreqList_.end())
    {
        // 不存在则创建
        freqToFreqList_[node->freq] = new FreqList<Key, Value>(node->freq);
    }

    freqToFreqList_[freq]->addNode(node);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addFreqNum()
{
    curTotalNum_++;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();

    if (curAverageNum_ > maxAverageNum_)
    {
       handleOverMaxAverageNum();
    }
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::decreaseFreqNum(int num)
{
    // 减少平均访问频次和总访问频次
    curTotalNum_ -= num;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::handleOverMaxAverageNum()
{
    if (nodeMap_.empty())
        return;

    // 当前平均访问频次已经超过了最大平均访问频次，所有结点的访问频次- (maxAverageNum_ / 2)
    for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it)
    {
        // 检查结点是否为空
        if (!it->second)
            continue;

        NodePtr node = it->second;

        // 先从当前频率列表中移除
        removeFromFreqList(node);

        // 减少频率
        int oldFreq = node->freq;

        int decay = maxAverageNum_ / 2;
        node->freq -= decay;

        if (node->freq < 1)
            node->freq = 1;

        int delta = node->freq - oldFreq; 
        curTotalNum_ += delta;

        // 添加到新的频率列表
        addToFreqList(node);
    }

    // 更新最小频率
    updateMinFreq();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::updateMinFreq() 
{
    minFreq_ = INT8_MAX;
    for (const auto& pair : freqToFreqList_) 
    {
        if (pair.second && !pair.second->isEmpty()) 
        {
            minFreq_ = std::min(minFreq_, pair.first);
        }
    }
    if (minFreq_ == INT8_MAX) 
        minFreq_ = 1;
}

/**
 * @class KHashLfuCache
 * @brief 分片哈希 LFU 缓存
 *
 * 将总缓存容量分成多个独立的 LFU 缓存分片，每个分片有自己的锁。
 * 通过键的哈希值决定路由到哪个分片，从而减少锁竞争，提高并发性能。
 * 适用于多线程高并发场景，但可能因哈希不均匀导致分片负载不平衡。
 */
template<typename Key, typename Value>
class KHashLfuCache
{
public:
    /**
     * @brief 构造一个分片 LFU 缓存对象
     * @param capacity 总缓存容量
     * @param sliceNum 分片数量，如果为 0 则使用硬件并发线程数
     * @param maxAverageNum 每个分片的最大平均访问频率阈值
     * @note 总容量被均匀分配到各分片（向上取整），每个分片是一个独立的 KLfuCache 实例
     */
    KHashLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 10)
        : sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
        , capacity_(capacity)
    {
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_)); // 每个lfu分片的容量
        for (int i = 0; i < sliceNum_; ++i)
        {
            lfuSliceCaches_.emplace_back(new KLfuCache<Key, Value>(sliceSize, maxAverageNum));
        }
    }

    /**
     * @brief 向缓存中添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @note 根据键的哈希值选择分片，然后调用对应分片的 put 方法
     */
    void put(Key key, Value value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    /**
     * @brief 从缓存中获取指定键的值（传出参数版本）
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在，value 被设置为对应的值
     * @return false 如果键不存在，value 保持不变
     * @note 根据键的哈希值选择分片，然后调用对应分片的 get 方法
     */
    bool get(Key key, Value& value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    /**
     * @brief 从缓存中获取指定键的值（返回值版本）
     * @param key 要查找的键
     * @return Value 如果键存在则返回对应的值，否则返回默认构造的 Value 对象
     * @note 内部调用 bool get(Key, Value&) 版本，如果键不存在会返回默认值
     */
    Value get(Key key)
    {
        Value value;
        get(key, value);
        return value;
    }

    /**
     * @brief 清空所有分片缓存，释放资源
     * @note 遍历所有分片，调用每个分片的 purge() 方法。线程安全。
     */
    void purge()
    {
        for (auto& lfuSliceCache : lfuSliceCaches_)
        {
            lfuSliceCache->purge();
        }
    }

private:
    // 将key计算成对应哈希值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_; // 缓存总容量
    int sliceNum_; // 缓存分片数量
    std::vector<std::unique_ptr<KLfuCache<Key, Value>>> lfuSliceCaches_; // 缓存lfu分片容器
};

} // namespace KamaCache

