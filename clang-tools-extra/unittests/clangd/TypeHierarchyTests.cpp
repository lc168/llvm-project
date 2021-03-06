//===-- TypeHierarchyTests.cpp  ---------------------------*- C++ -*-------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Annotations.h"
#include "ClangdUnit.h"
#include "Compiler.h"
#include "Matchers.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "TestTU.h"
#include "XRefs.h"
#include "index/FileIndex.h"
#include "index/SymbolCollector.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Index/IndexingAction.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ScopedPrinter.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang {
namespace clangd {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;
using testing::IsEmpty;
using testing::Matcher;
using testing::Pointee;
using testing::UnorderedElementsAreArray;

// GMock helpers for matching TypeHierarchyItem.
MATCHER_P(WithName, N, "") { return arg.name == N; }
MATCHER_P(WithKind, Kind, "") { return arg.kind == Kind; }
MATCHER_P(SelectionRangeIs, R, "") { return arg.selectionRange == R; }
template <class... ParentMatchers>
testing::Matcher<TypeHierarchyItem> Parents(ParentMatchers... ParentsM) {
  return Field(&TypeHierarchyItem::parents, HasValue(ElementsAre(ParentsM...)));
}

TEST(FindRecordTypeAt, TypeOrVariable) {
  Annotations Source(R"cpp(
struct Ch^ild2 {
  int c;
};

int main() {
  Ch^ild2 ch^ild2;
  ch^ild2.c = 1;
}
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  for (Position Pt : Source.points()) {
    const CXXRecordDecl *RD = findRecordTypeAt(AST, Pt);
    EXPECT_EQ(&findDecl(AST, "Child2"), static_cast<const NamedDecl *>(RD));
  }
}

TEST(FindRecordTypeAt, Method) {
  Annotations Source(R"cpp(
struct Child2 {
  void met^hod ();
  void met^hod (int x);
};

int main() {
  Child2 child2;
  child2.met^hod(5);
}
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  for (Position Pt : Source.points()) {
    const CXXRecordDecl *RD = findRecordTypeAt(AST, Pt);
    EXPECT_EQ(&findDecl(AST, "Child2"), static_cast<const NamedDecl *>(RD));
  }
}

TEST(FindRecordTypeAt, Field) {
  Annotations Source(R"cpp(
struct Child2 {
  int fi^eld;
};

int main() {
  Child2 child2;
  child2.fi^eld = 5;
}
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  for (Position Pt : Source.points()) {
    const CXXRecordDecl *RD = findRecordTypeAt(AST, Pt);
    // A field does not unambiguously specify a record type
    // (possible associated reocrd types could be the field's type,
    // or the type of the record that the field is a member of).
    EXPECT_EQ(nullptr, RD);
  }
}

TEST(TypeParents, SimpleInheritance) {
  Annotations Source(R"cpp(
struct Parent {
  int a;
};

struct Child1 : Parent {
  int b;
};

struct Child2 : Child1 {
  int c;
};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent"));
  const CXXRecordDecl *Child1 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Child1"));
  const CXXRecordDecl *Child2 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Child2"));

  EXPECT_THAT(typeParents(Parent), ElementsAre());
  EXPECT_THAT(typeParents(Child1), ElementsAre(Parent));
  EXPECT_THAT(typeParents(Child2), ElementsAre(Child1));
}

TEST(TypeParents, MultipleInheritance) {
  Annotations Source(R"cpp(
struct Parent1 {
  int a;
};

struct Parent2 {
  int b;
};

struct Parent3 : Parent2 {
  int c;
};

struct Child : Parent1, Parent3 {
  int d;
};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent1 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent1"));
  const CXXRecordDecl *Parent2 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent2"));
  const CXXRecordDecl *Parent3 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent3"));
  const CXXRecordDecl *Child = dyn_cast<CXXRecordDecl>(&findDecl(AST, "Child"));

  EXPECT_THAT(typeParents(Parent1), ElementsAre());
  EXPECT_THAT(typeParents(Parent2), ElementsAre());
  EXPECT_THAT(typeParents(Parent3), ElementsAre(Parent2));
  EXPECT_THAT(typeParents(Child), ElementsAre(Parent1, Parent3));
}

TEST(TypeParents, ClassTemplate) {
  Annotations Source(R"cpp(
struct Parent {};

template <typename T>
struct Child : Parent {};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent"));
  const CXXRecordDecl *Child =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Child"))->getTemplatedDecl();

  EXPECT_THAT(typeParents(Child), ElementsAre(Parent));
}

MATCHER_P(ImplicitSpecOf, ClassTemplate, "") {
  const ClassTemplateSpecializationDecl *CTS =
      dyn_cast<ClassTemplateSpecializationDecl>(arg);
  return CTS &&
         CTS->getSpecializedTemplate()->getTemplatedDecl() == ClassTemplate &&
         CTS->getSpecializationKind() == TSK_ImplicitInstantiation;
}

// This is similar to findDecl(AST, QName), but supports using
// a template-id as a query.
const NamedDecl &findDeclWithTemplateArgs(ParsedAST &AST,
                                          llvm::StringRef Query) {
  return findDecl(AST, [&Query](const NamedDecl &ND) {
    std::string QName;
    llvm::raw_string_ostream OS(QName);
    PrintingPolicy Policy(ND.getASTContext().getLangOpts());
    // Use getNameForDiagnostic() which includes the template
    // arguments in the printed name.
    ND.getNameForDiagnostic(OS, Policy, /*Qualified=*/true);
    OS.flush();
    return QName == Query;
  });
}

TEST(TypeParents, TemplateSpec1) {
  Annotations Source(R"cpp(
template <typename T>
struct Parent {};

template <>
struct Parent<int> {};

struct Child1 : Parent<float> {};

struct Child2 : Parent<int> {};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Parent"))->getTemplatedDecl();
  const CXXRecordDecl *ParentSpec =
      dyn_cast<CXXRecordDecl>(&findDeclWithTemplateArgs(AST, "Parent<int>"));
  const CXXRecordDecl *Child1 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Child1"));
  const CXXRecordDecl *Child2 =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Child2"));

  EXPECT_THAT(typeParents(Child1), ElementsAre(ImplicitSpecOf(Parent)));
  EXPECT_THAT(typeParents(Child2), ElementsAre(ParentSpec));
}

TEST(TypeParents, TemplateSpec2) {
  Annotations Source(R"cpp(
struct Parent {};

template <typename T>
struct Child {};

template <>
struct Child<int> : Parent {};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent =
      dyn_cast<CXXRecordDecl>(&findDecl(AST, "Parent"));
  const CXXRecordDecl *Child =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Child"))->getTemplatedDecl();
  const CXXRecordDecl *ChildSpec =
      dyn_cast<CXXRecordDecl>(&findDeclWithTemplateArgs(AST, "Child<int>"));

  EXPECT_THAT(typeParents(Child), ElementsAre());
  EXPECT_THAT(typeParents(ChildSpec), ElementsAre(Parent));
}

// This is disabled for now, because support for dependent bases
// requires additional measures to avoid infinite recursion.
TEST(DISABLED_TypeParents, DependentBase) {
  Annotations Source(R"cpp(
template <typename T>
struct Parent {};

template <typename T>
struct Child1 : Parent<T> {};

template <typename T>
struct Child2 : Parent<T>::Type {};

template <typename T>
struct Child3 : T {};
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  const CXXRecordDecl *Parent =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Parent"))->getTemplatedDecl();
  const CXXRecordDecl *Child1 =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Child1"))->getTemplatedDecl();
  const CXXRecordDecl *Child2 =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Child2"))->getTemplatedDecl();
  const CXXRecordDecl *Child3 =
      dyn_cast<ClassTemplateDecl>(&findDecl(AST, "Child3"))->getTemplatedDecl();

  // For "Parent<T>", use the primary template as a best-effort guess.
  EXPECT_THAT(typeParents(Child1), ElementsAre(Parent));
  // For "Parent<T>::Type", there is nothing we can do.
  EXPECT_THAT(typeParents(Child2), ElementsAre());
  // Likewise for "T".
  EXPECT_THAT(typeParents(Child3), ElementsAre());
}

// Parts of getTypeHierarchy() are tested in more detail by the
// FindRecordTypeAt.* and TypeParents.* tests above. This test exercises the
// entire operation.
TEST(TypeHierarchy, Parents) {
  Annotations Source(R"cpp(
struct $Parent1Def[[Parent1]] {
  int a;
};

struct $Parent2Def[[Parent2]] {
  int b;
};

struct $Parent3Def[[Parent3]] : Parent2 {
  int c;
};

struct Ch^ild : Parent1, Parent3 {
  int d;
};

int main() {
  Ch^ild  ch^ild;

  ch^ild.a = 1;
}
)cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  for (Position Pt : Source.points()) {
    // Set ResolveLevels to 0 because it's only used for Children;
    // for Parents, getTypeHierarchy() always returns all levels.
    llvm::Optional<TypeHierarchyItem> Result = getTypeHierarchy(
        AST, Pt, /*ResolveLevels=*/0, TypeHierarchyDirection::Parents);
    ASSERT_TRUE(bool(Result));
    EXPECT_THAT(
        *Result,
        AllOf(
            WithName("Child"), WithKind(SymbolKind::Struct),
            Parents(AllOf(WithName("Parent1"), WithKind(SymbolKind::Struct),
                          SelectionRangeIs(Source.range("Parent1Def")),
                          Parents()),
                    AllOf(WithName("Parent3"), WithKind(SymbolKind::Struct),
                          SelectionRangeIs(Source.range("Parent3Def")),
                          Parents(AllOf(
                              WithName("Parent2"), WithKind(SymbolKind::Struct),
                              SelectionRangeIs(Source.range("Parent2Def")),
                              Parents()))))));
  }
}

TEST(TypeHierarchy, RecursiveHierarchy1) {
  Annotations Source(R"cpp(
  template <int N>
  struct S : S<N + 1> {};

  S^<0> s;
  )cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  // The compiler should produce a diagnostic for hitting the
  // template instantiation depth.
  ASSERT_TRUE(!AST.getDiagnostics().empty());

  // Make sure getTypeHierarchy() doesn't get into an infinite recursion.
  llvm::Optional<TypeHierarchyItem> Result = getTypeHierarchy(
      AST, Source.points()[0], 0, TypeHierarchyDirection::Parents);
  ASSERT_TRUE(bool(Result));
  EXPECT_THAT(*Result,
              AllOf(WithName("S"), WithKind(SymbolKind::Struct), Parents()));
}

TEST(TypeHierarchy, RecursiveHierarchy2) {
  Annotations Source(R"cpp(
  template <int N>
  struct S : S<N - 1> {};

  template <>
  struct S<0>{};

  S^<2> s;
  )cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  // Make sure getTypeHierarchy() doesn't get into an infinite recursion.
  llvm::Optional<TypeHierarchyItem> Result = getTypeHierarchy(
      AST, Source.points()[0], 0, TypeHierarchyDirection::Parents);
  ASSERT_TRUE(bool(Result));
  EXPECT_THAT(*Result,
              AllOf(WithName("S"), WithKind(SymbolKind::Struct), Parents()));
}

TEST(TypeHierarchy, RecursiveHierarchy3) {
  Annotations Source(R"cpp(
  template <int N>
  struct S : S<N - 1> {};

  template <>
  struct S<0>{};

  template <int N>
  struct Foo {
    S^<N> s;
  };
  )cpp");

  TestTU TU = TestTU::withCode(Source.code());
  auto AST = TU.build();

  ASSERT_TRUE(AST.getDiagnostics().empty());

  // Make sure getTypeHierarchy() doesn't get into an infinite recursion.
  llvm::Optional<TypeHierarchyItem> Result = getTypeHierarchy(
      AST, Source.points()[0], 0, TypeHierarchyDirection::Parents);
  ASSERT_TRUE(bool(Result));
  EXPECT_THAT(*Result,
              AllOf(WithName("S"), WithKind(SymbolKind::Struct), Parents()));
}

} // namespace
} // namespace clangd
} // namespace clang
