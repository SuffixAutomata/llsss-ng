#pragma once

#include "indexed_executor.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rlife::llsss {

class PackedTags {
public:
  PackedTags() = default;
  explicit PackedTags(std::uint64_t bit_count) { reset_size(bit_count); }

  void reset_size(std::uint64_t bit_count) {
    bit_count_ = bit_count;
    words_.assign(word_count(bit_count), 0);
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return bit_count_; }

  void reserve_bits(std::uint64_t bit_count) { words_.reserve(word_count(bit_count)); }

  void push_back(bool value) {
    if((bit_count_ & 63U) == 0) {
      words_.push_back(0);
    }
    if(value) {
      words_.back() |= std::uint64_t{1} << (bit_count_ & 63U);
    }
    ++bit_count_;
  }

  // Append up to eight already-compacted low bits with one size update.  Pair
  // gates naturally produce a small mask per terminal parent pair, so doing
  // this a bit at a time needlessly repeats the word-boundary bookkeeping.
  void append_low_bits(std::uint8_t value, unsigned count) {
    if(count == 0 || count > 8)
      return;
    const auto old_size = bit_count_;
    const auto new_size = old_size + count;
    words_.resize(word_count(new_size), 0);
    const auto word_index = static_cast<std::size_t>(old_size >> 6U);
    const auto offset = static_cast<unsigned>(old_size & 63U);
    const auto bits = static_cast<std::uint64_t>(value) & ((std::uint64_t{1} << count) - 1U);
    words_[word_index] |= bits << offset;
    if(offset + count > 64U)
      words_[word_index + 1U] |= bits >> (64U - offset);
    bit_count_ = new_size;
  }

  void clear() noexcept { std::fill(words_.begin(), words_.end(), 0); }

  void set_all() noexcept {
    std::fill(words_.begin(), words_.end(), ~std::uint64_t{0});
    if(!words_.empty() && (bit_count_ & 63U) != 0) {
      words_.back() &= (std::uint64_t{1} << (bit_count_ & 63U)) - 1U;
    }
  }

  [[nodiscard]] bool get(std::uint64_t index) const noexcept { return ((words_[index >> 6U] >> (index & 63U)) & 1U) != 0; }

  void set(std::uint64_t index) noexcept { words_[index >> 6U] |= std::uint64_t{1} << (index & 63U); }

  // Concurrent writers may monotonically mark the same packed word. Readers
  // must wait until the parallel marking phase has completed.
  void atomic_set(std::uint64_t index) noexcept {
    const auto mask = std::uint64_t{1} << (index & 63U);
    std::atomic_ref<std::uint64_t> word(words_[index >> 6U]);
    if((word.load(std::memory_order_relaxed) & mask) == 0)
      word.fetch_or(mask, std::memory_order_relaxed);
  }

  void set(std::uint64_t index, bool value) noexcept {
    const auto mask = std::uint64_t{1} << (index & 63U);
    auto& word = words_[index >> 6U];
    word = value ? word | mask : word & ~mask;
  }

  [[nodiscard]] std::size_t word_size() const noexcept { return words_.size(); }

  [[nodiscard]] std::uint64_t word(std::size_t word_index) const noexcept { return words_[word_index]; }

  [[nodiscard]] std::uint8_t get_4(std::uint64_t index) const noexcept {
    const auto word_index = static_cast<std::size_t>(index >> 6U);
    const auto offset = static_cast<unsigned>(index & 63U);
    auto value = words_[word_index] >> offset;
    if(offset > 60U) {
      value |= words_[word_index + 1U] << (64U - offset);
    }
    return static_cast<std::uint8_t>(value & 0x0fU);
  }

  // Hot trie walkers already prove the first bit is in range.  This variant
  // permits the four-bit window to straddle the logical tail without an
  // out-of-bounds second-word load; any returned padding bits are zero.
  [[nodiscard]] std::uint8_t get_4_tail_safe(std::uint64_t index) const noexcept {
    const auto word_index = static_cast<std::size_t>(index >> 6U);
    const auto offset = static_cast<unsigned>(index & 63U);
    auto value = words_[word_index] >> offset;
    if(offset > 60U && word_index + 1U < words_.size())
      value |= words_[word_index + 1U] << (64U - offset);
    return static_cast<std::uint8_t>(value & 0x0fU);
  }

  [[nodiscard]] std::uint8_t get_low_bits(std::uint64_t index, unsigned count) const noexcept {
    if(count == 0)
      return 0;
    const auto word_index = static_cast<std::size_t>(index >> 6U);
    const auto offset = static_cast<unsigned>(index & 63U);
    auto value = words_[word_index] >> offset;
    if(offset + count > 64U)
      value |= words_[word_index + 1U] << (64U - offset);
    return static_cast<std::uint8_t>(value & ((std::uint64_t{1} << count) - 1U));
  }

  // Direct reductions can assign disjoint destination words to workers.
  void or_word(std::size_t word_index, std::uint64_t value) noexcept { words_[word_index] |= value; }

  void atomic_or_word(std::size_t word_index, std::uint64_t value) noexcept {
    std::atomic_ref<std::uint64_t> word(words_[word_index]);
    if((word.load(std::memory_order_relaxed) & value) != value)
      word.fetch_or(value, std::memory_order_relaxed);
  }

  void or_4(std::uint64_t index, std::uint8_t value) noexcept {
    const auto word_index = static_cast<std::size_t>(index >> 6U);
    const auto offset = static_cast<unsigned>(index & 63U);
    const auto bits = static_cast<std::uint64_t>(value & 0x0fU);
    words_[word_index] |= bits << offset;
    if(offset > 60U) {
      words_[word_index + 1U] |= bits >> (64U - offset);
    }
  }

  void atomic_or_4(std::uint64_t index, std::uint8_t value) noexcept {
    const auto word_index = static_cast<std::size_t>(index >> 6U);
    const auto offset = static_cast<unsigned>(index & 63U);
    const auto bits = static_cast<std::uint64_t>(value & 0x0fU);
    const auto low_bits = bits << offset;
    std::atomic_ref<std::uint64_t> low_word(words_[word_index]);
    if((low_word.load(std::memory_order_relaxed) & low_bits) != low_bits)
      low_word.fetch_or(low_bits, std::memory_order_relaxed);
    if(offset > 60U) {
      const auto high_bits = bits >> (64U - offset);
      std::atomic_ref<std::uint64_t> high_word(words_[word_index + 1U]);
      if((high_word.load(std::memory_order_relaxed) & high_bits) != high_bits)
        high_word.fetch_or(high_bits, std::memory_order_relaxed);
    }
  }

  void append_ones(std::uint64_t count) {
    if(count == 0)
      return;
    const auto old_size = bit_count_;
    resize_preserving(old_size + count);
    set_range(old_size, bit_count_);
  }

  void append_zeros(std::uint64_t count) {
    if(count != 0)
      resize_preserving(bit_count_ + count);
  }

  void append(const PackedTags& source) {
    if(source.bit_count_ == 0)
      return;
    const auto old_size = bit_count_;
    resize_preserving(old_size + source.bit_count_);
    const auto shift = static_cast<unsigned>(old_size & 63U);
    const auto output_word = static_cast<std::size_t>(old_size >> 6U);
    if(shift == 0) {
      std::copy(source.words_.begin(), source.words_.end(), words_.begin() + static_cast<std::ptrdiff_t>(output_word));
    } else {
      for(std::size_t i = 0; i < source.words_.size(); ++i) {
        const auto value = source.words_[i];
        words_[output_word + i] |= value << shift;
        if(output_word + i + 1U < words_.size()) {
          words_[output_word + i + 1U] |= value >> (64U - shift);
        }
      }
    }
    clear_unused_tail();
  }

