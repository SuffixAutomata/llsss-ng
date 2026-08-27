#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace rlife::llsss {

struct CheckpointFingerprint {
  std::uint64_t size = 0;
  std::uint64_t checksum = 0;

  friend bool operator==(const CheckpointFingerprint&, const CheckpointFingerprint&) = default;
};

// A partition is tied to one exact checkpoint. Leaf ordinals are local to the
// selected slice's final BFS level and therefore require both the checkpoint
// fingerprint and the saved dimensions for validation.
struct PartitionConstraint {
  CheckpointFingerprint source;
  std::uint64_t height = 0;
  std::uint64_t slice = 0;
  std::uint64_t source_leaves = 0;
  std::uint64_t first_leaf = 0;
  std::uint64_t past_leaf = 0;
  std::uint64_t part_index = 0;
  std::uint64_t part_count = 0;
  std::string search_name;
};

struct PartitionSpec {
  std::filesystem::path checkpoint;
  PartitionConstraint constraint;
};

// Returns nullopt for ordinary checkpoint files. A file carrying the
// partition-spec magic is parsed strictly and errors are reported rather than
// falling through to the checkpoint reader.
std::optional<PartitionSpec> try_read_partition_spec(const std::filesystem::path& path);
void write_partition_spec(const std::filesystem::path& path, const PartitionSpec& spec, bool replace);

class PartitionCommand;
int run_partition_command(int argc, char** argv);
void print_partition_help(std::ostream& output);

} // namespace rlife::llsss
