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

add_subdirectory(dawn)
# Pytest is confused when the test directory is named 'dawn4py', so we added '-tests'
add_subdirectory(dawn4py-tests)
add_subdirectory(driver-includes)

if(atlas_FOUND AND eckit_FOUND)
    add_subdirectory(interface)
endif()
