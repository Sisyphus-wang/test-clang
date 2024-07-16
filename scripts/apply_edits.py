#!/usr/bin/env vpython3
# Copyright 2013 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Applies edits generated by a clang tool that was run on Chromium code.

Synopsis:

  cat run_tool.out | extract_edits.py | apply_edits.py <build dir> <filters...>

For example - to apply edits only to WTF sources:

  ... | apply_edits.py out/gn third_party/WebKit/Source/wtf

In addition to filters specified on the command line, the tool also skips edits
that apply to files that are not covered by git.
"""

import argparse
import collections
import functools
import multiprocessing
import os
import os.path
import re
import subprocess
import sys

script_dir = os.path.dirname(os.path.realpath(__file__))
tool_dir = os.path.abspath(os.path.join(script_dir, '../pylib'))
sys.path.insert(0, tool_dir)

from clang import compile_db

Edit = collections.namedtuple('Edit',
                              ('edit_type', 'offset', 'length', 'replacement'))


def _GetFilesFromGit(paths=None):
  """Gets the list of files in the git repository.

  Args:
    paths: Prefix filter for the returned paths. May contain multiple entries.
  """
  args = []
  if sys.platform == 'win32':
    args.append('git.bat')
  else:
    args.append('git')
  args.append('ls-files')
  if paths:
    args.extend(paths)
  command = subprocess.Popen(args, stdout=subprocess.PIPE)
  output, _ = command.communicate()
  output = output.decode('utf-8')
  return [os.path.realpath(p) for p in output.splitlines()]


def _ParseEditsFromStdin(build_directory):
  """Extracts generated list of edits from the tool's stdout.

  The expected format is documented at the top of this file.

  Args:
    build_directory: Directory that contains the compile database. Used to
      normalize the filenames.

  Returns:
    A dictionary mapping filenames to the associated edits.
  """
  path_to_resolved_path = {}
  def _ResolvePath(path):
    if path in path_to_resolved_path:
      return path_to_resolved_path[path]

    if not os.path.isfile(path):
      resolved_path = os.path.realpath(os.path.join(build_directory, path))
    else:
      resolved_path = os.path.realpath(path)

    if not os.path.isfile(resolved_path):
      sys.stderr.write('Edit applies to a non-existent file: %s\n' % path)
      resolved_path = None

    path_to_resolved_path[path] = resolved_path
    return resolved_path

  edits = collections.defaultdict(list)
  for line in sys.stdin:
    line = line.rstrip("\n\r")
    try:
      edit_type, path, offset, length, replacement = line.split(':::', 4)
      replacement = replacement.replace('\0', '\n')
      path = _ResolvePath(path)
      if not path: continue
      edits[path].append(
          Edit(edit_type, int(offset), int(length),
               replacement.encode("utf-8")))
    except ValueError:
      sys.stderr.write('Unable to parse edit: %s\n' % line)
  return edits


_PLATFORM_SUFFIX = \
    r'(?:_(?:android|aura|chromeos|ios|linux|mac|ozone|posix|win|x11))?'
_TEST_SUFFIX = \
    r'(?:_(?:browser|interactive_ui|ui|unit)?test)?'
_suffix_regex = re.compile(_PLATFORM_SUFFIX + _TEST_SUFFIX)


def _FindPrimaryHeaderBasename(filepath):
  """ Translates bar/foo.cc -> foo
                 bar/foo_posix.cc -> foo
                 bar/foo_unittest.cc -> foo
                 bar/foo.h -> None
  """
  dirname, filename = os.path.split(filepath)
  basename, extension = os.path.splitext(filename)
  if extension == '.h':
    return None
  basename = _suffix_regex.sub('', basename)
  return basename


_INCLUDE_INSERTION_POINT_REGEX_TEMPLATE = r'''
   ^(?!               # Match the start of the first line that is
                      # not one of the following:

      \s+             # 1. Line starting with whitespace
                      #    (this includes blank lines and continuations of
                      #     C comments that start with whitespace/indentation)

    | //              # 2a. A C++ comment
    | /\*             # 2b. A C comment
    | \*              # 2c. A continuation of a C comment
                      #     (see also rule 1. above)

    | \xef \xbb \xbf  # 3. "Lines" starting with BOM character

      # 4. Include guards (Chromium-style)
    | \#ifndef \s+ [A-Z0-9_]+_H ( | _ | __ ) \b \s* $
    | \#define \s+ [A-Z0-9_]+_H ( | _ | __ ) \b \s* $

      # 4b. Include guards (anything that repeats):
      #     - the same <guard> has to repeat in both the #ifndef and the #define
      #     - #define has to be "simple" - either:
      #         - either: #define GUARD
      #         - or    : #define GUARD 1
    | \#ifndef \s+ (?P<guard> [A-Za-z0-9_]* ) \s* $ ( \n | \r )* ^
      \#define \s+ (?P=guard) \s* ( | 1 \s* ) $
    | \#define \s+ (?P=guard) \s* ( | 1 \s* ) $  # Skipping previous line.

      # 5. A C/C++ system include
    | \#include \s* < .* >

      # 6. A primary header include
      #    (%%s should be the basename returned by _FindPrimaryHeaderBasename).
      #
      # TODO(lukasza): Do not allow any directory below - require the top-level
      # directory to be the same and at least one itermediate dirname to be the
      # same.
    | \#include \s*   "
          [^"]* \b       # Allowing any directory
          %s[^"/]*\.h "  # Matching both basename.h and basename_posix.h
    )
'''


_NEWLINE_CHARACTERS = [ord('\n'), ord('\r')]


def _FindStartOfPreviousLine(contents, index):
  """ Requires that `index` points to the start of a line.
      Returns an index to the start of the previous line.
  """
  assert (index > 0)
  assert (contents[index - 1] in _NEWLINE_CHARACTERS)

  # Go back over the newline characters associated with the *single* end of a
  # line just before `index`, despite of whether end of a line is designated by
  # "\r", "\n" or "\r\n".  Examples:
  # 1. "... \r\n <new index> \r\n <old index> ...
  # 2. "... \n <new index> \n <old index> ...
  index = index - 1
  if index > 0 and contents[index - 1] in _NEWLINE_CHARACTERS and \
      contents[index - 1] != contents[index]:
    index = index - 1

  # Go back until `index` points right after an end of a line (or at the
  # beginning of the `contents`).
  while index > 0 and contents[index - 1] not in _NEWLINE_CHARACTERS:
    index = index - 1

  return index


def _SkipOverPreviousComment(contents, index):
  """ Returns `index`, possibly moving it earlier so that it skips over comment
      lines appearing in `contents` just before the old `index.

      Example:
          <returned `index` points here>// Comment
                                        // Comment
          <original `index` points here>bar
  """
  # If `index` points at the start of the file, or `index` doesn't point at the
  # beginning of a line, then don't skip anything and just return `index`.
  if index == 0 or contents[index - 1] not in _NEWLINE_CHARACTERS:
    return index

  # Is the previous line a non-comment?  If so, just return `index`.
  new_index = _FindStartOfPreviousLine(contents, index)
  prev_text = contents[new_index:index]
  _COMMENT_START_REGEX = b"^  \s*  (  //  |  \*  )"
  if not re.search(_COMMENT_START_REGEX, prev_text, re.VERBOSE):
    return index

  # Otherwise skip over the previous line + continue skipping via recursion.
  return _SkipOverPreviousComment(contents, new_index)


def _InsertNonSystemIncludeHeader(filepath, header_line_to_add, contents):
  """ Mutates |contents| (contents of |filepath|) to #include
      the |header_to_add
  """
  # Don't add the header if it is already present.
  replacement_text = header_line_to_add
  if replacement_text in contents:
    return contents
  replacement_text += b"\n"

  # Find the right insertion point.
  #
  # Note that we depend on a follow-up |git cl format| for the right order of
  # headers.  Therefore we just need to find the right header group (e.g. skip
  # system headers and the primary header).
  primary_header_basename = _FindPrimaryHeaderBasename(filepath)
  if primary_header_basename is None:
    primary_header_basename = ':this:should:never:match:'
  regex_text = _INCLUDE_INSERTION_POINT_REGEX_TEMPLATE % primary_header_basename
  match = re.search(regex_text.encode("utf-8"), contents,
                    re.MULTILINE | re.VERBOSE)
  assert (match is not None)
  insertion_point = _SkipOverPreviousComment(contents, match.start())

  # Extra empty line is required if the addition is not adjacent to other
  # includes.
  if not contents[insertion_point:].startswith(b"#include"):
    replacement_text += b"\n"

  # Make the edit.
  return contents[:insertion_point] + replacement_text + \
      contents[insertion_point:]


def _ApplyReplacement(filepath, contents, edit, last_edit):
  assert (edit.edit_type == 'r')
  assert ((last_edit is None) or (last_edit.edit_type == 'r'))

  if last_edit is not None:
    if edit.offset == last_edit.offset and edit.length == last_edit.length:
      assert (edit.replacement != last_edit.replacement)
      raise ValueError(
          ('Conflicting replacement text: ' +
           '%s at offset %d, length %d: "%s" != "%s"\n') %
          (filepath, edit.offset, edit.length, edit.replacement.decode("utf-8"),
           last_edit.replacement.decode("utf-8")))

    if edit.offset + edit.length > last_edit.offset:
      raise ValueError(
          ('Overlapping replacements: ' +
           '%s at offset %d, length %d: "%s" and ' +
           'offset %d, length %d: "%s"\n') %
          (filepath, edit.offset, edit.length, edit.replacement.decode("utf-8"),
           last_edit.offset, last_edit.length,
           last_edit.replacement.decode("utf-8")))

  start = edit.offset
  end = edit.offset + edit.length
  original_contents = contents
  contents = contents[:start] + edit.replacement + contents[end:]
  if not edit.replacement:
    contents = _ExtendDeletionIfElementIsInList(original_contents, contents,
                                                edit.offset, edit.length)
  return contents


def _ApplyIncludeHeader(filepath, contents, edit, last_edit):
  header_line_to_add = '#include "%s"' % edit.replacement.decode("utf-8")
  return _InsertNonSystemIncludeHeader(filepath,
                                       header_line_to_add.encode("utf-8"),
                                       contents)


def _ApplySingleEdit(filepath, contents, edit, last_edit):
  if edit.edit_type == 'r':
    return _ApplyReplacement(filepath, contents, edit, last_edit)
  elif edit.edit_type == 'include-user-header':
    return _ApplyIncludeHeader(filepath, contents, edit, last_edit)
  else:
    raise ValueError('Unrecognized edit directive "%s": %s\n' %
                     (edit.edit_type, filepath))
    return contents


def _ApplyEditsToSingleFileContents(filepath, contents, edits):
  # Sort the edits and iterate through them in reverse order. Sorting allows
  # duplicate edits to be quickly skipped, while reversing means that
  # subsequent edits don't need to have their offsets updated with each edit
  # applied.
  #
  # Note that after sorting in reverse, the 'i' directives will come after 'r'
  # directives.
  edits.sort(reverse=True)

  edit_count = 0
  error_count = 0
  last_edit = None
  for edit in edits:
    if edit == last_edit:
      continue
    try:
      contents = _ApplySingleEdit(filepath, contents, edit, last_edit)
      last_edit = edit
      edit_count += 1
    except ValueError as err:
      sys.stderr.write(str(err) + '\n')
      error_count += 1

  return (contents, edit_count, error_count)


def _ApplyEditsToSingleFile(filepath, edits):
  with open(filepath, 'rb+') as f:
    contents = f.read()
    (contents, edit_count,
     error_count) = _ApplyEditsToSingleFileContents(filepath, contents, edits)
    f.seek(0)
    f.truncate()
    f.write(contents)
  return (edit_count, error_count)


def _ApplyEdits(edits):
  """Apply the generated edits.

  Args:
    edits: A dict mapping filenames to Edit instances that apply to that file.
  """
  edit_count = 0
  error_count = 0
  done_files = 0
  for k, v in edits.items():
    tmp_edit_count, tmp_error_count = _ApplyEditsToSingleFile(k, v)
    edit_count += tmp_edit_count
    error_count += tmp_error_count
    done_files += 1
    percentage = (float(done_files) / len(edits)) * 100
    sys.stdout.write('Applied %d edits (%d errors) to %d files [%.2f%%]\r' %
                     (edit_count, error_count, done_files, percentage))

  sys.stdout.write('\n')
  return -error_count


_WHITESPACE_BYTES = frozenset((ord('\t'), ord('\n'), ord('\r'), ord(' ')))


def _ExtendDeletionIfElementIsInList(original_contents, contents, offset,
                                     length):
  """Extends the range of a deletion if the deleted element was part of a list.

  This rewriter helper makes it easy for refactoring tools to remove elements
  from a list. Even if a matcher callback knows that it is removing an element
  from a list, it may not have enough information to accurately remove the list
  element; for example, another matcher callback may end up removing an adjacent
  list element, or all the list elements may end up being removed.

  With this helper, refactoring tools can simply remove the list element and not
  worry about having to include the comma in the replacement.

  Args:
    original_contents: A bytearray before the deletion was applied.
    contents: A bytearray with the deletion already applied.
    offset: The offset in the bytearray where the deleted range used to be.
    length: The length in the bytearray where the deleted range used to be.
  """
  char_before = char_after = None
  left_trim_count = 0
  for byte in reversed(contents[:offset]):
    left_trim_count += 1
    if byte in _WHITESPACE_BYTES:
      continue
    if byte in (ord(','), ord(':'), ord('('), ord('{')):
      char_before = chr(byte)
    break

  right_trim_count = 0
  for byte in contents[offset:]:
    right_trim_count += 1
    if byte in _WHITESPACE_BYTES:
      continue
    if byte == ord(','):
      char_after = chr(byte)
    break

  def notify(left_offset, right_offset):
    (start, end) = (offset, offset + length)
    deleted = original_contents[start:end].decode('utf-8')
    (start, end) = (start - left_offset, end + right_offset)
    extended = original_contents[start:end].decode('utf-8')
    (start, end) = (max(0, start - 5), end + 5)
    context = original_contents[start:end].decode('utf-8')
    sys.stdout.write('Extended deletion of "%s" to "%s" in "...%s..."\n' %
                     (deleted, extended, context))

  if char_before:
    if char_after:
      notify(0, right_trim_count)
      return contents[:offset] + contents[offset + right_trim_count:]
    elif char_before in (',', ':'):
      notify(left_trim_count, 0)
      return contents[:offset - left_trim_count] + contents[offset:]
  return contents


def main():
  parser = argparse.ArgumentParser(
      epilog="""
Reads edit directives from stdin and applies them to all files under
Git control, modulo the path filters.

See docs/clang_tool_refactoring.md for details.

When an edit direct has an empty replacement text (e.g.,
"r:::path/to/file/to/edit:::offset1:::length1:::") and the script detects that
the deleted text is part of a "list" (e.g., function parameters, initializers),
the script extends the deletion to remove commas, etc. as needed. A way to
suppress this behavior is to replace the text with a single space or similar
(e.g., "r:::path/to/file/to/edit:::offset1:::length1::: ").
""",
      formatter_class=argparse.RawTextHelpFormatter,
  )
  parser.add_argument(
      '-p',
      required=True,
      help='path to the build dir (dir that edit paths are relative to)')
  parser.add_argument(
      'path_filter',
      nargs='*',
      help='optional paths to filter what files the tool is run on')
  args = parser.parse_args()

  filenames = set(_GetFilesFromGit(args.path_filter))
  edits = _ParseEditsFromStdin(args.p)
  return _ApplyEdits(
      {k: v
       for k, v in edits.items() if os.path.realpath(k) in filenames})


if __name__ == '__main__':
  sys.exit(main())
