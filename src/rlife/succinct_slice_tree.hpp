#pragma once

#include <algorithm>
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

    void push_back(bool value) {
        if ((bit_count_ & 63U) == 0) {
            words_.push_back(0);
        }
        if (value) {
            words_.back() |= std::uint64_t{1} << (bit_count_ & 63U);
        }
        ++bit_count_;
    }

    void clear() noexcept { std::fill(words_.begin(), words_.end(), 0); }

    void set_all() noexcept {
        std::fill(words_.begin(), words_.end(), ~std::uint64_t{0});
        if (!words_.empty() && (bit_count_ & 63U) != 0) {
            words_.back() &= (std::uint64_t{1} << (bit_count_ & 63U)) - 1U;
        }
    }

    [[nodiscard]] bool get(std::uint64_t index) const noexcept {
        return ((words_[index >> 6U] >> (index & 63U)) & 1U) != 0;
    }

    void set(std::uint64_t index) noexcept {
        words_[index >> 6U] |= std::uint64_t{1} << (index & 63U);
    }

    void set(std::uint64_t index, bool value) noexcept {
        const auto mask = std::uint64_t{1} << (index & 63U);
        auto& word = words_[index >> 6U];
        word = value ? word | mask : word & ~mask;
    }

    // Parallel reductions assign disjoint destination words to workers.  The
    // caller is responsible for that ownership; no atomic operation is
    // needed here.
    void or_word(std::size_t word_index, std::uint64_t value) noexcept {
        words_[word_index] |= value;
    }

    [[nodiscard]] std::size_t word_size() const noexcept {
        return words_.size();
    }

    void append_ones(std::uint64_t count) {
        if (count == 0) return;
        const auto old_size = bit_count_;
        resize_preserving(old_size + count);
        set_range(old_size, bit_count_);
    }

    void append(const PackedTags& source) {
        if (source.bit_count_ == 0) return;
        const auto old_size = bit_count_;
        resize_preserving(old_size + source.bit_count_);
        const auto shift = static_cast<unsigned>(old_size & 63U);
        const auto output_word = static_cast<std::size_t>(old_size >> 6U);
        if (shift == 0) {
            std::copy(source.words_.begin(), source.words_.end(),
                      words_.begin() + static_cast<std::ptrdiff_t>(output_word));
        } else {
            for (std::size_t i = 0; i < source.words_.size(); ++i) {
                const auto value = source.words_[i];
                words_[output_word + i] |= value << shift;
                if (output_word + i + 1U < words_.size()) {
                    words_[output_word + i + 1U] |= value >> (64U - shift);
                }
            }
        }
        clear_unused_tail();
    }

    void clear_range(std::uint64_t begin, std::uint64_t end) noexcept {
        if (begin >= end) return;
        const auto first_word = static_cast<std::size_t>(begin >> 6U);
        const auto last_word = static_cast<std::size_t>((end - 1U) >> 6U);
        const auto first_offset = static_cast<unsigned>(begin & 63U);
        const auto last_offset = static_cast<unsigned>(end & 63U);
        if (first_word == last_word) {
            const auto high = last_offset == 0 ? ~std::uint64_t{0}
                                               : (std::uint64_t{1} << last_offset) - 1U;
            const auto low = first_offset == 0 ? std::uint64_t{0}
                                               : (std::uint64_t{1} << first_offset) - 1U;
            words_[first_word] &= ~(high & ~low);
            return;
        }
        words_[first_word] &= first_offset == 0
            ? std::uint64_t{0}
            : (std::uint64_t{1} << first_offset) - 1U;
        std::fill(words_.begin() + static_cast<std::ptrdiff_t>(first_word + 1U),
                  words_.begin() + static_cast<std::ptrdiff_t>(last_word), 0);
        if (last_offset == 0) {
            words_[last_word] = 0;
        } else {
            words_[last_word] &= ~((std::uint64_t{1} << last_offset) - 1U);
        }
    }

    [[nodiscard]] std::uint64_t count(std::uint64_t begin,
                                      std::uint64_t end) const noexcept {
        std::uint64_t total = 0;
        for (auto index = begin; index < end;) {
            const auto word_index = index >> 6U;
            const auto offset = static_cast<unsigned>(index & 63U);
            const auto take = static_cast<unsigned>(
                std::min<std::uint64_t>(64U - offset, end - index));
            const auto low = take == 64 ? ~std::uint64_t{0}
                                        : ((std::uint64_t{1} << take) - 1U);
            total += std::popcount((words_[word_index] >> offset) & low);
            index += take;
        }
        return total;
    }

    [[nodiscard]] std::size_t allocated_bytes() const noexcept {
        return words_.capacity() * sizeof(std::uint64_t);
    }

