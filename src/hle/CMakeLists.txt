# XDK functions replaced by name (see hle.h and tools/recomp/hle.py).
#
# Implementations are marked HLE_EXPORT(Name). tools.recomp scans this
# directory for the markers and generates the thunks that reach them, so a
# function added here is picked up at the next lift with no other edit.
#
# The DirectSound replacement (hle_dsound.c and the files it uses) is adapted
# from doaxbv-re (https://github.com/NoRain211/doaxbv-re), GPL-3.0.

if(MSVC)
    # /Zi so the PDB names this library's static functions in a sampled
    # profile (RECOMP_SAMPLE); see src/kernel/CMakeLists.txt.
    add_compile_options(/Zi)
endif()

add_library(xbox_hle STATIC
    hle_d3d8.c
    hle_d3d8_state.c
    hle_d3d8_texture.c
    hle_d3d8_vertex.c
    hle_d3d8_record.c
    hle_d3d8_interp.c
    d3d8_capture.c
    hle_dsound.c
    hle_dsound_3d.c
    hle_dsound_stream.c
    dsound_buffer_model.c
    xbox_adpcm.c
    hle_input.c
    hle_xmv.c
    hle_xmv_play.c
    hle_xapi_fiber.c
    input_model.c
    # The host pads behind the replaced XAPI input, through src/input's
    # bindings (SDL3 or XInput), plus RECOMP_FAKE_INPUT / RECOMP_INPUT_SEQ.
    input_host.c
)

# Host audio: XAudio2 on Windows, SDL3 elsewhere, with the same switches
# (RECOMP_MUTE, RECOMP_AUDIO_GAIN) and the same play-position clock.
if(WIN32)
    target_sources(xbox_hle PRIVATE audio_output_xaudio2.cpp)
else()
    target_sources(xbox_hle PRIVATE audio_output_sdl.c)
    target_link_libraries(xbox_hle PRIVATE SDL3::SDL3)
endif()
target_include_directories(xbox_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_hle PUBLIC xbox_config)
target_link_libraries(xbox_hle PUBLIC xbox_kernel)
# Host device choice and per-control bindings (src/input/input_bindings.c),
# shared with the USB gamepad so one config file covers both input paths.
target_link_libraries(xbox_hle PUBLIC xbox_input)
# The D3D8 replacements drive the host renderer in src/d3d, on every
# platform, and open a window of their own for it: Win32 on Windows, the host
# shell (src/host) elsewhere.
target_link_libraries(xbox_hle PRIVATE xbox_d3d8 xbox_video)
if(WIN32)
    target_link_libraries(xbox_hle PRIVATE xaudio2 ole32 xinput9_1_0 user32)
else()
    # The host shell: the main-thread loop a title's main.c runs, and the
    # game window. PUBLIC so a title gets it through this library.
    target_link_libraries(xbox_hle PUBLIC xbox_host)
endif()
