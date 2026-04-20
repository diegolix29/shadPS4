#include <algorithm>
#include <cstdint>
#include <memory>
#include <type_traits>

template <typename T, uint32_t N>
class SmallVector {
    T* m_data;
    uint32_t m_size{};
    uint32_t m_capacity;
    alignas(T) uint8_t m_inline_storage[sizeof(T) * N];

    [[gnu::cold, gnu::noinline]]
    void grow(uint32_t desired_capacity) {
        uint32_t new_cap = std::max(desired_capacity, std::max(m_capacity * 2, 8u));
        T* new_data = reinterpret_cast<T*>(std::malloc(sizeof(T) * new_cap));
        std::uninitialized_move_n(m_data, m_size, new_data);
        std::destroy_n(m_data, m_size);
        if (on_heap()) {
            std::free(m_data);
        }
        m_data = new_data;
        m_capacity = new_cap;
    }

public:
    using value_type = T;
    using size_type = uint32_t;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVector() noexcept : m_data{inline_storage()}, m_capacity{N} {}
    SmallVector(const SmallVector& other) {
        if (!other.on_heap()) {
            m_data = inline_storage();
            m_capacity = N;
        } else {
            m_data = reinterpret_cast<T*>(std::malloc(sizeof(T) * other.m_size));
            m_capacity = other.m_size;
        }
        m_size = other.m_size;
        std::uninitialized_copy_n(other.m_data, other.m_size, m_data);
    }
    SmallVector(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (!other.on_heap()) {
            m_data = inline_storage();
            std::uninitialized_move_n(other.m_data, other.m_size, m_data);
            std::destroy_n(other.m_data, other.m_size);
            m_capacity = N;
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
            other.m_data = other.inline_storage();
            other.m_capacity = N;
        }
        m_size = other.m_size;
        other.m_size = 0;
    }
    ~SmallVector() {
        std::destroy_n(m_data, m_size);
        if (on_heap()) {
            std::free(m_data);
        }
    }

    SmallVector& operator=(const SmallVector& other) = delete;
    SmallVector& operator=(SmallVector&& other) noexcept = delete;

    size_type size() const noexcept {
        return m_size;
    }
    size_type capacity() const noexcept {
        return m_capacity;
    }
    bool empty() const noexcept {
        return m_size == 0;
    }
    bool on_heap() const noexcept {
        return m_data != inline_storage();
    }
    T* inline_storage() noexcept {
        return reinterpret_cast<T*>(m_inline_storage);
    }
    const T* inline_storage() const noexcept {
        return reinterpret_cast<const T*>(m_inline_storage);
    }
    void clear() noexcept {
        std::destroy_n(m_data, m_size);
        m_size = 0;
    }
    void reserve(size_type size) {
        if (size > m_capacity) {
            grow(size);
        }
    }
    void resize(size_type size) {
        if (size > m_size) {
            if (size > m_capacity) {
                grow(size);
            }
            std::uninitialized_value_construct_n(m_data + m_size, size - m_size);
        } else if (size < m_size) {
            std::destroy_n(m_data + size, m_size - size);
        }
        m_size = size;
    }

    T& operator[](size_type i) noexcept {
        return m_data[i];
    }
    const T& operator[](size_type i) const noexcept {
        return m_data[i];
    }
    T& front() noexcept {
        return m_data[0];
    }
    const T& front() const noexcept {
        return m_data[0];
    }
    T& back() noexcept {
        return m_data[m_size - 1];
    }
    const T& back() const noexcept {
        return m_data[m_size - 1];
    }
    T* data() noexcept {
        return m_data;
    }
    const T* data() const noexcept {
        return m_data;
    }

    iterator begin() noexcept {
        return m_data;
    }
    iterator end() noexcept {
        return m_data + m_size;
    }
    const_iterator begin() const noexcept {
        return m_data;
    }
    const_iterator end() const noexcept {
        return m_data + m_size;
    }

    void push_back(const T& value) {
        if (m_size == m_capacity) [[unlikely]] {
            grow(m_size + 1);
        }
        std::construct_at(&m_data[m_size++], value);
    }
    void push_back(T&& value) {
        if (m_size == m_capacity) [[unlikely]] {
            grow(m_size + 1);
        }
        std::construct_at(&m_data[m_size++], std::move(value));
    }
    void pop_back() noexcept {
        std::destroy_at(&m_data[--m_size]);
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size == m_capacity) [[unlikely]] {
            grow(m_size + 1);
        }
        T* ptr = m_data + m_size++;
        std::construct_at(ptr, std::forward<Args>(args)...);
        return *ptr;
    }
};