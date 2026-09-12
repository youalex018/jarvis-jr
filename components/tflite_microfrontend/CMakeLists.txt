# Vendored TFLM audio frontend (not a git submodule; see ORIGIN.txt).
# Dropped from esp-tflite-micro 1.4. Includes:
#   tensorflow/lite/experimental/microfrontend/lib/*.h

set(lib tensorflow/lite/experimental/microfrontend/lib)

idf_component_register(
    SRCS
        ${lib}/fft.cc
        ${lib}/fft_util.cc
        ${lib}/filterbank.c
        ${lib}/filterbank_util.c
        ${lib}/frontend.c
        ${lib}/frontend_util.c
        ${lib}/kiss_fft_int16.cc
        ${lib}/log_lut.c
        ${lib}/log_scale.c
        ${lib}/log_scale_util.c
        ${lib}/noise_reduction.c
        ${lib}/noise_reduction_util.c
        ${lib}/pcan_gain_control.c
        ${lib}/pcan_gain_control_util.c
        ${lib}/window.c
        ${lib}/window_util.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp-tflite-micro)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=shadow
    -Wno-error=sign-compare
    -Wno-maybe-uninitialized
    -Wno-missing-field-initializers)
