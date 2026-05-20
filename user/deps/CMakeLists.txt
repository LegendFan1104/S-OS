

if(RISCV)
    # Add custom command to execute Perl script
    add_custom_command(
            OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/usys.S
            COMMAND perl ${CMAKE_CURRENT_SOURCE_DIR}/usys.pl > ${CMAKE_CURRENT_SOURCE_DIR}/usys.S
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usys.pl
    )
    add_custom_target(generate_usys ALL
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usys.S
    )
    file(GLOB sources "*.c" )
    add_library(ulib ${sources} ${CMAKE_CURRENT_SOURCE_DIR}/usys.S)
    add_dependencies(ulib generate_usys)
elseif(LOONGARCH)
    # Add custom command to execute Perl script
    add_custom_command(
            OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.S
            COMMAND perl ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.pl > ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.S
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.pl
    )
    add_custom_target(generate_usys ALL
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.S
    )
    file(GLOB sources "*.c" )
    add_library(ulib ${sources} ${CMAKE_CURRENT_SOURCE_DIR}/usys-loongarch.S)
    add_dependencies(ulib generate_usys)
endif()