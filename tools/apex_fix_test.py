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
"""unit test for apex_fix"""

import os
import unittest
import zipfile
import tempfile

from apex_fix import BLOCK_SIZE, fix_apex


class TestApexFix(unittest.TestCase):
  """Unit test class for apex_fix
  """

  def create_temp_apex(self):
    temp_apex = tempfile.mktemp()
    # Someday we may want to create a more realistic APEX file here.
    # But for now this is enough for testing.
    with zipfile.ZipFile(temp_apex, 'w') as zf:
      zf.writestr('test.txt', 'This is a test file.')
      zf.comment = b'This is a file comment.'
    return temp_apex

  def test_fix_size(self):
    input_apex = self.create_temp_apex()

    self.assertEqual(fix_apex(input_apex), 0)
    self.assertEqual(os.path.getsize(input_apex) % BLOCK_SIZE,  0)

  def test_fix_size_already_aligned(self):
    input_apex = self.create_temp_apex()

    self.assertEqual(fix_apex(input_apex), 0)
    filesize = os.path.getsize(input_apex)

    self.assertEqual(fix_apex(input_apex), 0)
    self.assertEqual(os.path.getsize(input_apex), filesize)


if __name__ == '__main__':
  unittest.main()
