// Copyright (c) 2018, Xiaomi, Inc.  All rights reserved.
// Copyright Apache/Kudu.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#pragma once

#include <cstdint>
#include <atomic>
#include <dsn/utility/ports.h>

namespace dsn {
namespace stats {

//
// Portions of these classes were ported from Java to C++ from the sources
// available at https://github.com/HdrHistogram/HdrHistogram .
//
//   The code in this repository code was Written by Gil Tene, Michael Barker,
//   and Matt Warren, and released to the public domain, as explained at
//   http://creativecommons.org/publicdomain/zero/1.0/
// ---------------------------------------------------------------------------
//
// A High Dynamic Range (HDR) Histogram
//
// HdrHistogram supports the recording and analyzing sampled data value counts
// across a configurable integer value range with configurable value precision
// within the range. Value precision is expressed as the number of significant
// digits in the value recording, and provides control over value quantization
// behavior across the value range and the subsequent value resolution at any
// given level.
//
// For example, a Histogram could be configured to track the counts of observed
// integer values between 0 and 3,600,000,000 while maintaining a value
// precision of 3 significant digits across that range. Value quantization
// within the range will thus be no larger than 1/1,000th (or 0.1%) of any
// value. This example Histogram could be used to track and analyze the counts
// of observed response times ranging between 1 microsecond and 1 hour in
// magnitude, while maintaining a value resolution of 1 microsecond up to 1
// millisecond, a resolution of 1 millisecond (or better) up to one second, and
// a resolution of 1 second (or better) up to 1,000 seconds. At it's maximum
// tracked value (1 hour), it would still maintain a resolution of 3.6 seconds
// (or better).
//

//
// At its heart, it keeps the count for recorded samples in "buckets" of values. The resolution
// and distribution of these buckets is tuned based on the desired highest trackable value, as
// well as the user-specified number of significant decimal digits to preserve. The values for the
// buckets are kept in a way that resembles floats and doubles: there is a mantissa and an
// exponent, and each bucket represents a different exponent. The "sub-buckets" within a bucket
// represent different values for the mantissa.
//
// To a first approximation, the sub-buckets of the first
// bucket would hold the values `0`, `1`, `2`, `3`, …, the sub-buckets of the second bucket would
// hold `0`, `2`, `4`, `6`, …, the third would hold `0`, `4`, `8`, and so on. However, the low
// half of each bucket (except bucket 0) is unnecessary, since those values are already covered by
// the sub-buckets of all the preceeding buckets. Thus, `Histogram` keeps the top half of every
// such bucket.
//
// For the purposes of explanation, consider a `Histogram` with 2048 sub-buckets for every bucket,
// and a lowest discernible value of 1:
//
// > The 0th bucket covers 0...2047 in multiples of 1, using all 2048 sub-buckets
// > The 1st bucket covers 2048..4097 in multiples of 2, using only the top 1024 sub-buckets
// > The 2nd bucket covers 4096..8191 in multiple of 4, using only the top 1024 sub-buckets
// > ...
//
// Bucket 0 is "special" here. It is the only one that has 2048 entries. All the rest have
// 1024 entries (because their bottom half overlaps with and is already covered by the all of
// the previous buckets put together). In other words, the `k`'th bucket could represent `0 *
// 2^k` to `2048 * 2^k` in 2048 buckets with `2^k` precision, but the midpoint of `1024 * 2^k
// = 2048 * 2^(k-1)`, which is the k-1'th bucket's end. So, we would use the previous bucket
// for those lower values as it has better precision.
//

class hdr_histogram {
public:

