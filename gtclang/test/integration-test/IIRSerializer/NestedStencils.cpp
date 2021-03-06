//===--------------------------------------------------------------------------------*- C++ -*-===//
//                         _       _
//                        | |     | |
//                    __ _| |_ ___| | __ _ _ __   __ _
//                   / _` | __/ __| |/ _` | '_ \ / _` |
//                  | (_| | || (__| | (_| | | | | (_| |
//                   \__, |\__\___|_|\__,_|_| |_|\__, | - GridTools Clang DSL
//                    __/ |                       __/ |
//                   |___/                       |___/
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

// RUN: %gtclang% %file% -fwrite-iir -fno-codegen
// EXPECTED_FILE: OUTPUT:NestedStencils.iir REFERENCE:%filename%_ref.iir IGNORE:filename

#include "gtclang_dsl_defs/gtclang_dsl.hpp"

using namespace gtclang::dsl;

stencil NestedStencils {
  storage field_a, field_b;

  Do {
    vertical_region(k_start, k_end) { field_a = field_b; }
  }
};

stencil Nesting1 {
  storage filed_c, field_d;

  Do { NestedStencils(filed_c, field_d); }
};

stencil Nesting2 {
  storage field_e, field_f;

  Do {
    Nesting1(field_e, field_f);
    NestedStencils(field_f, field_e);
  }
};
