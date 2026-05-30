# ---------------------------------------------------------------------------
# Third-party / system dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Sqlpp11 CONFIG REQUIRED COMPONENTS MariaDB)
find_package(Protobuf REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Code Generation (DDL -> C++)
# ---------------------------------------------------------------------------
set(DDL2CPP_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/ddl2cpp")
set(SCHEMA_SQL "${CMAKE_SOURCE_DIR}/sql/schema.sql")
set(GENERATED_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/model")
set(GENERATED_SCHEMA_HEADER "${GENERATED_MODEL_DIR}/schema.h")

# Create the output directory if it doesn't exist (although we made it via shell, good for portability)
file(MAKE_DIRECTORY ${GENERATED_MODEL_DIR})

add_custom_command(
    OUTPUT ${GENERATED_SCHEMA_HEADER}
    COMMAND Python3::Interpreter ${DDL2CPP_SCRIPT} ${SCHEMA_SQL} ${GENERATED_MODEL_DIR}/schema model
    DEPENDS ${SCHEMA_SQL} ${DDL2CPP_SCRIPT}
    COMMENT "Generating C++ models from SQL schema using ddl2cpp"
    VERBATIM
)

# Create a target to ensure generation happens before compilation
add_custom_target(generate_sql_models DEPENDS ${GENERATED_SCHEMA_HEADER})

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PROJECT_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/buffer/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/handler/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/log/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/pool/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/timer/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/dao/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/service/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp"
)

# Remove main.cpp from the library source list
list(REMOVE_ITEM PROJECT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

# ---------------------------------------------------------------------------
# Define static library
# ---------------------------------------------------------------------------
add_library(termchat_core STATIC ${PROJECT_SRC} ${GENERATED_SCHEMA_HEADER})
add_dependencies(termchat_core generate_sql_models)

# Public include path
target_include_directories(termchat_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${cpp_rs_driver_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Link all dependencies
# ---------------------------------------------------------------------------
target_link_libraries(termchat_core PUBLIC 
    Threads::Threads 
    protobuf::libprotobuf
    sqlpp11::sqlpp11
    sqlpp11::mariadb
    jwt-cpp::jwt-cpp
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    scylla-cpp-driver
)

if(TARGET proto_generated)
    target_link_libraries(termchat_core PUBLIC proto_generated)
endif()

# ---------------------------------------------------------------------------
# Server executable target
# ---------------------------------------------------------------------------
add_executable(server "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

target_link_libraries(server PRIVATE termchat_core)
