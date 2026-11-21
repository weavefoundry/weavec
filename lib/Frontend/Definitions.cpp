//===- Definitions.cpp - Safe units agree when they are linked ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0051 §5. A unit's record is a table the comparison reads and a string
// area it reads only to report a mismatch, so that a link touches little of
// each object:
//
//   "weavec-defs 2\n"  u32 count  u32 string bytes
//   count entries of 40 bytes, little-endian:
//     u8 kind, 3 bytes of zero, u32 name, u64 key, u64 hash, u32 place,
//     u32 description, u32 users, 4 bytes of zero
//   the strings, each ending in a zero byte (the u32s are their offsets)
//
// The kinds:
//
//   T  a C++ class, struct, union or enumeration other units can name (its
//      qualified name): Clang's ODR hash of its definition, its layout (size,
//      alignment, each base's and field's offset) and the WeaveC annotations
//      of the class and its members
//   F  a function with external linkage the unit defines or names (its
//      symbol): its result's and parameters' types, `noreturn`, and the
//      annotations of its parameters and result
//   G  a global with external linkage the unit defines or names: its type
//      (with its array bound), its alignment, and its annotations
//   C  a C struct, union or enumeration that a recorded function or global
//      names in its type, with those declarations (`users`, comma-separated):
//      C has no one-definition rule, so two units' differ only where they
//      share a user
//
// `key` is a hash of the kind and the name, `hash` of what the units must
// agree on. The record is in a section the linker does not copy into the
// image it links (Mach-O's debug attribute, ELF's SHF_EXCLUDE): an image
// carries none of its units' records.
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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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
    std::string strings;
    llvm::StringMap<std::uint32_t> offsets;
    const auto string = [&](llvm::StringRef text) {
      const auto [found, added] =
          offsets.try_emplace(text, static_cast<std::uint32_t>(strings.size()));
      if (added) {
        strings += text;
        strings.push_back('\0');
      }
      return found->second;
    };
    std::string table;
    const auto put = [](std::string &into, std::uint64_t value,
                        unsigned bytes) {
      for (unsigned i = 0; i < bytes; ++i)
        into.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
    };
    for (const Entry &entry : entries) {
      std::string users;
      for (const std::string &user : entry.users) {
        if (!users.empty())
          users += ',';
        users += user;
      }
      table.push_back(entry.kind);
      table.append(3, '\0');
      put(table, string(entry.name), 4);
      put(table, llvm::xxh3_64bits(std::string(1, entry.kind) + entry.name), 8);
      put(table, entry.hash, 8);
      put(table, string(entry.place), 4);
      put(table, string(entry.description), 4);
      put(table, string(users), 4);
      put(table, 0, 4);
    }
    std::string out = DefinitionsHeader.str();
    put(out, entries.size(), 4);
    put(out, strings.size(), 4);
    return out + table + strings;
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
  // Not copied into a linked image: ELF's SHF_EXCLUDE (Mach-O's is the
  // section's debug attribute).
  record->setMetadata(llvm::LLVMContext::MD_exclude,
                      llvm::MDNode::get(module.context, {}));
  llvm::appendToCompilerUsed(module.module, {record});
}

//===----------------------------------------------------------------------===//
// The comparison (§5.2)
//===----------------------------------------------------------------------===//

namespace {

/// One entry as read: its key and hash, and where its strings are in the
/// section's bytes, which the comparison keeps mapped.
struct Recorded {
  char kind = 'T';
  std::uint64_t key = 0;
  std::uint64_t hash = 0;
  const char *strings = nullptr;
  std::uint32_t name = 0;
  std::uint32_t place = 0;
  std::uint32_t description = 0;
  std::uint32_t users = 0;
  /// Index into the comparison's inputs' names.
  unsigned from = 0;

  [[nodiscard]] llvm::StringRef text(std::uint32_t offset) const {
    return strings + offset;
  }
};

/// The comparison's state: the mapped inputs, their names, the entries.
struct Records {
  std::vector<llvm::object::OwningBinary<llvm::object::Binary>> binaries;
  std::vector<std::unique_ptr<llvm::object::Binary>> members;
  std::vector<std::string> names;
  std::vector<Recorded> entries;
  DefinitionsCompared found;
  /// The input defines the allocator's symbol strongly.
  bool allocator = false;
};

/// What one read of a link's inputs looks for.
struct Wanted {
  bool compare = true;
  llvm::StringRef allocator;
};

} // namespace

