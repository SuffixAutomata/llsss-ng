#pragma once

#include "rule.hpp"
#include "succinct_slice_tree.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace rlife::llsss {

enum class EdgeMode { Background, Odd, Even };

std::uint64_t getMaxRSS() {
#ifdef __linux__
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        return usage.ru_maxrss * 1024ll;
#endif
    return 0;
}

inline std::string integer_format(std::uint64_t n) {
    if (n < 10*1024) return std::to_string(n);
    if (n < 10*1024*1024) return std::to_string(n>>10) + "K";
    if (n < (10ll<<30)) return std::to_string(n>>20) + "M";
    return std::to_string(n>>30) + "G";
}

inline std::string_view edge_name(EdgeMode mode) {
    switch (mode) {
        case EdgeMode::Background: return "bg";
        case EdgeMode::Odd: return "odd";
        case EdgeMode::Even: return "even";
    }
    return "?";
}

inline EdgeMode parse_edge(const std::string& text) {
    if (text == "bg" || text == "background" || text == "asymmetric") {
        return EdgeMode::Background;
    }
    if (text == "odd") {
        return EdgeMode::Odd;
    }
    if (text == "even") {
        return EdgeMode::Even;
    }
    throw std::runtime_error("unsupported edge: " + text + " (use bg, odd, or even)");
}

struct Geometry {
    int displacement = 1;
    int period = 0;
    std::string source;

    static Geometry parse(const std::string& text) {
        static const std::regex pattern(R"(^([0-9]*)c([0-9]+)-f2b$)");
        std::smatch match;
        if (!std::regex_match(text, match, pattern)) {
            throw std::runtime_error(
                "orthogonal geometry must look like c4-f2b or 2c5-f2b");
        }
        Geometry result;
        result.source = text;
        result.displacement = match[1].str().empty() ? 1 : std::stoi(match[1].str());
        result.period = std::stoi(match[2].str());
        if (result.period <= 0 || result.displacement < 0
            || result.displacement > result.period) {
            throw std::runtime_error(
                "geometry requires 0 <= displacement <= period and period > 0");
        }
        return result;
    }
};

enum class PartialMode { None, Final, Every };

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
    int reserved_threads = 1;
    int halt_height = -1;
    PartialMode partial_mode = PartialMode::Final;
    int partial_every = 1;
    std::string partial_output;
    std::string stats_output;
    bool verbose = false;
};

// One bit for each CA-compatible neighboring leaf pair in deterministic
// synchronized-DFS order when a filter actually rejects a pair.  An all-one
// gate is implicit and costs no payload.  This retains correlation that a
// unary slice projection cannot express, without storing either endpoint.
struct PairGate {
    PackedTags bits;
    std::uint64_t bit_count = 0;

    [[nodiscard]] std::uint64_t size() const noexcept { return bit_count; }
    [[nodiscard]] bool get(std::uint64_t index) const noexcept {
        return bits.size() == 0 ? true : bits.get(index);
    }
    void push_back(bool value) {
        if (bits.size() != 0) {
            bits.push_back(value);
        } else if (!value) {
            bits.reset_size(bit_count + 1U);
            bits.set_all();
            bits.set(bit_count, false);
        }
        ++bit_count;
    }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept {
        return bits.allocated_bytes();
    }
    [[nodiscard]] std::uint64_t count() const noexcept {
        return bits.size() == 0 ? bit_count : bits.count(0, bits.size());
    }
};

inline std::vector<std::string> split_words(std::string text) {
    for (char& ch : text) {
        if (ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',') {
            ch = ' ';
        }
    }
    std::istringstream input(text);
    std::vector<std::string> result;
    for (std::string word; input >> word;) {
        result.push_back(std::move(word));
    }
    return result;
}

inline void print_help(std::ostream& out) {
    out <<
R"(rlife_llsss llsss [options] <geometry> <start>

Orthogonal fixed-width LLSSS using succinct two-column slice trees.

  <geometry>                 c4-f2b, 2c5-f2b, ...
  <start>                    @bg(W), @bg:W, an RLE file, or an ASCII grid

  --rule RULE                isotropic B/S or Hensel rule (default S23/B3)
  --symmetry MODE            asymmetric, odd, or even; symmetry applies at left only
  --left-edge MODE           bg, odd, or even
  --right-edge MODE          bg, odd, or even
  --bg-agar zero             zero background is the supported agar
  --filters bcaf             BCAF zero-background witness filter
  --ends default|none        zero-background completion detection (default)
  --[no-]halt-on-ends        halt after the first completion (default: halt)
  --halts w_pos:N            stop at row-sequence height N
  --partials MODE            none, final, default, or every:N (default: final)
  --partial-output FILE      write RLE partials/completions to FILE
  --dump-slice-stats FILE    append per-height succinct-slice statistics
  --phase-progress           print individual sweep phases
  --verbose                  verbose information at every row
  --threads N                reserved for future parallel DFS (currently serial)
  -h, --help                 show this help

The implementation has no autochoke and stores no join endpoints or join DAG.
)";
}

inline Options parse_cli(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error("usage: rlife_llsss llsss [options] <geometry> <start>");
    }
    int index = 1;
    if (std::string(argv[index++]) != "llsss") {
        throw std::runtime_error("the only subcommand is llsss");
    }

    Options options;
    std::optional<EdgeMode> symmetry_edge;
    std::optional<EdgeMode> explicit_left_edge;
    std::optional<EdgeMode> explicit_right_edge;
    std::vector<std::string> positional;
    auto argument = [&](const std::string& option) {
        if (index >= argc) {
            throw std::runtime_error("missing argument for " + option);
        }
        return std::string(argv[index++]);
    };

    while (index < argc) {
        const std::string current = argv[index++];
        if (current == "--") {
            while (index < argc) {
                positional.emplace_back(argv[index++]);
            }
            break;
        }
        if (current == "-h" || current == "--help") {
            print_help(std::cout);
            std::exit(0);
        }
        if (!current.starts_with("--")) {
            positional.push_back(current);
            continue;
        }
        if (current == "--rule") {
            options.rule = argument(current);
        } else if (current == "--left-edge") {
            explicit_left_edge = parse_edge(argument(current));
        } else if (current == "--right-edge") {
            explicit_right_edge = parse_edge(argument(current));
        } else if (current == "--symmetry") {
            const auto value = argument(current);
            if (value == "asymmetric" || value == "asym") {
                symmetry_edge = EdgeMode::Background;
            } else if (value == "odd" || value == "even") {
                symmetry_edge = parse_edge(value);
            } else {
                throw std::runtime_error("--symmetry must be asymmetric, odd, or even");
            }
        } else if (current == "--bg-agar") {
            if (argument(current) != "zero") {
                throw std::runtime_error("this rendition currently supports --bg-agar zero only");
            }
        } else if (current == "--filters") {
            for (const auto& filter : split_words(argument(current))) {
                if (filter == "bcaf") {
                    options.bcaf = true;
                } else if (filter != "none") {
                    throw std::runtime_error("unsupported filter: " + filter);
                }
            }
        } else if (current == "--ends") {
            const auto value = argument(current);
            if (value == "default" || value == "bg") {
                options.detect_ends = true;
            } else if (value == "none") {
                options.detect_ends = false;
            } else {
                throw std::runtime_error("--ends supports default or none");
            }
        } else if (current == "--halt-on-ends") {
            options.halt_on_ends = true;
        } else if (current == "--no-halt-on-ends") {
            options.halt_on_ends = false;
        } else if (current == "--halts") {
            const auto value = argument(current);
            std::smatch match;
            if (!std::regex_match(value, match, std::regex(R"(^w_pos:([0-9]+)$)"))) {
                throw std::runtime_error("--halts supports w_pos:N");
            }
            options.halt_height = std::stoi(match[1].str());
        } else if (current == "--partials" || current == "--pre-partials") {
            const auto value = argument(current);
            if (value == "none" || value == "[]") {
                options.partial_mode = PartialMode::None;
            } else if (value == "final") {
                options.partial_mode = PartialMode::Final;
            } else if (value == "default" || value == "unique" || value == "every") {
                options.partial_mode = PartialMode::Every;
                options.partial_every = 1;
            } else if (value.starts_with("every:")) {
                options.partial_mode = PartialMode::Every;
                options.partial_every = std::stoi(value.substr(6));
                if (options.partial_every <= 0) {
                    throw std::runtime_error("partial interval must be positive");
                }
            } else {
                throw std::runtime_error("--partials supports none, final, default, or every:N");
            }
        } else if (current == "--partial-output") {
            options.partial_output = argument(current);
        } else if (current == "--dump-slice-stats") {
            options.stats_output = argument(current);
        } else if (current == "--phase-progress") {
            options.phase_progress = true;
        } else if (current == "--verbose") {
            options.verbose = true;
        } else if (current == "--threads") {
            options.reserved_threads = std::stoi(argument(current));
            if (options.reserved_threads <= 0) {
                throw std::runtime_error("--threads must be positive");
            }
        } else if (current == "--pre-reify-autochoke"
                   || current == "--pre-reify-autochoke-type") {
            throw std::runtime_error("autochoke is deliberately not implemented");
        } else {
            throw std::runtime_error("unknown option: " + current);
        }
    }

    if (positional.size() != 2) {
        throw std::runtime_error("expected exactly <geometry> <start>");
    }
    // The convenience option describes at most one symmetry boundary.  Direct
    // edge options are independent and override it regardless of CLI order.
    if (symmetry_edge.has_value()) {
        options.left_edge = *symmetry_edge;
        options.right_edge = EdgeMode::Background;
    }
    if (explicit_left_edge.has_value()) options.left_edge = *explicit_left_edge;
    if (explicit_right_edge.has_value()) options.right_edge = *explicit_right_edge;
    options.geometry = positional[0];
    options.start = positional[1];
    return options;
}

