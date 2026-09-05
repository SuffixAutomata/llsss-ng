#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace rlife::llsss {

// Isolate separately allocated CPU worker buffers, not just their owners.
// Fixed 64-byte isolation matches the benchmark host; this is not a CUDA
// scratch-layout requirement. Preserve stronger element alignment as well.
template <class T> struct CacheAlignedAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;
  static constexpr std::size_t alignment = alignof(T) > 64 ? alignof(T) : 64;

  CacheAlignedAllocator() noexcept = default;
  template <class U> CacheAlignedAllocator(const CacheAlignedAllocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t count) {
    if(count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::bad_array_new_length();
    return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{alignment}));
  }

  void deallocate(T* pointer, std::size_t) noexcept {
    ::operator delete(pointer, std::align_val_t{alignment});
  }

  template <class U> bool operator==(const CacheAlignedAllocator<U>&) const noexcept { return true; }
};

}  // namespace rlife::llsss
