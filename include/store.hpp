#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Transparent hashing/equality so std::unordered_map<std::string, ...> can be
// queried with a std::string_view without constructing a std::string for every
// lookup. Required since C++20 to enable the heterogeneous find() overload.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

class Store {
public:
    // Shards are independently locked unordered_maps; the only point of
    // contention is within a single shard. Default 16 is enough headroom past
    // common core counts on a single-box deployment. Throws std::invalid_argument
    // on 0.
    explicit Store(std::size_t shard_count = 16);

    void set(std::string_view key, std::string_view value);
    void set_with_ttl(std::string_view key, std::string_view value,
                      std::chrono::seconds ttl);

    std::optional<std::string> get(std::string_view key);
    bool del(std::string_view key);
    bool exists(std::string_view key);

    // Returns false if key is absent or already expired; otherwise overwrites
    // the existing entry's TTL.
    bool expire(std::string_view key, std::chrono::seconds ttl);

    // Per-shard snapshot of currently-live keys. Not a global snapshot: shards
    // are sampled sequentially under shared locks, so a key written to shard A
    // after we move on to shard B will be missed.
    std::vector<std::string> keys();

private:
    using clock = std::chrono::steady_clock;

    struct Entry {
        std::string value;
        std::optional<clock::time_point> expires_at;
    };

    using Map = std::unordered_map<std::string, Entry, StringHash, StringEqual>;

    struct Shard {
        mutable std::shared_mutex mtx;
        Map map;
    };

    // unique_ptr because std::shared_mutex is neither copyable nor movable,
    // which would otherwise prevent the vector from owning Shard by value.
    std::vector<std::unique_ptr<Shard>> shards_;

    Shard& shard_for(std::string_view key);
};
