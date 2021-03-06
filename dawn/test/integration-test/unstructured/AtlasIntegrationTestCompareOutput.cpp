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

#include "dawn/Support/Assert.h"
#include "driver-includes/unstructured_domain.hpp"
#include "driver-includes/unstructured_interface.hpp"

#include "AtlasCartesianWrapper.h"
#include "UnstructuredVerifier.h"

#include "atlas/grid.h"
#include "atlas/mesh/actions/BuildCellCentres.h"
#include "atlas/mesh/actions/BuildEdges.h"
#include "atlas/mesh/actions/BuildPeriodicBoundaries.h"
#include "atlas/meshgenerator.h"
#include "atlas/option/Options.h"
#include "atlas/output/Gmsh.h"
#include "interface/atlas_interface.hpp"

#include <atlas/util/CoordinateEnums.h>

#include <gtest/gtest.h>

#include <sstream>
#include <tuple>

namespace {
atlas::Mesh generateQuadMesh(size_t nx, size_t ny) {
  std::stringstream configStr;
  configStr << "L" << nx << "x" << ny;
  atlas::StructuredGrid structuredGrid = atlas::Grid(configStr.str());
  atlas::StructuredMeshGenerator generator;
  auto mesh = generator.generate(structuredGrid);
  atlas::mesh::actions::build_edges(
      mesh, atlas::util::Config("pole_edges", false)); // work around to eliminate pole edges
  atlas::mesh::actions::build_node_to_edge_connectivity(mesh);
  atlas::mesh::actions::build_element_to_edge_connectivity(mesh);
  return mesh;
}

atlas::Mesh generateEquilatMesh(size_t nx, size_t ny) {

  // right handed triangle mesh
  auto x = atlas::grid::LinearSpacing(0, nx, nx, false);
  auto y = atlas::grid::LinearSpacing(0, ny, ny, false);
  atlas::Grid grid = atlas::StructuredGrid{x, y};

  auto meshgen = atlas::StructuredMeshGenerator{atlas::util::Config("angle", -1.)};
  atlas::Mesh mesh = meshgen.generate(grid);

  // coordinate trafo to mold this into an equilat mesh
  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  for(int nodeIdx = 0; nodeIdx < mesh.nodes().size(); nodeIdx++) {
    double x = xy(nodeIdx, atlas::LON);
    double y = xy(nodeIdx, atlas::LAT);
    x = x - 0.5 * y;
    y = y * sqrt(3) / 2.;
    xy(nodeIdx, atlas::LON) = x;
    xy(nodeIdx, atlas::LAT) = y;
  }

  // build up nbh tables
  atlas::mesh::actions::build_edges(mesh, atlas::util::Config("pole_edges", false));
  atlas::mesh::actions::build_node_to_edge_connectivity(mesh);
  atlas::mesh::actions::build_element_to_edge_connectivity(mesh);

  // mesh constructed this way is missing node to cell connectivity, built it as well
  for(int nodeIdx = 0; nodeIdx < mesh.nodes().size(); nodeIdx++) {
    const auto& nodeToEdge = mesh.nodes().edge_connectivity();
    const auto& edgeToCell = mesh.edges().cell_connectivity();
    auto& nodeToCell = mesh.nodes().cell_connectivity();

    std::set<int> nbh;
    for(int nbhEdgeIdx = 0; nbhEdgeIdx < nodeToEdge.cols(nodeIdx); nbhEdgeIdx++) {
      int edgeIdx = nodeToEdge(nodeIdx, nbhEdgeIdx);
      if(edgeIdx == nodeToEdge.missing_value()) {
        continue;
      }
      for(int nbhCellIdx = 0; nbhCellIdx < edgeToCell.cols(edgeIdx); nbhCellIdx++) {
        int cellIdx = edgeToCell(edgeIdx, nbhCellIdx);
        if(cellIdx == edgeToCell.missing_value()) {
          continue;
        }
        nbh.insert(cellIdx);
      }
    }

    assert(nbh.size() <= 6);
    std::vector<int> initData(nbh.size(), nodeToCell.missing_value());
    nodeToCell.add(1, nbh.size(), initData.data());
    int copyIter = 0;
    for(const int n : nbh) {
      nodeToCell.set(nodeIdx, copyIter++, n);
    }
  }

  return mesh;
}

std::tuple<atlas::Field, atlasInterface::Field<double>> makeAtlasField(const std::string& name,
                                                                       size_t size, size_t kSize) {
  atlas::Field field_F{name, atlas::array::DataType::real64(),
                       atlas::array::make_shape(size, kSize)};
  return {field_F, atlas::array::make_view<double, 2>(field_F)};
}

std::tuple<atlas::Field, atlasInterface::VerticalField<double>>
makeAtlasVerticalField(const std::string& name, size_t kSize) {
  atlas::Field field_F{name, atlas::array::DataType::real64(), atlas::array::make_shape(kSize)};
  return {field_F, atlas::array::make_view<double, 1>(field_F)};
}

std::tuple<atlas::Field, atlasInterface::SparseDimension<double>>
makeAtlasSparseField(const std::string& name, size_t size, size_t sparseSize, int kSize) {
  atlas::Field field_F{name, atlas::array::DataType::real64(),
                       atlas::array::make_shape(size, kSize, sparseSize)};
  return {field_F, atlas::array::make_view<double, 3>(field_F)};
}

template <typename T>
void initField(atlasInterface::Field<T>& field, size_t numEl, size_t kSize, T val) {
  for(int level = 0; level < kSize; ++level) {
    for(int elIdx = 0; elIdx < numEl; ++elIdx) {
      field(elIdx, level) = val;
    }
  }
}

template <typename T>
void initSparseField(atlasInterface::SparseDimension<T>& sparseField, size_t numEl, size_t kSize,
                     size_t sparseSize, T val) {
  for(int level = 0; level < kSize; ++level) {
    for(int elIdx = 0; elIdx < numEl; ++elIdx) {
      for(int nbhIdx = 0; nbhIdx < sparseSize; nbhIdx++) {
        sparseField(elIdx, nbhIdx, level) = val;
      }
    }
  }
}

namespace {
#include <generated_copyCell.hpp>
TEST(AtlasIntegrationTestCompareOutput, CopyCell) {
  // Setup a 32 by 32 grid of quads and generate a mesh out of it
  auto mesh = generateQuadMesh(32, 32);
  // We only need one vertical level
  size_t nb_levels = 1;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(in_v, mesh.cells().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  // Run the stencil
  dawn_generated::cxxnaiveico::copyCell<atlasInterface::atlasTag>(mesh, static_cast<int>(nb_levels),
                                                                  in_v, out_v)
      .run();

  // Check correctness of the output
  for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx)
    ASSERT_EQ(out_v(cell_idx, 0), 1.0);
}
} // namespace

namespace {
#include <generated_copyEdge.hpp>
TEST(AtlasIntegrationTestCompareOutput, CopyEdge) {
  auto mesh = generateQuadMesh(32, 32);
  size_t nb_levels = 1;

  auto [in_F, in_v] = makeAtlasField("in", mesh.edges().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.edges().size(), nb_levels);

  initField(in_v, mesh.edges().size(), nb_levels, 1.0);
  initField(out_v, mesh.edges().size(), nb_levels, -1.0);

  dawn_generated::cxxnaiveico::copyEdge<atlasInterface::atlasTag>(mesh, static_cast<int>(nb_levels),
                                                                  in_v, out_v)
      .run();

  for(int edge_idx = 0; edge_idx < mesh.edges().size(); ++edge_idx)
    ASSERT_EQ(out_v(edge_idx, 0), 1.0);
}
} // namespace

namespace {
#include <generated_verticalSum.hpp>
TEST(AtlasIntegrationTestCompareOutput, verticalCopy) {
  auto mesh = generateQuadMesh(32, 32);
  size_t nb_levels = 5; // must be >= 3

  auto [in_F, in_v] = makeAtlasField("in", mesh.edges().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.edges().size(), nb_levels);

  double initValue = 10.;
  initField(in_v, mesh.cells().size(), nb_levels, initValue);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  // Run verticalSum, which just copies the values in the cells above and below into the current
  // level and adds them up
  dawn_generated::cxxnaiveico::verticalSum<atlasInterface::atlasTag>(mesh, nb_levels, in_v, out_v)
      .run();

  // Thats why we expct all the levels except the top and bottom one to hold twice the initial value
  for(int level = 1; level < nb_levels - 1; ++level) {
    for(int cell = 0; cell < mesh.cells().size(); ++cell) {
      ASSERT_EQ(out_v(cell, level), 2 * initValue);
    }
  }
}
} // namespace

namespace {
#include <generated_accumulateEdgeToCell.hpp>
TEST(AtlasIntegrationTestCompareOutput, Accumulate) {
  auto mesh = generateQuadMesh(32, 32);
  size_t nb_levels = 1;

  auto [in_F, in_v] = makeAtlasField("in", mesh.edges().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  initField(in_v, mesh.edges().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  dawn_generated::cxxnaiveico::accumulateEdgeToCell<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v, out_v)
      .run();

  // Check correctness of the output
  for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
    ASSERT_EQ(out_v(cell_idx, 0), 4.0);
  }
}
} // namespace

namespace {
#include <generated_diffusion.hpp>
#include <reference_diffusion.hpp>
TEST(AtlasIntegrationTestCompareOutput, Diffusion) {
  auto mesh = generateQuadMesh(32, 32);
  size_t nb_levels = 1;

  // Create input (on cells) and output (on cells) fields for generated and reference stencils
  auto [in_ref, in_v_ref] = makeAtlasField("in_v_ref", mesh.cells().size(), nb_levels);
  auto [in_gen, in_v_gen] = makeAtlasField("in_v_gen", mesh.cells().size(), nb_levels);
  auto [out_ref, out_v_ref] = makeAtlasField("out_v_ref", mesh.cells().size(), nb_levels);
  auto [out_gen, out_v_gen] = makeAtlasField("out_v_gen", mesh.cells().size(), nb_levels);

  AtlasToCartesian atlasToCartesianMapper(mesh);

  for(int cellIdx = 0, size = mesh.cells().size(); cellIdx < size; ++cellIdx) {
    auto [cartX, cartY] = atlasToCartesianMapper.cellMidpoint(mesh, cellIdx);
    bool inX = cartX > 0.375 && cartX < 0.625;
    bool inY = cartY > 0.375 && cartY < 0.625;
    in_v_ref(cellIdx, 0) = (inX && inY) ? 1 : 0;
    in_v_gen(cellIdx, 0) = (inX && inY) ? 1 : 0;
  }

  for(int i = 0; i < 5; ++i) {
    // Run the stencils
    dawn_generated::cxxnaiveico::reference_diffusion<atlasInterface::atlasTag>(
        mesh, static_cast<int>(nb_levels), in_v_ref, out_v_ref)
        .run();
    dawn_generated::cxxnaiveico::diffusion<atlasInterface::atlasTag>(
        mesh, static_cast<int>(nb_levels), in_v_gen, out_v_gen)
        .run();

    // Swap in and out
    using std::swap;
    swap(in_ref, out_ref);
    swap(in_gen, out_gen);
  }

  // Check correctness of the output
  {
    auto out_v_ref = atlas::array::make_view<double, 2>(out_ref);
    auto out_v_gen = atlas::array::make_view<double, 2>(out_gen);
    UnstructuredVerifier v;
    EXPECT_TRUE(v.compareArrayView(out_v_gen, out_v_ref)) << "while comparing output (on cells)";
  }
}
} // namespace

namespace {
#include <generated_diamond.hpp>
#include <reference_diamond.hpp>
TEST(AtlasIntegrationTestCompareOutput, Diamond) {
  auto mesh = generateEquilatMesh(32, 32);
  const size_t nb_levels = 1;
  const size_t level = 0;

  // Create input (on cells) and output (on cells) fields for generated and reference stencils
  auto [in, in_v] = makeAtlasField("in_v", mesh.nodes().size(), nb_levels);
  auto [out_ref, out_v_ref] = makeAtlasField("out_v_ref", mesh.edges().size(), nb_levels);
  auto [out_gen, out_v_gen] = makeAtlasField("out_v_gen", mesh.edges().size(), nb_levels);

  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  for(int nodeIdx = 0, size = mesh.nodes().size(); nodeIdx < size; ++nodeIdx) {
    double x = xy(nodeIdx, atlas::LON);
    double y = xy(nodeIdx, atlas::LAT);
    in_v(nodeIdx, level) = sin(x) * sin(y);
  }

  dawn_generated::cxxnaiveico::reference_diamond<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), out_v_ref, in_v)
      .run();
  dawn_generated::cxxnaiveico::diamond<atlasInterface::atlasTag>(mesh, static_cast<int>(nb_levels),
                                                                 out_v_gen, in_v)
      .run();

