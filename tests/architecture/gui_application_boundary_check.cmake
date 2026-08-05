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

message(STATUS "GUI uses the application-service boundary")
