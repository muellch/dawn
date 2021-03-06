#include "gtclang_dsl_defs/gtclang_dsl.hpp"
using namespace gtclang::dsl;

stencil Test {
  storage field_a, field_b, field_c, field_d;

  Do {
    vertical_region(k_start + 1, k_end - 1) { field_b = field_a[i - 1]; }
  }
};