  void clear_range(std::uint64_t begin, std::uint64_t end) noexcept {
    if(begin >= end)
      return;
    const auto first_word = static_cast<std::size_t>(begin >> 6U);
    const auto last_word = static_cast<std::size_t>((end - 1U) >> 6U);
    const auto first_offset = static_cast<unsigned>(begin & 63U);
    const auto last_offset = static_cast<unsigned>(end & 63U);
    if(first_word == last_word) {
      const auto high = last_offset == 0 ? ~std::uint64_t{0} : (std::uint64_t{1} << last_offset) - 1U;
      const auto low = first_offset == 0 ? std::uint64_t{0} : (std::uint64_t{1} << first_offset) - 1U;
      words_[first_word] &= ~(high & ~low);
      return;
    }
    words_[first_word] &= first_offset == 0 ? std::uint64_t{0} : (std::uint64_t{1} << first_offset) - 1U;
    std::fill(words_.begin() + static_cast<std::ptrdiff_t>(first_word + 1U), words_.begin() + static_cast<std::ptrdiff_t>(last_word), 0);
    if(last_offset == 0) {
      words_[last_word] = 0;
    } else {
      words_[last_word] &= ~((std::uint64_t{1} << last_offset) - 1U);
    }
  }

  [[nodiscard]] std::uint64_t count(std::uint64_t begin, std::uint64_t end) const noexcept {
    std::uint64_t total = 0;
    for(auto index = begin; index < end;) {
      const auto word_index = index >> 6U;
      const auto offset = static_cast<unsigned>(index & 63U);
      const auto take = static_cast<unsigned>(std::min<std::uint64_t>(64U - offset, end - index));
      const auto low = take == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << take) - 1U);
      total += std::popcount((words_[word_index] >> offset) & low);
      index += take;
    }
    return total;
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept { return words_.capacity() * sizeof(std::uint64_t); }

  [[nodiscard]] const std::vector<std::uint64_t>& checkpoint_words() const noexcept { return words_; }

  static PackedTags from_checkpoint(std::uint64_t bit_count, std::vector<std::uint64_t> words) {
    if(words.size() != word_count(bit_count)) {
      throw std::runtime_error("invalid packed tags in checkpoint");
    }
    if(!words.empty() && (bit_count & 63U) != 0) {
      const auto used = static_cast<unsigned>(bit_count & 63U);
      if((words.back() >> used) != 0) {
        throw std::runtime_error("packed checkpoint tags have nonzero padding");
      }
    }
    PackedTags result;
    result.bit_count_ = bit_count;
    result.words_ = std::move(words);
    return result;
  }

private:
  static std::size_t word_count(std::uint64_t bits) {
    const auto words = bits / 64U + static_cast<std::uint64_t>(bits % 64U != 0);
    if constexpr(sizeof(std::size_t) < sizeof(words)) {
      if(words > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("packed-tag word count does not fit this platform");
    }
    return static_cast<std::size_t>(words);
  }

  void resize_preserving(std::uint64_t bit_count) {
    words_.resize(word_count(bit_count), 0);
    bit_count_ = bit_count;
    clear_unused_tail();
  }

  void set_range(std::uint64_t begin, std::uint64_t end) noexcept {
    if(begin >= end)
      return;
    const auto first_word = static_cast<std::size_t>(begin >> 6U);
    const auto last_word = static_cast<std::size_t>((end - 1U) >> 6U);
    const auto first_offset = static_cast<unsigned>(begin & 63U);
    const auto last_offset = static_cast<unsigned>(end & 63U);
    if(first_word == last_word) {
      const auto high = last_offset == 0 ? ~std::uint64_t{0} : (std::uint64_t{1} << last_offset) - 1U;
      const auto low = first_offset == 0 ? std::uint64_t{0} : (std::uint64_t{1} << first_offset) - 1U;
      words_[first_word] |= high & ~low;
      return;
    }
    words_[first_word] |= ~std::uint64_t{0} << first_offset;
    std::fill(words_.begin() + static_cast<std::ptrdiff_t>(first_word + 1U), words_.begin() + static_cast<std::ptrdiff_t>(last_word), ~std::uint64_t{0});
    words_[last_word] |= last_offset == 0 ? ~std::uint64_t{0} : (std::uint64_t{1} << last_offset) - 1U;
  }

  void clear_unused_tail() noexcept {
    if(!words_.empty() && (bit_count_ & 63U) != 0) {
      words_.back() &= (std::uint64_t{1} << (bit_count_ & 63U)) - 1U;
    }
  }

  std::uint64_t bit_count_ = 0;
  std::vector<std::uint64_t> words_;
};

struct TagPair {
  TagPair() = default;
  explicit TagPair(std::uint64_t node_count) : planes{PackedTags(node_count), PackedTags(node_count)} {}

  PackedTags& operator[](std::size_t index) { return planes[index]; }
  const PackedTags& operator[](std::size_t index) const { return planes[index]; }

  PackedTags planes[2];
};

// A quaternary trie whose node records are exactly four child bits.  Nodes are
// in breadth-first order, so the child selected by bit position (4*n + label)
// is node 1 + rank1(4*n + label).  The rank directory adds one 16-bit relative
// count per 16 nodes and one 64-bit absolute count per 8192 nodes.
class SuccinctSliceTree {
public:
  using Node = std::uint64_t;

  struct ChildBlock {
    Node first = 0;
    std::uint8_t mask = 0;
  };

  SuccinctSliceTree() : words_(1, 0), level_begin_{0, 1}, node_count_(1), depth_(0) { rebuild_rank_directory(); }

