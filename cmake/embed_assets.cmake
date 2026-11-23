# Inputs:
#   BLACKBOX_ASSET_DIR : directory containing index.html, app.css, app.js
#   BLACKBOX_OUTPUT    : generated C++ source path
#
# Produces a C++ translation unit with the assets as byte arrays. The build has
# no runtime filesystem dependency for the UI.

if(NOT DEFINED BLACKBOX_ASSET_DIR OR NOT DEFINED BLACKBOX_OUTPUT)
  message(FATAL_ERROR "BLACKBOX_ASSET_DIR and BLACKBOX_OUTPUT are required")
endif()

set(ASSET_NAMES index.html app.css app.js)
set(ASSET_TYPES
  "text/html\; charset=utf-8"
  "text/css\; charset=utf-8"
  "application/javascript\; charset=utf-8"
)

get_filename_component(OUTPUT_DIR "${BLACKBOX_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(CONTENT "#include <blackbox/assets.hpp>\n\nnamespace bb::assets {\nnamespace {\n")

set(INDEX 0)
foreach(NAME IN LISTS ASSET_NAMES)
  set(PATH "${BLACKBOX_ASSET_DIR}/${NAME}")
  if(NOT EXISTS "${PATH}")
    message(FATAL_ERROR "missing UI asset: ${PATH}")
  endif()
  file(READ "${PATH}" HEX_DATA HEX)
  if(HEX_DATA STREQUAL "")
    set(BYTES "0x00")
  else()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX_DATA}")
  endif()
  string(APPEND CONTENT "const unsigned char kAsset${INDEX}[] = {${BYTES}};\n")
  math(EXPR INDEX "${INDEX} + 1")
endforeach()

string(APPEND CONTENT "const Asset kAssets[] = {\n")
set(INDEX 0)
foreach(NAME IN LISTS ASSET_NAMES)
  list(GET ASSET_TYPES ${INDEX} TYPE)
  string(APPEND CONTENT
    "  {\"/${NAME}\", \"${TYPE}\", {kAsset${INDEX}, sizeof(kAsset${INDEX})}},\n")
  math(EXPR INDEX "${INDEX} + 1")
endforeach()
string(APPEND CONTENT "};\n\n}  // namespace\n\n")
string(APPEND CONTENT
  "std::span<const Asset> all() { return {kAssets, sizeof(kAssets) / sizeof(kAssets[0])}; }\n\n")
string(APPEND CONTENT
  "const Asset* find(std::string_view path) {\n")
string(APPEND CONTENT
  "  if (!path.empty() && path.front() != '/') {\n    path = std::string_view();\n  }\n")
string(APPEND CONTENT
  "  for (const Asset& asset : all()) {\n    if (asset.path == path) {\n      return &asset;\n    }\n  }\n  return nullptr;\n}\n\n")
string(APPEND CONTENT "}  // namespace bb::assets\n")

file(WRITE "${BLACKBOX_OUTPUT}" "${CONTENT}")
