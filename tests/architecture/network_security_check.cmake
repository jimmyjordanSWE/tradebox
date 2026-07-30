file(READ
  "${SOURCE_ROOT}/src/broker/alpaca/alpaca_rest_transport.cpp"
  REST_SOURCE
)
file(READ
  "${SOURCE_ROOT}/src/broker/alpaca/alpaca_service.cpp"
  SERVICE_SOURCE
)

foreach(REQUIRED_TOKEN
    "WINHTTP_OPTION_REDIRECT_POLICY_NEVER"
    "WINHTTP_ENABLE_SSL_REVOCATION"
    "kMaximumRestResponseBytes")
  string(FIND "${REST_SOURCE}" "${REQUIRED_TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR
      "REST transport security policy lost ${REQUIRED_TOKEN}")
  endif()
endforeach()

foreach(REQUIRED_TOKEN
    "WINHTTP_OPTION_REDIRECT_POLICY_NEVER"
    "WINHTTP_ENABLE_SSL_REVOCATION"
    "kMaximumMarketStreamMessageBytes"
    "kMaximumAccountStreamMessageBytes")
  string(FIND "${SERVICE_SOURCE}" "${REQUIRED_TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR
      "WebSocket security policy lost ${REQUIRED_TOKEN}")
  endif()
endforeach()

string(REGEX MATCHALL
  "ApplySecureRequestPolicy\\("
  SERVICE_POLICY_CALLS
  "${SERVICE_SOURCE}")
list(LENGTH SERVICE_POLICY_CALLS SERVICE_POLICY_CALL_COUNT)
if(SERVICE_POLICY_CALL_COUNT LESS 3)
  message(FATAL_ERROR
    "Both Alpaca WebSocket upgrades must apply the secure request policy")
endif()

string(REGEX MATCHALL
  "AppendInboundPayload\\("
  SERVICE_LIMIT_CALLS
  "${SERVICE_SOURCE}")
list(LENGTH SERVICE_LIMIT_CALLS SERVICE_LIMIT_CALL_COUNT)
if(SERVICE_LIMIT_CALL_COUNT LESS 2)
  message(FATAL_ERROR
    "Both Alpaca WebSocket streams must enforce inbound payload limits")
endif()
