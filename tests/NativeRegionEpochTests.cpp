#include "local/native-region-epochs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

using Libs::Graphics::GuestRange;
using Libs::Graphics::TRACKER_ADDRESS_SIZE;
using Libs::Graphics::TRACKER_REGION_SIZE;
using Ranges = std::vector<GuestRange>;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "NativeRegionEpochTests: failed: %s\n", message);
		std::abort();
	}
}

uint64_t RegionAddress(uint64_t address) {
	const auto region = address / TRACKER_REGION_SIZE;
	return region == 0 ? uint64_t {1} : region * TRACKER_REGION_SIZE;
}

// This deliberately models the old per-dependency sum: every range contributes
// each region epoch independently, including repeated regions and dependencies.
uint64_t OldDependencySum(const Ranges& ranges,
						  const std::map<uint64_t, uint64_t>& epochs) {
	uint64_t total = 0;
	for (const auto& range: ranges) {
		if (!range.Valid()) return 0;
		const auto first = range.address / TRACKER_REGION_SIZE;
		const auto last = (range.address + range.size - 1) / TRACKER_REGION_SIZE;
		if (last - first >= 64) return 0;
		uint64_t subtotal = 0;
		for (auto region = first; region <= last; ++region) {
			const auto address = region == 0 ? uint64_t {1} : region * TRACKER_REGION_SIZE;
			const auto found = epochs.find(address);
			if (found == epochs.end() || found->second == 0) return 0;
			subtotal += found->second;
		}
		if (subtotal == 0) return 0;
		total += subtotal;
	}
	return total;
}

struct QueryLog {
	std::map<uint64_t, uint64_t> epochs;
	std::map<uint64_t, size_t> calls;

	uint64_t operator()(uint64_t address, uint64_t size) {
		Check(size == 1, "query did not request a one-byte sample");
		++calls[address];
		const auto found = epochs.find(address);
		return found == epochs.end() ? 0 : found->second;
	}
};

void TestAggregateMatchesDependencySum() {
	const auto region = TRACKER_REGION_SIZE;
	const Ranges ranges {
		{1, 8},
		{region - 2, 5},
		{region + 3, 1},
		{3 * region + 10, 2 * region},
		{region + 3, 1},
	};
	QueryLog query {{
		{1, 5},
		{region, 7},
		{3 * region, 13},
		{4 * region, 17},
		{5 * region, 19},
	}, {}};
	const auto expected = OldDependencySum(ranges, query.epochs);
	Check(expected != 0, "stable fixture unexpectedly has a zero old sum");
	Check(NativeRegionEpochs::Aggregate(ranges, query) == expected,
		  "aggregate differed for duplicate and multiregion dependencies");
	for (const auto& [address, epoch] : query.epochs) {
		(void)epoch;
		Check(query.calls[address] == 2,
			  "stable aggregate did not query each unique region twice");
	}
}

void TestInvalidRangesReturnZero() {
	const auto limit = TRACKER_ADDRESS_SIZE;
	const std::array<Ranges, 6> invalid {{
		Ranges {{0, 1}},
		Ranges {{1, 0}},
		Ranges {{limit, 1}},
		Ranges {{limit - 1, 2}},
		Ranges {{UINT64_MAX - 3, 8}},
		Ranges {{0, 0}},
	}};
	for (const auto& ranges: invalid) {
		QueryLog query;
		Check(NativeRegionEpochs::Aggregate(ranges, query) == 0,
			  "invalid guest range did not return the sentinel");
		Check(query.calls.empty(), "invalid guest range was queried");
	}

	QueryLog end_query;
	const Ranges reaches_address_end {{limit - 1, 1}};
	end_query.epochs[RegionAddress(limit - 1)] = 17;
	Check(NativeRegionEpochs::Aggregate(reaches_address_end, end_query) == 17,
		  "range ending exactly at the guest address limit was rejected");
}

