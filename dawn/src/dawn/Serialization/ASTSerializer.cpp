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

#include "dawn/Serialization/ASTSerializer.h"
#include "dawn/AST/ASTExpr.h"
#include "dawn/AST/ASTStmt.h"
#include "dawn/AST/LocationType.h"
#include "dawn/IIR/ASTExpr.h"
#include "dawn/IIR/ASTStmt.h"
#include "dawn/IIR/Extents.h"
#include "dawn/IIR/IIR/IIR.pb.h"
#include "dawn/SIR/ASTStmt.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/Unreachable.h"
#include <google/protobuf/util/json_util.h>
#include <list>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

using namespace dawn;
using namespace ast;

namespace {

void fillData(iir::IIRStmtData& data, dawn::proto::ast::StmtData const& dataProto) {
  if(dataProto.has_accesses()) {
    iir::Accesses callerAccesses;
    for(auto writeAccess : dataProto.accesses().writeaccess()) {
      callerAccesses.addWriteExtent(writeAccess.first, makeExtents(&writeAccess.second));
    }
    for(auto readAccess : dataProto.accesses().readaccess()) {
      callerAccesses.addReadExtent(readAccess.first, makeExtents(&readAccess.second));
    }
    data.CallerAccesses = std::move(callerAccesses);
  }
}

std::unique_ptr<ast::StmtData> makeData(ast::StmtData::DataType dataType,
                                        dawn::proto::ast::StmtData const& dataProto) {
  if(dataType == ast::StmtData::SIR_DATA_TYPE)
    return std::make_unique<sir::SIRStmtData>();
  else {
    auto data = std::make_unique<iir::IIRStmtData>();
    fillData(*data, dataProto);
    return data;
  }
}

std::unique_ptr<ast::StmtData>
makeVarDeclStmtData(ast::StmtData::DataType dataType,
                    dawn::proto::ast::StmtData const& dataProto,
                    const dawn::proto::ast::VarDeclStmtData& varDeclStmtDataProto) {
  if(dataType == ast::StmtData::SIR_DATA_TYPE) {
    return std::make_unique<sir::SIRStmtData>();
  } else {
    auto data = std::make_unique<iir::VarDeclStmtData>();
    fillData(*data, dataProto);
    if(varDeclStmtDataProto.has_accessid())
      data->AccessID = std::make_optional(varDeclStmtDataProto.accessid().value());
    return data;
  }
}
void fillAccessExprDataFromProto(ast::Offsets& offset,
                                 const dawn::proto::ast::AccessExprData& dataProto) {
  if(dataProto.has_accessid())
    offset.setVerticalIndirectionAccessID(dataProto.accessid().value());
}
void fillAccessExprDataFromProto(iir::IIRAccessExprData& data,
                                 const dawn::proto::ast::AccessExprData& dataProto) {
  if(dataProto.has_accessid())
    data.AccessID = std::make_optional(dataProto.accessid().value());
}
void setAccessExprData(dawn::proto::ast::AccessExprData* dataProto,
                       const iir::IIRAccessExprData& data) {
  if(data.AccessID) {
    auto accessID = dataProto->mutable_accessid();
    accessID->set_value(*data.AccessID);
  }
}
void setAccessExprData(dawn::proto::ast::AccessExprData* dataProto,
                       std::optional<int> dataAccessID) {
  if(dataAccessID.has_value()) {
    auto accessID = dataProto->mutable_accessid();
    accessID->set_value(dataAccessID.value());
  }
}
void setStmtData(proto::ast::StmtData* protoStmtData, ast::Stmt& stmt) {
  if(stmt.getDataType() == ast::StmtData::IIR_DATA_TYPE) {
    if(stmt.getData<iir::IIRStmtData>().CallerAccesses.has_value()) {
      setAccesses(protoStmtData->mutable_accesses(),
                  stmt.getData<iir::IIRStmtData>().CallerAccesses);
    }
    DAWN_ASSERT_MSG(!stmt.getData<iir::IIRStmtData>().CalleeAccesses,
                    "inlining did not work as we have callee-accesses");
  }
}

void setVarDeclStmtData(dawn::proto::ast::VarDeclStmtData* dataProto,
                        const ast::VarDeclStmt& stmt) {
  if(stmt.getDataType() == ast::StmtData::IIR_DATA_TYPE) {
    if(stmt.getData<iir::VarDeclStmtData>().AccessID) {
      auto accessID = dataProto->mutable_accessid();
      accessID->set_value(*stmt.getData<iir::VarDeclStmtData>().AccessID);
    }
  }
}

} // namespace

proto::ast::LocationType getProtoLocationTypeFromLocationType(ast::LocationType locationType) {
  proto::ast::LocationType protoLocationType;
  switch(locationType) {
  case ast::LocationType::Cells:
    protoLocationType = proto::ast::LocationType::Cell;
    break;
  case ast::LocationType::Edges:
    protoLocationType = proto::ast::LocationType::Edge;
    break;
  case ast::LocationType::Vertices:
    protoLocationType = proto::ast::LocationType::Vertex;
    break;
  default:
    dawn_unreachable("unknown location type");
  }
  return protoLocationType;
}

ast::LocationType
getLocationTypeFromProtoLocationType(proto::ast::LocationType protoLocationType) {
  ast::LocationType loc;
  switch(protoLocationType) {
  case proto::ast::LocationType::Cell:
    loc = ast::LocationType::Cells;
    break;
  case proto::ast::LocationType::Edge:
    loc = ast::LocationType::Edges;
    break;
  case proto::ast::LocationType::Vertex:
    loc = ast::LocationType::Vertices;
    break;
  default:
    dawn_unreachable("unknown location type");
  }
  return loc;
}

