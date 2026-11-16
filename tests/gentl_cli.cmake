function(run_cli status_pattern output_pattern)
    execute_process(COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status MATCHES "${status_pattern}" OR NOT "${output}${error}" MATCHES "${output_pattern}")
        message(FATAL_ERROR "iris-gentl ${ARGN} returned ${status}\n${output}\n${error}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
endfunction()

set(complete "complete=3 incomplete=0 unsupported_payload=0 invalid_image=0 timeouts=0.*interval_samples=2")
if(SIMULATOR STREQUAL "continuous")
    run_cli("^0$" "${complete}" play 3 --cti "${CTI}"
        --serial test-serial --timeout 2000 --set AcquisitionMode=Continuous)
    return()
elseif(SIMULATOR STREQUAL "triggered")
    run_cli("^0$" "${complete}" play 3 --cti "${CTI}" --stream-only --trigger TriggerSoftware
        --set TriggerSelector=FrameStart --set TriggerMode=Off
        --set TriggerSource=Software --set TriggerMode=On)
    return()
endif()

get_filename_component(cti_name "${CTI}" NAME)
string(RANDOM LENGTH 12 suffix)
set(scan_dir "${CMAKE_CURRENT_BINARY_DIR}/iris-gentl-${suffix}")
file(MAKE_DIRECTORY "${scan_dir}")
file(CREATE_LINK "${CTI}" "${scan_dir}/${cti_name}" SYMBOLIC)
set(ENV{GENICAM_GENTL64_PATH} "${scan_dir}:${scan_dir}")
set(ENV{IRIS_FAULT} example)
run_cli("^0$" "Usage: iris-gentl" --help)
run_cli("^0$" "test-interface.*test-device.*test-serial" info)
run_cli("^0$" "test-interface.*test-device.*test-serial" info "--cti=${cti_name}" --serial test-serial)
run_cli("^0$" "\"serial\":\"test-serial\"" info "--cti=${cti_name}" --json)
file(REMOVE "${scan_dir}/${cti_name}")
file(REMOVE_RECURSE "${scan_dir}")
run_cli("^0$" "TriggerSelector.*enumeration" features --cti "${CTI}" "Trigger*")
run_cli("^0$" "\"type\":\"enumeration\"" features --cti "${CTI}" "Trigger*" --json)
run_cli("^0$" "Range: 100 to 1000 us" describe --cti "${CTI}" ExposureTime)
set(ENV{IRIS_FAULT} nodes)
run_cli("^1$" "multiple devices match" describe --cti "${CTI}" DeviceUserID)
run_cli("^0$" "Maximum length: 16" describe --cti "${CTI}" --interface test-interface --device test-device --set DeviceUserID=iris-demo DeviceUserID)
run_cli("^0$" "\"values\":\\[16,32,64\\]" describe --cti "${CTI}"
    --interface test-interface --device test-device WidthRaw --json)
run_cli("^0$" "Values:\n  1\n  2\n  4" describe --cti "${CTI}"
    --interface test-interface --device test-device WidthScaled)
set(ENV{IRIS_FAULT} example)
run_cli("^0$" "\"increment\":16" describe --cti "${CTI}" Width --json)
run_cli("^0$" "\"choices\".*\"selectors\":\\[\"TriggerSelector\"\\]"
    describe --cti "${CTI}" TriggerMode --json)
run_cli("^0$" "Controlled features:.*TriggerMode" describe --cti "${CTI}" TriggerSelector)
run_cli("^0$" "Width=16" get --cti "${CTI}" --interface test-interface --device test-device Width)
run_cli("^0$" "Width=48" get --cti "${CTI}" --set Width=48 Width)
run_cli("^0$" "\"name\":\"DeviceUserID\"" get --cti "${CTI}" --json
    --set [=[DeviceUserID=iris"test\one]=] DeviceUserID)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
    string(JSON value GET "${cli_output}" value)
    if(NOT value STREQUAL [=[iris"test\one]=])
        message(FATAL_ERROR "Unexpected JSON string value: ${value}")
    endif()
endif()
string(ASCII 255 invalid_utf8)
run_cli("^0$" [=["value":"é漢📷\\u0009\\u00ff"]=] get --cti "${CTI}" --json
    --set "DeviceUserID=é漢📷\t${invalid_utf8}" DeviceUserID)
run_cli("^0$" "TriggerMode=On" get --cti "${CTI}"
    --set TriggerSelector=FrameStart --set TriggerMode=On TriggerMode)
run_cli("^0$" "^$" set --cti "${CTI}" DeviceUserID=iris-demo)
run_cli("^0$" "^$" exec --cti "${CTI}" TriggerSoftware)
run_cli("^0$" "<RegisterDescription" xml --cti "${CTI}")
run_cli("^1$" "no device matches" get --cti "${CTI}" --serial missing Width)
run_cli("^2$" "expected NAME=VALUE" set --cti "${CTI}" Width)
run_cli("^2$" "invalid count" grab --cti "${CTI}" 0)

set(before "${CMAKE_CURRENT_BINARY_DIR}/iris-before-${suffix}.snapshot")
set(after "${CMAKE_CURRENT_BINARY_DIR}/iris-after-${suffix}.snapshot")
set(other "${CMAKE_CURRENT_BINARY_DIR}/iris-other-${suffix}.snapshot")
run_cli("^0$" "iris-gentl snapshot 1" snapshot --cti "${CTI}"
    --set [=[DeviceUserID=iris"test\one]=] Width DeviceUserID)
file(WRITE "${before}" "${cli_output}")
run_cli("^0$" "iris-gentl snapshot 1" snapshot --cti "${CTI}"
    --set [=[DeviceUserID=iris"test\one]=] --set Width=48 DeviceUserID Width)
file(WRITE "${after}" "${cli_output}")
run_cli("^0$" "^$" diff "${before}" "${before}")
run_cli("^1$" "changed remote/Width before=\"16\" after=\"48\"" diff "${before}" "${after}")
run_cli("^1$" "\"change\":\"changed\",\"before\":\"16\",\"after\":\"48\"" diff "${before}" "${after}" --json)
file(WRITE "${other}" "bad header\n")
run_cli("^2$" "invalid snapshot header" diff "${before}" "${other}")
file(WRITE "${other}" "iris-gentl snapshot 1\n\"remote\"\t\"Width\"\t1\t")
run_cli("^2$" "invalid quoted field" diff "${before}" "${other}")

set(ENV{IRIS_FAULT} progressive-controls)
run_cli("^0$" "\"TriggerSelector\".*\"FrameStart\"" snapshot --cti "${CTI}"
    --set TriggerSelector=FrameStart TriggerMode)
file(WRITE "${before}" "${cli_output}")
run_cli("^0$" "\"TriggerSelector\".*\"ExposureStart\"" snapshot --cti "${CTI}"
    --set TriggerSelector=ExposureStart TriggerMode)
file(WRITE "${after}" "${cli_output}")
run_cli("^1$" "added remote/TriggerMode TriggerSelector=\"ExposureStart\".*removed remote/TriggerMode TriggerSelector=\"FrameStart\""
    diff "${before}" "${after}")
run_cli("^0$" "\"selectors\":\\{\"TriggerSelector\":\"FrameStart\"\\}" snapshot --cti "${CTI}" TriggerMode --json)
file(REMOVE "${before}" "${after}" "${other}")

set(ENV{IRIS_FAULT} events)
run_cli("^0$" "event=1234 bytes=" event --cti "${CTI}" --timeout 100)
run_cli("^0$" [=["event":"1234".*"name":"EventExposureEndTimestamp","type":"integer","status":-14]=]
    event --cti "${CTI}" --json)
run_cli("^0$" "EventExposureEndTimestamp: Iris status -14" event --cti "${CTI}")

set(ENV{IRIS_FAULT} progressive-complete)
run_cli("^0$" "\"width\":2" grab --cti "${CTI}" --stream-only --buffers 1 --json)
set(output_file "${CMAKE_CURRENT_BINARY_DIR}/iris-gentl-${suffix}.raw")
run_cli("^0$" "frame=2 width=2 height=4 pfnc=0x1100007 stride=4 bytes=16"
    grab 2 --cti "${CTI}" --stream-only --buffers 1 --output "${output_file}")
run_cli("^1$" "cannot create output file"
    grab --cti "${CTI}" --stream-only --buffers 1 --output "${output_file}")
file(READ "${output_file}" bytes HEX)
file(REMOVE "${output_file}")
if(NOT bytes STREQUAL "0102030405060708090a0b0c0d0e0f100102030405060708090a0b0c0d0e0f10")
    message(FATAL_ERROR "unexpected captured bytes: ${bytes}")
endif()

set(ENV{IRIS_FAULT} progressive-incomplete)
run_cli("^1$" "received a rejected image" grab --cti "${CTI}" --stream-only --buffers 1)

run_cli("^0$" "complete=0 incomplete=3 unsupported_payload=0 invalid_image=0 timeouts=0.*interval_samples=0"
    play 3 --cti "${CTI}" --stream-only --buffers 1)
set(ENV{IRIS_FAULT} payload)
run_cli("^0$" "unsupported_payload=2 invalid_image=0 timeouts=0"
    play 2 --cti "${CTI}" --stream-only --buffers 1)
set(ENV{IRIS_FAULT} invalid)
run_cli("^0$" "unsupported_payload=0 invalid_image=2 timeouts=0"
    play 2 --cti "${CTI}" --stream-only --buffers 1)
set(ENV{IRIS_FAULT} timeout)
run_cli("^0$" "\"timeouts\":2.*\"interval_samples\":0"
    play 2 --cti "${CTI}" --stream-only --buffers 1 --timeout 5 --json)
set(ENV{IRIS_FAULT} progressive-complete)
run_cli("^0$" "\"complete\":3,\"incomplete\":0.*\"timeouts\":0.*\"interval_samples\":2.*\"mean_interval_ns\":"
    play 3 --cti "${CTI}" --stream-only --buffers 1 --json)
run_cli("^0$" "complete=100.*interval_samples=99 min_interval_ns=" play --cti "${CTI}" --stream-only --buffers 1)
run_cli("^2$" "--output requires grab" play --cti "${CTI}" --output "${output_file}")

set(ENV{IRIS_FAULT} events-snapshot)
run_cli("^0$" "event=1234 bytes=12 host_timestamp_ns=[0-9]+ device_timestamp=0.*EventCounter=40"
    event --cti "${CTI}")
run_cli("^0$" "\"device_timestamp\":9007199254740993.*\"name\":\"EventCounter\",\"type\":\"integer\",\"value\":\"41\".*\"event\":\"112233445566778899\".*\"event\":\"abcd\".*\"features\":\\[\\]"
    event 4 --cti "${CTI}" --json)
run_cli("^1$" "receive event: Iris status" event 5 --cti "${CTI}" --json)
