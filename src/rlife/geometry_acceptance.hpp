#pragma once

#include "geometry.hpp"
#include "rule.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

#ifndef RLIFE_ENABLE_P7_GATHER
#define RLIFE_ENABLE_P7_GATHER 1
#endif

namespace rlife::llsss {

// Positions in the synchronized two-slice DFS.  The values are the raw
// three-column candidate bits read by the geometry-generated CA equations.
inline constexpr std::array<std::uint8_t, 8> geometry_pair_triple_order = {
    0, 4, 2, 6, 1, 5, 3, 7,
};

struct BoundaryStep {
  bool accepted = false;
  std::uint8_t history_label = 0;
};

// All geometry-dependent CA and reflection checks are compiled into small,
// read-only projections before the rule's partial lookup is released.  Search
// nodes retain only their original raw two-column labels.
class GeometryAcceptance {
public:
  void build(const Geometry& geometry, const RuleTable& rule, EdgeMode left_edge, EdgeMode right_edge) {
    short_window_ = geometry.short_window();
    subtile_count_ = static_cast<std::size_t>(geometry.subtile_count);
    for(auto& mode : boundary_acceptance_) {
      for(auto& side : mode) {
        side.clear();
      }
    }
    boundary_built_ = {};

    build_interior(geometry, rule);
    if(left_edge != EdgeMode::Background) {
      build_boundary(geometry, rule, left_edge, Side::Left);
    }
    if(right_edge != EdgeMode::Background) {
      build_boundary(geometry, rule, right_edge, Side::Right);
    }
  }

  // Coprime orthogonal geometries compile to one five-row projection.  Keep
  // that overwhelmingly common search path compact: the generic loop used to
  // leave a division, bounds check, and two dynamic loops at every visited
  // pair state.  Keep this out of line because the pair DFS has many callback
  // specializations and duplicating the lookup kernel bloats its hot code.
#if defined(__GNUC__) || defined(__clang__)
  [[gnu::noinline]]
#endif
  [[nodiscard]] inline std::uint8_t interior_mask(const std::uint8_t* triples, std::size_t row) const {
    if(row < short_window_) {
      return 0xffU;
    }
    if(fast_interior_masks_ != nullptr) {
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if(fast_p7_gather_mask_ != 0)
        return fast_p7_one_projection_mask(triples, row);
#endif
      return fast_one_projection_mask(triples, row);
    }
    if(fast_three_projection_two_phase_) {
      return fast_three_projection_mask(triples, row);
    }
    return generic_interior_mask(triples, row);
  }

  // Indexed DFS may retain the last 21 triples in a 63-bit shift register,
  // newest triple in bits 0..2. A zero mask selects the byte-history fallback.
  // The additional 32 KiB table uses PEXT order instead of the scalar order.
  [[nodiscard]] std::uint64_t packed_history_mask() const noexcept { return packed_history_mask_; }
  [[nodiscard]] const std::uint8_t* packed_history_table() const noexcept { return packed_history_table_.data(); }

#if defined(RLIFE_GEOMETRY_ACCEPTANCE_TESTING)
  [[nodiscard]] bool debug_p7_gather_enabled() const noexcept {
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return fast_p7_gather_mask_ != 0;
#else
    return false;
#endif
  }

  [[nodiscard]] std::pair<std::uint8_t, std::uint64_t> debug_p7_gather_parameters() const noexcept {
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return {fast_p7_block_offset_, fast_p7_gather_mask_};
#else
    return {};
#endif
  }

