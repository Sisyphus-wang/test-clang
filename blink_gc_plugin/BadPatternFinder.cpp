// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "BadPatternFinder.h"
#include <clang/AST/Decl.h>
#include "BlinkGCPluginOptions.h"
#include "Config.h"
#include "DiagnosticsReporter.h"

#include <algorithm>
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"

using namespace clang::ast_matchers;

namespace {

TypeMatcher GarbageCollectedType() {
  auto has_gc_base = hasCanonicalType(hasDeclaration(
      cxxRecordDecl(isDerivedFrom(hasAnyName("::blink::GarbageCollected",
                                             "::blink::GarbageCollectedMixin",
                                             "::cppgc::GarbageCollected",
                                             "::cppgc::GarbageCollectedMixin")))
          .bind("gctype")));
  return anyOf(has_gc_base,
               hasCanonicalType(arrayType(hasElementType(has_gc_base))));
}

class UniquePtrGarbageCollectedMatcher : public MatchFinder::MatchCallback {
 public:
  explicit UniquePtrGarbageCollectedMatcher(DiagnosticsReporter& diagnostics)
      : diagnostics_(diagnostics) {}

  void Register(MatchFinder& match_finder) {
    // Matches any application of make_unique where the template argument is
    // known to refer to a garbage-collected type.
    auto make_unique_matcher =
        callExpr(
            callee(functionDecl(
                       hasAnyName("::std::make_unique", "::base::WrapUnique"),
                       hasTemplateArgument(
                           0, refersToType(GarbageCollectedType())))
                       .bind("badfunc")))
            .bind("bad");
    match_finder.addDynamicMatcher(make_unique_matcher, this);
  }

  void run(const MatchFinder::MatchResult& result) override {
    auto* bad_use = result.Nodes.getNodeAs<clang::Expr>("bad");
    auto* bad_function = result.Nodes.getNodeAs<clang::FunctionDecl>("badfunc");
    auto* gc_type = result.Nodes.getNodeAs<clang::CXXRecordDecl>("gctype");
    diagnostics_.UniquePtrUsedWithGC(bad_use, bad_function, gc_type);
  }

 private:
  DiagnosticsReporter& diagnostics_;
};

class OptionalGarbageCollectedMatcher : public MatchFinder::MatchCallback {
 public:
  explicit OptionalGarbageCollectedMatcher(DiagnosticsReporter& diagnostics)
      : diagnostics_(diagnostics) {}

  void Register(MatchFinder& match_finder) {
    // Matches fields and new-expressions of type absl::optional where the
    // template argument is known to refer to a garbage-collected type.
    auto optional_type = hasType(
        classTemplateSpecializationDecl(
            hasName("::absl::optional"),
            hasTemplateArgument(0, refersToType(GarbageCollectedType())))
            .bind("optional"));
    auto optional_field = fieldDecl(optional_type).bind("bad_field");
    auto optional_new_expression =
        cxxNewExpr(has(cxxConstructExpr(optional_type))).bind("bad_new");
    match_finder.addDynamicMatcher(optional_field, this);
    match_finder.addDynamicMatcher(optional_new_expression, this);
  }

  void run(const MatchFinder::MatchResult& result) override {
    auto* optional = result.Nodes.getNodeAs<clang::CXXRecordDecl>("optional");
    auto* gc_type = result.Nodes.getNodeAs<clang::CXXRecordDecl>("gctype");
    if (auto* bad_field =
            result.Nodes.getNodeAs<clang::FieldDecl>("bad_field")) {
      diagnostics_.OptionalFieldUsedWithGC(bad_field, optional, gc_type);
    } else {
      auto* bad_new = result.Nodes.getNodeAs<clang::Expr>("bad_new");
      diagnostics_.OptionalNewExprUsedWithGC(bad_new, optional, gc_type);
    }
  }

