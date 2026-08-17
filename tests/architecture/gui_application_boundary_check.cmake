if(NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(GUI_FILE "${SOURCE_ROOT}/src/adapters/gui/main.cpp")
file(READ "${GUI_FILE}" CONTENTS)

set(BANNED_PATTERNS
  "tradebox/broker/alpaca_service\\.h"
  "tradebox/core/trading_core\\.h"
  "AlpacaService"
  "\\.core\\.Submit"
  "\\.alpaca\\."
  "MarketDataChanges\\("
  "ChangedMarketInstruments\\("
  "ApplyTrade\\("
  "UpsertDailyBar\\("
  "BuildOrderRequest\\("
)

foreach(BANNED_PATTERN IN LISTS BANNED_PATTERNS)
  string(REGEX MATCH "${BANNED_PATTERN}" BANNED_MATCH "${CONTENTS}")
  if(BANNED_MATCH)
    message(FATAL_ERROR
      "GUI bypasses the application boundary: ${BANNED_MATCH}"
    )
  endif()
endforeach()

string(REGEX MATCH "SnapshotForUi" SNAPSHOT_MATCH "${CONTENTS}")
if(NOT SNAPSHOT_MATCH)
  message(FATAL_ERROR
    "GUI does not consume the application-owned UI snapshot"
  )
endif()

set(CHART_GUI_FILES
  "${SOURCE_ROOT}/src/adapters/gui/chart_window.cpp"
  "${SOURCE_ROOT}/src/adapters/gui/chart_window.h"
  "${SOURCE_ROOT}/src/adapters/gui/chart_geometry.cpp"
  "${SOURCE_ROOT}/src/adapters/gui/chart_geometry.h"
)
foreach(CHART_GUI_FILE IN LISTS CHART_GUI_FILES)
  file(READ "${CHART_GUI_FILE}" CHART_GUI_CONTENTS)
  set(CHART_BANNED_PATTERNS
    "tradebox/broker/"
    "tradebox/persistence/"
    "AlpacaService"
    "BarStore"
    "MarketDataStore"
    "Database"
  )
  foreach(CHART_BANNED_PATTERN IN LISTS CHART_BANNED_PATTERNS)
    string(REGEX MATCH "${CHART_BANNED_PATTERN}" CHART_BANNED_MATCH
           "${CHART_GUI_CONTENTS}")
    if(CHART_BANNED_MATCH)
      message(FATAL_ERROR
        "Chart GUI bypasses the application boundary in ${CHART_GUI_FILE}: ${CHART_BANNED_MATCH}"
      )
    endif()
  endforeach()
endforeach()

message(STATUS "GUI uses the application-service boundary")
