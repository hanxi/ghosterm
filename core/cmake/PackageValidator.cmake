# Metadata validator uses the explicitly enabled pinned engine, never Node.
# Generated artifact/input hashes are checked offline on every configuration.
get_filename_component(_gt_package_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_gt_package_manifest "${_gt_package_root}/core/src/extension/package-validator-inputs.json")
file(READ "${_gt_package_manifest}" _gt_package_json)
string(JSON _gt_package_format GET "${_gt_package_json}" format)
if(NOT _gt_package_format EQUAL 1)
  message(FATAL_ERROR "Unsupported generated package validator manifest")
endif()
foreach(_gt_group inputs outputs)
  string(JSON _gt_count LENGTH "${_gt_package_json}" "${_gt_group}")
  if(_gt_group STREQUAL "inputs")
    set(_gt_required
      packages/plugin-sdk/schema/bridge-v1.candidate.json
      packages/plugin-sdk/schema/manifest-v1.candidate.json
      packages/plugin-sdk/schema/integrity-v1.candidate.json
      packages/plugin-sdk/testkit/package-metadata.mjs
      scripts/generate-plugin-package-validator.mjs
      scripts/fixtures/plugin-package-tools/package.json
      scripts/fixtures/plugin-package-tools/package-lock.json)
  else()
    set(_gt_required core/src/extension/package-validator-code.inc core/src/extension/package-validator-LICENSES.txt)
  endif()
  list(LENGTH _gt_required _gt_required_count)
  if(NOT _gt_count EQUAL _gt_required_count)
    message(FATAL_ERROR "Generated package validator ${_gt_group} set differs")
  endif()
  foreach(_gt_name IN LISTS _gt_required)
    string(JSON _gt_required_digest GET "${_gt_package_json}" "${_gt_group}" "${_gt_name}")
  endforeach()
  math(EXPR _gt_last "${_gt_count}-1")
  foreach(_gt_index RANGE 0 ${_gt_last})
    string(JSON _gt_file MEMBER "${_gt_package_json}" "${_gt_group}" ${_gt_index})
    string(JSON _gt_expected GET "${_gt_package_json}" "${_gt_group}" "${_gt_file}")
    if(NOT EXISTS "${_gt_package_root}/${_gt_file}")
      message(FATAL_ERROR "Generated package validator input missing: ${_gt_file}")
    endif()
    file(SHA256 "${_gt_package_root}/${_gt_file}" _gt_actual)
    if(NOT _gt_actual STREQUAL _gt_expected)
      message(FATAL_ERROR "Generated package validator drift: ${_gt_file}; regenerate with locked developer tools")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gt_package_root}/${_gt_file}")
  endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gt_package_manifest}")
