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

#include "dawn/CodeGen/CXXNaive-ico/CXXNaiveCodeGen.h"
#include "dawn/AST/LocationType.h"
#include "dawn/CodeGen/CXXNaive-ico/ASTStencilBody.h"
#include "dawn/CodeGen/CXXNaive-ico/ASTStencilDesc.h"
#include "dawn/CodeGen/CXXUtil.h"
#include "dawn/CodeGen/CodeGenProperties.h"
#include "dawn/CodeGen/IcoChainSizes.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/Assert.h"
#include "dawn/Support/Exception.h"
#include "dawn/Support/Logger.h"
#include "dawn/Support/StringUtil.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace dawn {
namespace codegen {
namespace cxxnaiveico {

// Requirement for a interface:
//
// - Tag: No requirement on the tag. Might be used for ADL.
//
// - The following functions should be declarared:
//
//   template<typename ValueType> <Location>FieldType <location>FieldType(Tag);
//   MeshType meshType(Tag);
//
// - <Location> is one of Cell, Edge, Vertex; <Locations> one of Cells, Edges, Vertices.
//
// - <Location>FieldType should be callable with <Location>Type
//   and return `ValueType&` or `ValueType const&`.
//
// - get<Locations>(Tag, MeshType const&) should return an object that can be used in a range-based
//   for-loop as follows:
//
//     for (auto&& x : get<Locations>(...)) needs to be well-defined such that deref(x) returns
//     an object of <Location>Type
//
// - A function `<Location>Type const& deref(X const& x)` should be defined,
//   where X is decltype(*get<Locations>(...).begin())
//
// - The following functions should be defined, where Weight is an arithmetic type:
//
//   template<typename Init, typename Op>
//   Init reduce(Tag, MeshType, reduceTo, Init,
//   std::vector<dawn::LocationType>, Op)
//
//   template<typename Init, typename Op, typename Weight>
//   Init reduce(Tag, MeshType, reduceTo, Init,
//   std::vector<dawn::LocationType>, Op, std::vector<Weight>)
//
//   where Op must be callable as
//     Op(Init, ValueType);

namespace {
std::string makeLoopImpl(int iExtent, int jExtent, const std::string& dim, const std::string& lower,
                         const std::string& upper, const std::string& comparison,
                         const std::string& increment) {
  return "for(int " + dim + " = " + lower + "+" + std::to_string(iExtent) + "; " + dim + " " +
         comparison + " " + upper + "+" + std::to_string(jExtent) + "; " + increment + dim + ")";
}

std::string makeIntervalBound(iir::Interval const& interval, iir::Interval::Bound bound) {
  return interval.levelIsEnd(bound)
             ? "( m_k_size == 0 ? 0 : (m_k_size - 1)) + " + std::to_string(interval.offset(bound))
             : std::to_string(interval.bound(bound));
}

std::string makeKLoop(bool isBackward, iir::Interval const& interval) {

  const std::string lower = makeIntervalBound(interval, iir::Interval::Bound::lower);
  const std::string upper = makeIntervalBound(interval, iir::Interval::Bound::upper);

  return isBackward ? makeLoopImpl(0, 0, "k", upper, lower, ">=", "--")
                    : makeLoopImpl(0, 0, "k", lower, upper, "<=", "++");
}
} // namespace

std::unique_ptr<TranslationUnit>
run(const std::map<std::string, std::shared_ptr<iir::StencilInstantiation>>&
        stencilInstantiationMap,
    const Options& options) {
  CXXNaiveIcoCodeGen CG(
      stencilInstantiationMap, options.MaxHaloSize,
      Padding{options.paddingCells, options.paddingEdges, options.paddingVertices});
  return CG.generateCode();
} // namespace cxxnaiveico

CXXNaiveIcoCodeGen::CXXNaiveIcoCodeGen(const StencilInstantiationContext& ctx, int maxHaloPoint,
                                       Padding padding)
    : CodeGen(ctx, maxHaloPoint, padding) {}

CXXNaiveIcoCodeGen::~CXXNaiveIcoCodeGen() {}

std::string CXXNaiveIcoCodeGen::generateStencilInstantiation(
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation) {
  using namespace codegen;

  std::stringstream ssSW;

  Namespace dawnNamespace("dawn_generated", ssSW);
  Namespace cxxnaiveNamespace("cxxnaiveico", ssSW);

  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // we might need to think about how to get Mesh and Field for a certain tag
  Class StencilWrapperClass(stencilInstantiation->getName(), ssSW, "typename LibTag");
  StencilWrapperClass.changeAccessibility("private");

  CodeGenProperties codeGenProperties = computeCodeGenProperties(stencilInstantiation.get());

  // generateStencilFunctions(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateStencilClasses(stencilInstantiation, StencilWrapperClass, codeGenProperties);

  generateStencilWrapperMembers(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateStencilWrapperCtr(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  generateGlobalsAPI(StencilWrapperClass, globalsMap, codeGenProperties);

  generateStencilWrapperRun(StencilWrapperClass, stencilInstantiation, codeGenProperties);

  StencilWrapperClass.commit();

  cxxnaiveNamespace.commit();
  dawnNamespace.commit();

  return ssSW.str();
}

void CXXNaiveIcoCodeGen::generateStencilWrapperRun(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {

  // Generate the run method by generate code for the stencil description AST
  MemberFunction RunMethod = stencilWrapperClass.addMemberFunction("void", "run", "");

  RunMethod.finishArgs();
  RunMethod.startBody();

  // generate the control flow code executing each inner stencil
  ASTStencilDesc stencilDescCGVisitor(stencilInstantiation->getMetaData(), codeGenProperties);
  stencilDescCGVisitor.setIndent(RunMethod.getIndent());
  for(const auto& statement :
      stencilInstantiation->getIIR()->getControlFlowDescriptor().getStatements()) {
    statement->accept(stencilDescCGVisitor);
    RunMethod.addStatement(stencilDescCGVisitor.getCodeAndResetStream());
  }

  RunMethod.commit();
}
void CXXNaiveIcoCodeGen::generateStencilWrapperCtr(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {

  const auto& stencils = stencilInstantiation->getStencils();
  const auto& metadata = stencilInstantiation->getMetaData();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // Generate stencil wrapper constructor
  const auto& APIFields = metadata.getAPIFields();
  auto stencilWrapperConstructor = stencilWrapperClass.addConstructor();

  stencilWrapperConstructor.addArg("const ::dawn::mesh_t<LibTag> &mesh");
  stencilWrapperConstructor.addArg("int k_size");

  auto getLocationTypeString = [](ast::LocationType type) {
    switch(type) {
    case ast::LocationType::Cells:
      return "cell_";
    case ast::LocationType::Vertices:
      return "vertex_";
    case ast::LocationType::Edges:
      return "edge_";
    default:
      dawn_unreachable("unexpected type");
      return "";
    }
  };
  for(auto APIfieldID : APIFields) {
    if(metadata.getFieldDimensions(APIfieldID).isVertical()) {
      stencilWrapperConstructor.addArg("::dawn::vertical_field_t<LibTag, ::dawn::float_type>& " +
                                       metadata.getNameFromAccessID(APIfieldID));
      continue;
    }
    if(ast::dimension_cast<const ast::UnstructuredFieldDimension&>(
           metadata.getFieldDimensions(APIfieldID).getHorizontalFieldDimension())
           .isDense()) {
      std::string typeString =
          getLocationTypeString(metadata.getDenseLocationTypeFromAccessID(APIfieldID));
      stencilWrapperConstructor.addArg("::dawn::" + typeString +
                                       "field_t<LibTag, ::dawn::float_type>& " +
                                       metadata.getNameFromAccessID(APIfieldID));
    } else {
      std::string typeString =
          getLocationTypeString(metadata.getDenseLocationTypeFromAccessID(APIfieldID));
      stencilWrapperConstructor.addArg("::dawn::sparse_" + typeString +
                                       "field_t<LibTag, ::dawn::float_type>& " +
                                       metadata.getNameFromAccessID(APIfieldID));
    }
  }

  // add the ctr initialization of each stencil
  for(const auto& stencilPtr : stencils) {
    iir::Stencil& stencil = *stencilPtr;
    if(stencil.isEmpty())
      continue;

    const auto stencilFields = stencil.getOrderedFields();

    const std::string stencilName =
        codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil.getStencilID());

    std::string initCtr = "m_" + stencilName;

    initCtr += "(mesh, k_size";
    for(const auto& fieldInfoPair : stencilFields) {
      const auto& fieldInfo = fieldInfoPair.second;
      initCtr += "," + (metadata.isAccessType(iir::FieldAccessType::InterStencilTemporary,
                                              fieldInfo.field.getAccessID()) ||
                                metadata.isAccessType(iir::FieldAccessType::StencilTemporary,
                                                      fieldInfo.field.getAccessID())
                            ? ("m_" + fieldInfo.Name)
                            : (fieldInfo.Name));
    }
    if(!globalsMap.empty()) {
      initCtr += ",m_globals";
    }
    initCtr += ")";
    stencilWrapperConstructor.addInit(initCtr);

    if(metadata.hasAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                  iir::FieldAccessType::StencilTemporary>()) {
      for(auto accessID : metadata.getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                                     iir::FieldAccessType::StencilTemporary>()) {

        auto field = stencilFields.at(accessID).field;
        std::string allocString;
        if(field.getFieldDimensions().isVertical()) {
          allocString = "allocateField(LibTag{}, k_size)";
        } else {
          auto hdims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
              field.getFieldDimensions().getHorizontalFieldDimension());

          auto getPaddedNumElCall =
              [&](const ast::UnstructuredFieldDimension& hdims) -> std::string {
            switch(hdims.getDenseLocationType()) {
            case ast::LocationType::Cells:
              return "numCells(LibTag{}, mesh) + " +
                     std::to_string(codeGenOptions.UnstrPadding.Cells());
            case ast::LocationType::Edges:
              return "numEdges(LibTag{}, mesh) + " +
                     std::to_string(codeGenOptions.UnstrPadding.Edges());
            case ast::LocationType::Vertices:
              return "numVertices(LibTag{}, mesh) + " +
                     std::to_string(codeGenOptions.UnstrPadding.Vertices());
            default:
              dawn_unreachable("invalid location");
            }
          };

          if(hdims.isDense()) {
            allocString = "allocateField(LibTag{}, " + getPaddedNumElCall(hdims) + ", k_size)";
          } else {
            allocString = "allocateField(LibTag{}, " + getPaddedNumElCall(hdims) + ", k_size, " +
                          std::to_string(ICOChainSize(hdims.getNeighborChain())) +
                          (hdims.getIncludeCenter() ? "+1" : "") + ")";
          }

          stencilWrapperConstructor.addInit("m_" + metadata.getNameFromAccessID(accessID) + "(" +
                                            allocString + ")");
        }
      }
    }
    stencilWrapperConstructor.commit();
  }
}

void CXXNaiveIcoCodeGen::generateStencilWrapperMembers(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    CodeGenProperties& codeGenProperties) const {

  const auto& metadata = stencilInstantiation->getMetaData();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  stencilWrapperClass.addMember("static constexpr const char* s_name =",
                                "\"" + stencilWrapperClass.getName() + "\"");

  if(!globalsMap.empty()) {
    stencilWrapperClass.addMember("globals", "m_globals");
  }

  for(auto stencilPropertiesPair :
      codeGenProperties.stencilProperties(StencilContext::SC_Stencil)) {
    stencilWrapperClass.addMember(stencilPropertiesPair.second->name_,
                                  "m_" + stencilPropertiesPair.second->name_);
  }

  stencilWrapperClass.changeAccessibility("public");
  stencilWrapperClass.addCopyConstructor(Class::ConstructorDefaultKind::Deleted);

  stencilWrapperClass.addComment("Members");
  //
  // Members
  //
  // Define allocated memebers if necessary
  if(metadata.hasAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                iir::FieldAccessType::StencilTemporary>()) {

    for(int AccessID : metadata.getAccessesOfType<iir::FieldAccessType::InterStencilTemporary,
                                                  iir::FieldAccessType::StencilTemporary>()) {
      auto dims = metadata.getFieldDimensions(AccessID);
      std::string fieldType;
      if(dims.isVertical()) {
        fieldType = "vertical_field_t<LibTag, ::dawn::float_type>";
      } else {
        auto hdims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
            dims.getHorizontalFieldDimension());
        fieldType = hdims.isSparse() ? "::dawn::sparse_" : "::dawn::";
        switch(hdims.getDenseLocationType()) {
        case ast::LocationType::Cells:
          fieldType += "cell_field_t<LibTag, ::dawn::float_type>";
          break;
        case ast::LocationType::Edges:
          fieldType += "edge_field_t<LibTag, ::dawn::float_type>";
          break;
        case ast::LocationType::Vertices:
          fieldType += "vertex_field_t<LibTag, ::dawn::float_type>";
          break;
        }
      }
      stencilWrapperClass.addMember(fieldType, "m_" + metadata.getFieldNameFromAccessID(AccessID));
    }
  }

  auto splitterIdxFun = stencilWrapperClass.addMemberFunction("void", "set_splitter_index");
  splitterIdxFun.addArg("::dawn::LocationType loc");
  splitterIdxFun.addArg("::dawn::UnstructuredSubdomain subdomain");
  splitterIdxFun.addArg("int offset");
  splitterIdxFun.addArg("int index");
  for(auto stencilPropertiesPair :
      codeGenProperties.stencilProperties(StencilContext::SC_Stencil)) {
    splitterIdxFun.addStatement(
        "m_" + stencilPropertiesPair.second->name_ +
        ".m_unstructured_domain.set_splitter_index({loc, subdomain, offset}, index)");
  }
  splitterIdxFun.commit();
}

void CXXNaiveIcoCodeGen::generateStencilClasses(
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    Class& stencilWrapperClass, const CodeGenProperties& codeGenProperties) const {

  const auto& stencils = stencilInstantiation->getStencils();
  const auto& globalsMap = stencilInstantiation->getIIR()->getGlobalVariableMap();

  // Stencil members:
  // generate the code for each of the stencils
  for(const auto& stencil : stencils) {

    std::string stencilName =
        codeGenProperties.getStencilName(StencilContext::SC_Stencil, stencil->getStencilID());

    if(stencil->isEmpty())
      continue;

    // fields used in the stencil
    const auto stencilFields = support::orderMap(stencil->getFields());

    auto nonTempFields =
        makeRange(stencilFields, [](std::pair<int, iir::Stencil::FieldInfo> const& p) {
          return !p.second.IsTemporary;
        });
    auto tempFields =
        makeRange(stencilFields, [](std::pair<int, iir::Stencil::FieldInfo> const& p) {
          return p.second.IsTemporary;
        });

    Structure stencilClass = stencilWrapperClass.addStruct(stencilName);

    ASTStencilBody stencilBodyCXXVisitor(stencilInstantiation->getMetaData(),
                                         StencilContext::SC_Stencil);

    auto fieldInfoToDeclString = [](iir::Stencil::FieldInfo info) {
      if(info.field.getFieldDimensions().isVertical()) {
        return std::string("::dawn::vertical_field_t<LibTag, ::dawn::float_type>");
      }

      const auto& unstructuredDims = ast::dimension_cast<ast::UnstructuredFieldDimension const&>(
          info.field.getFieldDimensions().getHorizontalFieldDimension());
      if(unstructuredDims.isDense()) {
        switch(unstructuredDims.getDenseLocationType()) {
        case ast::LocationType::Cells:
          return std::string("::dawn::cell_field_t<LibTag, ::dawn::float_type>");
        case ast::LocationType::Vertices:
          return std::string("::dawn::vertex_field_t<LibTag, ::dawn::float_type>");
        case ast::LocationType::Edges:
          return std::string("::dawn::edge_field_t<LibTag, ::dawn::float_type>");
        default:
          dawn_unreachable("invalid location");
          return std::string("");
        }
      } else {
        switch(unstructuredDims.getDenseLocationType()) {
        case ast::LocationType::Cells:
          return std::string("::dawn::sparse_cell_field_t<LibTag, ::dawn::float_type>");
        case ast::LocationType::Vertices:
          return std::string("::dawn::sparse_vertex_field_t<LibTag, ::dawn::float_type>");
        case ast::LocationType::Edges:
          return std::string("::dawn::sparse_edge_field_t<LibTag, ::dawn::float_type>");
        default:
          dawn_unreachable("invalid location");
          return std::string("");
        }
      }
    };

    stencilClass.addMember("::dawn::mesh_t<LibTag> const&", "m_mesh");
    stencilClass.addMember("int", "m_k_size");
    for(auto fieldIt : stencilFields) {
      stencilClass.addMember(fieldInfoToDeclString(fieldIt.second) + "&",
                             "m_" + fieldIt.second.Name);
    }
    stencilClass.addMember("::dawn::unstructured_domain ", " m_unstructured_domain ");
    if(!globalsMap.empty()) {
      stencilClass.addMember("const globals &", " m_globals");
    }

    // addTmpStorageDeclaration(StencilClass, tempFields);

    stencilClass.changeAccessibility("public");

    auto stencilClassCtr = stencilClass.addConstructor();

    stencilClassCtr.addArg("::dawn::mesh_t<LibTag> const &mesh");
    stencilClassCtr.addArg("int k_size");
    for(auto fieldIt : stencilFields) {
      stencilClassCtr.addArg(fieldInfoToDeclString(fieldIt.second) + "&" + fieldIt.second.Name);
    }

    // stencilClassCtr.addInit("m_dom(dom_)");
    if(!globalsMap.empty()) {
      stencilClassCtr.addArg("const globals &globals_");
    }

    stencilClassCtr.addInit("m_mesh(mesh)");
    stencilClassCtr.addInit("m_k_size(k_size)");
    for(auto fieldIt : stencilFields) {
      stencilClassCtr.addInit("m_" + fieldIt.second.Name + "(" + fieldIt.second.Name + ")");
    }

    if(!globalsMap.empty()) {
      stencilClassCtr.addInit("m_globals(globals_)");
    }

    // addTmpStorageInit(stencilClassCtr, *stencil, tempFields);
    stencilClassCtr.commit();

    // virtual dtor
    MemberFunction stencilClassDtr = stencilClass.addDestructor(false);
    stencilClassDtr.startBody();
    stencilClassDtr.commit();

    // synchronize storages method
    MemberFunction syncStoragesMethod = stencilClass.addMemberFunction("void", "sync_storages", "");
    syncStoragesMethod.startBody();

    // for(auto fieldIt : nonTempFields) {
    //   syncStoragesMethod.addStatement("m_" + fieldIt.second.Name + ".sync()");
    // }

    syncStoragesMethod.commit();

    // accumulated extents of API fields
    generateFieldExtentsInfo(stencilClass, nonTempFields, ast::GridType::Unstructured);

    //
    // Run-Method
    //
    MemberFunction StencilRunMethod = stencilClass.addMemberFunction("void", "run", "");
    StencilRunMethod.startBody();

    // TODO the generic deref should be moved to a different namespace
    StencilRunMethod.addStatement("using ::dawn::deref");

    // StencilRunMethod.addStatement("sync_storages()");
    for(const auto& multiStagePtr : stencil->getChildren()) {
      StencilRunMethod.ss() << "{\n";

      const iir::MultiStage& multiStage = *multiStagePtr;

      // create all the data views
      const auto& usedFields = multiStage.getFields();
      for(const auto& usedField : usedFields) {
        auto field = stencilFields.at(usedField.first);
        // auto storageName = field.IsTemporary ? "tmp_storage_t" :
        // StencilTemplates[usedField.first]; StencilRunMethod.addStatement(c_gt +
        // "data_view<" + storageName + "> " + field.Name +
        //                               "= " + c_gt + "make_host_view(m_" + field.Name +
        //                               ")");
        // StencilRunMethod.addStatement("std::array<int,3> " + field.Name +
        // "_offsets{0,0,0}");
      }
      auto intervals_set = multiStage.getIntervals();
      std::vector<iir::Interval> intervals_v;
      std::copy(intervals_set.begin(), intervals_set.end(), std::back_inserter(intervals_v));

      // compute the partition of the intervals
      auto partitionIntervals = iir::Interval::computePartition(intervals_v);
      if((multiStage.getLoopOrder() == iir::LoopOrderKind::Backward))
        std::reverse(partitionIntervals.begin(), partitionIntervals.end());

      auto spaceMagicNumToEnum = [](int magicNum) {
        switch(magicNum) {
        case 0:
          return "::dawn::UnstructuredSubdomain::LateralBoundary";
        case 1:
          return "::dawn::UnstructuredSubdomain::Nudging";
        case 2:
          return "::dawn::UnstructuredSubdomain::Interior";
        case 3:
          return "::dawn::UnstructuredSubdomain::Halo";
        case 4:
          return "::dawn::UnstructuredSubdomain::End";
        default:
          throw std::runtime_error("Invalid magic number");
        }
      };

      auto getLoop = [&](ast::LocationType type,
                         std::optional<iir::Interval> iterSpace) -> std::string {
        switch(type) {
        case ast::LocationType::Cells:
          return (iterSpace.has_value()
                      ? std::string("for(auto const& loc : getCells(LibTag{}, m_mesh, ") +
                            "m_unstructured_domain({::dawn::LocationType::Cells," +
                            spaceMagicNumToEnum(iterSpace->lowerBound()) + "," +
                            std::to_string(iterSpace->lowerOffset()) + "})," +
                            "m_unstructured_domain({::dawn::LocationType::Cells," +
                            spaceMagicNumToEnum(iterSpace->upperBound()) + "," +
                            std::to_string(iterSpace->upperOffset()) + "})))"
                      : "for(auto const& loc : getCells(LibTag{}, m_mesh))");
        case ast::LocationType::Vertices:
          return (iterSpace.has_value())
                     ? std::string("for(auto const& loc : getVertices(LibTag{}, m_mesh, ") +
                           "m_unstructured_domain({::dawn::LocationType::Vertices," +
                           spaceMagicNumToEnum(iterSpace->lowerBound()) + "," +
                           std::to_string(iterSpace->lowerOffset()) + "})," +
                           "m_unstructured_domain({::dawn::LocationType::Vertices," +
                           spaceMagicNumToEnum(iterSpace->upperBound()) + "," +
                           std::to_string(iterSpace->upperOffset()) + "})))"
                     : "for(auto const& loc : getVertices(LibTag{}, m_mesh))";
        case ast::LocationType::Edges:
          return (iterSpace.has_value())
                     ? std::string("for(auto const& loc : getEdges(LibTag{}, m_mesh, ") +
                           "m_unstructured_domain({::dawn::LocationType::Edges," +
                           spaceMagicNumToEnum(iterSpace->lowerBound()) + "," +
                           std::to_string(iterSpace->lowerOffset()) + "})," +
                           "m_unstructured_domain({::dawn::LocationType::Edges," +
                           spaceMagicNumToEnum(iterSpace->upperBound()) + "," +
                           std::to_string(iterSpace->upperOffset()) + "})))"
                     : "for(auto const& loc : getEdges(LibTag{}, m_mesh))";
        default:
          dawn_unreachable("invalid type");
          return "";
        }
      };

      for(auto interval : partitionIntervals) {
        StencilRunMethod.addBlockStatement(
            makeKLoop((multiStage.getLoopOrder() == iir::LoopOrderKind::Backward), interval), [&] {
              // for each interval, we generate naive nested loops
              for(const auto& stagePtr : multiStage.getChildren()) {
                const iir::Stage& stage = *stagePtr;

                DAWN_ASSERT_MSG(stage.getLocationType().has_value(),
                                "Stage must have a location type");
                std::string loopCode =
                    getLoop(*stage.getLocationType(), stage.getUnstructuredIterationSpace());
                StencilRunMethod.addBlockStatement(loopCode, [&] {
                  // Generate Do-Method
                  for(const auto& doMethodPtr : stage.getChildren()) {
                    const iir::DoMethod& doMethod = *doMethodPtr;
                    if(!doMethod.getInterval().overlaps(interval))
                      continue;

                    for(const auto& stmt : doMethod.getAST().getStatements()) {
                      stmt->accept(stencilBodyCXXVisitor);
                      StencilRunMethod << stencilBodyCXXVisitor.getCodeAndResetStream();
                    }
                  }
                });
              }
            });
      }
      StencilRunMethod.ss() << "}";
    }
    StencilRunMethod.addStatement("sync_storages()");
    StencilRunMethod.commit();
  }
}

void CXXNaiveIcoCodeGen::generateStencilFunctions(
    Class& stencilWrapperClass,
    const std::shared_ptr<iir::StencilInstantiation> stencilInstantiation,
    const CodeGenProperties& codeGenProperties) const {
  // TODO: this method is broken, it's on cartesian

  const auto& metadata = stencilInstantiation->getMetaData();
  // stencil functions
  //
  // Generate stencil functions code for stencils instantiated by this stencil
  //
  std::unordered_set<std::string> generatedStencilFun;
  size_t idx = 0;
  for(const auto& stencilFun : metadata.getStencilFunctionInstantiations()) {
    std::string stencilFunName = iir::StencilFunctionInstantiation::makeCodeGenName(*stencilFun);
    if(generatedStencilFun.emplace(stencilFunName).second) {

      // Field declaration
      const auto& fields = stencilFun->getCalleeFields();

      if(fields.empty()) {
        throw SemanticError(std::string("No storages referenced in stencil '") +
                                stencilInstantiation->getName() +
                                "', this would result in invalid gridtools code",
                            stencilInstantiation->getMetaData().getFileName(),
                            stencilInstantiation->getMetaData().getStencilLocation());
      }

      MemberFunction stencilFunMethod = stencilWrapperClass.addMemberFunction(
          std::string("static ") + (stencilFun->hasReturn() ? "double" : "void"), stencilFunName);

      if(fields.empty() && !stencilFun->hasReturn()) {
        throw SemanticError(std::string("No storages referenced in stencil function '") +
                                stencilInstantiation->getName() +
                                "', this would result in invalid gridtools code",
                            stencilInstantiation->getMetaData().getFileName(),
                            stencilFun->getStencilFunction()->Loc);
      }

      // Each stencil function call will pass the (i,j,k) position
      stencilFunMethod.addArg("const int i");
      stencilFunMethod.addArg("const int j");
      stencilFunMethod.addArg("const int k");

      // const auto& stencilProp = codeGenProperties.getStencilProperties(
      //    StencilContext::SC_StencilFunction, stencilFunName);

      // We need to generate the arguments in order (of the fn call expr)
      for(const auto& exprArg : stencilFun->getArguments()) {
        if(exprArg->Kind != sir::StencilFunctionArg::ArgumentKind::Field)
          continue;
        const std::string argName = exprArg->Name;

        // TODO: the following commented lines are broken (using gridtools::data_view)
        //        DAWN_ASSERT(stencilProp->paramNameToType_.count(argName));
        //        const std::string argType = stencilProp->paramNameToType_[argName];
        // each parameter being passed to a stencil function, is wrapped around the
        // param_wrapper that contains the storage and the offset, in order to resolve
        // offset passed to the storage during the function call. For example:
        // fn_call(v(i+1), v(j-1))
        //        stencilFunMethod.addArg("param_wrapper<" + c_gt + "data_view<" + argType
        //        + ">> pw_" + argName);
      }

      // add global parameter
      if(stencilFun->hasGlobalVariables()) {
        stencilFunMethod.addArg("globals m_globals");
      }
      ASTStencilBody stencilBodyCXXVisitor(stencilInstantiation->getMetaData(),
                                           StencilContext::SC_StencilFunction);

      stencilFunMethod.startBody();

      for(std::size_t m = 0; m < fields.size(); ++m) {

        std::string paramName =
            stencilFun->getOriginalNameFromCallerAccessID(fields[m].getAccessID());
        // TODO: the following commented lines are broken (using gridtools::data_view)
        //        stencilFunMethod << c_gt << "data_view<StorageType" + std::to_string(m)
        //        + "> "
        //                         << paramName << " = pw_" << paramName << ".dview_;";
        //        stencilFunMethod << "auto " << paramName << "_offsets = pw_" << paramName
        //        <<
        //        ".offsets_;";
      }
      stencilBodyCXXVisitor.setCurrentStencilFunction(stencilFun);
      stencilBodyCXXVisitor.setIndent(stencilFunMethod.getIndent());
      for(const auto& stmt : stencilFun->getStatements()) {
        stmt->accept(stencilBodyCXXVisitor);
        stencilFunMethod.indentStatment();
        stencilFunMethod << stencilBodyCXXVisitor.getCodeAndResetStream();
      }

      stencilFunMethod.commit();
    }
    idx++;
  }
}

std::unique_ptr<TranslationUnit> CXXNaiveIcoCodeGen::generateCode() {
  DAWN_LOG(INFO) << "Starting code generation for GTClang ...";

  // Generate code for StencilInstantiations
  std::map<std::string, std::string> stencils;
  for(const auto& nameStencilCtxPair : context_) {
    std::string code = generateStencilInstantiation(nameStencilCtxPair.second);
    if(code.empty())
      return nullptr;
    stencils.emplace(nameStencilCtxPair.first, std::move(code));
  }

  std::string globals = generateGlobals(context_, "dawn_generated", "cxxnaiveico");

  std::vector<std::string> ppDefines;
  ppDefines.push_back("#define DAWN_GENERATED 1");
  ppDefines.push_back("#undef DAWN_BACKEND_T");
  ppDefines.push_back("#define DAWN_BACKEND_T CXXNAIVEICO");
  ppDefines.push_back(
      "#define GRIDTOOLS_DAWN_NO_INCLUDE"); // Required to not include gridtools from math.hpp
  ppDefines.push_back("#include <driver-includes/unstructured_interface.hpp>");
  ppDefines.push_back("#include <driver-includes/unstructured_domain.hpp>");
  ppDefines.push_back("#include <driver-includes/math.hpp>");
  DAWN_LOG(INFO) << "Done generating code";

  std::string filename = generateFileName(context_);
  return std::make_unique<TranslationUnit>(filename, std::move(ppDefines), std::move(stencils),
                                           std::move(globals));
}

} // namespace cxxnaiveico
} // namespace codegen
} // namespace dawn