  [[nodiscard]] Node node_count() const noexcept { return node_count_; }
  [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
  [[nodiscard]] Node level_begin(std::size_t depth) const { return level_begin_.at(depth); }
  [[nodiscard]] Node level_end(std::size_t depth) const { return level_begin_.at(depth + 1); }
  [[nodiscard]] Node leaf_begin() const noexcept { return level_begin_[depth_]; }
  [[nodiscard]] Node leaf_end() const noexcept { return node_count_; }
  [[nodiscard]] Node leaf_count() const noexcept { return node_count_ - leaf_begin(); }

  [[nodiscard]] bool bcaf_clauses_present() const noexcept { return bcaf_first_child_depth_ != 0; }
  [[nodiscard]] std::uint64_t bcaf_payload_size() const noexcept { return bcaf_child_clauses_.size(); }
  [[nodiscard]] std::size_t bcaf_first_child_depth() const noexcept { return bcaf_first_child_depth_; }

  // One byte belongs to an internal parent: its low nibble stores P and its
  // high nibble stores S at the four raw child-label positions. Parents above
  // the first historical clause cannot be queried and are omitted.
  void initialize_bcaf_clauses(std::size_t first_child_depth) {
    validate_bcaf_clauses();
    if(bcaf_clauses_present()) {
      if(first_child_depth != bcaf_first_child_depth_)
        throw std::logic_error("slice-tree BCAF clause depth changed");
      return;
    }
    if(first_child_depth == 0 || first_child_depth > depth_)
      throw std::logic_error("invalid first slice-tree BCAF clause depth");
    bcaf_first_child_depth_ = first_child_depth;
    bcaf_parent_begin_ = level_begin_[first_child_depth - 1U];
    bcaf_child_clauses_.assign(checked_byte_count(leaf_begin() - bcaf_parent_begin_), 0);
  }

  void clear_bcaf_clauses() noexcept {
    bcaf_child_clauses_.clear();
    bcaf_first_child_depth_ = 0;
    bcaf_parent_begin_ = 0;
  }

  void restore_bcaf_clauses(std::size_t first_child_depth, std::vector<std::uint8_t> child_clauses) {
    if(first_child_depth == 0) {
      if(!child_clauses.empty())
        throw std::runtime_error("slice-tree checkpoint has BCAF payload without a first clause depth");
      clear_bcaf_clauses();
      return;
    }
    if(first_child_depth > depth_)
      throw std::runtime_error("slice-tree checkpoint has an invalid first BCAF clause depth");
    bcaf_first_child_depth_ = first_child_depth;
    bcaf_parent_begin_ = level_begin_[first_child_depth - 1U];
    if(child_clauses.size() != checked_byte_count(leaf_begin() - bcaf_parent_begin_))
      throw std::runtime_error("slice-tree checkpoint BCAF byte payload has the wrong size");
    bcaf_child_clauses_ = std::move(child_clauses);
    validate_bcaf_clauses();
    for(Node parent = bcaf_parent_begin_; parent < leaf_begin(); ++parent) {
      const auto mask = child_mask(parent);
      const auto allowed = static_cast<std::uint8_t>(mask | (mask << 4U));
      if((bcaf_child_clauses_[static_cast<std::size_t>(parent - bcaf_parent_begin_)] & ~allowed) != 0)
        throw std::runtime_error("slice-tree checkpoint BCAF clause refers to an absent child");
    }
  }

  [[nodiscard]] std::uint8_t bcaf_child_clauses(Node parent) const {
    validate_bcaf_payload_parent(parent);
    return bcaf_child_clauses_unchecked(parent);
  }
  [[nodiscard]] std::uint8_t bcaf_child_clauses_unchecked(Node parent) const noexcept {
    return bcaf_child_clauses_[static_cast<std::size_t>(parent - bcaf_parent_begin_)];
  }
  void set_bcaf_child_clauses(Node parent, std::uint8_t clauses) {
    validate_bcaf_payload_parent(parent);
    const auto mask = child_mask(parent);
    const auto allowed = static_cast<std::uint8_t>(mask | (mask << 4U));
    if((clauses & ~allowed) != 0)
      throw std::logic_error("slice-tree BCAF clause refers to an absent child");
    set_bcaf_child_clauses_unchecked(parent, clauses);
  }
  void set_bcaf_child_clauses_unchecked(Node parent, std::uint8_t clauses) noexcept {
    bcaf_child_clauses_[static_cast<std::size_t>(parent - bcaf_parent_begin_)] = clauses;
  }
  [[nodiscard]] const std::vector<std::uint8_t>& bcaf_checkpoint_bytes() const noexcept { return bcaf_child_clauses_; }

  [[nodiscard]] std::uint8_t child_mask(Node node) const noexcept {
    const auto word = words_[static_cast<std::size_t>(node >> 4U)];
    const auto shift = static_cast<unsigned>((node & 15U) * 4U);
    return static_cast<std::uint8_t>((word >> shift) & 0x0fU);
  }

  // Children of one BFS node occupy a consecutive ID range.  Fetching the
  // range once avoids repeating the rank-directory loads and popcount for
  // every outgoing label in synchronized DFS.
  [[nodiscard]] ChildBlock child_block(Node node) const noexcept {
    const auto word_index = static_cast<std::size_t>(node >> 4U);
    return child_block_from_word(node, words_[word_index]);
  }

  // Immediately after expand_leaves(), every former leaf has four newly
  // appended children.  Their IDs are arithmetic, so synchronized pair walks
  // can avoid two rank-directory lookups at their dominant final level.
  [[nodiscard]] ChildBlock expanded_leaf_child_block(Node node) const noexcept {
    return ChildBlock{
      level_begin_[depth_] + 4U * (node - level_begin_[depth_ - 1U]),
      0x0fU,
    };
  }

  [[nodiscard]] Node child(Node node, std::uint8_t label) const {
    const auto mask = child_mask(node);
    if((mask & (1U << label)) == 0) {
      throw std::logic_error("requested absent slice-tree child");
    }
    const auto word_index = static_cast<std::size_t>(node >> 4U);
    return child_from_word(node, label, words_[word_index]);
  }

  void append_uniform(std::uint8_t allowed_labels) {
    validate_bcaf_clauses();
    allowed_labels &= 0x0fU;
    const auto old_count = node_count_;
    const auto old_leaf_begin = leaf_begin();
    const auto leaves = leaf_count();
    const auto fanout = static_cast<Node>(std::popcount(allowed_labels));
    if(fanout != 0 && leaves > (std::numeric_limits<Node>::max() - old_count) / fanout) {
      throw std::overflow_error("slice tree is too large");
    }
    for(Node node = leaf_begin(); node < old_count; ++node) {
      set_child_mask(node, allowed_labels);
    }
    resize_nodes(old_count + leaves * fanout);
    if(bcaf_clauses_present()) {
      bcaf_child_clauses_.resize(bcaf_child_clauses_.size() + checked_byte_count(old_count - old_leaf_begin), 0);
    }
    level_begin_.push_back(node_count_);
    ++depth_;
    rebuild_rank_directory();
  }

  void expand_leaves() { append_uniform(0x0fU); }

  // Set all internal tag bits from the already-set leaf bits.
  bool close_from_leaves(PackedTags& tags) const {
    if(tags.size() != node_count_) {
      throw std::logic_error("tag/tree size mismatch");
    }
    tags.clear_range(0, leaf_begin());
    std::vector<Node> child_cursor(depth_);
    for(std::size_t depth = 0; depth < depth_; ++depth) {
      child_cursor[depth] = level_begin_[depth + 1U];
    }
    const bool live = close_dfs(0, 0, child_cursor, tags, nullptr);
    validate_child_cursors(child_cursor);
    return live;
  }

  // Rebuild the trie from tagged current leaves.  A single DFS marks live
  // ancestry.  The following stable nibble compaction is in-place and uses
  // the unchanged old bitstream/rank directory to rewrite child masks.
  bool reify(PackedTags& tags) {
    validate_bcaf_clauses();
    if(tags.size() != node_count_) {
      throw std::logic_error("tag/tree size mismatch during reification");
    }
    tags.clear_range(0, leaf_begin());
    std::vector<Node> live_per_level(depth_ + 1, 0);
    std::vector<Node> child_cursor(depth_);
    for(std::size_t depth = 0; depth < depth_; ++depth) {
      child_cursor[depth] = level_begin_[depth + 1U];
    }
    if(!close_dfs(0, 0, child_cursor, tags, &live_per_level)) {
      return false;
    }
    validate_child_cursors(child_cursor);

    Node retained_count = 0;
    for(const auto live : live_per_level)
      retained_count += live;
    const bool compact_bcaf = bcaf_clauses_present();
    const auto old_bcaf_begin = compact_bcaf ? bcaf_parent_begin_ : Node{0};
    const auto old_leaf_begin = leaf_begin();
    Node retained_bcaf_begin = 0;
    Node retained_leaf_begin = retained_count;
    std::vector<std::uint8_t> retained_clauses;
    if(compact_bcaf) {
      for(std::size_t level = 0; level + 1U < bcaf_first_child_depth_; ++level)
        retained_bcaf_begin += live_per_level[level];
      retained_leaf_begin -= live_per_level.back();
      retained_clauses.assign(checked_byte_count(retained_leaf_begin - retained_bcaf_begin), 0);
    }

    Node write = 0;
    Node old_child_cursor = 1;
    std::size_t output_word_index = 0;
    std::uint64_t output_word = 0;
    const auto old_count = node_count_;
    const auto old_word_count = words_.size();
    for(std::size_t word_index = 0; word_index < old_word_count; ++word_index) {
      // Earlier compacted nibbles may land in this word.  Save its old
      // contents before processing any of its original node records.
      const auto original_word = words_[word_index];
      const Node first = static_cast<Node>(word_index) * 16U;
      const Node last = std::min<Node>(first + 16U, old_count);
      for(Node node = first; node < last; ++node) {
        const auto shift = static_cast<unsigned>((node & 15U) * 4U);
        const auto original_mask = static_cast<std::uint8_t>((original_word >> shift) & 0x0fU);
        auto old_child = old_child_cursor;
        old_child_cursor += static_cast<Node>(std::popcount(original_mask));
        if(!tags.get(node)) {
          continue;
        }
        std::uint8_t retained_mask = 0;
        for(std::uint8_t label = 0; label < 4; ++label) {
          if((original_mask & (1U << label)) == 0) {
            continue;
          }
          if(tags.get(old_child++)) {
            retained_mask |= static_cast<std::uint8_t>(1U << label);
          }
        }
        if(compact_bcaf && node >= old_bcaf_begin && node < old_leaf_begin) {
          const auto clauses = bcaf_child_clauses_[static_cast<std::size_t>(node - old_bcaf_begin)];
          const auto retained_labels = static_cast<std::uint8_t>(retained_mask | (retained_mask << 4U));
          retained_clauses[static_cast<std::size_t>(write - retained_bcaf_begin)] = static_cast<std::uint8_t>(clauses & retained_labels);
        }
        const auto output_shift = static_cast<unsigned>((write & 15U) * 4U);
        output_word |= static_cast<std::uint64_t>(retained_mask) << output_shift;
        ++write;
        if((write & 15U) == 0) {
          words_[output_word_index++] = output_word;
          output_word = 0;
        }
      }
    }
    if(old_child_cursor != old_count) {
      throw std::logic_error("slice-tree compaction child cursor lost alignment");
    }
    if(write != retained_count)
      throw std::logic_error("slice-tree BCAF compaction count lost alignment");
    if((write & 15U) != 0) {
      words_[output_word_index] = output_word;
    }

    node_count_ = write;
    words_.resize(word_count_for_nodes(node_count_));
    clear_unused_tail();
    words_.shrink_to_fit();
    if(compact_bcaf)
      bcaf_child_clauses_ = std::move(retained_clauses);

    level_begin_.clear();
    level_begin_.reserve(depth_ + 2);
    Node begin = 0;
    level_begin_.push_back(begin);
    for(const Node live : live_per_level) {
      begin += live;
      level_begin_.push_back(begin);
    }
    if(begin != node_count_ || live_per_level.front() != 1) {
      throw std::logic_error("slice-tree level accounting failed");
    }
    if(compact_bcaf)
      bcaf_parent_begin_ = retained_bcaf_begin;

    rebuild_rank_directory();
    return true;
  }

  // Rebuild a large, skew-dominant trie with all workers. Closure proceeds
  // level by level over independent, word-aligned parent ranges. Stable
  // compaction counts live nodes in coarse source ranges and emits each range
  // at its prefix offset into a separate bitstream.
  bool reify_parallel(PackedTags& tags, int requested_workers) {
    validate_bcaf_clauses();
    if(requested_workers <= 1 || node_count_ < parallel_reify_min_nodes)
      return reify(tags);
    if(tags.size() != node_count_)
      throw std::logic_error("tag/tree size mismatch during parallel reification");

    tags.clear_range(0, leaf_begin());
    std::vector<Node> live_per_level(depth_ + 1U, 0);
    live_per_level[depth_] = tags.count(leaf_begin(), leaf_end());
    for(std::size_t reverse_level = depth_; reverse_level > 0; --reverse_level) {
      const auto level = reverse_level - 1U;
      const auto begin = level_begin_[level];
      const auto end = level_begin_[level + 1U];
      const auto first_word = static_cast<std::size_t>(begin >> 6U);
      const auto past_word = static_cast<std::size_t>((end + 63U) >> 6U);
      const auto task_count = (past_word - first_word + close_words_per_task - 1U) / close_words_per_task;
      std::vector<Node> live_per_task(task_count, 0);
      ParallelCloseContext context{this, &tags, begin, end, first_word, &live_per_task};
      execute_indexed_tasks(task_count, requested_workers, &context, &execute_parallel_close);
      Node live = 0;
      for(const auto count : live_per_task)
        live += count;
      live_per_level[level] = live;
    }
    if(live_per_level.front() == 0)
      return false;

    const auto old_count = node_count_;
    const auto task_count = static_cast<std::size_t>((old_count + compact_nodes_per_task - 1U) / compact_nodes_per_task);
    std::vector<Node> live_offsets(task_count + 1U, 0);
    std::vector<Node> child_starts(task_count, 0);
    ParallelCountContext count_context{&tags, old_count, &live_offsets};
    execute_indexed_tasks(task_count, requested_workers, &count_context, &execute_parallel_count);
    for(std::size_t task = 0; task < task_count; ++task) {
      const auto begin = static_cast<Node>(task) * compact_nodes_per_task;
      child_starts[task] = child_block(begin).first;
    }
    for(std::size_t task = 0; task < task_count; ++task)
      live_offsets[task + 1U] += live_offsets[task];
    const auto live_count = live_offsets.back();

    // Emission no longer needs old rank, so release it before allocating the
    // transient output bitstream.
    std::vector<std::uint64_t>().swap(absolute_rank_);
    std::vector<std::uint16_t>().swap(relative_rank_);
    std::vector<std::uint64_t> output(word_count_for_nodes(live_count), 0);
    const bool compact_bcaf = bcaf_clauses_present();
    const auto old_bcaf_begin = compact_bcaf ? bcaf_parent_begin_ : Node{0};
    const auto old_leaf_begin = leaf_begin();
    Node retained_bcaf_begin = 0;
    Node retained_leaf_begin = live_count;
    std::vector<std::uint8_t> retained_clauses;
    if(compact_bcaf) {
      for(std::size_t level = 0; level + 1U < bcaf_first_child_depth_; ++level)
        retained_bcaf_begin += live_per_level[level];
      retained_leaf_begin -= live_per_level.back();
      retained_clauses.assign(checked_byte_count(retained_leaf_begin - retained_bcaf_begin), 0);
    }
    ParallelEmitContext emit_context{
      this,
      &tags,
      old_count,
      &live_offsets,
      &child_starts,
      &output,
      compact_bcaf ? &bcaf_child_clauses_ : nullptr,
      compact_bcaf ? &retained_clauses : nullptr,
      old_bcaf_begin,
      old_leaf_begin,
      retained_bcaf_begin,
    };
    execute_indexed_tasks(task_count, requested_workers, &emit_context, &execute_parallel_emit);

    words_.swap(output);
    std::vector<std::uint64_t>().swap(output);
    node_count_ = live_count;
    if(compact_bcaf)
      bcaf_child_clauses_ = std::move(retained_clauses);
    clear_unused_tail();
    level_begin_.clear();
    level_begin_.reserve(depth_ + 2U);
    Node begin = 0;
    level_begin_.push_back(begin);
    for(const Node live : live_per_level) {
      begin += live;
      level_begin_.push_back(begin);
    }
    if(begin != node_count_ || live_per_level.front() != 1)
      throw std::logic_error("parallel slice-tree level accounting failed");
    if(compact_bcaf)
      bcaf_parent_begin_ = retained_bcaf_begin;
    rebuild_rank_directory();
    return true;
  }

  // Reify several independent tries with one shared worklist per phase.  A
  // whole-tree task leaves cores idle when the slice sizes differ, while
  // calling reify_parallel() once per tree repeatedly forms small OpenMP
  // teams.  This variant exposes word/range tasks from every tree to the same
  // team.  Per-tree prefix sums retain the original BFS order exactly.
  static bool reify_parallel_group(const std::vector<SuccinctSliceTree*>& trees, const std::vector<PackedTags*>& tags, int requested_workers) {
    if(trees.size() != tags.size())
      throw std::invalid_argument("parallel reification tree/tag count mismatch");
    if(requested_workers <= 1) {
      for(std::size_t i = 0; i < trees.size(); ++i) {
        if(trees[i] == nullptr || tags[i] == nullptr)
          throw std::invalid_argument("parallel reification received a null tree or tag set");
        if(!trees[i]->reify(*tags[i]))
          return false;
      }
      return true;
    }

    std::vector<GroupReifyState> states(trees.size());
    std::size_t maximum_depth = 0;
    for(std::size_t i = 0; i < trees.size(); ++i) {
      auto* tree = trees[i];
      auto* keep = tags[i];
      if(tree == nullptr || keep == nullptr)
        throw std::invalid_argument("parallel reification received a null tree or tag set");
      tree->validate_bcaf_clauses();
      if(keep->size() != tree->node_count_)
        throw std::logic_error("tag/tree size mismatch during grouped parallel reification");
      keep->clear_range(0, tree->leaf_begin());
      auto& state = states[i];
      state.tree = tree;
      state.tags = keep;
      state.live_per_level.assign(tree->depth_ + 1U, 0);
      state.live_per_level[tree->depth_] = keep->count(tree->leaf_begin(), tree->leaf_end());
      maximum_depth = std::max(maximum_depth, tree->depth_);
    }

    std::vector<GroupCloseTask> close_tasks;
    std::vector<Node> close_counts;
    for(std::size_t reverse_level = maximum_depth; reverse_level > 0; --reverse_level) {
      const auto level = reverse_level - 1U;
      close_tasks.clear();
      for(std::size_t state_index = 0; state_index < states.size(); ++state_index) {
        const auto& state = states[state_index];
        if(level >= state.tree->depth_)
          continue;
        const auto begin = state.tree->level_begin_[level];
        const auto end = state.tree->level_begin_[level + 1U];
        const auto first_word = static_cast<std::size_t>(begin >> 6U);
        const auto past_word = static_cast<std::size_t>((end + 63U) >> 6U);
        for(auto word = first_word; word < past_word; word += close_words_per_task) {
          close_tasks.push_back(GroupCloseTask{
            state_index,
            std::max<Node>(begin, static_cast<Node>(word) * 64U),
            std::min<Node>(end, static_cast<Node>(word + close_words_per_task) * 64U),
          });
        }
      }
      close_counts.assign(close_tasks.size(), 0);
      GroupCloseContext close_context{&states, &close_tasks, &close_counts};
      execute_indexed_tasks(close_tasks.size(), requested_workers, &close_context, &execute_group_close);
      for(std::size_t task = 0; task < close_tasks.size(); ++task)
        states[close_tasks[task].state].live_per_level[level] += close_counts[task];
    }
    for(const auto& state : states) {
      if(state.live_per_level.front() == 0)
        return false;
    }

    std::vector<GroupLocalTask> count_tasks;
    for(std::size_t state_index = 0; state_index < states.size(); ++state_index) {
      auto& state = states[state_index];
      state.old_count = state.tree->node_count_;
      const auto task_count = static_cast<std::size_t>((state.old_count + compact_nodes_per_task - 1U) / compact_nodes_per_task);
      state.live_offsets.assign(task_count + 1U, 0);
      state.child_starts.resize(task_count);
      for(std::size_t task = 0; task < task_count; ++task)
        count_tasks.push_back(GroupLocalTask{state_index, task});
    }
    GroupCountContext count_context{&states, &count_tasks};
    execute_indexed_tasks(count_tasks.size(), requested_workers, &count_context, &execute_group_count);

    for(auto& state : states) {
      for(std::size_t task = 0; task < state.child_starts.size(); ++task) {
        const auto begin = static_cast<Node>(task) * compact_nodes_per_task;
        state.child_starts[task] = state.tree->child_block(begin).first;
      }
      for(std::size_t task = 0; task + 1U < state.live_offsets.size(); ++task)
        state.live_offsets[task + 1U] += state.live_offsets[task];
      const auto live_count = state.live_offsets.back();
      Node level_sum = 0;
      for(const auto live : state.live_per_level)
        level_sum += live;
      if(live_count != level_sum)
        throw std::logic_error("grouped parallel slice-tree live count lost alignment");

      // Rank is only needed to establish child_starts.  Dropping it before
      // allocating all output tries keeps the global schedule's peak small.
      std::vector<std::uint64_t>().swap(state.tree->absolute_rank_);
      std::vector<std::uint16_t>().swap(state.tree->relative_rank_);
      state.output.assign(word_count_for_nodes(live_count), 0);
      state.compact_bcaf = state.tree->bcaf_clauses_present();
      if(state.compact_bcaf) {
        state.old_bcaf_begin = state.tree->bcaf_parent_begin_;
        state.old_leaf_begin = state.tree->leaf_begin();
        for(std::size_t level = 0; level + 1U < state.tree->bcaf_first_child_depth_; ++level)
          state.retained_bcaf_begin += state.live_per_level[level];
        const auto retained_leaf_begin = live_count - state.live_per_level.back();
        state.retained_clauses.assign(checked_byte_count(retained_leaf_begin - state.retained_bcaf_begin), 0);
      }
    }

    std::vector<ParallelEmitContext> emit_contexts(states.size());
    std::vector<GroupLocalTask> emit_tasks;
    emit_tasks.reserve(count_tasks.size());
    for(std::size_t state_index = 0; state_index < states.size(); ++state_index) {
      auto& state = states[state_index];
      emit_contexts[state_index] = ParallelEmitContext{
        state.tree,
        state.tags,
        state.old_count,
        &state.live_offsets,
        &state.child_starts,
        &state.output,
        state.compact_bcaf ? &state.tree->bcaf_child_clauses_ : nullptr,
        state.compact_bcaf ? &state.retained_clauses : nullptr,
        state.old_bcaf_begin,
        state.old_leaf_begin,
        state.retained_bcaf_begin,
      };
      for(std::size_t task = 0; task < state.child_starts.size(); ++task)
        emit_tasks.push_back(GroupLocalTask{state_index, task});
    }
    GroupEmitContext emit_context{&emit_contexts, &emit_tasks};
    execute_indexed_tasks(emit_tasks.size(), requested_workers, &emit_context, &execute_group_emit);

    GroupFinalizeContext finalize_context{&states};
    execute_indexed_tasks(states.size(), requested_workers, &finalize_context, &execute_group_finalize);
    return true;
  }

  [[nodiscard]] std::vector<std::uint8_t> lineage(Node leaf) const {
    if(leaf < leaf_begin() || leaf >= leaf_end()) {
      throw std::out_of_range("node is not a current slice-tree leaf");
    }
    std::vector<std::uint8_t> path(depth_);
    auto node = leaf;
    for(std::size_t depth = depth_; depth > 0; --depth) {
      const auto target_rank = node - 1U;
      auto first_word = static_cast<std::size_t>(level_begin_[depth - 1U] >> 4U);
      auto last_word = static_cast<std::size_t>((level_begin_[depth] + 15U) >> 4U);
      while(first_word < last_word) {
        const auto middle = first_word + (last_word - first_word) / 2U;
        const auto rank_before = absolute_rank_[middle / words_per_absolute_chunk] + relative_rank_[middle];
        const auto rank_after = rank_before + static_cast<Node>(std::popcount(words_[middle]));
        if(rank_after <= target_rank) {
          first_word = middle + 1U;
        } else {
          last_word = middle;
        }
      }
      if(first_word >= words_.size())
        throw std::logic_error("slice-tree lineage select exceeded its bitstream");
      const auto rank_before = absolute_rank_[first_word / words_per_absolute_chunk] + relative_rank_[first_word];
      auto bits = words_[first_word];
      auto remaining = target_rank - rank_before;
      while(remaining != 0) {
        bits &= bits - 1U;
        --remaining;
      }
      if(bits == 0)
        throw std::logic_error("slice-tree lineage select missed its child bit");
      const auto selected = static_cast<Node>(std::countr_zero(bits));
      node = static_cast<Node>(first_word) * nodes_per_word + selected / 4U;
      path[depth - 1U] = static_cast<std::uint8_t>(selected & 0b11U);
      if(node < level_begin_[depth - 1U] || node >= level_begin_[depth])
        throw std::logic_error("slice-tree lineage select found the wrong level");
    }
    return path;
  }

  [[nodiscard]] std::size_t bitstream_bytes() const noexcept { return words_.size() * sizeof(std::uint64_t); }
  [[nodiscard]] std::size_t rank_bytes() const noexcept {
    return absolute_rank_.size() * sizeof(std::uint64_t) + relative_rank_.size() * sizeof(std::uint16_t);
  }
  [[nodiscard]] std::size_t level_index_bytes() const noexcept { return level_begin_.size() * sizeof(Node); }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return words_.capacity() * sizeof(std::uint64_t) + absolute_rank_.capacity() * sizeof(std::uint64_t) + relative_rank_.capacity() * sizeof(std::uint16_t) +
           level_begin_.capacity() * sizeof(Node) + bcaf_child_clauses_.capacity();
  }

