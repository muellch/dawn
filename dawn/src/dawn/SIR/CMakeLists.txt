##===------------------------------------------------------------------------------*- CMake -*-===##
##                          _
##                         | |
##                       __| | __ ___      ___ ___
##                      / _` |/ _` \ \ /\ / / '_  |
##                     | (_| | (_| |\ V  V /| | | |
##                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
##
##
##  This file is distributed under the MIT License (MIT).
##  See LICENSE.txt for details.
##
##===------------------------------------------------------------------------------------------===##

# Defines dawn_sir_proto_files
include(proto/DawnSIR.cmake)

include(DawnProtobufGenerate)
dawn_protobuf_generate(
  OUT_FILES sir_proto_cpp_files
  INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../AST/proto/
  WDIR ${CMAKE_CURRENT_SOURCE_DIR}/proto
  PROTOS ${dawn_sir_proto_files}
  PACKG SIR
  LANGUAGE cpp
)
set(sir_proto_header_files ${sir_proto_cpp_files})
list(FILTER sir_proto_header_files INCLUDE REGEX ".+\\.h?h$")
install(FILES ${sir_proto_header_files} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dawn/SIR/SIR)

add_library(DawnSIRProto OBJECT ${sir_proto_cpp_files})
target_compile_features(DawnSIRProto PUBLIC cxx_std_11)
set_target_properties(DawnSIRProto PROPERTIES ${DAWN_TARGET_PROPERTIES})

# Generated files need to be compiled with the protobuf headers
target_include_directories(DawnSIRProto
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# ... and linked to protobuf
target_link_libraries(DawnSIRProto PUBLIC DawnASTProto protobuf::libprotobuf)

# Builds one library for both SIR and AST
add_library(DawnSIR
  AST.h
  ASTStmt.h
  ASTStmt.cpp
  SIR.cpp
  SIR.h
  VerticalRegion.h
  VerticalRegion.cpp
  $<TARGET_OBJECTS:DawnSIRProto>
)

target_add_dawn_standard_props(DawnSIR)
target_link_libraries(DawnSIR PUBLIC DawnSupport DawnAST DawnSIRProto)
# The include path below is necessary for the C++ proto headers
target_include_directories(DawnSIR
  PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dawn/SIR>
)
