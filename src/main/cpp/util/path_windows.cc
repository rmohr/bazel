// Copyright 2018 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/main/cpp/util/path_platform.h"

#include <assert.h>
#include <wchar.h>  // wcslen
#include <windows.h>

#include <algorithm>
#include <memory>  // unique_ptr
#include <sstream>
#include <vector>

#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file_platform.h"
#include "src/main/cpp/util/logging.h"
#include "src/main/cpp/util/strings.h"
#include "src/main/native/windows/file.h"

namespace blaze_util {

using bazel::windows::HasUncPrefix;

static char GetCurrentDrive();

template <typename char_type>
struct CharTraits {
  static bool IsAlpha(char_type ch);
};

template <>
struct CharTraits<char> {
  static bool IsAlpha(char ch) { return isalpha(ch); }
};

template <>
struct CharTraits<wchar_t> {
  static bool IsAlpha(wchar_t ch) { return iswalpha(ch); }
};

template <typename char_type>
static bool IsPathSeparator(char_type ch) {
  return ch == '/' || ch == '\\';
}

template <typename char_type>
static bool HasDriveSpecifierPrefix(const char_type* ch) {
  return CharTraits<char_type>::IsAlpha(ch[0]) && ch[1] == ':';
}

std::string ConvertPath(const std::string& path) {
  // The path may not be Windows-style and may not be normalized, so convert it.
  std::string converted_path;
  std::string error;
  if (!blaze_util::AsWindowsPath(path, &converted_path, &error)) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "ConvertPath(" << path << "): AsWindowsPath failed: " << error;
  }
  std::transform(converted_path.begin(), converted_path.end(),
                 converted_path.begin(), ::towlower);
  return converted_path;
}

std::string MakeAbsolute(const std::string& path) {
  // The path may not be Windows-style and may not be normalized, so convert it.
  std::wstring wpath;
  std::string error;
  if (!AsAbsoluteWindowsPath(path, &wpath, &error)) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "MakeAbsolute(" << path
        << "): AsAbsoluteWindowsPath failed: " << error;
  }
  std::transform(wpath.begin(), wpath.end(), wpath.begin(), ::towlower);
  return std::string(
      WstringToCstring(RemoveUncPrefixMaybe(wpath.c_str())).get());
}

std::string MakeAbsoluteAndResolveWindowsEnvvars(const std::string& path) {
  // Get the size of the expanded string, so we know how big of a buffer to
  // provide. The returned size includes the null terminator.
  std::unique_ptr<CHAR[]> resolved(new CHAR[MAX_PATH]);
  DWORD size =
      ::ExpandEnvironmentStrings(path.c_str(), resolved.get(), MAX_PATH);
  if (size == 0) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "MakeAbsoluteAndResolveWindowsEnvvars(" << path
        << "): ExpandEnvironmentStrings failed: " << GetLastErrorString();
  } else if (size > MAX_PATH) {
    // Try again with a buffer bigger than MAX_PATH.
    resolved.reset(new CHAR[size]);
    DWORD second_size =
        ::ExpandEnvironmentStrings(path.c_str(), resolved.get(), size);
    if (second_size == 0) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "MakeAbsoluteAndResolveWindowsEnvvars(" << path
          << "): ExpandEnvironmentStrings failed with second buffer: "
          << GetLastErrorString();
    }
    assert(second_size <= size);
  }
  return MakeAbsolute(std::string(resolved.get()));
}

bool CompareAbsolutePaths(const std::string& a, const std::string& b) {
  return ConvertPath(a) == ConvertPath(b);
}

std::string PathAsJvmFlag(const std::string& path) {
  std::string cpath;
  std::string error;
  if (!AsWindowsPath(path, &cpath, &error)) {
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "PathAsJvmFlag(" << path << "): AsWindowsPath failed: " << error;
  }
  // Convert forward slashes and backslashes to double (escaped) backslashes, so
  // they are safe to pass on the command line to the JVM and the JVM won't
  // misinterpret them.
  // See https://github.com/bazelbuild/bazel/issues/2576 and
  // https://github.com/bazelbuild/bazel/issues/6098
  size_t separators = 0;
  for (const auto& c : cpath) {
    if (c == '/' || c == '\\') {
      separators++;
    }
  }
  // In the result we replace each '/' and '\' with "\\", i.e. the total size
  // *increases* by `separators`.
  // Create a string of that size, filled with zeroes.
  std::string result(/* count */ cpath.size() + separators, '\0');
  std::string::size_type i = 0;
  for (const auto& c : cpath) {
    if (c == '/' || c == '\\') {
      result[i++] = '\\';
      result[i++] = '\\';
    } else {
      result[i++] = c;
    }
  }
  return result;
}

