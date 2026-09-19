//===- CheckEmitterTest.cpp - Tests for the check rewrites ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §10.6 and §11: `CheckEmitter` over small units. Each unit runs the
// unit pipeline (sites, the engine's defaults, the planner), optionally has
// its plan edited to reach placements and forms the engine of this stage
// does not plan yet, and is rewritten through Sema; the tests read the
// rewritten bodies back (pretty-printed) and, where the code generator must
// accept the result, its IR.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/CheckEmitter.h"

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/UnitPipeline.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/DeferredCodeGenConsumer.h"
#include "weavec/Frontend/Prelude.h"
#include "weavec/Frontend/ZeroInit.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {

namespace {

using Entry = core::CheckPlanEntry;
/// Changes a unit's plan before it is emitted.
using PlanEdit =
    std::function<void(analysis::PlannedLedger &, clang::ASTContext &)>;

/// What one emission produced.
struct Emitted {
  bool ok = true;
  std::size_t inserted = 0;
  std::size_t lowered = 0;
  /// The main file's function definitions, pretty-printed on one line each.
  std::string bodies;
  /// The error diagnostics, in order.
  std::vector<std::string> errors;
  /// The module, when the unit was compiled.
  std::string ir;
};

struct EmitOptions {
  core::ChecksMode mode = core::ChecksMode::Trap;
  /// Include the inline prelude, as a system header.
  bool prelude = true;
  /// §10.9: declare the helpers `extern` instead.
  bool externalHelpers = false;
  bool zeroInit = false;
  PlanEdit edit = nullptr;
};

/// Records error diagnostics.
class Errors final : public clang::DiagnosticConsumer {
public:
  explicit Errors(std::vector<std::string> &out) : out(out) {}
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level < clang::DiagnosticsEngine::Error)
      return;
    llvm::SmallString<128> text;
    info.FormatDiagnostic(text);
    out.emplace_back(text.str());
  }

private:
  std::vector<std::string> &out;
};

/// A body on one line: `{ return *p; }`.
std::string oneLine(const clang::Stmt &body, const clang::ASTContext &context) {
  std::string text;
  llvm::raw_string_ostream os(text);
  body.printPretty(os, nullptr, context.getPrintingPolicy());
  std::string flat;
  bool space = false;
  for (const char c : text) {
    if (c == '\n' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !flat.empty())
      flat += ' ';
    space = false;
    flat += c;
  }
  return flat;
}

/// The analysis, the plan edit and the rewrites, at the end of the unit.
void emitUnit(clang::ASTContext &context, clang::Sema &sema,
              const EmitOptions &setup, Emitted &out) {
  analysis::UnitPipelineOptions pipeline;
  pipeline.config.checks = setup.mode;
  pipeline.config.zeroInit = setup.zeroInit;
  core::DiagnosticCollector ignored;
  analysis::UnitPipelineResult unit =
      analysis::runUnitAnalysis(context, pipeline, ignored);
  if (setup.edit)
    setup.edit(*unit.ledger, context);
  CheckEmitter emitter(
      sema, CheckEmitterOptions{.mode = setup.mode,
                                .externalHelpers = setup.externalHelpers});
  out.ok = emitter.emit(*unit.ledger);
  if (setup.zeroInit) {
    const ZeroInitPlan plan =
        planZeroInit(context, core::LibrarySpec::shipped(), true);
    out.ok = emitter.lowerZeroInit(plan) && out.ok;
  }
  out.inserted = emitter.checksInserted();
  out.lowered = emitter.zeroInitRewrites();
  const clang::SourceManager &sm = context.getSourceManager();
  for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls()) {
    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (function == nullptr || !function->doesThisDeclarationHaveABody() ||
        !sm.isInMainFile(function->getLocation()))
      continue;
    out.bodies += function->getNameAsString() + " " +
                  oneLine(*function->getBody(), context) + "\n";
  }
}

/// Runs `emitUnit` from a Sema consumer; the unit is not compiled.
class EmitConsumer final : public clang::SemaConsumer {
public:
  EmitConsumer(const EmitOptions &setup, Emitted &out)
      : setup(setup), out(out) {}
  void InitializeSema(clang::Sema &s) override { sema = &s; }
  void ForgetSema() override { sema = nullptr; }
  void HandleTranslationUnit(clang::ASTContext &context) override {
    if (sema != nullptr)
      emitUnit(context, *sema, setup, out);
  }

private:
  const EmitOptions &setup;
  Emitted &out;
  clang::Sema *sema = nullptr;
};

