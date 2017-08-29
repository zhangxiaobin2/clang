//===--- CrossTranslationUnit.cpp - -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements the CrossTranslationUnit interface.
//
//===----------------------------------------------------------------------===//
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CrossTU/CrossTUDiagnostic.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <sstream>

namespace clang {
namespace cross_tu {

namespace {
// FIXME: This class is will be removed after the transition to llvm::Error.
class IndexErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "clang.index"; }

  std::string message(int Condition) const override {
    switch (static_cast<index_error_code>(Condition)) {
    case index_error_code::unspecified:
      return "An unknown error has occurred.";
    case index_error_code::missing_index_file:
      return "The index file is missing.";
    case index_error_code::invalid_index_format:
      return "Invalid index file format.";
    case index_error_code::multiple_definitions:
      return "Multiple definitions in the index file.";
    case index_error_code::missing_definition:
      return "Missing definition from the index file.";
    }
    llvm_unreachable("Unrecognized index_error_code.");
  }
};

static llvm::ManagedStatic<IndexErrorCategory> Category;
} // end anonymous namespace

char IndexError::ID = 0;

void IndexError::log(raw_ostream &OS) const {
  OS << Category->message(static_cast<int>(Code)) << '\n';
}

std::error_code IndexError::convertToErrorCode() const {
  return std::error_code(static_cast<int>(Code), *Category);
}

llvm::Expected<llvm::StringMap<std::string>>
parseCrossTUIndex(StringRef IndexPath, StringRef CrossTUDir) {
  llvm::StringMap<std::string> Result;
  std::ifstream ExternalFnMapFile(IndexPath);
  if (!ExternalFnMapFile)
    return llvm::make_error<IndexError>(index_error_code::missing_index_file);

  StringRef FunctionName, FileName;
  std::string Line;
  unsigned LineNo = 0;
  while (std::getline(ExternalFnMapFile, Line)) {
    size_t Pos = Line.find(" ");
    StringRef LineRef{Line};
    if (Pos > 0 && Pos != std::string::npos) {
      FunctionName = LineRef.substr(0, Pos);
      if (Result.count(FunctionName))
        return llvm::make_error<IndexError>(
            index_error_code::multiple_definitions);
      FileName = LineRef.substr(Pos + 1);
      SmallString<256> FilePath = CrossTUDir;
      llvm::sys::path::append(FilePath, FileName);
      Result[FunctionName] = FilePath.str().str();
    } else
      return llvm::make_error<IndexError>(
          index_error_code::invalid_index_format, LineNo + 1);
    LineNo++;
  }
  return Result;
}

std::string createCrossTUIndexString(const std::vector<IndexEntry> &Index) {
  std::stringstream Result;
  for (const IndexEntry &E : Index) {
    Result << E.USR << " " << E.FilePath << '\n';
  }
  return Result.str();
}

CrossTranslationUnitContext::CrossTranslationUnitContext(CompilerInstance &CI)
    : CI(CI), Context(CI.getASTContext()) {}

CrossTranslationUnitContext::~CrossTranslationUnitContext() {}

std::string CrossTranslationUnitContext::getLookupName(const NamedDecl *ND) {
  SmallString<128> DeclUSR;
  bool Ret = index::generateUSRForDecl(ND, DeclUSR);
  assert(!Ret && "Unable to generate USR");
  return DeclUSR.str();
}

/// Recursively visits the funtion decls of a DeclContext, and looks up a
/// function based on USRs.
const FunctionDecl *
CrossTranslationUnitContext::findFunctionInDeclContext(const DeclContext *DC,
                                                       StringRef LookupFnName) {
  assert(DC && "Declaration Context must not be null");
  for (const Decl *D : DC->decls()) {
    const auto *SubDC = dyn_cast<DeclContext>(D);
    if (SubDC)
      if (const auto *FD = findFunctionInDeclContext(SubDC, LookupFnName))
        return FD;

    const auto *ND = dyn_cast<FunctionDecl>(D);
    const FunctionDecl *ResultDecl;
    if (!ND || !ND->hasBody(ResultDecl))
      continue;
    if (getLookupName(ResultDecl) != LookupFnName)
      continue;
    return ResultDecl;
  }
  return nullptr;
}

