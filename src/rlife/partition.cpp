#include "rlife/partition.hpp"

#include "rlife/solver.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rlife::llsss {

namespace {

constexpr std::string_view partition_spec_magic = "RLIFE-PARTITION-SPEC";
constexpr std::uint64_t partition_spec_version = 1;

void expect_key(std::istream& input, std::string_view expected) {
  std::string actual;
  if(!(input >> actual) || actual != expected)
    throw std::runtime_error("invalid partition spec: expected " + std::string(expected));
}

std::uint64_t parse_hex_u64(const std::string& text) {
  if(text.size() != 16U)
    throw std::runtime_error("invalid partition spec checkpoint checksum");
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if(result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::runtime_error("invalid partition spec checkpoint checksum");
  return value;
}

std::size_t positive_size(std::string_view option, const std::string& text) {
  std::size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if(text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
    throw std::runtime_error(std::string(option) + " must be a positive integer");
  return value;
}

double boundary_slack_percent(const std::string& text) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stod(text, &consumed);
    if(consumed != text.size() || !std::isfinite(value) || value < 0.0 || value >= 50.0)
      throw std::out_of_range("partition boundary slack");
    return value;
  } catch(const std::exception&) {
    throw std::runtime_error("--boundary-slack must be a percentage in [0,50)");
  }
}

std::string replace_name_placeholder(std::string pattern, std::string_view name, std::string_view option, std::size_t parts) {
  constexpr std::string_view placeholder = "{name}";
  bool replaced = false;
  for(std::size_t position = 0; (position = pattern.find(placeholder, position)) != std::string::npos;) {
    pattern.replace(position, placeholder.size(), name);
    position += name.size();
    replaced = true;
  }
  if(parts > 1U && !pattern.empty() && !replaced)
    throw std::runtime_error(std::string(option) + " must contain {name} when materializing more than one partition");
  return pattern;
}

} // namespace