  // Check correctness of the output
  {
    auto out_v_ref = atlas::array::make_view<double, 2>(out_ref);
    auto out_v_gen = atlas::array::make_view<double, 2>(out_gen);
    UnstructuredVerifier v;
    EXPECT_TRUE(v.compareArrayView(out_v_gen, out_v_ref)) << "while comparing output (on cells)";
  }
}
} // namespace

namespace {
#include <generated_diamondWeights.hpp>
#include <reference_diamondWeights.hpp>
TEST(AtlasIntegrationTestCompareOutput, DiamondWeight) {
  auto mesh = generateEquilatMesh(32, 32);
  const size_t nb_levels = 1;
  const size_t level = 0;

  // Create input (on cells) and output (on cells) fields for generated and reference stencils
  auto [in, in_v] = makeAtlasField("in_v", mesh.nodes().size(), nb_levels);
  auto [out_ref, out_v_ref] = makeAtlasField("out_v_ref", mesh.edges().size(), nb_levels);
  auto [out_gen, out_v_gen] = makeAtlasField("out_v_gen", mesh.edges().size(), nb_levels);

  auto [inv_edge_length, inv_edge_length_v] =
      makeAtlasField("inv_edge_length", mesh.edges().size(), nb_levels);
  auto [inv_vert_length, inv_vert_length_v] =
      makeAtlasField("inv_vert_length", mesh.edges().size(), nb_levels);

  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  for(int nodeIdx = 0, size = mesh.nodes().size(); nodeIdx < size; ++nodeIdx) {
    double x = xy(nodeIdx, atlas::LON);
    double y = xy(nodeIdx, atlas::LAT);
    in_v(nodeIdx, level) = sin(x) * sin(y);
  }

  for(int edgeIdx = 0, size = mesh.edges().size(); edgeIdx < size; ++edgeIdx) {
    int nodeIdx0 = mesh.edges().node_connectivity()(edgeIdx, 0);
    int nodeIdx1 = mesh.edges().node_connectivity()(edgeIdx, 1);
    double dx = xy(nodeIdx0, atlas::LON) - xy(nodeIdx1, atlas::LON);
    double dy = xy(nodeIdx0, atlas::LAT) - xy(nodeIdx1, atlas::LAT);
    double length = sqrt(dx * dx + dy * dy);
    inv_edge_length_v(edgeIdx, level) = 1. / length;
    inv_vert_length_v(edgeIdx, level) =
        1. / (0.5 * sqrt(3) * length * 2); // twice the height of equilat triangle
  }

  dawn_generated::cxxnaiveico::reference_diamondWeights<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), out_v_ref, inv_edge_length_v, inv_vert_length_v, in_v)
      .run();
  dawn_generated::cxxnaiveico::diamondWeights<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), out_v_gen, inv_edge_length_v, inv_vert_length_v, in_v)
      .run();

  // Check correctness of the output
  {
    auto out_v_ref = atlas::array::make_view<double, 2>(out_ref);
    auto out_v_gen = atlas::array::make_view<double, 2>(out_gen);
    UnstructuredVerifier v;
    EXPECT_TRUE(v.compareArrayView(out_v_gen, out_v_ref)) << "while comparing output (on cells)";
  }
}
} // namespace