void AddUncPrefixMaybe(std::wstring* path) {
  if (path->size() >= MAX_PATH && !HasUncPrefix(path->c_str())) {
    *path = std::wstring(L"\\\\?\\") + *path;
  }
}

const wchar_t* RemoveUncPrefixMaybe(const wchar_t* ptr) {
  return ptr + (HasUncPrefix(ptr) ? 4 : 0);
}

// Checks if the path is absolute and/or is a root path.
//
// If `must_be_root` is true, then in addition to being absolute, the path must
// also be just the root part, no other components, e.g. "c:\" is both absolute
// and root, but "c:\foo" is just absolute.
template <typename char_type>
static bool IsRootOrAbsolute(const std::basic_string<char_type>& path,
                             bool must_be_root) {
  // An absolute path is one that starts with "/", "\", "c:/", "c:\",
  // "\\?\c:\", or rarely "\??\c:\" or "\\.\c:\".
  //
  // It is unclear whether the UNC prefix is just "\\?\" or is "\??\" also
  // valid (in some cases it seems to be, though MSDN doesn't mention it).
  return
      // path is (or starts with) "/" or "\"
      ((must_be_root ? path.size() == 1 : !path.empty()) &&
       IsPathSeparator(path[0])) ||
      // path is (or starts with) "c:/" or "c:\" or similar
      ((must_be_root ? path.size() == 3 : path.size() >= 3) &&
       HasDriveSpecifierPrefix(path.c_str()) && IsPathSeparator(path[2])) ||
      // path is (or starts with) "\\?\c:\" or "\??\c:\" or similar
      ((must_be_root ? path.size() == 7 : path.size() >= 7) &&
       HasUncPrefix(path.c_str()) &&
       HasDriveSpecifierPrefix(path.c_str() + 4) && IsPathSeparator(path[6]));
}

template <typename char_type>
static std::pair<std::basic_string<char_type>, std::basic_string<char_type> >
SplitPathImpl(const std::basic_string<char_type>& path) {
  if (path.empty()) {
    return std::make_pair(std::basic_string<char_type>(),
                          std::basic_string<char_type>());
  }

  size_t pos = path.size() - 1;
  for (auto it = path.crbegin(); it != path.crend(); ++it, --pos) {
    if (IsPathSeparator(*it)) {
      if ((pos == 2 || pos == 6) &&
          IsRootOrAbsolute(path.substr(0, pos + 1), /* must_be_root */ true)) {
        // Windows path, top-level directory, e.g. "c:\foo",
        // result is ("c:\", "foo").
        // Or UNC path, top-level directory, e.g. "\\?\c:\foo"
        // result is ("\\?\c:\", "foo").
        return std::make_pair(
            // Include the "/" or "\" in the drive specifier.
            path.substr(0, pos + 1), path.substr(pos + 1));
      } else {
        // Windows path (neither top-level nor drive root), Unix path, or
        // relative path.
        return std::make_pair(
            // If the only "/" is the leading one, then that shall be the first
            // pair element, otherwise the substring up to the rightmost "/".
            pos == 0 ? path.substr(0, 1) : path.substr(0, pos),
            // If the rightmost "/" is the tail, then the second pair element
            // should be empty.
            pos == path.size() - 1 ? std::basic_string<char_type>()
                                   : path.substr(pos + 1));
      }
    }
  }
  // Handle the case with no '/' or '\' in `path`.
  return std::make_pair(std::basic_string<char_type>(), path);
}

std::pair<std::string, std::string> SplitPath(const std::string& path) {
  return SplitPathImpl(path);
}

std::pair<std::wstring, std::wstring> SplitPathW(const std::wstring& path) {
  return SplitPathImpl(path);
}

void assignNUL(std::string* s) { s->assign("NUL"); }

void assignNUL(std::wstring* s) { s->assign(L"NUL"); }

