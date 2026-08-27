#pragma once

#include "solver.hpp"

namespace rlife::llsss {

inline constexpr std::array<std::uint8_t, 16> checkpoint_magic_ = {
    'R', 'L', 'I', 'F', 'E', '-', 'L', 'L', 'S', 'S', 'S', '-', 'C', 'P', 0, 1,
};
inline constexpr std::uint32_t checkpoint_version_ = 5;
inline constexpr std::uint32_t oldest_checkpoint_version_ = 4;
inline constexpr std::size_t checkpoint_buffer_size_ = 8U * 1024U * 1024U;

class CheckpointWriter {
public:
  explicit CheckpointWriter(std::ostream& output) : output_(output), encoded_(checkpoint_buffer_size_) {}

  void bytes(const void* data, std::size_t size) {
    output_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if(!output_)
      throw std::runtime_error("failed while writing checkpoint");
    checksum(data, size);
  }

  void u8(std::uint8_t value) { bytes(&value, sizeof(value)); }
  void boolean(bool value) { u8(value ? 1U : 0U); }
  void u32(std::uint32_t value) { integer(value); }
  void u64(std::uint64_t value) { integer(value); }

  void string(const std::string& value) {
    u64(value.size());
    bytes(value.data(), value.size());
  }

  void vector_u64(const std::vector<std::uint64_t>& values) {
    u64(values.size());
    for(std::size_t offset = 0; offset < values.size();) {
      const auto count = std::min(encoded_.size() / sizeof(std::uint64_t), values.size() - offset);
      for(std::size_t index = 0; index < count; ++index) {
        const auto value = values[offset + index];
        for(std::size_t byte = 0; byte < sizeof(value); ++byte)
          encoded_[index * sizeof(value) + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
      }
      bytes(encoded_.data(), count * sizeof(std::uint64_t));
      offset += count;
    }
  }

  void vector_u8(const std::vector<std::uint8_t>& values) {
    u64(values.size());
    if(!values.empty())
      bytes(values.data(), values.size());
  }

  void finish() {
    const auto checksum_value = checksum_;
    std::array<std::uint8_t, sizeof(checksum_value)> encoded{};
    encode(checksum_value, encoded.data());
    output_.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if(!output_)
      throw std::runtime_error("failed while finishing checkpoint");
  }

private:
  template <class Integer> static void encode(Integer value, std::uint8_t* output) noexcept {
    for(std::size_t byte = 0; byte < sizeof(value); ++byte)
      output[byte] = static_cast<std::uint8_t>(value >> (8U * byte));
  }

  template <class Integer> void integer(Integer value) {
    std::array<std::uint8_t, sizeof(value)> encoded{};
    encode(value, encoded.data());
    bytes(encoded.data(), encoded.size());
  }

  void checksum(const void* data, std::size_t size) noexcept {
    const auto* input = static_cast<const std::uint8_t*>(data);
    for(std::size_t i = 0; i < size; ++i)
      checksum_ = (checksum_ ^ input[i]) * 1099511628211ULL;
  }

  std::ostream& output_;
  std::vector<std::uint8_t> encoded_;
  std::uint64_t checksum_ = 14695981039346656037ULL;
};

class CheckpointReader {
public:
  CheckpointReader(std::istream& input, std::uint64_t size) : input_(input), remaining_(size), encoded_(checkpoint_buffer_size_) {}