namespace {
#include <generated_intp.hpp>
#include <reference_intp.hpp>
TEST(AtlasIntegrationTestCompareOutput, Intp) {
  auto mesh = generateEquilatMesh(32, 32);
  const size_t nb_levels = 1;
  const size_t level = 0;

  // Create input (on cells) and output (on cells) fields for generated and reference stencils
  auto [in_ref, in_v_ref] = makeAtlasField("in_v_ref", mesh.cells().size(), nb_levels);
  auto [in_gen, in_v_gen] = makeAtlasField("in_v_gen", mesh.cells().size(), nb_levels);
  auto [out_ref, out_v_ref] = makeAtlasField("out_v_ref", mesh.cells().size(), nb_levels);
  auto [out_gen, out_v_gen] = makeAtlasField("out_v_gen", mesh.cells().size(), nb_levels);

  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  for(int cellIdx = 0, size = mesh.cells().size(); cellIdx < size; ++cellIdx) {
    int v0 = mesh.cells().node_connectivity()(cellIdx, 0);
    int v1 = mesh.cells().node_connectivity()(cellIdx, 1);
    int v2 = mesh.cells().node_connectivity()(cellIdx, 2);
    double x = 1. / 3. * (xy(v0, atlas::LON) + xy(v1, atlas::LON) + xy(v2, atlas::LON));
    double y = 1. / 3. * (xy(v0, atlas::LAT) + xy(v1, atlas::LAT) + xy(v2, atlas::LON));
    in_v_ref(cellIdx, level) = sin(x) * sin(y);
    in_v_gen(cellIdx, level) = sin(x) * sin(y);
  }

  dawn_generated::cxxnaiveico::reference_intp<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v_ref, out_v_ref)
      .run();
  dawn_generated::cxxnaiveico::intp<atlasInterface::atlasTag>(mesh, static_cast<int>(nb_levels),
                                                              in_v_gen, out_v_gen)
      .run();

  // Check correctness of the output
  {
    auto out_v_ref = atlas::array::make_view<double, 2>(out_ref);
    auto out_v_gen = atlas::array::make_view<double, 2>(out_gen);
    UnstructuredVerifier v;
    EXPECT_TRUE(v.compareArrayView(out_v_gen, out_v_ref)) << "while comparing output (on cells)";
  }
}
} // namespace

namespace {
#include <generated_gradient.hpp>
#include <reference_gradient.hpp>

void build_periodic_edges(atlas::Mesh& mesh, int nx, int ny, const AtlasToCartesian& atlasMapper) {
  atlas::mesh::HybridElements::Connectivity& edgeCellConnectivity =
      mesh.edges().cell_connectivity();
  const int missingVal = edgeCellConnectivity.missing_value();

  auto unhash = [](int idx, int nx) -> std::tuple<int, int> {
    int j = idx / nx;
    int i = idx - (j * nx);
    return {i, j};
  };

  for(int edgeIdx = 0; edgeIdx < mesh.edges().size(); edgeIdx++) {
    int numNbh = edgeCellConnectivity.cols(edgeIdx);
    assert(numNbh == 2);

    int nbhLo = edgeCellConnectivity(edgeIdx, 0);
    int nbhHi = edgeCellConnectivity(edgeIdx, 1);

    assert(!(nbhLo == missingVal && nbhHi == missingVal));

    // if we encountered a missing value, we need to fix the neighbor list
    if(nbhLo == missingVal || nbhHi == missingVal) {
      int validIdx = (nbhLo == missingVal) ? nbhHi : nbhLo;
      auto [cellI, cellJ] = unhash(validIdx, nx);
      // depending whether we are vertical or horizontal, we need to reflect either the first or
      // second index
      if(atlasMapper.edgeOrientation(mesh, edgeIdx) == Orientation::Vertical) {
        assert(cellI == nx - 1 || cellI == 0);
        cellI = (cellI == nx - 1) ? 0 : nx - 1;
      } else { // Orientation::Horizontal
        assert(cellJ == ny - 1 || cellJ == 0);
        cellJ = (cellJ == ny - 1) ? 0 : ny - 1;
      }
      int oppositeIdx = cellI + cellJ * nx;
      // ammend the neighbor list
      if(nbhLo == missingVal) {
        edgeCellConnectivity.set(edgeIdx, 0, oppositeIdx);
      } else {
        edgeCellConnectivity.set(edgeIdx, 1, oppositeIdx);
      }
    }
  }
}

TEST(AtlasIntegrationTestCompareOutput, Gradient) {
  // this test computes a gradient in a periodic domain
  //
  //   this is  achieved by reducing a signal from a cell
  //   field onto the edges using the weights [1, -1]. This is equiavlent
  //   to a second order finite difference stencils, missing the division by the cell spacing
  //   (currently omitted).
  //
  //   after this first step, vertical edges contain the x gradient and horizontal edges contain the
  //   y gradient of the original signal. to get the x gradients on the cells (in order to properly
  //   visualize them) the edges are reduced again onto the cells, using weights [0.5, 0, 0, 0.5]
  //
  //   this test uses the AtlasCartesianMapper to assign values

  // kept low for now to get easy debug-able output
  const int numCell = 10;

  // apparently, one needs to be added to the second dimension in order to get a
  // square mesh, or we are mis-interpreting the output
  auto mesh = generateQuadMesh(numCell, numCell + 1);

  AtlasToCartesian atlasToCartesianMapper(mesh);
  build_periodic_edges(mesh, numCell, numCell, atlasToCartesianMapper);

  int nb_levels = 1;

  auto [ref_cells, ref_cells_v] = makeAtlasField("ref_cells", mesh.cells().size(), nb_levels);
  auto [ref_edges, ref_edges_v] = makeAtlasField("ref_edges", mesh.edges().size(), nb_levels);
  auto [gen_cells, gen_cells_v] = makeAtlasField("gen_cells", mesh.cells().size(), nb_levels);
  auto [gen_edges, gen_edges_v] = makeAtlasField("gen_edges", mesh.edges().size(), nb_levels);

  for(int cellIdx = 0, size = mesh.cells().size(); cellIdx < size; ++cellIdx) {
    auto [cartX, cartY] = atlasToCartesianMapper.cellMidpoint(mesh, cellIdx);
    double val =
        sin(cartX * M_PI) * sin(cartY * M_PI); // periodic signal fitting periodic boundaries
    ref_cells_v(cellIdx, 0) = val;
    gen_cells_v(cellIdx, 0) = val;
  }

  dawn_generated::cxxnaiveico::reference_gradient<atlasInterface::atlasTag>(
      mesh, nb_levels, ref_cells_v, ref_edges_v)
      .run();
  dawn_generated::cxxnaiveico::gradient<atlasInterface::atlasTag>(mesh, nb_levels, gen_cells_v,
                                                                  gen_edges_v)
      .run();

  // Check correctness of the output
  {
    auto ref_cells_v = atlas::array::make_view<double, 2>(ref_cells);
    auto gen_cells_v = atlas::array::make_view<double, 2>(gen_cells);
    UnstructuredVerifier v;
    EXPECT_TRUE(v.compareArrayView(ref_cells_v, gen_cells_v))
        << "while comparing output (on cells)";
  }
}
} // namespace