template <typename char_type>
bool AsWindowsPath(const std::basic_string<char_type>& path,
                   std::basic_string<char_type>* result, std::string* error) {
  if (path.empty()) {
    result->clear();
    return true;
  }
  if (IsDevNull(path.c_str())) {
    assignNUL(result);
    return true;
  }
  if (HasUncPrefix(path.c_str())) {
    // Path has "\\?\" prefix --> assume it's already Windows-style.
    *result = path.c_str();
    return true;
  }
  if (IsPathSeparator(path[0]) && path.size() > 1 && IsPathSeparator(path[1])) {
    // Unsupported path: "\\" or "\\server\path", or some degenerate form of
    // these, such as "//foo".
    if (error) {
      *error = "network paths are unsupported";
    }
    return false;
  }
  if (HasDriveSpecifierPrefix(path.c_str()) &&
      (path.size() < 3 || !IsPathSeparator(path[2]))) {
    // Unsupported path: "c:" or "c:foo"
    if (error) {
      *error = "working-directory relative paths are unsupported";
    }
    return false;
  }

  std::basic_string<char_type> mutable_path = path;
  if (path[0] == '/') {
    if (error) {
      *error = "Unix-style paths are unsupported";
    }
    return false;
  }

  if (path[0] == '\\') {
    // This is an absolute Windows path on the current drive, e.g. "\foo\bar".
    std::basic_string<char_type> drive(1, GetCurrentDrive());
    drive.push_back(':');
    mutable_path = drive + path;
  }  // otherwise this is a relative path, or absolute Windows path.
  result->assign(NormalizeWindowsPath(mutable_path));
  return true;
}

bool AsWindowsPath(const std::string& path, std::wstring* result,
                   std::string* error) {
  std::string normalized_win_path;
  if (!AsWindowsPath(path, &normalized_win_path, error)) {
    return false;
  }

  result->assign(CstringToWstring(normalized_win_path.c_str()).get());
  return true;
}

template <typename char_type>
bool AsAbsoluteWindowsPath(const std::basic_string<char_type>& path,
                           std::wstring* result, std::string* error) {
  if (path.empty()) {
    result->clear();
    return true;
  }
  if (IsDevNull(path.c_str())) {
    result->assign(L"NUL");
    return true;
  }
  if (!AsWindowsPath(path, result, error)) {
    return false;
  }
  if (!IsRootOrAbsolute(*result, /* must_be_root */ false)) {
    *result = GetCwdW() + L"\\" + *result;
  }
  if (!HasUncPrefix(result->c_str())) {
    *result = std::wstring(L"\\\\?\\") + *result;
  }
  return true;
}

bool AsShortWindowsPath(const std::string& path, std::string* result,
                        std::string* error) {
  if (IsDevNull(path.c_str())) {
    result->assign("NUL");
    return true;
  }

  result->clear();
  std::wstring wpath;
  std::wstring wsuffix;
  if (!AsAbsoluteWindowsPath(path, &wpath, error)) {
    return false;
  }
  DWORD size = ::GetShortPathNameW(wpath.c_str(), nullptr, 0);
  if (size == 0) {
    // GetShortPathNameW can fail if `wpath` does not exist. This is expected
    // when we are about to create a file at that path, so instead of failing,
    // walk up in the path until we find a prefix that exists and can be
    // shortened, or is a root directory. Save the non-existent tail in
    // `wsuffix`, we'll add it back later.
    std::vector<std::wstring> segments;
    while (size == 0 && !IsRootDirectoryW(wpath)) {
      std::pair<std::wstring, std::wstring> split = SplitPathW(wpath);
      wpath = split.first;
      segments.push_back(split.second);
      size = ::GetShortPathNameW(wpath.c_str(), nullptr, 0);
    }

    // Join all segments.
    std::wostringstream builder;
    bool first = true;
    for (auto it = segments.crbegin(); it != segments.crend(); ++it) {
      if (!first || !IsRootDirectoryW(wpath)) {
        builder << L'\\' << *it;
      } else {
        builder << *it;
      }
      first = false;
    }
    wsuffix = builder.str();
  }

  std::wstring wresult;
  if (IsRootDirectoryW(wpath)) {
    // Strip the UNC prefix from `wpath`, and the leading "\" from `wsuffix`.
    wresult = std::wstring(RemoveUncPrefixMaybe(wpath.c_str())) + wsuffix;
  } else {
    std::unique_ptr<WCHAR[]> wshort(
        new WCHAR[size]);  // size includes null-terminator
    if (size - 1 != ::GetShortPathNameW(wpath.c_str(), wshort.get(), size)) {
      if (error) {
        std::string last_error = GetLastErrorString();
        std::stringstream msg;
        msg << "AsShortWindowsPath(" << path << "): GetShortPathNameW("
            << WstringToString(wpath) << ") failed: " << last_error;
        *error = msg.str();
      }
      return false;
    }
    // GetShortPathNameW may preserve the UNC prefix in the result, so strip it.
    wresult = std::wstring(RemoveUncPrefixMaybe(wshort.get())) + wsuffix;
  }

  result->assign(WstringToCstring(wresult.c_str()).get());
  ToLower(result);
  return true;
}

