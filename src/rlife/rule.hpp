#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rlife::llsss {

// A direct 10-bit Moore-neighborhood table.  Bits 0..8 are the three
// left-to-right rows of the neighborhood and bit 9 is the future center.
class RuleTable {
public:
    static RuleTable parse(const std::string& text) {
        RuleTable table;
        table.parse_hensel(text);
        table.build_partial_lookup();
        return table;
    }

    [[nodiscard]] bool accepts(std::uint16_t neighborhood) const noexcept {
        return values_[neighborhood & 0x3ffU] != 0;
    }

    [[nodiscard]] bool accepts_partial(std::uint16_t known,
                                       std::uint16_t value) const noexcept {
        std::size_t key = 0;
        std::size_t place = 1;
        for (unsigned i = 0; i < 10; ++i) {
            const auto bit = static_cast<std::uint16_t>(1U << i);
            if ((known & bit) != 0) {
                key += place * (((value & bit) != 0) ? 2U : 1U);
            }
            place *= 3U;
        }
        return ((partial_values_[key >> 6U] >> (key & 63U)) & 1U) != 0;
    }

    void release_partial_lookup() {
        partial_values_.clear();
        partial_values_.shrink_to_fit();
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return sizeof(values_) + partial_values_.capacity() * sizeof(std::uint64_t);
    }

private:
    struct NeighborhoodType {
        std::vector<std::tuple<char, char, std::uint16_t>> representatives;
        std::array<std::pair<char, char>, 512> pattern_to_class{};
    };

    std::array<std::uint8_t, 1024> values_{};
    static constexpr std::size_t partial_pattern_count_ = 59049; // 3^10
    std::vector<std::uint64_t> partial_values_;

    void build_partial_lookup() {
        partial_values_.assign((partial_pattern_count_ + 63U) / 64U, 0);
        for (std::uint16_t exact = 0; exact < 1024; ++exact) {
            if (values_[exact] == 0) continue;
            for (std::uint16_t unknown = 0; unknown < 1024; ++unknown) {
                std::size_t key = 0;
                std::size_t place = 1;
                for (unsigned i = 0; i < 10; ++i) {
                    const auto bit = static_cast<std::uint16_t>(1U << i);
                    if ((unknown & bit) == 0) {
                        key += place * (((exact & bit) != 0) ? 2U : 1U);
                    }
                    place *= 3U;
                }
                partial_values_[key >> 6U] |= std::uint64_t{1} << (key & 63U);
            }
        }
    }

    static const std::array<std::pair<int, int>, 9>& neighborhood_order() {
        static const std::array<std::pair<int, int>, 9> order = {
            std::pair{-1, -1}, std::pair{0, -1}, std::pair{1, -1},
            std::pair{-1,  0}, std::pair{0,  0}, std::pair{1,  0},
            std::pair{-1,  1}, std::pair{0,  1}, std::pair{1,  1},
        };
        return order;
    }

