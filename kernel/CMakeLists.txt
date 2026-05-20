
#entry.S 处于SOURCES的第一个位置

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

if(RISCV)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T ${CMAKE_CURRENT_SOURCE_DIR}/linker/riscv/kernel.ld")
elseif(LOONGARCH)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T ${CMAKE_CURRENT_SOURCE_DIR}/linker/loongarch/kernel.ld")
endif()

add_subdirectory(boot)
add_subdirectory(driver)
add_subdirectory(fs)
add_subdirectory(lib)
add_subdirectory(mem)
add_subdirectory(proc)
add_subdirectory(sys)
add_subdirectory(trap)

set(SOURCES
        ${boot_sources}
        ${driver_sources}
        ${fs_sources}
        ${lib_sources}
        ${mem_sources}
        ${proc_sources}
        ${sys_sources}
        ${trap_sources}
)

if(RISCV)
    add_executable(
            kernel-riscv
            ${SOURCES}
    )
    add_dependencies(kernel-riscv initcode-rv)
    set_target_properties(kernel-riscv PROPERTIES LINK_FLAGS "-z max-page-size=4096")
elseif(LOONGARCH)
    add_executable(
            kernel-loongarch
            ${SOURCES}
    )
    add_dependencies(kernel-loongarch initcode-la)
    set_target_properties(kernel-loongarch PROPERTIES LINK_FLAGS "-z max-page-size=4096")
endif()
