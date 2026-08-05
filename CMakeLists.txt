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
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

FetchContent_Declare(
  rapidjson
  GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
  GIT_TAG 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
  SOURCE_SUBDIR tradebox-no-cmake-project
)
FetchContent_MakeAvailable(rapidjson)
add_library(rapidjson_headers INTERFACE)
target_include_directories(rapidjson_headers SYSTEM INTERFACE
  ${rapidjson_SOURCE_DIR}/include
)

FetchContent_Declare(
  sqlite_amalgamation
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
)
FetchContent_MakeAvailable(sqlite_amalgamation)
add_library(sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_ENABLE_DBSTAT_VTAB
)

add_library(imgui STATIC
  ${dear_imgui_SOURCE_DIR}/imgui.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_demo.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_draw.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_tables.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
  ${dear_imgui_SOURCE_DIR}
  ${dear_imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static d3d11 dxgi)

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
  src/core/indicator.cpp
  src/core/asset_catalog.cpp
  src/core/bar_series.cpp
  src/core/bar_store.cpp
  src/core/market_data_store.cpp
  src/core/order_projection.cpp
  src/core/order_request.cpp
  src/core/system_clock.cpp
  src/core/trade_pressure.cpp
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
tradebox_enable_strict_warnings(tradebox_persistence)

add_library(tradebox_platform STATIC
  src/platform/credentials.cpp
)
target_include_directories(tradebox_platform PUBLIC include)
target_compile_features(tradebox_platform PUBLIC cxx_std_23)
target_link_libraries(tradebox_platform PRIVATE crypt32 advapi32)
tradebox_enable_windows_api(tradebox_platform)
tradebox_enable_strict_warnings(tradebox_platform)

add_library(tradebox_workstation STATIC
  src/workstation/chart_documents.cpp
  src/workstation/defaults.cpp
  src/workstation/profile_codec.cpp
  src/workstation/profile_lock.cpp
  src/workstation/profile_store.cpp
  src/workstation/stable_id.cpp
  src/workstation/validation.cpp
)
target_include_directories(tradebox_workstation PUBLIC include)
target_compile_features(tradebox_workstation PUBLIC cxx_std_23)
target_link_libraries(tradebox_workstation
  PUBLIC
    tradebox_core
    tomlplusplus::tomlplusplus
  PRIVATE shell32 ole32
)
tradebox_enable_windows_api(tradebox_workstation)
tradebox_enable_strict_warnings(tradebox_workstation)

add_library(tradebox_broker_alpaca STATIC
  src/broker/alpaca/alpaca_account_activity.cpp
  src/broker/alpaca/alpaca_auth.cpp
  src/broker/alpaca/alpaca_bar_request.cpp
  src/broker/alpaca/alpaca_market_stream_decoder.cpp
  src/broker/alpaca/alpaca_order_codec.cpp
  src/broker/alpaca/alpaca_page_validation.cpp
  src/broker/alpaca/alpaca_rest_transport.cpp
  src/broker/alpaca/alpaca_stream_supervision.cpp
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
    rapidjson_headers
    winhttp
)
tradebox_enable_windows_api(tradebox_broker_alpaca)
tradebox_enable_strict_warnings(tradebox_broker_alpaca)

add_library(tradebox_application STATIC
  src/application/chart_query.cpp
  src/application/history_request_tracker.cpp
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
  src/adapters/gui/account_popup.cpp
  src/adapters/gui/application_chrome.cpp
  src/adapters/gui/chart_geometry.cpp
  src/adapters/gui/chart_window.cpp
  src/adapters/gui/gui_controls.cpp
  src/adapters/gui/gui_support.cpp
  src/adapters/gui/main.cpp
  src/adapters/gui/workspace.cpp
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
  tradebox_workstation
  imgui
  SDL3::SDL3-static
  d3d11
  dxgi
)
add_custom_command(TARGET tradebox_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets
)

include(CTest)
option(
  TRADEBOX_ENABLE_ALPACA_PAPER_CONTRACT_TESTS
  "Register credentialed tests that create Alpaca Paper orders"
  OFF
)
tradebox_enable_strict_warnings(tradebox_gui)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)

  add_executable(tradebox_core_tests
    tests/unit/bar_store_tests.cpp
    tests/unit/decimal_tests.cpp
    tests/unit/indicator_tests.cpp
    tests/unit/market_data_store_tests.cpp
    tests/unit/order_request_tests.cpp
    tests/unit/order_projection_tests.cpp
    tests/unit/trading_core_tests.cpp
    tests/unit/trade_pressure_tests.cpp
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
    tests/unit/chart_application_tests.cpp
    tests/unit/alpaca_account_activity_tests.cpp
    tests/unit/alpaca_bar_request_tests.cpp
    tests/unit/alpaca_market_stream_decoder_tests.cpp
    tests/unit/alpaca_order_codec_tests.cpp
    tests/unit/alpaca_polling_policy_tests.cpp
    tests/unit/alpaca_rest_transport_tests.cpp
    tests/unit/alpaca_stream_supervision_tests.cpp
    tests/unit/credential_store_tests.cpp
    tests/unit/database_order_command_journal_tests.cpp
    tests/unit/order_execution_service_tests.cpp
  )
  target_compile_features(tradebox_order_pipeline_tests PRIVATE cxx_std_23)
  target_link_libraries(tradebox_order_pipeline_tests PRIVATE
    tradebox_application
    tradebox_broker_alpaca
    tradebox_platform
    tradebox_persistence
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  tradebox_enable_strict_warnings(tradebox_order_pipeline_tests)
  tradebox_enable_windows_api(tradebox_order_pipeline_tests)
  gtest_discover_tests(tradebox_order_pipeline_tests)

  add_executable(tradebox_workstation_tests
    tests/unit/chart_geometry_tests.cpp
    tests/unit/native_chrome_layout_tests.cpp
    tests/unit/workstation_state_tests.cpp
    tests/unit/workspace_dirty_tests.cpp
    src/adapters/gui/workspace.cpp
  )
  target_compile_features(tradebox_workstation_tests PRIVATE cxx_std_23)
  target_include_directories(tradebox_workstation_tests PRIVATE
    src/adapters/gui
  )
  target_sources(tradebox_workstation_tests PRIVATE
    src/adapters/gui/chart_geometry.cpp
  )
  target_link_libraries(tradebox_workstation_tests PRIVATE
    tradebox_workstation
    imgui
    GTest::gtest_main
  )
  tradebox_enable_windows_api(tradebox_workstation_tests)
  tradebox_enable_strict_warnings(tradebox_workstation_tests)
  gtest_discover_tests(tradebox_workstation_tests)

  set(TRADEBOX_REPLAY_CORPUS
    ${CMAKE_CURRENT_BINARY_DIR}/testdata/alpaca_market_stream_10x10000.jsonl
  )
  set(TRADEBOX_SIP_SOAK_CORPUS
    ${CMAKE_CURRENT_BINARY_DIR}/testdata/alpaca_sip_stream_20x25000.jsonl
  )
  add_custom_command(
    OUTPUT
      ${TRADEBOX_REPLAY_CORPUS}
      ${TRADEBOX_REPLAY_CORPUS}.assets.json
    COMMAND ${CMAKE_COMMAND} -E make_directory
      ${CMAKE_CURRENT_BINARY_DIR}/testdata
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_synthetic_market_stream.py
      --output ${TRADEBOX_REPLAY_CORPUS}
      --instruments 10
      --events-per-instrument 10000
      --frame-size 100
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_synthetic_market_stream.py
    VERBATIM
  )
  add_custom_target(tradebox_replay_corpus
    DEPENDS
      ${TRADEBOX_REPLAY_CORPUS}
      ${TRADEBOX_REPLAY_CORPUS}.assets.json
  )
  add_custom_command(
    OUTPUT
      ${TRADEBOX_SIP_SOAK_CORPUS}
      ${TRADEBOX_SIP_SOAK_CORPUS}.assets.json
    COMMAND ${CMAKE_COMMAND} -E make_directory
      ${CMAKE_CURRENT_BINARY_DIR}/testdata
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_synthetic_market_stream.py
      --output ${TRADEBOX_SIP_SOAK_CORPUS}
      --instruments 20
      --events-per-instrument 25000
      --frame-size 100
      --sip-shaped
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_synthetic_market_stream.py
    VERBATIM
  )
  add_custom_target(tradebox_sip_soak_corpus
    DEPENDS
      ${TRADEBOX_SIP_SOAK_CORPUS}
      ${TRADEBOX_SIP_SOAK_CORPUS}.assets.json
  )
  add_executable(tradebox_market_replay_benchmark
    tests/performance/market_stream_replay_benchmark.cpp
  )
  add_dependencies(
    tradebox_market_replay_benchmark
      tradebox_replay_corpus
      tradebox_sip_soak_corpus
  )
  target_compile_features(
    tradebox_market_replay_benchmark PRIVATE cxx_std_23
  )
  target_compile_definitions(
    tradebox_market_replay_benchmark PRIVATE
      TRADEBOX_REPLAY_CORPUS="${TRADEBOX_REPLAY_CORPUS}"
      TRADEBOX_REPLAY_EVENT_COUNT=100000
  )
  target_link_libraries(
    tradebox_market_replay_benchmark PRIVATE
      tradebox_broker_alpaca
      tradebox_persistence
      tradebox_core
  )
  tradebox_enable_strict_warnings(
    tradebox_market_replay_benchmark
  )
  add_test(
    NAME Performance.MarketStreamReplay
    COMMAND tradebox_market_replay_benchmark
      ${TRADEBOX_REPLAY_CORPUS}
      100000
      ${CMAKE_CURRENT_BINARY_DIR}/benchmark-results/market_stream_replay_latest.json
  )
  set_tests_properties(
    Performance.MarketStreamReplay PROPERTIES
      LABELS performance
      TIMEOUT 60
  )
  add_test(
    NAME Soak.SipShapedMarketStream
    COMMAND tradebox_market_replay_benchmark
      ${TRADEBOX_SIP_SOAK_CORPUS}
      500000
      ${CMAKE_CURRENT_BINARY_DIR}/benchmark-results/sip_soak_latest.json
      sip
      20
  )
  set_tests_properties(
    Soak.SipShapedMarketStream PROPERTIES
      LABELS "performance;soak"
      TIMEOUT 120
  )

  add_executable(tradebox_alpaca_paper_contract
    tests/contract/alpaca_paper_contract.cpp
  )
  target_compile_features(
    tradebox_alpaca_paper_contract PRIVATE cxx_std_23
  )
  target_link_libraries(
    tradebox_alpaca_paper_contract PRIVATE
      tradebox_application
      tradebox_persistence
      tradebox_platform
  )
  tradebox_enable_strict_warnings(
    tradebox_alpaca_paper_contract
  )
  tradebox_enable_windows_api(
    tradebox_alpaca_paper_contract
  )
  if(TRADEBOX_ENABLE_ALPACA_PAPER_CONTRACT_TESTS)
    add_test(
      NAME Contract.AlpacaPaperOrderLifecycle
      COMMAND tradebox_alpaca_paper_contract
    )
    set_tests_properties(
      Contract.AlpacaPaperOrderLifecycle PROPERTIES
        LABELS "contract;paper;credentialed"
        TIMEOUT 300
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
  endif()

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
  add_test(
    NAME Architecture.NetworkSecurityPolicy
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/network_security_check.cmake
  )
endif()