std::optional<PartitionSpec> try_read_partition_spec(const std::filesystem::path& path) {
  std::ifstream input(path);
  if(!input)
    return std::nullopt;

  std::array<char, partition_spec_magic.size()> magic{};
  input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if(input.gcount() != static_cast<std::streamsize>(magic.size()) || !std::equal(magic.begin(), magic.end(), partition_spec_magic.begin()))
    return std::nullopt;
  const auto separator = input.peek();
  if(separator == std::char_traits<char>::eof() || std::isspace(static_cast<unsigned char>(separator)) == 0)
    throw std::runtime_error("invalid partition spec header: " + path.string());
  std::uint64_t version = 0;
  if(!(input >> version) || version != partition_spec_version)
    throw std::runtime_error("unsupported partition spec format: " + path.string());

  PartitionSpec spec;
  std::string checkpoint;
  expect_key(input, "checkpoint");
  if(!(input >> std::quoted(checkpoint)) || checkpoint.empty())
    throw std::runtime_error("invalid partition spec checkpoint path");
  expect_key(input, "checkpoint_size");
  if(!(input >> spec.constraint.source.size))
    throw std::runtime_error("invalid partition spec checkpoint size");
  expect_key(input, "checkpoint_checksum");
  std::string checksum;
  if(!(input >> checksum))
    throw std::runtime_error("invalid partition spec checkpoint checksum");
  spec.constraint.source.checksum = parse_hex_u64(checksum);
  expect_key(input, "height");
  if(!(input >> spec.constraint.height))
    throw std::runtime_error("invalid partition spec height");
  expect_key(input, "slice");
  if(!(input >> spec.constraint.slice))
    throw std::runtime_error("invalid partition spec slice");
  expect_key(input, "source_leaves");
  if(!(input >> spec.constraint.source_leaves))
    throw std::runtime_error("invalid partition spec source leaf count");
  expect_key(input, "range");
  if(!(input >> spec.constraint.first_leaf >> spec.constraint.past_leaf))
    throw std::runtime_error("invalid partition spec leaf range");
  expect_key(input, "part");
  if(!(input >> spec.constraint.part_index >> spec.constraint.part_count))
    throw std::runtime_error("invalid partition spec part identity");
  expect_key(input, "search_name");
  if(!(input >> std::quoted(spec.constraint.search_name)) || !valid_search_name(spec.constraint.search_name))
    throw std::runtime_error("invalid partition spec search name");
  input >> std::ws;
  if(input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("partition spec has trailing data");

  auto checkpoint_path = std::filesystem::path(checkpoint);
  if(checkpoint_path.is_relative())
    checkpoint_path = std::filesystem::absolute(path).parent_path() / checkpoint_path;
  spec.checkpoint = checkpoint_path.lexically_normal();
  return spec;
}

void write_partition_spec(const std::filesystem::path& path, const PartitionSpec& spec, bool replace) {
  std::error_code error;
  if(!replace && std::filesystem::exists(path, error))
    throw std::runtime_error("partition output already exists: " + path.string());

  auto directory = std::filesystem::absolute(path).parent_path();
  auto checkpoint = std::filesystem::absolute(spec.checkpoint);
  auto stored_checkpoint = std::filesystem::relative(checkpoint, directory, error);
  if(error || stored_checkpoint.empty()) {
    error.clear();
    stored_checkpoint = checkpoint;
  }

  auto temporary = path;
  temporary += ".tmp";
  std::ofstream output(temporary, std::ios::out | std::ios::trunc);
  if(!output)
    throw std::runtime_error("cannot open partition spec for writing: " + temporary.string());
  output << partition_spec_magic << ' ' << partition_spec_version << '\n';
  output << "checkpoint " << std::quoted(stored_checkpoint.generic_string()) << '\n';
  output << "checkpoint_size " << spec.constraint.source.size << '\n';
  output << "checkpoint_checksum " << std::hex << std::setw(16) << std::setfill('0') << spec.constraint.source.checksum << std::dec << '\n';
  output << "height " << spec.constraint.height << '\n';
  output << "slice " << spec.constraint.slice << '\n';
  output << "source_leaves " << spec.constraint.source_leaves << '\n';
  output << "range " << spec.constraint.first_leaf << ' ' << spec.constraint.past_leaf << '\n';
  output << "part " << spec.constraint.part_index << ' ' << spec.constraint.part_count << '\n';
  output << "search_name " << std::quoted(spec.constraint.search_name) << '\n';
  output.close();
  if(!output)
    throw std::runtime_error("cannot finish partition spec: " + temporary.string());
  std::filesystem::rename(temporary, path, error);
  if(error)
    throw std::runtime_error("cannot replace partition spec " + path.string() + ": " + error.message());
}

void print_partition_help(std::ostream& output) {
  output << R"(rlife partition --load CHECKPOINT --parts N [options]

Partition one checkpoint by contiguous leaf intervals in a selected slice.
By default this writes small .rlp specifiers; --materialize loads each part,
runs one ordinary extend/prune/report row, and writes child checkpoints.

  --load FILE                source checkpoint (required)
  --parts N                  number of partitions (required)
  --slice auto|INDEX         one-based slice to partition (default: auto)
  --search-name NAME         parent identity for NAME-1, NAME-2, ...
  --output DIRECTORY         output directory (default: FILE.parts)
  --boundary-slack PERCENT   endpoint search radius per ideal part (default: 1)
  --materialize              write checkpoints after one ordinary row
  --dry-run                  print the plan without writing files
  --force                    replace existing partition outputs
  --                         pass following llsss runtime options while materializing
  -h, --help                 show this help

Runtime --save, --savedir, --search-name, --halts, and --load are controlled
by the partition command. For per-child --partial-output or
--dump-slice-stats or --status-output paths, include {name} in the path.
)";
}

class PartitionCommand {
public:
  using Node = SuccinctSliceTree::Node;

