#pragma once

#include "succinct_slice_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rlife::llsss {

// A sweep never addresses historical trie nodes through its BCAF planes.  The
// leaf origin keeps hot callers in absolute node coordinates while storing
// only the current-leaf interval.
struct LeafTags {
  using Node = SuccinctSliceTree::Node;

  LeafTags() = default;
  LeafTags(Node first, Node count) : first(first), bits(count) {}

  [[nodiscard]] bool get(Node node) const noexcept { return bits.get(node - first); }
  void set(Node node) noexcept { bits.set(node - first); }
  void atomic_set(Node node) noexcept { bits.atomic_set(node - first); }
  void set(Node node, bool value) noexcept { bits.set(node - first, value); }
  void clear() noexcept { bits.clear(); }
  [[nodiscard]] std::uint8_t get_4(Node node) const noexcept { return bits.get_4(node - first); }
  void or_4(Node node, std::uint8_t value) noexcept { bits.or_4(node - first, value); }
  void atomic_or_4(Node node, std::uint8_t value) noexcept { bits.atomic_or_4(node - first, value); }

  // OR an absolute leaf interval from a full-tree tag plane into this
  // leaf-local plane. Source and destination have a fixed bit offset but are
  // not necessarily word aligned.
  void or_source_range(const PackedTags& source, Node begin, Node end) noexcept {
    auto source_bit = begin;
    auto destination_bit = begin - first;
    auto remaining = end - begin;
    while(remaining != 0) {
      const auto destination_offset = static_cast<unsigned>(destination_bit & 63U);
      const auto take = static_cast<unsigned>(std::min<Node>(remaining, 64U - destination_offset));
      const auto source_word = static_cast<std::size_t>(source_bit >> 6U);
      const auto source_offset = static_cast<unsigned>(source_bit & 63U);
      auto value = source.word(source_word) >> source_offset;
      if(source_offset != 0 && source_offset + take > 64U && source_word + 1U < source.word_size())
        value |= source.word(source_word + 1U) << (64U - source_offset);
      if(take != 64U)
        value &= (std::uint64_t{1} << take) - 1U;
      bits.or_word(static_cast<std::size_t>(destination_bit >> 6U), value << destination_offset);
      source_bit += take;
      destination_bit += take;
      remaining -= take;
    }
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept { return bits.allocated_bytes(); }

  Node first = 0;
  PackedTags bits;
};

struct LeafTagPair {
  using Node = SuccinctSliceTree::Node;

  LeafTagPair() = default;
  LeafTagPair(Node first, Node count) : planes{LeafTags(first, count), LeafTags(first, count)} {}

  LeafTags& operator[](std::size_t index) { return planes[index]; }
  const LeafTags& operator[](std::size_t index) const { return planes[index]; }

  LeafTags planes[2];
};

} // namespace rlife::llsss
