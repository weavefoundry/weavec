//===- Definitions.cpp - Safe units agree when they are linked ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0051 §5. The record is text, one entry a line after the header line,
// each field separated by a tab:
//
//   T <name> <hash> <file>:<line>:<column> <description>
//       a C++ class, struct, union or enumeration other units can name (its
//       qualified name): Clang's ODR hash of its definition, its layout
//       (size, alignment, each base's and field's offset) and the WeaveC
//       annotations of the class and its members
//   F <symbol> <hash> <place> <description>
//       a function with external linkage the unit defines or names: its
//       type, `noreturn`, and the annotations of its parameters and result
//   G <symbol> <hash> <place> <description>
//       a global with external linkage the unit defines or names: its type
//       (with its array bound), and its annotations
//   C <tag> <hash> <place> <description> <user>,<user>...
//       a C struct, union or enumeration that a recorded function or global
//       names in its type: C has no one-definition rule, so two units'
//       differ only where they share a user
//
// A unit's record ends in a zero byte, so that a linked image's section,
// the units' records one after another, is read the same way.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Definitions.h"

#include "GuardPassImpl.h"
#include "SafeCheckImpl.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/xxhash.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace weavec::frontend {

//===----------------------------------------------------------------------===//
// The record (§5.1)
//===----------------------------------------------------------------------===//

/// The WeaveC annotations of a type, as text.
static std::string annotationText(clang::QualType type,
                                  const clang::ASTContext &ast) {
  std::string text;
  for (const TypeAnnotation &annotation : annotationsOf(type, ast))
    text += (annotation.name + "(" + annotation.argument + ")").str();
  return text;
}

/// The WeaveC annotations a declaration carries as attributes, but those
/// that say whether its own code is safe (a declaration and its definition
/// may differ in them).
static std::string attributeText(const clang::Decl &declaration) {
  std::string text;
  for (const auto *annotate :
       declaration.specific_attrs<clang::AnnotateAttr>()) {
    const llvm::StringRef name = annotate->getAnnotation();
    // What the source writes, not what a unit's checker adds (a fast unit's
    // field annotations, placed locals).
    if (annotate->isImplicit() || !name.starts_with("weavec.") ||
        name == SafeAnnotation || name == GuardedAnnotation ||
        name == "weavec.unsafe" || name.starts_with(PlacedAnnotation) ||
        name == FieldAnnotation || name == CountAnnotation)
      continue;
    text += name.str() + ";";
  }
  return text;
}

/// No tab or line break inside a field.
static std::string field(llvm::StringRef text) {
  std::string out;
  for (const char c : text)
    out.push_back(c == '\t' || c == '\n' || c == '\r' ? ' ' : c);
  return out;
}

namespace {

struct Entry {
  char kind = 'T';
  std::string name;
  std::uint64_t hash = 0;
  std::string place;
  std::string description;
  std::set<std::string> users;
};

class Recorder final : public clang::RecursiveASTVisitor<Recorder> {
public:
  explicit Recorder(clang::ASTContext &ast)
      : ast(ast), sources(ast.getSourceManager()), names(ast),
        policy(ast.getPrintingPolicy()) {
    policy.SuppressTagKeyword = false;
  }

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
    return false;
  }

  bool TraverseDecl(clang::Decl *declaration) {
    if (declaration != nullptr &&
        !llvm::isa<clang::TranslationUnitDecl>(declaration) &&
        sources.isInSystemHeader(declaration->getLocation()))
      return true;
    // A function's own declarations (its locals, local classes) are its
    // own.
    if (auto *function =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(declaration)) {
      WalkUpFromFunctionDecl(function);
      return true;
    }
    return clang::RecursiveASTVisitor<Recorder>::TraverseDecl(declaration);
  }

