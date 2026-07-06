# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(
    aegir-genie
    VERSION 0.1.0
    DESCRIPTION "GENIE neutrino event generation plugin for the aegir simulation framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies (from the default pixi environment)
find_package(SHiPDataModel REQUIRED)

# Phlex's cmake config does not call find_dependency for its transitive deps
# (still true as of phlex 0.3.0: its generated deps block is empty), so declare
# them explicitly before find_package(phlex).
find_package(Boost REQUIRED COMPONENTS json)
find_package(TBB REQUIRED)
find_package(spdlog REQUIRED)
find_package(phlex REQUIRED)
find_package(
    ROOT
    6.40
    REQUIRED
    COMPONENTS
        Core
        RIO
        Tree
        Hist
        MathCore
        MathMore
        Matrix
        Physics
        GenVector
        EG
        Geom
        Gdml
        ROOTNTuple
)

find_path(Random123_INCLUDE_DIR Random123/philox.h REQUIRED)

# ── GENIE ────────────────────────────────────────────────────────────────────
# GENIE has no CMake config; create an imported target from genie-config
# (mirroring aegir's pythia8-config pattern). By default genie-config is
# located on PATH (the pixi environment); pass -DGENIE_PREFIX=<prefix> to
# use an out-of-environment GENIE installation instead.
if(NOT GENIE_PREFIX)
    find_program(GENIE_CONFIG genie-config REQUIRED)
    get_filename_component(GENIE_BINDIR ${GENIE_CONFIG} DIRECTORY)
    get_filename_component(GENIE_PREFIX ${GENIE_BINDIR} DIRECTORY)
else()
    find_program(
        GENIE_CONFIG genie-config
        PATHS ${GENIE_PREFIX}/bin
        NO_DEFAULT_PATH
        REQUIRED
    )
endif()

# genie-config needs $GENIE (the runtime/config area, share/genie in the
# conda package) to resolve its Make.config; derive it from the prefix.
if(DEFINED ENV{GENIE})
    set(GENIE_ENV_DIR $ENV{GENIE})
else()
    set(GENIE_ENV_DIR ${GENIE_PREFIX}/share/genie)
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GENIE=${GENIE_ENV_DIR} ${GENIE_CONFIG}
            --libs
    OUTPUT_VARIABLE GENIE_CONFIG_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT GENIE_CONFIG_LIBS MATCHES "-lGFwEG")
    message(
        FATAL_ERROR
        "genie-config --libs returned '${GENIE_CONFIG_LIBS}', which does not "
        "look like a GENIE installation (is \$GENIE usable?)"
    )
endif()

# Parse "-L<dir> -lA -lB ..." into absolute library paths: GENIE's shared
# libraries record no DT_NEEDED entries at all, so make every dependency
# explicit rather than trusting the link line to resolve transitively.
separate_arguments(GENIE_CONFIG_LIBS)
set(GENIE_LIBDIR "")
set(GENIE_LIBRARIES "")
foreach(arg IN LISTS GENIE_CONFIG_LIBS)
    if(arg MATCHES "^-L(.+)")
        set(GENIE_LIBDIR ${CMAKE_MATCH_1})
    elseif(arg MATCHES "^-l(.+)")
        list(APPEND GENIE_LIBRARIES ${CMAKE_MATCH_1})
    endif()
endforeach()
if(NOT GENIE_LIBDIR OR NOT EXISTS "${GENIE_LIBDIR}")
    message(
        FATAL_ERROR
        "genie-config --libs reported library directory '${GENIE_LIBDIR}', which does not exist"
    )
endif()

set(GENIE_LIBRARY_FILES "")
foreach(lib IN LISTS GENIE_LIBRARIES)
    set(lib_file
        "${GENIE_LIBDIR}/lib${lib}${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )
    if(NOT EXISTS "${lib_file}")
        message(FATAL_ERROR "GENIE library not found: ${lib_file}")
    endif()
    list(APPEND GENIE_LIBRARY_FILES ${lib_file})
endforeach()

# GENIE's public headers (Messenger.h) include log4cpp headers. They must
# come from the default environment: adding ${GENIE_PREFIX}/include would
# also expose that environment's C++20 ROOT headers, shadowing the default
# ROOT. The log4cpp *library* is still linked from the GENIE prefix below.
find_path(LOG4CPP_INCLUDE_DIR log4cpp/Category.hh REQUIRED)

# GENIE's underlinked libraries need these resolved at link time; take them
# from the same prefix as GENIE so they match its build exactly.
foreach(dep log4cpp xml2 LHAPDF EGPythia6 Pythia6 gsl gslcblas)
    find_library(
        GENIE_DEP_${dep}
        ${dep}
        PATHS ${GENIE_PREFIX}/lib
        NO_DEFAULT_PATH
        REQUIRED
    )
    list(APPEND GENIE_DEP_LIBRARIES ${GENIE_DEP_${dep}})
endforeach()

add_library(GENIE::GENIE INTERFACE IMPORTED)
target_include_directories(
    GENIE::GENIE
    INTERFACE ${GENIE_PREFIX}/include/GENIE ${LOG4CPP_INCLUDE_DIR}
)
# --no-as-needed: conda's default LDFLAGS include --as-needed, which prunes
# GENIE libraries the consumer does not reference directly — but GENIE's
# underlinked libraries resolve their own symbols through whatever the
# consumer loaded, so the full set must stay in DT_NEEDED.
target_link_libraries(
    GENIE::GENIE
    INTERFACE
        -Wl,--push-state,--no-as-needed
        ${GENIE_LIBRARY_FILES}
        ${GENIE_DEP_LIBRARIES}
        -Wl,--pop-state
        ROOT::Core
        ROOT::RIO
        ROOT::Tree
        ROOT::Hist
        ROOT::MathCore
        ROOT::MathMore
        ROOT::Matrix
        ROOT::Physics
        ROOT::GenVector
        ROOT::EG
        ROOT::Geom
)

# ── targets ──────────────────────────────────────────────────────────────────

# Phlex plugin: the embedded GENIE event generator source.
add_library(genie_source MODULE src/genie_source.cpp src/ship_flux_driver.cpp)
# ROOT targets are listed before GENIE::GENIE so the default environment's
# library directory precedes the GENIE prefix (which also holds a C++20 ROOT
# build) in the generated RPATH.
target_link_libraries(
    genie_source
    PRIVATE
        phlex::module
        SHiP::SHiPDataModel
        ROOT::ROOTNTuple
        ROOT::Gdml
        GENIE::GENIE
)
target_include_directories(genie_source PRIVATE ${Random123_INCLUDE_DIR})

# Standalone flux-driver test: no phlex, no GMCJDriver, no splines — this
# runs today (see tests/flux_driver_test.cpp).
add_executable(flux_driver_test tests/flux_driver_test.cpp
    src/ship_flux_driver.cpp
)
target_link_libraries(
    flux_driver_test
    PRIVATE ROOT::ROOTNTuple GENIE::GENIE
)
target_include_directories(
    flux_driver_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Install
include(GNUInstallDirs)
install(TARGETS genie_source LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY workflows/ DESTINATION share/aegir-genie/workflows)

# Tests
enable_testing()
add_test(
    NAME flux_driver
    COMMAND flux_driver_test
        ${CMAKE_CURRENT_BINARY_DIR}/flux_driver_test_synthetic.root
)