/// Records the errors of the unit.
template <typename Base>
class Recording : public Base {
public:
  template <typename... Args>
  explicit Recording(Emitted &out, Args &&...args)
      : Base(std::forward<Args>(args)...), out(out) {}

protected:
  bool BeginSourceFileAction(clang::CompilerInstance &compiler) override {
    errors = std::make_unique<Errors>(out.errors);
    compiler.getDiagnostics().setClient(errors.get(), false);
    return Base::BeginSourceFileAction(compiler);
  }
  Emitted &out;

private:
  std::unique_ptr<Errors> errors;
};

class EmitAction final : public Recording<clang::ASTFrontendAction> {
public:
  EmitAction(Emitted &out, const EmitOptions &setup)
      : Recording(out), setup(setup) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*compiler*/,
                    llvm::StringRef /*inFile*/) override {
    return std::make_unique<EmitConsumer>(setup, out);
  }

private:
  const EmitOptions &setup;
};

/// Compiles the unit to IR, with the rewrites (when `setup` is given)
/// between the analysis and the code generator, as `weavec-cc` does.
class CompileAction final : public Recording<clang::WrapperFrontendAction> {
public:
  CompileAction(Emitted &out, const EmitOptions *setup)
      : Recording(out, std::make_unique<clang::EmitLLVMOnlyAction>()),
        setup(setup) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    auto codeGen = WrapperFrontendAction::CreateASTConsumer(compiler, inFile);
    if (setup == nullptr)
      return codeGen;
    return std::make_unique<DeferredCodeGenConsumer>(
        std::move(codeGen),
        [this](clang::ASTContext &context, clang::Sema &sema) {
          emitUnit(context, sema, *setup, out);
        });
  }
  // The wrapper forwards EndSourceFile itself; the code generator's module
  // is ready after it.
  void EndSourceFile() override {
    WrapperFrontendAction::EndSourceFile();
    auto *codeGen = static_cast<clang::CodeGenAction *>(wrapped());
    if (std::unique_ptr<llvm::Module> module = codeGen->takeModule()) {
      llvm::raw_string_ostream os(out.ir);
      module->print(os, nullptr);
    }
  }

private:
  const EmitOptions *setup;
  clang::FrontendAction *wrapped() { return WrappedAction.get(); }
};

constexpr llvm::StringLiteral PreludeDir = "/weavec-test/include";

std::vector<std::string> argumentsFor(const EmitOptions &setup) {
  std::vector<std::string> args{"-std=c11", "-target",
                                "arm64-apple-macosx14.0.0", "-Wno-everything"};
  if (setup.prelude) {
    args.insert(args.end(),
                {"-isystem", PreludeDir.str(), "-include", "weavec_prelude.h"});
  }
  if (setup.zeroInit)
    args.emplace_back("-ftrivial-auto-var-init=zero");
  return args;
}

clang::tooling::FileContentMappings preludeFiles(const EmitOptions &setup) {
  PreludeOptions options;
  switch (setup.mode) {
  case core::ChecksMode::Report:
    options.mode = CheckMode::Report;
    break;
  case core::ChecksMode::Verify:
    options.mode = CheckMode::Verify;
    break;
  default:
    options.mode = CheckMode::Trap;
    break;
  }
  options.zeroInit = setup.zeroInit;
  options.usableSize = UsableSizeQuery::MallocSize;
  return {
      {(PreludeDir + "/weavec_prelude.h").str(), buildCheckPrelude(options)}};
}

/// Rewrites `code` and reads the bodies back.
Emitted rewrite(llvm::StringRef code, const EmitOptions &setup = {}) {
  Emitted out;
  EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
                  std::make_unique<EmitAction>(out, setup), code,
                  argumentsFor(setup), "unit.c", "weavec-test",
                  std::make_shared<clang::PCHContainerOperations>(),
                  preludeFiles(setup)) ||
              !out.errors.empty());
  return out;
}

/// Compiles `code` to IR; with `setup`, through the rewrites.
Emitted compile(llvm::StringRef code, const EmitOptions *setup,
                const EmitOptions &flags = {}) {
  Emitted out;
  const EmitOptions &used = setup != nullptr ? *setup : flags;
  clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<CompileAction>(out, setup), code, argumentsFor(used),
      "unit.c", "weavec-test",
      std::make_shared<clang::PCHContainerOperations>(), preludeFiles(used));
  return out;
}

/// The IR without the lines that name the source.
std::string comparable(const std::string &ir) {
  std::string out;
  llvm::SmallVector<llvm::StringRef, 128> lines;
  llvm::StringRef(ir).split(lines, '\n');
  for (const llvm::StringRef line : lines)
    if (!line.starts_with("; ModuleID") && !line.starts_with("source_filename"))
      out += line.str() + "\n";
  return out;
}