  [[nodiscard]] std::uint8_t debug_scalar_one_projection_mask(const std::uint8_t* triples, std::size_t row) const noexcept {
    assert(fast_interior_masks_ != nullptr);
    return fast_one_projection_mask(triples, row);
  }

#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  [[nodiscard]] std::uint8_t debug_p7_one_projection_mask(const std::uint8_t* triples, std::size_t row) const noexcept {
    assert(fast_p7_gather_mask_ != 0);
    return fast_p7_one_projection_mask(triples, row);
  }
#endif
#endif

private:
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#if defined(__GNUC__) || defined(__clang__)
  [[gnu::noinline]]
#endif
  [[nodiscard]] std::uint8_t fast_p7_one_projection_mask(const std::uint8_t* triples, std::size_t row) const noexcept {
    // With P=7 and 1<=K<=3, the canonical five-triple key is
    //   a=row-14, b=row-(7+K), c=row-7, d=row-K, e=row-(7-K).
    // A qword beginning at b contains b/c/e/d at byte offsets 0/K/2K/7.
    std::uint64_t block = 0;
    std::memcpy(&block, triples + row - fast_p7_block_offset_, sizeof(block));
    const auto raw = _pext_u64(block, fast_p7_gather_mask_);
    const auto key = static_cast<std::size_t>(triples[row - 14U]) |
                     (static_cast<std::size_t>(raw & 0x03fU) << 3U) |
                     static_cast<std::size_t>(raw & 0xe00U) |
                     (static_cast<std::size_t>(raw & 0x1c0U) << 6U);
    return fast_interior_masks_[key];
  }
#endif

#if defined(__GNUC__) || defined(__clang__)
  [[gnu::noinline]]
#endif
  [[nodiscard]] std::uint8_t fast_one_projection_mask(const std::uint8_t* triples, std::size_t row) const noexcept {
    const auto key = static_cast<std::size_t>(triples[row - fast_interior_offsets_[0]]) |
                     (static_cast<std::size_t>(triples[row - fast_interior_offsets_[1]]) << 3U) |
                     (static_cast<std::size_t>(triples[row - fast_interior_offsets_[2]]) << 6U) |
                     (static_cast<std::size_t>(triples[row - fast_interior_offsets_[3]]) << 9U) |
                     (static_cast<std::size_t>(triples[row - fast_interior_offsets_[4]]) << 12U);
    return fast_interior_masks_[key];
  }

#if defined(__GNUC__) || defined(__clang__)
  [[gnu::noinline]]
#endif
  [[nodiscard]] std::uint8_t fast_three_projection_mask(const std::uint8_t* triples, std::size_t row) const noexcept {
    const auto& projection = fast_three_projection_[row & 1U];
    const auto* offsets = projection.offsets.data();
    const auto a = static_cast<std::size_t>(triples[row - offsets[0]]);
    const auto b = static_cast<std::size_t>(triples[row - offsets[1]]);
    const auto c = static_cast<std::size_t>(triples[row - offsets[2]]);
    const auto d = static_cast<std::size_t>(triples[row - offsets[3]]);
    const auto e = static_cast<std::size_t>(triples[row - offsets[4]]);
    const auto f = static_cast<std::size_t>(triples[row - offsets[5]]);
    const auto g = static_cast<std::size_t>(triples[row - offsets[6]]);
    const auto h = static_cast<std::size_t>(triples[row - offsets[7]]);
    const auto i = static_cast<std::size_t>(triples[row - offsets[8]]);
    const auto first_key = a | (b << 3U) | (c << 6U) | (d << 9U) | (e << 12U);
    const auto second_key = f | (a << 3U) | (c << 6U) | (d << 9U);
    const auto third_key = g | (h << 3U) | (i << 6U);
    return static_cast<std::uint8_t>(projection.masks[0][first_key] & projection.masks[1][second_key] & projection.masks[2][third_key]);
  }

#if defined(__GNUC__) || defined(__clang__)
  [[gnu::noinline]]
#endif
  [[nodiscard]] std::uint8_t generic_interior_mask(const std::uint8_t* triples, std::size_t row) const {
    std::uint8_t accepted = 0xffU;
    const auto phase = row % subtile_count_;
    for(const auto& projection : interior_acceptance_.at(phase)) {
      std::size_t history = 0;
      for(std::size_t index = 0; index < projection.history_offsets.size(); ++index) {
        history |= static_cast<std::size_t>(triples[row - projection.history_offsets[index]] & 0b111U) << (3U * index);
      }
      accepted = static_cast<std::uint8_t>(accepted & projection.candidate_masks[history]);
    }
    return accepted;
  }

public:

  [[nodiscard]] BoundaryStep boundary_step(const std::uint8_t* pairs,
                                           std::size_t row,
                                           EdgeMode mode,
                                           Side side,
                                           std::uint8_t raw_label) const {
    if(mode == EdgeMode::Background) {
      return {raw_label == 0, raw_label};
    }
    const auto mode_index = edge_index(mode);
    const auto side_index = side == Side::Left ? 0U : 1U;
    if(!boundary_built_[mode_index][side_index]) {
      throw std::logic_error("requested edge acceptance table was not built");
    }
    if(row < short_window_) {
      return {true, raw_label};
    }

    std::uint8_t accepted = 0b1111U;
    const auto phase = row % subtile_count_;
    for(const auto& projection : boundary_acceptance_[mode_index][side_index].at(phase)) {
      std::size_t history = 0;
      for(std::size_t index = 0; index < projection.history_bits.size(); ++index) {
        const auto read = projection.history_bits[index];
        const auto pair = pairs[row - read.offset] & 0b11U;
        const auto state = read.column == 0 ? ((pair >> 1U) & 1U) != 0 : (pair & 1U) != 0;
        history |= static_cast<std::size_t>(state) << index;
      }
      accepted = static_cast<std::uint8_t>(accepted & projection.candidate_masks[history]);
    }
    return {(accepted & (1U << raw_label)) != 0, raw_label};
  }

  [[nodiscard]] std::size_t storage_bytes() const noexcept {
    std::size_t result = packed_history_table_.capacity();
    for(const auto& phase : interior_acceptance_) {
      for(const auto& projection : phase) {
        result += projection.storage_bytes();
      }
    }
    for(const auto& mode : boundary_acceptance_) {
      for(const auto& side : mode) {
        for(const auto& phase : side) {
          for(const auto& projection : phase) {
            result += projection.storage_bytes();
          }
        }
      }
    }
    return result;
  }

private:
  static constexpr std::size_t edge_mode_count_ = 5;

  struct Read {
    enum class Kind : std::uint8_t { Unknown, Candidate, History };

    Kind kind = Kind::Unknown;
    std::size_t offset = 0;
    std::uint8_t column = 0;

    bool operator==(const Read&) const = default;
    bool operator<(const Read& other) const noexcept { return std::tie(kind, offset, column) < std::tie(other.kind, other.offset, other.column); }
  };

  using Equation = std::array<Read, 10>;

  struct InteriorProjection {
    std::vector<std::size_t> history_offsets;
    std::vector<Equation> equations;
    std::vector<std::uint8_t> candidate_masks;

    [[nodiscard]] std::size_t storage_bytes() const noexcept {
      return history_offsets.capacity() * sizeof(history_offsets[0]) + equations.capacity() * sizeof(equations[0]) +
             candidate_masks.capacity() * sizeof(candidate_masks[0]);
    }
  };

  struct BoundaryCondition {
    enum class Kind : std::uint8_t { Equation, Equality };

    Kind kind = Kind::Equation;
    Equation equation{};
    std::array<Read, 2> equality{};
  };

  struct BoundaryProjection {
    std::vector<Read> history_bits;
    std::vector<BoundaryCondition> conditions;
    std::vector<std::uint8_t> candidate_masks;

    [[nodiscard]] std::size_t storage_bytes() const noexcept {
      return history_bits.capacity() * sizeof(history_bits[0]) + conditions.capacity() * sizeof(conditions[0]) +
             candidate_masks.capacity() * sizeof(candidate_masks[0]);
    }
  };

  static constexpr std::array<std::array<int, 3>, 10> ca_offsets_ = {
      std::array{-1, -1, 0}, std::array{0, -1, 0}, std::array{1, -1, 0}, std::array{-1, 0, 0}, std::array{0, 0, 0},
      std::array{1, 0, 0},   std::array{-1, 1, 0}, std::array{0, 1, 0},  std::array{1, 1, 0},  std::array{0, 0, 1},
  };

  static constexpr std::size_t edge_index(EdgeMode mode) noexcept { return static_cast<std::size_t>(mode); }