  // The child bitstream and level boundaries are the complete non-derived
  // representation of a slice tree.  Checkpoints store these two arrays and
  // rebuild the rank directory on load.
  [[nodiscard]] const std::vector<std::uint64_t>& checkpoint_words() const noexcept { return words_; }
  [[nodiscard]] const std::vector<Node>& checkpoint_levels() const noexcept { return level_begin_; }

  static SuccinctSliceTree from_checkpoint(std::vector<std::uint64_t> words, std::vector<Node> levels, Node node_count, std::size_t depth) {
    if(node_count == 0 || depth > std::numeric_limits<std::size_t>::max() - 2U || levels.size() != depth + 2U || levels.front() != 0 || levels[1] != 1 ||
       levels.back() != node_count || words.size() != word_count_for_nodes(node_count)) {
      throw std::runtime_error("invalid slice tree in checkpoint");
    }
    for(std::size_t level = 0; level + 1U < levels.size(); ++level) {
      if(levels[level] > levels[level + 1U] || levels[level + 1U] > node_count) {
        throw std::runtime_error("invalid slice-tree levels in checkpoint");
      }
    }

    SuccinctSliceTree result;
    result.words_ = std::move(words);
    result.level_begin_ = std::move(levels);
    result.node_count_ = node_count;
    result.depth_ = depth;
    result.clear_unused_tail();
    result.rebuild_rank_directory();

    for(std::size_t level = 0; level < depth; ++level) {
      Node child_count = 0;
      for(Node node = result.level_begin_[level]; node < result.level_begin_[level + 1U]; ++node) {
        child_count += static_cast<Node>(std::popcount(result.child_mask(node)));
      }
      if(child_count != result.level_begin_[level + 2U] - result.level_begin_[level + 1U]) {
        throw std::runtime_error("slice-tree level contents do not match checkpoint boundaries");
      }
    }
    for(Node leaf = result.leaf_begin(); leaf < result.leaf_end(); ++leaf) {
      if(result.child_mask(leaf) != 0) {
        throw std::runtime_error("slice-tree checkpoint leaf has children");
      }
    }
    return result;
  }

private:
  static constexpr std::size_t nodes_per_word = 16;
  static constexpr std::size_t nodes_per_absolute_chunk = 8192;
  static constexpr std::size_t words_per_absolute_chunk = nodes_per_absolute_chunk / nodes_per_word;
  static constexpr Node parallel_reify_min_nodes = 1U << 18U;
  static constexpr std::size_t close_words_per_task = 1024;
  static constexpr Node compact_nodes_per_task = 1U << 18U;

