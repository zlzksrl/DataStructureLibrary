# 工具链
CC = arm-linux-gnueabihf-gcc

# 项目配置
TARGET_Name        = KernelLinkedList
ProgramVER         = 1.0.0
USERNAME           = zlzksrl
LIBFILE_PATH       = ~/zlzksrl/LinuxARM/PublicLibrary/LinuxARM_IMX6ULL

# 安装路径
INSTALL_PATH       = $(LIBFILE_PATH)/$(TARGET_Name)/
INSTALLBOARD       = root@192.168.1.6:/usr/zlzksrl/app/$(TARGET_Name)/

# 编译选项
CFLAGS  = -g -Wall -Wextra

# 源文件
MAIN_SOURCE = main.c

# 目标文件
MAIN_OBJ    = $(MAIN_SOURCE:.c=.o)

# 可执行文件
TARGET_DATE    = $(TARGET_Name)_DebugPro$(shell date "+%Y%m%d").bin
TARGET         = $(TARGET_Name)_DebugPro.bin

#自动生成的工程信息
PROJECT_INFO     = ./ProjectInfo.txt


# ==============================================================================
MAKETIME      = $(shell date "+%G.%m.%d-%H.%M.%S")

# ==============================================================================
# 伪目标
.PHONY: all clean install install_lib

all: app
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME)" >  $(PROJECT_INFO)
	md5sum $(TARGET) >> $(PROJECT_INFO)
	cat $(PROJECT_INFO)
	rm -f *.o  -rf
# ==============================================================================

# 可执行程序（纯头文件，无需链接库）
app: $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(MAIN_OBJ)
	@chmod 755 $(TARGET)

# main.o 编译
%.o: %.c ../include/KernelLinkedList.h
	$(CC) $(CFLAGS) -c $< -o $@

# ==============================================================================
# 清理
clean:
	rm -f *.bin
	rm -f *.o
	rm -f $(TARGET)
	rm -f $(PROJECT_INFO)
	
# 安装可执行文件
install:app
	scp $(TARGET) $(INSTALLBOARD)
	# 安装库文件

install_lib:
	rm $(INSTALL_PATH) -rf
	mkdir -p $(INSTALL_PATH)
	mkdir -p $(INSTALL_PATH)include/
	cp ../include/* $(INSTALL_PATH)include/
	cp $(PROJECT_INFO) $(INSTALL_PATH)