bool IsDevNull(const char* path) {
  return path != NULL && *path != 0 &&
         (strncmp("/dev/null\0", path, 10) == 0 ||
          ((path[0] == 'N' || path[0] == 'n') &&
           (path[1] == 'U' || path[1] == 'u') &&
           (path[2] == 'L' || path[2] == 'l') && path[3] == 0));
}

bool IsDevNull(const wchar_t* path) {
  return path != NULL && *path != 0 &&
         (wcsncmp(L"/dev/null\0", path, 10) == 0 ||
          ((path[0] == L'N' || path[0] == L'n') &&
           (path[1] == L'U' || path[1] == L'u') &&
           (path[2] == L'L' || path[2] == L'l') && path[3] == 0));
}

bool IsRootDirectory(const std::string& path) {
  return IsRootOrAbsolute(path, true);
}

bool IsAbsolute(const std::string& path) {
  return IsRootOrAbsolute(path, false);
}

bool IsAbsolute(const std::wstring& path) {
  return IsRootOrAbsolute(path, false);
}

bool IsRootDirectoryW(const std::wstring& path) {
  return IsRootOrAbsolute(path, true);
}

static char GetCurrentDrive() {
  std::wstring cwd = GetCwdW();
  wchar_t wdrive = RemoveUncPrefixMaybe(cwd.c_str())[0];
  wchar_t offset = wdrive >= L'A' && wdrive <= L'Z' ? L'A' : L'a';
  return 'a' + wdrive - offset;
}

template <typename char_type>
std::basic_string<char_type> NormalizeWindowsPath(
    std::basic_string<char_type> path) {
  if (path.empty()) {
    return std::basic_string<char_type>();
  }
  if (path[0] == '/') {
    // This is an absolute MSYS path, error out.
    BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
        << "NormalizeWindowsPath(" << path << "): expected a Windows path";
  }
  if (path.size() >= 4 && HasUncPrefix(path.c_str())) {
    path = path.substr(4);
  }

  static const std::basic_string<char_type> dot(1, '.');
  static const std::basic_string<char_type> dotdot(2, '.');

  std::vector<std::basic_string<char_type>> segments;
  int segment_start = -1;
  // Find the path segments in `path` (separated by "/").
  for (int i = 0;; ++i) {
    if (!IsPathSeparator(path[i]) && path[i] != '\0') {
      // The current character does not end a segment, so start one unless it's
      // already started.
      if (segment_start < 0) {
        segment_start = i;
      }
    } else if (segment_start >= 0 && i > segment_start) {
      // The current character is "/" or "\0", so this ends a segment.
      // Add that to `segments` if there's anything to add; handle "." and "..".
      std::basic_string<char_type> segment(path, segment_start,
                                           i - segment_start);
      segment_start = -1;
      if (segment == dotdot) {
        if (!segments.empty() &&
            !HasDriveSpecifierPrefix(segments[0].c_str())) {
          segments.pop_back();
        }
      } else if (segment != dot) {
        segments.push_back(segment);
      }
    }
    if (path[i] == '\0') {
      break;
    }
  }

  // Handle the case when `path` is just a drive specifier (or some degenerate
  // form of it, e.g. "c:\..").
  if (segments.size() == 1 && segments[0].size() == 2 &&
      HasDriveSpecifierPrefix(segments[0].c_str())) {
    segments[0].push_back('\\');
    return segments[0];
  }

  // Join all segments.
  bool first = true;
  std::basic_ostringstream<char_type> result;
  for (const auto& s : segments) {
    if (!first) {
      result << '\\';
    }
    first = false;
    result << s;
  }
  return result.str();
}

}  // namespace blaze_util