  struct ParallelCloseContext {
    const SuccinctSliceTree* tree = nullptr;
    PackedTags* tags = nullptr;
    Node begin = 0;
    Node end = 0;
    std::size_t first_word = 0;
    std::vector<Node>* live_per_task = nullptr;
  };
  struct ParallelCountContext {
    const PackedTags* tags = nullptr;
    Node old_count = 0;
    std::vector<Node>* live_offsets = nullptr;
  };
  struct ParallelEmitContext {
    const SuccinctSliceTree* tree = nullptr;
    const PackedTags* tags = nullptr;
    Node old_count = 0;
    const std::vector<Node>* live_offsets = nullptr;
    const std::vector<Node>* child_starts = nullptr;
    std::vector<std::uint64_t>* output = nullptr;
    const std::vector<std::uint8_t>* source_clauses = nullptr;
    std::vector<std::uint8_t>* output_clauses = nullptr;
    Node source_clause_begin = 0;
    Node source_leaf_begin = 0;
    Node output_clause_begin = 0;
  };

  struct GroupReifyState {
    SuccinctSliceTree* tree = nullptr;
    PackedTags* tags = nullptr;
    Node old_count = 0;
    std::vector<Node> live_per_level;
    std::vector<Node> live_offsets;
    std::vector<Node> child_starts;
    std::vector<std::uint64_t> output;
    bool compact_bcaf = false;
    Node old_bcaf_begin = 0;
    Node old_leaf_begin = 0;
    Node retained_bcaf_begin = 0;
    std::vector<std::uint8_t> retained_clauses;
  };
  struct GroupCloseTask {
    std::size_t state = 0;
    Node begin = 0;
    Node end = 0;
  };
  struct GroupLocalTask {
    std::size_t state = 0;
    std::size_t local = 0;
  };
  struct GroupCloseContext {
    std::vector<GroupReifyState>* states = nullptr;
    const std::vector<GroupCloseTask>* tasks = nullptr;
    std::vector<Node>* live_counts = nullptr;
  };
  struct GroupCountContext {
    std::vector<GroupReifyState>* states = nullptr;
    const std::vector<GroupLocalTask>* tasks = nullptr;
  };
  struct GroupEmitContext {
    std::vector<ParallelEmitContext>* contexts = nullptr;
    const std::vector<GroupLocalTask>* tasks = nullptr;
  };
  struct GroupFinalizeContext {
    std::vector<GroupReifyState>* states = nullptr;
  };