private:
    static std::size_t word_count(std::uint64_t bits) {
        return static_cast<std::size_t>((bits + 63U) / 64U);
    }

    void resize_preserving(std::uint64_t bit_count) {
        words_.resize(word_count(bit_count), 0);
        bit_count_ = bit_count;
        clear_unused_tail();
    }

    void set_range(std::uint64_t begin, std::uint64_t end) noexcept {
        if (begin >= end) return;
        const auto first_word = static_cast<std::size_t>(begin >> 6U);
        const auto last_word = static_cast<std::size_t>((end - 1U) >> 6U);
        const auto first_offset = static_cast<unsigned>(begin & 63U);
        const auto last_offset = static_cast<unsigned>(end & 63U);
        if (first_word == last_word) {
            const auto high = last_offset == 0 ? ~std::uint64_t{0}
                                               : (std::uint64_t{1} << last_offset) - 1U;
            const auto low = first_offset == 0 ? std::uint64_t{0}
                                               : (std::uint64_t{1} << first_offset) - 1U;
            words_[first_word] |= high & ~low;
            return;
        }
        words_[first_word] |= ~std::uint64_t{0} << first_offset;
        std::fill(words_.begin() + static_cast<std::ptrdiff_t>(first_word + 1U),
                  words_.begin() + static_cast<std::ptrdiff_t>(last_word),
                  ~std::uint64_t{0});
        words_[last_word] |= last_offset == 0
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << last_offset) - 1U;
    }

    void clear_unused_tail() noexcept {
        if (!words_.empty() && (bit_count_ & 63U) != 0) {
            words_.back() &= (std::uint64_t{1} << (bit_count_ & 63U)) - 1U;
        }
    }

    std::uint64_t bit_count_ = 0;
    std::vector<std::uint64_t> words_;
};

struct TagPair {
    TagPair() = default;
    explicit TagPair(std::uint64_t node_count)
        : planes{PackedTags(node_count), PackedTags(node_count)} {}

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

    SuccinctSliceTree()
        : words_(1, 0), level_begin_{0, 1}, node_count_(1), depth_(0) {
        rebuild_rank_directory();
    }

    [[nodiscard]] Node node_count() const noexcept { return node_count_; }
    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
    [[nodiscard]] Node level_begin(std::size_t depth) const {
        return level_begin_.at(depth);
    }
    [[nodiscard]] Node level_end(std::size_t depth) const {
        return level_begin_.at(depth + 1);
    }
    [[nodiscard]] Node leaf_begin() const noexcept { return level_begin_[depth_]; }
    [[nodiscard]] Node leaf_end() const noexcept { return node_count_; }
    [[nodiscard]] Node leaf_count() const noexcept { return node_count_ - leaf_begin(); }

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

    [[nodiscard]] Node child(Node node, std::uint8_t label) const {
        const auto mask = child_mask(node);
        if ((mask & (1U << label)) == 0) {
            throw std::logic_error("requested absent slice-tree child");
        }
        const auto word_index = static_cast<std::size_t>(node >> 4U);
        return child_from_word(node, label, words_[word_index]);
    }

