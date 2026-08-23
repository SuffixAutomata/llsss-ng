#pragma once

#include "geometry.hpp"
#include "rule.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

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

  [[nodiscard]] std::uint8_t interior_mask(const std::uint8_t* triples, std::size_t row) const {
    if(row < short_window_) {
      return 0xffU;
    }
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
    std::size_t result = 0;
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
  std::vector<std::vector<InteriorProjection>> interior_acceptance_;
  std::array<std::array<std::vector<std::vector<BoundaryProjection>>, 2>, edge_mode_count_> boundary_acceptance_{};
  std::array<std::array<bool, 2>, edge_mode_count_> boundary_built_{};
};

} // namespace rlife::llsss