  static void execute_parallel_close(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<ParallelCloseContext*>(opaque);
    const auto first_word = context.first_word + task * close_words_per_task;
    const auto past_word = first_word + close_words_per_task;
    const auto begin = std::max<Node>(context.begin, static_cast<Node>(first_word) * 64U);
    const auto end = std::min<Node>(context.end, static_cast<Node>(past_word) * 64U);
    if(begin >= end)
      return;
    auto child = context.tree->child_block(begin).first;
    Node live_count = 0;
    for(Node node = begin; node < end; ++node) {
      const auto mask = context.tree->child_mask(node);
      const auto fanout = static_cast<unsigned>(std::popcount(mask));
      const bool live = context.tags->get_low_bits(child, fanout) != 0;
      child += fanout;
      if(live) {
        context.tags->set(node);
        ++live_count;
      }
    }
    (*context.live_per_task)[task] = live_count;
  }

  static void execute_parallel_count(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<ParallelCountContext*>(opaque);
    const auto begin = static_cast<Node>(task) * compact_nodes_per_task;
    const auto end = std::min<Node>(context.old_count, begin + compact_nodes_per_task);
    (*context.live_offsets)[task + 1U] = context.tags->count(begin, end);
  }

  static std::uint8_t scatter_compact_bits(std::uint8_t mask, std::uint8_t compact) noexcept {
    std::uint8_t result = 0;
    std::uint8_t source_bit = 1;
    for(std::uint8_t label = 0; label < 4; ++label) {
      const auto label_bit = static_cast<std::uint8_t>(1U << label);
      if((mask & label_bit) == 0)
        continue;
      if((compact & source_bit) != 0)
        result = static_cast<std::uint8_t>(result | label_bit);
      source_bit = static_cast<std::uint8_t>(source_bit << 1U);
    }
    return result;
  }

