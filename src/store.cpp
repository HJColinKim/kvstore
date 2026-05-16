#include "store.hpp"

#include <mutex>
#include <stdexcept>

namespace {

bool is_expired(const std::optional<std::chrono::steady_clock::time_point>& expires_at,
                std::chrono::steady_clock::time_point now) noexcept {
    return expires_at.has_value() && *expires_at <= now;
}

}  // namespace

Store::Store(std::size_t shard_count) {
    if (shard_count == 0) {
        throw std::invalid_argument("Store: shard_count must be > 0");
    }
    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

Store::Shard& Store::shard_for(std::string_view key) {
    const std::size_t h = std::hash<std::string_view>{}(key);
    return *shards_[h % shards_.size()];
}

void Store::set(std::string_view key, std::string_view value) {
    auto& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto& entry = s.map[std::string(key)];
    entry.value.assign(value);
    entry.expires_at.reset();
}

void Store::set_with_ttl(std::string_view key, std::string_view value,
                         std::chrono::seconds ttl) {
    const auto deadline = clock::now() + ttl;
    auto& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto& entry = s.map[std::string(key)];
    entry.value.assign(value);
    entry.expires_at = deadline;
}

std::optional<std::string> Store::get(std::string_view key) {
    auto& s = shard_for(key);
    {
        std::shared_lock lock(s.mtx);
        auto it = s.map.find(key);
        if (it == s.map.end()) return std::nullopt;
        if (!is_expired(it->second.expires_at, clock::now())) {
            return it->second.value;
        }
    }
    // Entry was observed expired. std::shared_mutex does not support lock
    // upgrade, so drop the shared lock and re-acquire exclusively; another
    // writer may have refreshed or removed it in the meantime, so re-check
    // before erasing.
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    if (is_expired(it->second.expires_at, clock::now())) {
        s.map.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool Store::del(std::string_view key) {
    auto& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    // erase has no heterogeneous overload, so go through find first.
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    s.map.erase(it);
    return true;
}

bool Store::exists(std::string_view key) {
    auto& s = shard_for(key);
    {
        std::shared_lock lock(s.mtx);
        auto it = s.map.find(key);
        if (it == s.map.end()) return false;
        if (!is_expired(it->second.expires_at, clock::now())) return true;
    }
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    if (is_expired(it->second.expires_at, clock::now())) {
        s.map.erase(it);
        return false;
    }
    return true;
}

bool Store::expire(std::string_view key, std::chrono::seconds ttl) {
    const auto deadline = clock::now() + ttl;
    auto& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    if (is_expired(it->second.expires_at, clock::now())) {
        s.map.erase(it);
        return false;
    }
    it->second.expires_at = deadline;
    return true;
}

std::vector<std::string> Store::keys() {
    std::vector<std::string> out;
    for (auto& sp : shards_) {
        std::shared_lock lock(sp->mtx);
        out.reserve(out.size() + sp->map.size());
        const auto now = clock::now();
        for (const auto& [k, e] : sp->map) {
            if (!is_expired(e.expires_at, now)) out.push_back(k);
        }
    }
    return out;
}