dawn::proto::ast::Extents makeProtoExtents(dawn::iir::Extents const& extents) {
  dawn::proto::ast::Extents protoExtents;
  extent_dispatch(
      extents.horizontalExtent(),
      [&](iir::CartesianExtent const& hExtent) {
        auto cartesianExtent = protoExtents.mutable_cartesian_extent();
        auto protoIExtent = cartesianExtent->mutable_i_extent();
        protoIExtent->set_minus(hExtent.iMinus());
        protoIExtent->set_plus(hExtent.iPlus());
        auto protoJExtent = cartesianExtent->mutable_j_extent();
        protoJExtent->set_minus(hExtent.jMinus());
        protoJExtent->set_plus(hExtent.jPlus());
      },
      [&](iir::UnstructuredExtent const& hExtent) {
        auto protoHExtent = protoExtents.mutable_unstructured_extent();
        protoHExtent->set_has_extent(hExtent.hasExtent());
      },
      [&] { protoExtents.mutable_zero_extent(); });

  auto const& vExtent = extents.verticalExtent();
  auto protoVExtent = protoExtents.mutable_vertical_extent();
  if(!extents.verticalExtent().isUndefined()) {
    protoVExtent->set_minus(vExtent.minus());
    protoVExtent->set_plus(vExtent.plus());
    protoVExtent->set_undefined(false);
  } else {
    protoVExtent->set_undefined(true);
  }

  return protoExtents;
}

void setAccesses(dawn::proto::ast::Accesses* protoAccesses,
                 const std::optional<iir::Accesses>& accesses) {
  auto protoReadAccesses = protoAccesses->mutable_readaccess();
  for(auto IDExtentsPair : accesses->getReadAccesses())
    protoReadAccesses->insert({IDExtentsPair.first, makeProtoExtents(IDExtentsPair.second)});

  auto protoWriteAccesses = protoAccesses->mutable_writeaccess();
  for(auto IDExtentsPair : accesses->getWriteAccesses())
    protoWriteAccesses->insert({IDExtentsPair.first, makeProtoExtents(IDExtentsPair.second)});
}

iir::Extents makeExtents(const dawn::proto::ast::Extents* protoExtents) {
  using ProtoExtents = dawn::proto::ast::Extents;
  iir::Extent vExtent;
  if(protoExtents->vertical_extent().undefined()) {
    vExtent = iir::Extent(iir::UndefinedExtent{});
  } else {
    vExtent = iir::Extent(protoExtents->vertical_extent().minus(),
                          protoExtents->vertical_extent().plus());
  }

  switch(protoExtents->horizontal_extent_case()) {
  case ProtoExtents::kCartesianExtent: {
    auto const& hExtent = protoExtents->cartesian_extent();
    return {iir::HorizontalExtent{ast::cartesian, hExtent.i_extent().minus(),
                                  hExtent.i_extent().plus(), hExtent.j_extent().minus(),
                                  hExtent.j_extent().plus()},
            vExtent};
  }
  case ProtoExtents::kUnstructuredExtent: {
    auto const& hExtent = protoExtents->unstructured_extent();
    return {iir::HorizontalExtent{ast::unstructured, hExtent.has_extent()}, vExtent};
  }
  case ProtoExtents::kZeroExtent:
    return iir::Extents{iir::HorizontalExtent{}, vExtent};
  default:
    dawn_unreachable("unknown extent");
  }
}

void setAST(dawn::proto::ast::AST* astProto, const AST* ast);

void setLocation(dawn::proto::ast::SourceLocation* locProto, const SourceLocation& loc) {
  locProto->set_column(loc.Column);
  locProto->set_line(loc.Line);
}

void setBuiltinType(dawn::proto::ast::BuiltinType* builtinTypeProto,
                    const BuiltinTypeID& builtinType) {
  builtinTypeProto->set_type_id(
      static_cast<dawn::proto::ast::BuiltinType_TypeID>(builtinType));
}

void setInterval(dawn::proto::ast::Interval* intervalProto, const Interval* interval) {
  if(interval->LowerLevel == Interval::Start)
    intervalProto->set_special_lower_level(dawn::proto::ast::Interval::Start);
  else if(interval->LowerLevel == Interval::End)
    intervalProto->set_special_lower_level(dawn::proto::ast::Interval::End);
  else
    intervalProto->set_lower_level(interval->LowerLevel);

  if(interval->UpperLevel == Interval::Start)
    intervalProto->set_special_upper_level(dawn::proto::ast::Interval::Start);
  else if(interval->UpperLevel == Interval::End)
    intervalProto->set_special_upper_level(dawn::proto::ast::Interval::End);
  else
    intervalProto->set_upper_level(interval->UpperLevel);

  intervalProto->set_lower_offset(interval->LowerOffset);
  intervalProto->set_upper_offset(interval->UpperOffset);
}

void setDirection(dawn::proto::ast::Direction* directionProto,
                  const sir::Direction* direction) {
  directionProto->set_name(direction->Name);
  setLocation(directionProto->mutable_loc(), direction->Loc);
}

void setOffset(dawn::proto::ast::Offset* offsetProto, const sir::Offset* offset) {
  offsetProto->set_name(offset->Name);
  setLocation(offsetProto->mutable_loc(), offset->Loc);
}

