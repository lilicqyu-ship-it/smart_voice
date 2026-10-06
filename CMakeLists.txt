# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_voice)

# The Chinese offline TTS voice is a separate read-only data image.  Keep the
# source in the ESP-SR component and flash it together with the application so
# a normal `ninja flash` cannot leave the TTS partition stale or empty.
set(SMART_VOICE_TTS_DATA
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat")
if(EXISTS "${SMART_VOICE_TTS_DATA}")
    partition_table_get_partition_info(voice_data_size "--partition-name voice_data" "size")
    partition_table_get_partition_info(voice_data_offset "--partition-name voice_data" "offset")
    if(voice_data_size AND voice_data_offset)
        add_custom_target(smart_voice_tts_data ALL DEPENDS "${SMART_VOICE_TTS_DATA}")
        add_dependencies(flash smart_voice_tts_data)
        esptool_py_flash_to_partition(flash "voice_data" "${SMART_VOICE_TTS_DATA}")
    endif()
endif()
