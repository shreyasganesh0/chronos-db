cmake_minimum_required(VERSION 3.20)

# Project Definition
project(ChronosDB 
    VERSION 0.1.0 
    LANGUAGES CXX
)

# ==============================================================================
# 1. COMPILER STANDARDS (The "Google/Meta" Config)
# ==============================================================================

# Force C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Ensure we use ISO C++, not GCC extensions

# Define the "Systems Programming" Warning/Error Set
# We put this in a variable so we can apply it to specific targets later.
set(SYSTEMS_FLAGS
    -Wall -Wextra -Werror -pedantic # Zero tolerance for warnings
    -fno-exceptions                 # NO TRY/CATCH. Forces Result<T> pattern.
    -fno-rtti                       # NO DYNAMIC_CAST. Forces static polymorphism.
    -O3                             # Maximize optimization
    -march=native                   # Optimize for YOUR specific CPU
)

# ==============================================================================
# 2. DEPENDENCIES
# ==============================================================================

# We will need liburing later. 
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING liburing) 

# ==============================================================================
# 3. THE ENGINE LIBRARY (chronos_core)
# ==============================================================================

add_library(chronos_core STATIC
    src/io/uring_context.cpp 
)

# Include Directories
# PUBLIC means: "Anyone who uses this lib also gets access to 'include/'"
target_include_directories(chronos_core PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
)

# Apply our strict flags to the library
target_compile_options(chronos_core PRIVATE ${SYSTEMS_FLAGS})

# Link external dependencies (commented out until you actually use liburing calls)
# target_link_libraries(chronos_core PRIVATE ${LIBURING_LIBRARIES})

# ==============================================================================
# 4. THE EXECUTABLE (chronos_server)
# ==============================================================================

add_executable(chronos_server
    src/main.cpp
)

# Link our engine library to the executable
target_link_libraries(chronos_server PRIVATE chronos_core)

# Apply flags to the executable as well
target_compile_options(chronos_server PRIVATE ${SYSTEMS_FLAGS})

# ==============================================================================
# 5. TESTING (Future Proofing)
# ==============================================================================
enable_testing()
# add_subdirectory(tests) 