  static int run(int argc, char** argv) {
    const auto config = parse(argc, argv);
    if(auto nested = try_read_partition_spec(config.load))
      throw std::runtime_error("partition requires a materialized checkpoint, not a partition spec: " + config.load.string());

    Options inspection_options;
    inspection_options.loadfile = std::filesystem::absolute(config.load).string();
    inspection_options.save_mode = SaveMode::None;
    inspection_options.worker_count = 1;
    inspection_options.inspection_only = true;
    inspection_options.explicitly_set = {"save", "threads"};
    auto source = std::make_unique<Solver>(std::move(inspection_options));
    if(source->exhausted_ || source->slices_.empty() || source->options_.partition_constraint.has_value())
      throw std::runtime_error("cannot partition an exhausted or pending-partition checkpoint");

    const auto slice_index = choose_slice(*source, config);
    const auto& tree = source->slices_[slice_index];
    if(config.parts > tree.leaf_count())
      throw std::runtime_error("--parts exceeds the selected slice's leaf count");
    const auto parent_name = config.search_name.value_or(source->options_.search_name);
    if(!valid_search_name(parent_name))
      throw std::runtime_error("partition search name must be a nonempty filename component");

    const auto plan = make_plan(tree, config.parts, config.slack_percent, parent_name);
    print_plan(*source, slice_index, plan, config.slack_percent);

    auto output_directory = config.output.value_or(default_output_directory(config.load));
    output_directory = std::filesystem::absolute(output_directory).lexically_normal();
    if(config.dry_run) {
      std::cout << "dry run: outputs would be written under " << output_directory.string() << '\n';
      return 0;
    }
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if(error)
      throw std::runtime_error("cannot create partition output directory " + output_directory.string() + ": " + error.message());

    const auto source_path = std::filesystem::absolute(config.load).lexically_normal();
    std::vector<PartitionSpec> specs;
    specs.reserve(plan.parts.size());
    for(std::size_t index = 0; index < plan.parts.size(); ++index) {
      PartitionSpec spec;
      spec.checkpoint = source_path;
      spec.constraint.source = source->loaded_checkpoint_fingerprint_;
      spec.constraint.height = source->height_;
      spec.constraint.slice = slice_index;
      spec.constraint.source_leaves = tree.leaf_count();
      spec.constraint.first_leaf = plan.parts[index].first;
      spec.constraint.past_leaf = plan.parts[index].past;
      spec.constraint.part_index = index;
      spec.constraint.part_count = plan.parts.size();
      spec.constraint.search_name = plan.parts[index].name;
      specs.push_back(std::move(spec));
    }

    const auto source_height = source->height_;
    const auto inherited_partial_output = source->options_.partial_output;
    const auto inherited_stats_output = source->options_.stats_output;
    source.reset();

    preflight_outputs(source_path, output_directory, specs, source_height, config.materialize, config.force);
    if(!config.materialize) {
      for(const auto& spec : specs) {
        const auto path = output_directory / (spec.constraint.search_name + ".rlp");
        write_partition_spec(path, spec, config.force);
        std::cout << "partition spec saved: " << path.string() << '\n';
      }
      return 0;
    }

    auto materialize_template = parse_materialize_options(source_path, config.runtime_arguments);
    const bool explicit_partial_output = materialize_template.explicitly_set.contains("partial_output");
    const bool explicit_stats_output = materialize_template.explicitly_set.contains("stats_output");
    const bool explicit_status_output = materialize_template.explicitly_set.contains("status_output");
    for(const auto& spec : specs) {
      auto child = materialize_template;
      child.loadfile = source_path.string();
      child.partition_constraint = spec.constraint;
      child.row_limit = 1U;
      child.halt_height = -1;
      child.save_mode = SaveMode::Final;
      child.search_name = spec.constraint.search_name;
      child.savedir = output_directory.string();
      child.explicitly_set.insert("halt_height");
      child.explicitly_set.insert("save");
      child.explicitly_set.insert("savedir");
      child.explicitly_set.insert("search_name");
      if(explicit_partial_output) {
        child.partial_output = replace_name_placeholder(child.partial_output, child.search_name, "--partial-output", specs.size());
      } else if(!inherited_partial_output.empty()) {
        child.partial_output = (output_directory / (child.search_name + ".rle")).string();
        child.explicitly_set.insert("partial_output");
      }
      if(explicit_stats_output) {
        child.stats_output = replace_name_placeholder(child.stats_output, child.search_name, "--dump-slice-stats", specs.size());
      } else if(!inherited_stats_output.empty()) {
        child.stats_output = (output_directory / (child.search_name + ".stats")).string();
        child.explicitly_set.insert("stats_output");
      }
      if(explicit_status_output)
        child.status_output = replace_name_placeholder(child.status_output, child.search_name, "--status-output", specs.size());
      ensure_not_source_output(child.partial_output, "partial output", source_path);
      ensure_not_source_output(child.stats_output, "slice-stats output", source_path);
      ensure_not_source_output(child.status_output, "status output", source_path);
      std::cout << "materializing partition " << child.search_name << '\n';
      const auto status = Solver(std::move(child)).run();
      if(status != 0)
        return status;
    }
    return 0;
  }

private:
  struct Config {
    std::filesystem::path load;
    std::size_t parts = 0;
    std::optional<std::size_t> slice;
    std::optional<std::string> search_name;
    std::optional<std::filesystem::path> output;
    double slack_percent = 1.0;
    bool materialize = false;
    bool dry_run = false;
    bool force = false;
    std::vector<std::string> runtime_arguments;
  };

