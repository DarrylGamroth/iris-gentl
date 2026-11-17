cmake_minimum_required(VERSION 3.18)
project(iris VERSION 0.2.4 DESCRIPTION "Iris GenTL" LANGUAGES C CXX)
include(GNUInstallDirs)
include(CTest)
option(IRIS_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(IRIS_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    # C test executables load instrumented C++ libraries.
    add_link_options(-fsanitize=address,undefined
        $<$<LINK_LANG_AND_ID:C,Clang>:-fsanitize-link-c++-runtime>)
    link_libraries($<$<LINK_LANG_AND_ID:C,Clang>:stdc++>)
    add_compile_definitions(IRIS_TEST_SANITIZERS)
endif()
option(IRIS_BUNDLE_GENAPI "Install the official EMVA GenApi runtime with Iris" OFF)
find_package(Threads REQUIRED)
find_package(LibArchive REQUIRED)
find_package(LibXml2 REQUIRED)
include(cmake/Emva.cmake)
add_library(iris SHARED
    src/chunk.cpp
    src/device.cpp
    src/error.cpp
    src/event_snapshot.cpp
    src/feature.cpp
    src/frame.cpp
    src/node_map.cpp
    src/node_map_events.cpp
    src/node_map_source.cpp
    src/node_map_xml.cpp
    src/parts.cpp
    src/producer.cpp
    src/stream.cpp
    src/write_stack.cpp)
target_compile_features(iris PRIVATE cxx_std_17)
target_compile_options(iris PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(iris
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc/internal)
target_include_directories(iris SYSTEM PRIVATE ${GENTL_INCLUDE_DIR} ${GENAPI_INCLUDE_DIR})
target_link_libraries(iris PRIVATE ${GENAPI_LIBRARY} ${GCBASE_LIBRARY} Threads::Threads
    LibArchive::LibArchive LibXml2::LibXml2 ${CMAKE_DL_LIBS})
target_link_options(iris PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map" "LINKER:-z,now")
set_target_properties(iris PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
set_property(TARGET iris APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
install(TARGETS iris LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES inc/iris.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
option(IRIS_TRACE_INVALID_IMAGE "Print raw GenTL metadata for rejected images" OFF)
if(IRIS_TRACE_INVALID_IMAGE)
    target_compile_definitions(iris PRIVATE IRIS_TRACE_INVALID_IMAGE)
endif()
option(IRIS_COVERAGE "Instrument the consumer and CLI for gcov" OFF)
if(IRIS_COVERAGE)
    target_compile_options(iris PRIVATE --coverage -O0 -g)
    target_link_options(iris PRIVATE --coverage)
endif()
add_executable(iris-capture tests/capture.c tests/allocations.c)
target_link_libraries(iris-capture PRIVATE iris Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(iris-capture PRIVATE -Wall -Wextra -Werror)
install(TARGETS iris-capture RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
add_executable(iris-gentl tools/gentl.cpp tools/gentl_features.cpp)
target_compile_features(iris-gentl PRIVATE cxx_std_17)
target_compile_options(iris-gentl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(iris-gentl PRIVATE iris Threads::Threads)
if(IRIS_COVERAGE)
    target_compile_options(iris-gentl PRIVATE --coverage -O0 -g)
    target_link_options(iris-gentl PRIVATE --coverage)
endif()
install(TARGETS iris-gentl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(IRIS_BUNDLE_GENAPI)
    include(cmake/BinaryPackage.cmake)
endif()
add_subdirectory(examples)
if(BUILD_TESTING)
    set(IRIS_triggered_CTI "" CACHE FILEPATH "triggered mock producer")
    set(IRIS_triggered_CLPROTOCOL "" CACHE FILEPATH "triggered mock CLProtocol")
    set(IRIS_continuous_CTI "" CACHE FILEPATH "continuous simulator producer")
    set(IRIS_continuous_RUNTIME "" CACHE PATH "continuous SDK3 runtime directory")
    if(IRIS_continuous_CTI)
        get_filename_component(continuous_dir "${IRIS_continuous_CTI}" DIRECTORY)
        get_filename_component(continuous_cti "${IRIS_continuous_CTI}" NAME)
        add_test(NAME example-configure-and-acquire
            COMMAND iris-example-configure-and-acquire "${continuous_cti}" test-serial 2000)
        set_tests_properties(example-configure-and-acquire PROPERTIES TIMEOUT 15
            RESOURCE_LOCK continuous-simulator ENVIRONMENT
            "GENICAM_GENTL64_PATH=${continuous_dir};LD_LIBRARY_PATH=${IRIS_continuous_RUNTIME}:$ENV{LD_LIBRARY_PATH}")
        add_test(NAME gentl-cli-continuous-play COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:iris-gentl>" "-DCTI=${continuous_cti}" -DSIMULATOR=continuous
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/gentl_cli.cmake")
        set_tests_properties(gentl-cli-continuous-play PROPERTIES TIMEOUT 15
            RESOURCE_LOCK continuous-simulator ENVIRONMENT
            "GENICAM_GENTL64_PATH=${continuous_dir};LD_LIBRARY_PATH=${IRIS_continuous_RUNTIME}:$ENV{LD_LIBRARY_PATH}")
    endif()
    if(IRIS_triggered_CTI AND IRIS_triggered_CLPROTOCOL)
        get_filename_component(triggered_dir "${IRIS_triggered_CTI}" DIRECTORY)
        get_filename_component(triggered_cti "${IRIS_triggered_CTI}" NAME)
        set(triggered_mock_environment "trigger_fixture_GENTL_MOCK=1;trigger_fixture_CLPROTOCOL=${IRIS_triggered_CLPROTOCOL}")
        add_test(NAME example-software-trigger COMMAND iris-example-software-trigger "${triggered_cti}")
        set_tests_properties(example-software-trigger PROPERTIES TIMEOUT 10
            ENVIRONMENT "${triggered_mock_environment};GENICAM_GENTL64_PATH=${triggered_dir};trigger_fixture_ZERO_COPY=0")
        add_test(NAME gentl-cli-software-trigger COMMAND iris-gentl acquire 2 --cti "${triggered_cti}"
            --stream-only --trigger TriggerSoftware --set TriggerSelector=FrameStart
            --set TriggerMode=Off --set TriggerSource=Software --set TriggerMode=On)
        set_tests_properties(gentl-cli-software-trigger PROPERTIES TIMEOUT 10
            PASS_REGULAR_EXPRESSION "frame=2 width="
            ENVIRONMENT "${triggered_mock_environment};GENICAM_GENTL64_PATH=${triggered_dir};trigger_fixture_ZERO_COPY=0")
        add_test(NAME gentl-cli-triggered-play COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:iris-gentl>" "-DCTI=${triggered_cti}" -DSIMULATOR=triggered
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/gentl_cli.cmake")
        set_tests_properties(gentl-cli-triggered-play PROPERTIES TIMEOUT 10
            ENVIRONMENT "${triggered_mock_environment};GENICAM_GENTL64_PATH=${triggered_dir};trigger_fixture_ZERO_COPY=0")
    endif()
    if(IRIS_triggered_CTI AND IRIS_triggered_CLPROTOCOL AND IRIS_continuous_CTI)
        add_test(NAME mixed-simulators COMMAND iris-capture --test "${triggered_cti}" "${continuous_cti}")
        add_test(NAME mixed-caller-buffers COMMAND iris-capture --test-caller-buffers "${triggered_cti}" "${continuous_cti}")
        add_test(NAME mixed-remote-xml-override COMMAND iris-capture --test-xml-override "${triggered_cti}" "${continuous_cti}")
        add_test(NAME mixed-triggered-native-ring COMMAND iris-capture --test-native-ring "${triggered_cti}" "${continuous_cti}")
        set_tests_properties(mixed-simulators mixed-caller-buffers mixed-remote-xml-override PROPERTIES TIMEOUT 30
            RESOURCE_LOCK continuous-simulator ENVIRONMENT
            "${triggered_mock_environment};GENICAM_GENTL64_PATH=${triggered_dir}:${continuous_dir};LD_LIBRARY_PATH=${IRIS_continuous_RUNTIME}:$ENV{LD_LIBRARY_PATH}")
        set_tests_properties(mixed-triggered-native-ring PROPERTIES TIMEOUT 30
            RESOURCE_LOCK continuous-simulator ENVIRONMENT
            "${triggered_mock_environment};GENICAM_GENTL64_PATH=${triggered_dir}:${continuous_dir};LD_LIBRARY_PATH=${IRIS_continuous_RUNTIME}:$ENV{LD_LIBRARY_PATH};trigger_fixture_ZERO_COPY=1;trigger_fixture_RING_BUFFERS=4")
    endif()
endif()

if(BUILD_TESTING)
    add_library(iris-test-cti MODULE tests/fault_cti.cpp)
    target_compile_features(iris-test-cti PRIVATE cxx_std_17)
    target_include_directories(iris-test-cti PRIVATE ${GENTL_INCLUDE_DIR})
    target_link_libraries(iris-test-cti PRIVATE Threads::Threads)
    set_target_properties(iris-test-cti PROPERTIES PREFIX "" SUFFIX ".cti")
    set(second_mock_cti "${CMAKE_CURRENT_BINARY_DIR}/iris-test-cti-second.cti")
    add_custom_command(OUTPUT "${second_mock_cti}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:iris-test-cti> "${second_mock_cti}"
        DEPENDS iris-test-cti VERBATIM)
    add_custom_target(iris-second-mock-cti DEPENDS "${second_mock_cti}")
    add_executable(iris-mixed-mock tests/mixed_mock.c tests/allocations.c)
    target_link_libraries(iris-mixed-mock PRIVATE iris ${CMAKE_DL_LIBS})
    add_dependencies(iris-mixed-mock iris-second-mock-cti)
    add_test(NAME mixed-mock-producers COMMAND iris-mixed-mock $<TARGET_FILE:iris-test-cti> "${second_mock_cti}")
    set_tests_properties(mixed-mock-producers PROPERTIES TIMEOUT 10)
    add_executable(iris-mock-buffer-pool tests/mock_buffer_pool.c tests/allocations.c)
    target_link_libraries(iris-mock-buffer-pool PRIVATE iris ${CMAKE_DL_LIBS})
    add_test(NAME mock-buffer-pool COMMAND iris-mock-buffer-pool $<TARGET_FILE:iris-test-cti>)
    add_test(NAME mock-buffer-pool-stacked COMMAND iris-mock-buffer-pool $<TARGET_FILE:iris-test-cti>)
    add_test(NAME mock-buffer-pool-stacked-fallback COMMAND iris-mock-buffer-pool $<TARGET_FILE:iris-test-cti>)
    set_tests_properties(mock-buffer-pool PROPERTIES TIMEOUT 10)
    set_tests_properties(mock-buffer-pool-stacked PROPERTIES TIMEOUT 10 ENVIRONMENT "IRIS_MOCK_STACKED_INFO=1")
    set_tests_properties(mock-buffer-pool-stacked-fallback PROPERTIES TIMEOUT 10 ENVIRONMENT "IRIS_MOCK_STACKED_INFO=transient-error")
    add_test(NAME example-features COMMAND "${CMAKE_COMMAND}"
        "-DFEATURES=$<TARGET_FILE:iris-example-features>" "-DCTI=$<TARGET_FILE:iris-test-cti>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/example_features.cmake")
    set_tests_properties(example-features PROPERTIES TIMEOUT 10)
    add_test(NAME example-mock-software-trigger COMMAND iris-example-software-trigger $<TARGET_FILE:iris-test-cti>)
    set_tests_properties(example-mock-software-trigger PROPERTIES TIMEOUT 10
        ENVIRONMENT "IRIS_FAULT=progressive-trigger")
    add_test(NAME gentl-cli COMMAND "${CMAKE_COMMAND}"
        "-DCLI=$<TARGET_FILE:iris-gentl>" "-DCTI=$<TARGET_FILE:iris-test-cti>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/gentl_cli.cmake")
    set_tests_properties(gentl-cli PROPERTIES TIMEOUT 10)
    add_executable(iris-failures tests/failures.c)
    target_link_libraries(iris-failures PRIVATE iris Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME failure-recovery COMMAND iris-failures $<TARGET_FILE:iris-test-cti>)
    add_test(NAME failure-recovery-stacked COMMAND iris-failures $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-progressive tests/progressive.c tests/allocations.c)
    target_link_libraries(iris-progressive PRIVATE iris Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME progressive COMMAND iris-progressive $<TARGET_FILE:iris-test-cti>)
    add_test(NAME progressive-stacked COMMAND iris-progressive $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-nodes tests/nodes.c)
    target_link_libraries(iris-nodes PRIVATE iris)
    add_test(NAME node-settings COMMAND iris-nodes $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-feature-browse tests/feature_browse.c)
    target_link_libraries(iris-feature-browse PRIVATE iris LibArchive::LibArchive)
    add_test(NAME feature-browse COMMAND iris-feature-browse $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-feature-batch tests/feature_batch.cpp)
    target_compile_features(iris-feature-batch PRIVATE cxx_std_17)
    target_include_directories(iris-feature-batch PRIVATE inc/internal)
    target_include_directories(iris-feature-batch SYSTEM PRIVATE ${GENTL_INCLUDE_DIR} ${GENAPI_INCLUDE_DIR})
    target_link_libraries(iris-feature-batch PRIVATE iris Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME feature-batch COMMAND iris-feature-batch $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-write-stack tests/write_stack.c)
    target_link_libraries(iris-write-stack PRIVATE iris ${CMAKE_DL_LIBS})
    add_test(NAME write-stack COMMAND iris-write-stack $<TARGET_FILE:iris-test-cti>)
    set_tests_properties(write-stack PROPERTIES TIMEOUT 10)
    add_executable(iris-events tests/events.c tests/allocations.c)
    target_link_libraries(iris-events PRIVATE iris ${CMAKE_DL_LIBS})
    add_test(NAME node-map-events COMMAND iris-events $<TARGET_FILE:iris-test-cti>)
    add_executable(iris-payloads tests/payloads.c tests/allocations.c)
    target_link_libraries(iris-payloads PRIVATE iris ${CMAKE_DL_LIBS})
    add_test(NAME payloads COMMAND iris-payloads $<TARGET_FILE:iris-test-cti>)
    add_test(NAME payloads-stacked COMMAND iris-payloads $<TARGET_FILE:iris-test-cti>)
    set_tests_properties(failure-recovery-stacked progressive-stacked payloads-stacked
        PROPERTIES TIMEOUT 10 ENVIRONMENT "IRIS_MOCK_STACKED_INFO=1")
    set_tests_properties(payloads PROPERTIES TIMEOUT 10)
    add_executable(iris-feature-values tests/feature_values.cpp src/error.cpp)
    target_compile_features(iris-feature-values PRIVATE cxx_std_17)
    target_include_directories(iris-feature-values PRIVATE inc inc/internal)
    target_include_directories(iris-feature-values SYSTEM PRIVATE ${GENAPI_INCLUDE_DIR})
    add_test(NAME feature-values COMMAND iris-feature-values)
    add_executable(iris-nonblocking tests/nonblocking.cpp)
    target_compile_features(iris-nonblocking PRIVATE cxx_std_17)
    target_include_directories(iris-nonblocking PRIVATE inc/internal)
    target_include_directories(iris-nonblocking SYSTEM PRIVATE ${GENTL_INCLUDE_DIR} ${GENAPI_INCLUDE_DIR})
    target_link_libraries(iris-nonblocking PRIVATE iris Threads::Threads)
    add_test(NAME nonblocking-contention COMMAND iris-nonblocking $<TARGET_FILE:iris-test-cti>)
    set_tests_properties(failure-recovery node-settings feature-browse feature-batch node-map-events feature-values nonblocking-contention PROPERTIES TIMEOUT 10)
    if(IRIS_COVERAGE)
        target_compile_options(iris-feature-values PRIVATE --coverage -O0 -g)
        target_link_options(iris-feature-values PRIVATE --coverage)
        find_program(GCOVR gcovr REQUIRED)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --no-tests=error
            COMMAND ${GCOVR} --root ${CMAKE_CURRENT_SOURCE_DIR}
                --filter "${CMAKE_CURRENT_SOURCE_DIR}/src/"
                --filter "${CMAKE_CURRENT_SOURCE_DIR}/inc/internal/"
                --fail-under-line 90.01 --print-summary
                ${CMAKE_BINARY_DIR}
            COMMAND ${GCOVR} --root ${CMAKE_CURRENT_SOURCE_DIR}
                --filter "${CMAKE_CURRENT_SOURCE_DIR}/tools/"
                --fail-under-line 90.01 --print-summary ${CMAKE_BINARY_DIR}
            DEPENDS iris-capture iris-mixed-mock iris-mock-buffer-pool iris-gentl iris-failures iris-progressive iris-nodes iris-feature-browse iris-feature-batch iris-events iris-feature-values
                    iris-nonblocking iris-test-cti iris-example-features iris-example-configure-and-acquire
                    iris-example-software-trigger iris-payloads iris-write-stack
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()
endif()
