#pragma once

#include "../KICachePolicy.h"
#include "KArcLruPart.h"
#include "KArcLfuPart.h"
#include <memory>

/**
 * @file KArcCache.h
 * @brief ARC（自适应替换缓存）实现
 *
 * ARC 缓存结合了 LRU 和 LFU 的优点，自适应地调整两种策略的权重。
 * 维护两个链表：LRU 部分（最近访问）和 LFU 部分（频繁访问），
 * 以及两个幽灵链表（ghost list）用于记录最近淘汰的条目。
 * 当访问幽灵链表中的键时，调整 LRU/LFU 部分的容量比例。
 */

namespace KamaCache
{

/**
 * @class KArcCache
 * @brief ARC（自适应替换缓存）实现类
 *
 * ARC 算法由 Nimrod Megiddo 和 Dharmendra S. Modha 提出，结合了 LRU 和 LFU 的优点。
 * 维护四个数据结构：
 * 1. T1: LRU 部分，存储最近访问一次的条目
 * 2. T2: LFU 部分，存储最近访问多次的条目
 * 3. B1: T1 的幽灵链表，记录最近从 T1 淘汰的键
 * 4. B2: T2 的幽灵链表，记录最近从 T2 淘汰的键
 *
 * 当缓存满时，根据幽灵链表的命中情况自适应调整 T1 和 T2 的容量比例。
 */
template<typename Key, typename Value>
class KArcCache : public KICachePolicy<Key, Value> 
{
public:
    /**
     * @brief 构造一个 ARC 缓存对象
     * @param capacity 总缓存容量（T1 + T2 的容量总和）
     * @param transformThreshold 转换阈值，节点访问次数达到此值后从 T1 移动到 T2
     * @note 初始化时 T1 和 T2 各分配一半容量，幽灵链表容量与对应主链表相同
     */
    explicit KArcCache(size_t capacity = 10, size_t transformThreshold = 2)
        : capacity_(capacity)
        , transformThreshold_(transformThreshold)
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold))
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold))
    {}

    ~KArcCache() override = default;

    /**
     * @brief 向缓存中添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @note 首先检查幽灵链表，如果命中则调整容量比例；
     *       然后检查键是否已在 LFU 部分，如果是则同时更新 LFU 部分；
     *       最后总是更新 LRU 部分（T1）。
     */
    void put(Key key, Value value) override 
    {
        checkGhostCaches(key);

        // 检查 LFU 部分是否存在该键
        bool inLfu = lfuPart_->contain(key);
        // 更新 LRU 部分缓存
        lruPart_->put(key, value);
        // 如果 LFU 部分存在该键，则更新 LFU 部分
        if (inLfu) 
        {
            lfuPart_->put(key, value);
        }
    }

    /**
     * @brief 从缓存中获取指定键的值（传出参数版本）
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在，value 被设置为对应的值
     * @return false 如果键不存在，value 保持不变
     * @note 首先检查幽灵链表，如果命中则调整容量比例；
     *       先在 LRU 部分（T1）查找，如果找到且访问次数达到阈值，则移动到 LFU 部分（T2）；
     *       如果在 LRU 部分未找到，则在 LFU 部分查找。
     */
    bool get(Key key, Value& value) override 
    {
        checkGhostCaches(key);

        bool shouldTransform = false;
        if (lruPart_->get(key, value, shouldTransform)) 
        {
            if (shouldTransform) 
            {
                lfuPart_->put(key, value);
            }
            return true;
        }
        return lfuPart_->get(key, value);
    }

    /**
     * @brief 从缓存中获取指定键的值（返回值版本）
     * @param key 要查找的键
     * @return Value 如果键存在则返回对应的值，否则返回默认构造的 Value 对象
     * @note 内部调用 bool get(Key, Value&) 版本，如果键不存在会返回默认值
     */
    Value get(Key key) override 
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    /**
     * @brief 检查键是否在幽灵链表中，并根据命中情况调整容量比例
     * @param key 要检查的键
     * @return true 如果键在任意幽灵链表中命中
     * @note 如果键在 LRU 的幽灵链表（B1）中命中，则减少 LFU 部分容量，增加 LRU 部分容量；
     *       如果键在 LFU 的幽灵链表（B2）中命中，则减少 LRU 部分容量，增加 LFU 部分容量。
     *       这是 ARC 算法自适应调整的核心逻辑。
     */
    bool checkGhostCaches(Key key) 
    {
        bool inGhost = false;
        if (lruPart_->checkGhost(key)) 
        {
            if (lfuPart_->decreaseCapacity()) 
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } 
        else if (lfuPart_->checkGhost(key)) 
        {
            if (lruPart_->decreaseCapacity()) 
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost;
    }

private:
    size_t capacity_;
    size_t transformThreshold_;
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;
};

} // namespace KamaCache