// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Portions of these classes were ported from Java to C++ from the sources
// available at https://github.com/HdrHistogram/HdrHistogram .
//
//   The code in this repository code was Written by Gil Tene, Michael Barker,
//   and Matt Warren, and released to the public domain, as explained at
//   http://creativecommons.org/publicdomain/zero/1.0/

#pragma once

#include <cstdint>
#include <atomic>
#include <dsn/c/api_utilities.h>
#include <dsn/utility/ports.h>

namespace dsn {
namespace stats {

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

// At its heart the algorithm reduces the precision of each recorded value to
// achieve lower memory usage than method that maintains all values in a
// sorted array.
//
// For example, given an array sorted of 100000 values, 200 is the 98.01th percentile,
// 201 is the P98.05, 211 is P99, then we don't need to maintain either
// 200 or 201 when only P99 is required.
// In HDR histogram, it removes the lower 1 bit of each value to reduce
// the bucket count. 200 and 201 are regarded as the same, 210 and 211 likewise.
// Larger value, more bits removed.

//
// HDR histogram uses two level of buckets to store data value counts:
//    counts = new uint64[bucket_count][sub_bucket_count]
//
// For example, to track value ranging from 1 to 10^8, number of significant digits is 3,
// the algorithm performs as below:
//
//  sub_bucket_count = number of bits that holds the 3 significant digits (from 0 to 999)
//                     since 1111111111 = 1023 > 999, it requires 10 bits.
//                   = 10
//
//      bucket_count = number of heading bits
//                     since 1111111111111111 1111111111   = 2^28-1 = 134217727 > 10^8
//                                bucket      sub-bucket
//                     it requires 16 bits
//                   = 16
//
// To record a value = 10000 (in bits: 10011100010000) into this histogram,
//
//         bucket_id = the highest bit in bucket part,
//                     since 1111111111111111 1111111111
//                                       1001 1100010000
//                                       |
//                                       4th in bucket part
//                   = 4
//
// values within [0 (0 00000 00000), 2047 (1 11111 11111)] all belong to bucket 1,
//               [2048 (10 00000 00000), 4095 (11 11111 11111)] to bucket 2
//               [4096 (100 00000 00000), 8191 (111 11111 11111)] to bucket 3
//               [8192 (1000 00000 00000), 16383 (1111 11111 11111)] to bucket 4
//               ....
//
//     sub_bucket_id = (10000 >> bucket_id)
//  value_from_index = sub_bucket_id << bucket_id
//

class hdr_histogram
{
public:
    /// \param highest_trackable_value:
    /// The highest value to be tracked by the histogram.
    /// For example, for measurement of latency range in [1us, 100s],
    /// highest_trackable_value = 10^8
    ///
    /// \param num_significant_digits:
    /// The number of significant decimal digits to which the
    /// histogram will maintain value resolution and separation.
    /// For example, if you want P99, P999, and P9999 PUT latencies in 10s,
    /// num_significant_digits = 4
    ///
    hdr_histogram(uint64_t highest_trackable_value, int num_significant_digits)
        : _highest_trackable_value(highest_trackable_value),
          _num_significant_digits(num_significant_digits)
    {
        dassert(highest_trackable_value >= 2, "highest_trackable_value must be >= 2");
        dassert(num_significant_digits >= 1 && num_significant_digits <= 5,
                "num_significant_digits must be between 1 and 5");

        uint32_t largest_value_with_single_unit_resolution =
            2 * static_cast<uint32_t>(pow(10.0, _num_significant_digits));
        int sub_bucket_count_magnitude =
            static_cast<int>(ceil(log2(largest_value_with_single_unit_resolution)));
        _sub_bucket_half_count_magnitude = sub_bucket_count_magnitude - 1;
        _sub_bucket_count = 1u << sub_bucket_count_magnitude;
        _sub_bucket_mask = _sub_bucket_count - 1;
        _sub_bucket_half_count = _sub_bucket_count / 2;

        _bucket_count = 1;
        while (_bucket_count < highest_trackable_value) {
            highest_trackable_value <<= 1;
            _bucket_count++;
        }

        _counts_array_length = (_bucket_count + 1) * _sub_bucket_half_count;
        _counts.reset(new std::atomic_uint_fast64_t[_counts_array_length]{0});
    }

    /// Get the exact minimum value (may lie outside the histogram).
    uint64_t min() const { return _min.load(std::memory_order_relaxed); }