  void bytes(void* data, std::size_t size) {
    if(size > remaining_)
      throw std::runtime_error("checkpoint is truncated");
    input_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if(!input_)
      throw std::runtime_error("failed while reading checkpoint");
    checksum(data, size);
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

  [[nodiscard]] std::uint32_t u32() { return integer<std::uint32_t>(); }
  [[nodiscard]] std::uint64_t u64() { return integer<std::uint64_t>(); }

  [[nodiscard]] std::string string() {
    const auto size = u64();
    if(size > remaining_ || size > std::numeric_limits<std::size_t>::max())
      throw std::runtime_error("invalid string size in checkpoint");
    std::string value(static_cast<std::size_t>(size), '\0');
    bytes(value.data(), value.size());
    return value;
  }

  [[nodiscard]] std::vector<std::uint64_t> vector_u64() {
    const auto count = u64();
    if(count > std::numeric_limits<std::size_t>::max() || count > remaining_ / sizeof(std::uint64_t))
      throw std::runtime_error("invalid vector size in checkpoint");
    std::vector<std::uint64_t> values(static_cast<std::size_t>(count));
    for(std::size_t offset = 0; offset < values.size();) {
      const auto chunk = std::min(encoded_.size() / sizeof(std::uint64_t), values.size() - offset);
      bytes(encoded_.data(), chunk * sizeof(std::uint64_t));
      for(std::size_t index = 0; index < chunk; ++index)
        values[offset + index] = decode<std::uint64_t>(encoded_.data() + index * sizeof(std::uint64_t));
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

  [[nodiscard]] std::uint64_t finish() {
    if(remaining_ != sizeof(std::uint64_t))
      throw std::runtime_error("checkpoint has trailing or missing data");
    std::array<std::uint8_t, sizeof(std::uint64_t)> encoded{};
    input_.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if(!input_)
      throw std::runtime_error("checkpoint checksum is truncated");
    remaining_ = 0;
    if(decode<std::uint64_t>(encoded.data()) != checksum_)
      throw std::runtime_error("checkpoint checksum mismatch");
    return checksum_;
  }

private:
  template <class Integer> static Integer decode(const std::uint8_t* input) noexcept {
    Integer value = 0;
    for(std::size_t byte = 0; byte < sizeof(value); ++byte)
      value |= static_cast<Integer>(input[byte]) << (8U * byte);
    return value;
  }

  template <class Integer> Integer integer() {
    std::array<std::uint8_t, sizeof(Integer)> encoded{};
    bytes(encoded.data(), encoded.size());
    return decode<Integer>(encoded.data());
  }

  void checksum(const void* data, std::size_t size) noexcept {
    const auto* input = static_cast<const std::uint8_t*>(data);
    for(std::size_t i = 0; i < size; ++i)
      checksum_ = (checksum_ ^ input[i]) * 1099511628211ULL;
  }

  std::istream& input_;
  std::uint64_t remaining_;
  std::vector<std::uint8_t> encoded_;
  std::uint64_t checksum_ = 14695981039346656037ULL;
};

inline volatile std::sig_atomic_t checkpoint_interrupt_requested_ = 0;

inline void checkpoint_interrupt_handler_(int) {
  std::cerr << "Interrupt requested. Will exit after completing the current row." << std::endl;
  checkpoint_interrupt_requested_ = 1;
}

inline void install_checkpoint_interrupt_handler() {
  checkpoint_interrupt_requested_ = 0;
  if(std::signal(SIGINT, checkpoint_interrupt_handler_) == SIG_ERR)
    throw std::runtime_error("cannot install Ctrl-C handler");
}

inline void Solver::write_config(CheckpointWriter& output, const Options& options) {
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
  output.string(options.search_name);
}

inline int Solver::checkpoint_positive_int(CheckpointReader& input, std::string_view field) {
  const auto value = input.u64();
  if(value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("invalid " + std::string(field) + " in checkpoint");
  }
  return static_cast<int>(value);
}

inline Options Solver::read_config(CheckpointReader& input, std::uint32_t checkpoint_version) {
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
  options.search_name = checkpoint_version >= 5U ? input.string() : "save";
  if(options.rule.empty() || options.geometry.empty() || options.start.empty() || options.savefile.empty()) {
    throw std::runtime_error("checkpoint configuration has an empty required value");
  }
  if(!valid_search_name(options.search_name))
    throw std::runtime_error("checkpoint configuration has an invalid search name");
  return options;
}

template <class T> inline void Solver::merge_immutable(const Options& command_line, const Options& saved, std::string_view name, T Options::* member) {
  if(command_line.explicitly_set.contains(std::string(name)) && command_line.*member != saved.*member) {
    throw std::runtime_error("cannot alter checkpoint search-tree option " + std::string(name));
  }
  options_.*member = saved.*member;
}

template <class T> inline void Solver::merge_mutable(const Options& command_line, const Options& saved, std::string_view name, T Options::* member) {
  if(!command_line.explicitly_set.contains(std::string(name))) {
    options_.*member = saved.*member;
  }
}

inline void Solver::merge_checkpoint_config(const Options& saved) {
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
  merge_mutable(command_line, saved, "search_name", &Options::search_name);
}

inline std::size_t Solver::checkpoint_size(std::uint64_t value, std::string_view field) {
  if(value > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("checkpoint " + std::string(field) + " does not fit this platform");
  }
  return static_cast<std::size_t>(value);
}

inline void Solver::load_checkpoint(const std::string& path) {
  std::vector<char> stream_buffer(checkpoint_buffer_size_);
  std::ifstream input;
  input.rdbuf()->pubsetbuf(stream_buffer.data(), static_cast<std::streamsize>(stream_buffer.size()));
  input.open(path, std::ios::binary | std::ios::ate);
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
  if(magic != checkpoint_magic_ || checkpoint_version < oldest_checkpoint_version_ || checkpoint_version > checkpoint_version_) {
    throw std::runtime_error("unsupported checkpoint format: " + path);
  }

  merge_checkpoint_config(read_config(reader, checkpoint_version));
  exhausted_ = reader.boolean();
  completion_at_current_row_ = reader.boolean();
  width_ = checkpoint_size(reader.u64(), "width");
  height_ = checkpoint_size(reader.u64(), "height");
  const auto slice_count = checkpoint_size(reader.u64(), "slice count");
  if(!reader.boolean())
    throw std::runtime_error("checkpoint uses the removed dense relation representation");
  bcaf_clause_begin_depth_ = checkpoint_size(reader.u64(), "first BCAF clause depth");
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
    auto bcaf_child_clauses = reader.vector_u8();
    auto slice = SuccinctSliceTree::from_checkpoint(std::move(words), std::move(levels), nodes, depth);
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
    if(stored_bits != 0 || !words.empty())
      throw std::runtime_error("checkpoint uses the removed dense pair-gate representation");
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
    pair_gates_.push_back(std::move(gate));
  }
  loaded_checkpoint_fingerprint_ = CheckpointFingerprint{static_cast<std::uint64_t>(end), reader.finish()};
}

inline void Solver::validate_loaded_state() const {
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

inline void Solver::validate_partition_constraint() const {
  if(!options_.partition_constraint.has_value())
    return;
  const auto& partition = *options_.partition_constraint;
  if(exhausted_ || slices_.empty())
    throw std::runtime_error("cannot apply a partition to an exhausted checkpoint");
  if(partition.source != loaded_checkpoint_fingerprint_)
    throw std::runtime_error("partition spec checkpoint fingerprint mismatch");
  if(partition.height != height_ || partition.slice >= slices_.size())
    throw std::runtime_error("partition spec checkpoint dimensions mismatch");
  const auto& tree = slices_[static_cast<std::size_t>(partition.slice)];
  if(partition.source_leaves != tree.leaf_count() || partition.part_count < 2U || partition.part_index >= partition.part_count ||
     partition.first_leaf >= partition.past_leaf || partition.past_leaf > partition.source_leaves || !valid_search_name(partition.search_name)) {
    throw std::runtime_error("partition spec has an invalid leaf range or identity");
  }
}

inline void Solver::apply_partition_restriction(std::size_t position, PackedTags& tags) const {
  if(!options_.partition_constraint.has_value() || options_.partition_constraint->slice != position)
    return;
  const auto& partition = *options_.partition_constraint;
  const auto& tree = slices_[position];
  Node expansion = 0;
  if(tree.leaf_count() == partition.source_leaves) {
    expansion = 1;
  } else if(expanded_uniform_tail_ && partition.source_leaves <= std::numeric_limits<Node>::max() / 4U && tree.leaf_count() == 4U * partition.source_leaves) {
    expansion = 4;
  } else {
    throw std::logic_error("partitioned slice shape changed before its restriction was applied");
  }
  const auto first = tree.leaf_begin() + expansion * partition.first_leaf;
  const auto past = tree.leaf_begin() + expansion * partition.past_leaf;
  tags.clear_range(tree.leaf_begin(), first);
  tags.clear_range(past, tree.leaf_end());
}

[[nodiscard]] inline std::filesystem::path Solver::checkpoint_path() const {
  const std::filesystem::path prefix(options_.savefile);
  std::error_code error;
  const bool directory = std::filesystem::is_directory(prefix, error) || options_.savefile.ends_with('/') || options_.savefile.ends_with('\\');
  if(directory) {
    return prefix / (options_.search_name + "_" + std::to_string(height_));
  }
  return std::filesystem::path(options_.savefile + "_" + std::to_string(height_));
}

inline void Solver::save_checkpoint() {
  if(options_.save_mode == SaveMode::None || last_checkpoint_height_ == height_) {
    return;
  }
  if(options_.partition_constraint.has_value())
    throw std::runtime_error("cannot save before the pending partition restriction has been applied");
  const auto path = checkpoint_path();
  auto temporary = path;
  temporary += ".tmp";
  {
    std::vector<char> stream_buffer(checkpoint_buffer_size_);
    std::ofstream output;
    output.rdbuf()->pubsetbuf(stream_buffer.data(), static_cast<std::streamsize>(stream_buffer.size()));
    output.open(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
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
    writer.boolean(true);
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
      writer.u64(0);
      writer.vector_u64({});
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

inline int Solver::finish(int status) {
  if(options_.save_mode != SaveMode::None)
    save_checkpoint();
  return status;
}

inline int Solver::finish_interrupted() {
  if(options_.partition_constraint.has_value()) {
    std::cout << "interrupt requested before the pending partition restriction was applied; exiting without checkpoint\n";
    return 130;
  }
  if(options_.save_mode == SaveMode::None)
    std::cout << "interrupt requested after completed row " << height_ << "; exiting without checkpoint\n";
  else
    std::cout << "interrupt requested; saving completed row " << height_ << " and exiting\n";
  return finish(130);
}

} // namespace rlife::llsss
