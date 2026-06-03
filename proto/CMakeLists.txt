find_package(protobuf CONFIG REQUIRED)

# Define output directories for generated files
set(PROTO_CPP_DIR ${CMAKE_CURRENT_BINARY_DIR}/cpp)
set(PROTO_PY_DIR ${CMAKE_CURRENT_BINARY_DIR}/python)
set(PROTO_GO_DIR ${CMAKE_CURRENT_BINARY_DIR}/go)

# Create output directories
file(MAKE_DIRECTORY ${PROTO_CPP_DIR})
file(MAKE_DIRECTORY ${PROTO_PY_DIR})
file(MAKE_DIRECTORY ${PROTO_GO_DIR})

# Explicit proto list to avoid stale deleted files
set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/common.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/auth_service.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/friend_service.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/message_service.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol.proto
)

set(PROTO_SRCS)
set(PROTO_HDRS)
set(PROTO_PYS)
set(PROTO_PYIS)
set(PROTO_GOS)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PROTO_SRC "${PROTO_CPP_DIR}/${PROTO_NAME}.pb.cc")
    set(PROTO_HDR "${PROTO_CPP_DIR}/${PROTO_NAME}.pb.h")
    set(PROTO_PY "${PROTO_PY_DIR}/${PROTO_NAME}_pb2.py")
    set(PROTO_PYI "${PROTO_PY_DIR}/${PROTO_NAME}_pb2.pyi")
    set(PROTO_GO "${PROTO_GO_DIR}/${PROTO_NAME}.pb.go")

    set(PROTO_OUTPUTS ${PROTO_SRC} ${PROTO_HDR})
    set(PROTO_ARGS --cpp_out=${PROTO_CPP_DIR})

    list(APPEND PROTO_SRCS ${PROTO_SRC})
    list(APPEND PROTO_HDRS ${PROTO_HDR})

    if(BUILD_PROTO_PYTHON)
        list(APPEND PROTO_OUTPUTS ${PROTO_PY} ${PROTO_PYI})
        list(APPEND PROTO_ARGS --python_out=${PROTO_PY_DIR} --pyi_out=${PROTO_PY_DIR})
        list(APPEND PROTO_PYS ${PROTO_PY})
        list(APPEND PROTO_PYIS ${PROTO_PYI})
    endif()

    if(BUILD_PROTO_GO)
        list(APPEND PROTO_OUTPUTS ${PROTO_GO})
        list(APPEND PROTO_ARGS --go_out=${PROTO_GO_DIR} --go_opt=paths=source_relative)
        list(APPEND PROTO_GOS ${PROTO_GO})
    endif()

    add_custom_command(
        OUTPUT ${PROTO_OUTPUTS}
        COMMAND protobuf::protoc
        ARGS ${PROTO_ARGS} -I${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating protobuf outputs from ${PROTO_NAME}.proto"
    )
endforeach()

if(BUILD_PROTO_PYTHON)
    add_custom_target(proto_python_generated ALL DEPENDS ${PROTO_PYS} ${PROTO_PYIS})
endif()

if(BUILD_PROTO_GO)
    add_custom_target(proto_go_generated ALL DEPENDS ${PROTO_GOS})
endif()

add_library(proto_generated STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_link_libraries(proto_generated PUBLIC protobuf::libprotobuf)
target_include_directories(proto_generated PUBLIC ${PROTO_CPP_DIR})