void setFieldDimensions(dawn::proto::ast::FieldDimensions* protoFieldDimensions,
                        const ast::FieldDimensions& fieldDimensions) {
  protoFieldDimensions->set_mask_k(fieldDimensions.K());
  if(!fieldDimensions.isVertical()) {
    if(dawn::ast::dimension_isa<ast::CartesianFieldDimension const&>(
           fieldDimensions.getHorizontalFieldDimension())) {
      auto const& cartesianDimension =
          dawn::ast::dimension_cast<dawn::ast::CartesianFieldDimension const&>(
              fieldDimensions.getHorizontalFieldDimension());

      dawn::proto::ast::CartesianDimension* protoCartesianDimension =
          protoFieldDimensions->mutable_cartesian_horizontal_dimension();

      protoCartesianDimension->set_mask_cart_i(cartesianDimension.I());
      protoCartesianDimension->set_mask_cart_j(cartesianDimension.J());

    } else {
      auto const& unstructuredDimension =
          dawn::ast::dimension_cast<dawn::ast::UnstructuredFieldDimension const&>(
              fieldDimensions.getHorizontalFieldDimension());

      auto protoIterSpace =
          protoFieldDimensions->mutable_unstructured_horizontal_dimension()->mutable_iter_space();

      if(unstructuredDimension.isSparse()) {
        for(auto locType : unstructuredDimension.getNeighborChain()) {
          protoIterSpace->add_chain(getProtoLocationTypeFromLocationType(locType));
        }
      } else {
        protoIterSpace->add_chain(
            getProtoLocationTypeFromLocationType(unstructuredDimension.getDenseLocationType()));
      }
      protoIterSpace->set_include_center(unstructuredDimension.getIncludeCenter());
    }
  }
}

void setField(dawn::proto::ast::Field* fieldProto, const sir::Field* field) {
  fieldProto->set_name(field->Name);
  fieldProto->set_is_temporary(field->IsTemporary);
  setLocation(fieldProto->mutable_loc(), field->Loc);

  setFieldDimensions(fieldProto->mutable_field_dimensions(), field->Dimensions);
}

ProtoStmtBuilder::ProtoStmtBuilder(dawn::proto::ast::Stmt* stmtProto,
                                   dawn::ast::StmtData::DataType dataType)
    : dataType_(dataType) {
  currentStmtProto_.push(stmtProto);
}

ProtoStmtBuilder::ProtoStmtBuilder(dawn::proto::ast::Expr* exprProto,
                                   dawn::ast::StmtData::DataType dataType)
    : dataType_(dataType) {
  currentExprProto_.push(exprProto);
}

dawn::proto::ast::Stmt* ProtoStmtBuilder::getCurrentStmtProto() {
  DAWN_ASSERT(!currentStmtProto_.empty());
  return currentStmtProto_.top();
}

dawn::proto::ast::Expr* ProtoStmtBuilder::getCurrentExprProto() {
  DAWN_ASSERT(!currentExprProto_.empty());
  return currentExprProto_.top();
}

void ProtoStmtBuilder::visit(const std::shared_ptr<BlockStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_block_stmt();

  for(const auto& s : stmt->getStatements()) {
    currentStmtProto_.push(protoStmt->add_statements());
    s->accept(*this);
    currentStmtProto_.pop();
  }

  setStmtData(protoStmt->mutable_data(), *stmt);

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());
  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<LoopStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_loop_stmt();

  currentStmtProto_.push(protoStmt->mutable_statements());
  stmt->getBlockStmt()->accept(*this);
  currentStmtProto_.pop();

  const auto* descrPtr = stmt->getIterationDescrPtr();
  const auto maybeChainPtr = dynamic_cast<const ChainIterationDescr*>(descrPtr);
  if(maybeChainPtr) {
    auto protoChainDescrIterSpace =
        protoStmt->mutable_loop_descriptor()->mutable_loop_descriptor_chain()->mutable_iter_space();
    for(auto loc : maybeChainPtr->getChain()) {
      protoChainDescrIterSpace->add_chain(getProtoLocationTypeFromLocationType(loc));
    }
    protoChainDescrIterSpace->set_include_center(maybeChainPtr->getIncludeCenter());
  } else {
    dawn_unreachable("Loop descriptor not implemented.");
  }

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());
  setStmtData(protoStmt->mutable_data(), *stmt);
  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<ExprStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_expr_stmt();
  currentExprProto_.push(protoStmt->mutable_expr());
  stmt->getExpr()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<ReturnStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_return_stmt();

  currentExprProto_.push(protoStmt->mutable_expr());
  stmt->getExpr()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<VarDeclStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_var_decl_stmt();

  if(stmt->getType().isBuiltinType())
    setBuiltinType(protoStmt->mutable_type()->mutable_builtin_type(),
                   stmt->getType().getBuiltinTypeID());
  else
    protoStmt->mutable_type()->set_name(stmt->getType().getName());
  protoStmt->mutable_type()->set_is_const(stmt->getType().isConst());
  protoStmt->mutable_type()->set_is_volatile(stmt->getType().isVolatile());

  protoStmt->set_name(stmt->getName());
  protoStmt->set_dimension(stmt->getDimension());
  protoStmt->set_op(stmt->getOp());

  for(const auto& expr : stmt->getInitList()) {
    currentExprProto_.push(protoStmt->add_init_list());
    expr->accept(*this);
    currentExprProto_.pop();
  }

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setVarDeclStmtData(protoStmt->mutable_var_decl_stmt_data(), *stmt);

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<VerticalRegionDeclStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_vertical_region_decl_stmt();

  dawn::sir::VerticalRegion* verticalRegion = stmt->getVerticalRegion().get();
  dawn::proto::ast::VerticalRegion* verticalRegionProto =
      protoStmt->mutable_vertical_region();

  // VerticalRegion.Loc
  setLocation(verticalRegionProto->mutable_loc(), verticalRegion->Loc);

  // VerticalRegion.Ast
  setAST(verticalRegionProto->mutable_ast(), verticalRegion->Ast.get());

  // VerticalRegion.VerticalInterval
  setInterval(verticalRegionProto->mutable_interval(), verticalRegion->VerticalInterval.get());

  // VerticalRegion.LoopOrder
  verticalRegionProto->set_loop_order(verticalRegion->LoopOrder ==
                                              dawn::sir::VerticalRegion::LoopOrderKind::Backward
                                          ? dawn::proto::ast::VerticalRegion::Backward
                                          : dawn::proto::ast::VerticalRegion::Forward);

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());

  // VerticalRegion.VerticalInterval
  if(verticalRegion->IterationSpace[0]) {
    setInterval(verticalRegionProto->mutable_i_range(), &verticalRegion->IterationSpace[0].value());
  }
  if(verticalRegion->IterationSpace[1]) {
    setInterval(verticalRegionProto->mutable_j_range(), &verticalRegion->IterationSpace[1].value());
  }
}

