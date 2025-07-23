/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "interval.h"

#include <algorithm>
#include <vector>

namespace android::apex {

static bool IsNormalized(const std::vector<Interval>& intervals) {
  for (size_t i = 0; i < intervals.size(); ++i) {
    const Interval& current = intervals[i];
    // Should not be zero-length
    if (current.length == 0) {
      return false;
    }
    // Should not overlap with the previous interval
    if (i > 0) {
      const Interval& previous = intervals[i - 1];
      if (current.offset <= previous.end()) {
        return false;
      }
    }
  }
  return true;
}

uint64_t IntervalsGetLength(const std::vector<Interval>& intervals) {
  uint64_t size = 0;
  for (const auto& interval : intervals) {
    size += interval.length;
  }
  return size;
}

// Normalizes the interval vector by sorting it and merging overlapping or
// adjacent intervals.
// Example:
// - Input: {{10, 3}, {20, 5}, {13, 2}, {0, 5}}
// - Sorted: {{0, 5}, {10, 3}, {13, 2}, {20, 5}}
// - Merged: {{0, 5}, {10, 5}, {20, 5}}
std::vector<Interval> NormalizeIntervals(std::vector<Interval> intervals) {
  // Remove zero-length intervals first
  intervals.erase(
      std::remove_if(intervals.begin(), intervals.end(),
                     [](const Interval& i) { return i.length == 0; }),
      intervals.end());
  if (intervals.empty()) {
    return {};
  }

  std::sort(intervals.begin(), intervals.end());
  size_t count = 1;
  for (size_t i = 0; i < intervals.size(); ++i) {
    Interval& last = intervals[count - 1];
    const Interval& current = intervals[i];

    if (current.offset <= last.end()) {
      // The current interval can be merged into the last one in the list
      auto new_end = std::max(last.end(), current.end());
      last.length = new_end - last.offset;
    } else {
      // Since there's a gap between the last and the current, the current one
      // can't be merged. Adding the current interval to the list.
      intervals[count++] = current;
    }
  }
  intervals.erase(intervals.begin() + count, intervals.end());
  return intervals;
}

// Subtracts list_b from list_a. Given that both lists are normalized (sorted
// and non-overlapping), use two-pointer algorithm, similar to merging sorted
// lists.
std::vector<Interval> SubtractIntervals(const std::vector<Interval>& list_a,
                                        const std::vector<Interval>& list_b) {
  if (!IsNormalized(list_a) || !IsNormalized(list_b)) {
    return SubtractIntervals(NormalizeIntervals(list_a),
                             NormalizeIntervals(list_b));
  }

  std::vector<Interval> result;
  size_t i = 0;  // Pointer for list_a
  size_t j = 0;  // Pointer for list_b

  while (i < list_a.size()) {
    Interval current_a = list_a[i];

    // Loop to process current_a until it's fully consumed or no more overlaps
    // with B
    while (current_a.length > 0 && j < list_b.size()) {
      Interval current_b = list_b[j];

      // Case 1: current_b is completely before current_a, move to next B
      if (current_b.end() <= current_a.offset) {
        j++;
        continue;  // Check current_a against the next B interval
      }

      // Case 2: current_a is completely before current_b, add current_a and
      // move to next A
      if (current_a.end() <= current_b.offset) {
        break;  // Exit inner loop, move to next A in outer loop
      }

      // Case 3: Overlap exists
      auto [before, overlap] =
          current_a.SplitAtOffset(std::max(current_a.offset, current_b.offset));

      // Add the part of current_a before current_b (if any)
      if (before.length > 0) {
        result.push_back(before);
      }

      auto [_, after] =
          overlap.SplitAtOffset(std::min(overlap.end(), current_b.end()));
      current_a = after;

      if (current_a.length > 0) {
        // current_a still has a remainder, but current_b has been "used up"
        // against current_a. So we need to consider the next current_b for the
        // remaining current_a.
        j++;
      } else {
        // current_a was fully processed by current_b (or current_b was past
        // current_a originally)
        // no need to advance j because current_a was fully consumed.
        // j will be correctly positioned for the next current_a.
      }
    }
    // After the inner loop, if current_a still has a positive length,
    // it means all of list_b has been exhausted and current_a was not fully
    // consumed. Add the remaining part of current_a to the result.
    if (current_a.length > 0) {
      result.push_back(current_a);
    }
    i++;  // Move to the next interval in list_a
  }
  return result;
}

std::pair<std::vector<Interval>, std::vector<Interval>> TakeLengthFromStart(
    const std::vector<Interval>& intervals, uint64_t total_length) {
  // Partition the list where the cumulative_length <= total_length
  auto it = intervals.begin();
  uint64_t cumulative_length = 0;
  for (; it != intervals.end(); ++it) {
    if (cumulative_length + it->length > total_length) {
      break;
    }
    cumulative_length += it->length;
  }
  std::vector<Interval> first_part{intervals.begin(), it};
  std::vector<Interval> second_part{it, intervals.end()};

  if (cumulative_length < total_length) {
    // The input doesn't have enough length.
    if (second_part.empty()) {
      return std::make_pair(std::vector<Interval>(), intervals);
    }

    auto [before, after] =
        second_part[0].SplitAtLength(total_length - cumulative_length);
    first_part.push_back(before);
    second_part[0] = after;
  }
  return std::make_pair(first_part, second_part);
}

std::vector<Interval> ApplyOffsetLength(const std::vector<Interval>& intervals,
                                        uint32_t offset, size_t length) {
  auto [_, intervals_with_offset] = TakeLengthFromStart(intervals, offset);
  auto [intervals_truncated_by_length, __] =
      TakeLengthFromStart(intervals_with_offset, length);
  return intervals_truncated_by_length;
}

}  // namespace android::apex