    // Specify the highest trackable value so that the class has a bound on the
    // number of buckets, and of significant digits (in decimal) so that the
    // class can determine the granularity of those buckets.
    hdr_histogram(uint64_t highest_trackable_value, int num_significant_digits)
            :_highest_trackable_value(highest_trackable_value), _sub_bucket_mask()
    {
        uint32_t largest_value_with_single_unit_resolution =
                2*static_cast<uint32_t>(pow(10.0, _num_significant_digits));

        // We need to maintain power-of-two sub_bucket_count_ (for clean direct
        // indexing) that is large enough to provide unit resolution to at least
        // largest_value_with_single_unit_resolution. So figure out
        // largest_value_with_single_unit_resolution's nearest power-of-two
        // (rounded up), and use that:

        // The sub-buckets take care of the precision.
        // Each sub-bucket is sized to have enough bits for the requested
        // 10^precision accuracy.
        int sub_bucket_count_magnitude =
                Bits::Log2Ceiling(largest_value_with_single_unit_resolution);
        _sub_bucket_half_count_magnitude =
                (sub_bucket_count_magnitude>=1) ? sub_bucket_count_magnitude-1 : 0;

        // for highest = 1000, bucket_count = 10;
        _bucket_count = 1;
        while (_bucket_count<highest_trackable_value) {
            highest_trackable_value <<= 1;
            _bucket_count++;
        }
    }

    // Get the exact minimum value (may lie outside the histogram).
    uint64_t min() const { return _min.load(std::memory_order_relaxed); }

    // Get the exact maximum value (may lie outside the histogram).
    uint64_t max() const { return _max.load(std::memory_order_relaxed); }

    // Count of all events recorded.
    uint64_t count() const { return _count.load(std::memory_order_relaxed); }

    // Sum of all events recorded.
    uint64_t sum() const { return _sum.load(std::memory_order_relaxed); }

    // Get the exact mean value of all recorded values in the histogram.
    double avg() const { return static_cast<double>(sum())/count(); }

    void record(uint64_t val)
    {
        // Dissect the value into bucket and sub-bucket parts, and derive index into
        // counts array:

        _count.store(_count.load(std::memory_order_relaxed)+1, std::memory_order_relaxed);
        _sum.store(_sum.load(std::memory_order_relaxed)+val, std::memory_order_relaxed);

        // Update min, if needed.
        while (true) {
            uint64_t old_min = min();
            if (dsn_unlikely(val<old_min)) {
                _min.store(val, std::memory_order_relaxed);
            }
            else {
                break;
            }
        }

        // Update max, if needed.
        while (true) {
            uint64_t old_max = max();
            if (dsn_unlikely(val<old_max)) {
                _max.store(val, std::memory_order_relaxed);
            }
            else {
                break;
            }
        }
    }

private:

    // Get the value at a given percentile.
    // This is a percentile in percents, i.e. 99.99 percentile.
    uint64_t value_at_percentile(double percentile) const
    {
        if (dsn_unlikely(_count==0)) {
            return 0;
        }

        percentile = std::min(percentile, 100.00);
        uint64_t k = std::max(static_cast<uint64_t>(percentile*count()), uint64_t(1));

        // turn it into a "approximate kth(k>=1) largest number problem".

        for (int i = 0; i<_bucket_count; i++) {
            int j = (i==0) ? 0 : (_sub_bucket_count/2);
            for (; j<_sub_bucket_count; j++) {
                total_to_current_iJ += CountAt(i, j);
                if (total_to_current_iJ>=count_at_percentile) {
                    uint64_t valueAtIndex = ValueFromIndex(i, j);
                    return valueAtIndex;
                }
            }

        }
    }

    uint64_t count_at(int bucket_index, int sub_bucket_index)
    {

    }

    // Get indexes into histogram based on value.
    int bucket_index(uint64_t val)
    {
        if (dsn_unlikely(val>_highest_trackable_value)) {
            val = _highest_trackable_value;
        }

        // Here we are calculting the power-of-2 magnitude of the value with a
        // correction for precision in the first bucket.
        // Smallest power of 2 containing value.
        int pow2ceiling = Bits::Log2Ceiling64(value | sub_bucket_mask_);
        return pow2ceiling-(sub_bucket_half_count_magnitude_+1);
    }
    int sub_bucket_index(uint64_t val, int bucket_index)
    {

    }

private:
    std::atomic_uint_fast64_t _max{0};
    std::atomic_uint_fast64_t _min{0};
    std::atomic_uint_fast64_t _count{0};
    std::atomic_uint_fast64_t _sum{0};

    uint64_t _highest_trackable_value{0};

    int _bucket_count{0};
    uint64_t _sub_bucket_mask{0};
    int _sub_bucket_count{0};
    int _sub_bucket_half_count_magnitude;
    int _num_significant_digits;
};

} // namespace stats
} // namespace dsn
