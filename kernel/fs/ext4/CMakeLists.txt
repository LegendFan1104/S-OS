add_subdirectory(lwext4)

set(ext4_sources
        ${lwext4_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/vfs_ext4_blockdev_ext.c
        ${CMAKE_CURRENT_SOURCE_DIR}/vfs_ext4_ext.c
        PARENT_SCOPE
)