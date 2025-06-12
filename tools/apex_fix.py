#!/usr/bin/env python
#
# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""apex_fix is a tool that fixes an input apex for testing.

For now, it fixes the input apex to meet the size requirements: 4K alignment.
"""
import argparse
import os
import sys
import zipfile


BLOCK_SIZE = 4096


def fix_apex(input_path):
  try:
    # First, get the file size.
    original_size = os.path.getsize(input_path)
    # No need to fix because the file size is 4K aligned.
    if original_size % BLOCK_SIZE == 0:
      return 0

    # Modify EOCD comment to resize the file to match 4K alignment.

    # Open the file in read mode to find out the existing comment length.
    with zipfile.ZipFile(input_path, 'r') as zf_read:
      existing_comment_len = len(zf_read.comment)

    # Base size of the file excluding the comment (and excluding the comment part of EOCD)
    # The EOCD record itself is at the end of the file, and its size varies with the comment
    # length. Subtracting the existing comment length from the file size gives the size of
    # ZIP content + EOCD fixed part (22 bytes).
    base_size_without_comment = original_size - existing_comment_len

    # new_comment_len = (N * BLOCK_SIZE) - base_size_without_comment
    target_total_size = ((base_size_without_comment +
                          BLOCK_SIZE - 1) // BLOCK_SIZE) * BLOCK_SIZE

    new_comment_len = target_total_size - base_size_without_comment

    # Open the file in append mode to add the new comment.
    # This effectively truncates the existing EOCD and appends a new one with the new comment.

    with zipfile.ZipFile(input_path, 'a') as zf:
      zf.comment = b'\0' * new_comment_len  # Pad with null bytes

    # Assert the postcondition.
    assert os.path.getsize(input_path) == target_total_size, 'File size mismatch'

    return 0

  except FileNotFoundError:
    print(f"Error: File '{input_path}' not found.")
    return 1
  except zipfile.BadZipFile:
    print(f"Error: File '{input_path}' is not a valid ZIP file.")
    return 1


def main(argv):
  parser = argparse.ArgumentParser()
  parser.add_argument('input', type=str, help='input APEX file to fix')
  args = parser.parse_args(argv)
  return_code = fix_apex(args.input)
  sys.exit(return_code)


if __name__ == '__main__':
  main(sys.argv[1:])
