add_library(schemachange
  sc_add_table.c
  sc_alter_table.c
  sc_rename_table.c
  sc_callbacks.c
  sc_csc2.c
  sc_drop_table.c
  sc_fastinit_table.c
  sc_global.c
  sc_logic.c
  sc_lua.c
  sc_queues.c
  sc_records.c
  sc_schema.c
  sc_stripes.c
  sc_struct.c
  sc_util.c
  sc_view.c
  sc_import.c
  schemachange.c
  ${PROJECT_BINARY_DIR}/protobuf/schemachange.pb-c.c
  ${PROJECT_BINARY_DIR}/protobuf/importdata.pb-c.c
)

set(module schemachange)
set(MODULE SCHEMACHANGE)
configure_file(${PROJECT_SOURCE_DIR}/mem/mem.h.in mem_schemachange.h @ONLY)

set_source_files_properties(
    ${PROJECT_BINARY_DIR}/protobuf/schemachange.pb-c.c
    ${PROJECT_BINARY_DIR}/protobuf/importdata.pb-c.c
    PROPERTIES GENERATED TRUE
)

include_directories(
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/util
  ${PROJECT_SOURCE_DIR}/bbinc
  ${PROJECT_BINARY_DIR}/bbinc
  ${PROJECT_SOURCE_DIR}/bdb
  ${PROJECT_SOURCE_DIR}/cdb2api
  ${PROJECT_SOURCE_DIR}/crc32c
  ${PROJECT_SOURCE_DIR}/csc2
  ${PROJECT_SOURCE_DIR}/datetime
  ${PROJECT_SOURCE_DIR}/db
  ${PROJECT_BINARY_DIR}/db
  ${PROJECT_SOURCE_DIR}/dfp/decNumber
  ${PROJECT_SOURCE_DIR}/dfp/dfpal
  ${PROJECT_SOURCE_DIR}/dlmalloc
  ${PROJECT_SOURCE_DIR}/mem
  ${PROJECT_BINARY_DIR}/mem
  ${PROJECT_SOURCE_DIR}/net
  ${PROJECT_SOURCE_DIR}/protobuf
  ${PROJECT_BINARY_DIR}/protobuf
  ${PROJECT_SOURCE_DIR}/sqlite/src
  ${PROJECT_BINARY_DIR}/berkdb
  ${PROJECT_BINARY_DIR}/bdb
  ${PROJECT_SOURCE_DIR}/build/berkdb
  ${PROJECT_SOURCE_DIR}/build/bdb
  ${PROJECT_SOURCE_DIR}/berkdb
  ${PROJECT_SOURCE_DIR}/lua
  ${PROTOBUF-C_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
)
add_dependencies(schemachange mem proto bdb)
target_link_libraries(schemachange PUBLIC bdb)
if (COMDB2_BBCMAKE)
  comdb2_lib_target(schemachange)
endif()
