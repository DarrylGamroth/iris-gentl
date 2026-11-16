option(IRIS_DOWNLOAD_EMVA "Download missing GenICam dependencies from EMVA" ON)
set(IRIS_GENAPI_VERSION "3.5.1" CACHE STRING "Version of the official EMVA GenApi download")
set_property(CACHE IRIS_GENAPI_VERSION PROPERTY STRINGS "3.5.1" "3.5.0")

find_path(GENTL_INCLUDE_DIR GenTL.h)
find_path(GENAPI_INCLUDE_DIR GenApi/GenApi.h
    HINTS "${GENICAM_ROOT}" ENV GENICAM_ROOT PATH_SUFFIXES library/CPP/include include)
find_library(GENAPI_LIBRARY NAMES GenApi_gcc8_v3_5 GenApi_gcc54_v3_5 GenApi_gcc48_v3_5
    HINTS "${GENICAM_ROOT}" ENV GENICAM_ROOT PATH_SUFFIXES bin/Linux64_x64 lib)
find_library(GCBASE_LIBRARY NAMES GCBase_gcc8_v3_5 GCBase_gcc54_v3_5 GCBase_gcc48_v3_5
    HINTS "${GENICAM_ROOT}" ENV GENICAM_ROOT PATH_SUFFIXES bin/Linux64_x64 lib)

set(emva_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/emva")

function(iris_download_emva_archive filename sha256)
    file(MAKE_DIRECTORY "${emva_dir}")
    file(DOWNLOAD "https://www.emva.org/wp-content/uploads/${filename}" "${emva_dir}/${filename}"
        EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON
        TIMEOUT 180 INACTIVITY_TIMEOUT 30 STATUS download_status)
    list(GET download_status 0 code)
    if(NOT code EQUAL 0)
        list(GET download_status 1 reason)
        message(FATAL_ERROR "Could not download ${filename} from EMVA: ${reason}")
    endif()
endfunction()

if(IRIS_GENAPI_VERSION STREQUAL "3.5.1")
    set(genapi_archive GenICam_V3_5_1_public_data.zip)
    set(genapi_sha256 9ad0b97c7904efbbf37c2f65c547502cb5c369e982b1c1e4d8cce6c6d6d70858)
    set(genapi_component_prefix Linux64_x64/GenApi-3.5.1-Linux64_x64)
    set(genapi_compiler gcc8)
elseif(IRIS_GENAPI_VERSION STREQUAL "3.5.0")
    set(genapi_archive GenICam_v3_5_0_public_data.zip)
    set(genapi_sha256 1baab50ba15437791f76616949a2b230c4246ae9b1e283e2a120c07b32a0b7f5)
    set(genapi_component_prefix GenICam_V3_5_0-Linux64_x64_gcc48)
    set(genapi_compiler gcc48)
else()
    message(FATAL_ERROR "IRIS_GENAPI_VERSION must be 3.5.1 or 3.5.0.")
endif()

if(IRIS_BUNDLE_GENAPI OR NOT IRIS_GENAPI_VERSION STREQUAL "3.5.1"
    OR NOT GENAPI_INCLUDE_DIR OR NOT GENAPI_LIBRARY OR NOT GCBASE_LIBRARY)
    if(NOT IRIS_DOWNLOAD_EMVA)
        message(FATAL_ERROR "GenApi download required. Enable IRIS_DOWNLOAD_EMVA, or use an unpacked SDK and runtime with the default version and bundling disabled.")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
        OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Automatic GenApi downloads support Linux x86-64 only. Supply the SDK and runtime with GENICAM_ROOT.")
    endif()

    set(genapi_dir "${emva_dir}/genapi-${IRIS_GENAPI_VERSION}")
    set(genapi_runtime_dir "${genapi_dir}/bin/Linux64_x64")
    set(GENAPI_INCLUDE_DIR "${genapi_dir}/library/CPP/include")
    set(GENAPI_LIBRARY "${genapi_runtime_dir}/libGenApi_${genapi_compiler}_v3_5.so")
    set(GCBASE_LIBRARY "${genapi_runtime_dir}/libGCBase_${genapi_compiler}_v3_5.so")
    if(NOT EXISTS "${genapi_dir}/.extracted" OR NOT EXISTS "${GENAPI_INCLUDE_DIR}/GenApi/GenApi.h"
        OR NOT EXISTS "${GENAPI_LIBRARY}" OR NOT EXISTS "${GCBASE_LIBRARY}")
        message(STATUS "Preparing EMVA GenApi ${IRIS_GENAPI_VERSION} SDK and runtime")
        file(REMOVE "${genapi_dir}/.extracted")
        iris_download_emva_archive("${genapi_archive}" "${genapi_sha256}")
        foreach(component SDK Runtime)
            set(archive "${genapi_component_prefix}-${component}.tar.gz")
            file(ARCHIVE_EXTRACT INPUT "${emva_dir}/${genapi_archive}"
                DESTINATION "${emva_dir}" PATTERNS "${archive}")
            file(ARCHIVE_EXTRACT INPUT "${emva_dir}/${archive}" DESTINATION "${genapi_dir}")
            file(REMOVE "${emva_dir}/${archive}")
        endforeach()
        file(WRITE "${genapi_dir}/.extracted" "")
    endif()
endif()

if(NOT GENTL_INCLUDE_DIR)
    if(NOT IRIS_DOWNLOAD_EMVA)
        message(FATAL_ERROR "GenTL.h not found. Set GENTL_INCLUDE_DIR or enable IRIS_DOWNLOAD_EMVA.")
    endif()
    set(GENTL_INCLUDE_DIR "${emva_dir}/gentl-1.6")
    if(NOT EXISTS "${GENTL_INCLUDE_DIR}/GenTL.h")
        message(STATUS "Preparing EMVA GenTL 1.6 reference header")
        set(gentl_archive GenTL_v1_6.zip)
        iris_download_emva_archive("${gentl_archive}"
            48c0867219c4d04a968a27e740d81068fae08b6ab94c9834b221530d941bb4d1)
        file(ARCHIVE_EXTRACT INPUT "${emva_dir}/${gentl_archive}" DESTINATION "${GENTL_INCLUDE_DIR}")
    endif()
endif()
