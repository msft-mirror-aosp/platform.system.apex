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

#include <gtest/gtest.h>

namespace android::apex {

TEST(IntervalsTest, Normalize_Empty) {
  std::vector<Interval> intervals{};
  std::vector<Interval> expected{};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, Normalize_MergeAdjacent) {
  std::vector<Interval> intervals{{0, 5}, {5, 5}, {5, 3}};
  std::vector<Interval> expected{{0, 10}};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, Normalize_OutOfOrder) {
  std::vector<Interval> intervals{{5, 5}, {0, 5}};
  std::vector<Interval> expected{{0, 10}};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, Normalize_ZeroInterval) {
  std::vector<Interval> intervals{{5, 5}, {10, 0}, {0, 5}};
  std::vector<Interval> expected{{0, 10}};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, Normalize_ManyZeroIntervals) {
  std::vector<Interval> intervals{{}, {100, 0}, {5, 5}, {}, {20, 0}};
  std::vector<Interval> expected{{5, 5}};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, Normalize_SingleZeroInterval) {
  std::vector<Interval> intervals{{100, 0}};
  std::vector<Interval> expected{};
  ASSERT_EQ(expected, NormalizeIntervals(intervals));
}

TEST(IntervalsTest, TakeLengthFromStart_Simple) {
  std::vector<Interval> intervals{{0, 20}};
  auto [first, second] = TakeLengthFromStart(intervals, 5);
  std::vector<Interval> first_expected{{0, 5}};
  std::vector<Interval> second_expected{{5, 15}};
  ASSERT_EQ(first_expected, first);
  ASSERT_EQ(second_expected, second);
}

TEST(IntervalsTest, TakeLengthFromStart_SpanningMultiple) {
  std::vector<Interval> intervals{{0, 5}, {10, 5}};
  auto [first, second] = TakeLengthFromStart(intervals, 7);
  std::vector<Interval> first_expected{{0, 5}, {10, 2}};
  std::vector<Interval> second_expected{{12, 3}};
  ASSERT_EQ(first_expected, first);
  ASSERT_EQ(second_expected, second);
}

TEST(IntervalsTest, TakeLengthFromStart_InsufficientLength) {
  std::vector<Interval> intervals{{0, 5}, {10, 5}};
  auto [first, second] = TakeLengthFromStart(intervals, 20);
  std::vector<Interval> first_expected{};
  std::vector<Interval> second_expected = intervals;
  ASSERT_EQ(first_expected, first);
  ASSERT_EQ(second_expected, second);
}

TEST(IntervalsTest, TakeLengthFromStart_ExactBoundary) {
  std::vector<Interval> intervals{{0, 5}, {10, 5}, {20, 5}};
  auto [first, second] = TakeLengthFromStart(intervals, 10);
  std::vector<Interval> first_expected{{0, 5}, {10, 5}};
  std::vector<Interval> second_expected{{20, 5}};
  ASSERT_EQ(first_expected, first);
  ASSERT_EQ(second_expected, second);
}

TEST(IntervalsTest, TakeLengthFromStart_MatchingTotalLength) {
  std::vector<Interval> intervals{{0, 5}, {10, 5}, {20, 5}};
  auto [first, second] = TakeLengthFromStart(intervals, 15);
  std::vector<Interval> first_expected = intervals;
  std::vector<Interval> second_expected{};
  ASSERT_EQ(first_expected, first);
  ASSERT_EQ(second_expected, second);
}

TEST(IntervalsTest, Subtract_Simple) {
  std::vector<Interval> intervals{{0, 20}};
  std::vector<Interval> subtract{{5, 5}};
  std::vector<Interval> expected{{0, 5}, {10, 10}};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_Multiple) {
  std::vector<Interval> intervals{{0, 100}};
  std::vector<Interval> subtract{{10, 10}, {30, 5}, {50, 20}};
  std::vector<Interval> expected{{0, 10}, {20, 10}, {35, 15}, {70, 30}};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_NonOverlappingIntervals) {
  std::vector<Interval> intervals{{0, 10}, {50, 100}};
  std::vector<Interval> subtract{{10, 10}};
  std::vector<Interval> expected = intervals;

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_Bigger) {
  std::vector<Interval> intervals{{10, 10}, {50, 10}};
  std::vector<Interval> subtract{{0, 100}};
  std::vector<Interval> expected{};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_Same) {
  std::vector<Interval> intervals{{0, 20}};
  std::vector<Interval> subtract = intervals;
  std::vector<Interval> expected{};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_NotNormalized) {
  std::vector<Interval> intervals{{0, 100}, {100, 0}};
  std::vector<Interval> subtract{{10, 10}, {50, 20}, {30, 5}};
  std::vector<Interval> expected{{0, 10}, {20, 10}, {35, 15}, {70, 30}};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalsTest, Subtract_NotNormalized_SecondArg) {
  std::vector<Interval> intervals{{0, 100}};
  std::vector<Interval> subtract{{10, 10}, {30, 5}, {50, 10}, {60, 10}};
  std::vector<Interval> expected{{0, 10}, {20, 10}, {35, 15}, {70, 30}};

  ASSERT_EQ(SubtractIntervals(intervals, subtract), expected);
}

TEST(IntervalTest, ApplyOffsetLength_WithinIntervals) {
  const std::vector<Interval> intervals = {{10, 5}, {20, 10}, {40, 8}};
  auto result = ApplyOffsetLength(intervals, 2, 15);
  EXPECT_EQ(result, (std::vector<Interval>{{12, 3}, {20, 10}, {40, 2}}));
}

TEST(IntervalTest, ApplyOffsetLength_ZeroOffset) {
  const std::vector<Interval> intervals = {{10, 5}, {20, 10}, {40, 8}};
  auto result = ApplyOffsetLength(intervals, 0, 15);
  EXPECT_EQ(result, (std::vector<Interval>{{10, 5}, {20, 10}}));
}

TEST(IntervalTest, ApplyOffsetLength_ZeroLength) {
  const std::vector<Interval> intervals = {{10, 5}, {20, 10}, {40, 8}};
  auto result = ApplyOffsetLength(intervals, 5, 0);
  EXPECT_TRUE(result.empty());
}

TEST(IntervalTest, ApplyOffsetLength_ExactBoundary) {
  const std::vector<Interval> intervals = {{10, 5}, {20, 10}, {40, 8}};
  auto result = ApplyOffsetLength(intervals, 5, 10);
  EXPECT_EQ(result, (std::vector<Interval>{{20, 10}}));
}

TEST(IntervalTest, ApplyOffsetLength_ZeroInput) {
  const std::vector<Interval> intervals = {{10, 5}, {20, 10}, {40, 8}};
  auto result = ApplyOffsetLength({}, 10, 10);
  EXPECT_TRUE(result.empty());
}

}  // namespace android::apex
