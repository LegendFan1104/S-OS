# build the kernel and fs.img
add_custom_target(
        build
#        DEPENDS ../image/fs.img
)
if(RISCV)
    add_dependencies(build kernel-riscv ${userApps} initcode-rv)
elseif(LOONGARCH)
    add_dependencies(build kernel-loongarch ${userApps} initcode-la)
endif()