std::size_t count(llvm::StringRef text, llvm::StringRef needle) {
  std::size_t found = 0;
  for (std::size_t at = text.find(needle); at != llvm::StringRef::npos;
       at = text.find(needle, at + needle.size()))
    ++found;
  return found;
}

/// The planned entries of the site whose statement starts with `text`.
const analysis::SiteInfo *siteAt(const analysis::PlannedLedger &planned,
                                 clang::ASTContext &context,
                                 llvm::StringRef text, core::SiteKind kind) {
  const clang::SourceManager &sm = context.getSourceManager();
  for (const auto &function : planned.sites->functions())
    for (const analysis::SiteInfo &site : function.sites) {
      if (site.kind != kind || site.stmt == nullptr)
        continue;
      const llvm::StringRef source = clang::Lexer::getSourceText(
          clang::CharSourceRange::getTokenRange(site.stmt->getSourceRange()),
          sm, context.getLangOpts());
      if (source.starts_with(text))
        return &site;
    }
  return nullptr;
}

/// Replaces the planned checks of `site` with `entries`.
void replaceChecks(analysis::PlannedLedger &planned,
                   const analysis::SiteInfo &site, std::vector<Entry> entries) {
  std::erase_if(planned.plan.entries,
                [&](const Entry &entry) { return entry.site == site.id; });
  for (Entry &entry : entries) {
    entry.site = site.id;
    ASSERT_TRUE(core::isWellFormed(entry));
    planned.plan.add(std::move(entry));
  }
  planned.plan.sort();
}

Entry entry(Entry::Template kind, Entry::Form form, Entry::Placement placement,
            std::vector<core::CheckTerm> operands = {},
            std::uint8_t argument = 0) {
  Entry made;
  made.kind = kind;
  made.form = form;
  made.placement = placement;
  made.operands = std::move(operands);
  made.argument = argument;
  return made;
}

/// A place handle for the variable `name` of the unit.
core::CheckTerm placeOf(analysis::PlannedLedger &planned,
                        clang::ASTContext &context, llvm::StringRef name,
                        std::vector<core::CheckPathStep> path = {}) {
  const clang::ValueDecl *found = nullptr;
  for (const auto &function : planned.sites->functions()) {
    for (const clang::ParmVarDecl *param : function.decl->parameters())
      if (param->getName() == name)
        found = param;
    struct Finder : clang::RecursiveASTVisitor<Finder> {
      llvm::StringRef name;
      const clang::ValueDecl *found = nullptr;
      // NOLINTNEXTLINE(readability-identifier-naming)
      bool VisitVarDecl(clang::VarDecl *variable) {
        if (variable->getName() == name)
          found = variable;
        return true;
      }
    } finder;
    finder.name = name;
    finder.TraverseStmt(function.decl->getBody());
    if (finder.found != nullptr)
      found = finder.found;
  }
  EXPECT_NE(found, nullptr) << name.str();
  (void)context;
  return core::CheckTerm::ofPlace(
      found != nullptr ? planned.handles.place(*found) : 0, std::move(path));
}

core::CheckTerm sizeOf(analysis::PlannedLedger &planned, clang::QualType type) {
  return core::CheckTerm::sizeOf(planned.handles.type(type));
}

} // namespace

//===----------------------------------------------------------------------===//
// Signatures
//===----------------------------------------------------------------------===//