  void build_interior(const Geometry& geometry, const RuleTable& rule) {
    constexpr std::size_t maximum_projection_history_rows = 5;
    const auto lookback = geometry.short_window();
    interior_acceptance_.clear();
    interior_acceptance_.resize(static_cast<std::size_t>(geometry.subtile_count));

    for(std::size_t phase = 0; phase < interior_acceptance_.size(); ++phase) {
      std::map<Geometry::Coordinate, Read> board;
      for(std::uint8_t column = 0; column < 3; ++column) {
        board.emplace(geometry.cell(column, static_cast<std::int64_t>(phase)), Read{Read::Kind::Candidate, 0, column});
        for(std::size_t offset = 1; offset <= lookback; ++offset) {
          const auto inserted = board.emplace(
              geometry.cell(column, static_cast<std::int64_t>(phase) - static_cast<std::int64_t>(offset)),
              Read{Read::Kind::History, offset, column});
          if(!inserted.second) {
            throw std::logic_error("geometry history board aliases two lineage bits");
          }
        }
      }

      std::set<Geometry::Coordinate> centers;
      for(std::uint8_t column = 0; column < 3; ++column) {
        const auto candidate = geometry.cell(column, static_cast<std::int64_t>(phase));
        for(const auto& delta : ca_offsets_) {
          centers.insert(geometry.shift(candidate, -delta[0], -delta[1], -delta[2]));
        }
      }

      std::vector<Equation> equations;
      for(const auto center : centers) {
        Equation equation;
        bool touches_candidate = false;
        for(std::size_t bit = 0; bit < ca_offsets_.size(); ++bit) {
          const auto& delta = ca_offsets_[bit];
          const auto found = board.find(geometry.shift(center, delta[0], delta[1], delta[2]));
          if(found != board.end()) {
            equation[bit] = found->second;
            touches_candidate = touches_candidate || found->second.kind == Read::Kind::Candidate;
          }
        }
        if(touches_candidate) {
          equations.push_back(equation);
        }
      }

      auto& projections = interior_acceptance_[phase];
      for(const auto& equation : equations) {
        std::set<std::size_t> equation_offsets;
        for(const auto read : equation) {
          if(read.kind == Read::Kind::History) {
            equation_offsets.insert(read.offset);
          }
        }

        InteriorProjection* destination = nullptr;
        for(auto& projection : projections) {
          auto combined = std::set<std::size_t>(projection.history_offsets.begin(), projection.history_offsets.end());
          combined.insert(equation_offsets.begin(), equation_offsets.end());
          if(combined.size() <= maximum_projection_history_rows) {
            projection.history_offsets.assign(combined.begin(), combined.end());
            destination = &projection;
            break;
          }
        }
        if(destination == nullptr) {
          projections.emplace_back();
          projections.back().history_offsets.assign(equation_offsets.begin(), equation_offsets.end());
          destination = &projections.back();
        }
        destination->equations.push_back(equation);
      }

      for(auto& projection : projections) {
        const auto history_bits = 3U * projection.history_offsets.size();
        const auto history_patterns = std::size_t{1} << history_bits;
        projection.candidate_masks.resize(history_patterns, 0);
        for(std::size_t history = 0; history < history_patterns; ++history) {
          for(std::size_t position = 0; position < geometry_pair_triple_order.size(); ++position) {
            const auto candidate = geometry_pair_triple_order[position];
            bool accepted = true;
            for(const auto& equation : projection.equations) {
              std::uint16_t known = 0;
              std::uint16_t value = 0;
              for(std::size_t bit_index = 0; bit_index < equation.size(); ++bit_index) {
                const auto read = equation[bit_index];
                if(read.kind == Read::Kind::Unknown) {
                  continue;
                }
                bool state = false;
                if(read.kind == Read::Kind::Candidate) {
                  state = ((candidate >> read.column) & 1U) != 0;
                } else {
                  const auto found = std::lower_bound(projection.history_offsets.begin(), projection.history_offsets.end(), read.offset);
                  if(found == projection.history_offsets.end() || *found != read.offset) {
                    throw std::logic_error("geometry projection omitted a required history row");
                  }
                  const auto history_index = static_cast<std::size_t>(found - projection.history_offsets.begin());
                  state = ((history >> (3U * history_index + read.column)) & 1U) != 0;
                }
                const auto bit = static_cast<std::uint16_t>(1U << bit_index);
                known = static_cast<std::uint16_t>(known | bit);
                if(state) {
                  value = static_cast<std::uint16_t>(value | bit);
                }
              }
              if(!rule.accepts_partial(known, value)) {
                accepted = false;
                break;
              }
            }
            if(accepted) {
              projection.candidate_masks[history] =
                  static_cast<std::uint8_t>(projection.candidate_masks[history] | (1U << position));
            }
          }
        }
      }
    }

    fast_interior_masks_ = nullptr;
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    fast_p7_gather_mask_ = 0;
    fast_p7_block_offset_ = 0;
#endif
    fast_three_projection_two_phase_ = false;
    if(interior_acceptance_.size() == 1U && interior_acceptance_.front().size() == 1U) {
      auto& projection = interior_acceptance_.front().front();
      if(projection.history_offsets.size() == fast_interior_offsets_.size() && projection.candidate_masks.size() == (1U << 15U)) {
        const auto period = static_cast<std::size_t>(geometry.period);
        const auto displacement = static_cast<std::size_t>(geometry.displacement);
        const std::array<std::size_t, 5> locality_order = {
            2U * period, period + displacement, period, displacement, period - displacement,
        };
        if(std::is_permutation(projection.history_offsets.begin(), projection.history_offsets.end(), locality_order.begin(), locality_order.end())) {
          std::vector<std::uint8_t> reordered(projection.candidate_masks.size());
          for(std::size_t key = 0; key < reordered.size(); ++key) {
            std::size_t original_key = 0;
            for(std::size_t index = 0; index < locality_order.size(); ++index) {
              const auto original = std::find(projection.history_offsets.begin(), projection.history_offsets.end(), locality_order[index]);
              const auto original_index = static_cast<std::size_t>(original - projection.history_offsets.begin());
              original_key |= ((key >> (3U * index)) & 0b111U) << (3U * original_index);
            }
            reordered[key] = projection.candidate_masks[original_key];
          }
          projection.candidate_masks.swap(reordered);
          fast_interior_offsets_ = locality_order;
        } else {
          std::copy(projection.history_offsets.begin(), projection.history_offsets.end(), fast_interior_offsets_.begin());
        }
        fast_interior_masks_ = projection.candidate_masks.data();
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if(period == 7U && displacement >= 1U && displacement <= 3U) {
          const std::array<std::size_t, 5> p7_offsets = {
              14U, 7U + displacement, 7U, displacement, 7U - displacement,
          };
          if(fast_interior_offsets_ == p7_offsets) {
            fast_p7_block_offset_ = static_cast<std::uint8_t>(7U + displacement);
            fast_p7_gather_mask_ = std::uint64_t{0x07U} |
                                   (std::uint64_t{0x07U} << (8U * displacement)) |
                                   (std::uint64_t{0x07U} << (16U * displacement)) |
                                   (std::uint64_t{0x07U} << 56U);
          }
        }
#endif
      }
    }

    packed_history_mask_ = 0;
    packed_history_table_.clear();
#if defined(__BMI2__)
    if(fast_interior_masks_ != nullptr &&
       *std::max_element(fast_interior_offsets_.begin(), fast_interior_offsets_.end()) <= 21U &&
       *std::min_element(fast_interior_offsets_.begin(), fast_interior_offsets_.end()) > 0U) {
      auto ordered = fast_interior_offsets_;
      std::sort(ordered.begin(), ordered.end());
      for(const auto offset : ordered)
        packed_history_mask_ |= std::uint64_t{7} << (3U * (offset - 1U));
      packed_history_table_.resize(1U << 15U);
      for(std::size_t key = 0; key < packed_history_table_.size(); ++key) {
        std::size_t original = 0;
        for(std::size_t i = 0; i < fast_interior_offsets_.size(); ++i) {
          const auto index = std::find(ordered.begin(), ordered.end(), fast_interior_offsets_[i]) - ordered.begin();
          original |= ((key >> (3U * index)) & 7U) << (3U * i);
        }
        packed_history_table_[key] = fast_interior_masks_[original];
      }
    }
#endif

    if(interior_acceptance_.size() == 2U) {
      bool compatible = true;
      for(std::size_t phase = 0; phase < 2U; ++phase) {
        const auto& projections = interior_acceptance_[phase];
        if(projections.size() != 3U || projections[0].history_offsets.size() != 5U || projections[0].candidate_masks.size() != (1U << 15U) ||
           projections[1].history_offsets.size() != 4U || projections[1].candidate_masks.size() != (1U << 12U) ||
           projections[2].history_offsets.size() != 3U || projections[2].candidate_masks.size() != (1U << 9U) ||
           projections[1].history_offsets[1] != projections[0].history_offsets[0] ||
           projections[1].history_offsets[2] != projections[0].history_offsets[2] ||
           projections[1].history_offsets[3] != projections[0].history_offsets[3]) {
          compatible = false;
          break;
        }
        auto& fast = fast_three_projection_[phase];
        for(std::size_t index = 0; index < 5U; ++index)
          fast.offsets[index] = projections[0].history_offsets[index];
        fast.offsets[5] = projections[1].history_offsets[0];
        for(std::size_t index = 0; index < 3U; ++index)
          fast.offsets[6U + index] = projections[2].history_offsets[index];
        for(std::size_t index = 0; index < 3U; ++index)
          fast.masks[index] = projections[index].candidate_masks.data();
      }
      fast_three_projection_two_phase_ = compatible;
    }
  }