    /// Get the exact maximum value (may lie outside the histogram).
    uint64_t max() const { return _max.load(std::memory_order_relaxed); }

    /// Count of all events recorded.
    uint64_t total_count() const { return _total.load(std::memory_order_relaxed); }

    /// Sum of all events recorded.
    uint64_t sum() const { return _sum.load(std::memory_order_relaxed); }

    /// Get the exact mean value of all recorded values in the histogram.
    double avg() const { return static_cast<double>(sum()) / total_count(); }

    void record(uint64_t val)
    {
        int bucket_idx = bucket_index(val);
        int sub_bucket_idx = sub_bucket_index(val, bucket_idx);
        _counts[counts_array_index(bucket_idx, sub_bucket_idx)].fetch_add(1);

        _total.fetch_add(1, std::memory_order_relaxed);
        _sum.fetch_add(val, std::memory_order_relaxed);

        // Update min, if needed.
        while (true) {
            uint64_t old_min = min();
            if (dsn_unlikely(val < old_min)) {
                _min.store(val, std::memory_order_relaxed);
            } else {
                break;
            }
        }

        // Update max, if needed.
        while (true) {
            uint64_t old_max = max();
            if (dsn_unlikely(val < old_max)) {
                _max.store(val, std::memory_order_relaxed);
            } else {
                break;
            }
        }
    }

    /// Get the value at a given percentile.
    /// This is a percentile in percents, i.e. 99.99 percentile.
    uint64_t value_at_percentile(double percentile) const
    {
        if (dsn_unlikely(_total == 0)) {
            return 0;
        }

        percentile = std::min(percentile, 100.00);
        auto k = std::max(static_cast<uint64_t>(percentile * total_count()), uint64_t(1));

        // turn it into a "approximate kth(k>=1) largest number problem".

        uint64_t total_to_current = 0;
        for (int i = 0; i < _bucket_count; i++) {
            int j = (i == 0) ? 0 : (_sub_bucket_count / 2);
            for (; j < _sub_bucket_count; j++) {
                total_to_current += count_at(i, j);
                if (total_to_current >= k) {
                    return value_from_index(i, j);
                }
            }
        }

        return 0;
    }

private:
    int bucket_index(uint64_t val)
    {
        if (dsn_unlikely(val > _highest_trackable_value)) {
            val = _highest_trackable_value;
        }

        int pow2ceiling = static_cast<int>(ceil(log2(static_cast<double>(val | _sub_bucket_mask))));
        return pow2ceiling - (_sub_bucket_half_count_magnitude + 1);
    }

    int sub_bucket_index(uint64_t val, int bucket_index)
    {
        if (dsn_unlikely(val > _highest_trackable_value)) {
            val = _highest_trackable_value;
        }

        return static_cast<int>(val >> bucket_index);
    }

    int counts_array_index(int bucket_index, int sub_bucket_index) const
    {
        assert(bucket_index < _bucket_count);
        assert(sub_bucket_index < _sub_bucket_count);

        int bucket_base_index = (bucket_index + 1) << _sub_bucket_half_count_magnitude;
        int offset_in_bucket = sub_bucket_index - _sub_bucket_half_count;

        return bucket_base_index + offset_in_bucket;
    }

    uint64_t count_at(int bucket_index, int sub_bucket_index) const
    {
        return _counts[counts_array_index(bucket_index, sub_bucket_index)].load(
            std::memory_order_relaxed);
    }

    uint64_t value_from_index(int bucket_index, int sub_bucket_index) const
    {
        return static_cast<uint64_t>(sub_bucket_index) << bucket_index;
    }

private:
    std::atomic_uint_fast64_t _max{0};
    std::atomic_uint_fast64_t _min{0};
    std::atomic_uint_fast64_t _total{0};
    std::atomic_uint_fast64_t _sum{0};

    uint64_t _highest_trackable_value{0};
    int _num_significant_digits{0};

    int _bucket_count{0};
    uint32_t _sub_bucket_mask{0};
    uint32_t _sub_bucket_count{0};
    uint32_t _sub_bucket_half_count{0};
    int _sub_bucket_half_count_magnitude{0};

    std::unique_ptr<std::atomic_uint_fast64_t[]> _counts;
    int _counts_array_length{0};
};

} // namespace stats
} // namespace dsn
