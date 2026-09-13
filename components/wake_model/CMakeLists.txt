# Generated *_model.c files come from models/*.tflite (16-byte aligned) and
# are gitignored. PlatformIO EMBED_FILES pathing is unreliable
# (doubled .pio/.../*.tflite.S). Command models are optional.

get_filename_component(JARVIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

idf_build_get_property(python PYTHON)
if(NOT python)
    if(DEFINED ENV{PYTHON} AND NOT "$ENV{PYTHON}" STREQUAL "")
        set(python "$ENV{PYTHON}")
    else()
        find_program(python NAMES python python3 REQUIRED)
    endif()
endif()

function(wake_generate_model name)
    set(tflite "${JARVIS_ROOT}/models/${name}.tflite")
    set(cfile "${CMAKE_CURRENT_LIST_DIR}/${name}_model.c")
    if(NOT EXISTS "${tflite}")
        message(FATAL_ERROR "missing ${tflite}")
    endif()
    execute_process(
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gen_model_c.py"
                "${tflite}" "${cfile}" "${name}_tflite"
        RESULT_VARIABLE _wake_gen_rv)
    if(NOT _wake_gen_rv EQUAL 0)
        message(FATAL_ERROR "gen_model_c.py ${name} failed (${_wake_gen_rv})")
    endif()
endfunction()

set(WAKE_SRCS wake_model.cc)
set(WAKE_DEFS "")

wake_generate_model(hey_jarvis)
list(APPEND WAKE_SRCS hey_jarvis_model.c)

# Re-run configure when optional command models are added or replaced.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${JARVIS_ROOT}/models/light_on.tflite"
    "${JARVIS_ROOT}/models/light_off.tflite")

foreach(cmd light_on light_off)
    set(tflite "${JARVIS_ROOT}/models/${cmd}.tflite")
    if(EXISTS "${tflite}")
        wake_generate_model(${cmd})
        list(APPEND WAKE_SRCS ${cmd}_model.c)
        string(TOUPPER ${cmd} cmd_up)
        list(APPEND WAKE_DEFS WAKE_HAS_${cmd_up}=1)
        message(STATUS "wake: embedding ${cmd}.tflite")
    else()
        message(STATUS "wake: no models/${cmd}.tflite (listen window only)")
    endif()
endforeach()

idf_component_register(
    SRCS ${WAKE_SRCS}
    PRIV_INCLUDE_DIRS ${JARVIS_ROOT}/include
    REQUIRES esp-tflite-micro
    PRIV_REQUIRES heap)

if(WAKE_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${WAKE_DEFS})
endif()
