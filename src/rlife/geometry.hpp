#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rlife::llsss {

// Keep these values stable: checkpoints store EdgeMode as a byte.
enum class EdgeMode : std::uint8_t {
  Background = 0,
  Odd = 1,
  Even = 2,
  GlideEven = 3,
  GlideOdd = 4,
};

enum class Side : std::uint8_t { Left, Right };

inline std::string_view edge_name(EdgeMode mode) {
  switch(mode) {
  case EdgeMode::Background:
    return "bg";
  case EdgeMode::Odd:
    return "odd";
  case EdgeMode::Even:
    return "even";
  case EdgeMode::GlideEven:
    return "gse";
  case EdgeMode::GlideOdd:
    return "gso";
  }
  return "?";
}

inline EdgeMode parse_edge(const std::string& text) {
  if(text == "bg" || text == "background" || text == "asymmetric") {
    return EdgeMode::Background;
  }
  if(text == "odd") {
    return EdgeMode::Odd;
  }
  if(text == "even") {
    return EdgeMode::Even;
  }
  if(text == "gse" || text == "glide-even" || text == "glide_even") {
    return EdgeMode::GlideEven;
  }
  if(text == "gso" || text == "glide-odd" || text == "glide_odd") {
    return EdgeMode::GlideOdd;
  }
  throw std::runtime_error("unsupported edge: " + text + " (use bg, odd, even, gse, or gso)");
}

struct Geometry {
  enum class Lattice : std::uint8_t { Orthogonal, Diagonal };

  struct Coordinate {
    // Coordinates are scaled by subtile_count.  V is kept modulo that
    // determinant, while U and W retain the surrounding logical tiles.
    std::int64_t u = 0;
    std::int64_t v = 0;
    std::int64_t w = 0;

    bool operator==(const Coordinate&) const = default;
    bool operator<(const Coordinate& other) const noexcept { return std::tie(w, v, u) < std::tie(other.w, other.v, other.u); }
  };

  struct PhysicalCoordinate {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t t = 0;

    bool operator==(const PhysicalCoordinate&) const = default;
  };

  int displacement = 1;
  int period = 0;
  int gcd = 1;
  int subtile_count = 1;
  std::int64_t w_space = 0;
  std::int64_t w_time = 1;
  Lattice lattice = Lattice::Orthogonal;
  std::vector<Coordinate> spots;
  std::string source;

  static Geometry parse(const std::string& text) {
    static const std::regex pattern(R"(^([0-9]*)c([0-9]+)(d?)-f2b$)");
    std::smatch match;
    if(!std::regex_match(text, match, pattern)) {
      throw std::runtime_error("geometry must look like c4-f2b, 2c5-f2b, or c5d-f2b");
    }

    Geometry result;
    result.source = text;
    result.displacement = match[1].str().empty() ? 1 : std::stoi(match[1].str());
    result.period = std::stoi(match[2].str());
    result.lattice = match[3].str().empty() ? Lattice::Orthogonal : Lattice::Diagonal;
    if(result.period <= 0 || result.displacement < 0 || result.displacement > result.period) {
      throw std::runtime_error("geometry requires 0 <= displacement <= period and period > 0");
    }

    result.gcd = std::gcd(result.displacement, result.period);
    const auto determinant_multiplier = result.diagonal() ? 2LL : 1LL;
    const auto subtiles = determinant_multiplier * static_cast<std::int64_t>(result.gcd);
    if(subtiles > std::numeric_limits<int>::max()) {
      throw std::runtime_error("geometry has too many lattice subtiles");
    }
    result.subtile_count = static_cast<int>(subtiles);

    const auto [common, time, space] = extended_gcd(result.displacement, result.period);
    if(common != result.gcd) {
      throw std::logic_error("geometry Bezout construction failed");
    }
    result.w_time = time;
    result.w_space = space;
    result.build_spots();
    return result;
  }

  [[nodiscard]] bool diagonal() const noexcept { return lattice == Lattice::Diagonal; }

  [[nodiscard]] std::size_t short_window() const noexcept {
    return static_cast<std::size_t>(diagonal() ? 4 : 2) * static_cast<std::size_t>(period);
  }

  [[nodiscard]] std::size_t long_window() const noexcept { return short_window() + static_cast<std::size_t>(subtile_count); }

  [[nodiscard]] bool complete_tile(std::size_t depth) const noexcept { return depth % static_cast<std::size_t>(subtile_count) == 0; }

  [[nodiscard]] std::size_t w_position(std::size_t depth) const noexcept { return depth / static_cast<std::size_t>(subtile_count); }

