# Settings round trip for the USA-only "Skip Health & Safety Screen" option.
# Drives the game's headless settings CLI (the same one the launcher uses) in
# a scratch directory, so no real pikmin_settings.conf is read or written.
#
#   cmake -DP2_GAME=<nectar2> -DWORK_DIR=<scratch dir> -P pc_settings_skip_health_safety_test.cmake
cmake_minimum_required(VERSION 3.19)

if(NOT P2_GAME OR NOT WORK_DIR)
    message(FATAL_ERROR "P2_GAME and WORK_DIR are required")
endif()

set(kLabel "Skip Health & Safety Screen")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(conf "${WORK_DIR}/pikmin_settings.conf")

function(run_cli out_var)
    execute_process(COMMAND "${P2_GAME}" ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${P2_GAME} ${ARGN} exited ${rc}\n${err}")
    endif()
    # The dump is the last line of stdout; earlier lines may be log output.
    string(FIND "${out}" "{\"exactF1\"" at REVERSE)
    if(at LESS 0)
        message(FATAL_ERROR "no settings JSON in output:\n${out}")
    endif()
    string(SUBSTRING "${out}" ${at} -1 json)
    set(${out_var} "${json}" PARENT_SCOPE)
endfunction()

# Finds the row by label; sets FOUND_GROUP, FOUND_ROW, FOUND_VALUE,
# FOUND_SECTION and FOUND_ON (index of the "On" option).
function(find_row json)
    string(JSON groups LENGTH "${json}" groups)
    math(EXPR lastGroup "${groups} - 1")
    foreach(g RANGE ${lastGroup})
        string(JSON rows LENGTH "${json}" groups ${g} rows)
        if(rows EQUAL 0)
            continue()
        endif()
        math(EXPR lastRow "${rows} - 1")
        foreach(r RANGE ${lastRow})
            string(JSON label GET "${json}" groups ${g} rows ${r} label)
            if(label STREQUAL kLabel)
                string(JSON id GET "${json}" groups ${g} id)
                string(JSON value GET "${json}" groups ${g} rows ${r} value)
                string(JSON section GET "${json}" groups ${g} rows ${r} section)
                string(JSON options LENGTH "${json}" groups ${g} rows ${r} options)
                set(on -1)
                if(options GREATER 0)
                    math(EXPR lastOption "${options} - 1")
                    foreach(k RANGE ${lastOption})
                        string(JSON option GET "${json}" groups ${g} rows ${r} options ${k})
                        if(option STREQUAL "On")
                            set(on ${k})
                        endif()
                    endforeach()
                endif()
                set(FOUND_GROUP ${id} PARENT_SCOPE)
                set(FOUND_ROW ${r} PARENT_SCOPE)
                set(FOUND_VALUE "${value}" PARENT_SCOPE)
                set(FOUND_SECTION "${section}" PARENT_SCOPE)
                set(FOUND_ON ${on} PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
    message(FATAL_ERROR "row \"${kLabel}\" missing from the settings dump")
endfunction()

# 1. Fresh config: the row exists, sits under STARTUP and is off.
run_cli(json --settings-dump)
find_row("${json}")
if(NOT FOUND_VALUE STREQUAL "Off (original)")
    message(FATAL_ERROR "default should be \"Off (original)\", got \"${FOUND_VALUE}\"")
endif()
if(NOT FOUND_SECTION STREQUAL "STARTUP")
    message(FATAL_ERROR "expected section STARTUP, got \"${FOUND_SECTION}\"")
endif()
if(FOUND_ON LESS 0)
    message(FATAL_ERROR "row offers no \"On\" option")
endif()

# 2. Turning it on the way the launcher does persists it.
run_cli(json --settings-set ${FOUND_GROUP} ${FOUND_ROW} ${FOUND_ON})
file(READ "${conf}" text)
if(NOT text MATCHES "\nskipHealthSafety = 1\n")
    message(FATAL_ERROR "skipHealthSafety = 1 not saved:\n${text}")
endif()
run_cli(json --settings-dump)
find_row("${json}")
if(NOT FOUND_VALUE STREQUAL "On")
    message(FATAL_ERROR "after enabling, value is \"${FOUND_VALUE}\"")
endif()

# 3. Reset to defaults turns it back off.
run_cli(json --settings-reset)
file(READ "${conf}" text)
if(NOT text MATCHES "\nskipHealthSafety = 0\n")
    message(FATAL_ERROR "reset did not restore skipHealthSafety = 0:\n${text}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "skip Health & Safety setting: OK")
