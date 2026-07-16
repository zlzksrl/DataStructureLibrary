# ==============================================================================
# 工具链配置
# ==============================================================================
# 使用 ARM Linux 交叉编译工具链（针对 IMX6ULL 平台，ARM Cortex-A7）
CC ?= arm-linux-gnueabihf-gcc
CXX ?= arm-linux-gnueabihf-g++
AR ?= arm-linux-gnueabihf-ar
# ==============================================================================
# 项目配置
# ==============================================================================
TARGET_Name        = MemoryPool
ProgramVER         = 1.0.0
USERNAME           = zlzksrl
MAKETIME           = $(shell date "+%G.%m.%d-%H.%M.%S")
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
# ThreadQueue 头文件路径
ThreadQueue_LIBPATH		    = -L $(LIBFILE_PATH)/ThreadQueue/StaticLib
ThreadQueue_LIBINCPATH		= -I $(LIBFILE_PATH)/ThreadQueue/include

# ==============================================================================
# 编译选项
# ==============================================================================
# -g: 调试信息; -Wall: 常见警告; -Wextra: 额外警告
# ==============================================================================
# 编译选项
# ==============================================================================
# -g:          生成调试信息（GDB 调试用）
# -Wall:       开启常见警告
# -Wextra:     开启额外警告
# -std=gnu11:  显式指定 C11 + GNU 扩展。V1.1 抗并发销毁依赖 <stdatomic.h>（C11），
#              gnu11 同时打开 _GNU_SOURCE 保证 pthread/clock_gettime 等 POSIX 扩展可见。
#              GCC 4.9+ 支持；用老工具链需自行调整。
CFLAGS  = -g -Wall -Wextra -std=gnu11
CFLAGS  := $(CFLAGS) -pthread
CFLAGS  := $(CFLAGS) $(ThreadQueue_LIBINCPATH)

# 位置无关代码编译选项（动态库必需）
PIC_FLAGS = -fPIC

##############链接选项###########################################################
#####链接选项-开始#####
#LDFLAGS :=$(LDFLAGS) -Wl,--start-group 
# 链接选项：pthread + 依赖库
LDFLAGS = -pthread

LDFLAGS := $(LDFLAGS) $(ThreadQueue_LIBPATH) 
LDFLAGS := $(LDFLAGS)  -lThreadQueue

#####链接选项-结束#####
#LDFLAGS :=$(LDFLAGS) -Wl,--end-group
# ==============================================================================
# 源文件与目标文件
# ==============================================================================
# 库源文件（位于 ../src/ 目录）
SOURCES     = MemoryPool.c
# 测试主程序源文件（位于当前目录）
MAIN_SOURCE = main.c


# 目标文件（分离静态库和动态库的 .o 文件，避免 -fPIC 混用）
STATIC_OBJS = $(addprefix obj_static/,$(notdir $(SOURCES:.c=.o)))
SHARED_OBJS = $(addprefix obj_shared/,$(notdir $(SOURCES:.c=.o)))
MAIN_OBJ    = $(MAIN_SOURCE:.c=.o)

# ==============================================================================
# 输出文件
# ==============================================================================
# 静态库和动态库文件名
STATIC_LIBNAME = lib$(TARGET_Name).a
SHARED_LIBNAME = lib$(TARGET_Name).so

# 可执行文件（带日期戳的版本和默认版本）
TARGET_DATE    = $(TARGET_Name)_DebugPro$(shell date "+%Y%m%d").bin
TARGET         = $(TARGET_Name)_DebugPro.bin 


# 自动生成的头文件（包含编译时间戳信息）
TARGET_MAKETIME_H  = ./../src/$(TARGET_Name)_Maketime.h

# 自动生成的工程信息文件（包含库文件 MD5 校验）
PROJECT_INFO     = ./ProjectInfo.txt


# ==============================================================================
# 伪目标
.PHONY: all clean install install_lib slib dlib app cleantime time

# ---- 默认目标: 构建静态库、动态库、可执行程序 ----
all: time slib dlib app	
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME)" >  $(PROJECT_INFO)
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(STATIC_LIBNAME)" >  $(PROJECT_INFO)
	md5sum $(STATIC_LIBNAME) >> $(PROJECT_INFO)
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(SHARED_LIBNAME)" >> $(PROJECT_INFO)
	md5sum $(SHARED_LIBNAME) >> $(PROJECT_INFO)
	cat $(PROJECT_INFO)
	rm -f *.o  -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
# ================================================================================================================================
# 时间头文件
time:cleantime $(TARGET_MAKETIME_H)

$(TARGET_MAKETIME_H):
	echo "#ifndef __$(TARGET_Name)_PROJECT_MAKETIME_H__" >  $(TARGET_MAKETIME_H)
	echo "#define __$(TARGET_Name)_PROJECT_MAKETIME_H__" >> $(TARGET_MAKETIME_H)
	echo " "                                             >> $(TARGET_MAKETIME_H)
	echo "#define $(TARGET_Name)_PROJECT_MAKETIME \"$(USERNAME)-$(ProgramVER) at $(MAKETIME)\"" >> $(TARGET_MAKETIME_H)
	echo " "                                             >> $(TARGET_MAKETIME_H)
	echo "#endif"                                        >> $(TARGET_MAKETIME_H)
# ================================================================================================================================
	
# 静态库
slib: $(STATIC_LIBNAME)

$(STATIC_LIBNAME): $(STATIC_OBJS)
	rm -f $(STATIC_LIBNAME)
	$(AR) -crs $@ $^
# 普通 .o 文件（用于静态库，不加 -fPIC）

# 静态库 .o 文件（不加 -fPIC，避免性能损失）
obj_static/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) -o  $@  $< -c $(CFLAGS) 
# ==============================================================================
# ---- 动态库构建 ----
# -shared: 生成共享库, -fPIC: 位置无关代码
dlib: $(SHARED_LIBNAME)

$(SHARED_LIBNAME): $(SHARED_OBJS)
	rm -f $(SHARED_LIBNAME)
	$(CC) -shared $(PIC_FLAGS) $(CFLAGS) -o $@ $^

# 动态库 .o 文件（必须加 -fPIC），放入 obj_shared/ 目录
obj_shared/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_FLAGS) $(CFLAGS) -c $< -o $@

# ==============================================================================
# ---- 可执行程序构建（链接静态库） ----
app: $(STATIC_LIBNAME) $(MAIN_OBJ)
	rm -f $(TARGET)
	$(CC) $(CFLAGS) -o $(TARGET) $(MAIN_OBJ) $(STATIC_LIBNAME) $(LDFLAGS)
	@chmod 755 $(TARGET)

# main.o 单独编译（测试主程序）
$(MAIN_OBJ): $(MAIN_SOURCE)
	$(CC) $(CFLAGS) -c -o $@ $<

# ==============================================================================

# ==============================================================================
# ---- 清理目标 ----
clean:
	rm -f *.bin -rf
	rm -f *.o -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
	rm -f $(STATIC_LIBNAME) $(SHARED_LIBNAME)
	rm -f $(TARGET)
	rm -f $(PROJECT_INFO)
# ==============================================================================
# 清理
cleantime:
	rm -rf $(TARGET_MAKETIME_H)	
	
# ==============================================================================	

# ---- 安装可执行文件到目标开发板（通过 scp） ----
install:app
	scp $(TARGET) $(INSTALLBOARD);
# ==============================================================================
# 安装库文件
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
