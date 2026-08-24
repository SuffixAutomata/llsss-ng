#pragma once

#include "geometry_acceptance.hpp"
#include "geometry_render.hpp"
#include "indexed_executor.hpp"
#include "rule.hpp"
#include "succinct_slice_tree.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace rlife::llsss {

namespace detail {

[[nodiscard]] constexpr std::uint8_t expand_left_edges_reference(std::uint8_t leaves) noexcept {
  leaves &= 0x0fU;
  return static_cast<std::uint8_t>((leaves & 1U ? 0x03U : 0U) | (leaves & 2U ? 0x0cU : 0U) | (leaves & 4U ? 0x30U : 0U) |
                                   (leaves & 8U ? 0xc0U : 0U));
}

[[nodiscard]] constexpr std::uint8_t expand_left_edges_deposit_algebra(std::uint8_t leaves) noexcept {
  leaves &= 0x0fU;
  const auto spread = static_cast<std::uint8_t>((leaves & 0x01U) | ((leaves & 0x02U) << 1U) | ((leaves & 0x04U) << 2U) |
                                                ((leaves & 0x08U) << 3U));
  return static_cast<std::uint8_t>(spread | (spread << 1U));
}

static_assert([] {
  for(std::uint8_t leaves = 0; leaves < 16U; ++leaves) {
    if(expand_left_edges_deposit_algebra(leaves) != expand_left_edges_reference(leaves))
      return false;
  }
  return true;
}());

[[nodiscard]] inline std::uint8_t expand_left_edges(std::uint8_t leaves) noexcept {
#if defined(__BMI2__)
  const auto spread = static_cast<std::uint8_t>(_pdep_u32(leaves & 0x0fU, 0x55U));
  return static_cast<std::uint8_t>(spread | (spread << 1U));
#else
  constexpr std::array<std::uint8_t, 16> expanded = {
      0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
      0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff,
  };
  return expanded[leaves & 0x0fU];
#endif
}

} // namespace detail

std::uint64_t getMaxRSS() {
#ifdef __linux__
  struct rusage usage;
  if(getrusage(RUSAGE_SELF, &usage) == 0)
    return usage.ru_maxrss * 1024ll;
#endif
  return 0;
}

inline std::string integer_format(std::uint64_t n) {
  if(n < 10 * 1024)
    return std::to_string(n);
  if(n < 10 * 1024 * 1024)
    return std::to_string(n >> 10) + "K";
  if(n < (10ll << 30))
    return std::to_string(n >> 20) + "M";
  return std::to_string(n >> 30) + "G";
}

enum class PartialMode { None, Final, Every };
enum class SaveMode { None, Final, Every };

struct Options {
  std::string rule = "S23/B3";
  std::string geometry;
  std::string start;
  EdgeMode left_edge = EdgeMode::Background;
  EdgeMode right_edge = EdgeMode::Background;
  bool bcaf = false;
  bool detect_ends = true;
  bool halt_on_ends = true;
  bool phase_progress = false;
  int worker_count = 1;
  int halt_height = -1;
  PartialMode partial_mode = PartialMode::Final;
  int partial_every = 1;
  std::string partial_output;
  std::string stats_output;
  bool verbose = false;
  bool phase_timings = false;
  SaveMode save_mode = SaveMode::Final;
  int save_every = 1;
  std::string savefile = "save";
  std::string loadfile;
  // Checkpoint loading uses saved configuration as its baseline.  This set
  // distinguishes an invocation default from an option the user explicitly
  // supplied, so mutable settings can be overridden intentionally.
  std::set<std::string> explicitly_set;
};

// One bit for each CA-compatible neighboring leaf pair in deterministic
// synchronized-DFS order when a filter actually rejects a pair.  An all-one
// gate is implicit and costs no payload.  This retains correlation that a
// unary slice projection cannot express, without storing either endpoint.
struct PairGate {
  static constexpr std::uint64_t index_quantum = 1U << 14U;

  PackedTags bits;
  std::uint64_t bit_count = 0;
  std::uint32_t index_depth = 0;
  std::uint32_t index_words_per_path = 0;
  std::vector<std::uint64_t> index_starts;
  std::vector<std::uint64_t> index_paths;

  [[nodiscard]] std::uint64_t size() const noexcept { return bit_count; }
  [[nodiscard]] bool get(std::uint64_t index) const noexcept { return bits.size() == 0 ? true : bits.get(index); }
  void push_back(bool value) {
    if(bits.size() != 0) {
      bits.push_back(value);
    } else if(!value) {
      bits.reset_size(bit_count + 1U);
      bits.set_all();
      bits.set(bit_count, false);
    }
    ++bit_count;
  }
  void append_selected(std::uint8_t selected, std::uint8_t values) {
    if(selected == 0)
      return;
    const auto count = static_cast<unsigned>(std::popcount(static_cast<unsigned>(selected)));
    std::uint8_t packed = 0;
#if defined(__BMI2__)
    packed = static_cast<std::uint8_t>(_pext_u32(values, selected));
#else
    auto remaining = selected;
    unsigned output = 0;
    while(remaining != 0) {
      const auto bit = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
      packed = static_cast<std::uint8_t>(packed | (((values >> bit) & 1U) << output++));
      remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
    }
#endif
    const auto all = static_cast<std::uint8_t>((1U << count) - 1U);
    if(bits.size() == 0) {
      if(packed == all) {
        bit_count += count;
        return;
      }
      bits.reset_size(bit_count);
      bits.set_all();
    }
    bits.append_low_bits(packed, count);
    bit_count += count;
  }
  // The implicit relation builder emits only accepted edges, so its payload
  // remains all-one and only the logical length changes.
  void append_implicit_ones(std::uint64_t count) noexcept { bit_count += count; }
  void reset_index(std::size_t depth) {
    if(depth > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("pair-gate index depth is too large");
    }
    index_depth = static_cast<std::uint32_t>(depth);
    index_words_per_path = static_cast<std::uint32_t>((3U * depth + 63U) / 64U);
    index_starts.clear();
    index_paths.clear();
  }
  void add_index_path(std::uint64_t start, const std::uint8_t* triples, std::size_t depth) {
    if(depth != index_depth || index_words_per_path == 0 || (!index_starts.empty() && start <= index_starts.back())) {
      throw std::logic_error("invalid pair-gate index path");
    }
    index_starts.push_back(start);
    const auto offset = index_paths.size();
    index_paths.resize(offset + index_words_per_path, 0);
    for(std::size_t row = 0; row < depth; ++row) {
      const auto bit = 3U * row;
      const auto value = static_cast<std::uint64_t>(triples[row] & 0b111U);
      index_paths[offset + bit / 64U] |= value << (bit & 63U);
      if((bit & 63U) > 61U) {
        index_paths[offset + bit / 64U + 1U] |= value >> (64U - (bit & 63U));
      }
    }
  }
  void add_index_path_with_final(std::uint64_t start, const std::uint8_t* prefix, std::size_t prefix_depth, std::uint8_t final_triple) {
    if(index_depth != prefix_depth + 1U || index_words_per_path == 0 || (!index_starts.empty() && start <= index_starts.back())) {
      throw std::logic_error("invalid pair-gate final index path");
    }
    index_starts.push_back(start);
    const auto offset = index_paths.size();
    index_paths.resize(offset + index_words_per_path, 0);
    auto pack = [&](std::size_t row, std::uint8_t triple) {
      const auto bit = 3U * row;
      const auto value = static_cast<std::uint64_t>(triple & 0b111U);
      index_paths[offset + bit / 64U] |= value << (bit & 63U);
      if((bit & 63U) > 61U) {
        index_paths[offset + bit / 64U + 1U] |= value >> (64U - (bit & 63U));
      }
    };
    for(std::size_t row = 0; row < prefix_depth; ++row)
      pack(row, prefix[row]);
    pack(prefix_depth, final_triple);
  }
  [[nodiscard]] std::uint8_t indexed_triple(std::size_t checkpoint, std::size_t row) const noexcept {
    const auto offset = checkpoint * index_words_per_path;
    const auto bit = 3U * row;
    auto value = index_paths[offset + bit / 64U] >> (bit & 63U);
    if((bit & 63U) > 61U) {
      value |= index_paths[offset + bit / 64U + 1U] << (64U - (bit & 63U));
    }
    return static_cast<std::uint8_t>(value & 0b111U);
  }
  [[nodiscard]] bool index_ready(std::size_t depth) const noexcept {
    if(index_depth != depth || index_words_per_path != (std::uint64_t{3} * depth + 63U) / 64U || index_words_per_path == 0 || index_starts.empty() ||
       index_starts.front() != 0 || index_starts.back() >= bit_count ||
       index_starts.size() > std::numeric_limits<std::size_t>::max() / index_words_per_path ||
       index_paths.size() != index_starts.size() * index_words_per_path) {
      return false;
    }
    for(std::size_t index = 1; index < index_starts.size(); ++index) {
      if(index_starts[index - 1U] >= index_starts[index])
        return false;
    }
    return true;
  }
  void reserve_payload_bits(std::uint64_t count) { bits.reserve_bits(count); }
  void reserve_index_entries(std::size_t count) {
    if(index_words_per_path != 0 && count > std::numeric_limits<std::size_t>::max() / index_words_per_path)
      throw std::overflow_error("pair-gate index reservation is too large");
    index_starts.reserve(count);
    index_paths.reserve(count * index_words_per_path);
  }
  void append(PairGate&& segment) {
    if(segment.bit_count == 0)
      return;
    if(index_words_per_path == 0) {
      reset_index(segment.index_depth);
    }
    // The destination was fully validated after its index was built, and each
    // append below preserves ordering by offsetting a validated segment by the
    // old bit count.  Re-running index_ready() here would rescan the entire
    // accumulated destination for every segment, making an ordered merge
    // quadratic in the number of restart ranges on deep rows.
    const bool destination_shape_valid = [&]() {
      if(bit_count == 0)
        return index_starts.empty() && index_paths.empty();
      if(index_words_per_path == 0 || index_starts.empty() || index_starts.front() != 0 || index_starts.back() >= bit_count ||
         index_starts.size() > std::numeric_limits<std::size_t>::max() / index_words_per_path)
        return false;
      return index_paths.size() == index_starts.size() * index_words_per_path;
    }();
    if(!destination_shape_valid || !segment.index_ready(index_depth) || index_depth != segment.index_depth ||
       index_words_per_path != segment.index_words_per_path) {
      throw std::logic_error("cannot append incompatible indexed pair gates");
    }
    const auto output_offset = bit_count;
    if(bits.size() == 0 && segment.bits.size() == 0) {
      bit_count += segment.bit_count;
    } else {
      if(bits.size() == 0) {
        bits.reset_size(bit_count);
        bits.set_all();
      }
      if(segment.bits.size() == 0) {
        bits.append_ones(segment.bit_count);
      } else {
        bits.append(segment.bits);
      }
      bit_count += segment.bit_count;
    }
    index_starts.reserve(index_starts.size() + segment.index_starts.size());
    for(const auto start : segment.index_starts)
      index_starts.push_back(output_offset + start);
    index_paths.insert(index_paths.end(), segment.index_paths.begin(), segment.index_paths.end());
    segment = PairGate{};
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return bits.allocated_bytes() + index_starts.capacity() * sizeof(index_starts[0]) + index_paths.capacity() * sizeof(index_paths[0]);
  }
  [[nodiscard]] std::uint64_t count() const noexcept { return bits.size() == 0 ? bit_count : bits.count(0, bits.size()); }
};

inline constexpr std::array<std::uint8_t, 16> checkpoint_magic_ = {
    'R', 'L', 'I', 'F', 'E', '-', 'L', 'L', 'S', 'S', 'S', '-', 'C', 'P', 0, 1,
};
inline constexpr std::uint32_t checkpoint_version_ = 4;

class CheckpointWriter {
public:
  explicit CheckpointWriter(std::ostream& output) : output_(output) {}

  void bytes(const void* data, std::size_t size) {
    output_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if(!output_) {
      throw std::runtime_error("failed while writing checkpoint");
    }
    const auto* input = static_cast<const std::uint8_t*>(data);
    for(std::size_t i = 0; i < size; ++i) {
      checksum_ = (checksum_ ^ input[i]) * 1099511628211ULL;
    }
  }

  void u8(std::uint8_t value) { bytes(&value, sizeof(value)); }
  void boolean(bool value) { u8(value ? 1U : 0U); }
  void u32(std::uint32_t value) {
    std::array<std::uint8_t, 4> encoded{};
    for(std::size_t i = 0; i < encoded.size(); ++i)
      encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
    bytes(encoded.data(), encoded.size());
  }
  void u64(std::uint64_t value) {
    std::array<std::uint8_t, 8> encoded{};
    for(std::size_t i = 0; i < encoded.size(); ++i)
      encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
    bytes(encoded.data(), encoded.size());
  }
  void string(const std::string& value) {
    u64(value.size());
    bytes(value.data(), value.size());
  }
  void vector_u64(const std::vector<std::uint64_t>& values) {
    u64(values.size());
    std::array<std::uint8_t, 8192> encoded{};
    for(std::size_t offset = 0; offset < values.size();) {
      const auto count = std::min<std::size_t>(encoded.size() / sizeof(std::uint64_t), values.size() - offset);
      for(std::size_t index = 0; index < count; ++index) {
        const auto value = values[offset + index];
        for(std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte)
          encoded[index * sizeof(std::uint64_t) + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
      }
      bytes(encoded.data(), count * sizeof(std::uint64_t));
      offset += count;
    }
  }
  void vector_u8(const std::vector<std::uint8_t>& values) {
    u64(values.size());
    if(!values.empty())
      bytes(values.data(), values.size());
  }

  void finish() {
    const auto checksum = checksum_;
    std::array<std::uint8_t, 8> encoded{};
    for(std::size_t i = 0; i < encoded.size(); ++i)
      encoded[i] = static_cast<std::uint8_t>(checksum >> (8U * i));
    output_.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if(!output_) {
      throw std::runtime_error("failed while finishing checkpoint");
    }
  }

private:
  std::ostream& output_;
  std::uint64_t checksum_ = 14695981039346656037ULL;
};

class CheckpointReader {
public:
  CheckpointReader(std::istream& input, std::uint64_t size) : input_(input), remaining_(size) {}

  void bytes(void* data, std::size_t size) {
    if(size > remaining_) {
      throw std::runtime_error("checkpoint is truncated");
    }
    input_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if(!input_) {
      throw std::runtime_error("failed while reading checkpoint");
    }
    const auto* output = static_cast<const std::uint8_t*>(data);
    for(std::size_t i = 0; i < size; ++i) {
      checksum_ = (checksum_ ^ output[i]) * 1099511628211ULL;
    }
    remaining_ -= size;
  }

  [[nodiscard]] std::uint8_t u8() {
    std::uint8_t value = 0;
    bytes(&value, sizeof(value));
    return value;
  }
  [[nodiscard]] bool boolean() {
    const auto value = u8();
    if(value > 1U)
      throw std::runtime_error("invalid boolean in checkpoint");
    return value != 0;
  }
  [[nodiscard]] std::uint32_t u32() {
    std::array<std::uint8_t, 4> encoded{};
    bytes(encoded.data(), encoded.size());
    std::uint32_t value = 0;
    for(std::size_t i = 0; i < encoded.size(); ++i)
      value |= static_cast<std::uint32_t>(encoded[i]) << (8U * i);
    return value;
  }
  [[nodiscard]] std::uint64_t u64() {
    std::array<std::uint8_t, 8> encoded{};
    bytes(encoded.data(), encoded.size());
    std::uint64_t value = 0;
    for(std::size_t i = 0; i < encoded.size(); ++i)
      value |= static_cast<std::uint64_t>(encoded[i]) << (8U * i);
    return value;
  }
  [[nodiscard]] std::string string() {
    const auto size = u64();
    if(size > remaining_ || size > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error("invalid string size in checkpoint");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    bytes(value.data(), value.size());
    return value;
  }
  [[nodiscard]] std::vector<std::uint64_t> vector_u64() {
    const auto count = u64();
    if(count > std::numeric_limits<std::size_t>::max() || count > remaining_ / sizeof(std::uint64_t)) {
      throw std::runtime_error("invalid vector size in checkpoint");
    }
    std::vector<std::uint64_t> values(static_cast<std::size_t>(count));
    std::array<std::uint8_t, 8192> encoded{};
    for(std::size_t offset = 0; offset < values.size();) {
      const auto chunk = std::min<std::size_t>(encoded.size() / sizeof(std::uint64_t), values.size() - offset);
      bytes(encoded.data(), chunk * sizeof(std::uint64_t));
      for(std::size_t index = 0; index < chunk; ++index) {
        std::uint64_t value = 0;
        for(std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte)
          value |= static_cast<std::uint64_t>(encoded[index * sizeof(std::uint64_t) + byte]) << (8U * byte);
        values[offset + index] = value;
      }
      offset += chunk;
    }
    return values;
  }
  [[nodiscard]] std::vector<std::uint8_t> vector_u8() {
    const auto count = u64();
    if(count > remaining_ || count > std::numeric_limits<std::size_t>::max())
      throw std::runtime_error("invalid byte-vector size in checkpoint");
    std::vector<std::uint8_t> values(static_cast<std::size_t>(count));
    if(!values.empty())
      bytes(values.data(), values.size());
    return values;
  }

  void finish() {
    if(remaining_ != sizeof(std::uint64_t)) {
      throw std::runtime_error("checkpoint has trailing or missing data");
    }
    std::array<std::uint8_t, 8> encoded{};
    input_.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if(!input_)
      throw std::runtime_error("checkpoint checksum is truncated");
    std::uint64_t expected = 0;
    for(std::size_t i = 0; i < encoded.size(); ++i)
      expected |= static_cast<std::uint64_t>(encoded[i]) << (8U * i);
    remaining_ = 0;
    if(expected != checksum_)
      throw std::runtime_error("checkpoint checksum mismatch");
  }

private:
  std::istream& input_;
  std::uint64_t remaining_ = 0;
  std::uint64_t checksum_ = 14695981039346656037ULL;
};

inline volatile std::sig_atomic_t checkpoint_interrupt_requested_ = 0;

inline void checkpoint_interrupt_handler_(int) { checkpoint_interrupt_requested_ = 1; }

inline void install_checkpoint_interrupt_handler() {
  checkpoint_interrupt_requested_ = 0;
  if(std::signal(SIGINT, checkpoint_interrupt_handler_) == SIG_ERR) {
    throw std::runtime_error("cannot install Ctrl-C handler");
  }
}

inline std::vector<std::string> split_words(std::string text) {
  for(char& ch : text) {
    if(ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',') {
      ch = ' ';
    }
  }
  std::istringstream input(text);
  std::vector<std::string> result;
  for(std::string word; input >> word;) {
    result.push_back(std::move(word));
  }
  return result;
}

inline void print_help(std::ostream& out) {
  out <<
      R"(rlife_llsss llsss [options] <geometry> <start>

Orthogonal and diagonal fixed-width LLSSS using succinct two-column slice trees.

  <geometry>                 c4-f2b, 2c5-f2b, c5d-f2b, ...
  <start>                    @bg(W), @bg:W, an RLE file, or an ASCII grid

  --rule RULE                isotropic B/S or Hensel rule (default S23/B3)
  --symmetry MODE            asymmetric, odd, even, gso, or gse; left only
  --left-edge MODE           bg, odd, even, gso, or gse
  --right-edge MODE          bg, odd, even, gso, or gse
  --bg-agar zero             zero background is the supported agar
  --filters bcaf             BCAF zero-background witness filter
  --ends default|none        zero-background completion detection (default)
  --[no-]halt-on-ends        halt after the first completion (default: halt)
  --halts w_pos:N            stop at logical W-tile position N
  --save MODE                none, final, or every:N (default: final)
  --savefile FILE_PREFIX     save as FILE_PREFIX_{row} (default: save)
  --load FILE                resume a checkpoint; geometry/start may be omitted
  --partials MODE            none, final, default, or every:N (default: final)
  --partial-output FILE      write RLE partials/completions to FILE
  --dump-slice-stats FILE    append per-height succinct-slice statistics
  --phase-progress           print individual sweep phases
  --phase-timings            print final cumulative timings per phase
  --verbose                  verbose information at every row
  --threads N                indexed relation-walk workers (default: 1)
  -h, --help                 show this help

Symmetric edges are checked for compatibility with the selected lattice and
velocity. RLE/ASCII input rows are flattened logical-U lattice subtiles;
@bg(W) needs no conversion.
The implementation has no autochoke and stores no join endpoints or join DAG.
)";
}

inline Options parse_cli(int argc, char** argv) {
  if(argc < 2) {
    throw std::runtime_error("usage: rlife_llsss llsss [options] <geometry> <start>");
  }
  int index = 1;
  if(std::string(argv[index++]) != "llsss") {
    throw std::runtime_error("the only subcommand is llsss");
  }

  Options options;
  std::optional<EdgeMode> symmetry_edge;
  std::optional<EdgeMode> explicit_left_edge;
  std::optional<EdgeMode> explicit_right_edge;
  std::vector<std::string> positional;
  auto argument = [&](const std::string& option) {
    if(index >= argc) {
      throw std::runtime_error("missing argument for " + option);
    }
    return std::string(argv[index++]);
  };

  while(index < argc) {
    const std::string current = argv[index++];
    if(current == "--") {
      while(index < argc) {
        positional.emplace_back(argv[index++]);
      }
      break;
    }
    if(current == "-h" || current == "--help") {
      print_help(std::cout);
      std::exit(0);
    }
    if(!current.starts_with("--")) {
      positional.push_back(current);
      continue;
    }
    if(current == "--rule") {
      options.rule = argument(current);
      options.explicitly_set.insert("rule");
    } else if(current == "--left-edge") {
      explicit_left_edge = parse_edge(argument(current));
      options.explicitly_set.insert("left_edge");
    } else if(current == "--right-edge") {
      explicit_right_edge = parse_edge(argument(current));
      options.explicitly_set.insert("right_edge");
    } else if(current == "--symmetry") {
      const auto value = argument(current);
      if(value == "asymmetric" || value == "asym") {
        symmetry_edge = EdgeMode::Background;
      } else if(value == "odd" || value == "even" || value == "gse" || value == "glide-even" || value == "glide_even" || value == "gso" ||
                value == "glide-odd" || value == "glide_odd") {
        symmetry_edge = parse_edge(value);
      } else {
        throw std::runtime_error("--symmetry must be asymmetric, odd, even, gso, or gse");
      }
      options.explicitly_set.insert("left_edge");
      options.explicitly_set.insert("right_edge");
    } else if(current == "--bg-agar") {
      if(argument(current) != "zero") {
        throw std::runtime_error("this rendition currently supports --bg-agar zero only");
      }
    } else if(current == "--filters") {
      options.bcaf = false;
      options.explicitly_set.insert("bcaf");
      for(const auto& filter : split_words(argument(current))) {
        if(filter == "bcaf") {
          options.bcaf = true;
        } else if(filter != "none") {
          throw std::runtime_error("unsupported filter: " + filter);
        }
      }
    } else if(current == "--ends") {
      options.explicitly_set.insert("detect_ends");
      const auto value = argument(current);
      if(value == "default" || value == "bg") {
        options.detect_ends = true;
      } else if(value == "none") {
        options.detect_ends = false;
      } else {
        throw std::runtime_error("--ends supports default or none");
      }
    } else if(current == "--halt-on-ends") {
      options.halt_on_ends = true;
      options.explicitly_set.insert("halt_on_ends");
    } else if(current == "--no-halt-on-ends") {
      options.halt_on_ends = false;
      options.explicitly_set.insert("halt_on_ends");
    } else if(current == "--halts") {
      options.explicitly_set.insert("halt_height");
      const auto value = argument(current);
      std::smatch match;
      if(!std::regex_match(value, match, std::regex(R"(^w_pos:([0-9]+)$)"))) {
        throw std::runtime_error("--halts supports w_pos:N");
      }
      options.halt_height = std::stoi(match[1].str());
    } else if(current == "--save") {
      options.explicitly_set.insert("save");
      const auto value = argument(current);
      std::smatch match;
      if(value == "none") {
        options.save_mode = SaveMode::None;
      } else if(value == "final") {
        options.save_mode = SaveMode::Final;
      } else if(std::regex_match(value, match, std::regex(R"(^every:([0-9]+)$)"))) {
        options.save_mode = SaveMode::Every;
        options.save_every = std::stoi(match[1].str());
        if(options.save_every <= 0) {
          throw std::runtime_error("save interval must be positive");
        }
      } else {
        throw std::runtime_error("--save supports none, final, or every:N");
      }
    } else if(current == "--savefile") {
      options.savefile = argument(current);
      options.explicitly_set.insert("savefile");
      if(options.savefile.empty()) {
        throw std::runtime_error("--savefile must not be empty");
      }
    } else if(current == "--load") {
      options.loadfile = argument(current);
      if(options.loadfile.empty()) {
        throw std::runtime_error("--load must not be empty");
      }
    } else if(current == "--partials" || current == "--pre-partials") {
      options.explicitly_set.insert("partials");
      const auto value = argument(current);
      if(value == "none" || value == "[]") {
        options.partial_mode = PartialMode::None;
      } else if(value == "final") {
        options.partial_mode = PartialMode::Final;
      } else if(value == "default" || value == "unique" || value == "every") {
        options.partial_mode = PartialMode::Every;
        options.partial_every = 1;
      } else if(value.starts_with("every:")) {
        options.partial_mode = PartialMode::Every;
        options.partial_every = std::stoi(value.substr(6));
        if(options.partial_every <= 0) {
          throw std::runtime_error("partial interval must be positive");
        }
      } else {
        throw std::runtime_error("--partials supports none, final, default, or every:N");
      }
    } else if(current == "--partial-output") {
      options.partial_output = argument(current);
      options.explicitly_set.insert("partial_output");
    } else if(current == "--dump-slice-stats") {
      options.stats_output = argument(current);
      options.explicitly_set.insert("stats_output");
    } else if(current == "--phase-progress") {
      options.phase_progress = true;
      options.explicitly_set.insert("phase_progress");
    } else if(current == "--phase-timings") {
      options.phase_timings = true;
      options.explicitly_set.insert("phase_timings");
    } else if(current == "--verbose") {
      options.verbose = true;
      options.explicitly_set.insert("verbose");
    } else if(current == "--threads") {
      options.worker_count = std::stoi(argument(current));
      options.explicitly_set.insert("threads");
      if(options.worker_count <= 0) {
        throw std::runtime_error("--threads must be positive");
      }
    } else if(current == "--pre-reify-autochoke" || current == "--pre-reify-autochoke-type") {
      throw std::runtime_error("autochoke is deliberately not implemented");
    } else {
      throw std::runtime_error("unknown option: " + current);
    }
  }

  if(options.loadfile.empty() && positional.size() != 2) {
    throw std::runtime_error("expected exactly <geometry> <start>");
  }
  if(!options.loadfile.empty() && positional.size() != 0 && positional.size() != 2) {
    throw std::runtime_error("with --load, omit <geometry> <start> or supply both for validation");
  }
  // The convenience option describes at most one symmetry boundary.  Direct
  // edge options are independent and override it regardless of CLI order.
  if(symmetry_edge.has_value()) {
    options.left_edge = *symmetry_edge;
    options.right_edge = EdgeMode::Background;
  }
  if(explicit_left_edge.has_value())
    options.left_edge = *explicit_left_edge;
  if(explicit_right_edge.has_value())
    options.right_edge = *explicit_right_edge;
  if(!positional.empty()) {
    options.geometry = positional[0];
    options.start = positional[1];
    options.explicitly_set.insert("geometry");
    options.explicitly_set.insert("start");
  }
  return options;
}

struct StartGrid {
  std::size_t width = 0;
  std::size_t height = 0;
  // -1 is an independent wildcard, 0 dead, 1 live.
  std::vector<std::int8_t> cells;