namespace {
#include <generated_tridiagonalSolve.hpp>
TEST(AtlasIntegrationTestCompareOutput, verticalSolver) {
  const int numCell = 5;

  // This tests the unstructured vertical solver
  // A small system with a manufactured solution is generated for each cell

  // apparently, one needs to be added to the second dimension in order to get a
  // square mesh, or we are mis-interpreting the output
  auto mesh = generateQuadMesh(numCell, numCell + 1);

  // the 4 fields required for the thomas algorithm
  //  c.f.
  //  https://en.wikibooks.org/wiki/Algorithm_Implementation/Linear_Algebra/Tridiagonal_matrix_algorithm#C
  int nb_levels = 5;
  auto [a_f, a_v] = makeAtlasField("a", mesh.cells().size(), nb_levels);
  auto [b_f, b_v] = makeAtlasField("b", mesh.cells().size(), nb_levels);
  auto [c_f, c_v] = makeAtlasField("c", mesh.cells().size(), nb_levels);
  auto [d_f, d_v] = makeAtlasField("d", mesh.cells().size(), nb_levels);

  // solution to this problem will be [1,2,3,4,5] at each cell location
  for(int cell = 0; cell < mesh.cells().size(); ++cell) {
    for(int k = 0; k < nb_levels; k++) {
      a_v(cell, k) = k + 1;
      b_v(cell, k) = k + 1;
      c_v(cell, k) = k + 2;
    }

    d_v(cell, 0) = 5;
    d_v(cell, 1) = 15;
    d_v(cell, 2) = 31;
    d_v(cell, 3) = 53;
    d_v(cell, 4) = 45;
  }

  dawn_generated::cxxnaiveico::tridiagonalSolve<atlasInterface::atlasTag>(mesh, nb_levels, a_v, b_v,
                                                                          c_v, d_v)
      .run();

  for(int cell = 0; cell < mesh.cells().size(); ++cell) {
    for(int k = 0; k < nb_levels; k++) {
      EXPECT_TRUE(abs(d_v(cell, k) - (k + 1)) < 1e3 * std::numeric_limits<double>::epsilon());
    }
  }
}
} // namespace

namespace {
#include <generated_nestedSimple.hpp>
TEST(AtlasIntegrationTestCompareOutput, nestedSimple) {
  const int numCell = 10;
  auto mesh = generateQuadMesh(numCell, numCell + 1);

  int nb_levels = 1;
  auto [cells, v_cells] = makeAtlasField("cells", mesh.cells().size(), nb_levels);
  auto [edges, v_edges] = makeAtlasField("edges", mesh.edges().size(), nb_levels);
  auto [nodes, v_nodes] = makeAtlasField("nodes", mesh.nodes().size(), nb_levels);

  initField(v_nodes, mesh.nodes().size(), nb_levels, 1.);

  dawn_generated::cxxnaiveico::nestedSimple<atlasInterface::atlasTag>(mesh, nb_levels, v_cells,
                                                                      v_edges, v_nodes)
      .run();

  // each vertex stores value 1                 1
  // vertices are reduced onto edges            2
  // each face reduces its edges (4 per face)   8
  for(int i = 0; i < mesh.cells().size(); i++) {
    EXPECT_TRUE(fabs(v_cells(i, 0) - 8) < 1e3 * std::numeric_limits<double>::epsilon());
  }
}
} // namespace

namespace {
#include <generated_nestedWithField.hpp>
TEST(AtlasIntegrationTestCompareOutput, nestedWithField) {
  const int numCell = 10;
  auto mesh = generateQuadMesh(numCell, numCell + 1);

  int nb_levels = 1;
  auto [cells, v_cells] = makeAtlasField("cells", mesh.cells().size(), nb_levels);
  auto [edges, v_edges] = makeAtlasField("edges", mesh.edges().size(), nb_levels);
  auto [nodes, v_nodes] = makeAtlasField("nodes", mesh.nodes().size(), nb_levels);

  initField(v_nodes, mesh.nodes().size(), nb_levels, 1.);
  initField(v_edges, mesh.edges().size(), nb_levels, 200.);

  dawn_generated::cxxnaiveico::nestedWithField<atlasInterface::atlasTag>(mesh, nb_levels, v_cells,
                                                                         v_edges, v_nodes)
      .run();

  // each vertex stores value 1                 1
  // vertices are reduced onto edges            2
  // each edge stores 200                     202
  // each face reduces its edges (4 per face) 808
  for(int i = 0; i < mesh.cells().size(); i++) {
    EXPECT_TRUE(fabs(v_cells(i, 0) - 808) < 1e3 * std::numeric_limits<double>::epsilon());
  }
}
} // namespace

namespace {
#include <generated_sparseDimension.hpp>
TEST(AtlasIntegrationTestCompareOutput, sparseDimensions) {
  auto mesh = generateQuadMesh(10, 11);
  const int edgesPerCell = 4;
  const int nb_levels = 1;

  auto [cells_F, cells_v] = makeAtlasField("cells", mesh.cells().size(), nb_levels);
  auto [edges_F, edges_v] = makeAtlasField("edges", mesh.edges().size(), nb_levels);
  auto [sparseDim_F, sparseDim_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), edgesPerCell, nb_levels);

  initSparseField(sparseDim_v, mesh.cells().size(), nb_levels, edgesPerCell, 200.);
  initField(edges_v, mesh.edges().size(), nb_levels, 1.);

  dawn_generated::cxxnaiveico::sparseDimension<atlasInterface::atlasTag>(mesh, nb_levels, cells_v,
                                                                         edges_v, sparseDim_v)
      .run();

  // each edge stores 1                                         1
  // this is multiplied by the sparse dim storing 200         200
  // this is reduced by sum onto the cells at 4 eges p cell   800
  for(int i = 0; i < mesh.cells().size(); i++) {
    EXPECT_TRUE(fabs(cells_v(i, 0) - 800) < 1e3 * std::numeric_limits<double>::epsilon());
  }
}
} // namespace