  bool VisitTagDecl(clang::TagDecl *tag) {
    if (!tag->isThisDeclarationADefinition() || tag->isInvalidDecl() ||
        tag->isDependentContext() || tag->getIdentifier() == nullptr ||
        llvm::isa<clang::ClassTemplateSpecializationDecl>(tag))
      return true;
    if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
        record != nullptr &&
        (record->isLambda() || record->getDescribedClassTemplate() != nullptr))
      return true;
    if (ast.getLangOpts().CPlusPlus) {
      if (!tag->isExternallyVisible())
        return true;
      add(entryOf(*tag, 'T'));
    } else {
      tags[tagName(*tag)] = tag;
    }
    return true;
  }

  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    if (function->isImplicit() || function->isDeleted() ||
        function->isDefaulted() || function->isDependentContext() ||
        !function->isExternallyVisible() ||
        function->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate ||
        llvm::isa<clang::CXXDeductionGuideDecl>(function))
      return true;
    const clang::FunctionDecl *chosen = function->getDefinition();
    if (chosen == nullptr) {
      if (!function->isReferenced())
        return true;
      chosen = function->getFirstDecl();
    }
    const std::string symbol = symbolOf(*chosen);
    if (symbol.empty() || !seen.insert("F" + symbol).second)
      return true;
    // The result and the parameters, not the whole type: a defaulted or
    // implicit member's exception specification is computed when a unit
    // needs it, so units differ in it for one declaration.
    std::string text =
        chosen->getReturnType().getCanonicalType().getAsString(policy) + "(";
    // Each parameter's type as the function's type has it: a top-level
    // `const` on a definition's parameter is the definition's own.
    for (const clang::ParmVarDecl *parameter : chosen->parameters())
      text += parameter->getType()
                  .getCanonicalType()
                  .getUnqualifiedType()
                  .getAsString(policy) +
              ',';
    text += chosen->isVariadic() ? "...)" : ")";
    if (chosen->isNoReturn())
      text += " noreturn";
    text += annotationText(chosen->getReturnType(), ast);
    text += attributeText(*chosen);
    for (const clang::ParmVarDecl *parameter : chosen->parameters())
      text += "|" + annotationText(parameter->getType(), ast) +
              attributeText(*parameter);
    Entry entry{.kind = 'F',
                .name = symbol,
                .hash = llvm::xxh3_64bits(text),
                .place = placeOf(chosen->getLocation()),
                .description = describe(*chosen),
                .users = {}};
    if (!ast.getLangOpts().CPlusPlus) {
      usersOf(chosen->getReturnType(), symbol);
      for (const clang::ParmVarDecl *parameter : chosen->parameters())
        usersOf(parameter->getType(), symbol);
    }
    add(std::move(entry));
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *variable) {
    if (llvm::isa<clang::ParmVarDecl>(variable) ||
        !(variable->isFileVarDecl() || variable->isStaticDataMember()) ||
        variable->getDeclContext()->isDependentContext() ||
        !variable->isExternallyVisible() ||
        variable->getDescribedVarTemplate() != nullptr ||
        llvm::isa<clang::VarTemplateSpecializationDecl>(variable) ||
        variable->getType()->isIncompleteArrayType())
      return true;
    const clang::VarDecl *chosen = variable->getDefinition();
    if (chosen == nullptr) {
      if (!variable->isReferenced())
        return true;
      chosen = variable->getFirstDecl();
    }
    const std::string symbol = symbolOf(*chosen);
    if (symbol.empty() || !seen.insert("G" + symbol).second)
      return true;
    const clang::QualType type = chosen->getType();
    std::string text = type.getCanonicalType().getAsString(policy);
    if (chosen->getTLSKind() != clang::VarDecl::TLS_None)
      text += " thread_local";
    // (J.2 item 70: an alignment specifier is part of what units agree on.)
    if (chosen->hasAttr<clang::AlignedAttr>())
      text += " aligned(" +
              llvm::utostr(static_cast<std::uint64_t>(
                  ast.getDeclAlign(chosen).getQuantity())) +
              ")";
    text += annotationText(type, ast) + attributeText(*chosen);
    if (!ast.getLangOpts().CPlusPlus)
      usersOf(type, symbol);
    add({.kind = 'G',
         .name = symbol,
         .hash = llvm::xxh3_64bits(text),
         .place = placeOf(chosen->getLocation()),
         .description = type.getAsString(policy),
         .users = {}});
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

  /// The record: its header, then the entries, and (C) the tags the
  /// recorded declarations name.
  std::string text() {
    for (const auto &[tag, users] : tagUsers) {
      const auto found = tags.find(tag);
      if (found == tags.end())
        continue;
      Entry entry = entryOf(*found->second, 'C');
      entry.users = users;
      add(std::move(entry));
    }
    std::string out = (DefinitionsHeader + "\n").str();
    for (const Entry &entry : entries) {
      out.push_back(entry.kind);
      out += "\t" + field(entry.name) + "\t" +
             llvm::utohexstr(entry.hash, /*LowerCase=*/true) + "\t" +
             field(entry.place) + "\t" + field(entry.description);
      if (entry.kind == 'C') {
        out += '\t';
        bool first = true;
        for (const std::string &user : entry.users) {
          if (!first)
            out += ',';
          out += user;
          first = false;
        }
      }
      out.push_back('\n');
    }
    return out;
  }

private:
  clang::ASTContext &ast;
  const clang::SourceManager &sources;
  clang::ASTNameGenerator names;
  clang::PrintingPolicy policy;
  std::vector<Entry> entries;
  std::set<std::string> seen;
  /// C: the tags the unit defines, and which recorded declarations name
  /// each.
  std::map<std::string, const clang::TagDecl *> tags;
  std::map<std::string, std::set<std::string>> tagUsers;

  void add(Entry entry) { entries.push_back(std::move(entry)); }

  std::string placeOf(clang::SourceLocation at) const {
    const clang::PresumedLoc presumed = sources.getPresumedLoc(at);
    if (presumed.isInvalid())
      return "<unknown>";
    return (llvm::Twine(presumed.getFilename()) + ":" +
            llvm::Twine(presumed.getLine()) + ":" +
            llvm::Twine(presumed.getColumn()))
        .str();
  }

  std::string symbolOf(const clang::NamedDecl &declaration) {
    std::string symbol = names.getName(&declaration);
    llvm::StringRef name = symbol;
    name.consume_front(ast.getTargetInfo().getUserLabelPrefix());
    return name.str();
  }

  /// `struct node`, `union value`, `enum color`.
  static std::string tagName(const clang::TagDecl &tag) {
    return (tag.getKindName() + " " + tag.getName()).str();
  }

  /// The tags a C declaration of `user` names, through pointers, arrays,
  /// functions and typedefs.
  void usersOf(clang::QualType type, const std::string &user) {
    for (unsigned depth = 0; depth < 8 && !type.isNull(); ++depth) {
      type = type.getCanonicalType();
      if (const auto *tag = type->getAsTagDecl()) {
        if (tag->getIdentifier() != nullptr)
          tagUsers[tagName(*tag)].insert(user);
        return;
      }
      if (type->isPointerType())
        type = type->getPointeeType();
      else if (type->isArrayType())
        type =
            llvm::cast<clang::ArrayType>(type.getTypePtr())->getElementType();
      else
        return;
    }
  }

  /// A type's entry: its ODR hash, layout and annotations.
  Entry entryOf(const clang::TagDecl &tag, char kind) {
    std::string text;
    std::string description;
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(&tag)) {
      const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record);
      // (The hashes are computed once and cached in the declaration, which
      // is why the getter is not const.)
      // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
      text = llvm::utohexstr(
          cxx != nullptr
              ? const_cast<clang::CXXRecordDecl *>(cxx)->getODRHash()
              : const_cast<clang::RecordDecl *>(record)->getODRHash());
      // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
      text += attributeText(*record);
      const clang::ASTRecordLayout &layout = ast.getASTRecordLayout(record);
      text += ";" +
              llvm::utostr(
                  static_cast<std::uint64_t>(layout.getSize().getQuantity())) +
              "/" +
              llvm::utostr(static_cast<std::uint64_t>(
                  layout.getAlignment().getQuantity()));
      description = llvm::utostr(static_cast<std::uint64_t>(
                        layout.getSize().getQuantity())) +
                    " bytes:";
      if (cxx != nullptr)
        for (const clang::CXXBaseSpecifier &base : cxx->bases())
          if (const clang::CXXRecordDecl *of =
                  base.getType()->getAsCXXRecordDecl();
              of != nullptr && !base.isVirtual())
            text +=
                ";base@" + llvm::utostr(static_cast<std::uint64_t>(
                               layout.getBaseClassOffset(of).getQuantity()));
      bool first = true;
      for (const clang::FieldDecl *member : record->fields()) {
        const clang::QualType type = member->getType();
        text += ";" + member->getName().str() + "@" +
                llvm::utostr(layout.getFieldOffset(member->getFieldIndex())) +
                annotationText(type, ast) + attributeText(*member);
        description += (first ? " '" : ", '") + type.getAsString(policy) + " " +
                       member->getName().str() + "'";
        first = false;
      }
    } else if (const auto *enumeration =
                   llvm::dyn_cast<clang::EnumDecl>(&tag)) {
      text = llvm::utohexstr(
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): as above
          const_cast<clang::EnumDecl *>(enumeration)->getODRHash());
      description = "enumeration of '" +
                    enumeration->getIntegerType().getAsString(policy) + "'";
      for (const clang::EnumConstantDecl *value : enumeration->enumerators()) {
        text += ";" + value->getName().str() + "=" +
                llvm::toString(value->getInitVal(), 10);
      }
    }
    return {.kind = kind,
            .name = kind == 'C' ? tagName(tag) : tag.getQualifiedNameAsString(),
            .hash = llvm::xxh3_64bits(text),
            .place = placeOf(tag.getLocation()),
            .description = description,
            .users = {}};
  }

  std::string describe(const clang::FunctionDecl &function) const {
    std::string text = function.getReturnType().getAsString(policy) + " " +
                       function.getQualifiedNameAsString() + "(";
    bool first = true;
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      text += (first ? "" : ", ") +
              parameter->getType().getUnqualifiedType().getAsString(policy);
      // What the type's spelling does not show: its WeaveC annotations.
      for (const TypeAnnotation &annotation :
           annotationsOf(parameter->getType(), ast))
        text += (" " + annotation.name.drop_front(7) + "(" +
                 annotation.argument + ")")
                    .str();
      first = false;
    }
    if (function.isVariadic())
      text += first ? "..." : ", ...";
    text += ')';
    if (function.isNoReturn())
      text = "[[noreturn]] " + text;
    return text;
  }
};

} // namespace