const FunctionDecl *CrossTranslationUnitContext::getCrossTUDefinition(
    const FunctionDecl *FD, StringRef CrossTUDir, StringRef IndexName) {
  assert(!FD->hasBody() && "FD has a definition in current translation unit!");

  std::string LookupFnName = getLookupName(FD);
  if (LookupFnName.empty())
    return nullptr;
  ASTUnit *Unit = nullptr;
  auto FnUnitCacheEntry = FunctionASTUnitMap.find(LookupFnName);
  if (FnUnitCacheEntry == FunctionASTUnitMap.end()) {
    if (FunctionFileMap.empty()) {
      SmallString<256> ExternalFunctionMap = CrossTUDir;
      llvm::sys::path::append(ExternalFunctionMap, IndexName);
      llvm::Expected<llvm::StringMap<std::string>> IndexOrErr =
          parseCrossTUIndex(ExternalFunctionMap, CrossTUDir);
      if (IndexOrErr) {
        FunctionFileMap = *IndexOrErr;
      } else {
        handleErrors(IndexOrErr.takeError(), [&](const IndexError &IE) {
          switch (IE.getCode()) {
          case index_error_code::missing_index_file:
            Context.getDiagnostics().Report(diag::err_fe_error_opening)
                << ExternalFunctionMap
                << "required by the CrossTU functionality";
            break;

          case index_error_code::invalid_index_format:
            Context.getDiagnostics().Report(diag::err_fnmap_parsing)
                << ExternalFunctionMap << IE.getLineNum();
          case index_error_code::multiple_definitions:
            // FIXME: do we want to return nullptr or error code or issue a
            //        warning?
            break;
          default:
            llvm_unreachable("Unexpected IndexError.");
            break;
          }
        });
        return nullptr;
      }
    }

    StringRef ASTFileName;
    auto It = FunctionFileMap.find(LookupFnName);
    if (It == FunctionFileMap.end())
      return nullptr; // No definition found even in some other build unit.
    ASTFileName = It->second;
    auto ASTCacheEntry = FileASTUnitMap.find(ASTFileName);
    if (ASTCacheEntry == FileASTUnitMap.end()) {
      IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
      TextDiagnosticPrinter *DiagClient =
          new TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
      IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());
      IntrusiveRefCntPtr<DiagnosticsEngine> Diags(
          new DiagnosticsEngine(DiagID, &*DiagOpts, DiagClient));

      std::unique_ptr<ASTUnit> LoadedUnit(ASTUnit::LoadFromASTFile(
          ASTFileName, CI.getPCHContainerOperations()->getRawReader(),
          ASTUnit::LoadEverything, Diags, CI.getFileSystemOpts()));
      Unit = LoadedUnit.get();
      FileASTUnitMap[ASTFileName] = std::move(LoadedUnit);
    } else {
      Unit = ASTCacheEntry->second.get();
    }
    FunctionASTUnitMap[LookupFnName] = Unit;
  } else {
    Unit = FnUnitCacheEntry->second;
  }

  if (!Unit)
    return nullptr;
  assert(&Unit->getFileManager() ==
         &Unit->getASTContext().getSourceManager().getFileManager());
  ASTImporter &Importer = getOrCreateASTImporter(Unit->getASTContext());
  TranslationUnitDecl *TU = Unit->getASTContext().getTranslationUnitDecl();
  if (const FunctionDecl *ResultDecl =
          findFunctionInDeclContext(TU, LookupFnName)) {
    auto *ToDecl = cast<FunctionDecl>(
        Importer.Import(const_cast<FunctionDecl *>(ResultDecl)));
    assert(ToDecl->hasBody());
    assert(FD->hasBody() && "Functions already imported should have body.");
    return ToDecl;
  }
  return nullptr;
}

ASTImporter &
CrossTranslationUnitContext::getOrCreateASTImporter(ASTContext &From) {
  auto I = ASTUnitImporterMap.find(From.getTranslationUnitDecl());
  if (I != ASTUnitImporterMap.end())
    return *I->second;
  ASTImporter *NewImporter =
      new ASTImporter(Context, Context.getSourceManager().getFileManager(),
                      From, From.getSourceManager().getFileManager(), false);
  ASTUnitImporterMap[From.getTranslationUnitDecl()].reset(NewImporter);
  return *NewImporter;
}

} // namespace cross_tu
} // namespace clang
