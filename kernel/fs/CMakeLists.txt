add_subdirectory(ext4)
add_subdirectory(vfs)

if(RISCV)
    set(fs_sources
            ${ext4_sources}
            ${vfs_sources}
            ${CMAKE_CURRENT_SOURCE_DIR}/procfs.c
            PARENT_SCOPE
    )
elseif (LOONGARCH)
    set(fs_sources
            ${ext4_sources}
            ${vfs_sources}
            ${CMAKE_CURRENT_SOURCE_DIR}/procfs.c
            PARENT_SCOPE
    )
endif ()