  struct PlannedPart {
    Node first = 0;
    Node past = 0;
    Node spanning_nodes = 0;
    std::string name;
  };

  struct PlannedBoundary {
    Node ideal = 0;
    Node selected = 0;
    std::size_t ideal_lca_depth = 0;
    std::size_t selected_lca_depth = 0;
  };

  struct Plan {
    std::vector<PlannedPart> parts;
    std::vector<PlannedBoundary> boundaries;
    Node aggregate_spanning_nodes = 0;
  };

  static Config parse(int argc, char** argv) {
    Config config;
    int index = 2;
    auto argument = [&](std::string_view option) {
      if(index >= argc)
        throw std::runtime_error("missing argument for " + std::string(option));
      return std::string(argv[index++]);
    };
    while(index < argc) {
      const std::string option = argv[index++];
      if(option == "--") {
        while(index < argc)
          config.runtime_arguments.emplace_back(argv[index++]);
        break;
      }
      if(option == "-h" || option == "--help") {
        print_partition_help(std::cout);
        std::exit(0);
      } else if(option == "--load") {
        config.load = argument(option);
      } else if(option == "--parts") {
        config.parts = positive_size(option, argument(option));
      } else if(option == "--slice") {
        const auto value = argument(option);
        if(value == "auto")
          config.slice.reset();
        else
          config.slice = positive_size(option, value) - 1U;
      } else if(option == "--search-name") {
        config.search_name = argument(option);
        if(!valid_search_name(*config.search_name))
          throw std::runtime_error("--search-name must be a nonempty filename component");
      } else if(option == "--output") {
        config.output = argument(option);
        if(config.output->empty())
          throw std::runtime_error("--output must not be empty");
      } else if(option == "--boundary-slack") {
        config.slack_percent = boundary_slack_percent(argument(option));
      } else if(option == "--materialize") {
        config.materialize = true;
      } else if(option == "--dry-run") {
        config.dry_run = true;
      } else if(option == "--force") {
        config.force = true;
      } else {
        throw std::runtime_error("unknown partition option: " + option);
      }
    }
    if(config.load.empty())
      throw std::runtime_error("partition requires --load CHECKPOINT");
    if(config.parts < 2U)
      throw std::runtime_error("partition requires --parts N with N >= 2");
    if(!config.materialize && !config.runtime_arguments.empty())
      throw std::runtime_error("llsss runtime options after -- require --materialize");
    return config;
  }

  static std::filesystem::path default_output_directory(const std::filesystem::path& checkpoint) {
    auto result = std::filesystem::absolute(checkpoint).lexically_normal();
    result += ".parts";
    return result;
  }