struct StartGrid {
    std::size_t width = 0;
    std::size_t height = 0;
    // -1 is an independent wildcard, 0 dead, 1 live.
    std::vector<std::int8_t> cells;

    [[nodiscard]] std::int8_t at(std::size_t x, std::size_t y) const {
        return cells[y * width + x];
    }
};

class Solver {
public:
    explicit Solver(Options options)
        : options_(std::move(options)), geometry_(Geometry::parse(options_.geometry)),
          rule_(RuleTable::parse(options_.rule)) {
        configure_pair_history();
        build_pair_transition_table();
        build_row_acceptance_table();
        build_edge_acceptance_tables();
        rule_.release_partial_lookup();
        if (!options_.partial_output.empty()) {
            partial_file_.open(options_.partial_output, std::ios::out | std::ios::trunc);
            if (!partial_file_) {
                throw std::runtime_error("cannot open partial output: " + options_.partial_output);
            }
        }
        if (!options_.stats_output.empty()) {
            stats_file_.open(options_.stats_output, std::ios::out | std::ios::trunc);
            if (!stats_file_) {
                throw std::runtime_error("cannot open slice stats: " + options_.stats_output);
            }
        }
        initialize();
    }

    int run() {
        std::cout << "rlife_llsss: geom=" << geometry_.source
                  << " p=" << geometry_.period
                  << " k=" << geometry_.displacement
                  << " rule=" << options_.rule
                  << " width=" << width_
                  << " left_edge=" << edge_name(options_.left_edge)
                  << " right_edge=" << edge_name(options_.right_edge)
                  << " bcaf=" << (options_.bcaf ? "yes" : "no")
                  << " halt_on_ends=" << (options_.halt_on_ends ? "yes" : "no")
                  << '\n';
        if (options_.reserved_threads != 1) {
            std::cout << "note: --threads is reserved; this build runs synchronized DFS serially\n";
        }
        running_ = true;
        print_stats("init", 0.0);

        if (slices_.empty()) {
            std::cout << "search space is empty after initialization\n";
            return 0;
        }
        if (options_.halt_height >= 0
            && height_ >= static_cast<std::size_t>(options_.halt_height)) {
            emit_final_partial("halt");
            return 0;
        }

        for (;;) {
            const auto started = Clock::now();
            pair_states_ = 0;
            pair_leaves_ = 0;
            boundary_states_ = 0;
            peak_tag_bytes_ = 0;

            phase("extend slice-tree tails");
            for (auto& slice : slices_) {
                slice.expand_leaves();
            }
            ++height_;

            phase("support and filter sweeps");
            if (!prune_supported()) {
                std::cout << "search exhausted at height " << height_ << '\n';
                slices_.clear();
                return 0;
            }

            const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
            print_stats("step", seconds);

            if (options_.partial_mode == PartialMode::Every
                && height_ % static_cast<std::size_t>(options_.partial_every) == 0) {
                emit_board(reconstruct_partial(), "partial");
            }

            if (options_.detect_ends) {
                phase("end-tag propagation");
                if (auto completion = find_completion()) {
                    std::cout << "completion at height " << height_ << '\n';
                    emit_board(*completion, "completion");
                    if (options_.halt_on_ends) {
                        return 0;
                    }
                }
            }

            if (options_.halt_height >= 0
                && height_ >= static_cast<std::size_t>(options_.halt_height)) {
                std::cout << "height halt at " << height_ << '\n';
                emit_final_partial("halt");
                return 0;
            }
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    using Node = SuccinctSliceTree::Node;
    using Board = std::vector<std::vector<std::uint8_t>>;

    static StartGrid magic_background(const std::string& source, std::size_t height) {
        std::smatch match;
        static const std::regex current(R"(^@?bg\(([0-9]+)\)$)");
        static const std::regex legacy(R"(^@?bg:([0-9]+)$)");
        if (!std::regex_match(source, match, current)
            && !std::regex_match(source, match, legacy)) {
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
        if (!input) {
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
        while (std::getline(lines, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (header.empty()) {
                header = line;
            } else {
                body += line;
            }
        }
        std::smatch match;
        static const std::regex header_pattern(
            R"(^\s*x\s*=\s*([0-9]+)\s*,\s*y\s*=\s*([0-9]+).*$)",
            std::regex::icase);
        if (!std::regex_match(header, match, header_pattern)) {
            throw std::runtime_error("invalid RLE header");
        }
        StartGrid grid;
        grid.width = static_cast<std::size_t>(std::stoull(match[1].str()));
        grid.height = static_cast<std::size_t>(std::stoull(match[2].str()));
        grid.cells.assign(grid.width * grid.height, 0);
        std::size_t x = 0;
        std::size_t y = 0;
        std::size_t run = 0;
        auto count = [&] { const auto value = run == 0 ? 1U : run; run = 0; return value; };
        for (const char raw : body) {
            const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                run = run * 10U + static_cast<unsigned>(ch - '0');
            } else if (ch == 'b' || ch == 'o') {
                const auto amount = count();
                for (std::size_t i = 0; i < amount; ++i) {
                    if (x >= grid.width || y >= grid.height) {
                        throw std::runtime_error("RLE body exceeds declared dimensions");
                    }
                    grid.cells[y * grid.width + x++] = ch == 'o' ? 1 : 0;
                }
            } else if (ch == '$') {
                y += count();
                x = 0;
            } else if (ch == '!') {
                break;
            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                throw std::runtime_error("invalid character in RLE body");
            }
        }
        return grid;
    }

    static StartGrid parse_ascii(const std::string& text) {
        std::istringstream input(text);
        std::vector<std::vector<std::int8_t>> rows;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line[0] == '#') {
                continue;
            }
            std::vector<std::int8_t> row;
            for (const char raw : line) {
                const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
                if (ch == '.' || ch == 'b' || ch == '0') row.push_back(0);
                else if (ch == '*' || ch == 'o' || ch == '1') row.push_back(1);
                else if (ch == '?') row.push_back(-1);
                else if (!std::isspace(static_cast<unsigned char>(ch))) {
                    throw std::runtime_error("invalid character in ASCII start grid");
                }
            }
            if (!row.empty()) rows.push_back(std::move(row));
        }
        if (rows.empty()) {
            throw std::runtime_error("empty ASCII start grid");
        }
        StartGrid grid;
        grid.height = rows.size();
        for (const auto& row : rows) grid.width = std::max(grid.width, row.size());
        grid.cells.assign(grid.width * grid.height, 0);
        for (std::size_t y = 0; y < rows.size(); ++y) {
            std::copy(rows[y].begin(), rows[y].end(), grid.cells.begin() + y * grid.width);
        }
        return grid;
    }

    StartGrid load_start() const {
        if (auto grid = magic_background(options_.start,
                                         2U * static_cast<std::size_t>(geometry_.period));
            grid.width != 0) {
            return grid;
        }
        const auto text = read_file(options_.start);
        if (std::regex_search(text, std::regex(R"((^|\n)\s*x\s*=)", std::regex::icase))) {
            return parse_rle(text);
        }
        return parse_ascii(text);
    }

    static std::uint8_t allowed_pair_labels(std::int8_t left, std::int8_t right) {
        std::uint8_t mask = 0;
        for (std::uint8_t a = 0; a < 2; ++a) {
            if (left >= 0 && a != static_cast<std::uint8_t>(left)) continue;
            for (std::uint8_t b = 0; b < 2; ++b) {
                if (right >= 0 && b != static_cast<std::uint8_t>(right)) continue;
                mask |= static_cast<std::uint8_t>(1U << ((a << 1U) | b));
            }
        }
        return mask;
    }

    void initialize() {
        const auto start = load_start();
        if (start.width < 3) {
            throw std::runtime_error("start width must be at least three physical columns");
        }
        const auto minimum_height = 2U * static_cast<std::size_t>(geometry_.period);
        if (start.height < minimum_height) {
            throw std::runtime_error("start height must be at least 2*p = "
                                     + std::to_string(minimum_height));
        }
        width_ = start.width;
        height_ = start.height;
        slices_.resize(width_ - 1U);
        for (std::size_t x = 0; x + 1 < width_; ++x) {
            for (std::size_t y = 0; y < height_; ++y) {
                slices_[x].append_uniform(allowed_pair_labels(start.at(x, y), start.at(x + 1, y)));
            }
        }
        if (!prune_supported()) {
            slices_.clear();
        }
    }

    [[nodiscard]] bool row_candidate_accepts(
        const std::array<std::uint8_t, 5>& history,
        std::uint8_t candidate) const {
        const auto p = static_cast<std::int64_t>(geometry_.period);
        const auto k = static_cast<std::int64_t>(geometry_.displacement);
        const std::array<std::int64_t, 5> history_rows = {
            -2 * p, -p - k, -p, -k, -p + k,
        };

        // Appending three adjacent cells touches 18 local CA equations.  Most
        // are only partly known at this point.  Reject the candidate if any
        // such equation has no completion in the same exact 10-bit rule
        // table.  For an orthogonal k*c/p geometry, search-row coordinate is
        // w = k*t + p*y.
        auto equation_accepts = [&](std::int64_t center_x,
                                    std::int64_t center_w) {
            std::uint16_t known = 0;
            std::uint16_t value = 0;
            auto read = [&](unsigned bit_index, std::int64_t x,
                            std::int64_t w) {
                if (x < 0 || x >= 3 || w > 0) return;
                std::uint8_t triple = candidate;
                bool available = w == 0;
                for (std::size_t i = 0; !available && i < history.size(); ++i) {
                    if (w == history_rows[i]) {
                        triple = history[i];
                        available = true;
                    }
                }
                if (!available) return;
                const auto bit = static_cast<std::uint16_t>(1U << bit_index);
                known = static_cast<std::uint16_t>(known | bit);
                if (((triple >> static_cast<unsigned>(x)) & 1U) != 0) {
                    value = static_cast<std::uint16_t>(value | bit);
                }
            };

            unsigned bit_index = 0;
            for (const auto dw : {-p, std::int64_t{0}, p}) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    read(bit_index++, center_x + dx, center_w + dw);
                }
            }
            read(9, center_x, center_w + k);
            return rule_.accepts_partial(known, value);
        };

        for (const auto candidate_dw : {-p, std::int64_t{0}, p}) {
            const auto center_w = -candidate_dw;
            for (std::int64_t center_x = -1; center_x <= 3; ++center_x) {
                if (!equation_accepts(center_x, center_w)) return false;
            }
        }
        const auto future_center_w = -k;
        for (std::int64_t center_x = 0; center_x <= 2; ++center_x) {
            if (!equation_accepts(center_x, future_center_w)) return false;
        }
        return true;
    }