void TestUnknownAndDeferredQueriesReturnZero() {
	const Ranges ranges {{TRACKER_REGION_SIZE + 10, 4}};
	QueryLog unknown;
	Check(NativeRegionEpochs::Aggregate(ranges, unknown) == 0,
		  "unknown region epoch did not return zero");
	Check(unknown.calls[TRACKER_REGION_SIZE] == 1,
		  "unknown epoch was not queried exactly once before fallback");

	QueryLog deferred {{{TRACKER_REGION_SIZE, 0}}, {}};
	Check(NativeRegionEpochs::Aggregate(ranges, deferred) == 0,
		  "deferred-protection zero epoch did not return zero");
	Check(deferred.calls[TRACKER_REGION_SIZE] == 1,
		  "deferred-protection query did not stop after the zero epoch");
}

void TestPerRangeAndCacheLimits() {
	const auto region = TRACKER_REGION_SIZE;
	const Ranges too_wide {{region, 65 * region}};
	QueryLog width_query;
	Check(NativeRegionEpochs::Aggregate(too_wide, width_query) == 0,
		  "range covering more than 64 regions was accepted");
	Check(width_query.calls.empty(), "overwide range queried epochs");

	Ranges too_many_unique;
	QueryLog cache_query;
	for (uint64_t index = 1; index <= 33; ++index) {
		too_many_unique.push_back({index * region + 4, 1});
		cache_query.epochs[index * region] = index + 10;
	}
	Check(NativeRegionEpochs::Aggregate(too_many_unique, cache_query) == 0,
		  "more than 32 unique regions did not conservatively return zero");
	Check(cache_query.calls.size() == 32,
		  "cache overflow performed an unexpected query or final recheck");
	for (const auto& [address, count] : cache_query.calls) {
		(void)address;
		Check(count == 1, "cache overflow unexpectedly performed final rechecks");
	}
}

void TestRegionZeroAndDuplicateCallCount() {
	const Ranges region_zero {{1, 5}};
	QueryLog query {{{1, 23}}, {}};
	Check(NativeRegionEpochs::Aggregate(region_zero, query) == 23,
		  "region zero was not accepted");
	Check(query.calls[1] == 2, "region zero did not use address 1 for both samples");

	Ranges duplicated_dependencies(100, GuestRange {2 * TRACKER_REGION_SIZE + 17, 9});
	QueryLog repeated {{{2 * TRACKER_REGION_SIZE, 31}}, {}};
	Check(NativeRegionEpochs::Aggregate(duplicated_dependencies, repeated) == 3100,
		  "100 duplicated dependencies produced the wrong sum");
	Check(repeated.calls[2 * TRACKER_REGION_SIZE] == 2,
		  "100 duplicated dependencies did not share the single-region cache");
}

void TestWraparoundSemantics() {
	const auto region = TRACKER_REGION_SIZE;
	const Ranges per_dependency_wrap {{region + 1, region + 1}};
	QueryLog zero_subtotal {{{region, 9}, {2 * region, UINT64_MAX - 8}}, {}};
	Check(OldDependencySum(per_dependency_wrap, zero_subtotal.epochs) == 0,
		  "per-dependency wrap fixture was incorrect");
	Check(NativeRegionEpochs::Aggregate(per_dependency_wrap, zero_subtotal) == 0,
		  "wrapped per-dependency zero sentinel changed");

	const Ranges total_wrap {{region + 1, 1}, {2 * region + 1, 1}};
	QueryLog wrapped_total {{{region, UINT64_MAX}, {2 * region, 1}}, {}};
	Check(OldDependencySum(total_wrap, wrapped_total.epochs) == 0,
		  "total-wrap fixture was incorrect");
	Check(NativeRegionEpochs::Aggregate(total_wrap, wrapped_total) == 0,
		  "wrapped total did not preserve the zero result");
	Check(wrapped_total.calls[region] == 2 && wrapped_total.calls[2 * region] == 2,
		  "total wrap skipped a final validation query");
}