namespace {
#include <generated_nestedWithSparse.hpp>
TEST(AtlasIntegrationTestCompareOutput, nestedReduceSparseDimensions) {
  auto mesh = generateEquilatMesh(10, 10);
  const int edgesPerCell = 3;
  const int verticesPerEdge = 2;
  const int nb_levels = 1;

  auto [cells_F, cells_v] = makeAtlasField("cells", mesh.cells().size(), nb_levels);
  auto [edges_F, edges_v] = makeAtlasField("edges", mesh.edges().size(), nb_levels);
  auto [nodes_F, nodes_v] = makeAtlasField("nodes", mesh.nodes().size(), nb_levels);

  auto [sparseDim_ce_F, sparseDim_ce_v] =
      makeAtlasSparseField("sparse_ce", mesh.cells().size(), edgesPerCell, nb_levels);
  auto [sparseDim_ev_F, sparseDim_ev_v] =
      makeAtlasSparseField("sparse_ev", mesh.edges().size(), verticesPerEdge, nb_levels);

  initSparseField(sparseDim_ce_v, mesh.cells().size(), nb_levels, edgesPerCell, 200.);
  initSparseField(sparseDim_ev_v, mesh.edges().size(), nb_levels, verticesPerEdge, 300.);
  initField(edges_v, mesh.edges().size(), nb_levels, 1.);
  initField(nodes_v, mesh.nodes().size(), nb_levels, 2.);

  dawn_generated::cxxnaiveico::nestedWithSparse<atlasInterface::atlasTag>(
      mesh, nb_levels, cells_v, edges_v, nodes_v, sparseDim_ce_v, sparseDim_ev_v)
      .run();

  // each vertex stores 2                                                            2
  // this is multiplied by the sparse dim storing 300                              300
  // this is reduced by sum onto edges at 2 verts p edge                          1200
  // each edge stores 1                                                              1
  // this is multiplied by the reduction times the sparse dim storing 200          200
  // this is reduced by sum onto the cells at 3 eges p cell                       4200
  for(int i = 0; i < mesh.cells().size(); i++) {
    EXPECT_TRUE(fabs(cells_v(i, 0) - 4200) < 1e3 * std::numeric_limits<double>::epsilon());
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment0.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment0) {
  auto mesh = generateEquilatMesh(10, 10);
  const int diamondSize = 4;
  const int nb_levels = 1;

  auto [vn_f, vn_v] = makeAtlasSparseField("vn", mesh.edges().size(), diamondSize, nb_levels);
  auto [uVert_f, uVert_v] = makeAtlasField("uVert", mesh.nodes().size(), nb_levels);
  auto [vVert_f, vVert_v] = makeAtlasField("vVert", mesh.nodes().size(), nb_levels);
  auto [nx_f, nx_v] = makeAtlasField("nx", mesh.nodes().size(), nb_levels);
  auto [ny_f, ny_v] = makeAtlasField("ny", mesh.nodes().size(), nb_levels);

  initSparseField(vn_v, mesh.edges().size(), nb_levels, diamondSize, 1.);
  initField(uVert_v, mesh.nodes().size(), nb_levels, 1.);
  initField(vVert_v, mesh.nodes().size(), nb_levels, 2.);
  initField(nx_v, mesh.nodes().size(), nb_levels, 3.);
  initField(ny_v, mesh.nodes().size(), nb_levels, 4.);
  // dot product: vn(e,:) = u*nx + v*ny = 1*3 + 2*4 = 11

  dawn_generated::cxxnaiveico::sparseAssignment0<atlasInterface::atlasTag>(
      mesh, nb_levels, vn_v, uVert_v, vVert_v, nx_v, ny_v)
      .run();

  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t edgeIdx = 0; edgeIdx < mesh.edges().size(); edgeIdx++) {
      bool boundaryEdge = mesh.edges().cell_connectivity()(edgeIdx, 0) ==
                              mesh.edges().cell_connectivity().missing_value() ||
                          mesh.edges().cell_connectivity()(edgeIdx, 1) ==
                              mesh.edges().cell_connectivity().missing_value();
      size_t curDiamondSize = (boundaryEdge) ? 3 : 4;
      for(size_t sparse = 0; sparse < curDiamondSize; sparse++) {
        EXPECT_TRUE(fabs(vn_v(edgeIdx, sparse, level) - 11.) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment1.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment1) {
  auto mesh = generateEquilatMesh(10, 10);
  const int diamondSize = 4;
  const int nb_levels = 1;

  auto [vn_f, vn_v] = makeAtlasSparseField("vn", mesh.edges().size(), diamondSize, nb_levels);
  auto [uVert_f, uVert_v] = makeAtlasField("uVert", mesh.nodes().size(), nb_levels);
  auto [vVert_f, vVert_v] = makeAtlasField("vVert", mesh.nodes().size(), nb_levels);
  auto [nx_f, nx_v] = makeAtlasSparseField("nx", mesh.edges().size(), diamondSize, nb_levels);
  auto [ny_f, ny_v] = makeAtlasSparseField("ny", mesh.edges().size(), diamondSize, nb_levels);

  initSparseField(vn_v, mesh.edges().size(), nb_levels, diamondSize, 1.);
  initField(uVert_v, mesh.nodes().size(), nb_levels, 1.);
  initField(vVert_v, mesh.nodes().size(), nb_levels, 2.);
  initSparseField(nx_v, mesh.edges().size(), nb_levels, diamondSize, 3.);
  initSparseField(ny_v, mesh.edges().size(), nb_levels, diamondSize, 4.);
  // dot product: vn(e,:) = u*nx + v*ny = 1*3 + 2*4 = 11

  dawn_generated::cxxnaiveico::sparseAssignment1<atlasInterface::atlasTag>(
      mesh, nb_levels, vn_v, uVert_v, vVert_v, nx_v, ny_v)
      .run();

  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t edgeIdx = 0; edgeIdx < mesh.edges().size(); edgeIdx++) {
      bool boundaryEdge = mesh.edges().cell_connectivity()(edgeIdx, 0) ==
                              mesh.edges().cell_connectivity().missing_value() ||
                          mesh.edges().cell_connectivity()(edgeIdx, 1) ==
                              mesh.edges().cell_connectivity().missing_value();
      size_t curDiamondSize = (boundaryEdge) ? 3 : 4;
      for(size_t sparse = 0; sparse < curDiamondSize; sparse++) {
        EXPECT_TRUE(fabs(vn_v(edgeIdx, sparse, level) - 11.) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment2.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment2) {
  auto mesh = generateEquilatMesh(10, 10);
  const int diamondSize = 4;
  const int nb_levels = 1;

  auto [sparse_f, sparse_v] =
      makeAtlasSparseField("sparse", mesh.edges().size(), diamondSize, nb_levels);
  auto [edge_f, edge_v] = makeAtlasField("edge", mesh.edges().size(), nb_levels);
  auto [node_f, node_v] = makeAtlasField("node", mesh.nodes().size(), nb_levels);

  initSparseField(sparse_v, mesh.edges().size(), nb_levels, diamondSize, 1.);
  initField(edge_v, mesh.edges().size(), nb_levels, 1.);
  initField(node_v, mesh.nodes().size(), nb_levels, 2.);

  dawn_generated::cxxnaiveico::sparseAssignment2<atlasInterface::atlasTag>(mesh, nb_levels,
                                                                           sparse_v, edge_v, node_v)
      .run();

  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t edgeIdx = 0; edgeIdx < mesh.edges().size(); edgeIdx++) {
      bool boundaryEdge = mesh.edges().cell_connectivity()(edgeIdx, 0) ==
                              mesh.edges().cell_connectivity().missing_value() ||
                          mesh.edges().cell_connectivity()(edgeIdx, 1) ==
                              mesh.edges().cell_connectivity().missing_value();
      size_t curDiamondSize = (boundaryEdge) ? 3 : 4;
      for(size_t sparse = 0; sparse < curDiamondSize; sparse++) {
        EXPECT_TRUE(fabs(sparse_v(edgeIdx, sparse, level) - (-2.)) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment3.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment3) {
  auto mesh = generateEquilatMesh(10, 10);
  const int intpSize = 9;
  const int nb_levels = 1;

  auto [sparse_f, sparse_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), intpSize, nb_levels);
  auto [A_f, A_v] = makeAtlasField("A", mesh.cells().size(), nb_levels);
  auto [B_f, B_v] = makeAtlasField("B", mesh.cells().size(), nb_levels);

  initField(A_v, mesh.cells().size(), nb_levels, 1.);
  for(int cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
    B_v(cellIdx, 0) = cellIdx;
  }

  dawn_generated::cxxnaiveico::sparseAssignment3<atlasInterface::atlasTag>(mesh, nb_levels,
                                                                           sparse_v, A_v, B_v)
      .run();

  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
      size_t curIntpSize =
          atlasInterface::getNeighbors(atlasInterface::atlasTag{}, mesh,
                                       {dawn::LocationType::Cells, dawn::LocationType::Edges,
                                        dawn::LocationType::Cells, dawn::LocationType::Edges,
                                        dawn::LocationType::Cells},
                                       cellIdx)
              .size();
      for(size_t sparse = 0; sparse < curIntpSize; sparse++) {
        EXPECT_TRUE(fabs(sparse_v(cellIdx, sparse, 0) - (1. - cellIdx)) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment4.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment4) {
  auto mesh = generateEquilatMesh(10, 10);
  const int edgesPerCell = 3;
  const int nb_levels = 1;

  auto [sparse_f, sparse_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), edgesPerCell, nb_levels);
  auto [e_f, e_v] = makeAtlasField("e", mesh.nodes().size(), nb_levels);

  initField(e_v, mesh.nodes().size(), nb_levels, 1.);
  dawn_generated::cxxnaiveico::sparseAssignment4<atlasInterface::atlasTag>(mesh, nb_levels,
                                                                           sparse_v, e_v)
      .run();
  // reduce value 1 from vertices to edge (=2), assign to sparse dim

  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
      for(size_t sparse = 0; sparse < edgesPerCell; sparse++) {
        EXPECT_TRUE(fabs(sparse_v(cellIdx, sparse, 0) - 2.) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseAssignment5.hpp>
TEST(AtlasIntegrationTestCompareOutput, SparseAssignment5) {
  auto mesh = generateEquilatMesh(10, 10);
  const int edgesPerCell = 3;
  const int nb_levels = 1;

  auto [sparse_f, sparse_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), edgesPerCell, nb_levels);
  auto [v_f, v_v] = makeAtlasField("e", mesh.nodes().size(), nb_levels);
  auto [c_f, c_v] = makeAtlasField("c", mesh.cells().size(), nb_levels);

  initField(v_v, mesh.nodes().size(), nb_levels, 3.);
  initField(c_v, mesh.cells().size(), nb_levels, 2.);
  dawn_generated::cxxnaiveico::sparseAssignment5<atlasInterface::atlasTag>(mesh, nb_levels,
                                                                           sparse_v, v_v, c_v)
      .run();

  // reduce value 2 from cells to vertex (=12) and multiply by vertex field(=36) reduce this
  // onto edges (two vertices per edge = 72)
  for(size_t level = 0; level < nb_levels; level++) {
    for(size_t cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
      for(size_t sparse = 0; sparse < edgesPerCell; sparse++) {

        // auto e = f.edges()[sparse];
        const auto& connEN = mesh.edges().node_connectivity();
        const auto& connCE = mesh.cells().edge_connectivity();
        int edgeIdx = connCE(cellIdx, sparse);
        int nodeIdx0 = connEN(edgeIdx, 0);
        int nodeIdx1 = connEN(edgeIdx, 1);

        double sol = mesh.nodes().cell_connectivity().cols(nodeIdx0) * 6 +
                     mesh.nodes().cell_connectivity().cols(nodeIdx1) * 6;

        EXPECT_TRUE(fabs(sparse_v(cellIdx, sparse, 0) - sol) <
                    1e3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_sparseDimensionTwice.hpp>
TEST(AtlasIntegrationTestCompareOutput, sparseDimensionsTwice) {
  auto mesh = generateQuadMesh(10, 11);
  const int edgesPerCell = 4;
  const int nb_levels = 1;

  auto [cells_F, cells_v] = makeAtlasField("cells", mesh.cells().size(), nb_levels);
  auto [edges_F, edges_v] = makeAtlasField("edges", mesh.edges().size(), nb_levels);
  auto [sparseDim_F, sparseDim_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), edgesPerCell, nb_levels);

  initSparseField(sparseDim_v, mesh.cells().size(), nb_levels, edgesPerCell, 200.);
  initField(edges_v, mesh.edges().size(), nb_levels, 1.);

  dawn_generated::cxxnaiveico::sparseDimensionTwice<atlasInterface::atlasTag>(
      mesh, nb_levels, cells_v, edges_v, sparseDim_v)
      .run();

  // each edge stores 1                                                1
  // this is multiplied by the sparse dim storing 200                200
  // this is reduced by sum onto the cells at 4 eges p cell          800
  for(int i = 0; i < mesh.cells().size(); i++) {
    EXPECT_TRUE(fabs(cells_v(i, 0) - 800) < 1e3 * std::numeric_limits<double>::epsilon());
  }
  // NOTE that the second reduction simply overwrites the result of the first one since there is
  // "+=" in the IIRBuilder currently
}
} // namespace

namespace {
#include <generated_horizontalVertical.hpp>
TEST(AtlasIntegrationTestCompareOutput, horizontalVertical) {
  auto mesh = generateQuadMesh(10, 11);
  const int nb_levels = 10;
  const int cellsPerEdge = 2;

  auto [horizontal_F, horizontal_v] = makeAtlasField("horizontal", mesh.edges().size(), 1);
  auto [full_F, full_v] = makeAtlasField("full", mesh.edges().size(), nb_levels);
  auto [out1_F, out1_v] = makeAtlasField("out1", mesh.edges().size(), nb_levels);
  auto [out2_F, out2_v] = makeAtlasField("out2", mesh.edges().size(), nb_levels);
  auto [vertical_F, vertical_v] = makeAtlasVerticalField("vertical", nb_levels);
  auto [horizontal_sparse_F, horizontal_sparse_v] =
      makeAtlasSparseField("horizontal_sparse", mesh.edges().size(), cellsPerEdge, 1);

  initField(full_v, mesh.edges().size(), nb_levels, 1.);
  initField(horizontal_v, mesh.edges().size(), 1, 1.);
  initSparseField(horizontal_sparse_v, mesh.edges().size(), 1, cellsPerEdge, 1.);

  for(int k = 0; k < nb_levels; k++) {
    vertical_v(k) = k;
  }

  dawn_generated::cxxnaiveico::horizontalVertical<atlasInterface::atlasTag>(
      mesh, nb_levels, horizontal_v, horizontal_sparse_v, vertical_v, full_v, out1_v, out2_v)
      .run();

  const auto& conn = mesh.edges().cell_connectivity();
  auto is_boundary = [&](int edge_idx) {
    return conn(edge_idx, 0) == conn.missing_value() || conn(edge_idx, 1) == conn.missing_value();
  };
  for(int k = 0; k < nb_levels; k++) {
    for(int edge_iter = 0; edge_iter < mesh.edges().size(); edge_iter++) {
      EXPECT_TRUE(fabs(out1_v(edge_iter, k) - (2 + k)) <
                  1e-3 * std::numeric_limits<double>::epsilon());
    }
    for(int edge_iter = 0; edge_iter < mesh.edges().size(); edge_iter++) {
      if(is_boundary(edge_iter)) {
        EXPECT_TRUE(fabs(out2_v(edge_iter, k) - 1) < 1e-3 * std::numeric_limits<double>::epsilon());
      } else {
        EXPECT_TRUE(fabs(out2_v(edge_iter, k) - cellsPerEdge) <
                    1e-3 * std::numeric_limits<double>::epsilon());
      }
    }
  }
}
} // namespace

namespace {
#include <generated_verticalIndirecion.hpp>
TEST(AtlasIntegrationTestCompareOutput, verticalIndirection) {
  auto mesh = generateQuadMesh(10, 11);
  const int nb_levels = 10;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);
  auto [kidx_F, kidx_v] = makeAtlasField("kidx", mesh.cells().size(), nb_levels);

  for(int k = 0; k < nb_levels; k++) {
    for(int cell_iter = 0; cell_iter < mesh.cells().size(); cell_iter++) {
      in_v(cell_iter, k) = k;
      kidx_v(cell_iter, k) = k + 1;
    }
  }

  dawn_generated::cxxnaiveico::verticalIndirecion<atlasInterface::atlasTag>(mesh, nb_levels, in_v,
                                                                            out_v, kidx_v)
      .run();

  for(int k = 0; k < nb_levels - 1; k++) {
    for(int cell_iter = 0; cell_iter < mesh.cells().size(); cell_iter++) {
      EXPECT_TRUE(out_v(cell_iter, k) == k + 1);
    }
  }
}
} // namespace

namespace {
#include <generated_iterationSpaceUnstructured.hpp>
TEST(AtlasIntegrationTestCompareOutput, iterationSpaceUnstructured) {
  auto mesh = generateQuadMesh(10, 11);
  const int nb_levels = 1;
  const int level = 0;

  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);
  auto [in1_F, in1_v] = makeAtlasField("in_1", mesh.cells().size(), nb_levels);
  auto [in2_F, in2_v] = makeAtlasField("in_2", mesh.cells().size(), nb_levels);

  const int interiorIdx = 20;
  const int haloIdx = 80;

  const int interiorVal = 2;
  const int haloVal = 1;

  for(int cell_iter = interiorIdx; cell_iter < haloIdx; cell_iter++) {
    in2_v(cell_iter, level) = interiorVal;
  }
  for(int cell_iter = haloIdx; cell_iter < mesh.cells().size(); cell_iter++) {
    in1_v(cell_iter, level) = haloVal;
  }

  dawn_generated::cxxnaiveico::iterationSpaceUnstructured<atlasInterface::atlasTag> stencil(
      mesh, nb_levels, out_v, in1_v, in2_v);

  stencil.set_splitter_index(dawn::LocationType::Cells, dawn::UnstructuredSubdomain::Interior, 0,
                             interiorIdx);
  stencil.set_splitter_index(dawn::LocationType::Cells, dawn::UnstructuredSubdomain::Halo, 0,
                             haloIdx);
  stencil.set_splitter_index(dawn::LocationType::Cells, dawn::UnstructuredSubdomain::End, 0,
                             mesh.cells().size());
  stencil.run();

  for(int cell_iter = interiorIdx; cell_iter < haloIdx; cell_iter++) {
    EXPECT_TRUE(out_v(cell_iter, level) == interiorVal);
  }
  for(int cell_iter = haloIdx; cell_iter < mesh.cells().size(); cell_iter++) {
    EXPECT_TRUE(out_v(cell_iter, level) == haloVal);
  }
}
} // namespace

namespace {
#include <generated_globalVar.hpp>
TEST(AtlasIntegrationTestCompareOutput, globalVar) {
  auto mesh = generateQuadMesh(10, 10);
  size_t nb_levels = 10;
  const double dt = 2.0;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(in_v, mesh.cells().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::globalVar<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v, out_v);
  stencil.set_dt(dt);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(out_v(cell_idx, k), dt);
    }
  }
}
} // namespace

namespace {
#include <generated_tempFieldAllocation.hpp>
TEST(AtlasIntegrationTestCompareOutput, tempFieldAllocation) {
  auto mesh = generateQuadMesh(10, 10);
  size_t nb_levels = 10;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(in_v, mesh.cells().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::tempFieldAllocation<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v, out_v);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(out_v(cell_idx, k), 2.);
    }
  }
}
} // namespace

namespace {
#include <generated_sparseTempFieldAllocation.hpp>
TEST(AtlasIntegrationTestCompareOutput, sparseTempFieldAllocation) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 10;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(in_v, mesh.cells().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::sparseTempFieldAllocation<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v, out_v);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(out_v(cell_idx, k), 3.);
    }
  }
}
} // namespace

namespace {
#include <generated_reductionInIfConditional.hpp>
TEST(AtlasIntegrationTestCompareOutput, reductionInConditional) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 1;
  const int level = 0;
  const int edgesPerCell = 3;

  auto [in_F, in_v] = makeAtlasField("in", mesh.cells().size(), nb_levels);
  auto [e_F, e_v] = makeAtlasField("e", mesh.edges().size(), nb_levels);
  auto [sparse_F, sparse_v] =
      makeAtlasSparseField("sparse", mesh.cells().size(), edgesPerCell, nb_levels);
  auto [out_F, out_v] = makeAtlasField("out", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(in_v, mesh.cells().size(), nb_levels, 1.0);
  initField(out_v, mesh.cells().size(), nb_levels, -1.0);
  initField(e_v, mesh.edges().size(), nb_levels, -1.0);
  initSparseField(sparse_v, mesh.cells().size(), nb_levels, edgesPerCell, 1.);

  auto isBoundaryEdge = [mesh](const int edgeIdx) -> bool {
    return mesh.edges().cell_connectivity()(edgeIdx, 0) ==
               mesh.edges().cell_connectivity().missing_value() ||
           mesh.edges().cell_connectivity()(edgeIdx, 1) ==
               mesh.edges().cell_connectivity().missing_value();
  };

  for(int edgeIdx = 0; edgeIdx < mesh.edges().size(); edgeIdx++) {
    e_v(edgeIdx, level) = isBoundaryEdge(edgeIdx) ? 0 : 1;
  }

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::reductionInIfConditional<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), in_v, sparse_v, e_v, out_v);
  stencil.run();

  // Check correctness of the output
  {
    const auto& conn = mesh.cells().edge_connectivity();
    for(int cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
      int numInt = 0;
      for(int nbhIdx = 0; nbhIdx < conn.cols(cellIdx); nbhIdx++) {
        int edgeIdx = conn(cellIdx, nbhIdx);
        if(!isBoundaryEdge(edgeIdx)) {
          numInt++;
        }
      }
      if(numInt < 3) {
        ASSERT_EQ(out_v(cellIdx), 6);
      } else {
        ASSERT_EQ(out_v(cellIdx), 12);
      }
    }
  }
}
} // namespace

namespace {
#include <generated_reductionWithCenter.hpp>
TEST(AtlasIntegrationTestCompareOutput, reductionWithCenter) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 1;

  auto [cout_F, cout_v] = makeAtlasField("cout", mesh.cells().size(), nb_levels);
  auto [cin_F, cin_v] = makeAtlasField("cin", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(cout_v, mesh.cells().size(), nb_levels, .0);
  initField(cin_v, mesh.cells().size(), nb_levels, 1.0);

  auto isBoundaryEdge = [mesh](const int edgeIdx) -> bool {
    return mesh.edges().cell_connectivity()(edgeIdx, 0) ==
               mesh.edges().cell_connectivity().missing_value() ||
           mesh.edges().cell_connectivity()(edgeIdx, 1) ==
               mesh.edges().cell_connectivity().missing_value();
  };

  std::vector<size_t> nnbh_c2c(mesh.cells().size());
  const auto& conn = mesh.cells().edge_connectivity();
  for(int cIdx = 0; cIdx < mesh.cells().size(); cIdx++) {
    int nnbh = 0;
    for(int nbhIdx = 0; nbhIdx < conn.cols(cIdx); nbhIdx++) {
      int eIdx = conn(cIdx, nbhIdx);
      if(eIdx != conn.missing_value()) {
        nnbh += !isBoundaryEdge(eIdx) ? 1 : 0;
      }
    }
    nnbh_c2c[cIdx] = nnbh;
  }

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::reductionWithCenter<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), cin_v, cout_v);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(cout_v(cell_idx, k), nnbh_c2c[cell_idx] + 1);
    }
  }
}
} // namespace

