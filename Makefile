MAKEFLAGS += --no-print-directory


build-release-riscv:
	@echo "Building SOS release"
	@cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DRISCV=ON
	@cmake --build build --target build  -- -j 8
	@echo "Done"

build-release-loongarch:
	@echo "Building SOS release"
	@cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLOONGARCH=ON
	@cmake --build build --target build  -- -j 8
	@echo "Done"

build-debug-riscv:
	@echo "Building SOS debug"
	@cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DRISCV=ON
	@cmake --build build --target build -- -j 8
	@echo "Done"

build-debug-loongarch:
	@echo "Building SOS debug"
	@cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLOONGARCH=ON
	@cmake --build build --target build -- -j 8
	@echo "Done"


clean:
	@echo "Cleaning SOS"
	@if [ -d "build" ]; then rm -r build; fi
	@if [ -d "bin" ]; then rm -r bin; fi
	@if [ -d "image" ]; then rm -r image; fi
	@echo "Done"


make-image:
	$(call build_image,fs)

make-image-force:
	@rm -f image/fs.img
	$(call build_image,fs)

all: build-release-riscv make-image
	@cp bin/kernel-riscv kernel-rv
	@cp image/fs.img disk.img
	@${MAKE} clean
	@${MAKE} build-release-loongarch
	@cp bin/kernel-loongarch kernel-la


define build_image
	@mkdir -p image
	@mkdir -p data
	@mkdir -p data/mnt
	@if [ ! -f "image/$(1).img" ] || [ "$(1)" = "tests" ] || [ "x$(1)" = "xbusybox" ] || [ "x$(1)" = "xfinal" ] ; then\
		dd if=/dev/zero of=$(1).img bs=1M count=512 ;\
		mkfs.ext4 -O ^metadata_csum -F -b 4096 -L rootfs $(1).img ;\
		mount -o sync -t ext4 $(1).img data/mnt ;\
		cp -r user/bin/* data/mnt/ ;\
		sync data/mnt ;\
		umount -v data/mnt;\
		md5sum $(1).img ;\
		mv $(1).img image/;\
	fi
endef

loongarch:build-release-loongarch
		@cp bin/kernel-loongarch kernel-la

qemu-riscv: make-image-force build-release-riscv
	@cp bin/kernel-riscv kernel-rv
	@cp image/fs.img disk.img
	@sh scripts/qemu.sh
.PHONY:build-release

qemu-loongarch: make-image-force build-release-loongarch
	@cp bin/kernel-loongarch kernel-la
	@cp image/fs.img disk.img
	@sh scripts/qemu-loongarch.sh
.PHONY:build-release

build-image: make-image


qemu-gdb-riscv: build-debug-riscv
	@sh scripts/qemu-gdb.sh

gdb-client:
	riscv64-unknown-elf-gdb -iex "set auto-load safe-path /work/"

dump: build-release-riscv
	@sh scripts/objdump-files.sh
