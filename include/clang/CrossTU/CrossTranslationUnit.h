//===--- CrossTranslationUnit.h - -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file provides an interface to load binary AST dumps on demand. This
//  feature can be utilized for tools that require cross translation unit
//  support.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H
#define LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H

#include "clang/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"

namespace clang {
class CompilerInstance;
class ASTContext;
class ASTImporter;
class ASTUnit;
class DeclContext;
class FunctionDecl;
class NamedDecl;
class TranslationUnitDecl;

namespace cross_tu {

enum class index_error_code {
  unspecified = 1,
  missing_index_file,
  invalid_index_format,
  multiple_definitions,
  missing_definition
};

class IndexError : public llvm::ErrorInfo<IndexError> {
public:
  static char ID;
  IndexError(index_error_code C) : Code(C), LineNo(0) {}
  IndexError(index_error_code C, int LineNo) : Code(C), LineNo(LineNo) {}
  void log(raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
  index_error_code getCode() const { return Code; }
  int getLineNum() const { return LineNo; }

private:
  index_error_code Code;
  int LineNo;
};

/// \brief This function can parse an index file that determines which
///        translation unit contains which definition.
///
/// The index file format is the following:
/// each line consists of an USR separated by a filepath.
llvm::Expected<llvm::StringMap<std::string>>
parseCrossTUIndex(StringRef IndexPath, StringRef CrossTUDir);

struct IndexEntry {
  std::string USR;
  std::string FilePath;
};

std::string createCrossTUIndexString(const std::vector<IndexEntry> &Index);

/// \brief This class can be used for tools that requires cross translation
///        unit capability.
///
/// This class can load function definitions from external AST files.
/// The loaded definition will be merged back to the original AST using the
/// AST Importer.
/// In order to use this class, an index file is required that describes
/// the locations of the AST files for each function definition.
///
/// Note that this class also implements caching.
class CrossTranslationUnitContext {
public:
  CrossTranslationUnitContext(CompilerInstance &CI);
  ~CrossTranslationUnitContext();

  /// \brief This function can load a function definition from an external AST
  ///        file and merge it into the original AST.
  ///
  /// This method should only be used on functions that have no definitions in
  /// the current translation unit. A function definition with the same
  /// declaration will be looked up in the index file which should be in the
  /// \p CrossTUDir directory, called \p IndexName. In case the declaration is
  /// found in the index the corresponding AST file will be loaded and the
  /// definition of the function will be merged into the original AST using
  /// the AST Importer. The declaration with the definition will be returned.
  /// If no suitable definition is found in the index file, null will be
  /// returned.
  ///
  /// Note that the AST files should also be in the \p CrossTUDir.
  const FunctionDecl *getCrossTUDefinition(const FunctionDecl *FD,
                                           StringRef CrossTUDir,
                                           StringRef IndexName);

private:
  ASTImporter &getOrCreateASTImporter(ASTContext &From);
  std::string getLookupName(const NamedDecl *ND);
  const FunctionDecl *findFunctionInDeclContext(const DeclContext *DC,
                                                StringRef LookupFnName);

  llvm::StringMap<std::unique_ptr<clang::ASTUnit>> FileASTUnitMap;
  llvm::StringMap<clang::ASTUnit *> FunctionASTUnitMap;
  llvm::StringMap<std::string> FunctionFileMap;
  llvm::DenseMap<TranslationUnitDecl *, std::unique_ptr<ASTImporter>>
      ASTUnitImporterMap;
  CompilerInstance &CI;
  ASTContext &Context;
};

} // namespace cross_tu
} // namespace clang

#endif // LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H