    void build_row_acceptance_table() {
        for (std::size_t key = 0; key < row_acceptance_.size(); ++key) {
            auto remaining = key;
            std::array<std::uint8_t, 5> history{};
            for (auto& triple : history) {
                triple = static_cast<std::uint8_t>(remaining & 0b111U);
                remaining >>= 3U;
            }
            std::uint8_t mask = 0;
            for (std::uint8_t position = 0; position < 8; ++position) {
                const auto candidate = pair_triple_order_[position];
                if (row_candidate_accepts(history, candidate)) {
                    mask = static_cast<std::uint8_t>(mask | (1U << position));
                }
            }
            row_acceptance_[key] = mask;
        }
    }

    [[nodiscard]] bool edge_candidate_accepts(
        const std::array<std::uint8_t, 5>& history,
        std::uint8_t candidate, EdgeMode mode) const {
        const auto p = static_cast<std::int64_t>(geometry_.period);
        const auto k = static_cast<std::int64_t>(geometry_.displacement);
        const std::array<std::int64_t, 5> history_rows = {
            -2 * p, -p - k, -p, -k, -p + k,
        };
        const bool even = mode == EdgeMode::Even;
        // In the two-cell edge window, odd reflection produces A B A.
        // Even reflection puts both window cells on opposite sides of the
        // half-cell axis.  The reflected reads consequently overlap: CA
        // checks see A A, and the two newly appended bits must be equal.
        // Historical equality checks are intentionally absent, just as in
        // the Rust table: only the first physical cell is read from history.
        if (even && ((candidate & 1U) != ((candidate >> 1U) & 1U))) {
            return false;
        }
        const std::int64_t min_known_x = even ? 0 : -1;
        constexpr std::int64_t max_known_x = 1;

        auto equation_accepts = [&](std::int64_t center_x,
                                    std::int64_t center_w) {
            std::uint16_t known = 0;
            std::uint16_t value = 0;
            auto read = [&](unsigned bit_index, std::int64_t x,
                            std::int64_t w) {
                if (w > 0) return;
                std::uint8_t pair = candidate;
                bool available = w == 0;
                for (std::size_t i = 0; !available && i < history.size(); ++i) {
                    if (w == history_rows[i]) {
                        pair = history[i];
                        available = true;
                    }
                }
                if (!available) return;

                bool spatially_known = false;
                bool state = false;
                if (even) {
                    if (x == 0 || x == 1) {
                        spatially_known = true;
                        state = (pair & 0b01U) != 0; // first physical cell
                    }
                } else {
                    if (x == 0) {
                        spatially_known = true;
                        state = (pair & 0b01U) != 0; // boundary cell
                    } else if (x == -1 || x == 1) {
                        spatially_known = true;
                        state = (pair & 0b10U) != 0; // inward cell
                    }
                }
                if (!spatially_known) return;
                const auto bit = static_cast<std::uint16_t>(1U << bit_index);
                known = static_cast<std::uint16_t>(known | bit);
                if (state) value = static_cast<std::uint16_t>(value | bit);
            };

            unsigned bit_index = 0;
            for (const auto dw : {-p, std::int64_t{0}, p}) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    read(bit_index++, center_x + dx, center_w + dw);
                }
            }
            read(9, center_x, center_w + k);
            return rule_.accepts_partial(known, value);
        };

        for (const auto candidate_dw : {-p, std::int64_t{0}, p}) {
            const auto center_w = -candidate_dw;
            for (auto center_x = min_known_x - 1;
                 center_x <= max_known_x + 1; ++center_x) {
                if (!equation_accepts(center_x, center_w)) return false;
            }
        }
        for (auto center_x = min_known_x; center_x <= max_known_x; ++center_x) {
            if (!equation_accepts(center_x, -k)) return false;
        }
        return true;
    }

    void build_edge_acceptance_tables() {
        auto build = [&](EdgeMode mode, auto& table) {
            for (std::size_t key = 0; key < table.size(); ++key) {
                auto remaining = key;
                std::array<std::uint8_t, 5> history{};
                for (auto& pair : history) {
                    pair = static_cast<std::uint8_t>(remaining & 0b11U);
                    remaining >>= 2U;
                }
                std::uint8_t mask = 0;
                for (std::uint8_t candidate = 0; candidate < 4; ++candidate) {
                    if (edge_candidate_accepts(history, candidate, mode)) {
                        mask = static_cast<std::uint8_t>(mask | (1U << candidate));
                    }
                }
                table[key] = mask;
            }
        };
        build(EdgeMode::Odd, odd_edge_acceptance_);
        build(EdgeMode::Even, even_edge_acceptance_);
    }

    void configure_pair_history() {
        const auto p = static_cast<std::size_t>(geometry_.period);
        const auto k = static_cast<std::size_t>(geometry_.displacement);
        pair_history_offsets_ = {2U * p, p + k, p, k, p - k};
    }

    [[nodiscard]] std::uint8_t history_all_accepts(
        const std::uint8_t* triples, std::size_t row) const {
        if (row < pair_history_offsets_[0]) return 0xffU;
        const auto key =
            static_cast<std::size_t>(triples[row - pair_history_offsets_[0]])
            | (static_cast<std::size_t>(triples[row - pair_history_offsets_[1]]) << 3U)
            | (static_cast<std::size_t>(triples[row - pair_history_offsets_[2]]) << 6U)
            | (static_cast<std::size_t>(triples[row - pair_history_offsets_[3]]) << 9U)
            | (static_cast<std::size_t>(triples[row - pair_history_offsets_[4]]) << 12U);
        // const auto candidate = triples[row] & 0b111U;
        return row_acceptance_[key];
    }

    struct PairTransitions {
        // In the original left-label/right-label DFS order, bits 0..1 are the
        // offset within the left child block and bits 2..3 the offset within
        // the right block.
        std::array<std::uint8_t, 8> child_offsets{};
        std::uint8_t present = 0;
    };

    inline static constexpr std::array<std::uint8_t, 8> pair_triple_order_ = {
        0, 4, 2, 6, 1, 5, 3, 7,
    };

    void build_pair_transition_table() {
        for (std::uint8_t left_mask = 0; left_mask < 16; ++left_mask) {
            for (std::uint8_t right_mask = 0; right_mask < 16; ++right_mask) {
                auto& transitions = pair_transitions_[
                    static_cast<std::size_t>(left_mask)
                    | (static_cast<std::size_t>(right_mask) << 4U)];
                for (std::uint8_t position = 0; position < 8; ++position) {
                    const auto triple = pair_triple_order_[position];
                    const auto left_label = static_cast<std::uint8_t>(
                        ((triple & 1U) << 1U) | ((triple >> 1U) & 1U));
                    const auto right_label = static_cast<std::uint8_t>(
                        (triple & 0b010U) | ((triple >> 2U) & 1U));
                    if ((left_mask & (1U << left_label)) == 0
                        || (right_mask & (1U << right_label)) == 0) {
                        continue;
                    }
                    const auto left_offset = static_cast<std::uint8_t>(std::popcount(
                        static_cast<unsigned>(left_mask & ((1U << left_label) - 1U))));
                    const auto right_offset = static_cast<std::uint8_t>(std::popcount(
                        static_cast<unsigned>(right_mask & ((1U << right_label) - 1U))));
                    transitions.child_offsets[position] = static_cast<std::uint8_t>(
                        left_offset | (right_offset << 2U));
                    transitions.present = static_cast<std::uint8_t>(
                        transitions.present | (1U << position));
                }
            }
        }
    }

    [[nodiscard]] static std::uint8_t canonical_edge_pair(std::uint8_t label,
                                                          bool left_side,
                                                          EdgeMode mode) {
        const auto first = static_cast<std::uint8_t>((label >> 1U) & 1U);
        const auto second = static_cast<std::uint8_t>(label & 1U);
        // The even table's overlapping reflected reads select physical cell
        // zero on both sides.  Preserve physical order so bit zero names that
        // cell.  Odd reflection is more naturally canonicalized as
        // boundary/inward, making its left and right tables identical.
        if (mode == EdgeMode::Even) {
            return static_cast<std::uint8_t>(first | (second << 1U));
        }
        const auto inward = left_side ? second : first;
        const auto boundary = left_side ? first : second;
        return static_cast<std::uint8_t>(boundary | (inward << 1U));
    }

    [[nodiscard]] std::uint8_t edge_history_all_accepts(
        const std::uint8_t* pairs, std::size_t row, EdgeMode mode) const {
        const auto p = static_cast<std::size_t>(geometry_.period);
        const auto k = static_cast<std::size_t>(geometry_.displacement);
        if (row < 2U * p) return 0xffU;
        const std::array<std::size_t, 5> rows = {
            row - 2U * p, row - p - k, row - p, row - k, row - p + k,
        };
        std::size_t key = 0;
        unsigned shift = 0;
        for (const auto history_row : rows) {
            key |= static_cast<std::size_t>(pairs[history_row] & 0b11U) << shift;
            shift += 2U;
        }
        const auto& table = mode == EdgeMode::Even
            ? even_edge_acceptance_ : odd_edge_acceptance_;
        return table[key];
    }


    bool boundary_dfs(const SuccinctSliceTree& tree, Node node, std::size_t depth,
                      EdgeMode edge, bool left_side, std::vector<std::uint8_t>& history,
                      PackedTags& tags) {
        ++boundary_states_;
        if (depth == tree.depth()) {
            tags.set(node);
            return true;
        }
        bool any = false;
        const auto children = tree.child_block(node);
        auto child = children.first;
        const auto acceptor = edge_history_all_accepts(history.data(), depth, edge);
        for (std::uint8_t label = 0; label < 4; ++label) {
            if ((children.mask & (1U << label)) == 0) continue;
            const auto next = child++;
            // A background boundary slice is supplied by the background
            // generator itself.  For zero background both cells in that
            // terminal slice therefore extend with zero.
            if (edge == EdgeMode::Background && label != 0) continue;
            if (!(acceptor & (1U << canonical_edge_pair(label, left_side, edge)))) continue;
            history[depth] = canonical_edge_pair(label, left_side, edge);
            if (boundary_dfs(tree, next, depth + 1,
                                edge, left_side, history, tags)) {
                any = true;
            }
        }
        if (any) tags.set(node);
        return any;
    }

    void mark_boundary(const SuccinctSliceTree& tree, EdgeMode edge, bool left_side,
                       PackedTags& tags) {
        tags.clear();
        std::vector<std::uint8_t> history(tree.depth());
        boundary_dfs(tree, 0, 0, edge, left_side, history, tags);
    }

    struct PairPathSummary {
        std::size_t first_left_nonzero = std::numeric_limits<std::size_t>::max();
        std::size_t last_left_nonzero = std::numeric_limits<std::size_t>::max();
    };

    enum class PairGateLocation : std::uint8_t {
        None,
        ParentOfLeaf,
        Leaf,
    };

    template<bool CountStats, bool VisitRejected, bool TrackSummary,
             PairGateLocation GateLocation,
             class LeafCallback>
    void candidate_pair_dfs(const SuccinctSliceTree& left,
                            const SuccinctSliceTree& right,
                            Node left_node, Node right_node, std::size_t depth,
                            std::vector<std::uint8_t>& history,
                            const PairGate* parent_gate,
                            std::uint64_t& parent_gate_cursor,
                            bool ancestry_allowed,
                            PairPathSummary summary,
                            LeafCallback& leaf_callback) {
        if constexpr (CountStats) ++pair_states_;
        const auto tree_depth = left.depth();
        if constexpr (GateLocation == PairGateLocation::ParentOfLeaf) {
            if (depth + 1U == tree_depth) {
                if (parent_gate_cursor >= parent_gate->size()) {
                    throw std::logic_error("pair gate ended before its DFS enumeration");
                }
                ancestry_allowed = parent_gate->get(parent_gate_cursor++);
                if constexpr (!VisitRejected) {
                    if (!ancestry_allowed) return;
                }
            }
        }
        if (depth == tree_depth) {
            if constexpr (CountStats) ++pair_leaves_;
            if constexpr (TrackSummary) {
                leaf_callback(left_node, right_node, history, ancestry_allowed, summary);
            } else {
                leaf_callback(left_node, right_node, history, ancestry_allowed);
            }
            return;
        }

        const auto left_children = left.child_block(left_node);
        const auto right_children = right.child_block(right_node);
        const auto acceptor = history_all_accepts(history.data(), depth);
        const auto& transitions = pair_transitions_[
            static_cast<std::size_t>(left_children.mask)
            | (static_cast<std::size_t>(right_children.mask) << 4U)];
        auto active = static_cast<std::uint8_t>(acceptor & transitions.present);
        const bool children_are_leaves = depth + 1U == tree_depth;
        while (active != 0) {
            const auto position = static_cast<std::uint8_t>(std::countr_zero(
                static_cast<unsigned>(active)));
            const auto triple = pair_triple_order_[position];
            const auto offsets = transitions.child_offsets[position];
            history[depth] = triple;
            auto next_summary = summary;
            if constexpr (TrackSummary) {
                if ((triple & 0b011U) != 0) {
                    if (next_summary.first_left_nonzero
                        == std::numeric_limits<std::size_t>::max()) {
                        next_summary.first_left_nonzero = depth;
                    }
                    next_summary.last_left_nonzero = depth;
                }
            }
            const auto next_left = left_children.first + (offsets & 0b11U);
            const auto next_right = right_children.first + ((offsets >> 2U) & 0b11U);
            if (children_are_leaves) {
                bool leaf_allowed = ancestry_allowed;
                if constexpr (GateLocation == PairGateLocation::Leaf) {
                    if (parent_gate_cursor >= parent_gate->size()) {
                        throw std::logic_error(
                            "pair gate ended before its DFS enumeration");
                    }
                    leaf_allowed = parent_gate->get(parent_gate_cursor++);
                }
                if constexpr (CountStats) ++pair_states_;
                if constexpr (VisitRejected) {
                    if constexpr (CountStats) ++pair_leaves_;
                    if constexpr (TrackSummary) {
                        leaf_callback(next_left, next_right, history, leaf_allowed,
                                      next_summary);
                    } else {
                        leaf_callback(next_left, next_right, history, leaf_allowed);
                    }
                } else if (leaf_allowed) {
                    if constexpr (CountStats) ++pair_leaves_;
                    if constexpr (TrackSummary) {
                        leaf_callback(next_left, next_right, history, true, next_summary);
                    } else {
                        leaf_callback(next_left, next_right, history, true);
                    }
                }
            } else {
                candidate_pair_dfs<CountStats, VisitRejected, TrackSummary,
                                   GateLocation>(
                    left, right, next_left, next_right,
                    depth + 1, history, parent_gate,
                    parent_gate_cursor, ancestry_allowed, next_summary, leaf_callback);
            }
            active = static_cast<std::uint8_t>(active & (active - 1U));
        }
    }

    template<bool VisitRejected = true, bool TrackSummary = false,
             class LeafCallback>
    void walk_candidate_pairs(std::size_t position, LeafCallback&& leaf_callback) {
        const auto& left = slices_.at(position);
        const auto& right = slices_.at(position + 1U);
        if (left.depth() != right.depth()) {
            throw std::logic_error("neighboring slice trees have different depths");
        }
        const PairGate* parent_gate = nullptr;
        if (pair_gates_ready_) {
            parent_gate = &pair_gates_.at(position);
            if (pair_gate_depth_ > left.depth()) {
                throw std::logic_error("pair gate is deeper than its slice trees");
            }
        }

        std::uint64_t cursor = 0;
        std::vector<std::uint8_t> history(left.depth());
        auto callback = std::forward<LeafCallback>(leaf_callback);
        auto run = [&]<bool CountStats, PairGateLocation GateLocation>() {
            candidate_pair_dfs<CountStats, VisitRejected, TrackSummary,
                               GateLocation>(
                left, right, 0, 0, 0, history, parent_gate,
                cursor, true, PairPathSummary{}, callback);
        };
        auto run_for_gate = [&]<PairGateLocation GateLocation>() {
            if (options_.verbose) {
                run.template operator()<true, GateLocation>();
            } else {
                run.template operator()<false, GateLocation>();
            }
        };
        if (parent_gate == nullptr) {
            run_for_gate.template operator()<PairGateLocation::None>();
        } else if (pair_gate_depth_ + 1U == left.depth()) {
            run_for_gate.template operator()<PairGateLocation::ParentOfLeaf>();
        } else if (pair_gate_depth_ == left.depth()) {
            run_for_gate.template operator()<PairGateLocation::Leaf>();
        } else {
            throw std::logic_error("pair gate is not on the current or parent leaf level");
        }
        if (parent_gate != nullptr && cursor != parent_gate->size()) {
            throw std::logic_error("pair gate has bits beyond its DFS enumeration");
        }
    }

    template<class LeafCallback>
    void walk_current_edges(std::size_t position, LeafCallback&& leaf_callback) {
        if (!pair_gates_ready_ || pair_gate_depth_ != height_) {
            throw std::logic_error("current pair gates are unavailable");
        }
        auto callback = std::forward<LeafCallback>(leaf_callback);
        walk_candidate_pairs<false, true>(position,
            [&](Node left_leaf, Node right_leaf, const auto& history, bool allowed,
                const PairPathSummary& summary) {
                if (allowed) callback(left_leaf, right_leaf, history, summary);
            });
    }

    bool prune_supported() {
        if (slices_.empty()) return false;
        const auto adjacency_count = slices_.size() - 1U;
        std::vector<TagPair> normal;
        normal.reserve(slices_.size());
        for (const auto& slice : slices_) normal.emplace_back(slice.node_count());
        account_tags(normal);

        const auto bcaf_window = 2U * static_cast<std::size_t>(geometry_.period) + 1U;
        const bool bcaf_active = options_.bcaf && height_ >= bcaf_window;
        std::vector<TagPair> witness;
        if (bcaf_active) {
            witness.reserve(slices_.size());
            for (const auto& slice : slices_) witness.emplace_back(slice.node_count());
            peak_tag_bytes_ = std::max(
                peak_tag_bytes_, tag_bytes(normal) + tag_bytes(witness));
        }

        // The two planes remain pure boundary reachability.  Pair-gate bits
        // ensure that a compatibility removed at an earlier height cannot be
        // recreated merely because both endpoint slices still exist.
        mark_boundary(slices_.front(), options_.left_edge, true, normal.front()[0]);
        mark_boundary(slices_.back(), options_.right_edge, false, normal.back()[1]);
        std::vector<TagPair> clean;
        if (bcaf_active) {
            // Right reachability is sufficient to build the suffix witness:
            // once a left-to-right path reaches such a node, its witnessed
            // suffix completes a full path.  Symmetrically, left reachability
            // is sufficient for the prefix witness.  Computing the two in
            // dependency order lets the normal left-to-right sweep also be the
            // BCAF prefix and cleanup sweep.
            auto seed_suffix = [&](std::size_t position) {
                walk_leaves(slices_[position],
                    [&](Node leaf, std::size_t first_nonzero, std::size_t) {
                        if (normal[position][1].get(leaf)
                            && first_nonzero < bcaf_window) {
                            witness[position][1].set(leaf);
                        }
                    });
            };

            phase("  relation sweep: right to left + bcaf suffix");
            seed_suffix(slices_.size() - 1U);
            for (std::size_t i = adjacency_count; i > 0; --i) {
                normal[i - 1U][1].clear();
                walk_candidate_pairs<false>(i - 1U,
                    [&](Node left_leaf, Node right_leaf, const auto&,
                        bool ancestry_allowed) {
                        const bool reaches_right = ancestry_allowed
                            && normal[i][1].get(right_leaf);
                        if (reaches_right) {
                            normal[i - 1U][1].set(left_leaf);
                            if (witness[i][1].get(right_leaf)) {
                                witness[i - 1U][1].set(left_leaf);
                            }
                        }
                    });
                seed_suffix(i - 1U);
            }

            clean.reserve(slices_.size());
            for (const auto& slice : slices_) clean.emplace_back(slice.node_count());
            peak_tag_bytes_ = std::max(
                peak_tag_bytes_,
                tag_bytes(normal) + tag_bytes(witness) + tag_bytes(clean));

            auto seed_prefix = [&](std::size_t position) {
                walk_leaves(slices_[position],
                    [&](Node leaf, std::size_t first_nonzero, std::size_t) {
                        if (normal[position][0].get(leaf)
                            && first_nonzero < bcaf_window) {
                            witness[position][0].set(leaf);
                        }
                    });
            };

            seed_prefix(0);
            for (Node leaf = slices_.front().leaf_begin();
                 leaf < slices_.front().leaf_end(); ++leaf) {
                if (normal.front()[0].get(leaf)
                    && normal.front()[1].get(leaf)) {
                    clean.front()[0].set(leaf);
                }
            }

            phase("  relation sweep: left to right + bcaf prefix/cleanup");
            for (std::size_t i = 0; i < adjacency_count; ++i) {
                normal[i + 1U][0].clear();
                walk_candidate_pairs<false>(i,
                    [&](Node left_leaf, Node right_leaf, const auto&,
                        bool ancestry_allowed) {
                        const bool reaches_left = ancestry_allowed
                            && normal[i][0].get(left_leaf);
                        if (reaches_left) {
                            normal[i + 1U][0].set(right_leaf);
                        }
                        const bool normal_edge = reaches_left
                            && normal[i + 1U][1].get(right_leaf);
                        const bool interesting_prefix = witness[i][0].get(left_leaf);
                        if (normal_edge && interesting_prefix) {
                            witness[i + 1U][0].set(right_leaf);
                        }
                        const bool interesting_path = interesting_prefix
                            || witness[i + 1U][1].get(right_leaf);
                        if (normal_edge && interesting_path
                            && clean[i][0].get(left_leaf)) {
                            clean[i + 1U][0].set(right_leaf);
                        }
                    });
                seed_prefix(i + 1U);
            }
        } else {
            phase("  relation sweep: left to right");
            for (std::size_t i = 0; i < adjacency_count; ++i) {
                normal[i + 1U][0].clear();
                walk_candidate_pairs<false>(i,
                    [&](Node left_leaf, Node right_leaf, const auto&,
                        bool ancestry_allowed) {
                        if (ancestry_allowed && normal[i][0].get(left_leaf)) {
                            normal[i + 1U][0].set(right_leaf);
                        }
                    });
            }

            phase("  relation sweep: right to left");
            for (std::size_t i = adjacency_count; i > 0; --i) {
                normal[i - 1U][1].clear();
                walk_candidate_pairs<false>(i - 1U,
                    [&](Node left_leaf, Node right_leaf, const auto&,
                        bool ancestry_allowed) {
                        if (ancestry_allowed && normal[i][1].get(right_leaf)) {
                            normal[i - 1U][1].set(left_leaf);
                        }
                    });
            }
        }

        auto normally_live = [&](std::size_t position, Node leaf) {
            return normal[position][0].get(leaf) && normal[position][1].get(leaf);
        };

        if (normal.front()[0].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0
            || normal.front()[1].count(slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0) {
            return false;
        }

        std::vector<PairGate> next_gates(adjacency_count);
        bool slices_reified = false;

        if (bcaf_active) {
            phase("  bcaf relation gate");

            // Keeping a full temporary BCAF relation tape here makes it overlap
            // both the old and the newly produced persistent tapes.  Retain the
            // two witness planes and recompute the inexpensive edge predicate
            // during cleanup/final walks.  This costs two more tag bits per
            // expanded node, but removes one bit for every compatible
            // neighboring leaf pair -- a much larger term in broad searches.
            for (Node leaf = slices_.back().leaf_begin();
                 leaf < slices_.back().leaf_end(); ++leaf) {
                if (clean.back()[0].get(leaf)) clean.back()[1].set(leaf);
            }
            for (std::size_t i = adjacency_count; i > 0; --i) {
                walk_candidate_pairs<false>(i - 1U,
                    [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
                        const bool normal_edge = ancestry_allowed
                            && normal[i - 1U][0].get(left_leaf)
                            && normal[i][1].get(right_leaf);
                        const bool interesting_path = witness[i - 1U][0].get(left_leaf)
                            || witness[i][1].get(right_leaf);
                        const bool edge = normal_edge && interesting_path;
                        if (edge && clean[i - 1U][0].get(left_leaf)
                            && clean[i][1].get(right_leaf)) {
                            clean[i - 1U][1].set(left_leaf);
                        }
                    });
            }

            if (clean.front()[1].count(
                    slices_.front().leaf_begin(), slices_.front().leaf_end()) == 0) {
                return false;
            }

            // Emit only bits whose endpoints survive reification.  Once gate i
            // has been emitted, old slice i and its tags can never be consulted
            // again: later walks start at slice i+1.  Reify and release it here
            // so the growing final gate tape does not overlap all expanded
            // slices and all six tag planes.
            for (std::size_t i = 0; i < adjacency_count; ++i) {
                walk_candidate_pairs(i,
                    [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
                        const bool normal_edge = ancestry_allowed
                            && normal[i][0].get(left_leaf)
                            && normal[i + 1U][1].get(right_leaf);
                        const bool interesting_path = witness[i][0].get(left_leaf)
                            || witness[i + 1U][1].get(right_leaf);
                        const bool bcaf_edge = normal_edge && interesting_path;
                        const bool keep_left = clean[i][1].get(left_leaf);
                        const bool keep_right = clean[i + 1U][1].get(right_leaf);
                        if (keep_left && keep_right) {
                            const bool final_edge = bcaf_edge
                                && clean[i][0].get(left_leaf)
                                && clean[i + 1U][1].get(right_leaf);
                            next_gates[i].push_back(final_edge);
                        }
                    });
                if (pair_gates_ready_) pair_gates_[i] = PairGate{};
                if (!slices_[i].reify(clean[i][1])) return false;
                normal[i] = TagPair{};
                witness[i] = TagPair{};
                clean[i] = TagPair{};
            }
            if (!slices_.back().reify(clean.back()[1])) return false;
            normal.back() = TagPair{};
            witness.back() = TagPair{};
            clean.back() = TagPair{};
            slices_reified = true;
        } else {
            for (std::size_t i = 0; i < adjacency_count; ++i) {
                walk_candidate_pairs(i,
                    [&](Node left_leaf, Node right_leaf, const auto&, bool ancestry_allowed) {
                        const bool keep_left = normally_live(i, left_leaf);
                        const bool keep_right = normally_live(i + 1U, right_leaf);
                        if (keep_left && keep_right) {
                            const bool final_edge = ancestry_allowed
                                && normal[i][0].get(left_leaf)
                                && normal[i + 1U][1].get(right_leaf);
                            next_gates[i].push_back(final_edge);
                        }
                    });
            }
        }

        if (!slices_reified) {
            for (std::size_t i = 0; i < slices_.size(); ++i) {
                auto& keep = normal[i][0];
                for (Node leaf = slices_[i].leaf_begin();
                     leaf < slices_[i].leaf_end(); ++leaf) {
                    keep.set(leaf,
                             normal[i][0].get(leaf) && normal[i][1].get(leaf));
                }
                if (!slices_[i].reify(keep)) return false;
            }
        }

        pair_gates_ = std::move(next_gates);
        pair_gate_depth_ = height_;
        pair_gates_ready_ = true;
        return true;
    }

    template<class LeafCallback>
    void leaf_dfs(const SuccinctSliceTree& tree, Node node, std::size_t depth,
                  std::size_t first_nonzero, std::size_t last_nonzero,
                  std::vector<Node>& child_cursor,
                  LeafCallback& callback) const {
        if (depth == tree.depth()) {
            callback(node, first_nonzero, last_nonzero);
            return;
        }
        const auto mask = tree.child_mask(node);
        auto child = child_cursor[depth];
        child_cursor[depth] += static_cast<Node>(std::popcount(mask));
        for (std::uint8_t label = 0; label < 4; ++label) {
            if ((mask & (1U << label)) == 0) continue;
            const auto next = child++;
            auto next_first = first_nonzero;
            auto next_last = last_nonzero;
            if (label != 0) {
                if (next_first == std::numeric_limits<std::size_t>::max()) {
                    next_first = depth;
                }
                next_last = depth;
            }
            leaf_dfs(tree, next, depth + 1, next_first, next_last,
                     child_cursor, callback);
        }
    }

    template<class LeafCallback>
    void walk_leaves(const SuccinctSliceTree& tree, LeafCallback&& callback) const {
        auto actual = std::forward<LeafCallback>(callback);
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        std::vector<Node> child_cursor(tree.depth());
        for (std::size_t depth = 0; depth < tree.depth(); ++depth) {
            child_cursor[depth] = tree.level_begin(depth + 1U);
        }
        leaf_dfs(tree, 0, 0, none, none, child_cursor, actual);
    }

    static bool labels_interesting(const std::vector<std::uint8_t>& labels,
                                   std::size_t window) {
        if (labels.size() < window) return false;
        return std::any_of(labels.end() - static_cast<std::ptrdiff_t>(window), labels.end(),
                           [](std::uint8_t label) { return label != 0; });
    }

    static bool labels_prefix_interesting(const std::vector<std::uint8_t>& labels,
                                          std::size_t window) {
        if (labels.size() < window) return false;
        return std::any_of(labels.begin(),
                           labels.begin() + static_cast<std::ptrdiff_t>(window),
                           [](std::uint8_t label) { return label != 0; });
    }

    std::optional<Board> find_completion() {
        const auto short_window = 2U * static_cast<std::size_t>(geometry_.period);
        const auto long_window = short_window + 1U;
        if (height_ < long_window) return std::nullopt;

        std::vector<TagPair> suffix;
        suffix.reserve(slices_.size());
        for (const auto& slice : slices_) suffix.emplace_back(slice.node_count());
        account_tags(suffix);

        const auto short_start = height_ - short_window;
        const auto long_start = height_ - long_window;
        walk_leaves(slices_.back(), [&](Node leaf, std::size_t,
                                        std::size_t last_nonzero) {
            const bool valid = last_nonzero == std::numeric_limits<std::size_t>::max()
                || last_nonzero < short_start;
            if (valid) suffix.back()[0].set(leaf);
            if (valid && last_nonzero != std::numeric_limits<std::size_t>::max()
                && last_nonzero >= long_start) {
                suffix.back()[1].set(leaf);
            }
        });

        for (std::size_t i = slices_.size() - 1; i > 0; --i) {
            walk_current_edges(i - 1U,
                [&](Node left_leaf, Node right_leaf, const auto&,
                    const PairPathSummary& summary) {
                    const auto last = summary.last_left_nonzero;
                    const bool zero_tail = last == std::numeric_limits<std::size_t>::max()
                        || last < short_start;
                    if (!suffix[i][0].get(right_leaf)
                        || !zero_tail) return;
                    suffix[i - 1][0].set(left_leaf);
                    if (suffix[i][1].get(right_leaf)
                        || (last != std::numeric_limits<std::size_t>::max()
                            && last >= long_start)) {
                        suffix[i - 1][1].set(left_leaf);
                    }
                });
        }

        Node first = slices_.front().leaf_end();
        for (Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
            if (suffix.front()[1].get(leaf)) {
                first = leaf;
                break;
            }
        }
        if (first == slices_.front().leaf_end()) return std::nullopt;
        return reconstruct_end(first, suffix, long_window);
    }

    template<class Predicate>
    std::pair<Node, std::vector<std::uint8_t>> find_current_right(
        std::size_t position, Node left_leaf, Predicate&& predicate) {
        Node result = slices_[position + 1U].leaf_end();
        std::vector<std::uint8_t> result_labels;
        auto actual = std::forward<Predicate>(predicate);
        walk_current_edges(position,
            [&](Node candidate_left, Node candidate_right, const auto&, const auto&) {
                if (result != slices_[position + 1U].leaf_end()
                    || candidate_left != left_leaf) return;
                auto labels = slices_[position + 1U].lineage(candidate_right);
                if (actual(candidate_right, labels)) {
                    result = candidate_right;
                    result_labels = std::move(labels);
                }
            });
        if (result == slices_[position + 1U].leaf_end()) {
            throw std::logic_error("supported slice has no allowed successor");
        }
        return {result, std::move(result_labels)};
    }

    Board board_from_lineages(const std::vector<std::vector<std::uint8_t>>& lineages) const {
        Board board(height_, std::vector<std::uint8_t>(width_, 0));
        for (std::size_t row = 0; row < height_; ++row) {
            board[row][0] = static_cast<std::uint8_t>((lineages[0][row] >> 1U) & 1U);
            board[row][1] = static_cast<std::uint8_t>(lineages[0][row] & 1U);
            for (std::size_t slice = 1; slice < lineages.size(); ++slice) {
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
        for (std::size_t i = 1; i < slices_.size(); ++i) {
            auto [node, labels] = find_current_right(
                i - 1U, current, [](Node, const auto&) { return true; });
            current = node;
            lineages.push_back(std::move(labels));
        }
        return board_from_lineages(lineages);
    }

    Board reconstruct_interesting(std::size_t window) {
        std::vector<TagPair> suffix;
        suffix.reserve(slices_.size());
        for (const auto& slice : slices_) suffix.emplace_back(slice.node_count());
        account_tags(suffix);

        walk_leaves(slices_.back(), [&](Node leaf, std::size_t first_nonzero,
                                        std::size_t) {
            if (first_nonzero < window) suffix.back()[0].set(leaf);
        });
        for (std::size_t i = slices_.size() - 1; i > 0; --i) {
            walk_current_edges(i - 1U,
                [&](Node left_leaf, Node right_leaf, const auto&,
                    const PairPathSummary& summary) {
                    if (suffix[i][0].get(right_leaf)
                        || summary.first_left_nonzero < window) {
                        suffix[i - 1][0].set(left_leaf);
                    }
                });
        }

        Node first = slices_.front().leaf_end();
        for (Node leaf = slices_.front().leaf_begin(); leaf < slices_.front().leaf_end(); ++leaf) {
            if (suffix.front()[0].get(leaf)) {
                first = leaf;
                break;
            }
        }
        if (first == slices_.front().leaf_end()) {
            throw std::logic_error("bcaf-projected state has no interesting path");
        }

        std::vector<std::vector<std::uint8_t>> lineages;
        lineages.reserve(slices_.size());
        Node current = first;
        lineages.push_back(slices_.front().lineage(first));
        bool seen = labels_prefix_interesting(lineages.front(), window);
        for (std::size_t i = 1; i < slices_.size(); ++i) {
            auto [node, labels] = find_current_right(
                i - 1U, current, [&](Node leaf, const auto&) {
                    return seen || suffix[i][0].get(leaf);
                });
            current = node;
            seen = seen || labels_prefix_interesting(labels, window);
            lineages.push_back(std::move(labels));
        }
        if (!seen) {
            throw std::logic_error("interesting-path reconstruction lost its witness");
        }
        return board_from_lineages(lineages);
    }

    Board reconstruct_partial() {
        const auto bcaf_window = 2U * static_cast<std::size_t>(geometry_.period) + 1U;
        if (options_.bcaf && height_ >= bcaf_window) {
            return reconstruct_interesting(bcaf_window);
        }
        return reconstruct_any();
    }

    Board reconstruct_end(Node first, const std::vector<TagPair>& suffix,
                          std::size_t long_window) {
        std::vector<std::vector<std::uint8_t>> lineages;
        lineages.reserve(slices_.size());
        Node current = first;
        lineages.push_back(slices_.front().lineage(first));
        bool seen_interesting = labels_interesting(lineages.front(), long_window);
        for (std::size_t i = 1; i < slices_.size(); ++i) {
            const auto required_plane = seen_interesting ? 0U : 1U;
            auto [node, labels] = find_current_right(
                i - 1U, current, [&](Node leaf, const auto&) {
                    return suffix[i][required_plane].get(leaf);
                });
            current = node;
            seen_interesting = seen_interesting || labels_interesting(labels, long_window);
            lineages.push_back(std::move(labels));
        }
        if (!seen_interesting) {
            throw std::logic_error("end reconstruction lost its interesting witness");
        }
        return board_from_lineages(lineages);
    }

    static std::vector<std::uint8_t> reflect_left(
        const std::vector<std::uint8_t>& row, EdgeMode mode) {
        if (mode == EdgeMode::Background) return row;
        std::vector<std::uint8_t> result;
        result.reserve(mode == EdgeMode::Odd ? 2U * row.size() - 1U
                                             : 2U * row.size());
        // Odd reflection shares the boundary cell; even reflection mirrors
        // across the gap immediately outside it and therefore duplicates it.
        const std::size_t stop = mode == EdgeMode::Odd ? 1U : 0U;
        for (std::size_t index = row.size(); index > stop; --index) {
            result.push_back(row[index - 1U]);
        }
        result.insert(result.end(), row.begin(), row.end());
        return result;
    }

    static void reflect_right(std::vector<std::uint8_t>& row, EdgeMode mode) {
        if (mode == EdgeMode::Background) return;
        const auto original_size = row.size();
        row.reserve(mode == EdgeMode::Odd ? 2U * original_size - 1U
                                          : 2U * original_size);
        const std::size_t start = mode == EdgeMode::Odd ? original_size - 1U
                                                        : original_size;
        for (std::size_t index = start; index > 0; --index) {
            row.push_back(row[index - 1U]);
        }
    }

    std::vector<std::uint8_t> reflect_edges(
        const std::vector<std::uint8_t>& row) const {
        auto result = reflect_left(row, options_.left_edge);
        // If both boundaries are symmetric this deliberately reflects the
        // already left-expanded row a second time.  It is a useful finite view
        // of the otherwise spatially periodic configuration.
        reflect_right(result, options_.right_edge);
        return result;
    }

    Board render_phase_montage(const Board& row_sequence) const {
        constexpr std::size_t phase_spacing = 16;
        const auto period = static_cast<std::size_t>(geometry_.period);
        std::vector<std::vector<std::uint8_t>> reflected;
        reflected.reserve(row_sequence.size());
        for (const auto& row : row_sequence) {
            reflected.push_back(reflect_edges(row));
        }

        const auto phase_width = reflected.front().size();
        const auto phase_height = (reflected.size() + period - 1U) / period;
        const auto montage_width = period * phase_width
                                 + (period - 1U) * phase_spacing;
        Board montage(phase_height, std::vector<std::uint8_t>(montage_width, 0));
        for (std::size_t phase = 0; phase < period; ++phase) {
            const auto x = phase * (phase_width + phase_spacing);
            std::size_t y = 0;
            for (std::size_t source = phase; source < reflected.size(); source += period) {
                std::copy(reflected[source].begin(), reflected[source].end(),
                          montage[y].begin() + static_cast<std::ptrdiff_t>(x));
                ++y;
            }
        }
        return montage;
    }

    static std::string encode_rle(const Board& board) {
        std::vector<std::string> tokens;
        auto token = [&](std::size_t count, char symbol) {
            tokens.push_back((count > 1 ? std::to_string(count) : std::string{}) + symbol);
        };
        for (std::size_t y = 0; y < board.size(); ++y) {
            std::size_t x = 0;
            while (x < board[y].size()) {
                const auto value = board[y][x];
                std::size_t end = x + 1;
                while (end < board[y].size() && board[y][end] == value) ++end;
                token(end - x, value ? 'o' : 'b');
                x = end;
            }
            if (y + 1 < board.size()) token(1, '$');
        }
        token(1, '!');

        std::ostringstream output;
        std::size_t column = 0;
        for (const auto& part : tokens) {
            if (column != 0 && column + part.size() > 70) {
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
        const auto board = render_phase_montage(row_sequence);
        std::ostream& output = partial_file_.is_open()
            ? static_cast<std::ostream&>(partial_file_)
            : static_cast<std::ostream&>(std::cout);
        output << "#C llsss " << kind << " height=" << height_
               << " geometry=" << geometry_.source << '\n'
               << "#C phases 0.." << geometry_.period - 1
               << " left-to-right; phase i uses rows i, p+i, 2p+i, ..."
               << "; gap=16\n"
               << "x = " << board.front().size() << ", y = " << board.size()
               << ", rule = " << options_.rule << '\n'
               << encode_rle(board);
        output.flush();
    }

    void emit_final_partial(std::string_view kind) {
        if (options_.partial_mode != PartialMode::None && !slices_.empty()) {
            emit_board(reconstruct_partial(), kind);
        }
    }

    void phase(std::string_view message) const {
        if (running_ && options_.phase_progress) {
            std::cout << "height=" << height_ << " " << message << '\n';
        }
    }

    static std::size_t tag_bytes(const std::vector<TagPair>& tags) {
        std::size_t bytes = 0;
        for (const auto& pair : tags) {
            bytes += pair[0].allocated_bytes() + pair[1].allocated_bytes();
        }
        return bytes;
    }

    void account_tags(const std::vector<TagPair>& tags) {
        peak_tag_bytes_ = std::max(peak_tag_bytes_, tag_bytes(tags));
    }

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
        for (std::size_t i = 0; i < slices_.size(); ++i) {
            if (i != 0) {
                per_slice << ',';
                per_slice_leaves << ',';
            }
            per_slice << slices_[i].node_count();
            per_slice_leaves << slices_[i].leaf_count();
            slice_state << slices_[i].leaf_count();
            if (pair_gates_ready_ && i < pair_gates_.size()) {
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
        if (pair_gates_ready_) {
            for (const auto& gate : pair_gates_) {
                pair_candidates += gate.size();
                pair_allowed += gate.count();
                pair_gate_bytes += gate.allocated_bytes();
            }
        }
        const auto lookup_bytes = rule_.storage_bytes()
            + row_acceptance_.size() * sizeof(row_acceptance_[0])
            + odd_edge_acceptance_.size() * sizeof(odd_edge_acceptance_[0])
            + even_edge_acceptance_.size() * sizeof(even_edge_acceptance_[0])
            + pair_transitions_.size() * sizeof(pair_transitions_[0]);
        const auto persistent_payload_bytes = allocated + pair_gate_bytes + lookup_bytes;
        std::ostringstream line;
        if (options_.verbose) {
            line << "height=" << height_ << " label=" << label
                << " nodes=" << nodes << " leaves=" << leaves
                << " child_bytes=" << bitstream << " rank_bytes=" << rank
                << " level_bytes=" << levels << " allocated_bytes=" << allocated
                << " pair_gate_bytes=" << pair_gate_bytes
                << " lookup_bytes=" << lookup_bytes
                << " persistent_payload_bytes=" << persistent_payload_bytes
                << " pair_candidates=" << pair_candidates
                << " pair_allowed=" << pair_allowed
                << " tag_peak_bytes=" << peak_tag_bytes_
                << " pair_states=" << pair_states_ << " pair_leaves=" << pair_leaves_
                << " boundary_states=" << boundary_states_
                << " seconds=" << std::fixed << std::setprecision(6) << seconds
                << " slice_nodes=" << per_slice.str()
                << " slice_leaves=" << per_slice_leaves.str()
                << " slice_state=" << slice_state.str();
        } else {
            std::uint64_t maxrss = getMaxRSS();
            std::string maxrss_display;
            if (maxrss)
                maxrss_display = integer_format(maxrss) + "iB RSS, ";
            line << "Row " << height_ << ", " << nodes << " nodes, mem "
                 << integer_format(persistent_payload_bytes) << "iB, " << maxrss_display << "sec "
                 << std::fixed << std::setprecision(6) << seconds << ", cols: "
                 << slice_state.str();
        }
        std::cerr << line.str() << '\n';
        if (stats_file_) {
            stats_file_ << line.str() << '\n';
            stats_file_.flush();
        }
    }

    Options options_;
    Geometry geometry_;
    RuleTable rule_;
    std::array<std::uint8_t, 1U << 15U> row_acceptance_{};
    std::array<std::uint8_t, 1U << 10U> odd_edge_acceptance_{};
    std::array<std::uint8_t, 1U << 10U> even_edge_acceptance_{};
    std::array<PairTransitions, 1U << 8U> pair_transitions_{};
    std::array<std::size_t, 5> pair_history_offsets_{};
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::vector<SuccinctSliceTree> slices_;
    std::vector<PairGate> pair_gates_;
    std::size_t pair_gate_depth_ = 0;
    bool pair_gates_ready_ = false;
    std::ofstream partial_file_;
    std::ofstream stats_file_;
    std::uint64_t pair_states_ = 0;
    std::uint64_t pair_leaves_ = 0;
    std::uint64_t boundary_states_ = 0;
    std::size_t peak_tag_bytes_ = 0;
    bool running_ = false;
};

} // namespace rlife::llsss
