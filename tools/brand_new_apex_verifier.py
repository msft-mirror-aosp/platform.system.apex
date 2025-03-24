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
"""brand_new_apex_verifier verifies blocklist files and public keys for brand-new APEX.

Verifies the integrity of blocklist files and public keys associated with brand-new APEX modules.
Specifically, it checks for duplicate entries in blocklist files and duplicate public key content.

Example:
    $ brand_new_apex_verifier --pubkey_paths path1 path2 --blocklist_paths --output_path output
"""
import argparse
import os

import apex_blocklist_pb2
from google.protobuf import json_format


def ParseArgs():
  parser = argparse.ArgumentParser()
  parser.add_argument('--pubkey_paths', nargs='*', help='List of paths to the .avbpubkey files.')
  parser.add_argument(
      '--blocklist_paths', nargs='*', help='List of paths to the blocklist.json files.',
  )
  parser.add_argument(
      '--output_path', help='Output file path.',
  )
  return parser.parse_args()


def ParseBlocklistFile(file):
  """Parses a blocklist JSON file into an ApexBlocklist protobuf object.

  Args:
    file: The path to the blocklist JSON file.

  Returns:
    An ApexBlocklist protobuf object.

  Raises:
    Exception: If parsing fails due to JSON format errors.
  """
  try:
    with open(file, 'rb') as f:
      return json_format.Parse(f.read(), apex_blocklist_pb2.ApexBlocklist())
  except json_format.ParseError as err:
    raise ValueError(err) from err


def VerifyBlocklistFile(blocklist_paths):
  """Verifies the provided blocklist files.

  Checks for duplicate apex names within each blocklist file.

  Args:
    blocklist_paths: The paths to the blocklist files.

  Raises:
    Exception: If duplicate apex names are found in any blocklist file.
  """
  for blocklist_path in blocklist_paths:
    if not os.path.exists(blocklist_path):
      continue
    apex_blocklist = ParseBlocklistFile(blocklist_path)
    blocked_apex = set()
    for apex_item in apex_blocklist.blocked_apex:
      if apex_item.name in blocked_apex:
        raise ValueError(f'Duplicate apex name found in blocklist file: {blocklist_path}')
      blocked_apex.add(apex_item.name)


def VerifyPublicKey(pubkey_paths):
  """Verifies the provided public key files.

  Checks for duplicate public key file content.

  Args:
    pubkey_paths: Paths to the public key files.

  Raises:
    Exception: If duplicate public key content is found.
  """
  pubkeys = {}
  for file_path in pubkey_paths:
    if not os.path.exists(file_path):
      continue
    try:
      with open(file_path, 'rb') as f:
        file_content = f.read()
        if file_content in pubkeys:
          raise ValueError(
            f'Duplicate key material found: {pubkeys[file_content]} and '
            f'{file_path}.')
        pubkeys[file_content] = file_path
    except OSError as e:
      raise ValueError(f'Error reading public key file {file_path}: {e}') from e


def main():
  args = ParseArgs()

  with open(args.output_path, 'w', encoding='utf-8') as f:
    try:
      VerifyBlocklistFile(args.blocklist_paths)
      VerifyPublicKey(args.pubkey_paths)
      f.write('Verification successful.')
    except ValueError as e:
      f.write(f'Verification failed: {e}')

if __name__ == '__main__':
  main()