namespace {
#include <generated_reductionWithCenterSparse.hpp>
TEST(AtlasIntegrationTestCompareOutput, reductionWithCenterSparse) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 1;
  const int CEC_SIZE = 3;
  const double sparse_val = 2.;

  auto [cout_F, cout_v] = makeAtlasField("cout", mesh.cells().size(), nb_levels);
  auto [cin_F, cin_v] = makeAtlasField("cin", mesh.cells().size(), nb_levels);
  auto [sparse_F, sparse_v] =
      makeAtlasSparseField("cin", mesh.cells().size(), CEC_SIZE + 1, nb_levels);

  // Initialize fields with data
  initField(cout_v, mesh.cells().size(), nb_levels, .0);
  initField(cin_v, mesh.cells().size(), nb_levels, 1.0);
  initSparseField(sparse_v, mesh.cells().size(), nb_levels, CEC_SIZE + 1, sparse_val);

  auto isBoundaryEdge = [mesh](const int edgeIdx) -> bool {
    return mesh.edges().cell_connectivity()(edgeIdx, 0) ==
               mesh.edges().cell_connectivity().missing_value() ||
           mesh.edges().cell_connectivity()(edgeIdx, 1) ==
               mesh.edges().cell_connectivity().missing_value();
  };

  std::vector<size_t> nnbh_c2c(mesh.cells().size());
  const auto& conn = mesh.cells().edge_connectivity();
  for(int cIdx = 0; cIdx < mesh.cells().size(); cIdx++) {
    int nnbh = 0;
    for(int nbhIdx = 0; nbhIdx < conn.cols(cIdx); nbhIdx++) {
      int eIdx = conn(cIdx, nbhIdx);
      if(eIdx != conn.missing_value()) {
        nnbh += !isBoundaryEdge(eIdx) ? 1 : 0;
      }
    }
    nnbh_c2c[cIdx] = nnbh;
  }

