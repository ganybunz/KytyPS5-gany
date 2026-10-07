#pragma once

#include "graphics/host_gpu/regionDefinitions.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace NativeRegionEpochs {

// One validation call only. A bounded cache avoids repeatedly loading the same
// tracker-region epoch for different buffer dependencies. Recheck every sampled
// region before returning a proof; a concurrent change or deferred protection
// retains the caller's exact dirty-range fallback. Nothing persists across draws.
template <typename Ranges, typename Query>
[[nodiscard]] uint64_t Aggregate(const Ranges& ranges, Query&& query) {
	using namespace Libs::Graphics;
	struct Entry { uint64_t address, epoch; };
	std::array<Entry, 32> cache; // only initialized entries below `used` are read
	size_t used = 0;
	uint64_t total = 0;
	for (const auto& range: ranges) {
		if (!GuestRange {range.address, range.size}.Valid()) return 0;
		const auto first = range.address / TRACKER_REGION_SIZE;
		const auto last = (range.address + range.size - 1) / TRACKER_REGION_SIZE;
		if (last - first >= 64) return 0; // same per-range limit as BufferCache
		uint64_t subtotal = 0;
		for (auto region = first; region <= last; ++region) {
			// GuestRange rejects zero, but valid ranges can occupy tracker region 0.
			const auto address = region == 0 ? uint64_t {1} : region * TRACKER_REGION_SIZE;
			size_t index = 0;
			while (index < used && cache[index].address != address) ++index;
			if (index == used) {
				if (used == cache.size()) return 0; // conservative bounded overflow
				const auto epoch = query(address, uint64_t {1});
				if (epoch == 0) return 0;
				cache[used++] = {address, epoch};
			}
			subtotal += cache[index].epoch;
		}
		// Preserve the old per-dependency zero sentinel, including unsigned wrap.
		if (subtotal == 0) return 0;
		total += subtotal;
	}
	for (size_t index = 0; index < used; ++index) {
		if (query(cache[index].address, uint64_t {1}) != cache[index].epoch) return 0;
	}
	return total;
}

} // namespace NativeRegionEpochs
