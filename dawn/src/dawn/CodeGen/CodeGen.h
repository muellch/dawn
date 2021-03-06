//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#pragma once

#include "dawn/AST/GridType.h"
#include "dawn/CodeGen/CXXUtil.h"
#include "dawn/CodeGen/CodeGenProperties.h"
#include "dawn/CodeGen/Options.h"
#include "dawn/CodeGen/TranslationUnit.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Support/IndexRange.h"
#include <memory>

namespace dawn {
namespace codegen {

using StencilInstantiationContext =
    std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>;

/// @brief Interface of the backend code generation
/// @ingroup codegen
class CodeGen {
protected:
  const StencilInstantiationContext& context_;
  struct codeGenOption {
    int MaxHaloPoints;
    Padding UnstrPadding;
  } codeGenOptions;

  static size_t getVerticalTmpHaloSize(iir::Stencil const& stencil);
  size_t getVerticalTmpHaloSizeForMultipleStencils(
      const std::vector<std::unique_ptr<iir::Stencil>>& stencils) const;
  virtual void addTempStorageTypedef(Structure& stencilClass, iir::Stencil const& stencil) const;
  void addTmpStorageDeclaration(
      Structure& stencilClass,
      IndexRange<const std::map<int, iir::Stencil::FieldInfo>>& tmpFields) const;
  virtual void
  addTmpStorageInit(MemberFunction& ctr, const iir::Stencil& stencil,
                    IndexRange<const std::map<int, iir::Stencil::FieldInfo>>& tempFields) const;
  void
  addTmpStorageInitStencilWrapperCtr(MemberFunction& ctr,
                                     const std::vector<std::unique_ptr<iir::Stencil>>& stencils,
                                     const std::vector<std::string>& tempFields) const;

  void generateStencilWrapperSyncMethod(Class& stencilWrapperClass) const;

  void addMplIfdefs(std::vector<std::string>& ppDefines, int mplContainerMaxSize) const;

  bool
  hasGlobalIndices(const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) const;
  bool hasGlobalIndices(const iir::Stencil& stencil) const;

  void generateGlobalIndices(const iir::Stencil& stencil, Structure& stencilClass,
                             bool genCheckOffset = true) const;

  void
  generateFieldExtentsInfo(Structure& stencilClass,
                           IndexRange<const std::map<int, iir::Stencil::FieldInfo>>& nonTempFields,
                           ast::GridType const& gridType) const;

  const std::string tmpStorageTypename_ = "tmp_storage_t";
  const std::string tmpMetadataTypename_ = "tmp_meta_data_t";
  const std::string tmpMetadataName_ = "m_tmp_meta_data";
  const std::string tmpStorageName_ = "m_tmp_storage";
  const std::string bigWrapperMetadata_ = "m_meta_data";

public:
  CodeGen(const StencilInstantiationContext& ctx, int maxHaloPoints, Padding = {});
  virtual ~CodeGen() {}

  /// @brief Generate code
  virtual std::unique_ptr<TranslationUnit> generateCode() = 0;

  static std::string getStorageType(const sir::Field& field);
  static std::string getStorageType(const iir::Stencil::FieldInfo& field);
  static std::string getStorageType(const ast::FieldDimensions& dimensions);

  void generateBoundaryConditionFunctions(
      Class& stencilWrapperClass,
      const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation) const;

  CodeGenProperties
  computeCodeGenProperties(const iir::StencilInstantiation* stencilInstantiation) const;

  virtual void generateGlobalsAPI(Structure& stencilWrapperClass,
                                  const ast::GlobalVariableMap& globalsMap,
                                  const CodeGenProperties& codeGenProperties) const;
  virtual std::string generateGlobals(const StencilInstantiationContext& context,
                                      std::string namespace_);
  virtual std::string generateGlobals(const StencilInstantiationContext& context,
                                      std::string outer_namespace_, std::string inner_namespace_);
  virtual std::string generateGlobals(const ast::GlobalVariableMap& globalsMaps,
                                      std::string namespace_) const;

  void generateBCHeaders(std::vector<std::string>& ppDefines) const;

  std::string generateFileName(const StencilInstantiationContext& context) const;
};

} // namespace codegen
} // namespace dawn