    static int neighborhood_index(int x, int y) {
        const auto& order = neighborhood_order();
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[i] == std::pair{x, y}) {
                return i;
            }
        }
        throw std::logic_error("Moore-neighborhood coordinate is missing");
    }

    static bool bit(std::uint16_t value, int index) {
        return ((value >> index) & 1U) != 0;
    }

    static void set_bit(std::uint16_t& value, int index, bool state) {
        const auto mask = static_cast<std::uint16_t>(1U << index);
        value = state ? static_cast<std::uint16_t>(value | mask)
                      : static_cast<std::uint16_t>(value & ~mask);
    }

    static std::uint16_t rotate90(std::uint16_t value) {
        std::uint16_t result = 0;
        for (int i = 0; i < 9; ++i) {
            const auto [x, y] = neighborhood_order()[i];
            set_bit(result, neighborhood_index(-y, x), bit(value, i));
        }
        return result;
    }

    static std::uint16_t flip_x(std::uint16_t value) {
        std::uint16_t result = 0;
        for (int i = 0; i < 9; ++i) {
            const auto [x, y] = neighborhood_order()[i];
            set_bit(result, neighborhood_index(-x, y), bit(value, i));
        }
        return result;
    }

    // The Hensel representatives are the one allowed piece copied from the
    // earlier common helper.  The search engine itself shares no architecture
    // or storage with that implementation.
    static NeighborhoodType make_moore_type() {
        NeighborhoodType type;
        type.representatives = {
            {'0', 'x', 0b000000000},
            {'1', 'c', 0b100000000}, {'1', 'e', 0b010000000},
            {'2', 'c', 0b101000000}, {'2', 'e', 0b010100000},
            {'2', 'k', 0b010000001}, {'2', 'a', 0b110000000},
            {'2', 'i', 0b010000010}, {'2', 'n', 0b100000001},
            {'3', 'c', 0b101000001}, {'3', 'e', 0b010101000},
            {'3', 'k', 0b010100001}, {'3', 'a', 0b110100000},
            {'3', 'i', 0b100100100}, {'3', 'n', 0b101100000},
            {'3', 'y', 0b101000010}, {'3', 'q', 0b100100001},
            {'3', 'j', 0b001001010}, {'3', 'r', 0b011000010},
            {'4', 'c', 0b101000101}, {'4', 'e', 0b010101010},
            {'4', 'k', 0b011100001}, {'4', 'a', 0b100100110},
            {'4', 'i', 0b101101000}, {'4', 'n', 0b100100101},
            {'4', 'y', 0b101000110}, {'4', 'q', 0b110100001},
            {'4', 'j', 0b001101010}, {'4', 'r', 0b011001010},
            {'4', 't', 0b111000010}, {'4', 'w', 0b100100011},
            {'4', 'z', 0b110000011},
            {'5', 'c', 0b010101110}, {'5', 'e', 0b101000111},
            {'5', 'k', 0b101001110}, {'5', 'a', 0b001001111},
            {'5', 'i', 0b011001011}, {'5', 'n', 0b010001111},
            {'5', 'y', 0b010101101}, {'5', 'q', 0b011001110},
            {'5', 'j', 0b110100101}, {'5', 'r', 0b100101101},
            {'6', 'c', 0b010101111}, {'6', 'e', 0b101001111},
            {'6', 'k', 0b101101110}, {'6', 'a', 0b001101111},
            {'6', 'i', 0b101101101}, {'6', 'n', 0b011101110},
            {'7', 'c', 0b011101111}, {'7', 'e', 0b101101111},
            {'8', 'x', 0b111101111},
        };

        std::array<bool, 512> seen{};
        auto symmetries = [](std::uint16_t initial) {
            std::vector<std::uint16_t> result;
            std::vector<std::uint16_t> pending{initial};
            std::unordered_set<std::uint16_t> visited;
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                if (!visited.insert(current).second) {
                    continue;
                }
                result.push_back(current);
                pending.push_back(rotate90(current));
                pending.push_back(flip_x(current));
            }
            return result;
        };

        for (const auto& [count, letter, representative] : type.representatives) {
            for (const auto pattern : symmetries(representative)) {
                if (pattern >= type.pattern_to_class.size() || seen[pattern]) {
                    throw std::logic_error("invalid duplicate Hensel class");
                }
                seen[pattern] = true;
                type.pattern_to_class[pattern] = {count, letter};
            }
        }

        std::size_t mapped = 0;
        for (const bool value : seen) {
            mapped += value ? 1U : 0U;
        }
        if (mapped != 256) {
            throw std::logic_error("incomplete Hensel class table");
        }
        return type;
    }

    void parse_hensel(const std::string& rule) {
        const auto type = make_moore_type();
        std::unordered_map<char, std::vector<char>> legal_letters;
        for (const auto& [count, letter, ignored] : type.representatives) {
            (void)ignored;
            legal_letters[count].push_back(letter);
        }

        struct Key {
            bool center;
            char count;
            char letter;
            bool operator==(const Key&) const = default;
        };
        struct KeyHash {
            std::size_t operator()(const Key& key) const noexcept {
                std::size_t hash = key.center ? 1U : 0U;
                hash = hash * 257U + static_cast<unsigned char>(key.count);
                hash = hash * 257U + static_cast<unsigned char>(key.letter);
                return hash;
            }
        };

        // Pair is acceptance for a dead/live future bit.  Unmentioned classes
        // require a dead future bit.
        std::unordered_map<Key, std::pair<bool, bool>, KeyHash> classes;
        for (const auto& [count, letter, ignored] : type.representatives) {
            (void)ignored;
            classes.emplace(Key{false, count, letter}, std::pair{true, false});
            classes.emplace(Key{true, count, letter}, std::pair{true, false});
        }

        bool have_section = false;
        bool center = false;
        std::pair<bool, bool> selected{false, true};

        for (std::size_t i = 0; i < rule.size();) {
            const char ch = rule[i];
            if (ch == 'S' || ch == 's' || ch == 'B' || ch == 'b') {
                have_section = true;
                center = ch == 'S' || ch == 's';
                selected = {false, true};
                ++i;
                continue;
            }
            if (ch == '/') {
                ++i;
                continue;
            }
            if (ch == '!' || ch == '?') {
                if (!have_section) {
                    throw std::runtime_error("rule modifier appears before B/S");
                }
                selected = ch == '!' ? std::pair{false, false}
                                     : std::pair{true, true};
                ++i;
                continue;
            }
            if (ch >= '0' && ch <= '8') {
                if (!have_section) {
                    throw std::runtime_error("neighbor count appears before B/S");
                }
                const char count = ch;
                ++i;
                bool negate = false;
                if (i < rule.size() && rule[i] == '-') {
                    negate = true;
                    ++i;
                }
                std::unordered_set<char> listed;
                while (i < rule.size() && rule[i] >= 'a' && rule[i] <= 'z') {
                    listed.insert(rule[i++]);
                }
                const auto legal = legal_letters.find(count);
                if (legal == legal_letters.end()) {
                    throw std::runtime_error("illegal neighbor count in rule");
                }
                if (listed.empty() && negate) {
                    throw std::runtime_error("negated count needs Hensel letters");
                }
                for (const char letter : legal->second) {
                    if (listed.empty() || (listed.contains(letter) != negate)) {
                        classes[Key{center, count, letter}] = selected;
                    }
                }
                continue;
            }
            throw std::runtime_error("cannot parse rule near: " + rule.substr(i));
        }

        if (!have_section) {
            throw std::runtime_error("rule has no B/S section");
        }

        constexpr std::uint16_t neighbor_mask = 0b0111101111;
        for (std::uint16_t bits10 = 0; bits10 < 1024; ++bits10) {
            const auto neighbors = static_cast<std::uint16_t>(bits10 & neighbor_mask);
            const bool current_center = bit(bits10, 4);
            const bool future_center = bit(bits10, 9);
            const auto [count, letter] = type.pattern_to_class[neighbors];
            const auto found = classes.find(Key{current_center, count, letter});
            if (found == classes.end()) {
                throw std::logic_error("Hensel lookup construction failed");
            }
            values_[bits10] = static_cast<std::uint8_t>(
                future_center ? found->second.second : found->second.first);
        }
    }
};

} // namespace rlife::llsss
