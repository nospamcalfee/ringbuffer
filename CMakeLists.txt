cmake_minimum_required(VERSION 3.13...3.27)

if(NOT PICO_SDK_PATH)
  set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(PROGRAM_NAME circularbuffer)

project(${PROGRAM_NAME} C CXX ASM)

set(PICO_BOARD pico_w)
set(FAMILY rp2040)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(${PROGRAM_NAME}
  rbmain.c
  circular_buffer.c
  flash_onboard.c
)
target_link_libraries(${PROGRAM_NAME}
  hardware_adc
  hardware_flash
  hardware_rtc
  hardware_sync
  pico_stdlib
)

pico_set_linker_script(${PROGRAM_NAME} ${CMAKE_SOURCE_DIR}/memmap_custom.ld)

target_include_directories(${PROGRAM_NAME}
  PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/include
)
target_compile_options(${PROGRAM_NAME} PUBLIC -Werror -Wall -Wextra -Wnull-dereference -g -O0)
string(APPEND CMAKE_EXE_LINKER_FLAGS "-Wl,--print-memory-usage")

pico_enable_stdio_usb(${PROGRAM_NAME} 1)
pico_add_extra_outputs(${PROGRAM_NAME})


find_program(OPENOCD openocd)
if(OPENOCD)
  add_custom_target(flash
    COMMAND ${OPENOCD} -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "program ${CMAKE_PROJECT_NAME}.elf verify reset exit"
    DEPENDS ${CMAKE_PROJECT_NAME}
  )

  add_subdirectory(tests EXCLUDE_FROM_ALL)
endif()
