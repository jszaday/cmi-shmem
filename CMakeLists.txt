cmake_minimum_required(VERSION 3.18)

project (CMI-SHMEM)

set(CHARMC_PATH $ENV{CHARM_HOME})
if(CHARMC_PATH)
    message(STATUS "Charm compiler found at: ${CHARMC_PATH}")
else(CHARMC_PATH)
    message(FATAL_ERROR "Charm compiler not found, please update the environment variable CHARM_HOME to the right location.")
endif(CHARMC_PATH)

set(CMAKE_C_COMPILER "${CHARMC_PATH}/bin/charmc")
set(CMAKE_CXX_COMPILER "${CHARMC_PATH}/bin/charmc")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(src)
add_subdirectory(demo)
