add_subdirectory(ext4)
add_subdirectory(vfs)

if(RISCV)
    set(fs_sources
            ${ext4_sources}
            ${vfs_sources}
            PARENT_SCOPE
    )
elseif (LOONGARCH)
    set(fs_sources
            ${ext4_sources}
            ${vfs_sources}
            PARENT_SCOPE
    )
endif ()