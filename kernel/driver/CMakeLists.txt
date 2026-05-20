if(RISCV)
    set(driver_sources
            ${CMAKE_CURRENT_SOURCE_DIR}/riscv/virtio_disk.c
            ${CMAKE_CURRENT_SOURCE_DIR}/bio.c
            PARENT_SCOPE
    )

elseif(LOONGARCH)
    set(driver_sources
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/pci.c
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/virtio_disk.c
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/virtio_pci.c
            ${CMAKE_CURRENT_SOURCE_DIR}/loongarch/virtio_ring.c
            ${CMAKE_CURRENT_SOURCE_DIR}/bio.c
            PARENT_SCOPE
    )
endif()