  void build_boundary(const Geometry& geometry, const RuleTable& rule, EdgeMode mode, Side side) {
    constexpr std::size_t maximum_projection_history_bits = 10;
    const auto mode_index = edge_index(mode);
    const auto side_index = side == Side::Left ? 0U : 1U;
    auto& phases = boundary_acceptance_[mode_index][side_index];
    phases.clear();
    phases.resize(static_cast<std::size_t>(geometry.subtile_count));

    for(std::size_t phase = 0; phase < phases.size(); ++phase) {
      const auto lookback = geometry.short_window() + phase;
      std::map<Geometry::Coordinate, std::vector<Read>> board;
      std::vector<BoundaryCondition> conditions;
      std::set<Geometry::Coordinate> candidate_cells;

      auto insert_read = [&](Geometry::Coordinate coordinate, Read read) {
        auto& reads = board[coordinate];
        if(std::find(reads.begin(), reads.end(), read) == reads.end()) {
          reads.push_back(read);
        }
      };
      auto insert_read_and_reflection = [&](Geometry::Coordinate coordinate, Read read) {
        insert_read(coordinate, read);
        insert_read(geometry.reflect(coordinate, mode, side), read);
        if(read.kind == Read::Kind::Candidate) {
          candidate_cells.insert(coordinate);
          candidate_cells.insert(geometry.reflect(coordinate, mode, side));
        }
      };

      // Preserve vanilla's symbolic-read order.  In particular, an aliased
      // history coordinate supplies a CA equation before a candidate equality
      // is imposed.
      for(std::uint8_t column = 0; column < 2; ++column) {
        for(std::size_t offset = 1; offset <= lookback; ++offset) {
          insert_read_and_reflection(
              geometry.cell(column, static_cast<std::int64_t>(phase) - static_cast<std::int64_t>(offset)),
              Read{Read::Kind::History, offset, column});
        }
        insert_read_and_reflection(geometry.cell(column, static_cast<std::int64_t>(phase)), Read{Read::Kind::Candidate, 0, column});
      }

      for(const auto& [coordinate, reads] : board) {
        (void)coordinate;
        for(std::size_t index = 1; index < reads.size(); ++index) {
          if(reads[0].kind != Read::Kind::Candidate && reads[index].kind != Read::Kind::Candidate) {
            continue;
          }
          BoundaryCondition equality;
          equality.kind = BoundaryCondition::Kind::Equality;
          equality.equality = {reads[0], reads[index]};
          conditions.push_back(equality);
        }
      }

      std::set<Geometry::Coordinate> centers;
      for(const auto candidate : candidate_cells) {
        for(const auto& delta : ca_offsets_) {
          centers.insert(geometry.shift(candidate, -delta[0], -delta[1], -delta[2]));
        }
      }
      for(const auto center : centers) {
        BoundaryCondition condition;
        condition.kind = BoundaryCondition::Kind::Equation;
        bool has_known_read = false;
        for(std::size_t bit = 0; bit < ca_offsets_.size(); ++bit) {
          const auto& delta = ca_offsets_[bit];
          const auto found = board.find(geometry.shift(center, delta[0], delta[1], delta[2]));
          if(found != board.end()) {
            condition.equation[bit] = found->second.front();
            has_known_read = true;
          }
        }
        if(has_known_read) {
          conditions.push_back(condition);
        }
      }

      auto condition_history_bits = [](const BoundaryCondition& condition) {
        std::set<Read> bits;
        auto add = [&](Read read) {
          if(read.kind == Read::Kind::History) {
            bits.insert(read);
          }
        };
        if(condition.kind == BoundaryCondition::Kind::Equation) {
          for(const auto read : condition.equation) {
            add(read);
          }
        } else {
          add(condition.equality[0]);
          add(condition.equality[1]);
        }
        return bits;
      };

      auto& projections = phases[phase];
      for(const auto& condition : conditions) {
        const auto history_bits = condition_history_bits(condition);
        if(history_bits.size() > maximum_projection_history_bits) {
          throw std::logic_error("a geometry edge condition exceeds the projection bit budget");
        }
        BoundaryProjection* destination = nullptr;
        for(auto& projection : projections) {
          auto combined = std::set<Read>(projection.history_bits.begin(), projection.history_bits.end());
          combined.insert(history_bits.begin(), history_bits.end());
          if(combined.size() <= maximum_projection_history_bits) {
            projection.history_bits.assign(combined.begin(), combined.end());
            destination = &projection;
            break;
          }
        }
        if(destination == nullptr) {
          projections.emplace_back();
          projections.back().history_bits.assign(history_bits.begin(), history_bits.end());
          destination = &projections.back();
        }
        destination->conditions.push_back(condition);
      }

      for(auto& projection : projections) {
        const auto history_patterns = std::size_t{1} << projection.history_bits.size();
        projection.candidate_masks.resize(history_patterns, 0);
        for(std::size_t history = 0; history < history_patterns; ++history) {
          for(std::uint8_t candidate = 0; candidate < 4; ++candidate) {
            auto read_state = [&](Read read) {
              if(read.kind == Read::Kind::History) {
                const auto found = std::lower_bound(projection.history_bits.begin(), projection.history_bits.end(), read);
                if(found == projection.history_bits.end() || *found != read) {
                  throw std::logic_error("geometry edge projection omitted a required history bit");
                }
                return ((history >> static_cast<std::size_t>(found - projection.history_bits.begin())) & 1U) != 0;
              }
              return read.column == 0 ? ((candidate >> 1U) & 1U) != 0 : (candidate & 1U) != 0;
            };

            bool accepted = true;
            for(const auto& condition : projection.conditions) {
              if(condition.kind == BoundaryCondition::Kind::Equality) {
                if(read_state(condition.equality[0]) != read_state(condition.equality[1])) {
                  accepted = false;
                  break;
                }
                continue;
              }

              std::uint16_t known = 0;
              std::uint16_t value = 0;
              for(std::size_t bit_index = 0; bit_index < condition.equation.size(); ++bit_index) {
                const auto read = condition.equation[bit_index];
                if(read.kind == Read::Kind::Unknown) {
                  continue;
                }
                const auto bit = static_cast<std::uint16_t>(1U << bit_index);
                known = static_cast<std::uint16_t>(known | bit);
                if(read_state(read)) {
                  value = static_cast<std::uint16_t>(value | bit);
                }
              }
              if(!rule.accepts_partial(known, value)) {
                accepted = false;
                break;
              }
            }
            if(accepted) {
              projection.candidate_masks[history] =
                  static_cast<std::uint8_t>(projection.candidate_masks[history] | (1U << candidate));
            }
          }
        }
      }
    }
    boundary_built_[mode_index][side_index] = true;
  }

  std::size_t short_window_ = 0;
  std::size_t subtile_count_ = 1;
  std::array<std::size_t, 5> fast_interior_offsets_{};
  const std::uint8_t* fast_interior_masks_ = nullptr;
  std::uint64_t packed_history_mask_ = 0;
  std::vector<std::uint8_t> packed_history_table_;
#if RLIFE_ENABLE_P7_GATHER && defined(__BMI2__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  std::uint64_t fast_p7_gather_mask_ = 0;
  std::uint8_t fast_p7_block_offset_ = 0;
#endif
  struct FastThreeProjection {
    std::array<std::size_t, 9> offsets{};
    std::array<const std::uint8_t*, 3> masks{};
  };
  std::array<FastThreeProjection, 2> fast_three_projection_{};
  bool fast_three_projection_two_phase_ = false;
  std::vector<std::vector<InteriorProjection>> interior_acceptance_;
  std::array<std::array<std::vector<std::vector<BoundaryProjection>>, 2>, edge_mode_count_> boundary_acceptance_{};
  std::array<std::array<bool, 2>, edge_mode_count_> boundary_built_{};
};

} // namespace rlife::llsss
