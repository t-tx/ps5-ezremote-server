cmake_minimum_required(VERSION 3.5)

project(ezremote-server)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -Wno-keyword-macro -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fpermissive")

set(APP_VERSION "1.04")

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
  source/clients/archiveorg.cpp
  source/clients/baseclient.cpp
  source/clients/ftpclient.cpp
  source/clients/sftpclient.cpp
  source/clients/nfsclient.cpp
  source/clients/smbclient.cpp
  source/clients/webdav.cpp
  source/clients/apache.cpp
  source/clients/github.cpp
  source/clients/myrient.cpp
  source/clients/iis.cpp
  source/clients/nginx.cpp
  source/clients/npxserve.cpp
  source/clients/rclone.cpp
  source/filehost/1fichier.cpp
  source/filehost/alldebrid.cpp
  source/filehost/directhost.cpp
  source/filehost/filehost.cpp
  source/filehost/gdrive.cpp
  source/filehost/mediafire.cpp
  source/filehost/pixeldrain.cpp
  source/filehost/realdebrid.cpp
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
  smb2
  ssh2
  nfs
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
