cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED TRADEBOX_ARCHIVE OR TRADEBOX_ARCHIVE STREQUAL "")
  message(FATAL_ERROR "TRADEBOX_ARCHIVE is required")
endif()
if(NOT DEFINED TRADEBOX_VERIFY_DIR OR TRADEBOX_VERIFY_DIR STREQUAL "")
  message(FATAL_ERROR "TRADEBOX_VERIFY_DIR is required")
endif()
if(NOT EXISTS "${TRADEBOX_ARCHIVE}")
  message(FATAL_ERROR "Release archive does not exist: ${TRADEBOX_ARCHIVE}")
endif()

file(REMOVE_RECURSE "${TRADEBOX_VERIFY_DIR}")
file(MAKE_DIRECTORY "${TRADEBOX_VERIFY_DIR}")
file(ARCHIVE_EXTRACT INPUT "${TRADEBOX_ARCHIVE}"
     DESTINATION "${TRADEBOX_VERIFY_DIR}")

set(REQUIRED_RELEASE_FILES
  TradeBoxNative.exe
  README.md
  THIRD_PARTY_NOTICES.txt
  assets/fonts/B612-Regular.ttf
  assets/fonts/B612Mono-Regular.ttf
  assets/fonts/MaterialSymbolsRounded.ttf
  licenses/B612-OFL.txt
  licenses/B612Mono-OFL.txt
  licenses/Material-Design-Icons-Apache-2.0.txt
  licenses/Dear-ImGui-MIT.txt
  licenses/SDL3-Zlib.txt
  licenses/nlohmann-json-MIT.txt
  licenses/tomlplusplus-MIT.txt
  licenses/RapidJSON-License.txt
)
foreach(REQUIRED_FILE IN LISTS REQUIRED_RELEASE_FILES)
  if(NOT EXISTS "${TRADEBOX_VERIFY_DIR}/${REQUIRED_FILE}")
    message(FATAL_ERROR
      "Release archive is missing required file: ${REQUIRED_FILE}")
  endif()
endforeach()

file(GLOB_RECURSE RELEASE_FILES
  RELATIVE "${TRADEBOX_VERIFY_DIR}"
  "${TRADEBOX_VERIFY_DIR}/*")
foreach(RELEASE_FILE IN LISTS RELEASE_FILES)
  if(RELEASE_FILE MATCHES "\\.(pdb|db|sqlite|sqlite3|log|tbw)$")
    message(FATAL_ERROR
      "Release archive contains forbidden runtime file: ${RELEASE_FILE}")
  endif()
endforeach()

file(SIZE "${TRADEBOX_VERIFY_DIR}/TradeBoxNative.exe" EXECUTABLE_SIZE)
if(EXECUTABLE_SIZE LESS 100000)
  message(FATAL_ERROR "Packaged executable is unexpectedly small")
endif()

file(REMOVE_RECURSE "${TRADEBOX_VERIFY_DIR}")
message(STATUS "Verified release package: ${TRADEBOX_ARCHIVE}")