void ProtoStmtBuilder::visit(const std::shared_ptr<StencilCallDeclStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_stencil_call_decl_stmt();

  dawn::ast::StencilCall* stencilCall = stmt->getStencilCall().get();
  dawn::proto::ast::StencilCall* stencilCallProto = protoStmt->mutable_stencil_call();

  // StencilCall.Loc
  setLocation(stencilCallProto->mutable_loc(), stencilCall->Loc);

  // StencilCall.Callee
  stencilCallProto->set_callee(stencilCall->Callee);

  // StencilCall.Args
  for(const auto& argName : stencilCall->Args) {
    stencilCallProto->add_arguments(argName);
  }

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<BoundaryConditionDeclStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_boundary_condition_decl_stmt();
  protoStmt->set_functor(stmt->getFunctor());

  for(const auto& fieldName : stmt->getFields())
    protoStmt->add_fields(fieldName);

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<IfStmt>& stmt) {
  auto protoStmt = getCurrentStmtProto()->mutable_if_stmt();

  currentStmtProto_.push(protoStmt->mutable_cond_part());
  stmt->getCondStmt()->accept(*this);
  currentStmtProto_.pop();

  currentStmtProto_.push(protoStmt->mutable_then_part());
  stmt->getThenStmt()->accept(*this);
  currentStmtProto_.pop();

  if(stmt->getElseStmt()) {
    currentStmtProto_.push(protoStmt->mutable_else_part());
    stmt->getElseStmt()->accept(*this);
    currentStmtProto_.pop();
  }

  setLocation(protoStmt->mutable_loc(), stmt->getSourceLocation());

  setStmtData(protoStmt->mutable_data(), *stmt);

  protoStmt->set_id(stmt->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<UnaryOperator>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_unary_operator();
  protoExpr->set_op(expr->getOp());

  currentExprProto_.push(protoExpr->mutable_operand());
  expr->getOperand()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<BinaryOperator>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_binary_operator();
  protoExpr->set_op(expr->getOp());

  currentExprProto_.push(protoExpr->mutable_left());
  expr->getLeft()->accept(*this);
  currentExprProto_.pop();

  currentExprProto_.push(protoExpr->mutable_right());
  expr->getRight()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<AssignmentExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_assignment_expr();
  protoExpr->set_op(expr->getOp());

  currentExprProto_.push(protoExpr->mutable_left());
  expr->getLeft()->accept(*this);
  currentExprProto_.pop();

  currentExprProto_.push(protoExpr->mutable_right());
  expr->getRight()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<TernaryOperator>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_ternary_operator();

  currentExprProto_.push(protoExpr->mutable_cond());
  expr->getCondition()->accept(*this);
  currentExprProto_.pop();

  currentExprProto_.push(protoExpr->mutable_left());
  expr->getLeft()->accept(*this);
  currentExprProto_.pop();

  currentExprProto_.push(protoExpr->mutable_right());
  expr->getRight()->accept(*this);
  currentExprProto_.pop();

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<FunCallExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_fun_call_expr();
  protoExpr->set_callee(expr->getCallee());

  for(const auto& arg : expr->getArguments()) {
    currentExprProto_.push(protoExpr->add_arguments());
    arg->accept(*this);
    currentExprProto_.pop();
  }

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<StencilFunCallExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_stencil_fun_call_expr();
  protoExpr->set_callee(expr->getCallee());

  for(const auto& arg : expr->getArguments()) {
    currentExprProto_.push(protoExpr->add_arguments());
    arg->accept(*this);
    currentExprProto_.pop();
  }

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<StencilFunArgExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_stencil_fun_arg_expr();

  protoExpr->mutable_dimension()->set_direction(
      expr->getDimension() == -1
          ? dawn::proto::ast::Dimension::Invalid
          : static_cast<dawn::proto::ast::Dimension_Direction>(expr->getDimension()));
  protoExpr->set_offset(expr->getOffset());
  protoExpr->set_argument_index(expr->getArgumentIndex());

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<VarAccessExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_var_access_expr();

  protoExpr->set_name(expr->getName());
  protoExpr->set_is_external(expr->isExternal());

  if(expr->isArrayAccess()) {
    currentExprProto_.push(protoExpr->mutable_index());
    expr->getIndex()->accept(*this);
    currentExprProto_.pop();
  }

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  if(dataType_ == StmtData::IIR_DATA_TYPE)
    setAccessExprData(protoExpr->mutable_data(), expr->getData<iir::IIRAccessExprData>());
  else
    protoExpr->mutable_data();
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<FieldAccessExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_field_access_expr();

  protoExpr->set_name(expr->getName());

  auto const& offset = expr->getOffset();
  ast::offset_dispatch(
      offset.horizontalOffset(),
      [&](ast::CartesianOffset const& hOffset) {
        protoExpr->mutable_cartesian_offset()->set_i_offset(hOffset.offsetI());
        protoExpr->mutable_cartesian_offset()->set_j_offset(hOffset.offsetJ());
      },
      [&](ast::UnstructuredOffset const& hOffset) {
        protoExpr->mutable_unstructured_offset()->set_has_offset(hOffset.hasOffset());
      },
      [&] { protoExpr->mutable_zero_offset(); });
  protoExpr->set_vertical_shift(offset.verticalShift());
  if(offset.hasVerticalIndirection()) {
    protoExpr->set_vertical_indirection(offset.getVerticalIndirectionFieldName());
    if(dataType_ == StmtData::IIR_DATA_TYPE) {
      setAccessExprData(protoExpr->mutable_vertical_indirection_data(),
                        offset.getVerticalIndirectionAccessID());
    }
  }

  for(int argOffset : expr->getArgumentOffset())
    protoExpr->add_argument_offset(argOffset);

  for(int argMap : expr->getArgumentMap())
    protoExpr->add_argument_map(argMap);

  protoExpr->set_negate_offset(expr->negateOffset());

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  if(dataType_ == StmtData::IIR_DATA_TYPE)
    setAccessExprData(protoExpr->mutable_data(), expr->getData<iir::IIRAccessExprData>());
  else
    protoExpr->mutable_data();
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<LiteralAccessExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_literal_access_expr();

  protoExpr->set_value(expr->getValue());
  setBuiltinType(protoExpr->mutable_type(), expr->getBuiltinType());

  setLocation(protoExpr->mutable_loc(), expr->getSourceLocation());
  if(dataType_ == StmtData::IIR_DATA_TYPE)
    setAccessExprData(protoExpr->mutable_data(), expr->getData<iir::IIRAccessExprData>());
  else
    protoExpr->mutable_data();
  protoExpr->set_id(expr->getID());
}

void ProtoStmtBuilder::visit(const std::shared_ptr<ReductionOverNeighborExpr>& expr) {
  auto protoExpr = getCurrentExprProto()->mutable_reduction_over_neighbor_expr();

  protoExpr->set_op(expr->getOp());

  auto protoIterSpace = protoExpr->mutable_iter_space();
  for(const auto& loc : expr->getNbhChain()) {
    protoIterSpace->add_chain(getProtoLocationTypeFromLocationType(loc));
  }
  protoIterSpace->set_include_center(expr->getIncludeCenter());

  currentExprProto_.push(protoExpr->mutable_rhs());
  expr->getRhs()->accept(*this);
  currentExprProto_.pop();

  currentExprProto_.push(protoExpr->mutable_init());
  expr->getInit()->accept(*this);
  currentExprProto_.pop();

  if(expr->getWeights()) {
    for(const auto& weight : expr->getWeights().value()) {
      currentExprProto_.push(protoExpr->add_weights());
      weight->accept(*this);
      currentExprProto_.pop();
    }
  }
}

void setAST(proto::ast::AST* astProto, const AST* ast) {
  // Dynamically determine data type
  auto dataType = ast->getRoot()->getDataType();
  ProtoStmtBuilder builder(astProto->mutable_root(), dataType);
  ast->accept(builder);
}

//===------------------------------------------------------------------------------------------===//
// Deserialization
//===------------------------------------------------------------------------------------------===//

ast::FieldDimensions
makeFieldDimensions(const proto::ast::FieldDimensions& protoFieldDimensions) {

  if(protoFieldDimensions.has_cartesian_horizontal_dimension()) {
    const auto& protoCartesianDimension = protoFieldDimensions.cartesian_horizontal_dimension();
    return ast::FieldDimensions(
        ast::HorizontalFieldDimension(
            dawn::ast::cartesian,
            std::array<bool, 2>({(bool)protoCartesianDimension.mask_cart_i(),
                                 (bool)protoCartesianDimension.mask_cart_j()})),
        (bool)protoFieldDimensions.mask_k());
  } else if(protoFieldDimensions.has_unstructured_horizontal_dimension()) {

    const auto& protoUnstructuredDimension =
        protoFieldDimensions.unstructured_horizontal_dimension();

    NeighborChain neighborChain;
    for(int i = 0; i < protoUnstructuredDimension.iter_space().chain_size(); ++i) {
      neighborChain.push_back(
          getLocationTypeFromProtoLocationType(protoUnstructuredDimension.iter_space().chain(i)));
    }

    return ast::FieldDimensions(
        ast::HorizontalFieldDimension(dawn::ast::unstructured, neighborChain,
                                      protoUnstructuredDimension.iter_space().include_center()),
        protoFieldDimensions.mask_k());

  } else {
    return ast::FieldDimensions(protoFieldDimensions.mask_k());
  }
}

BuiltinTypeID makeBuiltinTypeID(const proto::ast::BuiltinType& builtinTypeProto) {
  switch(builtinTypeProto.type_id()) {
  case proto::ast::BuiltinType_TypeID_Invalid:
    return BuiltinTypeID::Invalid;
  case proto::ast::BuiltinType_TypeID_Auto:
    return BuiltinTypeID::Auto;
  case proto::ast::BuiltinType_TypeID_Boolean:
    return BuiltinTypeID::Boolean;
  case proto::ast::BuiltinType_TypeID_Integer:
    return BuiltinTypeID::Integer;
  case proto::ast::BuiltinType_TypeID_Float:
    return BuiltinTypeID::Float;
  case proto::ast::BuiltinType_TypeID_Double:
    return BuiltinTypeID::Double;
  default:
    return BuiltinTypeID::Invalid;
  }
  return BuiltinTypeID::Invalid;
}

std::shared_ptr<sir::Direction> makeDirection(const proto::ast::Direction& directionProto) {
  return std::make_shared<sir::Direction>(directionProto.name(), makeLocation(directionProto));
}

std::shared_ptr<sir::Offset> makeOffset(const proto::ast::Offset& offsetProto) {
  return std::make_shared<sir::Offset>(offsetProto.name(), makeLocation(offsetProto));
}

std::shared_ptr<ast::Interval> makeInterval(const proto::ast::Interval& intervalProto) {
  int lowerLevel = -1, upperLevel = -1, lowerOffset = -1, upperOffset = -1;

  if(intervalProto.LowerLevel_case() == proto::ast::Interval::kSpecialLowerLevel)
    lowerLevel = intervalProto.special_lower_level() ==
                         proto::ast::Interval_SpecialLevel::Interval_SpecialLevel_Start
                     ? ast::Interval::Start
                     : ast::Interval::End;
  else
    lowerLevel = intervalProto.lower_level();

  if(intervalProto.UpperLevel_case() == proto::ast::Interval::kSpecialUpperLevel)
    upperLevel = intervalProto.special_upper_level() ==
                         proto::ast::Interval_SpecialLevel::Interval_SpecialLevel_Start
                     ? ast::Interval::Start
                     : ast::Interval::End;
  else
    upperLevel = intervalProto.upper_level();

  lowerOffset = intervalProto.lower_offset();
  upperOffset = intervalProto.upper_offset();
  return std::make_shared<ast::Interval>(lowerLevel, upperLevel, lowerOffset, upperOffset);
}

std::shared_ptr<Expr> makeExpr(const proto::ast::Expr& expressionProto,
                               ast::StmtData::DataType dataType, int& maxID) {
  switch(expressionProto.expr_case()) {
  case proto::ast::Expr::kUnaryOperator: {
    const auto& exprProto = expressionProto.unary_operator();
    auto expr = std::make_shared<UnaryOperator>(makeExpr(exprProto.operand(), dataType, maxID),
                                                exprProto.op(), makeLocation(exprProto));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kBinaryOperator: {
    const auto& exprProto = expressionProto.binary_operator();
    auto expr = std::make_shared<BinaryOperator>(
        makeExpr(exprProto.left(), dataType, maxID), exprProto.op(),
        makeExpr(exprProto.right(), dataType, maxID), makeLocation(exprProto));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kAssignmentExpr: {
    const auto& exprProto = expressionProto.assignment_expr();
    auto expr = std::make_shared<AssignmentExpr>(makeExpr(exprProto.left(), dataType, maxID),
                                                 makeExpr(exprProto.right(), dataType, maxID),
                                                 exprProto.op(), makeLocation(exprProto));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kTernaryOperator: {
    const auto& exprProto = expressionProto.ternary_operator();
    auto expr = std::make_shared<TernaryOperator>(
        makeExpr(exprProto.cond(), dataType, maxID), makeExpr(exprProto.left(), dataType, maxID),
        makeExpr(exprProto.right(), dataType, maxID), makeLocation(exprProto));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kFunCallExpr: {
    const auto& exprProto = expressionProto.fun_call_expr();
    auto expr = std::make_shared<FunCallExpr>(exprProto.callee(), makeLocation(exprProto));
    for(const auto& argProto : exprProto.arguments())
      expr->getArguments().emplace_back(makeExpr(argProto, dataType, maxID));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kStencilFunCallExpr: {
    const auto& exprProto = expressionProto.stencil_fun_call_expr();
    auto expr = std::make_shared<StencilFunCallExpr>(exprProto.callee(), makeLocation(exprProto));
    for(const auto& argProto : exprProto.arguments())
      expr->getArguments().emplace_back(makeExpr(argProto, dataType, maxID));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kStencilFunArgExpr: {
    const auto& exprProto = expressionProto.stencil_fun_arg_expr();
    int direction = -1, offset = 0, argumentIndex = -1; // default values

    if(exprProto.has_dimension()) {
      switch(exprProto.dimension().direction()) {
      case proto::ast::Dimension_Direction_I:
        direction = 0;
        break;
      case proto::ast::Dimension_Direction_J:
        direction = 1;
        break;
      case proto::ast::Dimension_Direction_K:
        direction = 2;
        break;
      case proto::ast::Dimension_Direction_Invalid:
      default:
        direction = -1;
        break;
      }
    }
    offset = exprProto.offset();
    argumentIndex = exprProto.argument_index();
    auto expr = std::make_shared<StencilFunArgExpr>(direction, offset, argumentIndex,
                                                    makeLocation(exprProto));
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kVarAccessExpr: {
    const auto& exprProto = expressionProto.var_access_expr();
    auto expr = std::make_shared<VarAccessExpr>(
        exprProto.name(),
        exprProto.has_index() ? makeExpr(exprProto.index(), dataType, maxID) : nullptr,
        makeLocation(exprProto));
    expr->setIsExternal(exprProto.is_external());
    if(dataType == StmtData::IIR_DATA_TYPE)
      fillAccessExprDataFromProto(expr->getData<iir::IIRAccessExprData>(), exprProto.data());
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kFieldAccessExpr: {

    using ProtoFieldAccessExpr = dawn::proto::ast::FieldAccessExpr;
    const auto& exprProto = expressionProto.field_access_expr();
    auto name = exprProto.name();
    auto negateOffset = exprProto.negate_offset();

    auto throwException = [&exprProto](const char* member) {
      throw std::runtime_error(format("FieldAccessExpr::%s (loc %s) exceeds 3 dimensions", member,
                                      makeLocation(exprProto)));
    };

    ast::Offsets offset;
    switch(exprProto.horizontal_offset_case()) {
    case ProtoFieldAccessExpr::kCartesianOffset: {
      auto const& hOffset = exprProto.cartesian_offset();
      if(!exprProto.vertical_indirection().empty()) {
        offset = ast::Offsets{ast::cartesian, hOffset.i_offset(), hOffset.j_offset(),
                              exprProto.vertical_shift(), exprProto.vertical_indirection()};
        if(dataType == StmtData::IIR_DATA_TYPE)
          fillAccessExprDataFromProto(offset, exprProto.vertical_indirection_data());
      } else {
        offset = ast::Offsets{ast::cartesian, hOffset.i_offset(), hOffset.j_offset(),
                              exprProto.vertical_shift()};
      }
      break;
    }
    case ProtoFieldAccessExpr::kUnstructuredOffset: {
      auto const& hOffset = exprProto.unstructured_offset();
      if(!exprProto.vertical_indirection().empty()) {
        offset = ast::Offsets{ast::unstructured, hOffset.has_offset(), exprProto.vertical_shift(),
                              exprProto.vertical_indirection()};
        if(dataType == StmtData::IIR_DATA_TYPE && offset.hasVerticalIndirection())
          fillAccessExprDataFromProto(offset, exprProto.vertical_indirection_data());
      } else {
        offset = ast::Offsets{ast::unstructured, hOffset.has_offset(), exprProto.vertical_shift()};
      }
      break;
    }
    case ProtoFieldAccessExpr::kZeroOffset:
      if(!exprProto.vertical_indirection().empty()) {
        offset = ast::Offsets{ast::HorizontalOffset{}, exprProto.vertical_shift(),
                              exprProto.vertical_indirection()};
        if(dataType == StmtData::IIR_DATA_TYPE && offset.hasVerticalIndirection())
          fillAccessExprDataFromProto(offset, exprProto.vertical_indirection_data());
      } else {
        offset = ast::Offsets{ast::HorizontalOffset{}, exprProto.vertical_shift()};
      }
      break;
    default:
      dawn_unreachable("unknown offset");
    }

    Array3i argumentOffset{{0, 0, 0}};
    if(!exprProto.argument_offset().empty()) {
      if(exprProto.argument_offset().size() > 3)
        throwException("argument_offset");

      std::copy(exprProto.argument_offset().begin(), exprProto.argument_offset().end(),
                argumentOffset.begin());
    }

    Array3i argumentMap{{-1, -1, -1}};
    if(!exprProto.argument_map().empty()) {
      if(exprProto.argument_map().size() > 3)
        throwException("argument_map");

      std::copy(exprProto.argument_map().begin(), exprProto.argument_map().end(),
                argumentMap.begin());
    }

    auto expr = std::make_shared<FieldAccessExpr>(name, offset, argumentMap, argumentOffset,
                                                  negateOffset, makeLocation(exprProto));
    if(dataType == StmtData::IIR_DATA_TYPE)
      fillAccessExprDataFromProto(expr->getData<iir::IIRAccessExprData>(), exprProto.data());
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kLiteralAccessExpr: {
    const auto& exprProto = expressionProto.literal_access_expr();
    auto expr = std::make_shared<LiteralAccessExpr>(
        exprProto.value(), makeBuiltinTypeID(exprProto.type()), makeLocation(exprProto));
    if(dataType == StmtData::IIR_DATA_TYPE)
      fillAccessExprDataFromProto(expr->getData<iir::IIRAccessExprData>(), exprProto.data());
    expr->setID(exprProto.id());
    maxID = std::max(std::abs(exprProto.id()), maxID);
    return expr;
  }
  case proto::ast::Expr::kReductionOverNeighborExpr: {
    const auto& exprProto = expressionProto.reduction_over_neighbor_expr();
    auto weights = exprProto.weights();

    ast::NeighborChain chain;
    for(int i = 0; i < exprProto.iter_space().chain_size(); ++i) {
      chain.push_back(getLocationTypeFromProtoLocationType(exprProto.iter_space().chain(i)));
    }

    if(weights.empty()) {
      auto expr = std::make_shared<ReductionOverNeighborExpr>(
          exprProto.op(), makeExpr(exprProto.rhs(), dataType, maxID),
          makeExpr(exprProto.init(), dataType, maxID), chain,
          exprProto.iter_space().include_center(), makeLocation(exprProto));
      return expr;
    } else {
      std::vector<std::shared_ptr<ast::Expr>> deserializedWeights;
      for(const auto weight : weights) {
        deserializedWeights.push_back(makeExpr(weight, dataType, maxID));
      }
      auto expr = std::make_shared<ReductionOverNeighborExpr>(
          exprProto.op(), makeExpr(exprProto.rhs(), dataType, maxID),
          makeExpr(exprProto.init(), dataType, maxID), deserializedWeights, chain,
          exprProto.iter_space().include_center(), makeLocation(exprProto));
      return expr;
    }
  }
  case proto::ast::Expr::EXPR_NOT_SET:
  default:
    dawn_unreachable("expr not set");
  }
  return nullptr;
}

std::shared_ptr<Stmt> makeStmt(const proto::ast::Stmt& statementProto,
                               ast::StmtData::DataType dataType, int& maxID) {
  switch(statementProto.stmt_case()) {
  case proto::ast::Stmt::kBlockStmt: {
    const auto& stmtProto = statementProto.block_stmt();
    auto stmt =
        std::make_shared<BlockStmt>(makeData(dataType, stmtProto.data()), makeLocation(stmtProto));

    for(const auto& s : stmtProto.statements())
      stmt->push_back(makeStmt(s, dataType, maxID));
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kLoopStmt: {
    const auto& stmtProto = statementProto.loop_stmt();
    const auto& blockStmt = makeStmt(stmtProto.statements(), dataType, maxID);
    DAWN_ASSERT_MSG(blockStmt->getKind() == Stmt::Kind::BlockStmt, "Expected a BlockStmt.");

    switch(stmtProto.loop_descriptor().desc_case()) {
    case dawn::proto::ast::LoopDescriptor::kLoopDescriptorChain: {

      ast::NeighborChain chain;
      for(int i = 0;
          i < stmtProto.loop_descriptor().loop_descriptor_chain().iter_space().chain_size(); ++i) {
        chain.push_back(getLocationTypeFromProtoLocationType(
            stmtProto.loop_descriptor().loop_descriptor_chain().iter_space().chain(i)));
      }
      auto stmt = std::make_shared<LoopStmt>(
          makeData(dataType, stmtProto.data()), std::move(chain),
          stmtProto.loop_descriptor().loop_descriptor_chain().iter_space().include_center(),
          std::dynamic_pointer_cast<BlockStmt>(blockStmt), makeLocation(stmtProto));
      stmt->setID(stmtProto.id());
      maxID = std::max(std::abs(stmtProto.id()), maxID);
      return stmt;
    }
    case dawn::proto::ast::LoopDescriptor::kLoopDescriptorGeneral: {
      dawn_unreachable("general loop bounds not implemented!\n");
      break;
    }
    default:
      dawn_unreachable("descriptor not set!\n");
    }
  }
  case proto::ast::Stmt::kExprStmt: {
    const auto& stmtProto = statementProto.expr_stmt();
    auto stmt = std::make_shared<ExprStmt>(makeData(dataType, stmtProto.data()),
                                           makeExpr(stmtProto.expr(), dataType, maxID),
                                           makeLocation(stmtProto));
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kReturnStmt: {
    const auto& stmtProto = statementProto.return_stmt();
    auto stmt = std::make_shared<ReturnStmt>(makeData(dataType, stmtProto.data()),
                                             makeExpr(stmtProto.expr(), dataType, maxID),
                                             makeLocation(stmtProto));
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kVarDeclStmt: {
    const auto& stmtProto = statementProto.var_decl_stmt();

    std::vector<std::shared_ptr<Expr>> initList;
    for(const auto& e : stmtProto.init_list())
      initList.emplace_back(makeExpr(e, dataType, maxID));

    const proto::ast::Type& typeProto = stmtProto.type();
    CVQualifier cvQual = CVQualifier::Invalid;
    if(typeProto.is_const())
      cvQual |= CVQualifier::Const;
    if(typeProto.is_volatile())
      cvQual |= CVQualifier::Volatile;
    Type type = typeProto.name().empty() ? Type(makeBuiltinTypeID(typeProto.builtin_type()), cvQual)
                                         : Type(typeProto.name(), cvQual);

    auto stmt = std::make_shared<VarDeclStmt>(
        makeVarDeclStmtData(dataType, stmtProto.data(), stmtProto.var_decl_stmt_data()), type,
        stmtProto.name(), stmtProto.dimension(), stmtProto.op().c_str(), initList,
        makeLocation(stmtProto));
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kStencilCallDeclStmt: {
    auto metaloc = makeLocation(statementProto.stencil_call_decl_stmt());
    const auto& stmtProto = statementProto.stencil_call_decl_stmt();
    auto loc = makeLocation(stmtProto.stencil_call());
    std::shared_ptr<ast::StencilCall> call =
        std::make_shared<ast::StencilCall>(stmtProto.stencil_call().callee(), loc);
    for(const auto& argName : stmtProto.stencil_call().arguments()) {
      call->Args.push_back(argName);
    }
    auto stmt =
        std::make_shared<StencilCallDeclStmt>(makeData(dataType, stmtProto.data()), call, metaloc);
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kVerticalRegionDeclStmt: {
    const auto& stmtProto = statementProto.vertical_region_decl_stmt();
    auto loc = makeLocation(stmtProto.vertical_region());
    std::shared_ptr<ast::Interval> interval = makeInterval(stmtProto.vertical_region().interval());
    sir::VerticalRegion::LoopOrderKind looporder;
    switch(stmtProto.vertical_region().loop_order()) {
    case proto::ast::VerticalRegion_LoopOrder_Forward:
      looporder = sir::VerticalRegion::LoopOrderKind::Forward;
      break;
    case proto::ast::VerticalRegion_LoopOrder_Backward:
      looporder = sir::VerticalRegion::LoopOrderKind::Backward;
      break;
    default:
      dawn_unreachable("no looporder specified");
    }
    auto ast = makeAST(stmtProto.vertical_region().ast(), dataType, maxID);
    std::shared_ptr<sir::VerticalRegion> verticalRegion =
        std::make_shared<sir::VerticalRegion>(ast, interval, looporder, loc);
    auto stmt = std::make_shared<VerticalRegionDeclStmt>(makeData(dataType, stmtProto.data()),
                                                         verticalRegion, loc);
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    if(stmtProto.vertical_region().has_i_range()) {
      auto range = stmtProto.vertical_region().i_range();
      verticalRegion->IterationSpace[0] = *makeInterval(range);
    }
    if(stmtProto.vertical_region().has_j_range()) {
      auto range = stmtProto.vertical_region().j_range();
      verticalRegion->IterationSpace[1] = *makeInterval(range);
      ;
    }
    return stmt;
  }
  case proto::ast::Stmt::kBoundaryConditionDeclStmt: {
    const auto& stmtProto = statementProto.boundary_condition_decl_stmt();
    auto stmt = std::make_shared<BoundaryConditionDeclStmt>(
        makeData(dataType, stmtProto.data()), stmtProto.functor(), makeLocation(stmtProto));
    for(const auto& fieldName : stmtProto.fields())
      stmt->getFields().emplace_back(fieldName);
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::kIfStmt: {
    const auto& stmtProto = statementProto.if_stmt();
    auto stmt = std::make_shared<IfStmt>(
        makeData(dataType, stmtProto.data()), makeStmt(stmtProto.cond_part(), dataType, maxID),
        makeStmt(stmtProto.then_part(), dataType, maxID),
        stmtProto.has_else_part() ? makeStmt(stmtProto.else_part(), dataType, maxID) : nullptr,
        makeLocation(stmtProto));
    stmt->setID(stmtProto.id());
    maxID = std::max(std::abs(stmtProto.id()), maxID);
    return stmt;
  }
  case proto::ast::Stmt::STMT_NOT_SET:
  default:
    dawn_unreachable("stmt not set");
  }
  return nullptr;
}

std::shared_ptr<AST> makeAST(const dawn::proto::ast::AST& astProto,
                             ast::StmtData::DataType dataType, int& maxID) {
  auto root = dyn_pointer_cast<BlockStmt>(makeStmt(astProto.root(), dataType, maxID));
  if(!root)
    throw std::runtime_error("root statement of AST is not a 'BlockStmt'");
  auto ast = std::make_shared<AST>(root);
  return ast;
}
