include_directories(
  ${PROJECT_SOURCE_DIR}/bbinc
  ${PROJECT_SOURCE_DIR}/cdb2api
  ${PROJECT_SOURCE_DIR}/comdb2rle
  ${PROJECT_SOURCE_DIR}/datetime
  ${PROJECT_SOURCE_DIR}/mem
  ${PROJECT_SOURCE_DIR}/protobuf
  ${PROJECT_BINARY_DIR}/protobuf
  ${PROJECT_SOURCE_DIR}/bb
  ${PROJECT_SOURCE_DIR}/bbinc
  ${OPENSSL_INCLUDE_DIR}
)

add_executable(blob blob.c)
add_executable(bound bound.cpp)
add_executable(cdb2_client cdb2_client.c)
add_executable(cdb2api_caller cdb2api_caller.cpp)
add_executable(cdb2bind cdb2bind.c)
add_executable(comdb2_blobtest comdb2_blobtest.c)
add_executable(comdb2_sqltest client_datetime.c endian_core.c md5.c slt_comdb2.c slt_sqlite.c sqllogictest.c)
add_executable(crle crle.c)
add_executable(hatest hatest.c)
add_executable(insert_lots_mt insert_lots_mt.cpp)
add_executable(leakcheck leakcheck.c)
add_executable(localrep localrep.c strbuf.c)
add_executable(overflow_blobtest overflow_blobtest.c)
add_executable(ptrantest ptrantest.c)
add_executable(recom recom.c)
add_executable(selectv selectv.c)
add_executable(serial serial.c)
add_executable(sicountbug sicountbug.c)
add_executable(sirace sirace.c)
add_executable(simple_ssl simple_ssl.c)
add_executable(stepper stepper.c stepper_client.c)
add_executable(utf8 utf8.c)
add_executable(insert insert.c nemesis.c testutil.c)
add_executable(register register.c nemesis.c testutil.c)
add_executable(breakloop breakloop.c nemesis.c testutil.c)

set(slowconnect_src 
  slowconnect.c 
  cdb2api_patched.c 
  ${PROJECT_SOURCE_DIR}/bb/sbuf2.c
  ${PROJECT_BINARY_DIR}/protobuf/sqlquery.pb-c.c
  ${PROJECT_BINARY_DIR}/protobuf/sqlresponse.pb-c.c
)
set_source_files_properties(
  ${PROJECT_BINARY_DIR}/protobuf/sqlquery.pb-c.c
  ${PROJECT_BINARY_DIR}/protobuf/sqlresponse.pb-c.c
  PROPERTIES GENERATED TRUE
)
if(WITH_SSL)
  list(APPEND slowconnect_src ${PROJECT_SOURCE_DIR}/bb/ssl_support.c)
endif()

add_executable(slowconnect ${slowconnect_src})

foreach(executable blob bound cdb2api_caller cdb2bind comdb2_blobtest insert_lots_mt leakcheck localrep overflow_blobtest selectv serial sicountbug sirace simple_ssl utf8 insert register breakloop)
  target_link_libraries(${executable} cdb2api ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})
endforeach()

# slowconnect works with a patched cdb2api 
target_link_libraries(slowconnect ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})
add_definitions(-DSBUF2_SERVER=0)
add_custom_command(
   OUTPUT cdb2api_patched.c
   DEPENDS ${CMAKE_SOURCE_DIR}/cdb2api/cdb2api.c
   COMMAND sed 's!.*DEBUG-HOOK: slowconnect.*!poll\(NULL, 0, (rand() % 2 == 0) ? 50 : 1\)\;!' ${CMAKE_SOURCE_DIR}/cdb2api/cdb2api.c > cdb2api_patched.c
)

# cdb2api shared object
target_link_libraries(cdb2_client cdb2api_shared ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})

# needs readline!
target_link_libraries(hatest cdb2api ${READLINE_LIBRARY} ${CURSES_LIBRARIES} ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})

# sqlite3_snprintf
target_link_libraries(comdb2_sqltest cdb2api ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${SQLITE3_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})
target_link_libraries(ptrantest cdb2api ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${SQLITE3_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})
target_link_libraries(recom cdb2api ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${SQLITE3_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})

# everything!
target_link_libraries(stepper cdb2api mem dlmalloc bb ${OPENSSL_LIBRARIES} ${PROTOBUF_C_LIBRARY} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})
