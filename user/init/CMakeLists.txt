
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
if(RISCV)
    add_executable(initcode-rv initcode.S ../app/init-riscv.c)
    target_link_libraries(initcode-rv ulib)
    set_target_properties(initcode-rv PROPERTIES LINKER_LANGUAGE C COMPILE_FLAGS "-march=rv64g -nostdinc -I. -Ikernel -c" LINK_FLAGS "-N -e start -Ttext 0")
    add_custom_command(
        TARGET initcode-rv
        POST_BUILD
        COMMAND riscv64-unknown-elf-objcopy -O binary $<TARGET_FILE:initcode-rv> ${PROJECT_SOURCE_DIR}/user/initcode-rv
        VERBATIM
    )
elseif(LOONGARCH)
    add_executable(initcode-la initcode-loongarch.S ../app/init-loongarch.c)
    target_link_libraries(initcode-la ulib)
    set_target_properties(initcode-la PROPERTIES LINKER_LANGUAGE C COMPILE_FLAGS "-march=loongarch64 -nostdinc -I. -Ikernel -c" LINK_FLAGS "-N -e start -Ttext 0")
    add_custom_command(
        TARGET initcode-la
        POST_BUILD
        COMMAND loongarch64-linux-gnu-objcopy -O binary $<TARGET_FILE:initcode-la> ${PROJECT_SOURCE_DIR}/user/initcode-la
        VERBATIM
    )
endif()