// §10.9: the `extern` declarations are built from a table, which must agree
// with the prelude the table stands for, in every mode and in both forms.
TEST(CheckEmitterTest, HelperSignaturesMatchThePrelude) {
  struct Form {
    CheckMode mode;
    PreludeForm form;
    bool report;
  };
  for (const Form form :
       {Form{CheckMode::Trap, PreludeForm::Inline, false},
        Form{CheckMode::Verify, PreludeForm::Inline, false},
        Form{CheckMode::Report, PreludeForm::Inline, true},
        Form{CheckMode::Verify, PreludeForm::OutOfLine, false},
        Form{CheckMode::Report, PreludeForm::OutOfLine, true}}) {
    PreludeOptions options;
    options.mode = form.mode;
    options.form = form.form;
    options.usableSize = UsableSizeQuery::MallocSize;
    std::string code = buildCheckPrelude(options);
    if (form.form == PreludeForm::OutOfLine)
      code = "#define WEAVEC_CHK_TRAP(c, r) __builtin_trap()\n"
             "#define WEAVEC_CHK_USABLE(p) 0\n"
             "extern void __weavec_rt_report(const char *, const char *, "
             "unsigned, unsigned);\n" +
             code;
    const std::unique_ptr<clang::ASTUnit> ast =
        clang::tooling::buildASTFromCodeWithArgs(
            code, {"-std=c11", "-target", "arm64-apple-macosx14.0.0"},
            "prelude.c");
    ASSERT_NE(ast, nullptr);
    clang::ASTContext &context = ast->getASTContext();
    std::size_t matched = 0;
    for (const HelperSignature &helper : CheckEmitter::helperSignatures()) {
      std::string name = helper.name.str();
      if (form.form == PreludeForm::OutOfLine && form.report && helper.reports)
        name += "_report";
      const auto found = context.getTranslationUnitDecl()->lookup(
          clang::DeclarationName(&context.Idents.get(name)));
      if (found.empty())
        continue;
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(found.front());
      ASSERT_NE(function, nullptr) << name;
      EXPECT_TRUE(
          context.hasSameType(function->getType(),
                              helperFunctionType(context, helper, form.report)))
          << name << " in " << checkModeName(form.mode).str();
      ++matched;
    }
    // Out of line, the report object has only the check helpers.
    EXPECT_GE(matched, 11U) << checkModeName(form.mode).str();
  }
  EXPECT_EQ(findHelperSignature("__weavec_chk_span")->params[4],
            HelperSignature::Type::UnsignedLongLong);
  EXPECT_EQ(findHelperSignature("__weavec_chk_bogus"), nullptr);
}

//===----------------------------------------------------------------------===//
// Placements (§10.4)
//===----------------------------------------------------------------------===//

// Each placement the planner uses at this stage, from the unit pipeline.
TEST(CheckEmitterTest, EveryPlacementKind) {
  const Emitted out = rewrite(R"C(
__attribute__((annotate("weavec.assume"))) static inline void
weavec_assume_(int c) { (void)c; }
#define WEAVEC_ASSUME(e) weavec_assume_((e) != 0)
unsigned long strlen(const char *);
void *memset(void *, int, unsigned long);
int sprintf(char *, const char *, ...);
int snprintf(char *, unsigned long, const char *, ...);
int deref(int *p) { return *p; }
int at(int i) { int a[10] = {0}; return a[i]; }
unsigned long argument(const char *s) { return strlen(s); }
int access(int n, int i) { int v[n]; v[0] = 0; return v[i]; }
void before(unsigned long n) { char b[8]; memset(b, 0, n); }
void assume(int n) { WEAVEC_ASSUME(n > 0); }
int print(int x) { char b[4]; return sprintf(b, "%d", x); }
)C");
  EXPECT_TRUE(out.ok);
  EXPECT_TRUE(out.errors.empty()) << out.errors.front();
  const std::string &text = out.bodies;
  // WrapOperand.
  EXPECT_NE(text.find("deref { return *__weavec_chk_nonnull((p)); }"),
            std::string::npos)
      << text;
  // WrapIndex.
  EXPECT_NE(text.find("return a[__weavec_chk_index((i), 10ULL)];"),
            std::string::npos)
      << text;
  // WrapArgument.
  EXPECT_NE(text.find("return strlen(__weavec_chk_nonnull((s)));"),
            std::string::npos)
      << text;
  // ReplaceAccess: `v[i]` becomes a dereference of the checked address.
  EXPECT_NE(text.find("return *__weavec_chk_span((v), (i), v, sizeof(int[n]), "
                      "sizeof(int));"),
            std::string::npos)
      << text;
  // BeforeCall.
  EXPECT_NE(
      text.find("__weavec_chk_len(n, sizeof(char[8])) , (memset(b, 0, n));"),
      std::string::npos)
      << text;
  // ReplaceCall, both uses.
  EXPECT_NE(text.find("__weavec_chk_assert(((n > 0) != 0));"),
            std::string::npos)
      << text;
  EXPECT_NE(
      text.find("return __weavec_chk_len_r(snprintf((b), sizeof(char[4]), "
                "(\"%d\"), (x)), sizeof(char[4]));"),
      std::string::npos)
      << text;
  EXPECT_EQ(out.inserted, 8U) << text;
}

// §10.6: stores, compound assignment, `++`, `&`, `->` and array decay all
// still compile after the rewrite; each access has its check.
TEST(CheckEmitterTest, LvalueContextsCompile) {
  const EmitOptions setup;
  const Emitted out = compile(R"C(
struct s { int f; char name[8]; };
int *contexts(int *p, struct s *q, int v) {
  *p = v;
  *p += 2;
  ++*p;
  (*p)--;
  q->f = v;
  char *n = q->name;
  (void)n;
  return &q->f;
}
)C",
                              &setup);
  EXPECT_TRUE(out.ok);
  EXPECT_TRUE(out.errors.empty()) << out.errors.front();
  EXPECT_EQ(out.inserted, 7U);
  EXPECT_EQ(count(out.ir, "call void @llvm.trap()"), 7U) << out.ir;
}

