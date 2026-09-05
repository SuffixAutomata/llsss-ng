#include "rlife/cache_aligned_allocator.hpp"
#include "rlife/geometry_acceptance.hpp"
#include "rlife/succinct_slice_tree.hpp"

#include <iostream>
#include <numeric>
#include <random>
#include <set>

using namespace rlife::llsss;

static void require(bool condition) {
  if(!condition)
    throw std::runtime_error("structural regression mismatch");
}

template <class T> static void test_buffer_alignment() {
  using Buffer = std::vector<T, CacheAlignedAllocator<T>>;
  for(const std::size_t depth : {1, 24, 43, 44, 48, 65, 86, 128}) {
    std::vector<Buffer> workers(20);
    for(auto& buffer : workers)
      buffer.resize(depth);
    for(unsigned growth = 0; growth < 2; ++growth) {
      std::set<std::uintptr_t> lines;
      for(auto& buffer : workers) {
        if(growth != 0)
          buffer.resize(depth + 1);
        const auto first = reinterpret_cast<std::uintptr_t>(buffer.data());
        require(first % CacheAlignedAllocator<T>::alignment == 0);
        const auto last = first + buffer.size() * sizeof(T) - 1;
        for(auto line = first / 64; line <= last / 64; ++line)
          require(lines.insert(line).second);
      }
    }
  }
}

static void test_scratch_allocator() {
  struct alignas(128) OverAligned { std::uint8_t byte; };
  test_buffer_alignment<std::uint8_t>();
  test_buffer_alignment<std::uint64_t>();
  test_buffer_alignment<std::array<std::uint64_t, 3>>();
  test_buffer_alignment<std::array<std::uint64_t, 5>>();
  test_buffer_alignment<OverAligned>();
  using Buffer = std::vector<std::uint8_t, CacheAlignedAllocator<std::uint8_t>>;
  Buffer original(43, 7);
  original.resize(44, 9);
  auto copy = original;
  auto moved = std::move(copy);
  require(moved == original && moved.front() == 7 && moved.back() == 9);
  copy = moved;
  original.swap(copy);
  require(original == moved);
  CacheAlignedAllocator<std::uint64_t> allocator;
  bool rejected = false;
  try {
    auto* pointer = allocator.allocate(std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 1);
    allocator.deallocate(pointer, 0);
  } catch(const std::bad_array_new_length&) {
    rejected = true;
  }
  require(rejected);
}

static void test_history() {
  for(const auto* rule_name : {"B3/S12", "B36/S125", "B36/S245"}) {
    const auto rule = RuleTable::parse(rule_name);
    for(int period = 3; period <= 11; ++period) {
      for(int displacement = 1; displacement < period; ++displacement) {
        if(std::gcd(period, displacement) != 1)
          continue;
        const auto geometry = Geometry::parse(std::to_string(displacement) + "c" + std::to_string(period) + "-f2b");
        GeometryAcceptance acceptance;
        acceptance.build(geometry, rule, EdgeMode::Background, EdgeMode::Background);
        const auto mask = acceptance.packed_history_mask();
#if defined(__BMI2__)
        require((mask != 0) == (period <= 10));
        if(mask == 0)
          continue;
        // Exhaust every key, including the largest supported history window.
        for(std::uint64_t key = 0; key < (1U << 15U); ++key) {
          const auto packed = _pdep_u64(key, mask);
          std::array<std::uint8_t, 21> bytes{};
          for(std::size_t i = 0; i < bytes.size(); ++i)
            bytes[20U - i] = (packed >> (3U * i)) & 7U;
          require(acceptance.packed_history_table()[key] == acceptance.interior_mask(bytes.data(), bytes.size()));
        }
#else
        require(mask == 0);
#endif
      }
    }
    for(const auto* name : {"c6d-f2b", "2c4-f2b", "c11-f2b"}) {
      GeometryAcceptance acceptance;
      acceptance.build(Geometry::parse(name), rule, EdgeMode::Background, EdgeMode::Background);
      require(acceptance.packed_history_mask() == 0);
    }
  }
}

static void same_tree(const SuccinctSliceTree& a, const SuccinctSliceTree& b) {
  require(a.node_count() == b.node_count());
  require(a.checkpoint_words() == b.checkpoint_words());
  require(a.checkpoint_levels() == b.checkpoint_levels());
  require(a.bcaf_checkpoint_bytes() == b.bcaf_checkpoint_bytes());
}

static void test_trees() {
  std::mt19937_64 random(0x63b17);
  for(unsigned trial = 0; trial < 70; ++trial) {
    SuccinctSliceTree seed;
    // Include the root-only case, word boundaries, and multi-task compaction.
    const auto depth = trial == 69 ? 9U : trial % 7U;
    for(unsigned level = 0; level < depth; ++level) {
      seed.append_uniform(15);
      if(trial != 69) {
        PackedTags subset(seed.node_count());
        for(auto leaf = seed.leaf_begin(); leaf < seed.leaf_end(); ++leaf)
          if(random() % 3 != 0)
            subset.set(leaf);
        subset.set(seed.leaf_begin());
        require(seed.reify(subset));
      }
    }
    if(depth != 0) {
      seed.initialize_bcaf_clauses(1);
      for(std::uint64_t parent = 0; parent < seed.leaf_begin(); ++parent) {
        const auto mask = seed.child_mask(parent);
        seed.set_bcaf_child_clauses(parent, random() & (mask | (mask << 4U)));
      }
    }
    auto material = seed;
    auto virtual_tree = seed;
    if(trial % 5 != 0 || trial == 69) {
      material.append_uniform(15);
      virtual_tree.expand_leaves();
      require(material.node_count() == virtual_tree.node_count());
      require(virtual_tree.bitstream_bytes() <= material.bitstream_bytes());
    }
    PackedTags keep(material.node_count());
    std::vector<std::vector<std::uint8_t>> expected;
    for(auto leaf = material.leaf_begin(); leaf < material.leaf_end(); ++leaf) {
      if(random() % 4 == 0 || leaf == material.leaf_begin()) {
        keep.set(leaf);
        expected.push_back(material.lineage(leaf));
        require(virtual_tree.lineage(leaf) == expected.back());
        require(virtual_tree.ancestry(leaf) == material.ancestry(leaf));
      }
    }
    auto grouped_tree = virtual_tree;
    auto material_keep = keep;
    auto grouped_keep = keep;
    require(material.reify(material_keep));
    require(virtual_tree.reify(keep));
    require(SuccinctSliceTree::reify_parallel_group({&grouped_tree}, {&grouped_keep}, 4));
    same_tree(material, virtual_tree);
    same_tree(material, grouped_tree);
    require(material.leaf_count() == expected.size());
    for(std::size_t i = 0; i < expected.size(); ++i)
      require(material.lineage(material.leaf_begin() + i) == expected[i]);
    const auto restored = SuccinctSliceTree::from_checkpoint(material.checkpoint_words(), material.checkpoint_levels(), material.node_count(), material.depth());
    require(restored.checkpoint_words() == material.checkpoint_words());
    PackedTags empty(material.node_count());
    require(!material.reify(empty));
    require(!SuccinctSliceTree::reify_parallel_group({&grouped_tree}, {&empty}, 4));
  }
}

int main() {
  test_scratch_allocator();
  test_history();
  test_trees();
  std::cout << "scratch-alignment, packed-history and virtual-leaf regression passed\n";
}