  [[nodiscard]] std::int8_t at(std::size_t x, std::size_t y) const { return cells[y * width + x]; }
};

class Solver {
public:
  explicit Solver(Options options) : options_(std::move(options)) {
    if(options_.save_mode != SaveMode::None)
      install_checkpoint_interrupt_handler();
    const bool loading = !options_.loadfile.empty();
    loaded_from_checkpoint_ = loading;
    if(loading) {
      load_checkpoint(options_.loadfile);
    }
    geometry_ = Geometry::parse(options_.geometry);
    rule_ = RuleTable::parse(options_.rule);
    geometry_.validate_edges(options_.left_edge, options_.right_edge);
    geometry_acceptance_.build(geometry_, rule_, options_.left_edge, options_.right_edge);
    build_pair_transition_table();
    rule_.release_partial_lookup();
    if(loading) {
      validate_loaded_state();
    }
    if(!options_.partial_output.empty()) {
      const auto mode = loading && !options_.explicitly_set.contains("partial_output") ? std::ios::app : std::ios::trunc;
      partial_file_.open(options_.partial_output, std::ios::out | mode);
      if(!partial_file_) {
        throw std::runtime_error("cannot open partial output: " + options_.partial_output);
      }
    }
    if(!options_.stats_output.empty()) {
      const auto mode = loading && !options_.explicitly_set.contains("stats_output") ? std::ios::app : std::ios::trunc;
      stats_file_.open(options_.stats_output, std::ios::out | mode);
      if(!stats_file_) {
        throw std::runtime_error("cannot open slice stats: " + options_.stats_output);
      }
    }
    if(loading) {
      std::cout << "checkpoint loaded: " << options_.loadfile << " at row " << height_ << '\n';
    } else {
      initialize();
    }
    if(options_.worker_count > 1 && !options_.verbose && pair_gates_ready_) {
      const bool indexes_ready = std::ranges::all_of(pair_gates_, [&](const PairGate& gate) {
        return gate.size() == 0 || gate.index_ready(pair_gate_depth_);
      });
      if(!indexes_ready)
        build_pair_gate_indexes();
    }
  }

  ~Solver() {
    if(options_.phase_timings) {
      finish_phase_timing();
      for(auto [phase, seconds] : phase_timings_)
        std::cerr << phase << " >> " << std::fixed << std::setprecision(6) << seconds << '\n';
    }
  }

  int run() {
    std::cout << "rlife_llsss: geom=" << geometry_.source << " lattice=" << (geometry_.diagonal() ? "diagonal" : "orthogonal") << " p=" << geometry_.period
              << " k=" << geometry_.displacement << " subtiles=" << geometry_.subtile_count << " rule=" << options_.rule
              << " width=" << width_ << " left_edge=" << edge_name(options_.left_edge) << " right_edge=" << edge_name(options_.right_edge)
              << " bcaf=" << (options_.bcaf ? "yes" : "no") << " halt_on_ends=" << (options_.halt_on_ends ? "yes" : "no") << '\n';
    if(options_.worker_count != 1) {
      std::cout << "indexed relation walks: " << options_.worker_count << " workers\n";
    }
    running_ = true;
    print_stats("init", 0.0);

    if(slices_.empty()) {
      std::cout << "search space is empty after initialization\n";
      return finish(0);
    }
    if(completion_at_current_row_ && options_.detect_ends && options_.halt_on_ends) {
      std::cout << "checkpoint row already contains a halting completion\n";
      return finish(0);
    }
    if(options_.halt_height >= 0 && geometry_.w_position(height_) >= static_cast<std::size_t>(options_.halt_height)) {
      if(!loaded_from_checkpoint_ || options_.explicitly_set.contains("partials")) {
        emit_final_partial("halt");
      }
      return finish(0);
    }
    if(checkpoint_interrupt_requested_) {
      return finish_interrupted();
    }

    for(;;) {
      if(checkpoint_interrupt_requested_) {
        return finish_interrupted();
      }
      const auto started = Clock::now();
      pair_states_ = 0;
      pair_leaves_ = 0;
      boundary_states_ = 0;
      peak_tag_bytes_ = 0;
      completion_at_current_row_ = false;
      bool row_reported = false;
      auto report_row = [&]() {
        if(row_reported)
          return;
        finish_phase_timing();
        const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
        print_stats("step", seconds);
        row_reported = true;
      };

      phase("extend slice-tree tails");
      expand_slice_tails();
      ++height_;
      expanded_uniform_tail_ = true;

      phase("support and filter sweeps");
      if(!prune_supported()) {
        if(geometry_.subtile_count != 1) {
          std::cout << "search exhausted at flattened depth " << height_ << " (w_pos " << geometry_.position_string(height_) << ")\n";
        } else {
          std::cout << "search exhausted at height " << height_ << '\n';
        }
        slices_.clear();
        pair_gates_.clear();
        pair_gates_ready_ = false;
        bcaf_clause_begin_depth_ = 0;
        exhausted_ = true;
        return finish(0);
      }
      expanded_uniform_tail_ = false;
      phase("row accounting");

      bool partial_emitted = false;
      if(options_.partial_mode == PartialMode::Every && height_ % static_cast<std::size_t>(options_.partial_every) == 0) {
        phase("partial reconstruction/output");
        emit_board(reconstruct_partial(), "partial");
        partial_emitted = true;
      }

      if(options_.detect_ends) {
        phase("end-tag propagation");
        if(auto completion = find_completion()) {
          completion_at_current_row_ = true;
          if(geometry_.subtile_count != 1) {
            std::cout << "completion at flattened depth " << height_ << " (w_pos " << geometry_.position_string(height_) << ")\n";
          } else {
            std::cout << "completion at height " << height_ << '\n';
          }
          emit_board(*completion, "completion");
          if(options_.halt_on_ends) {
            report_row();
            return finish(0);
          }
        }
      }

      if(options_.halt_height >= 0 && geometry_.w_position(height_) >= static_cast<std::size_t>(options_.halt_height)) {
        if(geometry_.subtile_count != 1) {
          std::cout << "w_pos halt at " << geometry_.position_string(height_) << '\n';
        } else {
          std::cout << "height halt at " << height_ << '\n';
        }
        if(!partial_emitted)
          emit_final_partial("halt");
        report_row();
        return finish(0);
      }

      report_row();
      if(options_.save_mode == SaveMode::Every && height_ % static_cast<std::size_t>(options_.save_every) == 0) {
        save_checkpoint();
      }
      if(checkpoint_interrupt_requested_) {
        return finish_interrupted();
      }
    }
  }

private:
  using Clock = std::chrono::steady_clock;
  using Node = SuccinctSliceTree::Node;

  // Relation sweeps touch only current leaves.  Keeping witness/suffix bits
  // in leaf-local coordinates avoids paying for every historical internal
  // trie node, which is substantial on the large diagonal searches.
  struct LeafTags {
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
    // leaf-local plane.  Source and destination have a fixed bit offset but
    // not necessarily the same word alignment, so extract one shifted source
    // window for each destination word fragment.
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
    LeafTagPair() = default;
    LeafTagPair(Node first, Node count) : planes{LeafTags(first, count), LeafTags(first, count)} {}

    LeafTags& operator[](std::size_t index) { return planes[index]; }
    const LeafTags& operator[](std::size_t index) const { return planes[index]; }

    LeafTags planes[2];
  };

  static void write_config(CheckpointWriter& output, const Options& options) {
    output.string(options.rule);
    output.string(options.geometry);
    output.string(options.start);
    output.u8(static_cast<std::uint8_t>(options.left_edge));
    output.u8(static_cast<std::uint8_t>(options.right_edge));
    output.boolean(options.bcaf);
    output.boolean(options.detect_ends);
    output.boolean(options.halt_on_ends);
    output.boolean(options.phase_progress);
    output.u64(static_cast<std::uint64_t>(options.worker_count));
    output.u64(options.halt_height < 0 ? 0U : static_cast<std::uint64_t>(options.halt_height) + 1U);
    output.u8(static_cast<std::uint8_t>(options.partial_mode));
    output.u64(static_cast<std::uint64_t>(options.partial_every));
    output.string(options.partial_output);
    output.string(options.stats_output);
    output.boolean(options.verbose);
    output.boolean(options.phase_timings);
    output.u8(static_cast<std::uint8_t>(options.save_mode));
    output.u64(static_cast<std::uint64_t>(options.save_every));
    output.string(options.savefile);
  }

  static int checkpoint_positive_int(CheckpointReader& input, std::string_view field) {
    const auto value = input.u64();
    if(value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("invalid " + std::string(field) + " in checkpoint");
    }
    return static_cast<int>(value);
  }

  static Options read_config(CheckpointReader& input) {
    Options options;
    options.rule = input.string();
    options.geometry = input.string();
    options.start = input.string();
    const auto left_edge = input.u8();
    const auto right_edge = input.u8();
    if(left_edge > static_cast<std::uint8_t>(EdgeMode::GlideOdd) || right_edge > static_cast<std::uint8_t>(EdgeMode::GlideOdd)) {
      throw std::runtime_error("invalid edge mode in checkpoint");
    }
    options.left_edge = static_cast<EdgeMode>(left_edge);
    options.right_edge = static_cast<EdgeMode>(right_edge);
    options.bcaf = input.boolean();
    options.detect_ends = input.boolean();
    options.halt_on_ends = input.boolean();
    options.phase_progress = input.boolean();
    options.worker_count = checkpoint_positive_int(input, "thread count");
    const auto encoded_halt = input.u64();
    if(encoded_halt > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1U) {
      throw std::runtime_error("invalid halt in checkpoint");
    }
    options.halt_height = encoded_halt == 0 ? -1 : static_cast<int>(encoded_halt - 1U);
    const auto partial_mode = input.u8();
    if(partial_mode > static_cast<std::uint8_t>(PartialMode::Every)) {
      throw std::runtime_error("invalid partial mode in checkpoint");
    }
    options.partial_mode = static_cast<PartialMode>(partial_mode);
    options.partial_every = checkpoint_positive_int(input, "partial interval");
    options.partial_output = input.string();
    options.stats_output = input.string();
    options.verbose = input.boolean();
    options.phase_timings = input.boolean();
    const auto save_mode = input.u8();
    if(save_mode > static_cast<std::uint8_t>(SaveMode::Every)) {
      throw std::runtime_error("invalid save mode in checkpoint");
    }
    options.save_mode = static_cast<SaveMode>(save_mode);
    options.save_every = checkpoint_positive_int(input, "save interval");
    options.savefile = input.string();
    if(options.rule.empty() || options.geometry.empty() || options.start.empty() || options.savefile.empty()) {
      throw std::runtime_error("checkpoint configuration has an empty required value");
    }
    return options;
  }

  template <class T> void merge_immutable(const Options& command_line, const Options& saved, std::string_view name, T Options::*member) {
    if(command_line.explicitly_set.contains(std::string(name)) && command_line.*member != saved.*member) {
      throw std::runtime_error("cannot alter checkpoint search-tree option " + std::string(name));
    }
    options_.*member = saved.*member;
  }

  template <class T> void merge_mutable(const Options& command_line, const Options& saved, std::string_view name, T Options::*member) {
    if(!command_line.explicitly_set.contains(std::string(name))) {
      options_.*member = saved.*member;
    }
  }

  void merge_checkpoint_config(const Options& saved) {
    const auto command_line = options_;
    merge_immutable(command_line, saved, "rule", &Options::rule);
    merge_immutable(command_line, saved, "geometry", &Options::geometry);
    merge_immutable(command_line, saved, "start", &Options::start);
    merge_immutable(command_line, saved, "left_edge", &Options::left_edge);
    merge_immutable(command_line, saved, "right_edge", &Options::right_edge);
    merge_immutable(command_line, saved, "bcaf", &Options::bcaf);

    merge_mutable(command_line, saved, "detect_ends", &Options::detect_ends);
    merge_mutable(command_line, saved, "halt_on_ends", &Options::halt_on_ends);
    merge_mutable(command_line, saved, "phase_progress", &Options::phase_progress);
    merge_mutable(command_line, saved, "threads", &Options::worker_count);
    merge_mutable(command_line, saved, "halt_height", &Options::halt_height);
    if(!command_line.explicitly_set.contains("partials")) {
      options_.partial_mode = saved.partial_mode;
      options_.partial_every = saved.partial_every;
    }
    merge_mutable(command_line, saved, "partial_output", &Options::partial_output);
    merge_mutable(command_line, saved, "stats_output", &Options::stats_output);
    merge_mutable(command_line, saved, "verbose", &Options::verbose);
    merge_mutable(command_line, saved, "phase_timings", &Options::phase_timings);
    if(!command_line.explicitly_set.contains("save")) {
      options_.save_mode = saved.save_mode;
      options_.save_every = saved.save_every;
    }
    merge_mutable(command_line, saved, "savefile", &Options::savefile);
  }

  static std::size_t checkpoint_size(std::uint64_t value, std::string_view field) {
    if(value > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error("checkpoint " + std::string(field) + " does not fit this platform");
    }
    return static_cast<std::size_t>(value);
  }

  void load_checkpoint(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if(!input) {
      throw std::runtime_error("cannot open checkpoint: " + path);
    }
    const auto end = input.tellg();
    if(end < 0) {
      throw std::runtime_error("cannot determine checkpoint size: " + path);
    }
    input.seekg(0);
    CheckpointReader reader(input, static_cast<std::uint64_t>(end));
    std::array<std::uint8_t, checkpoint_magic_.size()> magic{};
    reader.bytes(magic.data(), magic.size());
    const auto checkpoint_version = reader.u32();
    if(magic != checkpoint_magic_ || checkpoint_version == 0 || checkpoint_version > checkpoint_version_) {
      throw std::runtime_error("unsupported checkpoint format: " + path);
    }

    merge_checkpoint_config(read_config(reader));
    exhausted_ = reader.boolean();
    completion_at_current_row_ = reader.boolean();
    width_ = checkpoint_size(reader.u64(), "width");
    height_ = checkpoint_size(reader.u64(), "height");
    const auto slice_count = checkpoint_size(reader.u64(), "slice count");
    if(checkpoint_version >= 3U) {
      implicit_relations_ = reader.boolean();
      bcaf_clause_begin_depth_ = checkpoint_size(reader.u64(), "first BCAF clause depth");
    } else {
      // A projected dense gate cannot in general be factored after the fact.
      // Preserve v1/v2 searches exactly on the legacy engine.
      implicit_relations_ = false;
      bcaf_clause_begin_depth_ = 0;
    }
    if(width_ < 3 || (!exhausted_ && slice_count != width_ - 1U) || (exhausted_ && slice_count != 0)) {
      throw std::runtime_error("checkpoint slice count does not match its width/state");
    }
    slices_.clear();
    slices_.reserve(slice_count);
    for(std::size_t index = 0; index < slice_count; ++index) {
      const auto depth = checkpoint_size(reader.u64(), "tree depth");
      const auto nodes = reader.u64();
      auto levels = reader.vector_u64();
      auto words = reader.vector_u64();
      std::vector<std::uint64_t> bcaf_prefix_words;
      std::vector<std::uint64_t> bcaf_suffix_words;
      std::vector<std::uint8_t> bcaf_child_clauses;
      if(checkpoint_version == 3U) {
        bcaf_prefix_words = reader.vector_u64();
        bcaf_suffix_words = reader.vector_u64();
      } else if(checkpoint_version >= 4U) {
        bcaf_child_clauses = reader.vector_u8();
      }
      auto slice = SuccinctSliceTree::from_checkpoint(std::move(words), std::move(levels), nodes, depth);
      if(checkpoint_version == 3U)
        slice.restore_bcaf_clauses_v3(bcaf_clause_begin_depth_, std::move(bcaf_prefix_words), std::move(bcaf_suffix_words));
      else if(checkpoint_version >= 4U)
        slice.restore_bcaf_clauses(bcaf_clause_begin_depth_, std::move(bcaf_child_clauses));
      slices_.push_back(std::move(slice));
    }

    pair_gates_ready_ = reader.boolean();
    pair_gate_depth_ = checkpoint_size(reader.u64(), "pair-gate depth");
    const auto gate_count = checkpoint_size(reader.u64(), "pair-gate count");
    const auto expected_gates = pair_gates_ready_ && !slices_.empty() ? slices_.size() - 1U : 0U;
    if(gate_count != expected_gates || (!exhausted_ && !pair_gates_ready_)) {
      throw std::runtime_error("checkpoint pair gates do not match its slices");
    }
    pair_gates_.clear();
    pair_gates_.reserve(gate_count);
    for(std::size_t index = 0; index < gate_count; ++index) {
      PairGate gate;
      gate.bit_count = reader.u64();
      const auto stored_bits = reader.u64();
      auto words = reader.vector_u64();
      if(stored_bits != 0 && stored_bits != gate.bit_count) {
        throw std::runtime_error("checkpoint pair gate has inconsistent length");
      }
      gate.bits = PackedTags::from_checkpoint(stored_bits, std::move(words));
      if(checkpoint_version >= 2U) {
        const auto index_depth = checkpoint_size(reader.u64(), "pair-gate index depth");
        if(index_depth > std::numeric_limits<std::uint32_t>::max())
          throw std::runtime_error("checkpoint pair-gate index depth is too large");
        gate.index_depth = static_cast<std::uint32_t>(index_depth);
        gate.index_words_per_path = static_cast<std::uint32_t>((3U * static_cast<std::size_t>(gate.index_depth) + 63U) / 64U);
        gate.index_starts = reader.vector_u64();
        gate.index_paths = reader.vector_u64();
        const bool has_index = !gate.index_starts.empty() || !gate.index_paths.empty();
        if(has_index && !gate.index_ready(gate.index_depth))
          throw std::runtime_error("checkpoint pair gate has an invalid sparse index");
      }
      pair_gates_.push_back(std::move(gate));
    }
    reader.finish();
  }

