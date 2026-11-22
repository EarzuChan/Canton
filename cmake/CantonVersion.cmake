file(READ "${CMAKE_CURRENT_LIST_DIR}/../canton.version.json" _canton_version_json)
string(JSON CANTON_VERSION GET "${_canton_version_json}" frameworkVersion)
string(JSON CANTON_PROTOCOL_VERSION GET "${_canton_version_json}" frameworkInternalProtocolCode)

if(NOT CANTON_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Canton version must be <major>.<minor>.<patch>, got '${CANTON_VERSION}'")
endif()

string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" CANTON_NUMERIC_VERSION "${CANTON_VERSION}")
if(NOT CANTON_NUMERIC_VERSION)
  message(FATAL_ERROR "Canton version '${CANTON_VERSION}' has no numeric SemVer core")
endif()