std::string recordDefinitions(clang::ASTContext &ast, const SafeUnit &unit) {
  // A unit compiled with -fweavec-safe is a safe unit with or without a
  // function (a file of globals), and a WEAVEC_SAFE function makes one.
  if (unit.definedSafe.empty() && !unit.defined)
    return {};
  Recorder recorder(ast);
  recorder.TraverseDecl(ast.getTranslationUnitDecl());
  return recorder.text();
}

//===----------------------------------------------------------------------===//
// The section
//===----------------------------------------------------------------------===//

void guard::emitDefinitions(ModuleContext &module) {
  if (module.options.safe == nullptr ||
      module.options.safe->definitionRecord.empty())
    return;
  llvm::Constant *data = llvm::ConstantDataArray::getString(
      module.context, module.options.safe->definitionRecord,
      /*AddNull=*/true);
  auto *record = new llvm::GlobalVariable(
      module.module, data->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, data, "__weavec.defs");
  const llvm::Triple triple(module.module.getTargetTriple());
  record->setSection(triple.isOSBinFormatMachO() ? DefinitionsSectionMachO.str()
                                                 : DefinitionsSectionELF.str());
  record->setAlignment(llvm::Align(1));
  llvm::appendToUsed(module.module, {record});
}

//===----------------------------------------------------------------------===//
// The comparison (§5.2)
//===----------------------------------------------------------------------===//

