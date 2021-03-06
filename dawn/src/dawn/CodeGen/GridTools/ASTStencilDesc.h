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

#include "dawn/CodeGen/ASTCodeGenCXX.h"
#include "dawn/CodeGen/CodeGenProperties.h"
#include "dawn/Support/StringUtil.h"
#include <stack>
#include <unordered_map>
#include <vector>

namespace dawn {
namespace codegen {
namespace gt {

/// @brief ASTVisitor to generate C++ gridtools code for the stencil and stencil function bodies
/// @ingroup gt
class ASTStencilDesc : public ASTCodeGenCXX {
protected:
  const std::shared_ptr<iir::StencilInstantiation>& instantiation_;
  const iir::StencilMetaInformation& metadata_;

  /// StencilID to the name of the generated stencils for this ID
  const CodeGenProperties& codeGenProperties_;
  const std::unordered_map<int, std::string>& stencilIdToArguments_;

public:
  using Base = ASTCodeGenCXX;
  using Base::visit;

  ASTStencilDesc(const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
                 const CodeGenProperties& codeGenProperties,
                 const std::unordered_map<int, std::string>& stencilIdToArguments);

  virtual ~ASTStencilDesc();

  /// @name Statement implementation
  /// @{
  virtual void visit(const std::shared_ptr<ast::ReturnStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<ast::VerticalRegionDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<ast::StencilCallDeclStmt>& stmt) override;
  virtual void visit(const std::shared_ptr<ast::BoundaryConditionDeclStmt>& stmt) override;
  /// @}

  /// @name Expression implementation
  /// @{
  virtual void visit(const std::shared_ptr<ast::StencilFunCallExpr>& expr) override;
  virtual void visit(const std::shared_ptr<ast::StencilFunArgExpr>& expr) override;
  virtual void visit(const std::shared_ptr<ast::VarAccessExpr>& expr) override;
  virtual void visit(const std::shared_ptr<ast::FieldAccessExpr>& expr) override;
  /// @}

  std::string getName(const std::shared_ptr<ast::VarDeclStmt>& stmt) const override;
  std::string getName(const std::shared_ptr<ast::Expr>& expr) const override;
};

} // namespace gt
} // namespace codegen
} // namespace dawn