    void append_uniform(std::uint8_t allowed_labels) {
        allowed_labels &= 0x0fU;
        const auto old_count = node_count_;
        const auto leaves = leaf_count();
        const auto fanout = static_cast<Node>(std::popcount(allowed_labels));
        if (fanout != 0 && leaves > (std::numeric_limits<Node>::max() - old_count) / fanout) {
            throw std::overflow_error("slice tree is too large");
        }
        for (Node node = leaf_begin(); node < old_count; ++node) {
            set_child_mask(node, allowed_labels);
        }
        resize_nodes(old_count + leaves * fanout);
        level_begin_.push_back(node_count_);
        ++depth_;
        rebuild_rank_directory();
    }

    void expand_leaves() { append_uniform(0x0fU); }

    // Set all internal tag bits from the already-set leaf bits.
    bool close_from_leaves(PackedTags& tags) const {
        if (tags.size() != node_count_) {
            throw std::logic_error("tag/tree size mismatch");
        }
        tags.clear_range(0, leaf_begin());
        std::vector<Node> child_cursor(depth_);
        for (std::size_t depth = 0; depth < depth_; ++depth) {
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
        if (tags.size() != node_count_) {
            throw std::logic_error("tag/tree size mismatch during reification");
        }
        tags.clear_range(0, leaf_begin());
        std::vector<Node> live_per_level(depth_ + 1, 0);
        std::vector<Node> child_cursor(depth_);
        for (std::size_t depth = 0; depth < depth_; ++depth) {
            child_cursor[depth] = level_begin_[depth + 1U];
        }
        if (!close_dfs(0, 0, child_cursor, tags, &live_per_level)) {
            return false;
        }
        validate_child_cursors(child_cursor);
        return compact_closed_tags(tags, live_per_level);
    }

    // The parallel sweeps already close their retained leaf planes in order
    // to omit dead coarse roots.  Reuse those internal bits and obtain the
    // per-level counts with packed popcounts instead of repeating the DFS.
    bool reify_closed(PackedTags& tags) {
        if (tags.size() != node_count_) {
            throw std::logic_error(
                "tag/tree size mismatch during closed reification");
        }
        if (!tags.get(0)) return false;
        std::vector<Node> live_per_level(depth_ + 1U, 0);
        for (std::size_t depth = 0; depth <= depth_; ++depth) {
            live_per_level[depth] = tags.count(
                level_begin_[depth], level_begin_[depth + 1U]);
        }
        return compact_closed_tags(tags, live_per_level);
    }

    [[nodiscard]] std::vector<std::uint8_t> lineage(Node leaf) const {
        if (leaf < leaf_begin() || leaf >= leaf_end()) {
            throw std::out_of_range("node is not a current slice-tree leaf");
        }
        std::vector<std::uint8_t> path;
        path.reserve(depth_);
        if (!lineage_dfs(0, 0, leaf, path)) {
            throw std::logic_error("slice-tree leaf has no lineage");
        }
        return path;
    }

    [[nodiscard]] std::size_t bitstream_bytes() const noexcept {
        return words_.size() * sizeof(std::uint64_t);
    }
    [[nodiscard]] std::size_t rank_bytes() const noexcept {
        return absolute_rank_.size() * sizeof(std::uint64_t)
             + relative_rank_.size() * sizeof(std::uint16_t);
    }
    [[nodiscard]] std::size_t level_index_bytes() const noexcept {
        return level_begin_.size() * sizeof(Node);
    }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept {
        return words_.capacity() * sizeof(std::uint64_t)
             + absolute_rank_.capacity() * sizeof(std::uint64_t)
             + relative_rank_.capacity() * sizeof(std::uint16_t)
             + level_begin_.capacity() * sizeof(Node);
    }

private:
    static constexpr std::size_t nodes_per_word = 16;
    static constexpr std::size_t nodes_per_absolute_chunk = 8192;
    static constexpr std::size_t words_per_absolute_chunk =
        nodes_per_absolute_chunk / nodes_per_word;

    static std::size_t word_count_for_nodes(Node nodes) {
        return static_cast<std::size_t>((nodes + nodes_per_word - 1U) / nodes_per_word);
    }

    bool compact_closed_tags(PackedTags& tags,
                             const std::vector<Node>& live_per_level) {
        Node write = 0;
        Node old_child_cursor = 1;
        std::size_t output_word_index = 0;
        std::uint64_t output_word = 0;
        const auto old_count = node_count_;
        const auto old_word_count = words_.size();
        for (std::size_t word_index = 0;
             word_index < old_word_count; ++word_index) {
            // Earlier compacted nibbles may land in this word.  Save its old
            // contents before processing any of its original node records.
            const auto original_word = words_[word_index];
            const Node first = static_cast<Node>(word_index) * 16U;
            const Node last = std::min<Node>(first + 16U, old_count);
            for (Node node = first; node < last; ++node) {
                const auto shift =
                    static_cast<unsigned>((node & 15U) * 4U);
                const auto original_mask = static_cast<std::uint8_t>(
                    (original_word >> shift) & 0x0fU);
                auto old_child = old_child_cursor;
                old_child_cursor +=
                    static_cast<Node>(std::popcount(original_mask));
                if (!tags.get(node)) continue;
                std::uint8_t retained_mask = 0;
                for (std::uint8_t label = 0; label < 4; ++label) {
                    if ((original_mask & (1U << label)) == 0) continue;
                    if (tags.get(old_child++)) {
                        retained_mask |=
                            static_cast<std::uint8_t>(1U << label);
                    }
                }
                const auto output_shift =
                    static_cast<unsigned>((write & 15U) * 4U);
                output_word |=
                    static_cast<std::uint64_t>(retained_mask)
                    << output_shift;
                ++write;
                if ((write & 15U) == 0) {
                    words_[output_word_index++] = output_word;
                    output_word = 0;
                }
            }
        }
        if (old_child_cursor != old_count) {
            throw std::logic_error(
                "slice-tree compaction child cursor lost alignment");
        }
        if ((write & 15U) != 0) {
            words_[output_word_index] = output_word;
        }

        node_count_ = write;
        words_.resize(word_count_for_nodes(node_count_));
        clear_unused_tail();
        words_.shrink_to_fit();

        level_begin_.clear();
        level_begin_.reserve(depth_ + 2U);
        Node begin = 0;
        level_begin_.push_back(begin);
        for (const Node live : live_per_level) {
            begin += live;
            level_begin_.push_back(begin);
        }
        if (begin != node_count_ || live_per_level.front() != 1) {
            throw std::logic_error("slice-tree level accounting failed");
        }

        rebuild_rank_directory();
        return true;
    }

    void resize_nodes(Node nodes) {
        const auto old_words = words_.size();
        words_.resize(word_count_for_nodes(nodes), 0);
        if (words_.size() == old_words && nodes > node_count_) {
            // New records can share the formerly partial last word.
            const auto old_tail = static_cast<unsigned>((node_count_ & 15U) * 4U);
            if (old_tail != 0) {
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
        words_[word_index] = (words_[word_index] & ~field)
                           | (static_cast<std::uint64_t>(mask & 0x0fU) << shift);
    }

    void clear_unused_tail() noexcept {
        if (words_.empty()) {
            return;
        }
        const auto used = static_cast<unsigned>((node_count_ & 15U) * 4U);
        if (used != 0) {
            words_.back() &= (std::uint64_t{1} << used) - 1U;
        }
    }

    [[nodiscard]] Node child_from_word(Node node, std::uint8_t label,
                                       std::uint64_t original_word) const {
        const auto word_index = static_cast<std::size_t>(node >> 4U);
        const auto bit_offset = static_cast<unsigned>((node & 15U) * 4U + label);
        const auto lower = bit_offset == 0
            ? std::uint64_t{0}
            : (std::uint64_t{1} << bit_offset) - 1U;
        const auto rank = absolute_rank_[word_index / words_per_absolute_chunk]
                        + relative_rank_[word_index]
                        + static_cast<Node>(std::popcount(original_word & lower));
        return rank + 1U;
    }

    [[nodiscard]] ChildBlock child_block_from_word(
        Node node, std::uint64_t original_word) const noexcept {
        const auto word_index = static_cast<std::size_t>(node >> 4U);
        const auto shift = static_cast<unsigned>((node & 15U) * 4U);
        const auto lower = shift == 0
            ? std::uint64_t{0}
            : (std::uint64_t{1} << shift) - 1U;
        const auto rank = absolute_rank_[word_index / words_per_absolute_chunk]
                        + relative_rank_[word_index]
                        + static_cast<Node>(std::popcount(original_word & lower));
        return ChildBlock{
            rank + 1U,
            static_cast<std::uint8_t>((original_word >> shift) & 0x0fU),
        };
    }

    void rebuild_rank_directory() {
        absolute_rank_.clear();
        relative_rank_.assign(words_.size(), 0);
        absolute_rank_.reserve((words_.size() + words_per_absolute_chunk - 1U)
                               / words_per_absolute_chunk);
        Node total = 0;
        Node chunk_base = 0;
        for (std::size_t word = 0; word < words_.size(); ++word) {
            if (word % words_per_absolute_chunk == 0) {
                absolute_rank_.push_back(total);
                chunk_base = total;
            }
            const auto relative = total - chunk_base;
            if (relative > std::numeric_limits<std::uint16_t>::max()) {
                throw std::logic_error("slice-tree relative rank overflow");
            }
            relative_rank_[word] = static_cast<std::uint16_t>(relative);
            total += static_cast<Node>(std::popcount(words_[word]));
        }
        if (node_count_ != 0 && total + 1U != node_count_) {
            throw std::logic_error("slice-tree child bits do not describe its nodes");
        }
        absolute_rank_.shrink_to_fit();
        relative_rank_.shrink_to_fit();
    }

    void validate_child_cursors(const std::vector<Node>& child_cursor) const {
        for (std::size_t depth = 0; depth < depth_; ++depth) {
            if (child_cursor[depth] != level_begin_[depth + 2U]) {
                throw std::logic_error("slice-tree DFS child cursor lost alignment");
            }
        }
    }

    bool close_dfs(Node node, std::size_t depth,
                   std::vector<Node>& child_cursor, PackedTags& tags,
                   std::vector<Node>* live_per_level) const {
        bool live = false;
        if (depth == depth_) {
            live = tags.get(node);
        } else {
            const auto mask = child_mask(node);
            auto child = child_cursor[depth];
            child_cursor[depth] += static_cast<Node>(std::popcount(mask));
            for (std::uint8_t label = 0; label < 4; ++label) {
                if ((mask & (1U << label)) == 0) continue;
                if (close_dfs(child++, depth + 1, child_cursor,
                              tags, live_per_level)) {
                    live = true;
                }
            }
            tags.set(node, live);
        }
        if (live && live_per_level != nullptr) {
            ++(*live_per_level)[depth];
        }
        return live;
    }

    bool lineage_dfs(Node node, std::size_t depth, Node target,
                     std::vector<std::uint8_t>& path) const {
        if (depth == depth_) {
            return node == target;
        }
        const auto children = child_block(node);
        auto child = children.first;
        for (std::uint8_t label = 0; label < 4; ++label) {
            if ((children.mask & (1U << label)) == 0) continue;
            const auto next = child++;
            path.push_back(label);
            if (lineage_dfs(next, depth + 1, target, path)) {
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
    Node node_count_ = 0;
    std::size_t depth_ = 0;
};

} // namespace rlife::llsss
