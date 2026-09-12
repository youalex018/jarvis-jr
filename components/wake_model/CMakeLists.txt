# hey_jarvis_model.c is generated here from models/hey_jarvis.tflite (16-byte
# aligned) and gitignored. PlatformIO EMBED_FILES pathing is unreliable
# (doubled .pio/.../*.tflite.S).

get_filename_component(JARVIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MODEL_TFLITE "${JARVIS_ROOT}/models/hey_jarvis.tflite")
set(MODEL_C "${CMAKE_CURRENT_LIST_DIR}/hey_jarvis_model.c")

if(NOT EXISTS "${MODEL_TFLITE}")
    message(FATAL_ERROR "missing ${MODEL_TFLITE}")
endif()

idf_build_get_property(python PYTHON)
if(NOT python)
    if(DEFINED ENV{PYTHON} AND NOT "$ENV{PYTHON}" STREQUAL "")
        set(python "$ENV{PYTHON}")
    else()
        find_program(python NAMES python python3 REQUIRED)
    endif()
endif()

execute_process(
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gen_model_c.py"
            "${MODEL_TFLITE}" "${MODEL_C}"
    RESULT_VARIABLE _wake_gen_rv)
if(NOT _wake_gen_rv EQUAL 0)
    message(FATAL_ERROR "gen_model_c.py failed (${_wake_gen_rv})")
endif()

idf_component_register(
    SRCS wake_model.cc hey_jarvis_model.c
    PRIV_INCLUDE_DIRS ${JARVIS_ROOT}/include
    REQUIRES esp-tflite-micro
    PRIV_REQUIRES heap)
