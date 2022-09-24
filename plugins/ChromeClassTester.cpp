// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A general interface for filtering and only acting on classes in Chromium C++
// code.

#include "ChromeClassTester.h"

#include <algorithm>

#include "Util.h"
#include "clang/AST/AST.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"

#ifdef LLVM_ON_UNIX
#include <sys/param.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace clang;
using chrome_checker::Options;

namespace {

bool ends_with(const std::string& one, const std::string& two) {
  if (two.size() > one.size())
    return false;

  return one.compare(one.size() - two.size(), two.size(), two) == 0;
}

}  // namespace

ChromeClassTester::ChromeClassTester(CompilerInstance& instance,
                                     const Options& options)
    : options_(options),
      instance_(instance),
      diagnostic_(instance.getDiagnostics()) {
  BuildBannedLists();
}

ChromeClassTester::~ChromeClassTester() {}

void ChromeClassTester::CheckTag(TagDecl* tag) {
  // We handle class types here where we have semantic information. We can only
  // check structs/classes/enums here, but we get a bunch of nice semantic
  // information instead of just parsing information.
  SourceLocation location = tag->getInnerLocStart();
  LocationType location_type = ClassifyLocation(location);
  if (location_type == LocationType::kThirdParty)
    return;

  if (CXXRecordDecl* record = dyn_cast<CXXRecordDecl>(tag)) {
    // We sadly need to maintain a blacklist of types that violate these
    // rules, but do so for good reason or due to limitations of this
    // checker (i.e., we don't handle extern templates very well).
    std::string base_name = record->getNameAsString();
    if (IsIgnoredType(base_name))
      return;

    CheckChromeClass(location_type, location, record);
  }
}

ChromeClassTester::LocationType ChromeClassTester::ClassifyLocation(
    SourceLocation loc) {
  if (instance().getSourceManager().isInSystemHeader(loc))
    return LocationType::kThirdParty;

  std::string filename = GetFilename(instance(), loc);
  if (filename.empty()) {
    // If the filename cannot be determined, simply treat this as a banned
    // location, instead of going through the full lookup process.
    return LocationType::kThirdParty;
  }

  // We need to special case scratch space; which is where clang does its
  // macro expansion. We explicitly want to allow people to do otherwise bad
  // things through macros that were defined due to third party libraries.
  if (filename == "<scratch space>")
    return LocationType::kThirdParty;

  // Ensure that we can search for patterns of the form "/foo/" even
  // if we have a relative path like "foo/bar.cc".  We don't expect
  // this transformed path to exist necessarily.
  if (filename.front() != '/') {
    filename.insert(0, 1, '/');
  }

  // When using distributed cross compilation build tools, file paths can have
  // separators which differ from ones at this platform. Make them consistent.
  std::replace(filename.begin(), filename.end(), '\\', '/');

  // Don't check autogenerated files. ninja puts them in $OUT_DIR/gen.
  if (filename.find("/gen/") != std::string::npos)
    return LocationType::kThirdParty;

  if (filename.find("/third_party/blink/") != std::string::npos &&
      // Browser-side code should always use the full range of checks.
      filename.find("/third_party/blink/browser/") == std::string::npos) {
    return LocationType::kBlink;
  }

  for (const std::string& banned_dir : banned_directories_) {
    // If any of the banned directories occur as a component in filename,
    // this file is rejected.
    assert(banned_dir.front() == '/' && "Banned dir must start with '/'");
    assert(banned_dir.back() == '/' && "Banned dir must end with '/'");

    if (filename.find(banned_dir) != std::string::npos)
      return LocationType::kThirdParty;
  }

  return LocationType::kChrome;
}

bool ChromeClassTester::HasIgnoredBases(const CXXRecordDecl* record) {
  for (const auto& base : record->bases()) {
    CXXRecordDecl* base_record = base.getType()->getAsCXXRecordDecl();
    if (!base_record)
      continue;

    const std::string& base_name = base_record->getQualifiedNameAsString();
    if (ignored_base_classes_.count(base_name) > 0)
      return true;
    if (HasIgnoredBases(base_record))
      return true;
  }
  return false;
}

bool ChromeClassTester::InImplementationFile(SourceLocation record_location) {
  std::string filename;

  // If |record_location| is a macro, check the whole chain of expansions.
  const SourceManager& source_manager = instance_.getSourceManager();
  while (true) {
    filename = GetFilename(instance(), record_location);
    if (ends_with(filename, ".cc") || ends_with(filename, ".cpp") ||
        ends_with(filename, ".mm")) {
      return true;
    }
    if (!record_location.isMacroID()) {
      break;
    }
    record_location =
        source_manager.getImmediateExpansionRange(record_location).getBegin();
  }

  return false;
}

void ChromeClassTester::BuildBannedLists() {
  banned_directories_.emplace("/third_party/");
  banned_directories_.emplace("/native_client/");
  banned_directories_.emplace("/breakpad/");
  banned_directories_.emplace("/courgette/");
  banned_directories_.emplace("/ppapi/");
  banned_directories_.emplace("/testing/");
  banned_directories_.emplace("/v8/");
  banned_directories_.emplace("/frameworks/");

  // Used in really low level threading code that probably shouldn't be out of
  // lined.
  ignored_record_names_.emplace("ThreadLocalBoolean");

  // A complicated pickle derived struct that is all packed integers.
  ignored_record_names_.emplace("Header");

  // Part of the GPU system that uses multiple included header
  // weirdness. Never getting this right.
  ignored_record_names_.emplace("Validators");

  // Has a UNIT_TEST only constructor. Isn't *terribly* complex...
  ignored_record_names_.emplace("AutocompleteController");
  ignored_record_names_.emplace("HistoryURLProvider");

  // Used over in the net unittests. A large enough bundle of integers with 1
  // non-pod class member. Probably harmless.
  ignored_record_names_.emplace("MockTransaction");

  // Used heavily in ui_base_unittests and once in views_unittests. Fixing this
  // isn't worth the overhead of an additional library.
  ignored_record_names_.emplace("TestAnimationDelegate");

  // Part of our public interface that nacl and friends use. (Arguably, this
  // should mean that this is a higher priority but fixing this looks hard.)
  ignored_record_names_.emplace("PluginVersionInfo");

  // Measured performance improvement on cc_perftests. See
  // https://codereview.chromium.org/11299290/
  ignored_record_names_.emplace("QuadF");

  // Ignore IPC::NoParams bases, since these structs are generated via
  // macros and it makes it difficult to add explicit ctors.
  ignored_base_classes_.emplace("IPC::NoParams");
}

bool ChromeClassTester::IsIgnoredType(const std::string& base_name) {
  return ignored_record_names_.find(base_name) != ignored_record_names_.end();
}

DiagnosticsEngine::Level ChromeClassTester::getErrorLevel() {
  return diagnostic().getWarningsAsErrors() ? DiagnosticsEngine::Error
                                            : DiagnosticsEngine::Warning;
}
