#编译.o文件到$(WORKPATH)/user/build/riscv目录
#调用make riscv
#但是现在用户程序不会链接到内核，而是在user目录下生成init_code

#已经定义了 CFLAGS += -Iinclude; ASFLAGS += -Iinclude
RISCV_CFLAGS += -I../include
RISCV_ASFLAGS += -I../include

ifeq ($(RISCV_TEST_PROFILE),ltp-musl)
RISCV_CFLAGS += -DTEST_PROFILE_LTP_MUSL=1
endif

ifeq ($(RISCV_TEST_PROFILE),ltp-glibc)
RISCV_CFLAGS += -DTEST_PROFILE_LTP_GLIBC=1
endif

ifeq ($(RISCV_TEST_PROFILE),submit)
RISCV_CFLAGS += -DTEST_PROFILE_SUBMIT=1
endif

ifeq ($(RISCV_TEST_PROFILE),probe)
RISCV_CFLAGS += -DTEST_PROFILE_PROBE=1
endif

ifeq ($(RISCV_TEST_PROFILE),buildstorm)
RISCV_CFLAGS += -DTEST_PROFILE_BUILDSTORM=1
endif

# card source
c_src := $(wildcard *.c) #user.c
s_src := $(wildcard *.S) #usys.S

# card obj file name
c_obj = $(patsubst %.c,$(WORKPATH)/user/build/riscv/%.o,$(c_src))
s_obj = $(patsubst %.S,$(WORKPATH)/user/build/riscv/%.o,$(s_src))


# 用户程序二进制及头文件生成相关变量
user_out := $(WORKPATH)/user/build/riscv/user.out
user_bin := $(WORKPATH)/user/build/riscv/user.bin
user_header := ../build/rv_init_code.h
user_objs := $(WORKPATH)/user/build/riscv/user.o $(WORKPATH)/user/build/riscv/usys.o

# 链接用户程序
$(user_out): $(user_objs)
	$(RISCV_LD) -nostdlib -static -e  init_main -T user_initcode.ld -Ttext 0 -o $@ $^

# 提取二进制数据
$(user_bin): $(user_out)
	$(RISCV_OBJCOPY) --only-keep-debug  $< $(user_out).debug
	$(RISCV_OBJCOPY) -S -O binary $< $@

# 生成C头文件，确保目录存在
$(user_header): $(user_bin)
	mkdir -p $(dir $@)
	python3 -c "import sys; data=open(sys.argv[1],'rb').read(); print('unsigned char init_code[] = {'); [print('  '+', '.join(f'0x{b:02x}' for b in data[i:i+12])+',') for i in range(0,len(data),12)]; print('};'); print(f'unsigned int init_code_len = {len(data)};')" $< > $@


# the source files' dependencies, created by gcc
depends = \
	$(patsubst %.c, $(WORKPATH)/user/build/riscv/%.d, $(c_src)) \
	$(patsubst %.S, $(WORKPATH)/user/build/riscv/%.d, $(s_src))

#现在不需要把用户程序链接到内核了
riscv: init_path $(user_header) #$(c_obj) $(s_obj)
	cp $(user_header) $(WORKPATH)/include/kernel/rv_init_code.h

$(c_obj): $(WORKPATH)/user/build/riscv/%.o: %.c 
	$(RISCV_CC) -c $(RISCV_CFLAGS) -MF $(WORKPATH)/user/build/riscv/$*.d -o $@ $<

$(s_obj): $(WORKPATH)/user/build/riscv/%.o: %.S 
	$(RISCV_CC) -c $(RISCV_ASFLAGS) -MF $(WORKPATH)/user/build/riscv/$*.d -o $@ $<

-include $(depends)

init_path:
	mkdir -p $(WORKPATH)/user/build/riscv/