void TestConcurrentEpochChangeDuringFinalRecheck() {
	const Ranges ranges {{TRACKER_REGION_SIZE + 2, 4}};
	QueryLog query {{{TRACKER_REGION_SIZE, 41}}, {}};
	auto changing_query = [&query](uint64_t address, uint64_t size) {
		Check(size == 1, "recheck did not request a one-byte sample");
		const auto call = ++query.calls[address];
		if (address == TRACKER_REGION_SIZE && call == 2) {
			query.epochs[address] = 42;
		}
		return query.epochs[address];
	};
	Check(NativeRegionEpochs::Aggregate(ranges, changing_query) == 0,
		  "epoch change during final validation did not invalidate the aggregate");
	Check(query.calls[TRACKER_REGION_SIZE] == 2,
		  "epoch change was not observed during the final recheck");
	size_t deferred_calls = 0;
	Check(NativeRegionEpochs::Aggregate(ranges, [&](uint64_t, uint64_t) {
		return ++deferred_calls == 1 ? uint64_t {41} : uint64_t {0};
	}) == 0, "deferred protection beginning during recheck did not reject the proof");
}

void TestNoStateAcrossCalls() {
	const Ranges ranges {{4 * TRACKER_REGION_SIZE + 3, 2}};
	uint64_t epoch = 101;
	size_t calls = 0;
	const auto query = [&epoch, &calls](uint64_t address, uint64_t size) {
		Check(address == 4 * TRACKER_REGION_SIZE && size == 1,
			  "successive-call query used the wrong sample");
		++calls;
		return epoch;
	};
	Check(NativeRegionEpochs::Aggregate(ranges, query) == 101,
		  "first call returned the wrong epoch");
	Check(calls == 2, "first call did not sample and recheck");
	epoch = 202;
	Check(NativeRegionEpochs::Aggregate(ranges, query) == 202,
		  "second call reused state from the first call");
	Check(calls == 4, "second call did not independently sample and recheck");
}

void TestRandomizedStableDifferential() {
	uint64_t random = 0x4d59'5df4'd0f3'3173ull;
	const auto next_random = [&random] {
		random ^= random << 13;
		random ^= random >> 7;
		random ^= random << 17;
		return random;
	};
	const auto region = TRACKER_REGION_SIZE;
	for (size_t test = 0; test < 600; ++test) {
		QueryLog query;
		for (uint64_t index = 0; index < 32; ++index) {
			const auto selector = next_random();
			const auto epoch = selector % 17 == 0 ? UINT64_MAX - selector : selector | 1;
			query.epochs[RegionAddress(index * region + 1)] = epoch;
		}

		Ranges ranges;
		const auto count = 1 + next_random() % 6;
		for (uint64_t index = 0; index < count; ++index) {
			const auto first_region = next_random() % 29;
			const auto offset = 1 + next_random() % (region - 1);
			const auto size = 1 + next_random() % (2 * region);
			const auto address = first_region * region + offset;
			if (address + size > TRACKER_ADDRESS_SIZE) {
				ranges.push_back({address, TRACKER_ADDRESS_SIZE - address});
			} else {
				ranges.push_back({address, size});
			}
		}
		const auto expected = OldDependencySum(ranges, query.epochs);
		Check(NativeRegionEpochs::Aggregate(ranges, query) == expected,
			  "deterministic stable randomized result differed from old logic");
	}
}

} // namespace

int main() {
	TestAggregateMatchesDependencySum();
	TestInvalidRangesReturnZero();
	TestUnknownAndDeferredQueriesReturnZero();
	TestPerRangeAndCacheLimits();
	TestRegionZeroAndDuplicateCallCount();
	TestWraparoundSemantics();
	TestConcurrentEpochChangeDuringFinalRecheck();
	TestNoStateAcrossCalls();
	TestRandomizedStableDifferential();
	std::puts("NativeRegionEpochTests: all cases passed");
	return 0;
}