  void validate_loaded_state() const {
    if(height_ < geometry_.short_window()) {
      throw std::runtime_error("checkpoint height is shorter than its geometry lookback");
    }
    if(exhausted_) {
      if(completion_at_current_row_ || !slices_.empty() || pair_gates_ready_ || !pair_gates_.empty() || bcaf_clause_begin_depth_ != 0) {
        throw std::runtime_error("exhausted checkpoint contains live search state");
      }
      return;
    }
    if(slices_.size() != width_ - 1U || pair_gates_.size() + 1U != slices_.size() || pair_gate_depth_ != height_) {
      throw std::runtime_error("checkpoint search-state dimensions are inconsistent");
    }
    for(const auto& slice : slices_) {
      if(slice.depth() != height_) {
        throw std::runtime_error("checkpoint slice depth does not match its row");
      }
    }
    if(!implicit_relations_) {
      if(bcaf_clause_begin_depth_ != 0 || std::ranges::any_of(slices_, [](const SuccinctSliceTree& slice) { return slice.bcaf_clauses_present(); }))
        throw std::runtime_error("legacy checkpoint contains implicit BCAF payloads");
      return;
    }
    if(std::ranges::any_of(pair_gates_, [](const PairGate& gate) { return gate.bits.size() != 0; }))
      throw std::runtime_error("implicit checkpoint contains a dense pair-gate payload");
    if(bcaf_clause_begin_depth_ == 0) {
      if(std::ranges::any_of(slices_, [](const SuccinctSliceTree& slice) { return slice.bcaf_clauses_present(); }))
        throw std::runtime_error("implicit checkpoint has BCAF payloads without a first clause depth");
    } else {
      if(!options_.bcaf || bcaf_clause_begin_depth_ < geometry_.long_window() || bcaf_clause_begin_depth_ > height_ ||
         std::ranges::any_of(slices_, [&](const SuccinctSliceTree& slice) {
           return !slice.bcaf_clauses_present() || slice.bcaf_first_child_depth() != bcaf_clause_begin_depth_;
         }))
        throw std::runtime_error("implicit checkpoint has inconsistent BCAF clause payloads");
    }
  }

  [[nodiscard]] std::filesystem::path checkpoint_path() const {
    const std::filesystem::path prefix(options_.savefile);
    std::error_code error;
    const bool directory = std::filesystem::is_directory(prefix, error) || options_.savefile.ends_with('/') || options_.savefile.ends_with('\\');
    if(directory) {
      return prefix / ("save_" + std::to_string(height_));
    }
    return std::filesystem::path(options_.savefile + "_" + std::to_string(height_));
  }

  void save_checkpoint() {
    if(options_.save_mode == SaveMode::None || last_checkpoint_height_ == height_) {
      return;
    }
    const auto path = checkpoint_path();
    auto temporary = path;
    temporary += ".tmp";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
      if(!output) {
        throw std::runtime_error("cannot open checkpoint for writing: " + temporary.string());
      }
      CheckpointWriter writer(output);
      writer.bytes(checkpoint_magic_.data(), checkpoint_magic_.size());
      writer.u32(checkpoint_version_);
      write_config(writer, options_);
      writer.boolean(exhausted_);
      writer.boolean(completion_at_current_row_);
      writer.u64(width_);
      writer.u64(height_);
      writer.u64(slices_.size());
      writer.boolean(implicit_relations_);
      writer.u64(bcaf_clause_begin_depth_);
      for(const auto& slice : slices_) {
        writer.u64(slice.depth());
        writer.u64(slice.node_count());
        writer.vector_u64(slice.checkpoint_levels());
        writer.vector_u64(slice.checkpoint_words());
        writer.vector_u8(slice.bcaf_checkpoint_bytes());
      }
      writer.boolean(pair_gates_ready_);
      writer.u64(pair_gate_depth_);
      writer.u64(pair_gates_.size());
      for(const auto& gate : pair_gates_) {
        writer.u64(gate.bit_count);
        writer.u64(gate.bits.size());
        writer.vector_u64(gate.bits.checkpoint_words());
        writer.u64(gate.index_depth);
        writer.vector_u64(gate.index_starts);
        writer.vector_u64(gate.index_paths);
      }
      writer.finish();
      output.close();
      if(!output) {
        throw std::runtime_error("cannot finish checkpoint: " + temporary.string());
      }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if(error) {
      throw std::runtime_error("cannot replace checkpoint " + path.string() + ": " + error.message());
    }
    last_checkpoint_height_ = height_;
    std::cout << "checkpoint saved: " << path.string() << '\n';
  }

  int finish(int status) {
    if(options_.save_mode != SaveMode::None) {
      save_checkpoint();
    }
    return status;
  }

  int finish_interrupted() {
    if(options_.save_mode == SaveMode::None) {
      std::cout << "interrupt requested after completed row " << height_ << "; exiting without checkpoint\n";
    } else {
      std::cout << "interrupt requested; saving completed row " << height_ << " and exiting\n";
    }
    return finish(130);
  }

  static StartGrid magic_background(const std::string& source, std::size_t height) {
    std::smatch match;
    static const std::regex current(R"(^@?bg\(([0-9]+)\)$)");
    static const std::regex legacy(R"(^@?bg:([0-9]+)$)");
    if(!std::regex_match(source, match, current) && !std::regex_match(source, match, legacy)) {
      return {};
    }
    StartGrid grid;
    grid.width = static_cast<std::size_t>(std::stoull(match[1].str()));
    grid.height = height;
    grid.cells.assign(grid.width * grid.height, 0);
    return grid;
  }

  static std::string read_file(const std::string& path) {
    std::ifstream input(path);
    if(!input) {
      throw std::runtime_error("cannot open start grid: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  static StartGrid parse_rle(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    std::string header;
    std::string body;
    while(std::getline(lines, line)) {
      if(line.empty() || line[0] == '#') {
        continue;
      }
      if(header.empty()) {
        header = line;
      } else {
        body += line;
      }
    }
    std::smatch match;
    static const std::regex header_pattern(R"(^\s*x\s*=\s*([0-9]+)\s*,\s*y\s*=\s*([0-9]+).*$)", std::regex::icase);
    if(!std::regex_match(header, match, header_pattern)) {
      throw std::runtime_error("invalid RLE header");
    }
    StartGrid grid;
    grid.width = static_cast<std::size_t>(std::stoull(match[1].str()));
    grid.height = static_cast<std::size_t>(std::stoull(match[2].str()));
    grid.cells.assign(grid.width * grid.height, 0);
    std::size_t x = 0;
    std::size_t y = 0;
    std::size_t run = 0;
    auto count = [&] {
      const auto value = run == 0 ? 1U : run;
      run = 0;
      return value;
    };
    for(const char raw : body) {
      const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
      if(std::isdigit(static_cast<unsigned char>(ch))) {
        run = run * 10U + static_cast<unsigned>(ch - '0');
      } else if(ch == 'b' || ch == 'o') {
        const auto amount = count();
        for(std::size_t i = 0; i < amount; ++i) {
          if(x >= grid.width || y >= grid.height) {
            throw std::runtime_error("RLE body exceeds declared dimensions");
          }
          grid.cells[y * grid.width + x++] = ch == 'o' ? 1 : 0;
        }
      } else if(ch == '$') {
        y += count();
        x = 0;
      } else if(ch == '!') {
        break;
      } else if(!std::isspace(static_cast<unsigned char>(ch))) {
        throw std::runtime_error("invalid character in RLE body");
      }
    }
    return grid;
  }

  static StartGrid parse_ascii(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::vector<std::int8_t>> rows;
    for(std::string line; std::getline(input, line);) {
      if(!line.empty() && line[0] == '#') {
        continue;
      }
      std::vector<std::int8_t> row;
      for(const char raw : line) {
        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        if(ch == '.' || ch == 'b' || ch == '0')
          row.push_back(0);
        else if(ch == '*' || ch == 'o' || ch == '1')
          row.push_back(1);
        else if(ch == '?')
          row.push_back(-1);
        else if(!std::isspace(static_cast<unsigned char>(ch))) {
          throw std::runtime_error("invalid character in ASCII start grid");
        }
      }
      if(!row.empty())
        rows.push_back(std::move(row));
    }
    if(rows.empty()) {
      throw std::runtime_error("empty ASCII start grid");
    }
    StartGrid grid;
    grid.height = rows.size();
    for(const auto& row : rows)
      grid.width = std::max(grid.width, row.size());
    grid.cells.assign(grid.width * grid.height, 0);
    for(std::size_t y = 0; y < rows.size(); ++y) {
      std::copy(rows[y].begin(), rows[y].end(), grid.cells.begin() + y * grid.width);
    }
    return grid;
  }

  StartGrid load_start() const {
    if(auto grid = magic_background(options_.start, geometry_.short_window()); grid.width != 0) {
      return grid;
    }
    const auto text = read_file(options_.start);
    if(std::regex_search(text, std::regex(R"((^|\n)\s*x\s*=)", std::regex::icase))) {
      return parse_rle(text);
    }
    return parse_ascii(text);
  }

  static std::uint8_t allowed_pair_labels(std::int8_t left, std::int8_t right) {
    std::uint8_t mask = 0;
    for(std::uint8_t a = 0; a < 2; ++a) {
      if(left >= 0 && a != static_cast<std::uint8_t>(left))
        continue;
      for(std::uint8_t b = 0; b < 2; ++b) {
        if(right >= 0 && b != static_cast<std::uint8_t>(right))
          continue;
        mask |= static_cast<std::uint8_t>(1U << ((a << 1U) | b));
      }
    }
    return mask;
  }

  void initialize() {
    const auto start = load_start();
    if(start.width < 3) {
      throw std::runtime_error(geometry_.diagonal() ? "start width must be at least three logical U columns"
                                                    : "start width must be at least three physical columns");
    }
    const auto minimum_height = geometry_.short_window();
    if(start.height < minimum_height) {
      throw std::runtime_error("start height must cover the geometry lookback of " + std::to_string(minimum_height) + " flattened lattice rows");
    }
    if(!geometry_.complete_tile(start.height)) {
      throw std::runtime_error("start height must end at a complete lattice tile (a multiple of " + std::to_string(geometry_.subtile_count) + ")");
    }
    width_ = start.width;
    height_ = start.height;
    slices_.resize(width_ - 1U);
    for(std::size_t x = 0; x + 1 < width_; ++x) {
      for(std::size_t y = 0; y < height_; ++y) {
        slices_[x].append_uniform(allowed_pair_labels(start.at(x, y), start.at(x + 1, y)));
      }
    }
    if(!prune_supported()) {
      slices_.clear();
      pair_gates_.clear();
      pair_gates_ready_ = false;
      bcaf_clause_begin_depth_ = 0;
      exhausted_ = true;
    }
  }

  [[nodiscard]] std::uint8_t history_all_accepts(const std::uint8_t* triples, std::size_t row) const {
    return geometry_acceptance_.interior_mask(triples, row);
  }

  struct PairTransitions {
    // In the original left-label/right-label DFS order, bits 0..1 are the
    // offset within the left child block and bits 2..3 the offset within
    // the right block.
    std::array<std::uint8_t, 8> child_offsets{};
    std::uint8_t present = 0;
  };

  void build_pair_transition_table() {
    for(std::uint8_t left_mask = 0; left_mask < 16; ++left_mask) {
      for(std::uint8_t right_mask = 0; right_mask < 16; ++right_mask) {
        auto& transitions = pair_transitions_[static_cast<std::size_t>(left_mask) | (static_cast<std::size_t>(right_mask) << 4U)];
        for(std::size_t position = 0; position < geometry_pair_triple_order.size(); ++position) {
          const auto triple = geometry_pair_triple_order[position];
          const auto left_label = static_cast<std::uint8_t>(((triple & 1U) << 1U) | ((triple >> 1U) & 1U));
          const auto right_label = static_cast<std::uint8_t>((triple & 0b010U) | ((triple >> 2U) & 1U));
          if((left_mask & (1U << left_label)) == 0 || (right_mask & (1U << right_label)) == 0) {
            continue;
          }
          const auto left_offset = static_cast<std::uint8_t>(std::popcount(static_cast<unsigned>(left_mask & ((1U << left_label) - 1U))));
          const auto right_offset = static_cast<std::uint8_t>(std::popcount(static_cast<unsigned>(right_mask & ((1U << right_label) - 1U))));
          transitions.child_offsets[position] = static_cast<std::uint8_t>(left_offset | (right_offset << 2U));
          transitions.present = static_cast<std::uint8_t>(transitions.present | (1U << position));
        }
      }
    }
  }

  bool boundary_dfs(const SuccinctSliceTree& tree,
                    Node node,
                    std::size_t depth,
                    EdgeMode edge,
                    bool left_side,
                    std::vector<std::uint8_t>& history,
                    PackedTags& tags) {
    ++boundary_states_;
    if(depth == tree.depth()) {
      tags.set(node);
      return true;
    }
    bool any = false;
    const auto children = tree.child_block(node);
    auto child = children.first;
    const auto side = left_side ? Side::Left : Side::Right;
    for(std::uint8_t label = 0; label < 4; ++label) {
      if((children.mask & (1U << label)) == 0)
        continue;
      const auto next = child++;
      const auto step = geometry_acceptance_.boundary_step(history.data(), depth, edge, side, label);
      if(!step.accepted)
        continue;
      history[depth] = step.history_label;
      if(boundary_dfs(tree, next, depth + 1, edge, left_side, history, tags)) {
        any = true;
      }
    }
    if(any)
      tags.set(node);
    return any;
  }

  void mark_boundary(const SuccinctSliceTree& tree, EdgeMode edge, bool left_side, PackedTags& tags) {
    tags.clear();
    std::vector<std::uint8_t> history(tree.depth());
    boundary_dfs(tree, 0, 0, edge, left_side, history, tags);
  }

  struct PairPathSummary {
    std::size_t first_left_nonzero = std::numeric_limits<std::size_t>::max();
    std::size_t last_left_nonzero = std::numeric_limits<std::size_t>::max();
  };

  // Completion propagation needs only the class of the most recent nonzero
  // left label, not its exact depth.  The state is monotone along a path:
  // uninteresting -> interesting -> invalid.
  enum class CompletionPathState : std::uint8_t {
    ValidUninteresting,
    ValidInteresting,
    Invalid,
  };

  [[nodiscard]] static constexpr CompletionPathState advance_completion_path(CompletionPathState state,
                                                                              std::uint8_t triple,
                                                                              std::size_t depth,
                                                                              std::size_t long_start,
                                                                              std::size_t short_start) noexcept {
    if(state == CompletionPathState::Invalid || (triple & 0b011U) == 0)
      return state;
    if(depth >= short_start)
      return CompletionPathState::Invalid;
    if(depth >= long_start)
      return CompletionPathState::ValidInteresting;
    return state;
  }

#ifndef NDEBUG
  static void verify_completion_path_state(CompletionPathState state,
                                           const std::uint8_t* history,
                                           std::size_t depth,
                                           std::size_t long_start,
                                           std::size_t short_start) {
    auto last_nonzero = std::numeric_limits<std::size_t>::max();
    for(std::size_t row = 0; row < depth; ++row) {
      if((history[row] & 0b011U) != 0)
        last_nonzero = row;
    }
    auto expected = CompletionPathState::ValidUninteresting;
    if(last_nonzero != std::numeric_limits<std::size_t>::max()) {
      if(last_nonzero >= short_start)
        expected = CompletionPathState::Invalid;
      else if(last_nonzero >= long_start)
        expected = CompletionPathState::ValidInteresting;
    }
    if(state != expected)
      throw std::logic_error("compact completion path state diverged from exact summary");
  }
#endif

  enum class PairGateLocation : std::uint8_t {
    None,
    ParentOfLeaf,
    Leaf,
  };

  [[nodiscard]] static inline std::uint8_t expand_left_edges(std::uint8_t leaves) noexcept {
    return detail::expand_left_edges(leaves);
  }

  [[nodiscard]] static constexpr std::uint8_t expand_right_edges(std::uint8_t leaves) noexcept {
    leaves &= 0x0fU;
    return static_cast<std::uint8_t>(leaves | (leaves << 4U));
  }

  [[nodiscard]] static inline std::uint8_t project_left_edges(std::uint8_t edges) noexcept {
    const auto paired = static_cast<std::uint8_t>(edges | (edges >> 1U));
#if defined(__BMI2__)
    return static_cast<std::uint8_t>(_pext_u32(paired, 0x55U));
#else
    return static_cast<std::uint8_t>((paired & 0x01U) | ((paired >> 1U) & 0x02U) | ((paired >> 2U) & 0x04U) |
                                     ((paired >> 3U) & 0x08U));
#endif
  }

  [[nodiscard]] static constexpr std::uint8_t project_right_edges(std::uint8_t edges) noexcept {
    return static_cast<std::uint8_t>((edges | (edges >> 4U)) & 0x0fU);
  }

  template <class Tags> [[nodiscard]] static std::uint8_t get_leaf_four(const Tags& tags, Node first) noexcept {
    return tags.get_4(first);
  }

  template <class Tags> static void set_leaf_four(Tags& tags, Node first, std::uint8_t mask) noexcept {
    tags.or_4(first, mask);
  }

  template <class Tags> static void atomic_set_leaf_four(Tags& tags, Node first, std::uint8_t mask) noexcept {
    tags.atomic_or_4(first, mask);
  }

  [[nodiscard]] bool historical_pair_clause_active(std::size_t child_depth) const noexcept {
    return implicit_relations_ && bcaf_clause_begin_depth_ != 0 && child_depth >= bcaf_clause_begin_depth_ && child_depth <= pair_gate_depth_;
  }

  // The persistent byte is already scattered to raw child labels.
  [[nodiscard]] std::uint8_t filter_historical_pair_clause(const SuccinctSliceTree& left,
                                                            const SuccinctSliceTree& right,
                                                            Node left_parent,
                                                            Node right_parent,
                                                            std::size_t child_depth,
                                                            std::uint8_t active) const noexcept {
    if(active == 0 || !historical_pair_clause_active(child_depth))
      return active;
    const auto left_clauses = left.bcaf_child_clauses_unchecked(left_parent);
    const auto right_clauses = right.bcaf_child_clauses_unchecked(right_parent);
    return static_cast<std::uint8_t>(active & (expand_left_edges(left_clauses & 0x0fU) | expand_right_edges(right_clauses >> 4U)));
  }

  template <bool CountStats, bool VisitRejected, bool TrackSummary, class BatchCallback>
  void candidate_pair_batch_dfs(const SuccinctSliceTree& left,
                                const SuccinctSliceTree& right,
                                Node left_node,
                                Node right_node,
                                std::size_t depth,
                                std::vector<std::uint8_t>& history,
                                const PairGate& parent_gate,
                                std::uint64_t& parent_gate_cursor,
                                PairPathSummary summary,
                                BatchCallback& batch_callback) {
    if constexpr(CountStats)
      ++pair_states_;
    const auto tree_depth = left.depth();
    if(depth + 1U == tree_depth) {
      if(parent_gate_cursor >= parent_gate.size()) {
        throw std::logic_error("pair gate ended before its DFS enumeration");
      }
      const bool ancestry_allowed = parent_gate.get(parent_gate_cursor++);
      if constexpr(!VisitRejected) {
        if(!ancestry_allowed)
          return;
      }
      const auto active = history_all_accepts(history.data(), depth);
      if constexpr(CountStats) {
        pair_states_ += std::popcount(static_cast<unsigned>(active));
        if constexpr(VisitRejected) {
          pair_leaves_ += std::popcount(static_cast<unsigned>(active));
        } else if(ancestry_allowed) {
          pair_leaves_ += std::popcount(static_cast<unsigned>(active));
        }
      }
      if(active != 0) {
        batch_callback(left.expanded_leaf_child_block(left_node).first, right.expanded_leaf_child_block(right_node).first, active, ancestry_allowed, summary,
                       depth);
      }
      return;
    }

    const auto left_children = left.child_block(left_node);
    const auto right_children = right.child_block(right_node);
    const auto acceptor = history_all_accepts(history.data(), depth);
    const auto& transitions = pair_transitions_[static_cast<std::size_t>(left_children.mask) | (static_cast<std::size_t>(right_children.mask) << 4U)];
    auto active = static_cast<std::uint8_t>(acceptor & transitions.present);
    active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
    while(active != 0) {
      const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(active)));
      const auto triple = geometry_pair_triple_order[position];
      const auto offsets = transitions.child_offsets[position];
      history[depth] = triple;
      auto next_summary = summary;
      if constexpr(TrackSummary) {
        if((triple & 0b011U) != 0) {
          if(next_summary.first_left_nonzero == std::numeric_limits<std::size_t>::max()) {
            next_summary.first_left_nonzero = depth;
          }
          next_summary.last_left_nonzero = depth;
        }
      }
      candidate_pair_batch_dfs<CountStats, VisitRejected, TrackSummary>(left, right, left_children.first + (offsets & 0b11U),
                                                                        right_children.first + ((offsets >> 2U) & 0b11U), depth + 1U, history,
                                                                        parent_gate, parent_gate_cursor, next_summary, batch_callback);
      active = static_cast<std::uint8_t>(active & (active - 1U));
    }
  }