// Placements and forms the engine of this stage does not plan yet: span on
// an operand and on an argument, len on the argument that is the need,
// disjoint, a guarded check, a verify check of a proven facet, and terms
// with arithmetic, signed leaves, a member path and a bounded string length.
TEST(CheckEmitterTest, SyntheticPlacementsAndTerms) {
  EmitOptions setup;
  setup.mode = core::ChecksMode::Verify;
  setup.edit = [](analysis::PlannedLedger &planned,
                  clang::ASTContext &context) {
    using T = Entry::Template;
    using F = Entry::Form;
    using P = Entry::Placement;
    const clang::QualType intType = context.IntTy;
    const clang::QualType array =
        context.getConstantArrayType(intType, llvm::APInt(64, 4), nullptr,
                                     clang::ArraySizeModifier::Normal, 0);
    const auto *deref = siteAt(planned, context, "*p", core::SiteKind::Deref);
    const auto *memset =
        siteAt(planned, context, "memset", core::SiteKind::LibCall);
    const auto *copy =
        siteAt(planned, context, "memcpy", core::SiteKind::LibCall);
    const auto *index = siteAt(planned, context, "a[i]", core::SiteKind::Index);
    ASSERT_TRUE(deref && memset && copy && index);
    // span on the operand, against a local array (subsumes nonnull).
    replaceChecks(planned, *deref,
                  {entry(T::Span, F::Plain, P::WrapOperand,
                         {placeOf(planned, context, "arr"),
                          sizeOf(planned, array), sizeOf(planned, intType)})});
    // len on the need argument, guarded: only while `n - 2` is non-zero;
    // the need grows by one and the have is the counted field of `s`.
    Entry len = entry(
        T::Len, F::Plain, P::WrapArgument,
        {core::CheckTerm::sub(placeOf(planned, context, "s",
                                      {core::CheckPathStep::deref(),
                                       core::CheckPathStep::member("len")}),
                              core::CheckTerm::ofConstant(1))},
        2);
    Entry guarded =
        entry(T::Len, F::Plain, P::BeforeCall,
              {core::CheckTerm::add(placeOf(planned, context, "k"),
                                    core::CheckTerm::ofConstant(1)),
               core::CheckTerm::strnlen(placeOf(planned, context, "t"),
                                        core::CheckTerm::ofConstant(16))});
    guarded.guard = core::CheckTerm::sub(placeOf(planned, context, "k"),
                                         core::CheckTerm::ofConstant(2));
    replaceChecks(planned, *memset, {len, guarded});
    // disjoint and span on arguments.
    replaceChecks(planned, *copy,
                  {entry(T::Disjoint, F::Plain, P::WrapArgument,
                         {placeOf(planned, context, "arr"),
                          core::CheckTerm::mul(placeOf(planned, context, "k"),
                                               sizeOf(planned, intType))},
                         0),
                   entry(T::Span, F::Plain, P::WrapArgument,
                         {placeOf(planned, context, "arr"),
                          sizeOf(planned, array), sizeOf(planned, intType)},
                         1)});
    // A verify check of a proven facet.
    Entry proven = entry(T::Index, F::Plain, P::WrapIndex,
                         {core::CheckTerm::ofConstant(4)});
    proven.facet = core::Facet::Spatial;
    proven.proven = true;
    replaceChecks(planned, *index, {proven});
  };
  const char *code = R"C(
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
struct buf { int len; };
int f(int *p, int *q, int k, const char *t, struct buf *s, unsigned long n,
      int i) {
  int arr[4] = {0};
  int a[4] = {0};
  memset(q, 0, n);
  memcpy(q, arr, n);
  return *p + a[i];
}
)C";
  const Emitted out = rewrite(code, setup);
  EXPECT_TRUE(out.ok);
  EXPECT_TRUE(out.errors.empty()) << out.errors.front();
  const std::string &text = out.bodies;
  EXPECT_NE(text.find("*__weavec_chk_span((p), 0LL, arr, sizeof(int[4]), "
                      "sizeof(int))"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("__weavec_have_sub(__weavec_have_s(k), 2ULL) ? "
                      "__weavec_chk_len(__weavec_need_add(__weavec_need_s(k), "
                      "1ULL), __weavec_strnlen(t, 16ULL)) : 0"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("memset(q, 0, __weavec_chk_len((n), __weavec_have_sub("
                      "__weavec_have_s(*s.len), 1ULL)))"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("memcpy(__weavec_chk_disjoint((q), arr, "
                      "__weavec_need_mul(__weavec_need_s(k), sizeof(int))), "
                      "__weavec_chk_span((arr), 0LL, arr, sizeof(int[4]), "
                      "sizeof(int)), n)"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("a[__weavec_prv_index((i), 4ULL)]"), std::string::npos)
      << text;
  // And the code generator takes it.
  const Emitted compiled = compile(code, &setup);
  EXPECT_TRUE(compiled.errors.empty()) << compiled.errors.front();
  EXPECT_NE(compiled.ir.find("define"), std::string::npos);
}

// §3.4: the unconditional trap of a lowered violation, before the operation,
// in each placement the planner gives it.
TEST(CheckEmitterTest, LoweredViolationForms) {
  EmitOptions setup;
  setup.edit = [](analysis::PlannedLedger &planned,
                  clang::ASTContext &context) {
    const auto violation = [](Entry::Placement placement) {
      Entry made =
          entry(Entry::Template::Assert, Entry::Form::Violation, placement);
      made.facet = core::Facet::Temporal;
      return made;
    };
    const auto *deref = siteAt(planned, context, "*p", core::SiteKind::Deref);
    const auto *call = siteAt(planned, context, "g(p)", core::SiteKind::Call);
    const auto *assume =
        siteAt(planned, context, "weavec_assume_", core::SiteKind::Assume);
    ASSERT_TRUE(deref && call && assume);
    replaceChecks(planned, *deref, {violation(Entry::Placement::WrapOperand)});
    replaceChecks(planned, *call, {violation(Entry::Placement::BeforeCall)});
    replaceChecks(planned, *assume, {violation(Entry::Placement::ReplaceCall)});
    // The exits: a `return;` and the end of a body.
    for (const auto &function : planned.sites->functions())
      for (const analysis::SiteInfo &site : function.sites)
        if (site.kind == core::SiteKind::Call &&
            site.boundary == core::Boundary::Exit &&
            function.decl->getName() == "early")
          replaceChecks(planned, site,
                        {violation(Entry::Placement::BeforeCall)});
  };
  const char *code = R"C(
__attribute__((annotate("weavec.assume"))) static inline void
weavec_assume_(int c) { (void)c; }
void g(int *p);
int f(int *p, int n) { weavec_assume_(n > 0); g(p); return *p; }
void early(int c) { if (c) return; g(0); }
)C";
  const Emitted out = rewrite(code, setup);
  EXPECT_TRUE(out.ok);
  EXPECT_TRUE(out.errors.empty()) << out.errors.front();
  const std::string &text = out.bodies;
  EXPECT_NE(text.find("__weavec_chk_violation();"), std::string::npos) << text;
  EXPECT_NE(text.find("__weavec_chk_violation() , (g(p));"), std::string::npos)
      << text;
  EXPECT_NE(text.find("return *__weavec_chk_violation() , (p);"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("if (c) { __weavec_chk_violation(); return; }"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("g(0); __weavec_chk_violation(); }"), std::string::npos)
      << text;
  const Emitted compiled = compile(code, &setup);
  EXPECT_TRUE(compiled.errors.empty()) << compiled.errors.front();
}

// G8 for a lowered violation, in process: the engine of this stage does not
// publish violations, so the plan carries the planner's guard by hand; the
// IR must equal that of the hand-written rewrite.
TEST(CheckEmitterTest, LoweredViolationOracle) {
  EmitOptions setup;
  setup.edit = [](analysis::PlannedLedger &planned,
                  clang::ASTContext &context) {
    const auto *deref = siteAt(planned, context, "*p", core::SiteKind::Deref);
    const auto *release =
        siteAt(planned, context, "free", core::SiteKind::Release);
    ASSERT_TRUE(deref && release);
    Entry temporal = entry(Entry::Template::Assert, Entry::Form::Violation,
                           Entry::Placement::WrapOperand);
    temporal.facet = core::Facet::Temporal;
    Entry nonnull = entry(Entry::Template::Nonnull, Entry::Form::Plain,
                          Entry::Placement::WrapOperand);
    nonnull.facet = core::Facet::Null;
    replaceChecks(planned, *deref, {nonnull, temporal});
    Entry twice = entry(Entry::Template::Assert, Entry::Form::Violation,
                        Entry::Placement::BeforeCall);
    twice.facet = core::Facet::Temporal;
    replaceChecks(planned, *release, {twice});
  };
  const Emitted instrumented = compile(R"C(
void free(void *);
int f(int *p) {
  free(p);
  return *p;
}
)C",
                                       &setup);
  const Emitted expected = compile(R"C(
void free(void *);
int f(int *p) {
  (__weavec_chk_violation(), free(p));
  return *(__weavec_chk_violation(), (int *)__weavec_chk_nonnull(p));
}
)C",
                                   nullptr, setup);
  EXPECT_TRUE(instrumented.errors.empty()) << instrumented.errors.front();
  EXPECT_TRUE(expected.errors.empty()) << expected.errors.front();
  ASSERT_FALSE(instrumented.ir.empty());
  EXPECT_EQ(comparable(instrumented.ir), comparable(expected.ir));
}

//===----------------------------------------------------------------------===//
// Failure
//===----------------------------------------------------------------------===//

// §10.6: a rewrite Sema rejects is not applied. Here the unit's own
// `__weavec_chk_nonnull` takes a struct, so the call cannot be built: the
// result is invalid, the error Sema diagnosed stays inside the tentative
// scope, the tree is left as it was, and the compile fails with the internal
// error.
TEST(CheckEmitterTest, ForcedInvalidRewriteRestoresTheTree) {
  EmitOptions setup;
  setup.prelude = false;
  const Emitted out = rewrite(R"C(
struct bad { int x; };
static void *__weavec_chk_nonnull(struct bad b) { (void)b; return 0; }
int f(int *p) { return *p; }
)C",
                              setup);
  EXPECT_FALSE(out.ok);
  EXPECT_EQ(out.inserted, 0U);
  ASSERT_EQ(out.errors.size(), 1U);
  EXPECT_EQ(out.errors.front(),
            "WeaveC internal error: could not insert the check for '*p'; "
            "build with -fweavec-checks=none to bypass");
  EXPECT_NE(out.bodies.find("f { return *p; }"), std::string::npos)
      << out.bodies;
}

// A helper the unit does not have at all fails the same way.
TEST(CheckEmitterTest, MissingHelperIsAnInternalError) {
  EmitOptions setup;
  setup.prelude = false;
  const Emitted out = rewrite("int f(int *p) { return *p; }\n", setup);
  EXPECT_FALSE(out.ok);
  ASSERT_EQ(out.errors.size(), 1U);
  EXPECT_NE(out.errors.front().find("could not insert the check for '*p'"),
            std::string::npos);
  EXPECT_NE(out.bodies.find("f { return *p; }"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Precompiled headers and modules (§10.9)
//===----------------------------------------------------------------------===//

// Without the prelude the helpers are declared `extern`, once each; report
// mode calls the `_report` helpers of libweavec_chk.a.
TEST(CheckEmitterTest, ExternalHelpersAreDeclared) {
  for (const core::ChecksMode mode :
       {core::ChecksMode::Trap, core::ChecksMode::Report}) {
    EmitOptions setup;
    setup.prelude = false;
    setup.externalHelpers = true;
    setup.mode = mode;
    const Emitted out = compile(R"C(
int f(int *p, int *q, int i) { int a[3] = {0}; return *p + *q + a[i]; }
)C",
                                &setup);
    EXPECT_TRUE(out.ok);
    EXPECT_TRUE(out.errors.empty()) << out.errors.front();
    const bool report = mode == core::ChecksMode::Report;
    const std::string nonnull =
        report ? "declare ptr @__weavec_chk_nonnull_report(ptr noundef, ptr "
                 "noundef, i32 noundef, i32 noundef)"
               : "declare ptr @__weavec_chk_nonnull(ptr noundef)";
    EXPECT_EQ(count(out.ir, nonnull), 1U) << out.ir;
    EXPECT_EQ(count(out.ir, report ? "call ptr @__weavec_chk_nonnull_report("
                                   : "call ptr @__weavec_chk_nonnull("),
              2U)
        << out.ir;
    EXPECT_NE(out.ir.find(report ? "@__weavec_chk_index_report("
                                 : "@__weavec_chk_index("),
              std::string::npos);
  }
}

//===----------------------------------------------------------------------===//
// Zero-initialisation (§11)
//===----------------------------------------------------------------------===//

struct Planned {
  ZeroInitPlan plan;
  std::vector<std::string> helpers;
};

Planned planFor(llvm::StringRef code, ZeroInitOptions options = {},
                bool enabled = true) {
  const std::unique_ptr<clang::ASTUnit> ast =
      clang::tooling::buildASTFromCodeWithArgs(code,
                                               {"-std=c11", "-target",
                                                "arm64-apple-macosx14.0.0",
                                                "-Wno-everything"},
                                               "unit.c");
  EXPECT_NE(ast, nullptr);
  Planned out;
  if (ast == nullptr)
    return out;
  out.plan = planZeroInit(ast->getASTContext(), core::LibrarySpec::shipped(),
                          enabled, options);
  for (const ZeroInitRewrite &rewrite : out.plan.rewrites)
    out.helpers.push_back(rewrite.helper);
  // The plan points into the AST, which ends here.
  out.plan.rewrites.clear();
  out.plan.allocator = nullptr;
  return out;
}

constexpr llvm::StringLiteral Allocators = R"C(
typedef unsigned long size_t;
typedef struct FILE FILE;
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
void *aligned_alloc(size_t, size_t);
void *valloc(size_t);
char *strdup(const char *);
int *wcsdup(const int *);
long getline(char **, size_t *, FILE *);
int posix_memalign(void **, size_t, size_t);
void free(void *);
)C";

TEST(CheckEmitterTest, ZeroInitChoosesAWrapperPerRow) {
  const Planned out = planFor((Allocators + R"C(
void *(*keep)(size_t) = malloc;
void *(*other)(size_t) = valloc;
extern volatile size_t n;
void use(FILE *f, char **slot, const int *w) {
  char *line = 0;
  size_t cap = 0;
  void *p = 0;
  free(malloc(8));
  free(calloc(1, 8));
  free(realloc(0, 8));
  free(reallocarray(0, 2, 8));
  free(aligned_alloc(16, 32));
  free(strdup("x"));
  free(wcsdup(w));
  getline(&line, &cap, f);
  getline(slot, &cap, f);
  posix_memalign(&p, 16, 32);
  (void)__builtin_alloca(16);
  (void)__builtin_alloca(n);
}
)C")
                                  .str());
  EXPECT_EQ(out.helpers, (std::vector<std::string>{
                             "__weavec_malloc_zero_fn", "__weavec_malloc_zero",
                             "__weavec_calloc_zero", "__weavec_realloc_zero",
                             "__weavec_reallocarray_zero", "__weavec_zero_tail",
                             "__weavec_zero_string", "__weavec_zero_line",
                             "__weavec_posix_memalign_zero", ""}));
  // Under -std=c11 strdup is no builtin, so it keeps its call and the tail
  // after the terminator is zeroed. valloc's address, the wide string, the
  // slot that is not `&object` and the alloca of a volatile size are left
  // alone.
  EXPECT_EQ(out.plan.a5.nonLoweredAllocations, 4U);
}

TEST(CheckEmitterTest, ZeroInitLowersNothingInAnAllocator) {
  const Planned out = planFor((Allocators + R"C(
void *malloc(size_t n) { return calloc(1, n); }
void *twice(void) { return malloc(16); }
)C")
                                  .str());
  EXPECT_TRUE(out.helpers.empty());
  // The unit's own malloc is not the table's; calloc is not lowered.
  EXPECT_EQ(out.plan.a5.nonLoweredAllocations, 1U);
}

TEST(CheckEmitterTest, ZeroInitWithoutAUsableSizeQueryKeepsAlloca) {
  const Planned out = planFor((Allocators + R"C(
void use(void) { free(malloc(4)); (void)__builtin_alloca(8); }
)C")
                                  .str(),
                              ZeroInitOptions{.heap = false, .stack = true});
  EXPECT_EQ(out.helpers, std::vector<std::string>{""});
  EXPECT_EQ(out.plan.a5.nonLoweredAllocations, 1U);
  const Planned off = planFor((Allocators + R"C(
void use(void) { free(malloc(4)); (void)__builtin_alloca(8); }
)C")
                                  .str(),
                              {}, /*enabled=*/false);
  EXPECT_TRUE(off.helpers.empty());
  EXPECT_EQ(off.plan.a5.nonLoweredAllocations, 2U);
}

// A5: the pointer locals a goto, a computed goto or a switch can jump past.
TEST(CheckEmitterTest, CountsBypassedDeclarations) {
  const Planned out = planFor(R"C(
int f(int k) {
  if (k)
    goto later;
  int *skipped = &k;
  int kept = 1;
later:
  switch (k) {
    char *head;
  case 1:
    return head == 0;
  default:
    break;
  }
  int *after = &k;
  return *after + kept + (skipped != 0);
}
)C");
  EXPECT_EQ(out.plan.a5.bypassedDeclarations, 2U);
}

} // namespace weavec::frontend