  static std::size_t choose_slice(const Solver& source, const Config& config) {
    if(config.slice.has_value()) {
      if(*config.slice >= source.slices_.size())
        throw std::runtime_error("--slice is outside the checkpoint's slice range");
      return *config.slice;
    }
    auto preferred = source.slices_.size() / 2U;
    const bool left_symmetric = source.options_.left_edge != EdgeMode::Background;
    const bool right_symmetric = source.options_.right_edge != EdgeMode::Background;
    if(left_symmetric && !right_symmetric && preferred > 0)
      --preferred;
    else if(right_symmetric && !left_symmetric && preferred + 1U < source.slices_.size())
      ++preferred;
    if(source.slices_[preferred].leaf_count() >= config.parts)
      return preferred;

    std::optional<std::size_t> best;
    for(std::size_t slice = 0; slice < source.slices_.size(); ++slice) {
      if(source.slices_[slice].leaf_count() < config.parts)
        continue;
      if(!best.has_value() || std::max(slice, preferred) - std::min(slice, preferred) < std::max(*best, preferred) - std::min(*best, preferred) ||
         (std::max(slice, preferred) - std::min(slice, preferred) == std::max(*best, preferred) - std::min(*best, preferred) &&
          source.slices_[slice].node_count() > source.slices_[*best].node_count())) {
        best = slice;
      }
    }
    if(!best.has_value())
      throw std::runtime_error("no slice has enough leaves for the requested partition count");
    return *best;
  }

  static Node balanced_boundary(Node leaves, std::size_t part, std::size_t count) {
    const auto quotient = leaves / count;
    const auto remainder = leaves % count;
    return quotient * part + std::min<Node>(remainder, part);
  }

  static Node extreme_descendant_leaf(const SuccinctSliceTree& tree, Node node, std::size_t depth, bool last) {
    while(depth < tree.depth()) {
      const auto children = tree.child_block(node);
      const auto count = static_cast<Node>(std::popcount(static_cast<unsigned>(children.mask)));
      if(count == 0)
        throw std::logic_error("partition planner found an empty internal subtree");
      node = last ? children.first + count - 1U : children.first;
      ++depth;
    }
    return node - tree.leaf_begin();
  }

  static std::pair<Node, std::size_t> shallowest_boundary(const SuccinctSliceTree& tree, Node target, Node low, Node high) {
    Node node = 0;
    std::size_t depth = 0;
    while(depth < tree.depth()) {
      const auto children = tree.child_block(node);
      const auto count = static_cast<Node>(std::popcount(static_cast<unsigned>(children.mask)));
      std::optional<Node> best_boundary;
      struct Interval {
        Node node;
        Node first;
        Node past;
      };
      std::vector<Interval> intervals;
      intervals.reserve(static_cast<std::size_t>(count));
      for(Node offset = 0; offset < count; ++offset) {
        const auto child = children.first + offset;
        const auto first = extreme_descendant_leaf(tree, child, depth + 1U, false);
        const auto past = extreme_descendant_leaf(tree, child, depth + 1U, true) + 1U;
        intervals.push_back(Interval{child, first, past});
        if(offset == 0 || first < low || first > high)
          continue;
        if(!best_boundary.has_value() || (first > target ? first - target : target - first) <
                                           (*best_boundary > target ? *best_boundary - target : target - *best_boundary) ||
           ((first > target ? first - target : target - first) == (*best_boundary > target ? *best_boundary - target : target - *best_boundary) &&
            first < *best_boundary)) {
          best_boundary = first;
        }
      }
      if(best_boundary.has_value())
        return {*best_boundary, depth};

      bool descended = false;
      for(const auto& interval : intervals) {
        const auto nested_low = std::max<Node>(low, interval.first + 1U);
        const auto nested_high = interval.past == 0 ? Node{0} : std::min<Node>(high, interval.past - 1U);
        if(nested_low > nested_high)
          continue;
        node = interval.node;
        low = nested_low;
        high = nested_high;
        ++depth;
        descended = true;
        break;
      }
      if(!descended)
        break;
    }
    throw std::logic_error("partition planner could not locate a boundary in its search interval");
  }

