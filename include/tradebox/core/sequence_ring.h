#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tradebox::core {

// Preallocated, sequence-addressed circular storage. Entries never move after
// construction, and overwrite is explicit to cursor consumers through Oldest().
template <class T>
class SequenceRing {
public:
    struct Entry {
        std::uint64_t sequence = 0;
        T value;
    };

    explicit SequenceRing(std::size_t capacity = 1)
        : slots_((std::max<std::size_t>)(capacity, 1)) {}

    void Push(std::uint64_t sequence, T value) {
        slots_[write_] = Entry{sequence, std::move(value)};
        write_ = (write_ + 1) % slots_.size();
        count_ = (std::min)(count_ + 1, slots_.size());
    }

    [[nodiscard]] std::uint64_t Oldest() const {
        if (count_ == 0) return 0;
        return At(0).sequence;
    }

    [[nodiscard]] std::uint64_t Newest() const {
        if (count_ == 0) return 0;
        return At(count_ - 1).sequence;
    }

    [[nodiscard]] std::size_t Size() const { return count_; }
    [[nodiscard]] std::size_t Capacity() const {
        return slots_.size();
    }
    [[nodiscard]] bool Empty() const { return count_ == 0; }

    template <class Visitor>
    void VisitAfter(std::uint64_t after_sequence,
                    std::size_t maximum, Visitor&& visitor) const {
        std::size_t visited = 0;
        for (std::size_t index = 0;
             index < count_ && visited < maximum; ++index) {
            const Entry& entry = At(index);
            if (entry.sequence <= after_sequence) continue;
            visitor(entry);
            ++visited;
        }
    }

private:
    [[nodiscard]] const Entry& At(std::size_t logical_index) const {
        const std::size_t oldest =
            (write_ + slots_.size() - count_) % slots_.size();
        return *slots_[(oldest + logical_index) % slots_.size()];
    }

    std::vector<std::optional<Entry>> slots_;
    std::size_t write_ = 0;
    std::size_t count_ = 0;
};

}  // namespace tradebox::core
