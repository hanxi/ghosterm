# Runtime SDK consists of fixed host-owned portable modules, never package replacements.
# Generated artifact/input hashes are checked offline on every configuration.
get_filename_component(_gt_runtime_sdk_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_gt_runtime_sdk_manifest "${_gt_runtime_sdk_root}/core/src/extension/sdk-inputs.json")
file(READ "${_gt_runtime_sdk_manifest}" _gt_runtime_sdk_json)
string(JSON _gt_runtime_sdk_format GET "${_gt_runtime_sdk_json}" format)
if(NOT _gt_runtime_sdk_format EQUAL 1)
  message(FATAL_ERROR "Unsupported generated runtime SDK manifest")
endif()
foreach(_gt_group inputs outputs)
  string(JSON _gt_count LENGTH "${_gt_runtime_sdk_json}" "${_gt_group}")
  if(_gt_group STREQUAL "inputs")
    set(_gt_required
      packages/plugin-sdk/portable/codec.ts
      packages/plugin-sdk/portable/index.ts
      packages/plugin-sdk/portable/resources.ts
      packages/plugin-sdk/portable/session.ts
      packages/plugin-sdk/portable/wire-contract.ts
      packages/plugin-sdk/tsconfig.json
      packages/plugin-sdk/sdk-version.json
      scripts/generate-plugin-runtime-sdk.mjs
      scripts/fixtures/plugin-sdk-tools/package.json
      scripts/fixtures/plugin-sdk-tools/package-lock.json)
  else()
    set(_gt_required core/src/extension/sdk-code.inc core/src/extension/sdk-identity.h)
  endif()
  list(LENGTH _gt_required _gt_required_count)
  if(NOT _gt_count EQUAL _gt_required_count)
    message(FATAL_ERROR "Generated runtime SDK ${_gt_group} set differs")
  endif()
  foreach(_gt_name IN LISTS _gt_required)
    string(JSON _gt_required_digest GET "${_gt_runtime_sdk_json}" "${_gt_group}" "${_gt_name}")
  endforeach()
  math(EXPR _gt_last "${_gt_count}-1")
  foreach(_gt_index RANGE 0 ${_gt_last})
    string(JSON _gt_file MEMBER "${_gt_runtime_sdk_json}" "${_gt_group}" ${_gt_index})
    string(JSON _gt_expected GET "${_gt_runtime_sdk_json}" "${_gt_group}" "${_gt_file}")
    if(NOT EXISTS "${_gt_runtime_sdk_root}/${_gt_file}")
      message(FATAL_ERROR "Generated runtime SDK input missing: ${_gt_file}")
    endif()
    file(SHA256 "${_gt_runtime_sdk_root}/${_gt_file}" _gt_actual)
    if(NOT _gt_actual STREQUAL _gt_expected)
      message(FATAL_ERROR "Generated runtime SDK drift: ${_gt_file}; regenerate with locked developer tools")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gt_runtime_sdk_root}/${_gt_file}")
  endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gt_runtime_sdk_manifest}")