  static void execute_parallel_emit(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<ParallelEmitContext*>(opaque);
    const auto begin = static_cast<Node>(task) * compact_nodes_per_task;
    const auto end = std::min<Node>(context.old_count, begin + compact_nodes_per_task);
    auto child = (*context.child_starts)[task];
    auto output_node = (*context.live_offsets)[task];
    auto output_word_index = static_cast<std::size_t>(output_node >> 4U);
    auto output_nibble = static_cast<unsigned>(output_node & 15U);
    bool boundary_word = output_nibble != 0;
    std::uint64_t output_word = 0;
    auto flush_word = [&] {
      if(boundary_word) {
        std::atomic_ref<std::uint64_t> destination((*context.output)[output_word_index]);
        destination.fetch_or(output_word, std::memory_order_relaxed);
      } else {
        (*context.output)[output_word_index] = output_word;
      }
      ++output_word_index;
      output_nibble = 0;
      boundary_word = false;
      output_word = 0;
    };
    for(Node node = begin; node < end; ++node) {
      const auto mask = context.tree->child_mask(node);
      const auto fanout = static_cast<unsigned>(std::popcount(mask));
      const auto compact_live = context.tags->get_low_bits(child, fanout);
      child += fanout;
      if(!context.tags->get(node))
        continue;
      const auto retained_mask = scatter_compact_bits(mask, compact_live);
      output_word |= static_cast<std::uint64_t>(retained_mask) << (4U * output_nibble);
      if(context.source_clauses != nullptr && node >= context.source_clause_begin && node < context.source_leaf_begin) {
        const auto clauses = (*context.source_clauses)[static_cast<std::size_t>(node - context.source_clause_begin)];
        const auto retained_labels = static_cast<std::uint8_t>(retained_mask | (retained_mask << 4U));
        (*context.output_clauses)[static_cast<std::size_t>(output_node - context.output_clause_begin)] = static_cast<std::uint8_t>(clauses & retained_labels);
      }
      ++output_nibble;
      ++output_node;
      if(output_nibble == 16U)
        flush_word();
    }
    if(output_nibble != 0) {
      std::atomic_ref<std::uint64_t> destination((*context.output)[output_word_index]);
      destination.fetch_or(output_word, std::memory_order_relaxed);
    }
    if(output_node != (*context.live_offsets)[task + 1U])
      throw std::logic_error("parallel slice-tree compaction count changed during emission");
  }

  static void execute_group_close(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<GroupCloseContext*>(opaque);
    const auto range = (*context.tasks)[task];
    auto& state = (*context.states)[range.state];
    auto child = state.tree->child_block(range.begin).first;
    Node live_count = 0;
    for(Node node = range.begin; node < range.end; ++node) {
      const auto mask = state.tree->child_mask(node);
      const auto fanout = static_cast<unsigned>(std::popcount(mask));
      const bool live = state.tags->get_low_bits(child, fanout) != 0;
      child += fanout;
      if(live) {
        state.tags->set(node);
        ++live_count;
      }
    }
    (*context.live_counts)[task] = live_count;
  }

  static void execute_group_count(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<GroupCountContext*>(opaque);
    const auto local = (*context.tasks)[task];
    auto& state = (*context.states)[local.state];
    const auto begin = static_cast<Node>(local.local) * compact_nodes_per_task;
    const auto end = std::min<Node>(state.old_count, begin + compact_nodes_per_task);
    state.live_offsets[local.local + 1U] = state.tags->count(begin, end);
  }

  static void execute_group_emit(void* opaque, std::size_t task, std::size_t worker) {
    auto& context = *static_cast<GroupEmitContext*>(opaque);
    const auto local = (*context.tasks)[task];
    execute_parallel_emit(&(*context.contexts)[local.state], local.local, worker);
  }

  static void execute_group_finalize(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<GroupFinalizeContext*>(opaque);
    auto& state = (*context.states)[task];
    auto& tree = *state.tree;
    tree.words_.swap(state.output);
    std::vector<std::uint64_t>().swap(state.output);
    tree.node_count_ = state.live_offsets.back();
    if(state.compact_bcaf) {
      tree.bcaf_child_clauses_ = std::move(state.retained_clauses);
      tree.bcaf_parent_begin_ = state.retained_bcaf_begin;
    }
    tree.clear_unused_tail();
    tree.level_begin_.clear();
    tree.level_begin_.reserve(tree.depth_ + 2U);
    Node begin = 0;
    tree.level_begin_.push_back(begin);
    for(const Node live : state.live_per_level) {
      begin += live;
      tree.level_begin_.push_back(begin);
    }
    if(begin != tree.node_count_ || state.live_per_level.front() != 1)
      throw std::logic_error("grouped parallel slice-tree level accounting failed");
    tree.rebuild_rank_directory();
  }