  // Run the stencil
  auto stencil = dawn_generated::cxxnaiveico::reductionWithCenterSparse<atlasInterface::atlasTag>(
      mesh, static_cast<int>(nb_levels), cin_v, cout_v, sparse_v);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(cout_v(cell_idx, k), sparse_val * nnbh_c2c[cell_idx] + sparse_val);
    }
  }
}
} // namespace

namespace {
#include <generated_reductionAndFillWithCenterSparse.hpp>
TEST(AtlasIntegrationTestCompareOutput, reductionAndFillWithCenterSparse) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 1;
  const double sparse_val = 2.;

  auto [cout_F, cout_v] = makeAtlasField("cout", mesh.cells().size(), nb_levels);
  auto [cin_F, cin_v] = makeAtlasField("cin", mesh.cells().size(), nb_levels);

  // Initialize fields with data
  initField(cout_v, mesh.cells().size(), nb_levels, .0);
  initField(cin_v, mesh.cells().size(), nb_levels, 1.0);

  auto isBoundaryEdge = [mesh](const int edgeIdx) -> bool {
    return mesh.edges().cell_connectivity()(edgeIdx, 0) ==
               mesh.edges().cell_connectivity().missing_value() ||
           mesh.edges().cell_connectivity()(edgeIdx, 1) ==
               mesh.edges().cell_connectivity().missing_value();
  };

