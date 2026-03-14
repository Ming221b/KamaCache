#pragma once

/**
 * @file KICachePolicy.h
 * @brief 缓存策略接口类定义
 *
 * 该文件定义了缓存策略的抽象接口类 KICachePolicy。
 * 所有具体的缓存实现（如 LRU、LFU、ARC 等）都应继承此接口。
 * 接口提供了基本的缓存操作：添加（put）和获取（get）。
 * 使用模板支持任意类型的键（Key）和值（Value）。
 */

namespace KamaCache
{

/**
 * @class KICachePolicy
 * @brief 缓存策略抽象基类
 *
 * 定义了缓存的基本操作接口，所有具体缓存策略必须实现这些纯虚函数。
 * 模板参数：
 *   - Key: 键的类型，用于唯一标识缓存项
 *   - Value: 值的类型，缓存中存储的数据
 */
template <typename Key, typename Value>
class KICachePolicy
{
public:
    /// @brief 虚析构函数，确保派生类对象能正确释放资源
    virtual ~KICachePolicy() {};

    /**
     * @brief 向缓存中添加或更新键值对
     * @param key 要添加或更新的键
     * @param value 与键关联的值
     * @note 如果键已存在，则更新对应的值；否则添加新条目
     */
    virtual void put(Key key, Value value) = 0;

    /**
     * @brief 从缓存中获取指定键的值
     * @param key 要查找的键
     * @param value 传出参数，用于接收找到的值
     * @return true 如果键存在于缓存中，value 被设置为对应的值
     * @return false 如果键不存在于缓存中，value 保持不变
     * @note 此版本使用传出参数返回找到的值，适用于不希望构造默认值的情况
     */
    virtual bool get(Key key, Value& value) = 0;
    /**
     * @brief 从缓存中获取指定键的值（通过返回值）
     * @param key 要查找的键
     * @return Value 如果键存在则返回对应的值，否则返回 Value 类型的默认构造值
     * @note 此版本通过返回值返回找到的值，如果键不存在会返回默认构造的 Value 对象
     * @warning 对于非平凡类型（如 std::string），返回默认构造值可能有性能开销
     */
    virtual Value get(Key key) = 0;

};

} // namespace KamaCache