namespace {

/// One entry as read: views into the section's bytes, which the
/// comparison keeps mapped.
struct Recorded {
  char kind = 'T';
  llvm::StringRef name;
  llvm::StringRef hash;
  llvm::StringRef place;
  llvm::StringRef description;
  llvm::StringRef users;
  /// Index into the comparison's inputs' names.
  unsigned from = 0;
};

/// The comparison's state: the mapped inputs, their names, the entries.
struct Records {
  std::vector<llvm::object::OwningBinary<llvm::object::Binary>> binaries;
  std::vector<std::unique_ptr<llvm::object::Binary>> members;
  std::vector<std::string> names;
  std::vector<Recorded> entries;
  DefinitionsCompared found;
};

} // namespace

/// The records of a section's contents.
static void readRecords(llvm::StringRef contents, unsigned from,
                        Records &records) {
  while (!contents.empty()) {
    const auto [unit, rest] = contents.split('\0');
    contents = rest;
    llvm::StringRef lines = unit;
    const auto [header, body] = lines.split('\n');
    if (header != DefinitionsHeader)
      continue;
    ++records.found.safe;
    lines = body;
    while (!lines.empty()) {
      const auto [line, next] = lines.split('\n');
      lines = next;
      llvm::SmallVector<llvm::StringRef, 6> fields;
      line.split(fields, '\t');
      if (fields.size() < 5 || fields[0].size() != 1)
        continue;
      records.entries.push_back({.kind = fields[0][0],
                                 .name = fields[1],
                                 .hash = fields[2],
                                 .place = fields[3],
                                 .description = fields[4],
                                 .users = fields.size() > 5 ? fields[5] : "",
                                 .from = from});
    }
  }
}