  static std::vector<Node> ancestor_path(const SuccinctSliceTree& tree, Node leaf_offset) {
    return tree.ancestry(tree.leaf_begin() + leaf_offset);
  }

  static std::size_t lca_depth(const SuccinctSliceTree& tree, Node boundary) {
    const auto left = tree.lineage(tree.leaf_begin() + boundary - 1U);
    const auto right = tree.lineage(tree.leaf_begin() + boundary);
    std::size_t depth = 0;
    while(depth < left.size() && left[depth] == right[depth])
      ++depth;
    return depth;
  }

  static Node spanning_nodes(const SuccinctSliceTree& tree, Node first, Node past) {
    const auto left = ancestor_path(tree, first);
    const auto right = ancestor_path(tree, past - 1U);
    Node total = 0;
    for(std::size_t depth = 0; depth < left.size(); ++depth) {
      if(left[depth] > right[depth] || total > std::numeric_limits<Node>::max() - (right[depth] - left[depth] + 1U))
        throw std::overflow_error("partition spanning-tree size overflow");
      total += right[depth] - left[depth] + 1U;
    }
    return total;
  }

  static Plan make_plan(const SuccinctSliceTree& tree, std::size_t parts, double slack_percent, std::string_view parent_name) {
    Plan plan;
    std::vector<Node> boundaries(parts + 1U, 0);
    boundaries.back() = tree.leaf_count();
    const auto typical_size = static_cast<long double>(tree.leaf_count()) / static_cast<long double>(parts);
    const auto slack = static_cast<Node>(std::floor(typical_size * static_cast<long double>(slack_percent) / 100.0L));
    for(std::size_t part = 1; part < parts; ++part) {
      const auto ideal = balanced_boundary(tree.leaf_count(), part, parts);
      const auto lower_by_slack = ideal > slack ? ideal - slack : Node{1};
      const auto upper_by_slack = ideal <= tree.leaf_count() - 1U - slack ? ideal + slack : tree.leaf_count() - 1U;
      const auto low = std::max<Node>({Node{1}, lower_by_slack, boundaries[part - 1U] + 1U});
      const auto remaining_parts = static_cast<Node>(parts - part);
      const auto high = std::min<Node>(upper_by_slack, tree.leaf_count() - remaining_parts);
      if(low > high)
        throw std::logic_error("partition boundary slack produced an empty interval");
      const auto [selected, selected_lca] = shallowest_boundary(tree, ideal, low, high);
      boundaries[part] = selected;
      plan.boundaries.push_back(PlannedBoundary{ideal, selected, lca_depth(tree, ideal), selected_lca});
    }

    plan.parts.reserve(parts);
    for(std::size_t part = 0; part < parts; ++part) {
      PlannedPart planned;
      planned.first = boundaries[part];
      planned.past = boundaries[part + 1U];
      planned.spanning_nodes = spanning_nodes(tree, planned.first, planned.past);
      planned.name = std::string(parent_name) + '-' + std::to_string(part + 1U);
      if(!valid_search_name(planned.name))
        throw std::runtime_error("derived partition search name is not a valid filename component");
      if(plan.aggregate_spanning_nodes > std::numeric_limits<Node>::max() - planned.spanning_nodes)
        throw std::overflow_error("aggregate partition spanning-tree size overflow");
      plan.aggregate_spanning_nodes += planned.spanning_nodes;
      plan.parts.push_back(std::move(planned));
    }
    return plan;
  }

