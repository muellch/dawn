#!/usr/bin/env python

# ===-----------------------------------------------------------------------------*- Python -*-===##
#                          _
#                         | |
#                       __| | __ ___      ___ ___
#                      / _` |/ _` \ \ /\ / / '_  |
#                     | (_| | (_| |\ V  V /| | | |
#                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
#
#
#  This file is distributed under the MIT License (MIT).
#  See LICENSE.txt for details.
#
# ===------------------------------------------------------------------------------------------===##

"""Copy stencil HIR generator

This program creates the HIR corresponding to a stencil employing vertical indirection using the SIR serialization Python API.
The code is meant as an example for high-level DSLs that could generate HIR from their own
internal IR.
"""

import argparse
import os

import dawn4py
from dawn4py.serialization import SIR, AST
from dawn4py.serialization import utils as serial_utils
from dawn4py.serialization import to_json as sir_to_json
from google.protobuf.json_format import MessageToJson, Parse

OUTPUT_NAME = "vertical_indirection_stencil"
OUTPUT_FILE = f"{OUTPUT_NAME}.cpp"
OUTPUT_PATH = f"{OUTPUT_NAME}.cpp"


def main(args: argparse.Namespace):
    interval = serial_utils.make_interval(
        AST.Interval.Start, AST.Interval.End, 0, 0)

    # out[c,k] = in[c,vert_nbh[k]]
    body_ast_1 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("out"),
                serial_utils.make_unstructured_field_access_expr(
                    "in", vertical_shift=0, vertical_indirection="vert_nbh"),

                "=")

        ]
    )

    # out[c,k] = in[c,vert_nbh[k]+1]
    body_ast_2 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("out"),
                serial_utils.make_unstructured_field_access_expr(
                    "in", vertical_shift=1, vertical_indirection="vert_nbh"),

                "=")

        ]
    )

    # in_out[c,k] = in_out[c,vert_nbh[k]+1]
    body_ast_3 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("in_out"),
                serial_utils.make_unstructured_field_access_expr(
                    "in_out", vertical_shift=1, vertical_indirection="vert_nbh"),

                "=")

        ]
    )

    # vert_nbh[c,k] = vert_nbh[c,k+1]
    # out[c,k] = in[c,vert_nbh[k]]
    body_ast_4 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("vert_nbh"),
                serial_utils.make_unstructured_field_access_expr(
                    "vert_nbh", vertical_shift=1),

                "="),
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("out"),
                serial_utils.make_unstructured_field_access_expr(
                    "in", vertical_shift=0, vertical_indirection="vert_nbh"),

                "=")

        ]
    )

    # vert_nbh[c,k] = vert_nbh[c,k+1]
    # in_out[c,k] = in_out[c,vert_nbh[k]+1]
    body_ast_5 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("vert_nbh"),
                serial_utils.make_unstructured_field_access_expr(
                    "vert_nbh", vertical_shift=1),

                "="),
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("in_out"),
                serial_utils.make_unstructured_field_access_expr(
                    "in_out", vertical_shift=1, vertical_indirection="vert_nbh"),

                "=")

        ]
    )

    # in_out[c,k] = in_out[c,vert_nbh[k]-1]
    # technically a solver, but vert_nbh makes it a stencil
    #       => access expected to be treated like a stencil
    body_ast_6 = serial_utils.make_ast(
        [
            serial_utils.make_assignment_stmt(
                serial_utils.make_field_access_expr("in"),
                serial_utils.make_unstructured_field_access_expr(
                    "in", vertical_shift=-1, vertical_indirection="vert_nbh"),

                "="),

        ]
    )

    vertical_region_stmt_1 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_1, interval, AST.VerticalRegion.Forward
    )

    vertical_region_stmt_2 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_2, interval, AST.VerticalRegion.Forward
    )

    vertical_region_stmt_3 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_3, interval, AST.VerticalRegion.Forward
    )

    vertical_region_stmt_4 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_4, interval, AST.VerticalRegion.Forward
    )

    vertical_region_stmt_5 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_5, interval, AST.VerticalRegion.Forward
    )

    vertical_region_stmt_6 = serial_utils.make_vertical_region_decl_stmt(
        body_ast_6, interval, AST.VerticalRegion.Forward
    )

    sir = serial_utils.make_sir(
        OUTPUT_FILE,
        AST.GridType.Value("Unstructured"),
        [
            serial_utils.make_stencil(
                OUTPUT_NAME,
                serial_utils.make_ast(
                    [vertical_region_stmt_1,
                     vertical_region_stmt_2,
                     vertical_region_stmt_3,
                     vertical_region_stmt_4,
                     vertical_region_stmt_5,
                     vertical_region_stmt_6]),
                [
                    serial_utils.make_field(
                        "in",
                        serial_utils.make_field_dimensions_unstructured(
                            [AST.LocationType.Value("Cell")], 1
                        ),
                    ),
                    serial_utils.make_field(
                        "in_out",
                        serial_utils.make_field_dimensions_unstructured(
                            [AST.LocationType.Value("Cell")], 1
                        ),
                    ),
                    serial_utils.make_field(
                        "out",
                        serial_utils.make_field_dimensions_unstructured(
                            [AST.LocationType.Value("Cell")], 1
                        ),
                    ),
                    serial_utils.make_field(
                        "vert_nbh",
                        serial_utils.make_field_dimensions_unstructured(
                            [AST.LocationType.Value("Cell")], 1
                        ),
                    ),
                ],
            ),
        ],
    )

    # print the SIR       
    if args.verbose:
        print(MessageToJson(sir))

    # extend default passes by non standard passes that could potentially be affected
    # by indirected vertical reads
    pass_groups = dawn4py.default_pass_groups()
    pass_groups.insert(1, dawn4py.PassGroup.MultiStageMerger)
    pass_groups.insert(1, dawn4py.PassGroup.SetLoopOrder)
    pass_groups.insert(1, dawn4py.PassGroup.SetNonTempCaches)

    # compile
    code = dawn4py.compile(sir, groups=pass_groups,
                           backend=dawn4py.CodeGenBackend.CUDAIco)

    # with open("out.json", "w+") as f:
    #     f.write(sir_to_json(sir))

    # write to file
    print(f"Writing generated code to '{OUTPUT_PATH}'")
    with open(OUTPUT_PATH, "w") as f:
        f.write(code)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a simple unstructured copy stencil using Dawn compiler"
    )
    parser.add_argument(
        "-v",
        "--verbose",
        dest="verbose",
        action="store_true",
        default=False,
        help="Print the generated SIR",
    )
    main(parser.parse_args())