  [[nodiscard]] std::size_t subtile_position(std::size_t depth) const noexcept { return depth % static_cast<std::size_t>(subtile_count); }

  [[nodiscard]] std::string position_string(std::size_t depth) const {
    if(subtile_count == 1) {
      return std::to_string(depth);
    }
    return std::to_string(w_position(depth)) + "[" + std::to_string(subtile_position(depth)) + "]";
  }

  // Translate a quotient-lattice coordinate by a physical cell offset.  V
  // is canonicalized modulo the determinant because V itself is a search
  // period and therefore names the same quotient cell.
  [[nodiscard]] Coordinate shift(Coordinate coordinate, int dx, int dy, int dt) const {
    if(diagonal()) {
      coordinate.u += static_cast<std::int64_t>(gcd) * (dx - dy);
      coordinate.v += 2 * w_space * dt - w_time * (dx + dy);
      coordinate.w += static_cast<std::int64_t>(period) * (dx + dy) + 2LL * displacement * dt;
    } else {
      coordinate.u += static_cast<std::int64_t>(subtile_count) * dx;
      coordinate.v += w_space * dt - w_time * dy;
      coordinate.w += static_cast<std::int64_t>(period) * dy + static_cast<std::int64_t>(displacement) * dt;
    }
    coordinate.v = floor_mod(coordinate.v, subtile_count);
    return coordinate;
  }

  // Return the cell in logical U column `logical_u` at one flattened W
  // depth.  This is the common operation for both quotient lattices.
  [[nodiscard]] Coordinate cell(std::int64_t logical_u, std::int64_t flattened_depth) const {
    const auto tile = floor_div(flattened_depth, subtile_count);
    const auto phase = static_cast<std::size_t>(floor_mod(flattened_depth, subtile_count));
    auto coordinate = spots.at(phase);
    coordinate.u += static_cast<std::int64_t>(subtile_count) * logical_u;
    coordinate.w += static_cast<std::int64_t>(subtile_count) * tile;
    return coordinate;
  }

  [[nodiscard]] PhysicalCoordinate to_physical(Coordinate coordinate) const {
    const auto scale = static_cast<std::int64_t>(subtile_count);
    std::int64_t x_numerator = coordinate.u;
    std::int64_t y_numerator = 0;
    std::int64_t t_numerator = 0;
    if(diagonal()) {
      x_numerator -= static_cast<std::int64_t>(displacement) * coordinate.v;
      x_numerator += w_space * coordinate.w;
      y_numerator = -coordinate.u - static_cast<std::int64_t>(displacement) * coordinate.v + w_space * coordinate.w;
      t_numerator = static_cast<std::int64_t>(period) * coordinate.v + w_time * coordinate.w;
    } else {
      y_numerator = -static_cast<std::int64_t>(displacement) * coordinate.v + w_space * coordinate.w;
      t_numerator = static_cast<std::int64_t>(period) * coordinate.v + w_time * coordinate.w;
    }
    if(x_numerator % scale != 0 || y_numerator % scale != 0 || t_numerator % scale != 0) {
      throw std::logic_error("lattice coordinate does not map to an integral physical cell");
    }
    return {x_numerator / scale, y_numerator / scale, t_numerator / scale};
  }

  [[nodiscard]] bool edge_compatible(EdgeMode mode) const noexcept {
    switch(mode) {
    case EdgeMode::Background:
    case EdgeMode::Odd:
      return true;
    case EdgeMode::Even:
      return !diagonal();
    case EdgeMode::GlideOdd:
    case EdgeMode::GlideEven:
      if(!diagonal()) {
        return period % 2 == 0 && displacement % 2 == 0;
      }
      return period % 2 == 0 && (mode == EdgeMode::GlideOdd ? displacement % 2 == 0 : displacement % 2 != 0);
    }
    return false;
  }

  void validate_edge(EdgeMode mode) const {
    if(edge_compatible(mode)) {
      return;
    }
    if(diagonal() && mode == EdgeMode::Even) {
      throw std::runtime_error("diagonal even symmetry is incompatible with the lattice");
    }
    if(diagonal() && mode == EdgeMode::GlideOdd) {
      throw std::runtime_error("diagonal glide-odd symmetry requires an even period and even displacement");
    }
    if(diagonal() && mode == EdgeMode::GlideEven) {
      throw std::runtime_error("diagonal glide-even symmetry requires an even period and odd displacement");
    }
    throw std::runtime_error("orthogonal glide symmetry requires an even period and even displacement");
  }

