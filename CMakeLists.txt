cmake_minimum_required(VERSION 3.21)

#if(NOT RISCV)
#    set(RISCV 1)
#endif()

#if(NOT LOONGARCH)
#    set(LOONGARCH 1)
#endif()



if(NOT PLATFORM)
    set(PLATFORM QEMU)
endif()

if (PLATFORM STREQUAL "QEMU")
    add_definitions(-DQEMU)
endif()

if(LOONGARCH)
    add_definitions(-DLOONGARCH)
    message("loongarch")
    set(CMAKE_C_COMPILER loongarch64-linux-gnu-gcc)
    set(CMAKE_ASM_COMPILER loongarch64-linux-gnu-gcc)
    # set compile flags
    set(CMAKE_C_FLAGS "-fno-omit-frame-pointer -O -MD -ffreestanding -fno-common -nostdlib -fno-stack-protector -fno-pie -no-pie")
    # set linker script
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-warn-rwx-segment")
endif()

if(RISCV)
    add_definitions(-DRISCV)
    message("riscv")
    set(CMAKE_C_COMPILER riscv64-unknown-elf-gcc)
    set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)
#    set(CMAKE_C_COMPILER riscv64-linux-gnu-gcc)
#    set(CMAKE_ASM_COMPILER riscv64-linux-gnu-gcc)
    # set compile flags
    set(CMAKE_C_FLAGS "-fno-omit-frame-pointer -O -MD -mcmodel=medany -ffreestanding -fno-common -nostdlib -mno-relax -fno-stack-protector -fno-pie -no-pie -static")
    # set linker script
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endif()


#
project(SOS C ASM)
include_directories(include)

include_directories(libc)

add_subdirectory(user) # make user apps
add_subdirectory(kernel) # make kernel
add_subdirectory(scripts) # to run
