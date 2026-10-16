# ========================
#
# Created By : miruku_sora2026
#
# File name : debian_notices_tests.cmake
#
# Description :
#     가상 Debian 파일 트리에서 경로 별칭·다중 아키텍처·원본 고지 수집과 누락 실패를 검사한다.
#     Check Debian path aliases, multiarch owners, verbatim notices, and missing-notice failures in a mock filesystem.
#
# ========================

cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "TEST_DIR is required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(REAL_PATH "${TEST_DIR}" TEST_DIR)
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(helper "${source_root}/tools/packaging/debian-notices.cmake")
include("${helper}")
set(system "${TEST_DIR}/system")
set(stage "${TEST_DIR}/stage")
set(manifest "${stage}/share/aevix/licenses/dependency-notices.tsv")
file(MAKE_DIRECTORY "${system}/usr/lib/x86_64-linux-gnu" "${system}/usr/share/doc/libdemo1"
    "${system}/usr/share/common-licenses" "${stage}/share/aevix/licenses")
file(CREATE_LINK usr/lib "${system}/lib" SYMBOLIC RESULT link_error)
if(NOT link_error STREQUAL "0")
    message(FATAL_ERROR "Cannot create merged-/usr fixture: ${link_error}")
endif()
file(WRITE "${system}/usr/lib/x86_64-linux-gnu/libdemo.so.1.2" "fixture-library")
file(CREATE_LINK libdemo.so.1.2 "${system}/usr/lib/x86_64-linux-gnu/libdemo.so.1" SYMBOLIC)
file(CREATE_LINK libdemo1 "${system}/usr/share/doc/libdemo-dev" SYMBOLIC)
set(copyright "${system}/usr/share/doc/libdemo1/copyright")
file(WRITE "${copyright}" "Verbatim distro copyright.\nSee /usr/share/common-licenses/GPL-3.\n")
file(WRITE "${system}/usr/share/common-licenses/GPL-3-full" "Verbatim common license.\n")
file(CREATE_LINK GPL-3-full "${system}/usr/share/common-licenses/GPL-3" SYMBOLIC)
set(query "${TEST_DIR}/dpkg-query")
file(WRITE "${query}" [=[#!/bin/sh
set -eu
[ "$1" = -S ] || exit 2
case "${AEVIX_NOTICE_TEST_MODE:-normal}" in
    unowned) exit 1 ;;
    missing) printf 'libmissing:amd64: %s\n' "$2"; exit 0 ;;
esac
case "$2" in
    "$AEVIX_NOTICE_TEST_ROOT/usr/lib/x86_64-linux-gnu/libdemo.so.1.2") printf 'libdemo1:amd64: %s\n' "$2" ;;
    "$AEVIX_NOTICE_TEST_ROOT/usr/lib/x86_64-linux-gnu/libdemo.so.1") printf 'libdemo-dev:amd64: %s\n' "$2" ;;
    *) exit 1 ;;
esac
]=])
file(CHMOD "${query}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(ENV{AEVIX_NOTICE_TEST_ROOT} "${system}")
set(ENV{AEVIX_NOTICE_TEST_MODE} normal)
set(dependency "${system}/lib/x86_64-linux-gnu/libdemo.so.1")
file(WRITE "${manifest}" "dependency\tresolved_dependency\towner\tqueried_path\tnotice_source\tbundled_notices\tcommon_licenses\tstatus\n")
aevix_collect_debian_notices(DEPENDENCY "${dependency}" STAGE "${stage}" MANIFEST "${manifest}"
    DPKG_QUERY "${query}" SYSTEM_ROOT "${system}")
file(READ "${manifest}" mapping)
file(STRINGS "${manifest}" rows)
list(LENGTH rows row_count)
if(NOT row_count EQUAL 3 OR NOT mapping MATCHES "libdemo1:amd64" OR NOT mapping MATCHES "libdemo-dev:amd64"
    OR NOT mapping MATCHES "share/aevix/licenses/common-licenses/GPL-3")
    message(FATAL_ERROR "Dependency mapping lost canonical/alias owners or common licenses: ${mapping}")
endif()
file(SHA256 "${copyright}" expected_copyright)
foreach(package libdemo1 libdemo-dev)
    set(copied "${stage}/share/aevix/licenses/${package}/copyright")
    file(SHA256 "${copied}" copied_copyright)
    if(NOT copied_copyright STREQUAL expected_copyright OR IS_SYMLINK "${copied}")
        message(FATAL_ERROR "Distro copyright was changed or kept as a symlink for ${package}")
    endif()
endforeach()
file(SHA256 "${system}/usr/share/common-licenses/GPL-3-full" expected_common)
set(copied_common "${stage}/share/aevix/licenses/common-licenses/GPL-3")
file(SHA256 "${copied_common}" actual_common)
if(NOT expected_common STREQUAL actual_common OR IS_SYMLINK "${copied_common}")
    message(FATAL_ERROR "Common-license symlink contents were not copied verbatim")
endif()

set(child "${TEST_DIR}/collect.cmake")
file(WRITE "${child}" [=[
include("${HELPER}")
aevix_collect_debian_notices(DEPENDENCY "${DEPENDENCY}" STAGE "${STAGE}" MANIFEST "${MANIFEST}"
    DPKG_QUERY "${QUERY}" SYSTEM_ROOT "${SYSTEM}")
]=])
function(run_child mode expected pattern)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "AEVIX_NOTICE_TEST_MODE=${mode}" "${CMAKE_COMMAND}"
        "-DHELPER=${helper}" "-DDEPENDENCY=${dependency}" "-DSTAGE=${stage}" "-DMANIFEST=${manifest}"
        "-DQUERY=${query}" "-DSYSTEM=${system}" -P "${child}"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if((expected STREQUAL "success" AND NOT code EQUAL 0)
        OR (expected STREQUAL "failure" AND code EQUAL 0)
        OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${mode}: expected ${expected}/${pattern}, got ${code}: ${output}${error}")
    endif()
endfunction()
run_child(missing failure "Debian copyright notice is missing")
run_child(unowned success "No dpkg owner for bundled dependency")
file(READ "${manifest}" mapping)
if(NOT mapping MATCHES "unowned-no-notice")
    message(FATAL_ERROR "Unowned dependencies need an explicit manifest status")
endif()
file(WRITE "${copyright}" "Verbatim notice referencing /usr/share/common-licenses/MissingLicense.\n")
run_child(normal failure "unavailable common")
message(STATUS "Debian notice checks passed: merged-/usr, SONAME aliases, multiarch owners, verbatim symlinks/common licenses, and missing-notice diagnostics")
