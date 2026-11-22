#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace low_latency_graphdb {

// Open-addressing set of 64-bit keys that is cleared in O(1) by bumping a
// generation stamp, so steady-state queries neither allocate nor memset.
class GenerationSet {
public:
    void reserve(std::size_t keys) {
        std::size_t capacity = 16;
        while (capacity < keys * 2) {
            capacity <<= 1;
        }
        if (capacity > slots_.size()) {
            rebuild(capacity);
        }
    }

    void clear() {
        if (++generation_ == 0) {
            std::fill(slots_.begin(), slots_.end(), Slot{});
            generation_ = 1;
        }
        count_ = 0;
    }

    // Returns true if `key` was not present.
    bool insert(std::uint64_t key) {
        if ((count_ + 1) * 2 > slots_.size()) {
            rebuild(slots_.empty() ? 1024 : slots_.size() * 2);
        }
        return insert_no_grow(key);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

private:
    struct Slot {
        std::uint64_t key{0};
        std::uint32_t generation{0};
    };

    std::size_t slot_of(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    bool insert_no_grow(std::uint64_t key) noexcept {
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = slot_of(key);; i = (i + 1) & mask) {
            Slot& slot = slots_[i];
            if (slot.generation != generation_) {
                slot.key = key;
                slot.generation = generation_;
                ++count_;
                return true;
            }
            if (slot.key == key) {
                return false;
            }
        }
    }

    void rebuild(std::size_t capacity) {
        std::vector<Slot> old(capacity);
        old.swap(slots_);
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c >>= 1) {
            --shift_;
        }
        count_ = 0;
        for (const Slot& slot : old) {
            if (slot.generation == generation_ && generation_ != 0) {
                insert_no_grow(slot.key);
            }
        }
    }

    std::vector<Slot> slots_;
    std::uint32_t generation_{0};
    std::uint32_t shift_{64};
    std::size_t count_{0};
};

// Set over a dense [0, capacity) key range: one stamp per key, no hashing and
// O(1) clear. Used for hot-tier local IDs.
class DenseStampSet {
public:
    // Must be called when the key range changes; also clears the set.
    void resize(std::size_t capacity) {
        if (stamps_.size() != capacity) {
            stamps_.assign(capacity, 0);
            generation_ = 0;
        }
        clear();
    }

    void clear() {
        if (++generation_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0);
            generation_ = 1;
        }
    }

    // Returns true if `key` was not present. `key` must be < capacity.
    bool insert(std::uint64_t key) noexcept {
        std::uint32_t& stamp = stamps_[key];
        if (stamp == generation_) {
            return false;
        }
        stamp = generation_;
        return true;
    }

private:
    std::vector<std::uint32_t> stamps_;
    std::uint32_t generation_{0};
};

}  // namespace low_latency_graphdb