  std::vector<size_t> nnbh_c2c(mesh.cells().size());
  const auto& conn = mesh.cells().edge_connectivity();
  for(int cIdx = 0; cIdx < mesh.cells().size(); cIdx++) {
    int nnbh = 0;
    for(int nbhIdx = 0; nbhIdx < conn.cols(cIdx); nbhIdx++) {
      int eIdx = conn(cIdx, nbhIdx);
      if(eIdx != conn.missing_value()) {
        nnbh += !isBoundaryEdge(eIdx) ? 1 : 0;
      }
    }
    nnbh_c2c[cIdx] = nnbh;
  }

  // Run the stencil
  auto stencil =
      dawn_generated::cxxnaiveico::reductionAndFillWithCenterSparse<atlasInterface::atlasTag>(
          mesh, static_cast<int>(nb_levels), cin_v, cout_v);
  stencil.run();

  // Check correctness of the output
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size(); ++cell_idx) {
      ASSERT_EQ(cout_v(cell_idx, k), sparse_val * nnbh_c2c[cell_idx] + sparse_val);
    }
  }
}
} // namespace

namespace {
#include <generated_padding.hpp>
TEST(AtlasIntegrationTestCompareOutput, padding) {
  auto mesh = generateEquilatMesh(10, 10);
  size_t nb_levels = 10;

  const int padding_cells = 10;
  const int padding_edges = 20;
  const int padding_vertices = 30;

  // over commit memory
  auto [c_F, c_v] = makeAtlasField("c", mesh.cells().size() + padding_cells, nb_levels);
  auto [e_F, e_v] = makeAtlasField("e", mesh.edges().size() + padding_edges, nb_levels);
  auto [v_F, v_v] = makeAtlasField("v", mesh.nodes().size() + padding_vertices, nb_levels);

  // Initialize fields with data
  initField(c_v, mesh.cells().size() + padding_cells, nb_levels, -1.);
  initField(e_v, mesh.edges().size() + padding_edges, nb_levels, -1.);
  initField(v_v, mesh.nodes().size() + padding_vertices, nb_levels, -1.);

  dawn_generated::cxxnaiveico::padding<atlasInterface::atlasTag>(mesh, nb_levels, c_v, e_v, v_v)
      .run();

  // assert that payload is written to while padding area is left alone
  for(int k = 0; k < nb_levels; k++) {
    for(int cell_idx = 0; cell_idx < mesh.cells().size() + padding_cells; ++cell_idx) {
      if(cell_idx < mesh.cells().size()) {
        ASSERT_EQ(c_v(cell_idx, k), 1.);
      } else {
        ASSERT_EQ(c_v(cell_idx, k), -1.);
      }
    }
  }
  for(int k = 0; k < nb_levels; k++) {
    for(int edge_idx = 0; edge_idx < mesh.edges().size() + padding_edges; ++edge_idx) {
      if(edge_idx < mesh.edges().size()) {
        ASSERT_EQ(e_v(edge_idx, k), 1.);
      } else {
        ASSERT_EQ(e_v(edge_idx, k), -1.);
      }
    }
  }
  for(int k = 0; k < nb_levels; k++) {
    for(int vertex_idx = 0; vertex_idx < mesh.nodes().size() + padding_cells; ++vertex_idx) {
      if(vertex_idx < mesh.nodes().size()) {
        ASSERT_EQ(v_v(vertex_idx, k), 1.);
      } else {
        ASSERT_EQ(v_v(vertex_idx, k), -1.);
      }
    }
  }
}
} // namespace

} // namespace