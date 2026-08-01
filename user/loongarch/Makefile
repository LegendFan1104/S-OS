#编译.o文件到$(WORKPATH)/user/build/riscv目录
#调用make riscv
#但是现在用户程序不会链接到内核，而是在user目录下生成init_code

#已经定义了 CFLAGS += -Iinclude; ASFLAGS += -Iinclude
CFLAGS += -I../include
ASFLAGS += -I../include

ifeq ($(LOONGARCH_TEST_PROFILE),ltp-musl)
CFLAGS += -DTEST_PROFILE_LTP_MUSL=1
endif

ifeq ($(LOONGARCH_TEST_PROFILE),ltp-glibc)
CFLAGS += -DTEST_PROFILE_LTP_GLIBC=1
endif

ifeq ($(LOONGARCH_TEST_PROFILE),submit)
CFLAGS += -DTEST_PROFILE_SUBMIT=1
endif

ifeq ($(LOONGARCH_TEST_PROFILE),buildstorm)
CFLAGS += -DTEST_PROFILE_BUILDSTORM=1
endif

# card source
c_src := $(wildcard *.c) #user.c
s_src := $(wildcard *.S) #usys.S

# card obj file name
c_obj = $(patsubst %.c,$(WORKPATH)/user/build/loongarch/%.o,$(c_src))
s_obj = $(patsubst %.S,$(WORKPATH)/user/build/loongarch/%.o,$(s_src))

# 用户程序二进制及头文件生成相关变量
user_out := $(WORKPATH)/user/build/loongarch/user.out
user_bin := $(WORKPATH)/user/build/loongarch/user.bin
user_header := ../build/la_init_code.h
user_objs := $(WORKPATH)/user/build/loongarch/user.o $(WORKPATH)/user/build/loongarch/usys.o

# 链接用户程序
$(user_out): $(user_objs)
	$(LD) -nostdlib -static -e  init_main -T user_initcode.ld -Ttext 0 -o $@ $^

# 提取二进制数据
$(user_bin): $(user_out)
	$(OBJCOPY) --only-keep-debug  $< $(user_out).debug
	$(OBJCOPY) -S -O binary $< $@

# 生成C头文件，确保目录存在
$(user_header): $(user_bin)
	@mkdir -p $(dir $@)
	@python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.stdout.write('unsigned char init_code[] = {'); [sys.stdout.write(', '.join(f'0x{b:02x}' for b in d[i:i+12])+',') for i in range(0,len(d),12)]; sys.stdout.write('};\n'); sys.stdout.write(f'unsigned int init_code_len = {len(d)};\n')" $< > $@


# the source files' dependencies, created by gcc
depends = \
	$(patsubst %.c, $(WORKPATH)/user/build/loongarch/%.d, $(c_src)) \
	$(patsubst %.S, $(WORKPATH)/user/build/loongarch/%.d, $(s_src))

#现在不需要把用户程序链接到内核了
la: init_path $(user_header) #$(c_obj) $(s_obj)
	cp $(user_header) $(WORKPATH)/include/kernel/la_init_code.h

$(c_obj): $(WORKPATH)/user/build/loongarch/%.o: %.c 
	$(CC) -c $(CFLAGS) -MF $(WORKPATH)/user/build/loongarch/$*.d -o $@ $<

$(s_obj): $(WORKPATH)/user/build/loongarch/%.o: %.S 
	$(CC) -c $(ASFLAGS) -MF $(WORKPATH)/user/build/loongarch/$*.d -o $@ $<

-include $(depends)

init_path:
	mkdir -p $(WORKPATH)/user/build/loongarch/