  void validate_edges(EdgeMode left, EdgeMode right) const {
    validate_edge(left);
    validate_edge(right);
  }

  // Reflect an edge window in scaled quotient coordinates.  The caller's
  // logical origin anchors the local two-column window: boundary builders use
  // zero, while renderers shift it to the represented edge.  A glide advances
  // by half of V after reflection.
  [[nodiscard]] Coordinate reflect(Coordinate coordinate, EdgeMode mode, Side side, std::int64_t logical_u_origin = 0) const {
    if(mode == EdgeMode::Background) {
      throw std::logic_error("background edge does not define a reflection");
    }
    validate_edge(mode);

    const auto g = static_cast<std::int64_t>(gcd);
    std::int64_t local_constant = 0;
    if(diagonal()) {
      switch(mode) {
      case EdgeMode::Odd:
      case EdgeMode::GlideOdd:
        local_constant = side == Side::Left ? 2 * g : 4 * g;
        break;
      case EdgeMode::Even:
      case EdgeMode::GlideEven:
        local_constant = 3 * g;
        break;
      case EdgeMode::Background:
        break;
      }
    } else {
      switch(mode) {
      case EdgeMode::Odd:
      case EdgeMode::GlideOdd:
        local_constant = side == Side::Left ? 0 : 2 * g;
        break;
      case EdgeMode::Even:
      case EdgeMode::GlideEven:
        local_constant = g;
        break;
      case EdgeMode::Background:
        break;
      }
    }

    const auto base_u = static_cast<std::int64_t>(subtile_count) * logical_u_origin;
    coordinate.u = 2 * base_u + local_constant - coordinate.u;
    if(mode == EdgeMode::GlideOdd || mode == EdgeMode::GlideEven) {
      coordinate.v += diagonal() ? g : g / 2;
      coordinate.v = floor_mod(coordinate.v, subtile_count);
    }
    return coordinate;
  }

private:
  static std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) {
    auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    if(remainder < 0) {
      --quotient;
    }
    return quotient;
  }

  static std::int64_t floor_mod(std::int64_t value, std::int64_t modulus) {
    const auto remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
  }

  // Return (g, x, y) with a*x + b*y = g and g >= 0.
  static std::tuple<std::int64_t, std::int64_t, std::int64_t> extended_gcd(std::int64_t a, std::int64_t b) {
    std::int64_t old_r = a;
    std::int64_t r = b;
    std::int64_t old_x = 1;
    std::int64_t x = 0;
    std::int64_t old_y = 0;
    std::int64_t y = 1;
    while(r != 0) {
      const auto quotient = old_r / r;
      std::tie(old_r, r) = std::pair{r, old_r - quotient * r};
      std::tie(old_x, x) = std::pair{x, old_x - quotient * x};
      std::tie(old_y, y) = std::pair{y, old_y - quotient * y};
    }
    if(old_r < 0) {
      old_r = -old_r;
      old_x = -old_x;
      old_y = -old_y;
    }
    return {old_r, old_x, old_y};
  }

  void build_spots() {
    const auto modulus = static_cast<std::int64_t>(subtile_count);
    std::array<Coordinate, 3> generators{};
    if(diagonal()) {
      generators = {
          Coordinate{gcd, -w_time, period},
          Coordinate{-gcd, -w_time, period},
          Coordinate{0, 2 * w_space, 2LL * displacement},
      };
    } else {
      generators = {
          Coordinate{subtile_count, 0, 0},
          Coordinate{0, -w_time, period},
          Coordinate{0, w_space, displacement},
      };
    }

    auto normalized = [&](Coordinate coordinate) {
      coordinate.u = floor_mod(coordinate.u, modulus);
      coordinate.v = floor_mod(coordinate.v, modulus);
      coordinate.w = floor_mod(coordinate.w, modulus);
      return coordinate;
    };

    std::set<Coordinate> discovered;
    std::vector<Coordinate> pending;
    discovered.insert({});
    pending.push_back({});
    for(std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
      for(const auto generator : generators) {
        const auto next = normalized({pending[cursor].u + generator.u, pending[cursor].v + generator.v, pending[cursor].w + generator.w});
        if(discovered.insert(next).second) {
          pending.push_back(next);
        }
      }
    }
    if(discovered.size() != static_cast<std::size_t>(subtile_count)) {
      throw std::logic_error("lattice spot enumeration has the wrong determinant");
    }
    spots.assign(discovered.begin(), discovered.end());
  }
};

} // namespace rlife::llsss
