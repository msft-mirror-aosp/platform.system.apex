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

#pragma once

#include <cstdint>
#include <vector>

namespace android::apex {

// Represents an interval with a starting offset and a length.
struct Interval {
  uint64_t offset;
  uint64_t length;

  Interval(uint64_t o = 0, uint64_t l = 0) : offset(o), length(l) {}

  uint64_t end() const { return offset + length; }

  std::pair<Interval, Interval> SplitAtOffset(uint64_t new_offset) const {
    if (new_offset <= offset) {
      return {Interval(new_offset, 0), Interval(*this)};
    }
    if (new_offset >= end()) {
      return {Interval(*this), Interval(new_offset, 0)};
    }
    return {Interval(offset, new_offset - offset),
            Interval(new_offset, end() - new_offset)};
  }

  std::pair<Interval, Interval> SplitAtLength(uint64_t new_length) const {
    return SplitAtOffset(offset + new_length);
  }

  bool operator<(const Interval &other) const {
    if (offset != other.offset) {
      return offset < other.offset;
    }
    return length < other.length;
  }

  bool operator==(const Interval &other) const {
    return offset == other.offset && length == other.length;
  }
};

uint64_t IntervalsGetLength(const std::vector<Interval> &intervals);

std::vector<Interval> SubtractIntervals(const std::vector<Interval> &list_a,
                                        const std::vector<Interval> &list_b);

std::vector<Interval> NormalizeIntervals(std::vector<Interval> intervals);

// (intervals, length) -> (intervals, intervals)
std::pair<std::vector<Interval>, std::vector<Interval>> TakeLengthFromStart(
    const std::vector<Interval> &intervals, uint64_t length);

}  // namespace android::apex
