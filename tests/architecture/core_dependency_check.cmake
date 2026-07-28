if(NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE CORE_FILES
  "${SOURCE_ROOT}/include/tradebox/core/*.h"
  "${SOURCE_ROOT}/src/core/*.cpp"
)

if(NOT CORE_FILES)
  message(FATAL_ERROR "No core files were found")
endif()

set(BANNED_INCLUDE_PATTERN
  "#[ \t]*include[ \t]*[<\"](windows\\.h|winhttp\\.h|SDL3/|imgui|nlohmann/)"
)

foreach(CORE_FILE IN LISTS CORE_FILES)
  file(READ "${CORE_FILE}" CONTENTS)
  string(REGEX MATCH "${BANNED_INCLUDE_PATTERN}" BANNED_MATCH "${CONTENTS}")
  if(BANNED_MATCH)
    message(FATAL_ERROR
      "Core dependency boundary violated in ${CORE_FILE}: ${BANNED_MATCH}"
    )
  endif()
endforeach()

message(STATUS "Core dependency boundary is clean")