  static void print_plan(const Solver& source, std::size_t slice, const Plan& plan, double slack_percent) {
    const auto& tree = source.slices_[slice];
    std::cout << "partition plan: checkpoint=" << source.options_.loadfile << " height=" << source.height_ << " slice=" << (slice + 1U) << '/'
              << source.slices_.size() << " leaves=" << tree.leaf_count() << " parts=" << plan.parts.size() << " boundary_slack=" << slack_percent << "%\n";
    for(std::size_t index = 0; index < plan.boundaries.size(); ++index) {
      const auto& boundary = plan.boundaries[index];
      const auto shift = boundary.selected >= boundary.ideal ? static_cast<std::int64_t>(boundary.selected - boundary.ideal)
                                                               : -static_cast<std::int64_t>(boundary.ideal - boundary.selected);
      std::cout << "  boundary " << (index + 1U) << ": ideal=" << boundary.ideal << " selected=" << boundary.selected << " shift=" << shift
                << " lca_depth=" << boundary.ideal_lca_depth << "->" << boundary.selected_lca_depth << '\n';
    }
    for(std::size_t index = 0; index < plan.parts.size(); ++index) {
      const auto& part = plan.parts[index];
      std::cout << "  part " << (index + 1U) << ": name=" << part.name << " leaves=[" << part.first << ',' << part.past << ") count="
                << (part.past - part.first) << " spanning_nodes=" << part.spanning_nodes << '\n';
    }
    std::cout << "  aggregate_spanning_nodes=" << plan.aggregate_spanning_nodes << " duplicate_nodes="
              << (plan.aggregate_spanning_nodes - tree.node_count()) << '\n';
  }

  static bool aliases_source(const std::filesystem::path& candidate, const std::filesystem::path& source) {
    const auto absolute_candidate = std::filesystem::absolute(candidate).lexically_normal();
    const auto absolute_source = std::filesystem::absolute(source).lexically_normal();
    if(absolute_candidate == absolute_source)
      return true;
    std::error_code error;
    return std::filesystem::equivalent(absolute_candidate, absolute_source, error) && !error;
  }

  static void ensure_not_source_output(const std::string& candidate, std::string_view description, const std::filesystem::path& source) {
    if(!candidate.empty() && aliases_source(candidate, source))
      throw std::runtime_error("partition " + std::string(description) + " would overwrite its source checkpoint: " + candidate);
  }

  static void preflight_outputs(const std::filesystem::path& source, const std::filesystem::path& directory, const std::vector<PartitionSpec>& specs,
    std::size_t height, bool materialize, bool force) {
    if(materialize && height == std::numeric_limits<std::size_t>::max())
      throw std::overflow_error("partition materialization row overflow");
    for(const auto& spec : specs) {
      const auto filename = materialize ? spec.constraint.search_name + '_' + std::to_string(height + 1U) : spec.constraint.search_name + ".rlp";
      const auto path = directory / filename;
      auto temporary = path;
      temporary += ".tmp";
      if(aliases_source(path, source) || aliases_source(temporary, source))
        throw std::runtime_error("partition output would overwrite its source checkpoint: " + path.string());
      std::error_code error;
      const bool output_exists = std::filesystem::exists(path, error);
      error.clear();
      const bool temporary_exists = std::filesystem::exists(temporary, error);
      if(!force && (output_exists || temporary_exists))
        throw std::runtime_error("partition output already exists: " + path.string());
    }
  }

  static Options parse_materialize_options(const std::filesystem::path& checkpoint, const std::vector<std::string>& runtime_arguments) {
    for(const auto& argument : runtime_arguments) {
      if(argument == "--load" || argument == "--save" || argument == "--savedir" || argument == "--search-name" || argument == "--halts" ||
         argument == "-h" || argument == "--help") {
        throw std::runtime_error("partition --materialize controls runtime option " + argument);
      }
    }
    std::vector<std::string> arguments{"rlife", "llsss", "--load", checkpoint.string()};
    arguments.insert(arguments.end(), runtime_arguments.begin(), runtime_arguments.end());
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for(auto& argument : arguments)
      pointers.push_back(argument.data());
    return parse_cli(static_cast<int>(pointers.size()), pointers.data());
  }
};

int run_partition_command(int argc, char** argv) { return PartitionCommand::run(argc, argv); }

} // namespace rlife::llsss
