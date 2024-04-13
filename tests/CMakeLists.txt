set(CMAKE_BUILD_TYPE Debug)

add_executable(tests
  ../circular_buffer.c
  ../flash_onboard.c
  main.c
  test_create.c
  test_append.c
  test_cursor.c
  test_restore.c
  # test_across_flash_sectors.c
)

target_link_libraries(tests
  hardware_adc
  hardware_flash
  hardware_sync
  pico_stdlib
)

target_include_directories(tests
  PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include
)
target_compile_options(tests PRIVATE -Werror -Wall -Wextra -Wnull-dereference)
pico_add_extra_outputs(tests)


if(OPENOCD)
  add_custom_target(run_tests
    COMMAND ${OPENOCD} -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "program tests.elf verify reset exit"
    DEPENDS tests
  )
endif()