/// A little-endian integer of `bytes` bytes at `at`.
static std::uint64_t little(const char *at, unsigned bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(at[i]))
             << (8 * i);
  return value;
}

/// The records of a section's contents.
static void readRecords(llvm::StringRef contents, unsigned from,
                        Records &records) {
  constexpr std::size_t Header = DefinitionsHeader.size() + 8;
  constexpr std::size_t EntryBytes = 40;
  while (contents.size() >= Header && contents.starts_with(DefinitionsHeader)) {
    const char *at = contents.data() + DefinitionsHeader.size();
    const std::uint64_t count = little(at, 4);
    const std::uint64_t bytes = little(at + 4, 4);
    const std::uint64_t size = Header + (count * EntryBytes) + bytes;
    if (contents.size() < size)
      return;
    ++records.found.safe;
    const char *table = contents.data() + Header;
    const char *strings = table + (count * EntryBytes);
    for (std::uint64_t i = 0; i < count; ++i) {
      const char *entry = table + (i * EntryBytes);
      records.entries.push_back(
          {.kind = entry[0],
           .key = little(entry + 8, 8),
           .hash = little(entry + 16, 8),
           .strings = strings,
           .name = static_cast<std::uint32_t>(little(entry + 4, 4)),
           .place = static_cast<std::uint32_t>(little(entry + 24, 4)),
           .description = static_cast<std::uint32_t>(little(entry + 28, 4)),
           .users = static_cast<std::uint32_t>(little(entry + 32, 4)),
           .from = from});
    }
    contents = contents.drop_front(size);
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

/// Whether `file` defines `symbol` as a strong global.
static bool definesStrongly(const llvm::object::SymbolicFile &file,
                            llvm::StringRef symbol) {
  std::string name;
  for (const llvm::object::BasicSymbolRef &each : file.symbols()) {
    llvm::Expected<std::uint32_t> flags = each.getFlags();
    if (!flags) {
      llvm::consumeError(flags.takeError());
      continue;
    }
    if ((*flags & llvm::object::BasicSymbolRef::SF_Undefined) != 0 ||
        (*flags & llvm::object::BasicSymbolRef::SF_Global) == 0 ||
        (*flags & llvm::object::BasicSymbolRef::SF_Weak) != 0)
      continue;
    name.clear();
    llvm::raw_string_ostream os(name);
    if (llvm::Error error = each.printName(os)) {
      llvm::consumeError(std::move(error));
      continue;
    }
    if (name == symbol)
      return true;
  }
  return false;
}

/// What a link wants of one of its objects, or of an archive's member: its
/// records, and whether a relocatable one (not a linked image, whose
/// definitions are its own) defines the allocator.
static void readFile(const llvm::object::Binary &binary, std::string from,
                     const Wanted &wanted, Records &records) {
  const auto *file = llvm::dyn_cast<llvm::object::SymbolicFile>(&binary);
  if (file == nullptr)
    return;
  const auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(file);
  if (!wanted.allocator.empty() && !records.allocator &&
      (object == nullptr || object->isRelocatableObject()))
    records.allocator = definesStrongly(*file, wanted.allocator);
  if (wanted.compare && object != nullptr)
    readObject(*object, std::move(from), records);
}

/// Reads `path`: an object, a linked image or each member of an archive.
static void readInput(const std::string &path, const Wanted &wanted,
                      Records &records) {
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
  if (llvm::isa<llvm::object::SymbolicFile>(read)) {
    readFile(*read, path, wanted, records);
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
    readFile(*records.members.back(), std::move(from), wanted, records);
  }
  llvm::consumeError(std::move(error));
}

/// The comparison of what each input's read found.
static DefinitionsCompared compareRecords(std::vector<Records> &each,
                                          llvm::raw_ostream &errors);

/// Compares one entry with those met before of its kind and name.
static void compareEntry(
    const Recorded &entry, llvm::ArrayRef<const Recorded *> seen,
    llvm::DenseMap<std::uint64_t, llvm::SmallVector<const Recorded *, 2>> &more,
    llvm::DenseSet<std::uint64_t> &reported,
    llvm::ArrayRef<llvm::StringRef> names, DefinitionsCompared &found,
    llvm::raw_ostream &errors);

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

LinkRead readLink(llvm::ArrayRef<std::string> inputs, bool compare,
                  llvm::StringRef allocator, llvm::raw_ostream &errors) {
  // Each input read once, on its own (in parallel: reading is the cost),
  // then their entries in the link's order.
  const Wanted wanted{.compare = compare, .allocator = allocator};
  std::vector<Records> each(inputs.size());
  llvm::parallelFor(0, inputs.size(), [&](std::size_t i) {
    readInput(inputs[i], wanted, each[i]);
  });
  LinkRead read;
  for (std::size_t i = 0; i < each.size(); ++i)
    if (each[i].allocator) {
      read.allocator = inputs[i];
      break;
    }
  if (!compare)
    return read;
  read.definitions = compareRecords(each, errors);
  return read;
}

static DefinitionsCompared compareRecords(std::vector<Records> &each,
                                          llvm::raw_ostream &errors) {
  // The entries stay where each input's read put them: the comparison
  // walks them in the link's order, naming their inputs from `names`.
  DefinitionsCompared found;
  std::vector<llvm::StringRef> names;
  std::size_t total = 0;
  for (Records &one : each) {
    const auto offset = static_cast<unsigned>(names.size());
    for (const std::string &name : one.names)
      names.emplace_back(name);
    for (Recorded &entry : one.entries)
      entry.from += offset;
    total += one.entries.size();
    found.objects += one.found.objects;
    found.safe += one.found.safe;
  }
  // For each kind and name, the first entry met; `more` holds the first
  // entry of each other hash met (none, unless the units disagree) and,
  // for C's tags, each other entry, which only those that share a user
  // are compared with.
  llvm::DenseMap<std::uint64_t, const Recorded *> first;
  first.reserve(static_cast<unsigned>(total));
  llvm::DenseMap<std::uint64_t, llvm::SmallVector<const Recorded *, 2>> more;
  llvm::DenseSet<std::uint64_t> reported;
  llvm::SmallVector<const Recorded *, 4> seen;
  for (const Records &one : each)
    for (const Recorded &entry : one.entries) {
      const auto [met, inserted] = first.try_emplace(entry.key, &entry);
      if (inserted) {
        if (entry.kind == 'T' || entry.kind == 'C')
          ++found.types;
        else if (entry.kind == 'F')
          ++found.functions;
        else
          ++found.globals;
        continue;
      }
      // The common case: one more unit that agrees with the first.
      if (entry.kind != 'C' && met->second->hash == entry.hash)
        continue;
      seen.assign({met->second});
      const auto others = more.find(entry.key);
      if (others != more.end())
        seen.append(others->second.begin(), others->second.end());
      compareEntry(entry, seen, more, reported, names, found, errors);
    }
  return found;
}

/// Compares one entry with those met before of its kind and name.
static void compareEntry(
    const Recorded &entry, llvm::ArrayRef<const Recorded *> seen,
    llvm::DenseMap<std::uint64_t, llvm::SmallVector<const Recorded *, 2>> &more,
    llvm::DenseSet<std::uint64_t> &reported,
    llvm::ArrayRef<llvm::StringRef> names, DefinitionsCompared &found,
    llvm::raw_ostream &errors) {
  const Recorded *against = nullptr;
  bool same = false;
  for (const Recorded *other : seen) {
    if (entry.kind == 'C' &&
        !shareUser(entry.text(entry.users), other->text(other->users)))
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
    more[entry.key].push_back(&entry);
  if (against == nullptr || !reported.insert(entry.key).second)
    return;
  ++found.mismatches;
  llvm::StringRef shown = entry.text(entry.name);
  if (entry.kind == 'F') {
    // A function is named by its qualified name, not its symbol.
    const llvm::StringRef description = entry.text(entry.description);
    const std::size_t open = description.find('(');
    if (open != llvm::StringRef::npos)
      shown = description.take_front(open).rsplit(' ').second;
  }
  errors << "weavec-cc: error: '" << shown << "' is " << what(entry.kind)
         << " differently in two safe units "
            "[weavec-safe:definition-mismatch]\n";
  for (const Recorded *side : {against, &entry}) {
    const bool declared = side->kind == 'F' || side->kind == 'G';
    errors << side->text(side->place) << ": note: here it is "
           << (declared ? "'" : "") << side->text(side->description)
           << (declared ? "'" : "") << " (" << names[side->from] << ")\n";
  }
  errors << "weavec-cc: note: a program has one definition of each name: "
            "put a type only one file uses in an unnamed namespace, or "
            "define it in one header both include\n";
}

} // namespace weavec::frontend
