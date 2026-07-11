# ==============================================================================
# 工具链配置
# ==============================================================================
# 使用 ARM Linux 交叉编译工具链（针对 IMX6ULL 平台，ARM Cortex-A7）
CC = arm-linux-gnueabihf-gcc
AR = arm-linux-gnueabihf-ar

# ==============================================================================
# 项目配置
# ==============================================================================
TARGET_Name        = MemoryPool
ProgramVER         = 1.0.0
USERNAME           = zlzksrl
LIBFILE_PATH       = ~/zlzksrl/LinuxARM/Program/DataStructureLibrary/Library/

# ==============================================================================
# 安装路径
# ==============================================================================
INSTALL_PATH    =  $(LIBFILE_PATH)/$(TARGET_Name)/
INSTALLBOARD       = root@192.168.1.6:/usr/zlzksrl/app/$(TARGET_Name)/

# ThreadQueue 头文件路径
ThreadQueue_LIBPATH		    = -L $(LIBFILE_PATH)/ThreadQueue/StaticLib
ThreadQueue_LIBINCPATH		= -I $(LIBFILE_PATH)/ThreadQueue/include

# ==============================================================================
# 编译选项
# ==============================================================================
# -g: 调试信息; -Wall: 常见警告; -Wextra: 额外警告
CFLAGS  = -g -Wall -Wextra -pthread
CFLAGS  := $(CFLAGS) $(ThreadQueue_LIBINCPATH)

# 位置无关代码（动态库必需）
PIC_FLAGS = -fPIC

# 链接选项：MemoryPool 使用 pthread_mutex/cond，需要 -pthread
LDFLAGS = -pthread
LDFLAGS := $(LDFLAGS) $(ThreadQueue_LIBPATH) 
LDFLAGS := $(LDFLAGS)  -lThreadQueue

# ==============================================================================
# 源文件与目标文件
# ==============================================================================
SOURCES     = MemoryPool.c
MAIN_SOURCE = main.c

STATIC_OBJS = $(addprefix obj_static/,$(notdir $(SOURCES:.c=.o)))
SHARED_OBJS = $(addprefix obj_shared/,$(notdir $(SOURCES:.c=.o)))
MAIN_OBJ    = $(MAIN_SOURCE:.c=.o)

# ==============================================================================
# 输出文件
# ==============================================================================
STATIC_LIBNAME = lib$(TARGET_Name).a
SHARED_LIBNAME = lib$(TARGET_Name).so

TARGET_DATE    = $(TARGET_Name)_DebugPro$(shell date "+%Y%m%d").bin
TARGET         = $(TARGET_Name)_DebugPro.bin

TARGET_MAKETIME_H  = ./../src/$(TARGET_Name)_Maketime.h
PROJECT_INFO       = ./ProjectInfo.txt


# ==============================================================================
# 自动生成编译时间戳头文件
# ==============================================================================
MAKETIME      = $(shell date "+%G.%m.%d-%H.%M.%S")
MAKETIME_FILE := $(shell echo "\#ifndef __$(TARGET_Name)_PROJECT_MAKETIME_H__" >  $(TARGET_MAKETIME_H))
MAKETIME_FILE := $(shell echo "\#define __$(TARGET_Name)_PROJECT_MAKETIME_H__" >> $(TARGET_MAKETIME_H))
MAKETIME_FILE := $(shell echo " "                                              >> $(TARGET_MAKETIME_H))
MAKETIME_FILE := $(shell echo "\#define $(TARGET_Name)_PROJECT_MAKETIME \"$(USERNAME)-$(ProgramVER) at $(MAKETIME)\"" >> $(TARGET_MAKETIME_H))
MAKETIME_FILE := $(shell echo " "                                              >> $(TARGET_MAKETIME_H))
MAKETIME_FILE := $(shell echo "\#endif"                                        >> $(TARGET_MAKETIME_H))

# ==============================================================================
# 伪目标
# ==============================================================================
.PHONY: all clean install install_lib slib dlib app

# ---- 默认目标: 静态库 + 动态库 + 可执行程序 ----
all: slib dlib app
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(STATIC_LIBNAME)" >  $(PROJECT_INFO)
	md5sum $(STATIC_LIBNAME) >> $(PROJECT_INFO)
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(SHARED_LIBNAME)" >> $(PROJECT_INFO)
	md5sum $(SHARED_LIBNAME) >> $(PROJECT_INFO)
	cat $(PROJECT_INFO)
	rm -f *.o -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
# ==============================================================================

# ---- 静态库 ----
slib: $(STATIC_LIBNAME)

$(STATIC_LIBNAME): $(STATIC_OBJS)
	$(AR) -crs $@ $^

obj_static/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) -o  $@  $< -c $(CFLAGS)

# ==============================================================================
# ---- 动态库 ----
dlib: $(SHARED_LIBNAME)

$(SHARED_LIBNAME): $(SHARED_OBJS)
	$(CC) -shared $(PIC_FLAGS) $(CFLAGS) -o $@ $^

obj_shared/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_FLAGS) $(CFLAGS) -c $< -o $@

# ==============================================================================
# ---- 可执行程序（链接静态库） ----
app: $(STATIC_LIBNAME) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(MAIN_OBJ) $(STATIC_LIBNAME) $(LDFLAGS)
	@chmod 755 $(TARGET)

$(MAIN_OBJ): $(MAIN_SOURCE)
	$(CC) $(CFLAGS) -c -o $@ $<

# ==============================================================================
# ---- 清理 ----
clean:
	rm -f *.bin -rf
	rm -f *.o -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
	rm -f $(STATIC_LIBNAME) $(SHARED_LIBNAME)
	rm -f $(TARGET)
	rm -f $(PROJECT_INFO)

# ---- 安装可执行文件到目标开发板 ----
install:
	scp $(TARGET) $(INSTALLBOARD);

# ---- 安装库文件到本地公共库目录 ----
install_lib:slib dlib
	rm $(INSTALL_PATH) -rf
	mkdir -p $(INSTALL_PATH)
	mkdir -p $(INSTALL_PATH)include/
	mkdir -p $(INSTALL_PATH)StaticLib/
	mkdir -p $(INSTALL_PATH)SharedLib/
	cp $(STATIC_LIBNAME) $(INSTALL_PATH)StaticLib/
	cp $(SHARED_LIBNAME) $(INSTALL_PATH)SharedLib/
	cp ../include/* $(INSTALL_PATH)include/
	cp $(PROJECT_INFO) $(INSTALL_PATH)
