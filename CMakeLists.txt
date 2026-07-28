cmake_minimum_required(VERSION 3.24)
project(TradeBoxNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  dear_imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(dear_imgui)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.12
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  sqlite_amalgamation
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
)
FetchContent_MakeAvailable(sqlite_amalgamation)
add_library(sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

add_library(imgui STATIC
  ${dear_imgui_SOURCE_DIR}/imgui.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_draw.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_tables.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${dear_imgui_SOURCE_DIR}
  ${dear_imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static opengl32)

function(tradebox_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
  endif()
endfunction()

function(tradebox_enable_windows_api target)
  target_compile_definitions(${target} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
  )
endfunction()

add_library(tradebox_core STATIC
  src/core/decimal.cpp
  src/core/market_data_store.cpp
  src/core/order_request.cpp
  src/core/system_clock.cpp
  src/core/trading_core.cpp
)
target_include_directories(tradebox_core PUBLIC include)
target_compile_features(tradebox_core PUBLIC cxx_std_23)
tradebox_enable_strict_warnings(tradebox_core)

add_library(tradebox_persistence STATIC
  src/persistence/database.cpp
  src/persistence/database_event_journal.cpp
)
target_include_directories(tradebox_persistence PUBLIC include)
target_compile_features(tradebox_persistence PUBLIC cxx_std_23)
target_link_libraries(tradebox_persistence
  PUBLIC tradebox_core sqlite3
  PRIVATE shell32 nlohmann_json::nlohmann_json
)
tradebox_enable_windows_api(tradebox_persistence)

add_library(tradebox_platform STATIC
  src/platform/credentials.cpp
)
target_include_directories(tradebox_platform PUBLIC include)
target_compile_features(tradebox_platform PUBLIC cxx_std_23)
target_link_libraries(tradebox_platform PRIVATE crypt32 advapi32)
tradebox_enable_windows_api(tradebox_platform)

add_library(tradebox_broker_alpaca STATIC
  src/broker/alpaca/alpaca_order_codec.cpp
  src/broker/alpaca/alpaca_service.cpp
)
target_include_directories(tradebox_broker_alpaca PUBLIC include)
target_compile_features(tradebox_broker_alpaca PUBLIC cxx_std_23)
target_link_libraries(tradebox_broker_alpaca
  PUBLIC
    tradebox_persistence
    tradebox_platform
  PRIVATE
    nlohmann_json::nlohmann_json
    winhttp
)
tradebox_enable_windows_api(tradebox_broker_alpaca)

add_library(tradebox_application STATIC
  src/application/order_execution_service.cpp
  src/application/trading_application.cpp
)
target_include_directories(tradebox_application PUBLIC include)
target_compile_features(tradebox_application PUBLIC cxx_std_23)
target_link_libraries(tradebox_application
  PUBLIC tradebox_core
  PRIVATE
    tradebox_broker_alpaca
    tradebox_persistence
    tradebox_platform
)
tradebox_enable_strict_warnings(tradebox_application)

add_executable(tradebox_gui WIN32
  src/adapters/gui/main.cpp
)
set_target_properties(tradebox_gui PROPERTIES OUTPUT_NAME TradeBoxNative)
target_compile_features(tradebox_gui PRIVATE cxx_std_23)
target_compile_definitions(tradebox_gui PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)
target_link_libraries(tradebox_gui PRIVATE
  tradebox_application
  tradebox_persistence
  tradebox_platform
  imgui
  nlohmann_json::nlohmann_json
  SDL3::SDL3-static
  opengl32
  psapi
)

include(CTest)
if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)

  add_executable(tradebox_core_tests
    tests/unit/decimal_tests.cpp
    tests/unit/market_data_store_tests.cpp
    tests/unit/order_request_tests.cpp
    tests/unit/trading_core_tests.cpp
  )
  target_compile_features(tradebox_core_tests PRIVATE cxx_std_23)
  target_link_libraries(tradebox_core_tests PRIVATE
    tradebox_core
    GTest::gtest_main
  )
  tradebox_enable_strict_warnings(tradebox_core_tests)

  include(GoogleTest)
  gtest_discover_tests(tradebox_core_tests)

  add_executable(tradebox_order_pipeline_tests
    tests/unit/alpaca_order_codec_tests.cpp
    tests/unit/database_order_command_journal_tests.cpp
    tests/unit/order_execution_service_tests.cpp
  )
  target_compile_features(tradebox_order_pipeline_tests PRIVATE cxx_std_23)
  target_link_libraries(tradebox_order_pipeline_tests PRIVATE
    tradebox_application
    tradebox_broker_alpaca
    tradebox_persistence
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  tradebox_enable_strict_warnings(tradebox_order_pipeline_tests)
  gtest_discover_tests(tradebox_order_pipeline_tests)

  add_test(
    NAME Architecture.CoreHasNoAdapterDependencies
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/core_dependency_check.cmake
  )
  add_test(
    NAME Architecture.GuiUsesApplicationBoundary
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/gui_application_boundary_check.cmake
  )
endif()