  static std::size_t word_count_for_nodes(Node nodes) {
    const auto words = nodes / nodes_per_word + static_cast<Node>(nodes % nodes_per_word != 0);
    if constexpr(sizeof(std::size_t) < sizeof(words)) {
      if(words > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("slice-tree word count does not fit this platform");
    }
    return static_cast<std::size_t>(words);
  }

  void validate_node(Node node) const {
    if(node >= node_count_)
      throw std::out_of_range("slice-tree node is out of range");
  }

  static std::size_t checked_byte_count(Node count) {
    if constexpr(sizeof(std::size_t) < sizeof(Node)) {
      if(count > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("slice-tree BCAF byte payload does not fit this platform");
    }
    return static_cast<std::size_t>(count);
  }

  void validate_bcaf_payload_parent(Node parent) const {
    if(!bcaf_clauses_present() || parent < bcaf_parent_begin_ || parent >= leaf_begin())
      throw std::out_of_range("node is outside the slice-tree BCAF parent payload");
  }

  void validate_bcaf_clauses() const {
    if(bcaf_first_child_depth_ == 0) {
      if(!bcaf_child_clauses_.empty() || bcaf_parent_begin_ != 0)
        throw std::logic_error("slice-tree BCAF payload lacks a first child depth");
      return;
    }
    if(bcaf_first_child_depth_ > depth_ || bcaf_parent_begin_ != level_begin_[bcaf_first_child_depth_ - 1U] ||
       bcaf_child_clauses_.size() != checked_byte_count(leaf_begin() - bcaf_parent_begin_))
      throw std::logic_error("slice-tree BCAF payload size mismatch");
  }

  void resize_nodes(Node nodes) {
    const auto old_words = words_.size();
    words_.resize(word_count_for_nodes(nodes), 0);
    if(words_.size() == old_words && nodes > node_count_) {
      // New records can share the formerly partial last word.
      const auto old_tail = static_cast<unsigned>((node_count_ & 15U) * 4U);
      if(old_tail != 0) {
        words_.back() &= (std::uint64_t{1} << old_tail) - 1U;
      }
    }
    node_count_ = nodes;
    clear_unused_tail();
  }

  void set_child_mask(Node node, std::uint8_t mask) noexcept {
    const auto word_index = static_cast<std::size_t>(node >> 4U);
    const auto shift = static_cast<unsigned>((node & 15U) * 4U);
    const auto field = std::uint64_t{0x0f} << shift;
    words_[word_index] = (words_[word_index] & ~field) | (static_cast<std::uint64_t>(mask & 0x0fU) << shift);
  }

  void clear_unused_tail() noexcept {
    if(words_.empty()) {
      return;
    }
    const auto used = static_cast<unsigned>((node_count_ & 15U) * 4U);
    if(used != 0) {
      words_.back() &= (std::uint64_t{1} << used) - 1U;
    }
  }

  [[nodiscard]] Node child_from_word(Node node, std::uint8_t label, std::uint64_t original_word) const {
    const auto word_index = static_cast<std::size_t>(node >> 4U);
    const auto bit_offset = static_cast<unsigned>((node & 15U) * 4U + label);
    const auto lower = bit_offset == 0 ? std::uint64_t{0} : (std::uint64_t{1} << bit_offset) - 1U;
    const auto rank = absolute_rank_[word_index / words_per_absolute_chunk] + relative_rank_[word_index] + static_cast<Node>(std::popcount(original_word & lower));
    return rank + 1U;
  }

  [[nodiscard]] ChildBlock child_block_from_word(Node node, std::uint64_t original_word) const noexcept {
    const auto word_index = static_cast<std::size_t>(node >> 4U);
    const auto shift = static_cast<unsigned>((node & 15U) * 4U);
    const auto lower = shift == 0 ? std::uint64_t{0} : (std::uint64_t{1} << shift) - 1U;
    const auto rank = absolute_rank_[word_index / words_per_absolute_chunk] + relative_rank_[word_index] + static_cast<Node>(std::popcount(original_word & lower));
    return ChildBlock{
      rank + 1U,
      static_cast<std::uint8_t>((original_word >> shift) & 0x0fU),
    };
  }

  void rebuild_rank_directory() {
    absolute_rank_.clear();
    relative_rank_.assign(words_.size(), 0);
    absolute_rank_.reserve((words_.size() + words_per_absolute_chunk - 1U) / words_per_absolute_chunk);
    Node total = 0;
    Node chunk_base = 0;
    for(std::size_t word = 0; word < words_.size(); ++word) {
      if(word % words_per_absolute_chunk == 0) {
        absolute_rank_.push_back(total);
        chunk_base = total;
      }
      const auto relative = total - chunk_base;
      if(relative > std::numeric_limits<std::uint16_t>::max()) {
        throw std::logic_error("slice-tree relative rank overflow");
      }
      relative_rank_[word] = static_cast<std::uint16_t>(relative);
      total += static_cast<Node>(std::popcount(words_[word]));
    }
    if(node_count_ != 0 && total + 1U != node_count_) {
      throw std::logic_error("slice-tree child bits do not describe its nodes");
    }
    absolute_rank_.shrink_to_fit();
    relative_rank_.shrink_to_fit();
  }

  void validate_child_cursors(const std::vector<Node>& child_cursor) const {
    for(std::size_t depth = 0; depth < depth_; ++depth) {
      if(child_cursor[depth] != level_begin_[depth + 2U]) {
        throw std::logic_error("slice-tree DFS child cursor lost alignment");
      }
    }
  }

  bool close_dfs(Node node, std::size_t depth, std::vector<Node>& child_cursor, PackedTags& tags, std::vector<Node>* live_per_level) const {
    bool live = false;
    if(depth == depth_) {
      live = tags.get(node);
    } else {
      const auto mask = child_mask(node);
      auto child = child_cursor[depth];
      child_cursor[depth] += static_cast<Node>(std::popcount(mask));
      for(std::uint8_t label = 0; label < 4; ++label) {
        if((mask & (1U << label)) == 0)
          continue;
        if(close_dfs(child++, depth + 1, child_cursor, tags, live_per_level)) {
          live = true;
        }
      }
      tags.set(node, live);
    }
    if(live && live_per_level != nullptr) {
      ++(*live_per_level)[depth];
    }
    return live;
  }

  bool lineage_dfs(Node node, std::size_t depth, Node target, std::vector<std::uint8_t>& path) const {
    if(depth == depth_) {
      return node == target;
    }
    const auto children = child_block(node);
    auto child = children.first;
    for(std::uint8_t label = 0; label < 4; ++label) {
      if((children.mask & (1U << label)) == 0)
        continue;
      const auto next = child++;
      path.push_back(label);
      if(lineage_dfs(next, depth + 1, target, path)) {
        return true;
      }
      path.pop_back();
    }
    return false;
  }

  std::vector<std::uint64_t> words_;
  std::vector<std::uint64_t> absolute_rank_;
  std::vector<std::uint16_t> relative_rank_;
  std::vector<Node> level_begin_;
  std::vector<std::uint8_t> bcaf_child_clauses_;
  std::size_t bcaf_first_child_depth_ = 0;
  Node bcaf_parent_begin_ = 0;
  Node node_count_ = 0;
  std::size_t depth_ = 0;
};

} // namespace rlife::llsss
