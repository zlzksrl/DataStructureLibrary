# 工具链
CC = arm-linux-gnueabihf-gcc
CXX = arm-linux-gnueabihf-g++
AR = arm-linux-gnueabihf-ar

# 项目配置
TARGET_Name        = ThreadQueue
ProgramVER         = 1.0.01N
USERNAME           = zlzksrl
LIBFILE_PATH       = ~/zlzksrl/LinuxARM/PublicLibrary/LinuxARM_IMX6ULL

# 安装路径
INSTALL_PATH    =  $(LIBFILE_PATH)/$(TARGET_Name)/
INSTALLBOARD       = root@192.168.1.6:/usr/zlzksrl/app/$(TARGET_Name)/

#库路径
#PUBLIC_LIBPATH     = -L $(LIBFILE_PATH)/lib/
#PUBLIC_LIBINCPATH  = -I $(LIBFILE_PATH)/include/

# 编译选项
CFLAGS  = -g
CFLAGS  := $(CFLAGS) #$(PUBLIC_LIBINCPATH)			#项目库头文件路径

PIC_FLAGS = -fPIC

LDFLAGS = -pthread
LDFLAGS := $(LDFLAGS) #$(PUBLIC_LIBPATH)

# 源文件（明确列出）
SOURCES     = ThreadQueue_Main.c
MAIN_SOURCE = main.c

# 目标文件
STATIC_OBJS = $(addprefix obj_static/,$(notdir $(SOURCES:.c=.o)))
SHARED_OBJS = $(addprefix obj_shared/,$(notdir $(SOURCES:.c=.o)))
MAIN_OBJ    = $(MAIN_SOURCE:.c=.o)

# 库文件
STATIC_LIBNAME = libThreadQueue.a
SHARED_LIBNAME = libThreadQueue.so

# 可执行文件
TARGET_DATE    = $(TARGET_Name)_DebugPro$(shell date "+%Y%m%d").bin
TARGET         = $(TARGET_Name)_DebugPro.bin 

#自动生成的头文件
TARGET_MAKETIME_H  = ./../src/$(TARGET_Name)_Maketime.h

#自动生成的工程信息
PROJECT_INFO     = ./ProjectInfo.txt


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
.PHONY: all clean install install_lib slib dlib app

all: slib dlib app
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(STATIC_LIBNAME)" >  $(PROJECT_INFO)
	md5sum $(STATIC_LIBNAME) >> $(PROJECT_INFO)
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(SHARED_LIBNAME)" >> $(PROJECT_INFO)
	md5sum $(SHARED_LIBNAME) >> $(PROJECT_INFO)
	cat $(PROJECT_INFO)
	rm -f *.o  -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
# ==============================================================================
	
# 静态库
slib: $(STATIC_LIBNAME)

$(STATIC_LIBNAME): $(STATIC_OBJS)
	$(AR) -crs $@ $^

# 普通 .o 文件（用于静态库，不加 -fPIC）


obj_static/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) -o  $@  $< -c $(CFLAGS) 
# ==============================================================================
# 动态库
dlib: $(SHARED_LIBNAME)

$(SHARED_LIBNAME): $(SHARED_OBJS)
	$(CC) -shared $(PIC_FLAGS) $(CFLAGS) -o $@ $^
	
# 带 fPIC 的 .o 文件，放入 obj_shared/
obj_shared/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_FLAGS) $(CFLAGS) -c $< -o $@

# ==============================================================================
# 可执行程序（链接静态库）
app: $(STATIC_LIBNAME) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(MAIN_OBJ) $(STATIC_LIBNAME) $(LDFLAGS)
	@chmod 755 $(TARGET)

# main.o 单独编译
$(MAIN_OBJ): $(MAIN_SOURCE)

# ==============================================================================
# 清理
clean:
	rm -f *.bin  -rf
	rm -f *.o  -rf
	rm -f obj_shared/*.o obj_shared/ -rf
	rm -f obj_static/*.o obj_static/ -rf
	rm -f $(STATIC_LIBNAME) $(SHARED_LIBNAME)
	rm -f $(TARGET)
	
# 安装可执行文件
install:app
	scp $(TARGET) $(INSTALLBOARD);

# 安装库文件
install_lib:slib dlib
	rm $(INSTALL_PATH) -rf
	mkdir -p $(INSTALL_PATH)
	mkdir -p $(INSTALL_PATH)include/
	mkdir -p $(INSTALL_PATH)lib/
	cp $(STATIC_LIBNAME) $(INSTALL_PATH)lib/
	cp $(SHARED_LIBNAME) $(INSTALL_PATH)lib/
	cp ../include/* $(INSTALL_PATH)include/
	cp $(PROJECT_INFO) $(INSTALL_PATH)
