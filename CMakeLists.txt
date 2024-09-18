cmake_minimum_required(VERSION 3.13)

# set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD pico2)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PIMORONI_PICO_PATH}/pimoroni_pico_import.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(a8_pico_sio C CXX ASM)

pico_sdk_init()

include(libraries/pico_graphics/pico_graphics)
include(libraries/pico_display_2/pico_display_2)
include(drivers/rgbled/rgbled)
include(drivers/button/button)
include(drivers/st7789/st7789)
include(common/pimoroni_bus)

add_executable(a8_pico_sio)

## 16MB of Flash on Pimoroni Lipo Pico
#target_compile_definitions(a8_pico_sio PRIVATE
#    PICO_FLASH_SIZE_BYTES=16777216
#)

target_sources(a8_pico_sio PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/flash_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/hw_config.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/diskio.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ff.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ffunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/sd_driver/crc.c
    ${CMAKE_CURRENT_LIST_DIR}/sd_driver/sd_card.c
    ${CMAKE_CURRENT_LIST_DIR}/sd_driver/sd_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/sd_driver/spi.c
    # ffsystem.c is not used
)

pico_generate_pio_header(a8_pico_sio ${CMAKE_CURRENT_LIST_DIR}/pin_io.pio)
pico_generate_pio_header(a8_pico_sio ${CMAKE_CURRENT_LIST_DIR}/disk_counter.pio)

target_include_directories(a8_pico_sio PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/sd_driver
)

target_link_libraries(a8_pico_sio PUBLIC pico_stdlib pico_rand pico_multicore hardware_pio hardware_uart hardware_flash tinyusb_device hardware_dma hardware_irq hardware_spi hardware_pwm hardware_dma rgbled button pico_graphics pico_display_2 st7789)

pico_add_extra_outputs(a8_pico_sio)
