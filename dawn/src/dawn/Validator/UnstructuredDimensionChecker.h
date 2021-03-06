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

#include "dawn/AST/IterationSpace.h"
#include "dawn/AST/LocationType.h"
#include "dawn/IIR/ASTExpr.h"
#include "dawn/IIR/DoMethod.h"
#include "dawn/IIR/IIR.h"
#include "dawn/IIR/IIRNodeIterator.h"
#include "dawn/IIR/StencilMetaInformation.h"
#include "dawn/Support/SourceLocation.h"
#include <memory>

namespace dawn {
class UnstructuredDimensionChecker {

private:
  class UnstructuredDimensionCheckerImpl : public ast::ASTVisitorForwardingNonConst {
  public:
    enum class checkType { runOnIIR, runOnSIR };
    struct UnstructuredDimensionCheckerConfig {
      bool parentIsChainForLoop_ = false;
      std::optional<ast::UnstructuredIterationSpace> currentIterSpace_;
      UnstructuredDimensionCheckerConfig() {}
    };

  private:
    std::optional<ast::FieldDimensions> curDimensions_;
    const std::unordered_map<std::string, ast::FieldDimensions> nameToDimensions_;
    const std::unordered_map<int, std::string> idToNameMap_;
    const std::unordered_map<int, iir::LocalVariableData> idToLocalVariableData_;
    bool dimensionsConsistent_ = true;

    UnstructuredDimensionCheckerConfig config_;
    checkType checkType_ = checkType::runOnIIR;

    void checkBinaryOpUnstructured(const ast::FieldDimensions& left,
                                   const ast::FieldDimensions& right);

  public:
    void visit(const std::shared_ptr<ast::FieldAccessExpr>& stmt) override;
    void visit(const std::shared_ptr<ast::BinaryOperator>& stmt) override;
    void visit(const std::shared_ptr<ast::AssignmentExpr>& stmt) override;
    void visit(const std::shared_ptr<ast::ReductionOverNeighborExpr>& stmt) override;
    void visit(const std::shared_ptr<ast::LoopStmt>& stmt) override;
    void visit(const std::shared_ptr<ast::VarDeclStmt>& stmt) override;
    void visit(const std::shared_ptr<ast::VarAccessExpr>& stmt) override;
    void visit(const std::shared_ptr<ast::IfStmt>& stmt) override;
    void visit(const std::shared_ptr<ast::BlockStmt>& stmt) override;
    void visit(const std::shared_ptr<ast::Stmt>& stmt);

    void setCurDimensionFromLocType(iir::LocalVariableType&& type);
    bool isConsistent() const { return dimensionsConsistent_; }
    bool hasDimensions() const { return curDimensions_.has_value(); };
    bool hasHorizontalDimensions() const {
      return hasDimensions() && !curDimensions_->isVertical();
    };
    const ast::FieldDimensions& getDimensions() const;

    // This constructor is used when the check is performed on the SIR. In this case, each
    // Field is uniquely identified by its name
    UnstructuredDimensionCheckerImpl(
        const std::unordered_map<std::string, ast::FieldDimensions> nameToDimensionsMap,
        UnstructuredDimensionCheckerConfig = UnstructuredDimensionCheckerConfig());
    // This constructor is used when the check is performed from IIR. In this case, the fields may
    // have been renamed if stencils had to be merged. Hence, an additional map with key AccessID is
    // needed
    UnstructuredDimensionCheckerImpl(
        const std::unordered_map<std::string, ast::FieldDimensions> nameToDimensionsMap,
        const std::unordered_map<int, std::string> idToNameMap,
        const std::unordered_map<int, iir::LocalVariableData> idToLocalVariableData,
        UnstructuredDimensionCheckerConfig = UnstructuredDimensionCheckerConfig());
  };

public:
  /// @brief Result of check. First element indicates whether the check passed. When an
  /// inconsistency is found, the second element indicates its location in the source.
  using ConsistencyResult = std::tuple<bool, SourceLocation>;

  static ConsistencyResult checkDimensionsConsistency(const dawn::SIR&);
  static ConsistencyResult checkDimensionsConsistency(const dawn::iir::IIR&,
                                                      const iir::StencilMetaInformation&);
  static ConsistencyResult checkStageLocTypeConsistency(const dawn::iir::IIR&,
                                                        const iir::StencilMetaInformation&);
};
} // namespace dawn