/// The records in the definitions section of `object`, if it has one.
static void readObject(const llvm::object::ObjectFile &object, std::string from,
                       Records &records) {
  ++records.found.objects;
  for (const llvm::object::SectionRef &section : object.sections()) {
    llvm::Expected<llvm::StringRef> name = section.getName();
    if (!name) {
      llvm::consumeError(name.takeError());
      continue;
    }
    if (*name != DefinitionsSectionName && *name != DefinitionsSectionELF)
      continue;
    llvm::Expected<llvm::StringRef> contents = section.getContents();
    if (!contents) {
      llvm::consumeError(contents.takeError());
      continue;
    }
    records.names.push_back(std::move(from));
    readRecords(*contents, static_cast<unsigned>(records.names.size() - 1),
                records);
    return;
  }
}

/// Reads `path`: an object, a linked image or each member of an archive.
static void readInput(const std::string &path, Records &records) {
  if (!llvm::sys::fs::is_regular_file(path))
    return;
  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary =
      llvm::object::createBinary(path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return;
  }
  records.binaries.push_back(std::move(*binary));
  const llvm::object::Binary *read = records.binaries.back().getBinary();
  if (const auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(read)) {
    readObject(*object, path, records);
    return;
  }
  const auto *archive = llvm::dyn_cast<llvm::object::Archive>(read);
  if (archive == nullptr)
    return;
  llvm::Error error = llvm::Error::success();
  for (const llvm::object::Archive::Child &child : archive->children(error)) {
    llvm::Expected<std::unique_ptr<llvm::object::Binary>> member =
        child.getAsBinary();
    if (!member) {
      llvm::consumeError(member.takeError());
      continue;
    }
    llvm::Expected<llvm::StringRef> name = child.getName();
    std::string from =
        path + "(" + (name ? name->str() : std::string("?")) + ")";
    if (!name)
      llvm::consumeError(name.takeError());
    records.members.push_back(std::move(*member));
    if (const auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(
            records.members.back().get()))
      readObject(*object, std::move(from), records);
  }
  llvm::consumeError(std::move(error));
}

