#pragma once

#include "geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rlife::llsss {

using Board = std::vector<std::vector<std::uint8_t>>;

// Convert flattened quotient-lattice rows back into physical time phases.
// Reflection happens before physical conversion so ordinary and glide modes,
// including two reflected edges, share exactly the boundary transform.
inline Board render_phase_montage(const Geometry& geometry,
                                  const Board& row_sequence,
                                  EdgeMode left_edge,
                                  EdgeMode right_edge) {
  constexpr std::size_t phase_spacing = 16;
  if(row_sequence.empty() || row_sequence.front().empty()) {
    throw std::logic_error("cannot render an empty lattice board");
  }

  struct LatticeCell {
    Geometry::Coordinate coordinate;
    bool live = false;
  };
  struct PhaseCell {
    std::int64_t x = 0;
    std::int64_t y = 0;
    bool live = false;
  };
  struct PhaseImage {
    std::vector<PhaseCell> cells;
    std::int64_t min_x = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_x = std::numeric_limits<std::int64_t>::min();
    std::int64_t min_y = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_y = std::numeric_limits<std::int64_t>::min();
  };

  auto floor_div = [](std::int64_t numerator, std::int64_t denominator) {
    auto quotient = numerator / denominator;
    if(numerator % denominator < 0) {
      --quotient;
    }
    return quotient;
  };

  std::vector<LatticeCell> lattice_cells;
  lattice_cells.reserve(row_sequence.size() * row_sequence.front().size());
  for(std::size_t depth = 0; depth < row_sequence.size(); ++depth) {
    if(row_sequence[depth].size() != row_sequence.front().size()) {
      throw std::logic_error("lattice board rows have inconsistent widths");
    }
    for(std::size_t logical_u = 0; logical_u < row_sequence[depth].size(); ++logical_u) {
      lattice_cells.push_back(
          {geometry.cell(static_cast<std::int64_t>(logical_u), static_cast<std::int64_t>(depth)), row_sequence[depth][logical_u] != 0});
    }
  }

  auto add_reflection = [&](EdgeMode mode, Side side, std::int64_t logical_u_origin) {
    if(mode == EdgeMode::Background) {
      return;
    }
    const auto original_size = lattice_cells.size();
    lattice_cells.reserve(2U * original_size);
    for(std::size_t index = 0; index < original_size; ++index) {
      lattice_cells.push_back(
          {geometry.reflect(lattice_cells[index].coordinate, mode, side, logical_u_origin), lattice_cells[index].live});
    }
  };
  add_reflection(left_edge, Side::Left, 0);
  add_reflection(right_edge, Side::Right, static_cast<std::int64_t>(row_sequence.front().size()) - 2);

  std::vector<PhaseImage> phases(static_cast<std::size_t>(geometry.period));
  for(const auto& lattice_cell : lattice_cells) {
    auto physical = geometry.to_physical(lattice_cell.coordinate);
    const auto quotient = floor_div(physical.t, geometry.period);
    if(geometry.diagonal()) {
      physical.x += quotient * geometry.displacement;
    }
    physical.y += quotient * geometry.displacement;
    physical.t -= quotient * geometry.period;

    auto& phase = phases.at(static_cast<std::size_t>(physical.t));
    phase.cells.push_back({physical.x, physical.y, lattice_cell.live});
    phase.min_x = std::min(phase.min_x, physical.x);
    phase.max_x = std::max(phase.max_x, physical.x);
    phase.min_y = std::min(phase.min_y, physical.y);
    phase.max_y = std::max(phase.max_y, physical.y);
  }

  std::size_t montage_width = phase_spacing * (phases.size() - 1U);
  std::size_t montage_height = 0;
  std::vector<std::size_t> phase_x(phases.size());
  for(const auto& phase : phases) {
    if(phase.cells.empty()) {
      throw std::logic_error("physical phase montage has an empty time phase");
    }
    montage_width += static_cast<std::size_t>(phase.max_x - phase.min_x + 1);
    montage_height = std::max(montage_height, static_cast<std::size_t>(phase.max_y - phase.min_y + 1));
  }

  std::size_t next_x = 0;
  for(std::size_t index = 0; index < phases.size(); ++index) {
    phase_x[index] = next_x;
    next_x += static_cast<std::size_t>(phases[index].max_x - phases[index].min_x + 1);
    if(index + 1U < phases.size()) {
      next_x += phase_spacing;
    }
  }
  if(next_x != montage_width) {
    throw std::logic_error("physical phase montage width accounting failed");
  }

  Board montage(montage_height, std::vector<std::uint8_t>(montage_width, 0));
  for(std::size_t index = 0; index < phases.size(); ++index) {
    const auto& phase = phases[index];
    for(const auto cell : phase.cells) {
      if(cell.live) {
        montage[static_cast<std::size_t>(cell.y - phase.min_y)]
               [phase_x[index] + static_cast<std::size_t>(cell.x - phase.min_x)] = 1;
      }
    }
  }
  return montage;
}

} // namespace rlife::llsss