 private:
  DiagnosticsReporter& diagnostics_;
};

// For the absl::variant checker, we need to match the inside of a variadic
// template class, which doesn't seem easy with the built-in matchers: define a
// custom matcher to go through the template parameter list.
AST_MATCHER_P(clang::TemplateArgument,
              parameterPackHasAnyElement,
              // Clang exports other instantiations of Matcher via
              // using-declarations in public headers, e.g. `using TypeMatcher =
              // Matcher<QualType>`.
              //
              // Once https://reviews.llvm.org/D89920, a Clang patch adding a
              // similar alias for template arguments, lands, this can be
              // changed to TemplateArgumentMatcher and won't need to use the
              // internal namespace any longer.
              clang::ast_matchers::internal::Matcher<clang::TemplateArgument>,
              InnerMatcher) {
  if (Node.getKind() != clang::TemplateArgument::Pack)
    return false;
  return llvm::any_of(Node.pack_elements(),
                      [&](const clang::TemplateArgument& Arg) {
                        return InnerMatcher.matches(Arg, Finder, Builder);
                      });
}

class VariantGarbageCollectedMatcher : public MatchFinder::MatchCallback {
 public:
  explicit VariantGarbageCollectedMatcher(DiagnosticsReporter& diagnostics)
      : diagnostics_(diagnostics) {}

  void Register(MatchFinder& match_finder) {
    // Matches any constructed absl::variant where a template argument is
    // known to refer to a garbage-collected type.
    auto variant_construction =
        cxxConstructExpr(
            hasDeclaration(cxxConstructorDecl(
                ofClass(classTemplateSpecializationDecl(
                            hasName("::absl::variant"),
                            hasAnyTemplateArgument(parameterPackHasAnyElement(
                                refersToType(GarbageCollectedType()))))
                            .bind("variant")))))
            .bind("bad");
    match_finder.addDynamicMatcher(variant_construction, this);
  }

  void run(const MatchFinder::MatchResult& result) override {
    auto* bad_use = result.Nodes.getNodeAs<clang::Expr>("bad");
    auto* variant = result.Nodes.getNodeAs<clang::CXXRecordDecl>("variant");
    auto* gc_type = result.Nodes.getNodeAs<clang::CXXRecordDecl>("gctype");
    diagnostics_.VariantUsedWithGC(bad_use, variant, gc_type);
  }

 private:
  DiagnosticsReporter& diagnostics_;
};

class MemberOnStackMatcher : public MatchFinder::MatchCallback {
 public:
  explicit MemberOnStackMatcher(DiagnosticsReporter& diagnostics)
      : diagnostics_(diagnostics) {}

  void Register(MatchFinder& match_finder) {
    auto has_member_name = hasAnyName("::blink::Member", "::blink::WeakMember",
                                      "::cppgc::internal::BasicMember");
    auto class_member_variable_matcher =
        varDecl(anyOf(hasType(recordDecl(has_member_name)),
                      hasType(typeAliasTemplateDecl(has_member_name))))
            .bind("member");
    match_finder.addDynamicMatcher(class_member_variable_matcher, this);
  }

  void run(const MatchFinder::MatchResult& result) override {
    auto* member = result.Nodes.getNodeAs<clang::VarDecl>("member");
    if (Config::IsIgnoreAnnotated(member))
      return;
    diagnostics_.MemberOnStack(member);
  }

 private:
  DiagnosticsReporter& diagnostics_;
};

}  // namespace

void FindBadPatterns(clang::ASTContext& ast_context,
                     DiagnosticsReporter& diagnostics,
                     const BlinkGCPluginOptions& options) {
  MatchFinder match_finder;

  UniquePtrGarbageCollectedMatcher unique_ptr_gc(diagnostics);
  unique_ptr_gc.Register(match_finder);

  OptionalGarbageCollectedMatcher optional_gc(diagnostics);
  optional_gc.Register(match_finder);

  VariantGarbageCollectedMatcher variant_gc(diagnostics);
  variant_gc.Register(match_finder);

  MemberOnStackMatcher member_on_stack(diagnostics);
  if (options.enable_members_on_stack_check) {
    member_on_stack.Register(match_finder);
  }

  match_finder.matchAST(ast_context);
}