/// What the error says the two places disagree on.
static llvm::StringRef what(char kind) {
  return kind == 'F' || kind == 'G' ? "declared" : "defined";
}

/// Whether two C tags' comma-separated users share one.
static bool shareUser(llvm::StringRef a, llvm::StringRef b) {
  llvm::SmallVector<llvm::StringRef, 4> left;
  a.split(left, ',', -1, /*KeepEmpty=*/false);
  while (!b.empty()) {
    const auto [user, rest] = b.split(',');
    b = rest;
    if (!user.empty() && llvm::is_contained(left, user))
      return true;
  }
  return false;
}

DefinitionsCompared compareDefinitions(llvm::ArrayRef<std::string> inputs,
                                       llvm::raw_ostream &errors) {
  // Each input read on its own (in parallel: reading is the comparison's
  // cost), then their entries in the link's order.
  std::vector<Records> each(inputs.size());
  llvm::parallelFor(0, inputs.size(),
                    [&](std::size_t i) { readInput(inputs[i], each[i]); });
  Records records;
  for (Records &one : each) {
    const auto offset = static_cast<unsigned>(records.names.size());
    for (std::string &name : one.names)
      records.names.push_back(std::move(name));
    for (Recorded &entry : one.entries) {
      entry.from += offset;
      records.entries.push_back(entry);
    }
    records.found.objects += one.found.objects;
    records.found.safe += one.found.safe;
  }
  DefinitionsCompared &found = records.found;
  // For each kind and name, the first entry of each hash met (one, unless
  // the units disagree); for C's tags, each entry, which only those that
  // share a user are compared with.
  llvm::StringMap<llvm::SmallVector<const Recorded *, 1>> first;
  llvm::StringSet<> reported;
  std::string key;
  for (const Recorded &entry : records.entries) {
    key.assign(1, entry.kind);
    key += entry.name;
    auto &seen = first[key];
    if (seen.empty()) {
      if (entry.kind == 'T' || entry.kind == 'C')
        ++found.types;
      else if (entry.kind == 'F')
        ++found.functions;
      else
        ++found.globals;
    }
    const Recorded *against = nullptr;
    bool same = false;
    for (const Recorded *other : seen) {
      if (entry.kind == 'C' && !shareUser(entry.users, other->users))
        continue;
      if (other->hash == entry.hash) {
        same = true;
        if (entry.kind != 'C')
          break;
        continue;
      }
      against = other;
      break;
    }
    if (entry.kind == 'C' || !same)
      seen.push_back(&entry);
    if (against == nullptr || !reported.insert(key).second)
      continue;
    ++found.mismatches;
    llvm::StringRef shown = entry.name;
    if (entry.kind == 'F') {
      // A function is named by its qualified name, not its symbol.
      const llvm::StringRef description = entry.description;
      const std::size_t open = description.find('(');
      if (open != llvm::StringRef::npos)
        shown = description.take_front(open).rsplit(' ').second;
    }
    errors << "weavec-cc: error: '" << shown << "' is " << what(entry.kind)
           << " differently in two safe units "
              "[weavec-safe:definition-mismatch]\n";
    for (const Recorded *side : {against, &entry}) {
      const bool declared = side->kind == 'F' || side->kind == 'G';
      errors << side->place << ": note: here it is " << (declared ? "'" : "")
             << side->description << (declared ? "'" : "") << " ("
             << records.names[side->from] << ")\n";
    }
    errors << "weavec-cc: note: a program has one definition of each name: "
              "put a type only one file uses in an unnamed namespace, or "
              "define it in one header both include\n";
  }
  return found;
}

} // namespace weavec::frontend
