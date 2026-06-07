cmake_minimum_required(VERSION 3.5)

project(ezremote-server)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ezremote_versions.cmake")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -Wno-keyword-macro -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fpermissive")

set(APP_VERSION "${EZREMOTE_SERVER_VERSION}")

include_directories(
  source
  source/ps5_api
  source/wrapper
  source/pugixml
  source/ui_dummy
)

add_executable(${PROJECT_NAME}.elf
  source/http/httplib.cpp
  source/server/http_server.cpp
  source/clients/baseclient.cpp
  source/clients/ftpclient.cpp
  source/clients/iis.cpp
  source/config.cpp
  source/crypt.c
  source/dbglogger.c
  source/ps5_api/fs.cpp
  source/ps5_api/installer_api.cpp
  source/pugixml/pugixml.cpp
  source/wrapper/sfo.cpp
  source/wrapper/split_file.cpp
  source/wrapper/installer.cpp
  source/wrapper/zip_util.cpp
  source/ui_dummy/ui_dummy.cpp
  source/main.cpp
  source/server/legacy_dpi_server.cpp
  source/usecase/dpi_usecase.cpp
  source/usecase/pkg_install_usecase.cpp
  source/usecase/file_manager_usecase.cpp
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE EZREMOTE_VERSION=${APP_VERSION} CPPHTTPLIB_THREAD_POOL_COUNT=64)

target_link_libraries(${PROJECT_NAME}.elf
  zstd
  z
  psl
  httpclient
  crypto
  ssl
  curl
  json-c
  iconv
  charset
  archive
  minizip
  bz2
  lzma
  xml2
  lexbor_static
  SceNotification
  SceSystemService
  SceUserService
  SceSysCore
  kernel_sys
  SceAppInstUtil
)
