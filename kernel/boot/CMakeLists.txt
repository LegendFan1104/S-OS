
if(RISCV)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/riscv/initcode.S
            PROPERTIES
            OBJECT_DEPENDS ${PROJECT_SOURCE_DIR}/user/initcode-rv
    )
    set(boot_sources
            ${CMAKE_CURRENT_SOURCE_DIR}/riscv/entry.S
            ${CMAKE_CURRENT_SOURCE_DIR}/main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/riscv/start.c
            ${CMAKE_CURRENT_SOURCE_DIR}/riscv/initcode.S
            PARENT_SCOPE
    )
elseif(LOONGARCH)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/initcode.S
            PROPERTIES
            OBJECT_DEPENDS ${PROJECT_SOURCE_DIR}/user/initcode-la
    )
    set(boot_sources
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/entry.S
            ${CMAKE_CURRENT_SOURCE_DIR}/main.c
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/initcode.S
            PARENT_SCOPE
    )
endif()
