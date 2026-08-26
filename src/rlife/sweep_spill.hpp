#pragma once

#include "sweep_tags.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace rlife::llsss {

struct SweepSpillConfig {
  bool enabled = true;
  std::filesystem::path directory = "scratch";
  std::uint64_t activation_bytes = std::uint64_t{4} << 30U;
  std::uint64_t resident_budget_bytes = std::uint64_t{4} << 30U;
  bool checksum = true;
};

inline std::uint64_t parse_byte_count(std::string_view text) {
  if(text.empty())
    throw std::runtime_error("empty byte count");
  std::uint64_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  if(result.ec != std::errc{} || result.ptr == begin)
    throw std::runtime_error("invalid byte count: " + std::string(text));
  std::string suffix(result.ptr, end);
  std::ranges::transform(suffix, suffix.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  std::uint64_t multiplier = 1;
  if(suffix.empty() || suffix == "b") {
    multiplier = 1;
  } else if(suffix == "k" || suffix == "kb" || suffix == "kib") {
    multiplier = std::uint64_t{1} << 10U;
  } else if(suffix == "m" || suffix == "mb" || suffix == "mib") {
    multiplier = std::uint64_t{1} << 20U;
  } else if(suffix == "g" || suffix == "gb" || suffix == "gib") {
    multiplier = std::uint64_t{1} << 30U;
  } else if(suffix == "t" || suffix == "tb" || suffix == "tib") {
    multiplier = std::uint64_t{1} << 40U;
  } else {
    throw std::runtime_error("invalid byte-count suffix: " + suffix);
  }
  if(value > std::numeric_limits<std::uint64_t>::max() / multiplier)
    throw std::overflow_error("byte count is too large");
  return value * multiplier;
}

struct SweepSpillTelemetry {
  bool activated = false;
  bool direct_io = false;
  std::uint64_t logical_bytes_read = 0;
  std::uint64_t logical_bytes_written = 0;
  std::uint64_t physical_bytes_read = 0;
  std::uint64_t physical_bytes_written = 0;
  std::uint64_t turnaround_retained_bytes = 0;
  std::uint64_t peak_resident_bytes = 0;
  std::uint64_t records_read = 0;
  std::uint64_t records_written = 0;
  double read_seconds = 0;
  double write_seconds = 0;
  double main_read_wait_seconds = 0;
  double main_write_wait_seconds = 0;

  void add(const SweepSpillTelemetry& other) noexcept {
    activated = activated || other.activated;
    direct_io = direct_io || other.direct_io;
    logical_bytes_read += other.logical_bytes_read;
    logical_bytes_written += other.logical_bytes_written;
    physical_bytes_read += other.physical_bytes_read;
    physical_bytes_written += other.physical_bytes_written;
    turnaround_retained_bytes += other.turnaround_retained_bytes;
    peak_resident_bytes = std::max(peak_resident_bytes, other.peak_resident_bytes);
    records_read += other.records_read;
    records_written += other.records_written;
    read_seconds += other.read_seconds;
    write_seconds += other.write_seconds;
    main_read_wait_seconds += other.main_read_wait_seconds;
    main_write_wait_seconds += other.main_write_wait_seconds;
  }
};

struct SweepSuffixLayout {
  using Node = SuccinctSliceTree::Node;

  std::uint64_t normal_bits = 0;
  Node leaf_first = 0;
  Node leaf_bits = 0;
  bool witness = false;
  std::optional<SuccinctSliceTree::NativeLayout> tree;
};

struct SweepSuffixState {
  using Node = SuccinctSliceTree::Node;

  SweepSuffixState() = default;
  explicit SweepSuffixState(const SweepSuffixLayout& layout, bool allocate_tree = false)
      : normal(layout.normal_bits), witness(layout.witness ? LeafTags(layout.leaf_first, layout.leaf_bits) : LeafTags{}) {
    if(allocate_tree && layout.tree)
      tree = SuccinctSliceTree::allocate_native_image(*layout.tree);
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    auto result = normal.allocated_bytes() + witness.allocated_bytes();
    if(tree) {
      result += tree->words.capacity() * sizeof(tree->words[0]) + tree->absolute_rank.capacity() * sizeof(tree->absolute_rank[0]) +
                tree->relative_rank.capacity() * sizeof(tree->relative_rank[0]) + tree->levels.capacity() * sizeof(tree->levels[0]) +
                tree->bcaf_bytes.capacity() * sizeof(tree->bcaf_bytes[0]);
    }
    return result;
  }

  PackedTags normal;
  LeafTags witness;
  std::optional<SuccinctSliceTree::NativeImage> tree;
};

// Owns all residency decisions for the reverse-sweep records. Each record
// contains the expanded slice-tree image and its completed suffix tag planes.
// Search code submits a finished record, announces the forward
// turnaround/step, and receives a loaded lease. The worker, prefetch distance,
// turnaround cache, byte backpressure, native file format and checksums remain
// encapsulated here.
class SweepSpill {
public:
  using Clock = std::chrono::steady_clock;

  class Lease {
  public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept { move_from(std::move(other)); }
    Lease& operator=(Lease&& other) noexcept {
      if(this != &other) {
        reset();
        move_from(std::move(other));
      }
      return *this;
    }
    ~Lease() { reset(); }

    SweepSuffixState& state() noexcept { return *state_; }
    const SweepSuffixState& state() const noexcept { return *state_; }
    SweepSuffixState* operator->() noexcept { return state_.get(); }
    const SweepSuffixState* operator->() const noexcept { return state_.get(); }
    explicit operator bool() const noexcept { return state_ != nullptr; }

  private:
    friend class SweepSpill;
    Lease(SweepSpill* owner, std::size_t index, std::unique_ptr<SweepSuffixState> state, std::uint64_t bytes)
        : owner_(owner), index_(index), state_(std::move(state)), bytes_(bytes) {}

    void move_from(Lease&& other) noexcept {
      owner_ = std::exchange(other.owner_, nullptr);
      index_ = other.index_;
      state_ = std::move(other.state_);
      bytes_ = std::exchange(other.bytes_, 0);
    }
    void reset() noexcept {
      state_.reset();
      if(owner_ != nullptr)
        owner_->release_lease(index_, bytes_);
      owner_ = nullptr;
      bytes_ = 0;
    }

    SweepSpill* owner_ = nullptr;
    std::size_t index_ = 0;
    std::unique_ptr<SweepSuffixState> state_;
    std::uint64_t bytes_ = 0;
  };

  SweepSpill(const SweepSpillConfig& config, std::vector<SweepSuffixLayout> layouts, SweepSpillTelemetry& sink)
      : config_(config), sink_(&sink) {
    slots_.reserve(layouts.size());
    std::uint64_t total_bytes = 0;
    for(const auto& layout : layouts) {
      Slot slot;
      slot.layout = layout;
      slot.logical_bytes = checked_logical_bytes(layout);
      if(total_bytes > std::numeric_limits<std::uint64_t>::max() - slot.logical_bytes)
        throw std::overflow_error("sweep suffix payload is too large");
      total_bytes += slot.logical_bytes;
      slots_.push_back(std::move(slot));
    }

    telemetry_.activated = config_.enabled && !slots_.empty() && current_resident_bytes() + total_bytes > config_.activation_bytes;
    if(!telemetry_.activated)
      return;
    if(config_.resident_budget_bytes == 0)
      throw std::runtime_error("spill resident budget must be positive");

    // Records consumed immediately after the reverse-to-forward turnaround
    // are poor eviction candidates. Reserve half the byte budget for the
    // largest contiguous prefix that fits; the other half remains available
    // to drain reverse writes and prefetch the first genuinely spilled record.
    const auto turnaround_budget = config_.resident_budget_bytes / 2U;
    std::uint64_t retained = 0;
    for(auto& slot : slots_) {
      if(slot.logical_bytes > turnaround_budget - std::min(turnaround_budget, retained))
        break;
      slot.keep_at_turnaround = true;
      retained += slot.logical_bytes;
    }
    if(!slots_.empty() && retained == 0 && slots_.front().logical_bytes <= config_.resident_budget_bytes) {
      slots_.front().keep_at_turnaround = true;
      retained = slots_.front().logical_bytes;
    }
    telemetry_.turnaround_retained_bytes = retained;

    std::uint64_t offset = 0;
    for(auto& slot : slots_) {
      if(slot.keep_at_turnaround)
        continue;
      slot.file_offset = offset;
      slot.disk_bytes = align_up(slot.logical_bytes, io_alignment);
      if(offset > std::numeric_limits<std::uint64_t>::max() - slot.disk_bytes)
        throw std::overflow_error("sweep spill file is too large");
      offset += slot.disk_bytes;
    }
    open_file(offset);
    try {
      worker_ = std::thread([this] { worker_loop(); });
    } catch(...) {
      close_file();
      throw;
    }
  }

  SweepSpill(const SweepSpill&) = delete;
  SweepSpill& operator=(const SweepSpill&) = delete;

  ~SweepSpill() {
    if(worker_.joinable()) {
      {
        std::lock_guard lock(mutex_);
        stopping_ = true;
      }
      work_cv_.notify_all();
      room_cv_.notify_all();
      worker_.join();
    }
    close_file();
    if(sink_ != nullptr)
      sink_->add(telemetry_);
  }

  [[nodiscard]] bool active() const noexcept { return telemetry_.activated; }

  void submit(std::size_t index, SweepSuffixState state) {
    if(index >= slots_.size())
      throw std::out_of_range("sweep suffix index");
    auto payload = std::make_unique<SweepSuffixState>(std::move(state));
    auto& slot = slots_[index];
    const bool tree_shape_valid = [&] {
      if(!active() && slot.layout.tree && !payload->tree)
        return true;
      if(slot.layout.tree.has_value() != payload->tree.has_value())
        return false;
      if(!slot.layout.tree)
        return true;
      const auto& expected = *slot.layout.tree;
      const auto& image = *payload->tree;
      return image.words.size() == expected.word_count && image.absolute_rank.size() == expected.absolute_rank_count &&
             image.relative_rank.size() == expected.relative_rank_count && image.levels.size() == expected.level_count &&
             image.bcaf_bytes.size() == expected.bcaf_byte_count && image.layout.node_count == expected.node_count && image.layout.depth == expected.depth;
    }();
    if(payload->normal.size() != slot.layout.normal_bits || payload->witness.bits.size() != (slot.layout.witness ? slot.layout.leaf_bits : 0) || !tree_shape_valid)
      throw std::logic_error("submitted sweep suffix does not match its layout");

    std::unique_lock lock(mutex_);
    rethrow_worker_error();
    if(slot.status != Status::Empty)
      throw std::logic_error("sweep suffix submitted twice");
    if(!active()) {
      slot.payload = std::move(payload);
      slot.status = Status::Resident;
      return;
    }

    const auto wait_started = Clock::now();
    room_cv_.wait(lock, [&] { return failed_ || room_for(slot.logical_bytes); });
    telemetry_.main_write_wait_seconds += std::chrono::duration<double>(Clock::now() - wait_started).count();
    rethrow_worker_error();
    resident_bytes_ += slot.logical_bytes;
    update_peak_resident();
    slot.payload = std::move(payload);
    if(slot.keep_at_turnaround) {
      slot.status = Status::Resident;
      ready_cv_.notify_all();
    } else {
      slot.status = Status::WriteQueued;
      jobs_.push_back(Job{index, false});
      work_cv_.notify_one();
    }
  }

  // At the turnaround, make the first adjacent pair ready. The internal
  // policy may satisfy this from the retained prefix, an in-flight write, or
  // an actual disk read.
  void begin_forward() {
    if(slots_.empty())
      return;
    if(slots_.size() > 1)
      request(1, true);
    request(0, true);
  }

  // While relation i computes, one record beyond its right endpoint is the
  // earliest useful read. One-record lookahead is deliberate: it gives the
  // device a full relation walk to respond without evicting farther-future
  // records into RAM merely because they fit at one instant.
  void at_forward_step(std::size_t relation) {
    const auto next = relation + 2U;
    if(next < slots_.size())
      request(next, false);
  }

  Lease acquire(std::size_t index) {
    if(index >= slots_.size())
      throw std::out_of_range("sweep suffix index");
    const auto started = Clock::now();
    request(index, true);
    std::unique_lock lock(mutex_);
    auto& slot = slots_[index];
    ready_cv_.wait(lock, [&] { return failed_ || slot.status == Status::Resident; });
    telemetry_.main_read_wait_seconds += std::chrono::duration<double>(Clock::now() - started).count();
    rethrow_worker_error();
    slot.status = Status::Leased;
    return Lease(this, index, std::move(slot.payload), slot.logical_bytes);
  }

private:
  static constexpr std::uint64_t io_alignment = 4096;
  static constexpr std::size_t staging_bytes = 8U << 20U;

  enum class Status : std::uint8_t { Empty, WriteQueued, Writing, Spilled, ReadQueued, Reading, Resident, Leased, Consumed, Failed };

  struct Slot {
    SweepSuffixLayout layout;
    std::uint64_t logical_bytes = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t file_offset = 0;
    std::uint64_t checksum = 0;
    bool keep_at_turnaround = false;
    bool wanted_after_write = false;
    Status status = Status::Empty;
    std::unique_ptr<SweepSuffixState> payload;
  };

  struct Job {
    std::size_t index = 0;
    bool read = false;
  };

  static std::uint64_t checked_logical_bytes(const SweepSuffixLayout& layout) {
    const auto normal_words = (layout.normal_bits + 63U) / 64U;
    const auto witness_words = layout.witness ? (layout.leaf_bits + 63U) / 64U : 0U;
    if(normal_words > std::numeric_limits<std::uint64_t>::max() - witness_words || normal_words + witness_words > std::numeric_limits<std::uint64_t>::max() / 8U)
      throw std::overflow_error("sweep suffix record is too large");
    const auto tag_bytes = (normal_words + witness_words) * 8U;
    const auto tree_bytes = layout.tree ? layout.tree->byte_count() : 0U;
    if(tag_bytes > std::numeric_limits<std::uint64_t>::max() - tree_bytes)
      throw std::overflow_error("sweep residency record is too large");
    return tag_bytes + tree_bytes;
  }

  static std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    if(value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1U))
      throw std::overflow_error("aligned sweep spill record is too large");
    return (value + alignment - 1U) & ~(alignment - 1U);
  }

  static std::uint64_t current_resident_bytes() noexcept {
#ifdef __linux__
    std::ifstream input("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    if(input >> pages >> resident) {
      const auto page_size = ::sysconf(_SC_PAGESIZE);
      if(page_size > 0 && resident <= std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(page_size))
        return resident * static_cast<std::uint64_t>(page_size);
    }
#endif
    return 0;
  }

  void open_file(std::uint64_t bytes) {
#ifdef __linux__
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    if(error)
      throw std::runtime_error("cannot create spill directory " + config_.directory.string() + ": " + error.message());
    static std::atomic<std::uint64_t> sequence{0};
    const auto name = "rlife-sweep-" + std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    file_path_ = config_.directory / name;
    const int common = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC;
    file_descriptor_ = ::open(file_path_.c_str(), common, 0600);
    telemetry_.direct_io = false;
    if(file_descriptor_ < 0)
      throw std::system_error(errno, std::generic_category(), "cannot create sweep spill file " + file_path_.string());
#ifdef O_DIRECT
    const auto direct_descriptor = ::open(file_path_.c_str(), O_RDWR | O_CLOEXEC | O_DIRECT);
    if(direct_descriptor >= 0) {
      ::close(file_descriptor_);
      file_descriptor_ = direct_descriptor;
      telemetry_.direct_io = true;
    }
#endif
    if(bytes != 0) {
      const auto allocation = ::posix_fallocate(file_descriptor_, 0, static_cast<off_t>(bytes));
      if(allocation != 0) {
        close_file();
        throw std::system_error(allocation, std::generic_category(), "cannot allocate sweep spill file " + file_path_.string());
      }
    }
#else
    (void)bytes;
    throw std::runtime_error("external sweep residency is currently supported on Linux only");
#endif
  }

  void close_file() noexcept {
#ifdef __linux__
    if(file_descriptor_ >= 0) {
      ::close(file_descriptor_);
      file_descriptor_ = -1;
    }
    if(!file_path_.empty())
      ::unlink(file_path_.c_str());
#endif
  }

  [[nodiscard]] bool room_for(std::uint64_t bytes) const noexcept {
    if(resident_bytes_ == 0 && bytes > config_.resident_budget_bytes)
      return true;
    return bytes <= config_.resident_budget_bytes && resident_bytes_ <= config_.resident_budget_bytes - bytes;
  }

  void update_peak_resident() noexcept { telemetry_.peak_resident_bytes = std::max(telemetry_.peak_resident_bytes, resident_bytes_); }

  void request(std::size_t index, bool required) {
    std::unique_lock lock(mutex_);
    rethrow_worker_error();
    auto& slot = slots_[index];
    switch(slot.status) {
    case Status::Resident:
    case Status::ReadQueued:
    case Status::Reading:
    case Status::Leased:
      return;
    case Status::WriteQueued: {
      slot.wanted_after_write = true;
      const auto pending = std::ranges::find_if(jobs_, [&](const Job& job) { return !job.read && job.index == index; });
      if(pending != jobs_.end()) {
        const auto job = *pending;
        jobs_.erase(pending);
        jobs_.push_front(job);
        work_cv_.notify_one();
      }
      return;
    }
    case Status::Writing:
      slot.wanted_after_write = true;
      return;
    case Status::Spilled:
      if(!required && !room_for(slot.logical_bytes))
        return;
      resident_bytes_ += slot.logical_bytes;
      update_peak_resident();
      slot.status = Status::ReadQueued;
      jobs_.push_front(Job{index, true});
      work_cv_.notify_one();
      return;
    case Status::Empty:
      throw std::logic_error("requested sweep suffix before submission");
    case Status::Consumed:
      throw std::logic_error("requested consumed sweep suffix");
    case Status::Failed:
      rethrow_worker_error();
      break;
    }
  }

  void release_lease(std::size_t index, std::uint64_t bytes) noexcept {
    std::lock_guard lock(mutex_);
    auto& slot = slots_[index];
    slot.status = Status::Consumed;
    resident_bytes_ -= std::min(resident_bytes_, bytes);
    room_cv_.notify_all();
  }

  void rethrow_worker_error() const {
    if(failed_)
      std::rethrow_exception(failed_);
  }

  static std::vector<std::span<const std::byte>> const_segments(const SweepSuffixState& state) {
    std::vector<std::span<const std::byte>> result;
    result.reserve(state.tree ? 7U : 2U);
    auto add = [&](const auto& values) { result.push_back(std::as_bytes(std::span(values))); };
    add(state.normal.checkpoint_words());
    add(state.witness.bits.checkpoint_words());
    if(state.tree) {
      add(state.tree->words);
      add(state.tree->absolute_rank);
      add(state.tree->relative_rank);
      add(state.tree->levels);
      add(state.tree->bcaf_bytes);
    }
    return result;
  }

  static std::vector<std::span<std::byte>> mutable_segments(SweepSuffixState& state) {
    std::vector<std::span<std::byte>> result;
    result.reserve(state.tree ? 7U : 2U);
    auto add = [&](auto& values) { result.push_back(std::as_writable_bytes(std::span(values))); };
    add(state.normal.native_io_words());
    add(state.witness.bits.native_io_words());
    if(state.tree) {
      add(state.tree->words);
      add(state.tree->absolute_rank);
      add(state.tree->relative_rank);
      add(state.tree->levels);
      add(state.tree->bcaf_bytes);
    }
    return result;
  }

  static std::uint64_t checksum_words(const SweepSuffixState& state) {
    constexpr std::uint64_t prime = 0x9e3779b185ebca87ULL;
    std::array<std::uint64_t, 4> lanes = {0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL, 0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL};
    std::size_t ordinal = 0;
    for(const auto segment : const_segments(state)) {
      std::size_t offset = 0;
      while(offset < segment.size()) {
        std::uint64_t word = 0;
        const auto amount = std::min<std::size_t>(sizeof(word), segment.size() - offset);
        std::memcpy(&word, segment.data() + offset, amount);
        auto& lane = lanes[ordinal++ & 3U];
        lane = std::rotl(lane ^ word, 27) * prime;
        offset += amount;
      }
    }
    auto result = lanes[0] ^ std::rotl(lanes[1], 13) ^ std::rotl(lanes[2], 29) ^ std::rotl(lanes[3], 47) ^ ordinal;
    result ^= result >> 30U;
    result *= 0xbf58476d1ce4e5b9ULL;
    result ^= result >> 27U;
    result *= 0x94d049bb133111ebULL;
    return result ^ (result >> 31U);
  }

  static void copy_from_state(const SweepSuffixState& state, std::uint64_t offset, std::byte* output, std::size_t count) {
    for(const auto segment : const_segments(state)) {
      if(offset >= segment.size()) {
        offset -= segment.size();
        continue;
      }
      const auto amount = std::min<std::size_t>(count, segment.size() - static_cast<std::size_t>(offset));
      std::memcpy(output, segment.data() + offset, amount);
      output += amount;
      count -= amount;
      offset = 0;
      if(count == 0)
        return;
    }
    throw std::logic_error("sweep spill source range is out of bounds");
  }

  static void copy_to_state(SweepSuffixState& state, std::uint64_t offset, const std::byte* input, std::size_t count) {
    for(auto segment : mutable_segments(state)) {
      if(offset >= segment.size()) {
        offset -= segment.size();
        continue;
      }
      const auto amount = std::min<std::size_t>(count, segment.size() - static_cast<std::size_t>(offset));
      std::memcpy(segment.data() + offset, input, amount);
      input += amount;
      count -= amount;
      offset = 0;
      if(count == 0)
        return;
    }
    throw std::logic_error("sweep spill destination range is out of bounds");
  }

  void transfer(bool read, std::uint64_t offset, std::byte* buffer, std::size_t count) {
#ifdef __linux__
    std::size_t done = 0;
    while(done < count) {
      const auto result = read ? ::pread(file_descriptor_, buffer + done, count - done, static_cast<off_t>(offset + done))
                               : ::pwrite(file_descriptor_, buffer + done, count - done, static_cast<off_t>(offset + done));
      if(result < 0 && errno == EINTR)
        continue;
      if(result < 0)
        throw std::system_error(errno, std::generic_category(), read ? "sweep spill read failed" : "sweep spill write failed");
      if(result == 0)
        throw std::runtime_error(read ? "unexpected end of sweep spill file" : "short sweep spill write");
      done += static_cast<std::size_t>(result);
    }
#else
    (void)read;
    (void)offset;
    (void)buffer;
    (void)count;
#endif
  }

  std::unique_ptr<std::byte, void (*)(std::byte*)> staging_buffer() {
    void* memory = nullptr;
#ifdef __linux__
    const auto result = ::posix_memalign(&memory, io_alignment, staging_bytes);
    if(result != 0)
      throw std::system_error(result, std::generic_category(), "cannot allocate sweep spill staging buffer");
#endif
    return {static_cast<std::byte*>(memory), [](std::byte* pointer) { std::free(pointer); }};
  }

  void write_record(Slot& slot, const SweepSuffixState& state) {
    const auto started = Clock::now();
    if(config_.checksum)
      slot.checksum = checksum_words(state);
    auto buffer = staging_buffer();
    for(std::uint64_t position = 0; position < slot.disk_bytes; position += staging_bytes) {
      const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(staging_bytes, slot.disk_bytes - position));
      std::memset(buffer.get(), 0, chunk);
      if(position < slot.logical_bytes) {
        const auto logical = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, slot.logical_bytes - position));
        copy_from_state(state, position, buffer.get(), logical);
      }
      transfer(false, slot.file_offset + position, buffer.get(), chunk);
    }
    telemetry_.logical_bytes_written += slot.logical_bytes;
    telemetry_.physical_bytes_written += slot.disk_bytes;
    ++telemetry_.records_written;
    telemetry_.write_seconds += std::chrono::duration<double>(Clock::now() - started).count();
  }

  std::unique_ptr<SweepSuffixState> read_record(Slot& slot) {
    const auto started = Clock::now();
    auto state = std::make_unique<SweepSuffixState>(slot.layout, true);
    auto buffer = staging_buffer();
    for(std::uint64_t position = 0; position < slot.disk_bytes; position += staging_bytes) {
      const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(staging_bytes, slot.disk_bytes - position));
      transfer(true, slot.file_offset + position, buffer.get(), chunk);
      if(position < slot.logical_bytes) {
        const auto logical = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, slot.logical_bytes - position));
        copy_to_state(*state, position, buffer.get(), logical);
      }
    }
    if(config_.checksum && checksum_words(*state) != slot.checksum)
      throw std::runtime_error("sweep spill checksum mismatch");
    telemetry_.logical_bytes_read += slot.logical_bytes;
    telemetry_.physical_bytes_read += slot.disk_bytes;
    ++telemetry_.records_read;
    telemetry_.read_seconds += std::chrono::duration<double>(Clock::now() - started).count();
    return state;
  }

  void worker_loop() noexcept {
    for(;;) {
      Job job;
      std::unique_ptr<SweepSuffixState> writing;
      {
        std::unique_lock lock(mutex_);
        work_cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if(jobs_.empty()) {
          if(stopping_)
            return;
          continue;
        }
        job = jobs_.front();
        jobs_.pop_front();
        auto& slot = slots_[job.index];
        if(job.read) {
          slot.status = Status::Reading;
        } else {
          slot.status = Status::Writing;
          writing = std::move(slot.payload);
        }
      }

      try {
        auto& slot = slots_[job.index];
        if(job.read) {
          auto loaded = read_record(slot);
          std::lock_guard lock(mutex_);
          slot.payload = std::move(loaded);
          slot.status = Status::Resident;
          ready_cv_.notify_all();
        } else {
          write_record(slot, *writing);
          std::lock_guard lock(mutex_);
          if(slot.wanted_after_write) {
            slot.payload = std::move(writing);
            slot.status = Status::Resident;
            ready_cv_.notify_all();
          } else {
            slot.status = Status::Spilled;
            resident_bytes_ -= std::min(resident_bytes_, slot.logical_bytes);
            room_cv_.notify_all();
          }
        }
      } catch(...) {
        std::lock_guard lock(mutex_);
        failed_ = std::current_exception();
        slots_[job.index].status = Status::Failed;
        jobs_.clear();
        ready_cv_.notify_all();
        room_cv_.notify_all();
        return;
      }
    }
  }

  SweepSpillConfig config_;
  SweepSpillTelemetry* sink_ = nullptr;
  SweepSpillTelemetry telemetry_;
  std::vector<Slot> slots_;
  std::deque<Job> jobs_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable ready_cv_;
  std::condition_variable room_cv_;
  std::thread worker_;
  std::exception_ptr failed_;
  std::uint64_t resident_bytes_ = 0;
  bool stopping_ = false;
  std::filesystem::path file_path_;
  int file_descriptor_ = -1;
};

} // namespace rlife::llsss
