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

#include "gtclang_dsl_defs/gtclang_dsl.hpp"

using namespace gtclang::dsl;

#ifndef DAWN_GENERATED
interval k_flat = k_start + 4;
#endif

stencil intervals03 {
  storage in, out;

  Do {
    vertical_region(k_end - 1, k_end - 1)
        out = 0;
    vertical_region(k_end - 2, k_flat)
        out = out[k + 1] + in + 3;

    vertical_region(k_flat - 2, k_flat - 2)
        out = 1;

    vertical_region(k_flat - 3, k_start)
        out = out[k + 1] + in + 3;
  }
};
