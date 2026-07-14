# ==============================================================================
# 工具链配置
# ==============================================================================
# 使用 ARM Linux 交叉编译工具链（针对 IMX6ULL 平台，ARM Cortex-A7）
CC = arm-linux-gnueabihf-gcc
AR = arm-linux-gnueabihf-ar

# ==============================================================================
# 项目配置
# ==============================================================================
TARGET_Name        = SoftTimer
ProgramVER         = 1.0.0
USERNAME           = zlzksrl
# 用 ?= 使 CI/环境变量可 override：make LIBFILE_PATH=/opt/lib
LIBFILE_PATH      ?= ~/zlzksrl/LinuxARM/Program/DataStructureLibrary/Library

# ==============================================================================
# 安装路径
# ==============================================================================
# 本地安装路径（库文件和头文件安装到此目录）
INSTALL_PATH    =  $(LIBFILE_PATH)/$(TARGET_Name)/
# 远程部署目标（通过 scp 部署到目标开发板）
INSTALLBOARD       = root@192.168.1.6:/usr/zlzksrl/app/$(TARGET_Name)/

# ==============================================================================
# 依赖库路径（公共库目录）
# ==============================================================================

# KernelLinkedList 头文件路径（RedBlackTree 依赖 container_of / offsetof 宏）
KernelLinkedList_LIBINCPATH		= -I $(LIBFILE_PATH)/KernelLinkedList/include

# 红黑树 头文件路径
RedBlackTree_LIBPATH		= -L $(LIBFILE_PATH)/RedBlackTree/StaticLib
RedBlackTree_LIBINCPATH		= -I $(LIBFILE_PATH)/RedBlackTree/include

# ThreadQueue 头文件路径
ThreadQueue_LIBPATH		    = -L $(LIBFILE_PATH)/ThreadQueue/StaticLib
ThreadQueue_LIBINCPATH		= -I $(LIBFILE_PATH)/ThreadQueue/include

ThreadManage_LIBPATH		= -L $(LIBFILE_PATH)/ThreadManage/StaticLib
ThreadManage_LIBINCPATH		= -I $(LIBFILE_PATH)/ThreadManage/include

# ==============================================================================
# 编译选项
# ==============================================================================
# -g:      生成调试信息
# -Wall:   开启常见警告
# -Wextra: 开启额外警告
# -std=gnu99: C99 + GNU 扩展（pthread_condattr_setclock、CLOCK_MONOTONIC 需要 _GNU_SOURCE，gnu99 隐含开启）
CFLAGS  = -g -Wall -Wextra -std=gnu99
CFLAGS  := $(CFLAGS) -pthread
CFLAGS  := $(CFLAGS) $(KernelLinkedList_LIBINCPATH)
CFLAGS  := $(CFLAGS) $(RedBlackTree_LIBINCPATH)
CFLAGS  := $(CFLAGS) $(ThreadQueue_LIBINCPATH)
CFLAGS  := $(CFLAGS) $(ThreadManage_LIBINCPATH)

# 位置无关代码编译选项（动态库必需）
PIC_FLAGS = -fPIC

# 链接选项：pthread + rt（clock_gettime 在部分 glibc 需要 -lrt）+ 依赖库
LDFLAGS = -pthread
LDFLAGS := $(LDFLAGS) -lrt
LDFLAGS := $(LDFLAGS) $(ThreadManage_LIBPATH)
LDFLAGS := $(LDFLAGS)  -lThreadManage
LDFLAGS := $(LDFLAGS) $(ThreadQueue_LIBPATH)
LDFLAGS := $(LDFLAGS)  -lThreadQueue
LDFLAGS := $(LDFLAGS) $(RedBlackTree_LIBPATH)
LDFLAGS := $(LDFLAGS)  -lRedBlackTree

# ==============================================================================
# 源文件与目标文件
# ==============================================================================
SOURCES     = SoftTimer.c
MAIN_SOURCE = main.c

# 目标文件（分离静态库和动态库的 .o 文件，避免 -fPIC 混用）
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
# 伪目标声明
# ==============================================================================
.PHONY: all clean install install_lib slib dlib app

# ---- 默认目标: 构建静态库、动态库、可执行程序 ----
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

# ---- 静态库构建 ----
slib: $(STATIC_LIBNAME)

$(STATIC_LIBNAME): $(STATIC_OBJS)
	$(AR) -crs $@ $^

# 静态库 .o 文件（不加 -fPIC）
obj_static/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) -o  $@  $< -c $(CFLAGS)

# ==============================================================================
# ---- 动态库构建 ----
dlib: $(SHARED_LIBNAME)

$(SHARED_LIBNAME): $(SHARED_OBJS)
	$(CC) -shared $(PIC_FLAGS) $(CFLAGS) -o $@ $^

# 动态库 .o 文件（必须加 -fPIC）
obj_shared/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_FLAGS) $(CFLAGS) -c $< -o $@

# ==============================================================================
# ---- 可执行程序构建（链接静态库） ----
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

# ---- 安装可执行文件到目标开发板（scp） ----
install:
	scp $(TARGET) $(INSTALLBOARD);

# ---- 安装库文件到本地公共库目录 ----
install_lib: slib dlib
	rm $(INSTALL_PATH) -rf
	mkdir -p $(INSTALL_PATH)
	mkdir -p $(INSTALL_PATH)include/
	mkdir -p $(INSTALL_PATH)StaticLib/
	mkdir -p $(INSTALL_PATH)SharedLib/
	cp $(STATIC_LIBNAME) $(INSTALL_PATH)StaticLib/
	cp $(SHARED_LIBNAME) $(INSTALL_PATH)SharedLib/
	cp ../include/* $(INSTALL_PATH)include/
	cp $(PROJECT_INFO) $(INSTALL_PATH)