  template <bool VisitRejected = true, bool TrackSummary = false, class BatchCallback>
  void walk_candidate_pair_batches(std::size_t position, BatchCallback&& batch_callback) {
    const auto& left = slices_.at(position);
    const auto& right = slices_.at(position + 1U);
    if(!expanded_uniform_tail_ || !pair_gates_ready_ || pair_gate_depth_ + 1U != left.depth() || left.depth() != right.depth()) {
      throw std::logic_error("batched pair walk requires a freshly expanded parent gate");
    }
    const auto& parent_gate = pair_gates_.at(position);
    std::uint64_t cursor = 0;
    std::vector<std::uint8_t> history(left.depth());
    auto callback = std::forward<BatchCallback>(batch_callback);
    if(options_.verbose) {
      candidate_pair_batch_dfs<true, VisitRejected, TrackSummary>(left, right, 0, 0, 0, history, parent_gate, cursor, PairPathSummary{}, callback);
    } else {
      candidate_pair_batch_dfs<false, VisitRejected, TrackSummary>(left, right, 0, 0, 0, history, parent_gate, cursor, PairPathSummary{}, callback);
    }
    if(cursor != parent_gate.size()) {
      throw std::logic_error("pair gate has bits beyond its batched DFS enumeration");
    }
  }

  template <bool CountStats, bool VisitRejected, class BatchCallback>
  void candidate_pair_batch_completion_dfs(const SuccinctSliceTree& left,
                                            const SuccinctSliceTree& right,
                                            Node left_node,
                                            Node right_node,
                                            std::size_t depth,
                                            std::vector<std::uint8_t>& history,
                                            const PairGate& parent_gate,
                                            std::uint64_t& parent_gate_cursor,
                                            CompletionPathState completion_state,
                                            std::size_t long_start,
                                            std::size_t short_start,
                                            BatchCallback& batch_callback) {
    if constexpr(CountStats)
      ++pair_states_;
    const auto tree_depth = left.depth();
    if(depth + 1U == tree_depth) {
      if(parent_gate_cursor >= parent_gate.size())
        throw std::logic_error("pair gate ended before its DFS enumeration");
      const bool ancestry_allowed = parent_gate.get(parent_gate_cursor++);
      if constexpr(!VisitRejected) {
        if(!ancestry_allowed)
          return;
      }
      const auto active = history_all_accepts(history.data(), depth);
      if constexpr(CountStats) {
        pair_states_ += std::popcount(static_cast<unsigned>(active));
        if constexpr(VisitRejected) {
          pair_leaves_ += std::popcount(static_cast<unsigned>(active));
        } else if(ancestry_allowed) {
          pair_leaves_ += std::popcount(static_cast<unsigned>(active));
        }
      }
      if(active != 0) {
#ifndef NDEBUG
        verify_completion_path_state(completion_state, history.data(), depth, long_start, short_start);
#endif
        batch_callback(left.expanded_leaf_child_block(left_node).first, right.expanded_leaf_child_block(right_node).first, active, ancestry_allowed,
                       completion_state, depth);
      }
      return;
    }

    const auto left_children = left.child_block(left_node);
    const auto right_children = right.child_block(right_node);
    const auto acceptor = history_all_accepts(history.data(), depth);
    const auto& transitions = pair_transitions_[static_cast<std::size_t>(left_children.mask) | (static_cast<std::size_t>(right_children.mask) << 4U)];
    auto active = static_cast<std::uint8_t>(acceptor & transitions.present);
    active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
    while(active != 0) {
      const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(active)));
      const auto triple = geometry_pair_triple_order[position];
      const auto offsets = transitions.child_offsets[position];
      history[depth] = triple;
      const auto next_state = advance_completion_path(completion_state, triple, depth, long_start, short_start);
      candidate_pair_batch_completion_dfs<CountStats, VisitRejected>(
          left, right, left_children.first + (offsets & 0b11U), right_children.first + ((offsets >> 2U) & 0b11U), depth + 1U, history, parent_gate,
          parent_gate_cursor, next_state, long_start, short_start, batch_callback);
      active = static_cast<std::uint8_t>(active & (active - 1U));
    }
  }

  template <bool VisitRejected = true, class BatchCallback>
  void walk_candidate_pair_batches_completion(std::size_t position,
                                              std::size_t long_start,
                                              std::size_t short_start,
                                              BatchCallback&& batch_callback) {
    const auto& left = slices_.at(position);
    const auto& right = slices_.at(position + 1U);
    if(!expanded_uniform_tail_ || !pair_gates_ready_ || pair_gate_depth_ + 1U != left.depth() || left.depth() != right.depth())
      throw std::logic_error("batched completion walk requires a freshly expanded parent gate");
    const auto& parent_gate = pair_gates_.at(position);
    std::uint64_t cursor = 0;
    std::vector<std::uint8_t> history(left.depth());
    auto callback = std::forward<BatchCallback>(batch_callback);
    if(options_.verbose) {
      candidate_pair_batch_completion_dfs<true, VisitRejected>(left, right, 0, 0, 0, history, parent_gate, cursor,
                                                                CompletionPathState::ValidUninteresting, long_start, short_start, callback);
    } else {
      candidate_pair_batch_completion_dfs<false, VisitRejected>(left, right, 0, 0, 0, history, parent_gate, cursor,
                                                                 CompletionPathState::ValidUninteresting, long_start, short_start, callback);
    }
    if(cursor != parent_gate.size())
      throw std::logic_error("pair gate has bits beyond its batched completion DFS enumeration");
  }

  struct GateRangeFrame {
    Node left_first = 0;
    Node right_first = 0;
    PairPathSummary summary_before;
    std::uint8_t remaining = 0;
    std::uint8_t transition_key = 0;
  };

  struct CompactGateRangeFrame {
    Node left_first = 0;
    Node right_first = 0;
    std::uint8_t remaining = 0;
    std::uint8_t transition_key = 0;
    CompletionPathState completion_before : 2 = CompletionPathState::ValidUninteresting;
  };

  static_assert(sizeof(CompactGateRangeFrame) == 3U * sizeof(Node));

  struct GateRangeScratch {
    std::vector<std::uint8_t> history;
    std::vector<GateRangeFrame> frames;
    std::vector<CompactGateRangeFrame> compact_frames;
  };

  [[nodiscard]] static constexpr std::uint8_t triple_position(std::uint8_t triple) noexcept {
    return static_cast<std::uint8_t>(((triple & 0b001U) << 2U) | (triple & 0b010U) | ((triple & 0b100U) >> 2U));
  }

  template <bool VisitRejected, bool TrackSummary, bool TrackCompletion, bool ImplicitGate, bool CurrentLeaves, class BatchCallback>
  void walk_indexed_gate_range(std::size_t position,
                               std::size_t checkpoint,
                               std::size_t worker,
                               std::size_t long_start,
                               std::size_t short_start,
                               BatchCallback& batch_callback) {
    static_assert(!TrackSummary || !TrackCompletion);
    static_assert(!CurrentLeaves || (!VisitRejected && TrackSummary && !TrackCompletion));
    const auto& left = slices_[position];
    const auto& right = slices_[position + 1U];
    const auto& gate = pair_gates_[position];
    const auto parent_depth = static_cast<std::size_t>(gate.index_depth);
    auto& scratch = parallel_gate_scratch_[worker];
    auto& history = scratch.history;
    auto& frames = scratch.frames;
    auto& compact_frames = scratch.compact_frames;
    Node left_node = 0;
    Node right_node = 0;
    PairPathSummary summary;
    auto completion_state = CompletionPathState::ValidUninteresting;

    auto update_path_state = [&](std::uint8_t triple, std::size_t depth) {
      if constexpr(TrackSummary) {
        if((triple & 0b011U) != 0) {
          if(summary.first_left_nonzero == std::numeric_limits<std::size_t>::max())
            summary.first_left_nonzero = depth;
          summary.last_left_nonzero = depth;
        }
      } else if constexpr(TrackCompletion) {
        completion_state = advance_completion_path(completion_state, triple, depth, long_start, short_start);
      }
    };
    auto select_branch = [&](std::size_t depth, std::uint8_t position_in_order, std::uint8_t active, const SuccinctSliceTree::ChildBlock& left_children,
                             const SuccinctSliceTree::ChildBlock& right_children) {
      const auto selected = static_cast<std::uint8_t>(1U << position_in_order);
      if((active & selected) == 0)
        throw std::logic_error("pair-gate index path names an absent branch");
      const auto key = static_cast<std::uint8_t>(left_children.mask | (right_children.mask << 4U));
      const auto remaining = static_cast<std::uint8_t>(active & ~((selected << 1U) - 1U));
      if constexpr(TrackSummary) {
        frames[depth] = GateRangeFrame{left_children.first, right_children.first, summary, remaining, key};
      } else {
        auto& frame = compact_frames[depth];
        frame.left_first = left_children.first;
        frame.right_first = right_children.first;
        frame.remaining = remaining;
        frame.transition_key = key;
        if constexpr(TrackCompletion)
          frame.completion_before = completion_state;
      }
      const auto& transitions = pair_transitions_[key];
      const auto offsets = transitions.child_offsets[position_in_order];
      const auto triple = geometry_pair_triple_order[position_in_order];
      history[depth] = triple;
      update_path_state(triple, depth);
      left_node = left_children.first + (offsets & 0b11U);
      right_node = right_children.first + ((offsets >> 2U) & 0b11U);
    };

    for(std::size_t depth = 0; depth < parent_depth; ++depth) {
      const auto left_children = left.child_block(left_node);
      const auto right_children = right.child_block(right_node);
      const auto acceptor = history_all_accepts(history.data(), depth);
      const auto key = static_cast<std::uint8_t>(left_children.mask | (right_children.mask << 4U));
      auto active = static_cast<std::uint8_t>(acceptor & pair_transitions_[key].present);
      active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
      const auto triple = gate.indexed_triple(checkpoint, depth);
      select_branch(depth, triple_position(triple), active, left_children, right_children);
    }

    auto advance = [&]() {
      auto depth = parent_depth;
      while(depth > 0) {
        const auto parent = depth - 1U;
        auto& remaining = [&]() -> std::uint8_t& {
          if constexpr(TrackSummary)
            return frames[parent].remaining;
          else
            return compact_frames[parent].remaining;
        }();
        if(remaining == 0) {
          depth = parent;
          continue;
        }
        const auto branch = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
        remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
        if constexpr(TrackSummary)
          summary = frames[parent].summary_before;
        else if constexpr(TrackCompletion)
          completion_state = compact_frames[parent].completion_before;
        const auto transition_key = [&]() {
          if constexpr(TrackSummary)
            return frames[parent].transition_key;
          else
            return compact_frames[parent].transition_key;
        }();
        const auto offsets = pair_transitions_[transition_key].child_offsets[branch];
        const auto triple = geometry_pair_triple_order[branch];
        history[parent] = triple;
        update_path_state(triple, parent);
        if constexpr(TrackSummary) {
          left_node = frames[parent].left_first + (offsets & 0b11U);
          right_node = frames[parent].right_first + ((offsets >> 2U) & 0b11U);
        } else {
          left_node = compact_frames[parent].left_first + (offsets & 0b11U);
          right_node = compact_frames[parent].right_first + ((offsets >> 2U) & 0b11U);
        }
        depth = parent + 1U;

        while(depth < parent_depth) {
          const auto left_children = left.child_block(left_node);
          const auto right_children = right.child_block(right_node);
          const auto acceptor = history_all_accepts(history.data(), depth);
          const auto key = static_cast<std::uint8_t>(left_children.mask | (right_children.mask << 4U));
          auto active = static_cast<std::uint8_t>(acceptor & pair_transitions_[key].present);
          active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
          if(active == 0)
            break;
          const auto first = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(active)));
          select_branch(depth, first, active, left_children, right_children);
          ++depth;
        }
        if(depth == parent_depth)
          return true;
      }
      return false;
    };

    auto cursor = gate.index_starts[checkpoint];
    const auto end = checkpoint + 1U < gate.index_starts.size() ? gate.index_starts[checkpoint + 1U] : gate.size();
    const auto left_leaf_bias = [&]() {
      if constexpr(CurrentLeaves)
        return Node{0};
      else
        return left.level_begin(parent_depth + 1U) - 4U * left.level_begin(parent_depth);
    }();
    const auto right_leaf_bias = [&]() {
      if constexpr(CurrentLeaves)
        return Node{0};
      else
        return right.level_begin(parent_depth + 1U) - 4U * right.level_begin(parent_depth);
    }();
    auto gate_word_index = static_cast<std::uint64_t>(-1);
    std::uint64_t gate_word = ~std::uint64_t{0};
    while(cursor < end) {
      if constexpr(!ImplicitGate) {
        if((cursor >> 6U) != gate_word_index) {
          gate_word_index = cursor >> 6U;
          gate_word = gate.bits.word(static_cast<std::size_t>(gate_word_index));
        }
      }
      const bool ancestry_allowed =
          ImplicitGate || ((gate_word >> (cursor & 63U)) & 1U) != 0;
      ++cursor;
      if constexpr(CurrentLeaves) {
        if(ancestry_allowed)
          batch_callback(left_node, right_node, history, summary, checkpoint, worker);
      } else {
        const auto active = history_all_accepts(history.data(), parent_depth);
#ifndef NDEBUG
        if constexpr(TrackCompletion)
          verify_completion_path_state(completion_state, history.data(), parent_depth, long_start, short_start);
#endif
        if constexpr(VisitRejected) {
          if(active != 0) {
            if constexpr(TrackCompletion) {
              batch_callback(left_leaf_bias + 4U * left_node, right_leaf_bias + 4U * right_node, active, ancestry_allowed, completion_state, parent_depth,
                             history, checkpoint, worker);
            } else {
              batch_callback(left_leaf_bias + 4U * left_node, right_leaf_bias + 4U * right_node, active, ancestry_allowed, summary, parent_depth, history,
                             checkpoint, worker);
            }
          }
        } else if(ancestry_allowed) {
          if(active != 0) {
            if constexpr(TrackCompletion) {
              batch_callback(left_leaf_bias + 4U * left_node, right_leaf_bias + 4U * right_node, active, true, completion_state, parent_depth, history,
                             checkpoint, worker);
            } else {
              batch_callback(left_leaf_bias + 4U * left_node, right_leaf_bias + 4U * right_node, active, true, summary, parent_depth, history, checkpoint,
                             worker);
            }
          }
        }
      }
      if(cursor < end && !advance())
        throw std::logic_error("pair-gate indexed range ended before its gate interval");
    }
  }

  template <bool VisitRejected, bool TrackSummary, bool TrackCompletion, class BatchCallback> struct IndexedGateWalkContext {
    Solver* solver = nullptr;
    std::size_t position = 0;
    std::size_t long_start = 0;
    std::size_t short_start = 0;
    BatchCallback* callback = nullptr;
  };

  template <bool VisitRejected, bool TrackSummary, bool TrackCompletion, bool ImplicitGate, bool CurrentLeaves, class BatchCallback>
  static void execute_indexed_gate_range(void* opaque, std::size_t checkpoint, std::size_t worker) {
    auto& context = *static_cast<IndexedGateWalkContext<VisitRejected, TrackSummary, TrackCompletion, BatchCallback>*>(opaque);
    context.solver->template walk_indexed_gate_range<VisitRejected, TrackSummary, TrackCompletion, ImplicitGate, CurrentLeaves>(
        context.position, checkpoint, worker, context.long_start, context.short_start, *context.callback);
  }

  struct BcafReifyContext {
    Solver* solver = nullptr;
    std::vector<TagPair>* normal = nullptr;
    std::vector<LeafTagPair>* witness = nullptr;
    std::vector<std::uint8_t>* retained = nullptr;
  };

  static void execute_bcaf_reify(void* opaque, std::size_t position, std::size_t) {
    auto& context = *static_cast<BcafReifyContext*>(opaque);
    auto& tree = context.solver->slices_[position];
    auto& normal = (*context.normal)[position];
    auto& witness = (*context.witness)[position];
    auto& keep = normal[0];
    for(Node leaf = tree.leaf_begin(); leaf < tree.leaf_end(); ++leaf) {
      keep.set(leaf, keep.get(leaf) && normal[1].get(leaf) && (witness[0].get(leaf) || witness[1].get(leaf)));
    }
    (*context.retained)[position] = static_cast<std::uint8_t>(tree.reify(keep));
  }

  struct BcafKeepRange {
    std::size_t position = 0;
    Node begin = 0;
    Node end = 0;
  };

  struct BcafKeepContext {
    Solver* solver = nullptr;
    std::vector<TagPair>* normal = nullptr;
    std::vector<LeafTagPair>* witness = nullptr;
    const std::vector<BcafKeepRange>* ranges = nullptr;
  };

  static void execute_bcaf_keep_range(void* opaque, std::size_t task, std::size_t) {
    auto& context = *static_cast<BcafKeepContext*>(opaque);
    const auto range = (*context.ranges)[task];
    auto& tree = context.solver->slices_[range.position];
    auto& normal = (*context.normal)[range.position];
    auto& witness = (*context.witness)[range.position];
    auto& keep = normal[0];
    const auto leaf_begin = tree.leaf_begin();
    const auto parent_begin = tree.level_begin(tree.depth() - 1U);
    for(Node leaf = range.begin; leaf < range.end; ++leaf) {
      if(context.solver->expanded_uniform_tail_ && ((leaf - leaf_begin) & 3U) == 0) {
        const auto parent = parent_begin + (leaf - leaf_begin) / 4U;
        const auto prefix_four = witness[0].get_4(leaf);
        const auto suffix_four = witness[1].get_4(leaf);
        tree.set_bcaf_child_clauses_unchecked(parent, static_cast<std::uint8_t>(prefix_four | (suffix_four << 4U)));
      }
      const bool prefix = witness[0].get(leaf);
      const bool suffix = witness[1].get(leaf);
      keep.set(leaf, keep.get(leaf) && normal[1].get(leaf) && (prefix || suffix));
    }
  }

  template <bool VisitRejected = true, bool TrackSummary = false, bool ImplicitGate = false, class BatchCallback>
  void walk_candidate_pair_batches_parallel(std::size_t position, BatchCallback&& batch_callback) {
    const auto& gate = pair_gates_.at(position);
    if(!expanded_uniform_tail_ || !gate.index_ready(pair_gate_depth_) || pair_gate_depth_ + 1U != slices_[position].depth() ||
       slices_[position].depth() != slices_[position + 1U].depth()) {
      throw std::logic_error("parallel pair walk requires an indexed freshly expanded parent gate");
    }
    if constexpr(ImplicitGate) {
      if(gate.bits.size() != 0)
        throw std::logic_error("implicit parallel pair walk has a dense gate payload");
    }
    const auto workers = static_cast<std::size_t>(options_.worker_count);
    if(parallel_gate_scratch_.size() != workers) {
      parallel_gate_scratch_.resize(workers);
    }
    const auto required_depth = static_cast<std::size_t>(gate.index_depth);
    for(auto& scratch : parallel_gate_scratch_) {
      if(scratch.history.size() < required_depth)
        scratch.history.resize(required_depth);
      if constexpr(TrackSummary) {
        if(scratch.frames.size() < required_depth)
          scratch.frames.resize(required_depth);
      } else {
        if(scratch.compact_frames.size() < required_depth)
          scratch.compact_frames.resize(required_depth);
      }
    }
    auto callback = std::forward<BatchCallback>(batch_callback);
    IndexedGateWalkContext<VisitRejected, TrackSummary, false, decltype(callback)> context{this, position, 0, 0, &callback};
    execute_indexed_tasks(gate.index_starts.size(), options_.worker_count, &context,
                          &execute_indexed_gate_range<VisitRejected, TrackSummary, false, ImplicitGate, false, decltype(callback)>);
  }

  template <bool VisitRejected = true, bool ImplicitGate = false, class BatchCallback>
  void walk_candidate_pair_batches_parallel_completion(std::size_t position,
                                                       std::size_t long_start,
                                                       std::size_t short_start,
                                                       BatchCallback&& batch_callback) {
    const auto& gate = pair_gates_.at(position);
    if(!expanded_uniform_tail_ || !gate.index_ready(pair_gate_depth_) || pair_gate_depth_ + 1U != slices_[position].depth() ||
       slices_[position].depth() != slices_[position + 1U].depth()) {
      throw std::logic_error("parallel completion walk requires an indexed freshly expanded parent gate");
    }
    if constexpr(ImplicitGate) {
      if(gate.bits.size() != 0)
        throw std::logic_error("implicit parallel completion walk has a dense gate payload");
    }
    const auto workers = static_cast<std::size_t>(options_.worker_count);
    if(parallel_gate_scratch_.size() != workers)
      parallel_gate_scratch_.resize(workers);
    const auto required_depth = static_cast<std::size_t>(gate.index_depth);
    for(auto& scratch : parallel_gate_scratch_) {
      if(scratch.history.size() < required_depth)
        scratch.history.resize(required_depth);
      if(scratch.compact_frames.size() < required_depth)
        scratch.compact_frames.resize(required_depth);
    }
    auto callback = std::forward<BatchCallback>(batch_callback);
    IndexedGateWalkContext<VisitRejected, false, true, decltype(callback)> context{this, position, long_start, short_start, &callback};
    execute_indexed_tasks(gate.index_starts.size(), options_.worker_count, &context,
                          &execute_indexed_gate_range<VisitRejected, false, true, ImplicitGate, false, decltype(callback)>);
  }

  template <class LeafCallback> void walk_current_edges_parallel(std::size_t position, LeafCallback&& leaf_callback) {
    const auto& gate = pair_gates_.at(position);
    if(!gate.index_ready(pair_gate_depth_) || pair_gate_depth_ != slices_[position].depth() ||
       slices_[position].depth() != slices_[position + 1U].depth()) {
      throw std::logic_error("parallel current-edge walk requires an indexed leaf gate");
    }
    const auto workers = static_cast<std::size_t>(options_.worker_count);
    if(parallel_gate_scratch_.size() != workers)
      parallel_gate_scratch_.resize(workers);
    const auto required_depth = static_cast<std::size_t>(gate.index_depth);
    for(auto& scratch : parallel_gate_scratch_) {
      if(scratch.history.size() < required_depth)
        scratch.history.resize(required_depth);
      if(scratch.frames.size() < required_depth)
        scratch.frames.resize(required_depth);
    }
    auto callback = std::forward<LeafCallback>(leaf_callback);
    auto run = [&]<bool ImplicitGate>() {
      if constexpr(ImplicitGate) {
        if(gate.bits.size() != 0)
          throw std::logic_error("implicit current-edge walk has a dense gate payload");
      }
      IndexedGateWalkContext<false, true, false, decltype(callback)> context{this, position, 0, 0, &callback};
      execute_indexed_tasks(gate.index_starts.size(), options_.worker_count, &context,
                            &execute_indexed_gate_range<false, true, false, ImplicitGate, true, decltype(callback)>);
    };
    if(implicit_relations_)
      run.template operator()<true>();
    else
      run.template operator()<false>();
  }

  [[nodiscard]] bool can_walk_candidate_pair_batches_parallel(std::size_t position) const noexcept {
    return options_.worker_count > 1 && !options_.verbose && expanded_uniform_tail_ && position < pair_gates_.size() &&
           pair_gates_[position].index_ready(pair_gate_depth_);
  }

  [[nodiscard]] bool can_walk_current_edges_parallel(std::size_t position) const noexcept {
    return options_.worker_count > 1 && !options_.verbose && pair_gates_ready_ && position < pair_gates_.size() &&
           pair_gate_depth_ == slices_[position].depth() && slices_[position].depth() == slices_[position + 1U].depth() &&
           pair_gates_[position].index_ready(pair_gate_depth_);
  }

  template <bool CountStats, bool VisitRejected, bool TrackSummary, PairGateLocation GateLocation, class LeafCallback>
  void candidate_pair_dfs(const SuccinctSliceTree& left,
                          const SuccinctSliceTree& right,
                          Node left_node,
                          Node right_node,
                          std::size_t depth,
                          std::vector<std::uint8_t>& history,
                          const PairGate* parent_gate,
                          std::uint64_t& parent_gate_cursor,
                          bool ancestry_allowed,
                          PairPathSummary summary,
                          LeafCallback& leaf_callback) {
    if constexpr(CountStats)
      ++pair_states_;
    const auto tree_depth = left.depth();
    if constexpr(GateLocation == PairGateLocation::ParentOfLeaf) {
      if(depth + 1U == tree_depth) {
        if(parent_gate_cursor >= parent_gate->size()) {
          throw std::logic_error("pair gate ended before its DFS enumeration");
        }
        ancestry_allowed = parent_gate->get(parent_gate_cursor++);
        if constexpr(!VisitRejected) {
          if(!ancestry_allowed)
            return;
        }
      }
    }
    if(depth == tree_depth) {
      if constexpr(CountStats)
        ++pair_leaves_;
      if constexpr(TrackSummary) {
        leaf_callback(left_node, right_node, history, ancestry_allowed, summary);
      } else {
        leaf_callback(left_node, right_node, history, ancestry_allowed);
      }
      return;
    }


    const bool children_are_leaves = depth + 1U == tree_depth;
    if(expanded_uniform_tail_ && children_are_leaves) {
      const auto left_first = left.expanded_leaf_child_block(left_node).first;
      const auto right_first = right.expanded_leaf_child_block(right_node).first;
      auto active = history_all_accepts(history.data(), depth);
      active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
      while(active != 0) {
        const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(active)));
        const auto triple = geometry_pair_triple_order[position];
        history[depth] = triple;
        auto next_summary = summary;
        if constexpr(TrackSummary) {
          if((triple & 0b011U) != 0) {
            if(next_summary.first_left_nonzero == std::numeric_limits<std::size_t>::max()) {
              next_summary.first_left_nonzero = depth;
            }
            next_summary.last_left_nonzero = depth;
          }
        }
        bool leaf_allowed = ancestry_allowed;
        if constexpr(GateLocation == PairGateLocation::Leaf) {
          if(parent_gate_cursor >= parent_gate->size()) {
            throw std::logic_error("pair gate ended before its DFS enumeration");
          }
          leaf_allowed = parent_gate->get(parent_gate_cursor++);
        }
        if constexpr(CountStats)
          ++pair_states_;
        if constexpr(VisitRejected) {
          if constexpr(CountStats)
            ++pair_leaves_;
          if constexpr(TrackSummary) {
            leaf_callback(left_first + (position >> 1U), right_first + (position & 0b11U), history, leaf_allowed, next_summary);
          } else {
            leaf_callback(left_first + (position >> 1U), right_first + (position & 0b11U), history, leaf_allowed);
          }
        } else if(leaf_allowed) {
          if constexpr(CountStats)
            ++pair_leaves_;
          if constexpr(TrackSummary) {
            leaf_callback(left_first + (position >> 1U), right_first + (position & 0b11U), history, true, next_summary);
          } else {
            leaf_callback(left_first + (position >> 1U), right_first + (position & 0b11U), history, true);
          }
        }
        active = static_cast<std::uint8_t>(active & (active - 1U));
      }
      return;
    }

    const auto left_children = left.child_block(left_node);
    const auto right_children = right.child_block(right_node);
    const auto acceptor = history_all_accepts(history.data(), depth);
    const auto& transitions = pair_transitions_[static_cast<std::size_t>(left_children.mask) | (static_cast<std::size_t>(right_children.mask) << 4U)];
    auto active = static_cast<std::uint8_t>(acceptor & transitions.present);
    active = filter_historical_pair_clause(left, right, left_node, right_node, depth + 1U, active);
    while(active != 0) {
      const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(active)));
      const auto triple = geometry_pair_triple_order[position];
      const auto offsets = transitions.child_offsets[position];
      history[depth] = triple;
      auto next_summary = summary;
      if constexpr(TrackSummary) {
        if((triple & 0b011U) != 0) {
          if(next_summary.first_left_nonzero == std::numeric_limits<std::size_t>::max()) {
            next_summary.first_left_nonzero = depth;
          }
          next_summary.last_left_nonzero = depth;
        }
      }
      const auto next_left = left_children.first + (offsets & 0b11U);
      const auto next_right = right_children.first + ((offsets >> 2U) & 0b11U);
      if(children_are_leaves) {
        bool leaf_allowed = ancestry_allowed;
        if constexpr(GateLocation == PairGateLocation::Leaf) {
          if(parent_gate_cursor >= parent_gate->size()) {
            throw std::logic_error("pair gate ended before its DFS enumeration");
          }
          leaf_allowed = parent_gate->get(parent_gate_cursor++);
        }
        if constexpr(CountStats)
          ++pair_states_;
        if constexpr(VisitRejected) {
          if constexpr(CountStats)
            ++pair_leaves_;
          if constexpr(TrackSummary) {
            leaf_callback(next_left, next_right, history, leaf_allowed, next_summary);
          } else {
            leaf_callback(next_left, next_right, history, leaf_allowed);
          }
        } else if(leaf_allowed) {
          if constexpr(CountStats)
            ++pair_leaves_;
          if constexpr(TrackSummary) {
            leaf_callback(next_left, next_right, history, true, next_summary);
          } else {
            leaf_callback(next_left, next_right, history, true);
          }
        }
      } else {
        candidate_pair_dfs<CountStats, VisitRejected, TrackSummary, GateLocation>(left, right, next_left, next_right, depth + 1, history, parent_gate,
                                                                                  parent_gate_cursor, ancestry_allowed, next_summary, leaf_callback);
      }
      active = static_cast<std::uint8_t>(active & (active - 1U));
    }
  }

  template <bool VisitRejected = true, bool TrackSummary = false, class LeafCallback>
  void walk_candidate_pairs(std::size_t position, LeafCallback&& leaf_callback) {
    const auto& left = slices_.at(position);
    const auto& right = slices_.at(position + 1U);
    if(left.depth() != right.depth()) {
      throw std::logic_error("neighboring slice trees have different depths");
    }
    const PairGate* parent_gate = nullptr;
    if(pair_gates_ready_) {
      parent_gate = &pair_gates_.at(position);
      if(pair_gate_depth_ > left.depth()) {
        throw std::logic_error("pair gate is deeper than its slice trees");
      }
    }

    std::uint64_t cursor = 0;
    std::vector<std::uint8_t> history(left.depth());
    auto callback = std::forward<LeafCallback>(leaf_callback);
    auto run = [&]<bool CountStats, PairGateLocation GateLocation>() {
      candidate_pair_dfs<CountStats, VisitRejected, TrackSummary, GateLocation>(left, right, 0, 0, 0, history, parent_gate, cursor, true, PairPathSummary{},
                                                                                callback);
    };
    auto run_for_gate = [&]<PairGateLocation GateLocation>() {
      if(options_.verbose) {
        run.template operator()<true, GateLocation>();
      } else {
        run.template operator()<false, GateLocation>();
      }
    };
    if(parent_gate == nullptr) {
      run_for_gate.template operator()<PairGateLocation::None>();
    } else if(pair_gate_depth_ + 1U == left.depth()) {
      run_for_gate.template operator()<PairGateLocation::ParentOfLeaf>();
    } else if(pair_gate_depth_ == left.depth()) {
      run_for_gate.template operator()<PairGateLocation::Leaf>();
    } else {
      throw std::logic_error("pair gate is not on the current or parent leaf level");
    }
    if(parent_gate != nullptr && cursor != parent_gate->size()) {
      throw std::logic_error("pair gate has bits beyond its DFS enumeration");
    }
  }

  template <class LeafCallback> void walk_current_edges(std::size_t position, LeafCallback&& leaf_callback) {
    if(!pair_gates_ready_ || pair_gate_depth_ != height_) {
      throw std::logic_error("current pair gates are unavailable");
    }
    auto callback = std::forward<LeafCallback>(leaf_callback);
    walk_candidate_pairs<false, true>(position, [&](Node left_leaf, Node right_leaf, const auto& history, bool allowed, const PairPathSummary& summary) {
      if(allowed)
        callback(left_leaf, right_leaf, history, summary);
    });
  }

  void build_pair_gate_indexes() {
    if(!pair_gates_ready_ || pair_gate_depth_ != height_) {
      throw std::logic_error("pair gates are unavailable for indexing");
    }
    for(std::size_t position = 0; position < pair_gates_.size(); ++position) {
      auto& gate = pair_gates_[position];
      gate.reset_index(pair_gate_depth_);
      std::uint64_t seen = 0;
      walk_candidate_pairs(position, [&](Node, Node, const auto& history, bool) {
        if(seen % PairGate::index_quantum == 0) {
          gate.add_index_path(seen, history.data(), pair_gate_depth_);
        }
        ++seen;
      });
      if(seen != gate.size() || (seen != 0 && !gate.index_ready(pair_gate_depth_))) {
        throw std::logic_error("pair-gate index does not match its DFS enumeration");
      }
    }
  }

  bool prune_supported() {
    if(implicit_relations_)
      return prune_supported_implicit();
    return prune_supported_legacy();
  }

  // In the native representation, support pruning is an induced subgraph
  // operation.  Ordinary support therefore needs no binary payload after the
  // endpoint tries are reified.  Once BCAF is active, its only additional
  // historical constraint is the factorable clause P(left) || S(right),
  // stored on the corresponding nodes of the two slice tries.  The sparse
  // PairGate that remains is only an ordered restart index over accepted
  // parent paths.
  bool prune_supported_implicit() {
    if(slices_.empty())
      return false;
    cached_partial_.reset();
    cached_completion_.reset();
    cached_completion_height_ = std::numeric_limits<std::size_t>::max();

    const auto adjacency_count = slices_.size() - 1U;
    std::vector<TagPair> normal;
    normal.reserve(slices_.size());
    for(const auto& slice : slices_)
      normal.emplace_back(slice.node_count());
    account_tags(normal);

    const auto bcaf_window = geometry_.long_window();
    const bool bcaf_active = options_.bcaf && height_ >= bcaf_window;
    const bool cache_partial =
        (options_.partial_mode == PartialMode::Every && height_ % static_cast<std::size_t>(options_.partial_every) == 0) ||
        (options_.partial_mode != PartialMode::None && options_.halt_height >= 0 &&
         geometry_.w_position(height_) >= static_cast<std::size_t>(options_.halt_height));
    std::vector<LeafTagPair> witness;
    if(bcaf_active) {
      witness.reserve(slices_.size());
      for(const auto& slice : slices_)
        witness.emplace_back(slice.leaf_begin(), slice.leaf_count());
    }

    const bool cache_completion = bcaf_active && options_.detect_ends && geometry_.complete_tile(height_);
    std::vector<LeafTagPair> completion_prefix;
    if(cache_completion) {
      completion_prefix.reserve(slices_.size());
      for(const auto& slice : slices_)
        completion_prefix.emplace_back(slice.leaf_begin(), slice.leaf_count());
    }
    peak_tag_bytes_ = std::max(peak_tag_bytes_, tag_bytes(normal) + tag_bytes(witness) + tag_bytes(completion_prefix));

    mark_boundary(slices_.front(), options_.left_edge, true, normal.front()[0]);
    mark_boundary(slices_.back(), options_.right_edge, false, normal.back()[1]);

    std::vector<LeafRange> local_prefix_uninteresting;
    if(bcaf_active) {
      phase("  implicit local-prefix-interest ranges");
      local_prefix_uninteresting.reserve(slices_.size());
      for(const auto& slice : slices_)
        local_prefix_uninteresting.push_back(local_prefix_uninteresting_range(slice, bcaf_window));
    }

    auto seed_suffix = [&](std::size_t position) {
      if(!bcaf_active)
        return;
      const auto leaves = LeafRange{slices_[position].leaf_begin(), slices_[position].leaf_end()};
      const auto skip = local_prefix_uninteresting[position];
      witness[position][1].or_source_range(normal[position][1], leaves.begin, skip.begin);
      witness[position][1].or_source_range(normal[position][1], skip.end, leaves.end);
    };

    phase(bcaf_active ? "  implicit relation: right reach + bcaf suffix" : "  implicit relation: right reach");
    seed_suffix(slices_.size() - 1U);
    for(std::size_t i = adjacency_count; i > 0; --i) {
      const auto relation = i - 1U;
      if(expanded_uniform_tail_) {
        auto visit = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active) {
          const auto reaches_right = static_cast<std::uint8_t>(active & expand_right_edges(get_leaf_four(normal[i][1], right_first)));
          if(reaches_right == 0)
            return;
          const auto normal_output = project_left_edges(reaches_right);
          std::uint8_t witness_output = 0;
          if(bcaf_active) {
            const auto witnessed = static_cast<std::uint8_t>(reaches_right & expand_right_edges(get_leaf_four(witness[i][1], right_first)));
            witness_output = project_left_edges(witnessed);
          }
          if constexpr(AtomicWrites) {
            if(normal_output != 0)
              atomic_set_leaf_four(normal[relation][1], left_first, normal_output);
            if(witness_output != 0)
              atomic_set_leaf_four(witness[relation][1], left_first, witness_output);
          } else {
            set_leaf_four(normal[relation][1], left_first, normal_output);
            if(witness_output != 0)
              set_leaf_four(witness[relation][1], left_first, witness_output);
          }
        };
        if(can_walk_candidate_pair_batches_parallel(relation)) {
          walk_candidate_pair_batches_parallel<false, false, true>(
              relation, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t,
                            std::size_t) { visit.template operator()<true>(left_first, right_first, active); });
        } else {
          walk_candidate_pair_batches<false>(
              relation, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                visit.template operator()<false>(left_first, right_first, active);
              });
        }
      } else {
        walk_candidate_pairs<false>(relation, [&](Node left_leaf, Node right_leaf, const auto&, bool) {
          if(!normal[i][1].get(right_leaf))
            return;
          normal[relation][1].set(left_leaf);
          if(bcaf_active && witness[i][1].get(right_leaf))
            witness[relation][1].set(left_leaf);
        });
      }
      seed_suffix(relation);
    }

    auto seed_prefix = [&](std::size_t position) {
      if(!bcaf_active)
        return;
      const auto leaves = LeafRange{slices_[position].leaf_begin(), slices_[position].leaf_end()};
      const auto skip = local_prefix_uninteresting[position];
      witness[position][0].or_source_range(normal[position][0], leaves.begin, skip.begin);
      witness[position][0].or_source_range(normal[position][0], skip.end, leaves.end);
    };
    seed_prefix(0);

    auto live_leaf = [&](std::size_t position, Node leaf) {
      const bool supported = normal[position][0].get(leaf) && normal[position][1].get(leaf);
      return supported && (!bcaf_active || witness[position][0].get(leaf) || witness[position][1].get(leaf));
    };
    Node partial_current = slices_.front().leaf_end();
    std::vector<std::vector<std::uint8_t>> partial_lineages;
    bool partial_seen = false;
    if(cache_partial) {
      for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
        if(live_leaf(0, leaf)) {
          partial_current = leaf;
          break;
        }
      }
      if(partial_current == slices_.front().leaf_end())
        return false;
      partial_lineages.reserve(slices_.size());
      partial_lineages.push_back(slices_.front().lineage(partial_current));
      partial_seen = !bcaf_active || labels_prefix_interesting(partial_lineages.back(), bcaf_window);
    }
    if(cache_completion) {
      for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
        if(live_leaf(0, leaf))
          completion_prefix.front()[0].set(leaf);
      }
    }

    const auto short_start = cache_completion ? height_ - geometry_.short_window() : 0U;
    const auto long_start = cache_completion ? height_ - bcaf_window : 0U;
    const bool build_output_index = options_.worker_count > 1 && !options_.verbose;
    std::vector<PairGate> next_gates(adjacency_count);

    auto append_final_edges = [&](PairGate& output, std::uint8_t edges, const std::uint8_t* history, std::size_t parent_depth) {
      const auto count = static_cast<unsigned>(std::popcount(static_cast<unsigned>(edges)));
      if(count == 0)
        return;
      const auto start = output.size();
      static_assert((PairGate::index_quantum & (PairGate::index_quantum - 1U)) == 0);
      // A terminal batch has at most eight outputs and therefore crosses at
      // most one restart boundary.
      const auto ordinal = static_cast<unsigned>((PairGate::index_quantum - (start & (PairGate::index_quantum - 1U))) &
                                                 (PairGate::index_quantum - 1U));
      if(build_output_index && ordinal < count) {
        auto selected = edges;
        for(unsigned skipped = 0; skipped < ordinal; ++skipped)
          selected = static_cast<std::uint8_t>(selected & (selected - 1U));
        const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(selected)));
        output.add_index_path_with_final(start + ordinal, history, parent_depth, geometry_pair_triple_order[position]);
      }
      output.append_implicit_ones(count);
    };

    phase(bcaf_active ? "  implicit relation: left reach + bcaf prefix/index" : "  implicit relation: left reach/index");
    for(std::size_t i = 0; i < adjacency_count; ++i) {
      const auto absent_partial = slices_[i + 1U].leaf_end();
      auto partial_next = absent_partial;
      auto consider_partial_batch = [&](Node left_first, Node right_first, std::uint8_t final_edges, Node& candidate) {
        if(candidate != absent_partial || partial_current < left_first || partial_current >= left_first + 4U)
          return;
        const auto left_offset = static_cast<unsigned>(partial_current - left_first);
        auto remaining = static_cast<std::uint8_t>(final_edges & (0x03U << (2U * left_offset)));
        while(remaining != 0) {
          const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
          const auto right_leaf = right_first + (position & 0b11U);
          if(!bcaf_active || partial_seen || witness[i + 1U][1].get(right_leaf)) {
            candidate = right_leaf;
            return;
          }
          remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
        }
      };
      auto visit_batch = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active, const CompletionPathState* path_state,
                                                std::size_t final_depth) {
        const auto reaches_left = static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i][0], left_first)));
        if(reaches_left == 0)
          return std::uint8_t{0};
        const auto normal_output = project_right_edges(reaches_left);
        const auto normal_edges =
            static_cast<std::uint8_t>(reaches_left & expand_right_edges(get_leaf_four(normal[i + 1U][1], right_first)));
        std::uint8_t prefix_output = 0;
        auto final_edges = normal_edges;
        if(bcaf_active && normal_edges != 0) {
          const auto prefix_edges = expand_left_edges(get_leaf_four(witness[i][0], left_first));
          const auto suffix_edges = expand_right_edges(get_leaf_four(witness[i + 1U][1], right_first));
          prefix_output = project_right_edges(static_cast<std::uint8_t>(normal_edges & prefix_edges));
          final_edges = static_cast<std::uint8_t>(normal_edges & (prefix_edges | suffix_edges));
        }

        std::uint8_t completion_valid_output = 0;
        std::uint8_t completion_interesting_output = 0;
        if(path_state != nullptr && *path_state != CompletionPathState::Invalid && final_edges != 0) {
          const auto prefix_valid_edges = expand_left_edges(get_leaf_four(completion_prefix[i][0], left_first));
          const auto prefix_interesting_edges = expand_left_edges(get_leaf_four(completion_prefix[i][1], left_first));
          // In geometry_pair_triple_order, positions 0/1 have a zero left
          // label and positions 2..7 have a nonzero one.  Completion state is
          // therefore identical within each group; classify both masks once
          // instead of walking every surviving terminal edge.
          constexpr std::uint8_t zero_left_edges = 0x03U;
          constexpr std::uint8_t nonzero_left_edges = 0xfcU;
          const auto advanced_state = advance_completion_path(*path_state, 1U, final_depth, long_start, short_start);
          const auto valid_state_edges = static_cast<std::uint8_t>((*path_state != CompletionPathState::Invalid ? zero_left_edges : 0U) |
                                                                   (advanced_state != CompletionPathState::Invalid ? nonzero_left_edges : 0U));
          const auto interesting_state_edges =
              static_cast<std::uint8_t>((*path_state == CompletionPathState::ValidInteresting ? zero_left_edges : 0U) |
                                        (advanced_state == CompletionPathState::ValidInteresting ? nonzero_left_edges : 0U));
          const auto completion_valid_edges = static_cast<std::uint8_t>(final_edges & prefix_valid_edges & valid_state_edges);
          const auto completion_interesting_edges =
              static_cast<std::uint8_t>(completion_valid_edges & (prefix_interesting_edges | interesting_state_edges));
          completion_valid_output = project_right_edges(completion_valid_edges);
          completion_interesting_output = project_right_edges(completion_interesting_edges);
        }

        if constexpr(AtomicWrites) {
          if(normal_output != 0)
            atomic_set_leaf_four(normal[i + 1U][0], right_first, normal_output);
          if(prefix_output != 0)
            atomic_set_leaf_four(witness[i + 1U][0], right_first, prefix_output);
          if(completion_valid_output != 0)
            atomic_set_leaf_four(completion_prefix[i + 1U][0], right_first, completion_valid_output);
          if(completion_interesting_output != 0)
            atomic_set_leaf_four(completion_prefix[i + 1U][1], right_first, completion_interesting_output);
        } else {
          set_leaf_four(normal[i + 1U][0], right_first, normal_output);
          if(prefix_output != 0)
            set_leaf_four(witness[i + 1U][0], right_first, prefix_output);
          if(completion_valid_output != 0)
            set_leaf_four(completion_prefix[i + 1U][0], right_first, completion_valid_output);
          if(completion_interesting_output != 0)
            set_leaf_four(completion_prefix[i + 1U][1], right_first, completion_interesting_output);
        }
        return final_edges;
      };

      if(expanded_uniform_tail_ && can_walk_candidate_pair_batches_parallel(i)) {
        std::vector<PairGate> segments(pair_gates_[i].index_starts.size());
        for(auto& segment : segments)
          segment.reset_index(height_);
        std::vector<Node> partial_candidates(cache_partial ? segments.size() : 0, absent_partial);
        auto run_parallel = [&]<bool CapturePartial>() {
          if(cache_completion) {
            walk_candidate_pair_batches_parallel_completion<false, true>(
                i, long_start, short_start,
                [&](Node left_first, Node right_first, std::uint8_t active, bool, CompletionPathState path_state, std::size_t depth,
                       const auto& history, std::size_t checkpoint, std::size_t) {
                  const auto final_edges = visit_batch.template operator()<true>(left_first, right_first, active, &path_state, depth);
                  if constexpr(CapturePartial)
                    consider_partial_batch(left_first, right_first, final_edges, partial_candidates[checkpoint]);
                  append_final_edges(segments[checkpoint], final_edges, history.data(), depth);
                });
          } else {
            walk_candidate_pair_batches_parallel<false, false, true>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t depth, const auto& history,
                       std::size_t checkpoint, std::size_t) {
                  const auto final_edges = visit_batch.template operator()<true>(left_first, right_first, active, nullptr, depth);
                  if constexpr(CapturePartial)
                    consider_partial_batch(left_first, right_first, final_edges, partial_candidates[checkpoint]);
                  append_final_edges(segments[checkpoint], final_edges, history.data(), depth);
                });
          }
        };
        if(cache_partial)
          run_parallel.template operator()<true>();
        else
          run_parallel.template operator()<false>();
        if(cache_partial) {
          for(const auto candidate : partial_candidates) {
            if(candidate != absent_partial) {
              partial_next = candidate;
              break;
            }
          }
        }
        next_gates[i].reset_index(height_);
        std::size_t output_index_entries = 0;
        for(const auto& segment : segments)
          output_index_entries += segment.index_starts.size();
        next_gates[i].reserve_index_entries(output_index_entries);
        for(auto& segment : segments)
          next_gates[i].append(std::move(segment));
      } else if(expanded_uniform_tail_ && !build_output_index) {
        auto run_serial_batches = [&]<bool CapturePartial>() {
          if(cache_completion) {
            walk_candidate_pair_batches_completion<false>(
                i, long_start, short_start,
                [&](Node left_first, Node right_first, std::uint8_t active, bool, CompletionPathState path_state, std::size_t depth) {
                  const auto final_edges = visit_batch.template operator()<false>(left_first, right_first, active, &path_state, depth);
                  if constexpr(CapturePartial)
                    consider_partial_batch(left_first, right_first, final_edges, partial_next);
                  next_gates[i].append_implicit_ones(static_cast<unsigned>(std::popcount(static_cast<unsigned>(final_edges))));
                });
          } else {
            walk_candidate_pair_batches<false>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t depth) {
                  const auto final_edges = visit_batch.template operator()<false>(left_first, right_first, active, nullptr, depth);
                  if constexpr(CapturePartial)
                    consider_partial_batch(left_first, right_first, final_edges, partial_next);
                  next_gates[i].append_implicit_ones(static_cast<unsigned>(std::popcount(static_cast<unsigned>(final_edges))));
                });
          }
        };
        if(cache_partial)
          run_serial_batches.template operator()<true>();
        else
          run_serial_batches.template operator()<false>();
      } else {
        if(build_output_index)
          next_gates[i].reset_index(height_);
        auto visit_edge = [&]<bool CapturePartial>(Node left_leaf, Node right_leaf, const auto& history, const PairPathSummary* summary) {
          if(!normal[i][0].get(left_leaf))
            return;
          normal[i + 1U][0].set(right_leaf);
          const bool normal_edge = normal[i + 1U][1].get(right_leaf);
          if(!normal_edge)
            return;
          bool final_edge = true;
          if(bcaf_active) {
            if(witness[i][0].get(left_leaf))
              witness[i + 1U][0].set(right_leaf);
            final_edge = witness[i][0].get(left_leaf) || witness[i + 1U][1].get(right_leaf);
          }
          if(!final_edge)
            return;
          if(summary != nullptr && completion_prefix[i][0].get(left_leaf)) {
            const auto last = summary->last_left_nonzero;
            const bool local_valid = last == std::numeric_limits<std::size_t>::max() || last < short_start;
            if(local_valid) {
              completion_prefix[i + 1U][0].set(right_leaf);
              const bool local_interesting = last != std::numeric_limits<std::size_t>::max() && last >= long_start;
              if(local_interesting || completion_prefix[i][1].get(left_leaf))
                completion_prefix[i + 1U][1].set(right_leaf);
            }
          }
          if(build_output_index && next_gates[i].size() % PairGate::index_quantum == 0)
            next_gates[i].add_index_path(next_gates[i].size(), history.data(), height_);
          next_gates[i].push_back(true);
          if constexpr(CapturePartial) {
            if(partial_next == absent_partial && left_leaf == partial_current &&
               (!bcaf_active || partial_seen || witness[i + 1U][1].get(right_leaf)))
              partial_next = right_leaf;
          }
        };
        auto run_edges = [&]<bool CapturePartial>() {
          if(cache_completion) {
            walk_candidate_pairs<false, true>(i, [&](Node left_leaf, Node right_leaf, const auto& history, bool, const PairPathSummary& summary) {
              visit_edge.template operator()<CapturePartial>(left_leaf, right_leaf, history, &summary);
            });
          } else {
            walk_candidate_pairs<false>(i, [&](Node left_leaf, Node right_leaf, const auto& history, bool) {
              visit_edge.template operator()<CapturePartial>(left_leaf, right_leaf, history, nullptr);
            });
          }
        };
        if(cache_partial)
          run_edges.template operator()<true>();
        else
          run_edges.template operator()<false>();
      }
      seed_prefix(i + 1U);
      if(next_gates[i].size() == 0)
        return false;
      if(build_output_index && !next_gates[i].index_ready(height_))
        throw std::logic_error("implicit pair-gate emission produced an invalid sparse index");
      if(cache_partial) {
        if(partial_next == absent_partial)
          throw std::logic_error("interesting-path reconstruction lost its edge");
        partial_current = partial_next;
        partial_lineages.push_back(slices_[i + 1U].lineage(partial_current));
        partial_seen = partial_seen || !bcaf_active || labels_prefix_interesting(partial_lineages.back(), bcaf_window);
      }
    }
    if(cache_partial) {
      if(bcaf_active && !partial_seen)
        throw std::logic_error("interesting-path reconstruction lost its witness");
      cached_partial_ = board_from_lineages(partial_lineages);
    }
    bool completion_found = false;
    if(cache_completion) {
      walk_leaves(slices_.back(), [&](Node leaf, std::size_t, std::size_t last_nonzero) {
        if(completion_found || !live_leaf(slices_.size() - 1U, leaf) || !completion_prefix.back()[0].get(leaf))
          return;
        const bool local_valid = last_nonzero == std::numeric_limits<std::size_t>::max() || last_nonzero < short_start;
        const bool local_interesting = last_nonzero != std::numeric_limits<std::size_t>::max() && last_nonzero >= long_start;
        completion_found = local_valid && (local_interesting || completion_prefix.back()[1].get(leaf));
      });
      if(!completion_found)
        cached_completion_height_ = height_;
    }

    if(normal.front()[0].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0 ||
       normal.front()[1].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0) {
      return false;
    }

    if(bcaf_active) {
      if(bcaf_clause_begin_depth_ == 0) {
        bcaf_clause_begin_depth_ = height_;
        for(auto& slice : slices_)
          slice.initialize_bcaf_clauses(bcaf_clause_begin_depth_);
      }
      if(!expanded_uniform_tail_) {
        for(std::size_t position = 0; position < slices_.size(); ++position) {
          auto& tree = slices_[position];
          const auto parent_begin = tree.level_begin(tree.depth() - 1U);
          for(Node parent = parent_begin; parent < tree.leaf_begin(); ++parent) {
            const auto children = tree.child_block(parent);
            auto child = children.first;
            std::uint8_t clauses = 0;
            for(std::uint8_t label = 0; label < 4; ++label) {
              if((children.mask & (1U << label)) == 0)
                continue;
              if(witness[position][0].get(child))
                clauses = static_cast<std::uint8_t>(clauses | (1U << label));
              if(witness[position][1].get(child))
                clauses = static_cast<std::uint8_t>(clauses | (1U << (label + 4U)));
              ++child;
            }
            tree.set_bcaf_child_clauses(parent, clauses);
          }
        }
      }
    }

    phase("  implicit slice reification");
    if(pair_gates_ready_)
      pair_gates_.clear();
    std::vector<LeafTagPair>().swap(completion_prefix);
    const bool parallel_reification = bcaf_active && options_.worker_count > 1 && !options_.verbose;
    if(parallel_reification) {
      constexpr std::size_t keep_words_per_task = 4096;
      std::vector<BcafKeepRange> ranges;
      for(std::size_t position = 0; position < slices_.size(); ++position) {
        const auto begin = slices_[position].leaf_begin();
        const auto end = slices_[position].leaf_end();
        const auto first_word = static_cast<std::size_t>(begin >> 6U);
        const auto past_word = static_cast<std::size_t>((end + 63U) >> 6U);
        for(auto word = first_word; word < past_word; word += keep_words_per_task) {
          ranges.push_back(BcafKeepRange{
              position,
              std::max<Node>(begin, static_cast<Node>(word) * 64U),
              std::min<Node>(end, static_cast<Node>(word + keep_words_per_task) * 64U),
          });
        }
      }
      BcafKeepContext keep_context{this, &normal, &witness, &ranges};
      execute_indexed_tasks(ranges.size(), options_.worker_count, &keep_context, &execute_bcaf_keep_range);
      for(std::size_t position = 0; position < slices_.size(); ++position) {
        normal[position][1] = PackedTags{};
        witness[position] = LeafTagPair{};
      }
      std::vector<SuccinctSliceTree*> trees;
      std::vector<PackedTags*> keeps;
      trees.reserve(slices_.size());
      keeps.reserve(slices_.size());
      for(std::size_t position = 0; position < slices_.size(); ++position) {
        trees.push_back(&slices_[position]);
        keeps.push_back(&normal[position][0]);
      }
      if(!SuccinctSliceTree::reify_parallel_group(trees, keeps, options_.worker_count))
        return false;
      for(auto& tags : normal)
        tags[0] = PackedTags{};
    } else {
      for(std::size_t i = 0; i < slices_.size(); ++i) {
        auto& keep = normal[i][0];
        const auto leaf_begin = slices_[i].leaf_begin();
        const auto parent_begin = slices_[i].level_begin(slices_[i].depth() - 1U);
        for(Node leaf = slices_[i].leaf_begin(); leaf < slices_[i].leaf_end(); ++leaf) {
          if(bcaf_active && expanded_uniform_tail_ && ((leaf - leaf_begin) & 3U) == 0) {
            const auto parent = parent_begin + (leaf - leaf_begin) / 4U;
            const auto prefix_four = witness[i][0].get_4(leaf);
            const auto suffix_four = witness[i][1].get_4(leaf);
            slices_[i].set_bcaf_child_clauses_unchecked(parent, static_cast<std::uint8_t>(prefix_four | (suffix_four << 4U)));
          }
          keep.set(leaf, live_leaf(i, leaf));
        }
        if(!slices_[i].reify(keep))
          return false;
      }
    }

    pair_gates_ = std::move(next_gates);
    pair_gate_depth_ = height_;
    pair_gates_ready_ = true;
    return true;
  }

  bool prune_supported_legacy() {
    if(slices_.empty())
      return false;
    cached_partial_.reset();
    cached_completion_.reset();
    cached_completion_height_ = std::numeric_limits<std::size_t>::max();
    const auto adjacency_count = slices_.size() - 1U;
    std::vector<TagPair> normal;
    normal.reserve(slices_.size());
    for(const auto& slice : slices_)
      normal.emplace_back(slice.node_count());
    account_tags(normal);

    const auto bcaf_window = geometry_.long_window();
    const bool bcaf_active = options_.bcaf && height_ >= bcaf_window;
    std::vector<LeafTagPair> witness;
    if(bcaf_active) {
      witness.reserve(slices_.size());
      for(const auto& slice : slices_)
        witness.emplace_back(slice.leaf_begin(), slice.leaf_count());
      peak_tag_bytes_ = std::max(peak_tag_bytes_, tag_bytes(normal) + tag_bytes(witness));
    }

    // The two planes remain pure boundary reachability.  Pair-gate bits
    // ensure that a compatibility removed at an earlier height cannot be
    // recreated merely because both endpoint slices still exist.
    mark_boundary(slices_.front(), options_.left_edge, true, normal.front()[0]);
    mark_boundary(slices_.back(), options_.right_edge, false, normal.back()[1]);
    if(bcaf_active) {
      // Right reachability is sufficient to build the suffix witness:
      // once a left-to-right path reaches such a node, its witnessed
      // suffix completes a full path.  Symmetrically, left reachability
      // is sufficient for the prefix witness.  Computing the two in
      // dependency order lets the normal left-to-right sweep also be the
      // BCAF prefix and cleanup sweep.
      auto seed_suffix = [&](std::size_t position) {
        walk_leaves(slices_[position], [&](Node leaf, std::size_t first_nonzero, std::size_t) {
          if(normal[position][1].get(leaf) && first_nonzero < bcaf_window) {
            witness[position][1].set(leaf);
          }
        });
      };

      phase("  relation sweep: right to left + bcaf suffix");
      seed_suffix(slices_.size() - 1U);
      for(std::size_t i = adjacency_count; i > 0; --i) {
        normal[i - 1U][1].clear();
        if(expanded_uniform_tail_) {
          auto visit = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active) {
            const auto reaches_right = static_cast<std::uint8_t>(active & expand_right_edges(get_leaf_four(normal[i][1], right_first)));
            const auto normal_output = project_left_edges(reaches_right);
            const auto witnessed = static_cast<std::uint8_t>(reaches_right & expand_right_edges(get_leaf_four(witness[i][1], right_first)));
            const auto witness_output = project_left_edges(witnessed);
            if constexpr(AtomicWrites) {
              if(normal_output != 0)
                atomic_set_leaf_four(normal[i - 1U][1], left_first, normal_output);
              if(witness_output != 0)
                atomic_set_leaf_four(witness[i - 1U][1], left_first, witness_output);
            } else {
              set_leaf_four(normal[i - 1U][1], left_first, normal_output);
              set_leaf_four(witness[i - 1U][1], left_first, witness_output);
            }
          };
          if(can_walk_candidate_pair_batches_parallel(i - 1U)) {
            walk_candidate_pair_batches_parallel<false>(
                i - 1U, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t,
                            std::size_t) { visit.template operator()<true>(left_first, right_first, active); });
          } else {
            walk_candidate_pair_batches<false>(
                i - 1U, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                  visit.template operator()<false>(left_first, right_first, active);
                });
          }
        } else {
          walk_candidate_pairs<false>(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
            const bool reaches_right = ancestry_allowed && normal[i][1].get(right_leaf);
            if(reaches_right) {
              normal[i - 1U][1].set(left_leaf);
              if(witness[i][1].get(right_leaf)) {
                witness[i - 1U][1].set(left_leaf);
              }
            }
          });
        }
        seed_suffix(i - 1U);
      }

      peak_tag_bytes_ = std::max(peak_tag_bytes_, tag_bytes(normal) + tag_bytes(witness));

      auto seed_prefix = [&](std::size_t position) {
        walk_leaves(slices_[position], [&](Node leaf, std::size_t first_nonzero, std::size_t) {
          if(normal[position][0].get(leaf) && first_nonzero < bcaf_window) {
            witness[position][0].set(leaf);
          }
        });
      };

      seed_prefix(0);
      phase("  relation sweep: left to right + bcaf prefix");
      for(std::size_t i = 0; i < adjacency_count; ++i) {
        normal[i + 1U][0].clear();
        if(expanded_uniform_tail_) {
          auto visit = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active) {
            const auto reaches_left = static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i][0], left_first)));
            const auto normal_output = project_right_edges(reaches_left);
            const auto normal_edges =
                static_cast<std::uint8_t>(reaches_left & expand_right_edges(get_leaf_four(normal[i + 1U][1], right_first)));
            const auto interesting_prefix = expand_left_edges(get_leaf_four(witness[i][0], left_first));
            const auto witness_output = project_right_edges(static_cast<std::uint8_t>(normal_edges & interesting_prefix));
            if constexpr(AtomicWrites) {
              if(normal_output != 0)
                atomic_set_leaf_four(normal[i + 1U][0], right_first, normal_output);
              if(witness_output != 0)
                atomic_set_leaf_four(witness[i + 1U][0], right_first, witness_output);
            } else {
              set_leaf_four(normal[i + 1U][0], right_first, normal_output);
              set_leaf_four(witness[i + 1U][0], right_first, witness_output);
            }
          };
          if(can_walk_candidate_pair_batches_parallel(i)) {
            walk_candidate_pair_batches_parallel<false>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t,
                       std::size_t) { visit.template operator()<true>(left_first, right_first, active); });
          } else {
            walk_candidate_pair_batches<false>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                  visit.template operator()<false>(left_first, right_first, active);
                });
          }
        } else {
          walk_candidate_pairs<false>(i, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
            const bool reaches_left = ancestry_allowed && normal[i][0].get(left_leaf);
            if(reaches_left) {
              normal[i + 1U][0].set(right_leaf);
            }
            const bool normal_edge = reaches_left && normal[i + 1U][1].get(right_leaf);
            const bool interesting_prefix = witness[i][0].get(left_leaf);
            if(normal_edge && interesting_prefix) {
              witness[i + 1U][0].set(right_leaf);
            }
          });
        }
        seed_prefix(i + 1U);
      }
    } else {
      phase("  relation sweep: left to right");
      for(std::size_t i = 0; i < adjacency_count; ++i) {
        normal[i + 1U][0].clear();
        if(expanded_uniform_tail_) {
          auto visit = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active) {
            const auto edges = static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i][0], left_first)));
            const auto output = project_right_edges(edges);
            if constexpr(AtomicWrites) {
              if(output != 0)
                atomic_set_leaf_four(normal[i + 1U][0], right_first, output);
            } else {
              set_leaf_four(normal[i + 1U][0], right_first, output);
            }
          };
          if(can_walk_candidate_pair_batches_parallel(i)) {
            walk_candidate_pair_batches_parallel<false>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t,
                       std::size_t) { visit.template operator()<true>(left_first, right_first, active); });
          } else {
            walk_candidate_pair_batches<false>(
                i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                  visit.template operator()<false>(left_first, right_first, active);
                });
          }
        } else {
          walk_candidate_pairs<false>(i, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
            if(ancestry_allowed && normal[i][0].get(left_leaf)) {
              normal[i + 1U][0].set(right_leaf);
            }
          });
        }
      }

      phase("  relation sweep: right to left");
      for(std::size_t i = adjacency_count; i > 0; --i) {
        normal[i - 1U][1].clear();
        if(expanded_uniform_tail_) {
          auto visit = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active) {
            const auto edges = static_cast<std::uint8_t>(active & expand_right_edges(get_leaf_four(normal[i][1], right_first)));
            const auto output = project_left_edges(edges);
            if constexpr(AtomicWrites) {
              if(output != 0)
                atomic_set_leaf_four(normal[i - 1U][1], left_first, output);
            } else {
              set_leaf_four(normal[i - 1U][1], left_first, output);
            }
          };
          if(can_walk_candidate_pair_batches_parallel(i - 1U)) {
            walk_candidate_pair_batches_parallel<false>(
                i - 1U, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t,
                            std::size_t) { visit.template operator()<true>(left_first, right_first, active); });
          } else {
            walk_candidate_pair_batches<false>(
                i - 1U, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                  visit.template operator()<false>(left_first, right_first, active);
                });
          }
        } else {
          walk_candidate_pairs<false>(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
            if(ancestry_allowed && normal[i][1].get(right_leaf)) {
              normal[i - 1U][1].set(left_leaf);
            }
          });
        }
      }
    }

    auto normally_live = [&](std::size_t position, Node leaf) { return normal[position][0].get(leaf) && normal[position][1].get(leaf); };

    if(normal.front()[0].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0 ||
       normal.front()[1].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0) {
      return false;
    }

    std::vector<PairGate> next_gates(adjacency_count);
    bool slices_reified = false;

    if(bcaf_active) {
      phase("  bcaf relation gate");

      const bool cache_partial =
          (options_.partial_mode == PartialMode::Every && height_ % static_cast<std::size_t>(options_.partial_every) == 0) ||
          (options_.partial_mode != PartialMode::None && options_.halt_height >= 0 &&
           geometry_.w_position(height_) >= static_cast<std::size_t>(options_.halt_height));
      const bool cache_completion = options_.detect_ends && geometry_.complete_tile(height_);
      std::vector<PackedTags> partial_suffix;
      std::vector<LeafTagPair> completion_suffix;
      if(cache_partial) {
        partial_suffix.reserve(slices_.size());
        for(const auto& slice : slices_) {
          partial_suffix.emplace_back(slice.node_count());
        }
      }
      if(cache_completion) {
        completion_suffix.reserve(slices_.size());
        for(const auto& slice : slices_) {
          completion_suffix.emplace_back(slice.leaf_begin(), slice.leaf_count());
        }
      }
      std::size_t reconstruction_tag_bytes = 0;
      for(const auto& tags : partial_suffix) {
        reconstruction_tag_bytes += tags.allocated_bytes();
      }
      peak_tag_bytes_ =
          std::max(peak_tag_bytes_, tag_bytes(normal) + tag_bytes(witness) + tag_bytes(completion_suffix) + reconstruction_tag_bytes);

      const auto short_window = geometry_.short_window();
      const auto short_start = height_ - short_window;
      const auto long_start = height_ - bcaf_window;
      auto live_four = [&](std::size_t position, Node first) {
        return static_cast<std::uint8_t>(get_leaf_four(normal[position][0], first) & get_leaf_four(normal[position][1], first) &
                                         (get_leaf_four(witness[position][0], first) | get_leaf_four(witness[position][1], first)));
      };
      auto live_leaf = [&](std::size_t position, Node leaf) {
        return normal[position][0].get(leaf) && normal[position][1].get(leaf) &&
               (witness[position][0].get(leaf) || witness[position][1].get(leaf));
      };

      if(cache_partial || cache_completion) {
        walk_leaves(slices_.back(), [&](Node leaf, std::size_t first_nonzero, std::size_t last_nonzero) {
          const bool live = normal.back()[0].get(leaf) && normal.back()[1].get(leaf) &&
                            (witness.back()[0].get(leaf) || witness.back()[1].get(leaf));
          if(!live)
            return;
          if(cache_partial && first_nonzero < bcaf_window) {
            partial_suffix.back().set(leaf);
          }
          if(cache_completion) {
            const bool valid = last_nonzero == std::numeric_limits<std::size_t>::max() || last_nonzero < short_start;
            if(valid)
              completion_suffix.back()[0].set(leaf);
            if(valid && last_nonzero != std::numeric_limits<std::size_t>::max() && last_nonzero >= long_start) {
              completion_suffix.back()[1].set(leaf);
            }
          }
        });
      }
      for(std::size_t i = adjacency_count; i > 0; --i) {
        const auto relation = i - 1U;
        if(expanded_uniform_tail_) {
          const bool parallel_emit = can_walk_candidate_pair_batches_parallel(relation);
          std::vector<PairGate> segments;
          if(parallel_emit) {
            segments.resize(pair_gates_[relation].index_starts.size());
            for(auto& segment : segments)
              segment.reset_index(height_);
          }
          auto visit_batch = [&]<bool AtomicWrites>(Node left_first, Node right_first, std::uint8_t active, bool ancestry_allowed,
                                                    const PairPathSummary* summary, std::size_t final_depth) {
            const auto normal_edges = ancestry_allowed
                                          ? static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i - 1U][0], left_first)) &
                                                                      expand_right_edges(get_leaf_four(normal[i][1], right_first)))
                                          : std::uint8_t{0};
            const auto interesting_edges = static_cast<std::uint8_t>(
                expand_left_edges(get_leaf_four(witness[i - 1U][0], left_first)) | expand_right_edges(get_leaf_four(witness[i][1], right_first)));
            const auto eligible = static_cast<std::uint8_t>(normal_edges & interesting_edges);
            if(summary == nullptr || eligible == 0)
              return eligible;

            const auto partial_right = cache_partial ? get_leaf_four(partial_suffix[i], right_first) : std::uint8_t{0};
            const auto completion_valid_right = cache_completion ? get_leaf_four(completion_suffix[i][0], right_first) : std::uint8_t{0};
            const auto completion_interesting_right = cache_completion ? get_leaf_four(completion_suffix[i][1], right_first) : std::uint8_t{0};
            std::uint8_t partial_edges = 0;
            std::uint8_t completion_valid_edges = 0;
            std::uint8_t completion_interesting_edges = 0;
            auto remaining = eligible;
            while(remaining != 0) {
              const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
              const auto triple = geometry_pair_triple_order[position];
              auto first = summary->first_left_nonzero;
              auto last = summary->last_left_nonzero;
              if((triple & 0b011U) != 0) {
                if(first == std::numeric_limits<std::size_t>::max())
                  first = final_depth;
                last = final_depth;
              }
              const auto edge_bit = static_cast<std::uint8_t>(1U << position);
              const auto right_bit = static_cast<std::uint8_t>(1U << (position & 0b11U));
              if(cache_partial && ((partial_right & right_bit) != 0 || first < bcaf_window)) {
                partial_edges = static_cast<std::uint8_t>(partial_edges | edge_bit);
              }
              if(cache_completion) {
                const bool zero_tail = last == std::numeric_limits<std::size_t>::max() || last < short_start;
                if((completion_valid_right & right_bit) != 0 && zero_tail) {
                  completion_valid_edges = static_cast<std::uint8_t>(completion_valid_edges | edge_bit);
                  if((completion_interesting_right & right_bit) != 0 ||
                     (last != std::numeric_limits<std::size_t>::max() && last >= long_start)) {
                    completion_interesting_edges = static_cast<std::uint8_t>(completion_interesting_edges | edge_bit);
                  }
                }
              }
              remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
            }
            if(cache_partial) {
              const auto partial_output = project_left_edges(partial_edges);
              if constexpr(AtomicWrites) {
                if(partial_output != 0)
                  atomic_set_leaf_four(partial_suffix[i - 1U], left_first, partial_output);
              } else {
                set_leaf_four(partial_suffix[i - 1U], left_first, partial_output);
              }
            }
            if(cache_completion) {
              const auto valid_output = project_left_edges(completion_valid_edges);
              const auto interesting_output = project_left_edges(completion_interesting_edges);
              if constexpr(AtomicWrites) {
                if(valid_output != 0)
                  atomic_set_leaf_four(completion_suffix[i - 1U][0], left_first, valid_output);
                if(interesting_output != 0)
                  atomic_set_leaf_four(completion_suffix[i - 1U][1], left_first, interesting_output);
              } else {
                set_leaf_four(completion_suffix[i - 1U][0], left_first, valid_output);
                set_leaf_four(completion_suffix[i - 1U][1], left_first, interesting_output);
              }
            }
            return eligible;
          };
          auto emit_batch = [&](PairGate& output, Node left_first, Node right_first, std::uint8_t active, std::uint8_t final_edges,
                                auto&& record_index_path) {
            const auto keep_edges = static_cast<std::uint8_t>(active & expand_left_edges(live_four(relation, left_first)) &
                                                              expand_right_edges(live_four(i, right_first)));
            auto remaining = keep_edges;
            while(remaining != 0) {
              const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
              record_index_path(output, position);
              output.push_back((final_edges & (1U << position)) != 0);
              remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
            }
          };
          if(cache_partial || cache_completion) {
            if(parallel_emit) {
              walk_candidate_pair_batches_parallel<true, true>(
                  relation,
                  [&](Node left_first, Node right_first, std::uint8_t active, bool ancestry_allowed, const PairPathSummary& summary, std::size_t depth,
                      const auto& history, std::size_t checkpoint, std::size_t) {
                    const auto final_edges =
                        visit_batch.template operator()<true>(left_first, right_first, active, ancestry_allowed, &summary, depth);
                    emit_batch(segments[checkpoint], left_first, right_first, active, final_edges, [&](PairGate& output, std::uint8_t position) {
                      if(output.size() % PairGate::index_quantum == 0)
                        output.add_index_path_with_final(output.size(), history.data(), depth, geometry_pair_triple_order[position]);
                    });
                  });
            } else {
              walk_candidate_pair_batches<true, true>(
                  relation, [&](Node left_first, Node right_first, std::uint8_t active, bool ancestry_allowed, const PairPathSummary& summary,
                                std::size_t depth) {
                    const auto final_edges =
                        visit_batch.template operator()<false>(left_first, right_first, active, ancestry_allowed, &summary, depth);
                    emit_batch(next_gates[relation], left_first, right_first, active, final_edges, [](PairGate&, std::uint8_t) {});
                  });
            }
          } else {
            if(parallel_emit) {
              walk_candidate_pair_batches_parallel<true>(
                  relation,
                  [&](Node left_first, Node right_first, std::uint8_t active, bool ancestry_allowed, const PairPathSummary&, std::size_t depth,
                      const auto& history, std::size_t checkpoint, std::size_t) {
                    const auto final_edges = visit_batch.template operator()<true>(left_first, right_first, active, ancestry_allowed, nullptr, depth);
                    emit_batch(segments[checkpoint], left_first, right_first, active, final_edges, [&](PairGate& output, std::uint8_t position) {
                      if(output.size() % PairGate::index_quantum == 0)
                        output.add_index_path_with_final(output.size(), history.data(), depth, geometry_pair_triple_order[position]);
                    });
                  });
            } else {
              walk_candidate_pair_batches<true>(
                  relation, [&](Node left_first, Node right_first, std::uint8_t active, bool ancestry_allowed, const PairPathSummary&, std::size_t depth) {
                    const auto final_edges = visit_batch.template operator()<false>(left_first, right_first, active, ancestry_allowed, nullptr, depth);
                    emit_batch(next_gates[relation], left_first, right_first, active, final_edges, [](PairGate&, std::uint8_t) {});
                  });
            }
          }
          if(parallel_emit) {
            std::uint64_t output_size = 0;
            bool explicit_payload = false;
            for(const auto& segment : segments) {
              output_size += segment.size();
              explicit_payload = explicit_payload || segment.bits.size() != 0;
            }
            next_gates[relation].reset_index(height_);
            if(explicit_payload)
              next_gates[relation].reserve_payload_bits(output_size);
            for(auto& segment : segments)
              next_gates[relation].append(std::move(segment));
            if(output_size != 0 && !next_gates[relation].index_ready(height_))
              throw std::logic_error("fused pair-gate emission produced an invalid index");
          }
          continue;
        }
        auto visit_edge = [&](Node left_leaf, Node right_leaf, bool ancestry_allowed, const PairPathSummary* summary) {
          const bool normal_edge = ancestry_allowed && normal[i - 1U][0].get(left_leaf) && normal[i][1].get(right_leaf);
          const bool interesting_path = witness[i - 1U][0].get(left_leaf) || witness[i][1].get(right_leaf);
          const bool edge = normal_edge && interesting_path;
          if(live_leaf(i - 1U, left_leaf) && live_leaf(i, right_leaf))
            next_gates[i - 1U].push_back(edge);
          if(edge) {
            if(cache_partial && (partial_suffix[i].get(right_leaf) || summary->first_left_nonzero < bcaf_window)) {
              partial_suffix[i - 1U].set(left_leaf);
            }
            if(cache_completion) {
              const auto last = summary->last_left_nonzero;
              const bool zero_tail = last == std::numeric_limits<std::size_t>::max() || last < short_start;
              if(completion_suffix[i][0].get(right_leaf) && zero_tail) {
                completion_suffix[i - 1U][0].set(left_leaf);
                if(completion_suffix[i][1].get(right_leaf) || (last != std::numeric_limits<std::size_t>::max() && last >= long_start)) {
                  completion_suffix[i - 1U][1].set(left_leaf);
                }
              }
            }
          }
        };
        if(cache_partial || cache_completion) {
          walk_candidate_pairs<true, true>(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed, const PairPathSummary& summary) {
            visit_edge(left_leaf, right_leaf, ancestry_allowed, &summary);
          });
        } else {
          walk_candidate_pairs<true>(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
            visit_edge(left_leaf, right_leaf, ancestry_allowed, nullptr);
          });
        }
      }

      phase("  slice reification");
      bool any_bcaf_live = false;
      for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
        if(normal.front()[0].get(leaf) && normal.front()[1].get(leaf) && (witness.front()[0].get(leaf) || witness.front()[1].get(leaf))) {
          any_bcaf_live = true;
          break;
        }
      }
      if(!any_bcaf_live) {
        return false;
      }

      Node partial_current = slices_.front().leaf_end();
      std::vector<std::vector<std::uint8_t>> partial_lineages;
      bool partial_seen = false;
      if(cache_partial) {
        for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
          if(partial_suffix.front().get(leaf)) {
            partial_current = leaf;
            break;
          }
        }
        if(partial_current == slices_.front().leaf_end()) {
          throw std::logic_error("bcaf-projected state has no interesting path");
        }
        partial_lineages.reserve(slices_.size());
        partial_lineages.push_back(slices_.front().lineage(partial_current));
        partial_seen = labels_prefix_interesting(partial_lineages.front(), bcaf_window);
      }

      Node completion_current = slices_.front().leaf_end();
      std::vector<std::vector<std::uint8_t>> completion_lineages;
      bool completion_seen = false;
      bool completion_found = false;
      if(cache_completion) {
        for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
          if(completion_suffix.front()[1].get(leaf)) {
            completion_current = leaf;
            completion_found = true;
            break;
          }
        }
        if(completion_found) {
          completion_lineages.reserve(slices_.size());
          completion_lineages.push_back(slices_.front().lineage(completion_current));
          completion_seen = labels_interesting(completion_lineages.front(), bcaf_window);
        }
      }

      // Gate bits were emitted during the right-to-left suffix traversal.
      // Only a requested concrete witness needs another relation walk; the
      // ordinary search path can reify immediately without replaying the
      // paired tries a fourth time.
      const bool parallel_reification = options_.worker_count > 1 && !options_.verbose && !cache_partial && !completion_found;
      if(parallel_reification) {
        if(pair_gates_ready_)
          pair_gates_.clear();
        std::vector<std::uint8_t> retained(slices_.size(), 0);
        BcafReifyContext context{this, &normal, &witness, &retained};
        execute_indexed_tasks(slices_.size(), options_.worker_count, &context, &execute_bcaf_reify);
        if(std::ranges::find(retained, std::uint8_t{0}) != retained.end())
          return false;
        for(std::size_t i = 0; i < slices_.size(); ++i) {
          normal[i] = TagPair{};
          witness[i] = LeafTagPair{};
        }
      } else {
        for(std::size_t i = 0; i < adjacency_count; ++i) {
          bool partial_selected = false;
          bool completion_selected = false;
          if((cache_partial || completion_found) && expanded_uniform_tail_ && can_walk_candidate_pair_batches_parallel(i)) {
          const auto task_count = pair_gates_[i].index_starts.size();
          const auto absent = slices_[i + 1U].leaf_end();
          std::vector<Node> partial_candidates(cache_partial ? task_count : 0, absent);
          std::vector<Node> completion_candidates(completion_found ? task_count : 0, absent);
          walk_candidate_pair_batches_parallel<false>(
              i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t, const auto&, std::size_t checkpoint,
                     std::size_t) {
                const auto normal_edges = static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i][0], left_first)) &
                                                                    expand_right_edges(get_leaf_four(normal[i + 1U][1], right_first)));
                const auto interesting_edges = static_cast<std::uint8_t>(
                    expand_left_edges(get_leaf_four(witness[i][0], left_first)) | expand_right_edges(get_leaf_four(witness[i + 1U][1], right_first)));
                auto remaining = static_cast<std::uint8_t>(normal_edges & interesting_edges);
                while(remaining != 0) {
                  const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
                  const auto left_leaf = left_first + (position >> 1U);
                  const auto right_leaf = right_first + (position & 0b11U);
                  if(cache_partial && partial_candidates[checkpoint] == absent && left_leaf == partial_current &&
                     (partial_seen || partial_suffix[i + 1U].get(right_leaf))) {
                    partial_candidates[checkpoint] = right_leaf;
                  }
                  const auto completion_plane = completion_seen ? 0U : 1U;
                  if(completion_found && completion_candidates[checkpoint] == absent && left_leaf == completion_current &&
                     completion_suffix[i + 1U][completion_plane].get(right_leaf)) {
                    completion_candidates[checkpoint] = right_leaf;
                  }
                  remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
                }
              });

          if(cache_partial) {
            for(const auto candidate : partial_candidates) {
              if(candidate == absent)
                continue;
              partial_current = candidate;
              partial_lineages.push_back(slices_[i + 1U].lineage(candidate));
              partial_seen = partial_seen || labels_prefix_interesting(partial_lineages.back(), bcaf_window);
              partial_selected = true;
              break;
            }
          }
          if(completion_found) {
            for(const auto candidate : completion_candidates) {
              if(candidate == absent)
                continue;
              completion_current = candidate;
              completion_lineages.push_back(slices_[i + 1U].lineage(candidate));
              completion_seen = completion_seen || labels_interesting(completion_lineages.back(), bcaf_window);
              completion_selected = true;
              break;
            }
          }
        } else if((cache_partial || completion_found) && expanded_uniform_tail_) {
          walk_candidate_pair_batches<false>(
              i, [&](Node left_first, Node right_first, std::uint8_t active, bool, const PairPathSummary&, std::size_t) {
                const auto normal_edges = static_cast<std::uint8_t>(active & expand_left_edges(get_leaf_four(normal[i][0], left_first)) &
                                                                    expand_right_edges(get_leaf_four(normal[i + 1U][1], right_first)));
                const auto interesting_edges = static_cast<std::uint8_t>(
                    expand_left_edges(get_leaf_four(witness[i][0], left_first)) | expand_right_edges(get_leaf_four(witness[i + 1U][1], right_first)));
                auto remaining = static_cast<std::uint8_t>(normal_edges & interesting_edges);
                while(remaining != 0) {
                  const auto position = static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(remaining)));
                  const auto left_leaf = left_first + (position >> 1U);
                  const auto right_leaf = right_first + (position & 0b11U);
                  if(cache_partial && !partial_selected && left_leaf == partial_current &&
                     (partial_seen || partial_suffix[i + 1U].get(right_leaf))) {
                    partial_current = right_leaf;
                    partial_lineages.push_back(slices_[i + 1U].lineage(right_leaf));
                    partial_seen = partial_seen || labels_prefix_interesting(partial_lineages.back(), bcaf_window);
                    partial_selected = true;
                  }
                  const auto completion_plane = completion_seen ? 0U : 1U;
                  if(!completion_selected && completion_found && left_leaf == completion_current &&
                     completion_suffix[i + 1U][completion_plane].get(right_leaf)) {
                    completion_current = right_leaf;
                    completion_lineages.push_back(slices_[i + 1U].lineage(right_leaf));
                    completion_seen = completion_seen || labels_interesting(completion_lineages.back(), bcaf_window);
                    completion_selected = true;
                  }
                  remaining = static_cast<std::uint8_t>(remaining & (remaining - 1U));
                }
              });
        } else if(cache_partial || completion_found) {
          walk_candidate_pairs<false>(i, [&](Node left_leaf, Node right_leaf, const auto&, bool) {
            const bool normal_edge = normal[i][0].get(left_leaf) && normal[i + 1U][1].get(right_leaf);
            const bool interesting_path = witness[i][0].get(left_leaf) || witness[i + 1U][1].get(right_leaf);
            if(normal_edge && interesting_path) {
              if(cache_partial && !partial_selected && left_leaf == partial_current &&
                 (partial_seen || partial_suffix[i + 1U].get(right_leaf))) {
                partial_current = right_leaf;
                partial_lineages.push_back(slices_[i + 1U].lineage(right_leaf));
                partial_seen = partial_seen || labels_prefix_interesting(partial_lineages.back(), bcaf_window);
                partial_selected = true;
              }
              const auto completion_plane = completion_seen ? 0U : 1U;
              if(!completion_selected && completion_found && left_leaf == completion_current &&
                 completion_suffix[i + 1U][completion_plane].get(right_leaf)) {
                completion_current = right_leaf;
                completion_lineages.push_back(slices_[i + 1U].lineage(right_leaf));
                completion_seen = completion_seen || labels_interesting(completion_lineages.back(), bcaf_window);
                completion_selected = true;
              }
            }
          });
        }
        if(cache_partial && !partial_selected) {
          throw std::logic_error("interesting-path reconstruction lost its edge");
        }
        if(completion_found && !completion_selected) {
          throw std::logic_error("end reconstruction lost its edge");
        }
        if(pair_gates_ready_)
          pair_gates_[i] = PairGate{};
        auto& keep = normal[i][0];
        for(Node leaf = slices_[i].leaf_begin(); leaf < slices_[i].leaf_end(); ++leaf)
          keep.set(leaf, live_leaf(i, leaf));
        if(!slices_[i].reify(keep))
          return false;
        normal[i] = TagPair{};
        witness[i] = LeafTagPair{};
        }
      }
      if(!parallel_reification) {
        auto& keep_back = normal.back()[0];
        for(Node leaf = slices_.back().leaf_begin(); leaf < slices_.back().leaf_end(); ++leaf)
          keep_back.set(leaf, live_leaf(slices_.size() - 1U, leaf));
        if(!slices_.back().reify(keep_back))
          return false;
        normal.back() = TagPair{};
        witness.back() = LeafTagPair{};
      }
      if(cache_partial) {
        if(!partial_seen) {
          throw std::logic_error("interesting-path reconstruction lost its witness");
        }
        cached_partial_ = board_from_lineages(partial_lineages);
      }
      if(cache_completion) {
        cached_completion_height_ = height_;
        if(completion_found) {
          if(!completion_seen) {
            throw std::logic_error("end reconstruction lost its witness");
          }
          cached_completion_ = board_from_lineages(completion_lineages);
        }
      }
      slices_reified = true;
    } else {
      for(std::size_t i = 0; i < adjacency_count; ++i) {
        const bool build_output_index = options_.worker_count > 1 && !options_.verbose;
        if(build_output_index)
          next_gates[i].reset_index(height_);
        walk_candidate_pairs(i, [&](Node left_leaf, Node right_leaf, const auto& history, bool ancestry_allowed) {
          const bool keep_left = normally_live(i, left_leaf);
          const bool keep_right = normally_live(i + 1U, right_leaf);
          if(keep_left && keep_right) {
            if(build_output_index && next_gates[i].size() % PairGate::index_quantum == 0)
              next_gates[i].add_index_path(next_gates[i].size(), history.data(), height_);
            const bool final_edge = ancestry_allowed && normal[i][0].get(left_leaf) && normal[i + 1U][1].get(right_leaf);
            next_gates[i].push_back(final_edge);
          }
        });
        if(build_output_index && next_gates[i].size() != 0 && !next_gates[i].index_ready(height_))
          throw std::logic_error("non-BCAF pair-gate emission produced an invalid index");
      }
      phase("  slice reification");
    }

    if(!slices_reified) {
      for(std::size_t i = 0; i < slices_.size(); ++i) {
        auto& keep = normal[i][0];
        for(Node leaf = slices_[i].leaf_begin(); leaf < slices_[i].leaf_end(); ++leaf) {
          keep.set(leaf, normal[i][0].get(leaf) && normal[i][1].get(leaf));
        }
        if(!slices_[i].reify(keep))
          return false;
      }
    }

    pair_gates_ = std::move(next_gates);
    pair_gate_depth_ = height_;
    pair_gates_ready_ = true;
    return true;
  }

  template <class LeafCallback>
  void leaf_dfs(const SuccinctSliceTree& tree,
                Node node,
                std::size_t depth,
                std::size_t first_nonzero,
                std::size_t last_nonzero,
                std::vector<Node>& child_cursor,
                LeafCallback& callback) const {
    if(depth == tree.depth()) {
      callback(node, first_nonzero, last_nonzero);
      return;
    }
    const auto mask = tree.child_mask(node);
    auto child = child_cursor[depth];
    child_cursor[depth] += static_cast<Node>(std::popcount(mask));
    for(std::uint8_t label = 0; label < 4; ++label) {
      if((mask & (1U << label)) == 0)
        continue;
      const auto next = child++;
      auto next_first = first_nonzero;
      auto next_last = last_nonzero;
      if(label != 0) {
        if(next_first == std::numeric_limits<std::size_t>::max()) {
          next_first = depth;
        }
        next_last = depth;
      }
      leaf_dfs(tree, next, depth + 1, next_first, next_last, child_cursor, callback);
    }
  }

  template <class LeafCallback> void walk_leaves(const SuccinctSliceTree& tree, LeafCallback&& callback) const {
    auto actual = std::forward<LeafCallback>(callback);
    constexpr auto none = std::numeric_limits<std::size_t>::max();
    std::vector<Node> child_cursor(tree.depth());
    for(std::size_t depth = 0; depth < tree.depth(); ++depth) {
      child_cursor[depth] = tree.level_begin(depth + 1U);
    }
    leaf_dfs(tree, 0, 0, none, none, child_cursor, actual);
  }

  struct LeafRange {
    Node begin = 0;
    Node end = 0;
  };

  // A local path is uninteresting exactly when its first `window` labels are
  // all zero.  There is at most one such prefix node.  Since children are in
  // parent-ID order, the descendants of a contiguous node range remain
  // contiguous at every later BFS level, so its current leaves form one
  // interval rather than requiring a full trie walk or one bit per leaf.
  static LeafRange local_prefix_uninteresting_range(const SuccinctSliceTree& tree, std::size_t window) {
    auto zero_prefix = Node{0};
    const auto prefix_depth = std::min(window, tree.depth());
    for(std::size_t depth = 0; depth < prefix_depth; ++depth) {
      const auto children = tree.child_block(zero_prefix);
      if((children.mask & 1U) == 0)
        return LeafRange{tree.leaf_end(), tree.leaf_end()};
      // Raw label zero is the first compact child whenever it is present.
      zero_prefix = children.first;
    }

    auto begin = zero_prefix;
    auto end = zero_prefix + 1U;
    for(std::size_t depth = prefix_depth; depth < tree.depth(); ++depth) {
      if(begin == end)
        return LeafRange{tree.leaf_end(), tree.leaf_end()};
      const auto first_children = tree.child_block(begin);
      const auto last_children = tree.child_block(end - 1U);
      begin = first_children.first;
      end = last_children.first + static_cast<Node>(std::popcount(last_children.mask));
    }
    if(begin < tree.leaf_begin() || end < begin || end > tree.leaf_end())
      throw std::logic_error("all-zero prefix descendants lost leaf alignment");
    return LeafRange{begin, end};
  }

  struct ExpandSliceContext {
    Solver* solver = nullptr;
  };

  static void execute_expand_slice(void* opaque, std::size_t position, std::size_t) {
    auto& context = *static_cast<ExpandSliceContext*>(opaque);
    context.solver->slices_[position].expand_leaves();
  }

  void expand_slice_tails() {
    ExpandSliceContext context{this};
    if(options_.worker_count > 1 && !options_.verbose) {
      execute_indexed_tasks(slices_.size(), options_.worker_count, &context, &execute_expand_slice);
    } else {
      for(std::size_t position = 0; position < slices_.size(); ++position)
        execute_expand_slice(&context, position, 0);
    }
  }

  static bool labels_interesting(const std::vector<std::uint8_t>& labels, std::size_t window) {
    if(labels.size() < window)
      return false;
    return std::any_of(labels.end() - static_cast<std::ptrdiff_t>(window), labels.end(), [](std::uint8_t label) { return label != 0; });
  }

  static bool labels_prefix_interesting(const std::vector<std::uint8_t>& labels, std::size_t window) {
    if(labels.size() < window)
      return false;
    return std::any_of(labels.begin(), labels.begin() + static_cast<std::ptrdiff_t>(window), [](std::uint8_t label) { return label != 0; });
  }

  std::optional<Board> find_completion() {
    if(!geometry_.complete_tile(height_))
      return std::nullopt;
    if(cached_completion_height_ == height_)
      return cached_completion_;
    const auto short_window = geometry_.short_window();
    const auto long_window = geometry_.long_window();
    if(height_ < long_window)
      return std::nullopt;

    std::vector<TagPair> suffix;
    suffix.reserve(slices_.size());
    for(const auto& slice : slices_)
      suffix.emplace_back(slice.node_count());
    account_tags(suffix);

    const auto short_start = height_ - short_window;
    const auto long_start = height_ - long_window;
    walk_leaves(slices_.back(), [&](Node leaf, std::size_t, std::size_t last_nonzero) {
      const bool valid = last_nonzero == std::numeric_limits<std::size_t>::max() || last_nonzero < short_start;
      if(valid)
        suffix.back()[0].set(leaf);
      if(valid && last_nonzero != std::numeric_limits<std::size_t>::max() && last_nonzero >= long_start) {
        suffix.back()[1].set(leaf);
      }
    });

    for(std::size_t i = slices_.size() - 1; i > 0; --i) {
      walk_current_edges(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, const PairPathSummary& summary) {
        const auto last = summary.last_left_nonzero;
        const bool zero_tail = last == std::numeric_limits<std::size_t>::max() || last < short_start;
        if(!suffix[i][0].get(right_leaf) || !zero_tail)
          return;
        suffix[i - 1][0].set(left_leaf);
        if(suffix[i][1].get(right_leaf) || (last != std::numeric_limits<std::size_t>::max() && last >= long_start)) {
          suffix[i - 1][1].set(left_leaf);
        }
      });
    }

    Node first = slices_.front().leaf_end();
    for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
      if(suffix.front()[1].get(leaf)) {
        first = leaf;
        break;
      }
    }
    if(first == slices_.front().leaf_end())
      return std::nullopt;
    return reconstruct_end(first, suffix, long_window);
  }

  template <class Predicate> std::pair<Node, std::vector<std::uint8_t>> find_current_right(std::size_t position, Node left_leaf, Predicate&& predicate) {
    const auto absent = slices_[position + 1U].leaf_end();
    Node result = absent;
    auto actual = std::forward<Predicate>(predicate);
    if(can_walk_current_edges_parallel(position)) {
      std::vector<Node> candidates(pair_gates_[position].index_starts.size(), absent);
      walk_current_edges_parallel(position, [&](Node candidate_left, Node candidate_right, const auto&, const PairPathSummary&, std::size_t checkpoint,
                                                std::size_t) {
        if(candidates[checkpoint] == absent && candidate_left == left_leaf && actual(candidate_right))
          candidates[checkpoint] = candidate_right;
      });
      for(const auto candidate : candidates) {
        if(candidate != absent) {
          result = candidate;
          break;
        }
      }
    } else {
      walk_current_edges(position, [&](Node candidate_left, Node candidate_right, const auto&, const auto&) {
        if(result == absent && candidate_left == left_leaf && actual(candidate_right))
          result = candidate_right;
      });
    }
    if(result == absent) {
      throw std::logic_error("supported slice has no allowed successor");
    }
    return {result, slices_[position + 1U].lineage(result)};
  }

  Board board_from_lineages(const std::vector<std::vector<std::uint8_t>>& lineages) const {
    Board board(height_, std::vector<std::uint8_t>(width_, 0));
    for(std::size_t row = 0; row < height_; ++row) {
      board[row][0] = static_cast<std::uint8_t>((lineages[0][row] >> 1U) & 1U);
      board[row][1] = static_cast<std::uint8_t>(lineages[0][row] & 1U);
      for(std::size_t slice = 1; slice < lineages.size(); ++slice) {
        board[row][slice + 1U] = static_cast<std::uint8_t>(lineages[slice][row] & 1U);
      }
    }
    return board;
  }

  Board reconstruct_any() {
    std::vector<std::vector<std::uint8_t>> lineages;
    lineages.reserve(slices_.size());
    Node current = slices_.front().leaf_begin();
    lineages.push_back(slices_.front().lineage(current));
    for(std::size_t i = 1; i < slices_.size(); ++i) {
      auto [node, labels] = find_current_right(i - 1U, current, [](Node) { return true; });
      current = node;
      lineages.push_back(std::move(labels));
    }
    return board_from_lineages(lineages);
  }

  Board reconstruct_interesting(std::size_t window) {
    std::vector<LeafTags> suffix;
    suffix.reserve(slices_.size());
    for(const auto& slice : slices_)
      suffix.emplace_back(slice.leaf_begin(), slice.leaf_count());
    std::size_t suffix_bytes = 0;
    for(const auto& tags : suffix)
      suffix_bytes += tags.allocated_bytes();
    peak_tag_bytes_ = std::max(peak_tag_bytes_, suffix_bytes);

    walk_leaves(slices_.back(), [&](Node leaf, std::size_t first_nonzero, std::size_t) {
      if(first_nonzero < window)
        suffix.back().set(leaf);
    });
    for(std::size_t i = slices_.size() - 1; i > 0; --i) {
      if(can_walk_current_edges_parallel(i - 1U)) {
        walk_current_edges_parallel(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, const PairPathSummary& summary, std::size_t, std::size_t) {
          if(suffix[i].get(right_leaf) || summary.first_left_nonzero < window)
            suffix[i - 1U].atomic_set(left_leaf);
        });
      } else {
        walk_current_edges(i - 1U, [&](Node left_leaf, Node right_leaf, const auto&, const PairPathSummary& summary) {
          if(suffix[i].get(right_leaf) || summary.first_left_nonzero < window)
            suffix[i - 1U].set(left_leaf);
        });
      }
    }

    Node first = slices_.front().leaf_end();
    for(Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
      if(suffix.front().get(leaf)) {
        first = leaf;
        break;
      }
    }
    if(first == slices_.front().leaf_end()) {
      throw std::logic_error("bcaf-projected state has no interesting path");
    }

    std::vector<std::vector<std::uint8_t>> lineages;
    lineages.reserve(slices_.size());
    Node current = first;
    lineages.push_back(slices_.front().lineage(first));
    bool seen = labels_prefix_interesting(lineages.front(), window);
    for(std::size_t i = 1; i < slices_.size(); ++i) {
      auto [node, labels] = find_current_right(i - 1U, current, [&](Node leaf) { return seen || suffix[i].get(leaf); });
      current = node;
      seen = seen || labels_prefix_interesting(labels, window);
      lineages.push_back(std::move(labels));
    }
    if(!seen) {
      throw std::logic_error("interesting-path reconstruction lost its witness");
    }
    return board_from_lineages(lineages);
  }

  Board reconstruct_partial() {
    if(cached_partial_)
      return *cached_partial_;
    const auto bcaf_window = geometry_.long_window();
    if(options_.bcaf && height_ >= bcaf_window) {
      return reconstruct_interesting(bcaf_window);
    }
    return reconstruct_any();
  }

  template <class SuffixPair> Board reconstruct_end(Node first, const std::vector<SuffixPair>& suffix, std::size_t long_window) {
    std::vector<std::vector<std::uint8_t>> lineages;
    lineages.reserve(slices_.size());
    Node current = first;
    lineages.push_back(slices_.front().lineage(first));
    bool seen_interesting = labels_interesting(lineages.front(), long_window);
    for(std::size_t i = 1; i < slices_.size(); ++i) {
      const auto required_plane = seen_interesting ? 0U : 1U;
      auto [node, labels] = find_current_right(i - 1U, current, [&](Node leaf) { return suffix[i][required_plane].get(leaf); });
      current = node;
      seen_interesting = seen_interesting || labels_interesting(labels, long_window);
      lineages.push_back(std::move(labels));
    }
    if(!seen_interesting) {
      throw std::logic_error("end reconstruction lost its interesting witness");
    }
    return board_from_lineages(lineages);
  }

  static std::string encode_rle(const Board& board) {
    std::vector<std::string> tokens;
    auto token = [&](std::size_t count, char symbol) { tokens.push_back((count > 1 ? std::to_string(count) : std::string{}) + symbol); };
    for(std::size_t y = 0; y < board.size(); ++y) {
      std::size_t x = 0;
      while(x < board[y].size()) {
        const auto value = board[y][x];
        std::size_t end = x + 1;
        while(end < board[y].size() && board[y][end] == value)
          ++end;
        token(end - x, value ? 'o' : 'b');
        x = end;
      }
      if(y + 1 < board.size())
        token(1, '$');
    }
    token(1, '!');

    std::ostringstream output;
    std::size_t column = 0;
    for(const auto& part : tokens) {
      if(column != 0 && column + part.size() > 70) {
        output << '\n';
        column = 0;
      }
      output << part;
      column += part.size();
    }
    output << '\n';
    return output.str();
  }

  void emit_board(const Board& row_sequence, std::string_view kind) {
    const auto board = rlife::llsss::render_phase_montage(geometry_, row_sequence, options_.left_edge, options_.right_edge);
    std::ostream& output = partial_file_.is_open() ? static_cast<std::ostream&>(partial_file_) : static_cast<std::ostream&>(std::cout);
    output << "#C llsss " << kind << ' ';
    if(geometry_.subtile_count != 1) {
      output << "flattened_depth=" << height_ << " w_pos=" << geometry_.position_string(height_) << " geometry=" << geometry_.source << '\n';
    } else {
      output << "height=" << height_ << " geometry=" << geometry_.source << '\n';
    }
    output << "#C physical time phases 0.." << geometry_.period - 1 << " left-to-right; gap=16\n";
    output
           << "x = " << board.front().size() << ", y = " << board.size() << ", rule = " << options_.rule << '\n'
           << encode_rle(board);
    output.flush();
  }

  void emit_final_partial(std::string_view kind) {
    if(options_.partial_mode != PartialMode::None && !slices_.empty()) {
      phase("partial reconstruction/output");
      emit_board(reconstruct_partial(), kind);
    }
  }

  void phase(std::string_view message) {
    if(running_) {
      if(options_.phase_progress) {
        if(geometry_.subtile_count != 1) {
          std::cout << "depth=" << height_ << " w_pos=" << geometry_.position_string(height_) << ' ' << message << '\n';
        } else {
          std::cout << "height=" << height_ << ' ' << message << '\n';
        }
      }
      if(options_.phase_timings) {
        finish_phase_timing();
        timed_phase_ = message;
        timed_phase_started_ = Clock::now();
      }
    }
  }

  void finish_phase_timing() {
    if(timed_phase_.empty())
      return;
    const auto seconds = std::chrono::duration<double>(Clock::now() - timed_phase_started_).count();
    phase_timings_[timed_phase_] += seconds;
    timed_phase_.clear();
  }

  template <class Pair> static std::size_t tag_bytes(const std::vector<Pair>& tags) {
    std::size_t bytes = 0;
    for(const auto& pair : tags) {
      bytes += pair[0].allocated_bytes() + pair[1].allocated_bytes();
    }
    return bytes;
  }

  template <class Pair> void account_tags(const std::vector<Pair>& tags) { peak_tag_bytes_ = std::max(peak_tag_bytes_, tag_bytes(tags)); }

  void print_stats(std::string_view label, double seconds) {
    std::uint64_t nodes = 0;
    std::uint64_t leaves = 0;
    std::size_t bitstream = 0;
    std::size_t rank = 0;
    std::size_t levels = 0;
    std::size_t allocated = 0;
    std::uint64_t pair_candidates = 0;
    std::uint64_t pair_allowed = 0;
    std::size_t pair_gate_bytes = 0;
    std::ostringstream per_slice;
    std::ostringstream per_slice_leaves;
    std::ostringstream slice_state;
    per_slice << '[';
    per_slice_leaves << '[';
    slice_state << '[';
    for(std::size_t i = 0; i < slices_.size(); ++i) {
      if(i != 0) {
        per_slice << ',';
        per_slice_leaves << ',';
      }
      per_slice << slices_[i].node_count();
      per_slice_leaves << slices_[i].leaf_count();
      slice_state << slices_[i].leaf_count();
      if(pair_gates_ready_ && i < pair_gates_.size()) {
        slice_state << '/' << pair_gates_[i].count() << '\\';
      }
      nodes += slices_[i].node_count();
      leaves += slices_[i].leaf_count();
      bitstream += slices_[i].bitstream_bytes();
      rank += slices_[i].rank_bytes();
      levels += slices_[i].level_index_bytes();
      allocated += slices_[i].allocated_bytes();
    }
    per_slice << ']';
    per_slice_leaves << ']';
    slice_state << ']';
    if(pair_gates_ready_) {
      for(const auto& gate : pair_gates_) {
        pair_candidates += gate.size();
        pair_allowed += gate.count();
        pair_gate_bytes += gate.allocated_bytes();
      }
    }
    const auto lookup_bytes = rule_.storage_bytes() + geometry_acceptance_.storage_bytes() + pair_transitions_.size() * sizeof(pair_transitions_[0]);
    const auto persistent_payload_bytes = allocated + pair_gate_bytes + lookup_bytes;
    std::ostringstream line;
    if(options_.verbose) {
      if(geometry_.subtile_count != 1) {
        line << "depth=" << height_ << " w_pos=" << geometry_.position_string(height_);
      } else {
        line << "height=" << height_;
      }
      line << " label=" << label << " nodes=" << nodes << " leaves=" << leaves << " child_bytes=" << bitstream << " rank_bytes=" << rank
           << " level_bytes=" << levels << " allocated_bytes=" << allocated << " pair_gate_bytes=" << pair_gate_bytes << " lookup_bytes=" << lookup_bytes
           << " persistent_payload_bytes=" << persistent_payload_bytes << " pair_candidates=" << pair_candidates << " pair_allowed=" << pair_allowed
           << " tag_peak_bytes=" << peak_tag_bytes_ << " pair_states=" << pair_states_ << " pair_leaves=" << pair_leaves_
           << " boundary_states=" << boundary_states_ << " seconds=" << std::fixed << std::setprecision(6) << seconds << " slice_nodes=" << per_slice.str()
           << " slice_leaves=" << per_slice_leaves.str() << " slice_state=" << slice_state.str();
    } else {
      std::uint64_t maxrss = getMaxRSS();
      std::string maxrss_display;
      if(maxrss)
        maxrss_display = integer_format(maxrss) + "iB RSS, ";
      line << "Row " << height_;
      if(geometry_.subtile_count != 1) {
        line << " (w_pos " << geometry_.position_string(height_) << ')';
      }
      line << ", " << nodes << " nodes, mem " << integer_format(persistent_payload_bytes) << "iB, " << maxrss_display << "sec "
           << std::fixed << std::setprecision(6) << seconds << ", cols: " << slice_state.str();
    }
    std::cerr << line.str() << '\n';
    if(stats_file_) {
      stats_file_ << line.str() << '\n';
      stats_file_.flush();
    }
  }

  Options options_;
  Geometry geometry_;
  RuleTable rule_;
  GeometryAcceptance geometry_acceptance_;
  std::array<PairTransitions, 1U << 8U> pair_transitions_{};
  std::size_t width_ = 0;
  std::size_t height_ = 0;
  std::vector<SuccinctSliceTree> slices_;
  std::vector<PairGate> pair_gates_;
  std::size_t pair_gate_depth_ = 0;
  bool pair_gates_ready_ = false;
  // Native searches factor historical BCAF edge filters into two unary
  // payload bits on each retained trie node.  Older checkpoints keep using
  // the dense legacy relation exactly as serialized.
  bool implicit_relations_ = true;
  std::size_t bcaf_clause_begin_depth_ = 0;
  bool exhausted_ = false;
  bool completion_at_current_row_ = false;
  bool loaded_from_checkpoint_ = false;
  std::optional<std::size_t> last_checkpoint_height_;
  std::ofstream partial_file_;
  std::ofstream stats_file_;
  std::uint64_t pair_states_ = 0;
  std::uint64_t pair_leaves_ = 0;
  std::uint64_t boundary_states_ = 0;
  std::size_t peak_tag_bytes_ = 0;
  bool running_ = false;
  bool expanded_uniform_tail_ = false;
  std::optional<Board> cached_partial_;
  std::optional<Board> cached_completion_;
  std::size_t cached_completion_height_ = std::numeric_limits<std::size_t>::max();
  std::vector<GateRangeScratch> parallel_gate_scratch_;

  std::map<std::string, double> phase_timings_;
  std::string timed_phase_;
  Clock::time_point timed_phase_started_{};
};

} // namespace rlife::llsss
