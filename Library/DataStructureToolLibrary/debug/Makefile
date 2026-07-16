# ==============================================================================
# 工具链配置
# ==============================================================================
CC = arm-linux-gnueabihf-gcc
CXX = arm-linux-gnueabihf-g++ 
AR = arm-linux-gnueabihf-ar
#CC  = /opt/tool/5718_3358_sdk08_devkit/sysroots/x86_64-arago-linux/usr/bin/arm-none-linux-gnueabihf-gcc 
#CXX = /opt/tool/5718_3358_sdk08_devkit/sysroots/x86_64-arago-linux/usr/bin/arm-none-linux-gnueabihf-g++ 
#AR  = /opt/tool/5718_3358_sdk08_devkit/sysroots/x86_64-arago-linux/usr/bin/arm-none-linux-gnueabihf-ar  
#CC = gcc
#CXX = g++ 
#AR = ar
# ==============================================================================
# 项目配置
# ==============================================================================
TARGET_Name        = DataStructureToolLibrary
ProgramVER         = 1.0.0
USERNAME           = zlzksrl
MAKETIME           = $(shell date "+%G.%m.%d-%H.%M.%S")
# 本 Makefile 位于 <root>/Library/DataStructureToolLibrary/debug/，向上 3 层为工程根目录
#ROOT_DIR           = /opt/pipe/A-1000i/shutan/Program_Ver2/14_A-1000i_SensorWeldLayerParamPlan/10_DataStructureToolLibrary
ROOT_DIR			= ../../..

# 安装目录：Library/DataStructureToolLibrary/{StaticLib,sharedLib,include}
#INSTALL_PATH       = /opt/pipe/A-1000i/shutan/Program_Ver2/14_A-1000i_SensorWeldLayerParamPlan/10_DataStructureToolLibrary/Library/DataStructureToolLibrary
INSTALL_PATH        = ../
# ==============================================================================
# 子工程列表
# ------------------------------------------------------------------------------
# 顺序要求：AllPro / install_AllPro 会按此顺序调用各子工程 Makefile，
# 因此需要按依赖顺序排列 —— 被依赖的工程排在前面。
# 当前依赖关系：
#   KernelLinkedList  → 无
#   ThreadQueue       → 无
#   StreamBuffer      → 无
#   WindowQueue       → 无
#   RedBlackTree      → KernelLinkedList
#   MemoryPool        → ThreadQueue
#   ThreadManage      → KernelLinkedList / RedBlackTree / ThreadQueue
#   SoftTimer         → ThreadManage
#   FileWriter        → ThreadQueue / StreamBuffer / ThreadManage
#
# 新增子工程时：在正确的依赖位置追加工程名即可。
# ==============================================================================
SUBPROJECTS = KernelLinkedList  \
              ThreadQueue       \
              StreamBuffer      \
              WindowQueue       \
              RedBlackTree      \
              MemoryPool        \
              ThreadManage      \
              FileWriter        \
              SoftTimer         

# 传给各子工程 Makefile 的 LIBFILE_PATH（指向本地 Library/ 目录的绝对路径），
# 子工程 Makefile 中用 ?= 声明，因此可被外部覆盖。
LIB_LOCAL_PATH = $(realpath $(INSTALL_PATH)/..)

# ==============================================================================
# 源文件与头文件（自动收集所有子工程的 src/*.c 和 include/*.h）
# ==============================================================================
SOURCES  = $(foreach p,$(SUBPROJECTS),$(wildcard $(ROOT_DIR)/$(p)/src/*.c))
HEADERS  = $(foreach p,$(SUBPROJECTS),$(wildcard $(ROOT_DIR)/$(p)/include/*.h))

# 各子工程 include/ 与 src/（src 目录用于内部私有头 *_Main.h 等）
INC_FLAGS = $(foreach p,$(SUBPROJECTS),-I $(ROOT_DIR)/$(p)/include) \
            $(foreach p,$(SUBPROJECTS),-I $(ROOT_DIR)/$(p)/src)

# ==============================================================================
# 编译选项
# ==============================================================================
CFLAGS  = -g -Wall -Wextra -std=gnu11
CFLAGS := $(CFLAGS) -pthread
CFLAGS := $(CFLAGS) $(INC_FLAGS)

PIC_FLAGS = -fPIC

# 系统库：pthread（多模块使用）+ rt（clock_gettime）
LDFLAGS = -pthread -lrt

# ==============================================================================
# 目标文件（分离静态/动态，避免 -fPIC 混用）
# ==============================================================================
STATIC_OBJS = $(patsubst $(ROOT_DIR)/%.c,obj_static/%.o,$(SOURCES))
SHARED_OBJS = $(patsubst $(ROOT_DIR)/%.c,obj_shared/%.o,$(SOURCES))

# ==============================================================================
# 输出文件
# ==============================================================================
STATIC_LIBNAME = lib$(TARGET_Name).a
SHARED_LIBNAME = lib$(TARGET_Name).so
PROJECT_INFO   = ./ProjectInfo.txt

MAKETIME       = $(shell date "+%G.%m.%d-%H.%M.%S")

# ==============================================================================
# 伪目标
# ==============================================================================
.PHONY: all clean slib dlib install install_lib AllPro clean_AllPro install_AllPro

# ---- 默认目标：编译 + 安装到 Library/DataStructureToolLibrary/ ----
all: slib dlib install_lib
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(STATIC_LIBNAME)" >  $(PROJECT_INFO)
	md5sum $(STATIC_LIBNAME) >> $(PROJECT_INFO)
	echo "PROJECT_MAKETIME $(USERNAME)-$(ProgramVER) at $(MAKETIME) $(SHARED_LIBNAME)" >> $(PROJECT_INFO)
	md5sum $(SHARED_LIBNAME) >> $(PROJECT_INFO)
	cat $(PROJECT_INFO)

# ---- 静态库 ----
slib: $(STATIC_LIBNAME)

$(STATIC_LIBNAME): $(STATIC_OBJS)
	$(AR) -crs $@ $^

obj_static/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- 动态库 ----
dlib: $(SHARED_LIBNAME)

$(SHARED_LIBNAME): $(SHARED_OBJS)
	$(CC) -shared $(PIC_FLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj_shared/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_FLAGS) $(CFLAGS) -c -o $@ $<

# ---- 安装：拷贝库文件与所有子工程公共头到 Library/DataStructureToolLibrary/ ----
install_lib: slib dlib
	rm -rf $(INSTALL_PATH)/StaticLib $(INSTALL_PATH)/sharedLib $(INSTALL_PATH)/include
	mkdir -p $(INSTALL_PATH)/StaticLib
	mkdir -p $(INSTALL_PATH)/sharedLib
	mkdir -p $(INSTALL_PATH)/include
	cp $(STATIC_LIBNAME) $(INSTALL_PATH)/StaticLib/
	cp $(SHARED_LIBNAME) $(INSTALL_PATH)/sharedLib/
	cp $(HEADERS) $(INSTALL_PATH)/include/

# ---- 清理 ----
clean:
	rm -rf obj_static obj_shared
	rm -f $(STATIC_LIBNAME) $(SHARED_LIBNAME)
	rm -f $(PROJECT_INFO)

# ==============================================================================
# 一键操作各子工程：按 SUBPROJECTS 依赖顺序调用每个 <project>/debug/Makefile
#   AllPro          → 编译每个子工程（各自的默认目标 all）
#   install_AllPro  → 编译并把每个子工程的库/头文件安装到 Library/<project>/
#   clean_AllPro    → 清理每个子工程
# ==============================================================================
AllPro:
	@for p in $(SUBPROJECTS); do \
	    echo "==== [AllPro] $$p ===="; \
	    $(MAKE) -C $(ROOT_DIR)/$$p/debug all install_lib LIBFILE_PATH=$(LIB_LOCAL_PATH) CC=$(CC) CXX=$(CXX) AR=$(AR) || exit 1; \
	done

install_AllPro:
	@for p in $(SUBPROJECTS); do \
	    echo "==== [install_AllPro] $$p ===="; \
	    $(MAKE) -C $(ROOT_DIR)/$$p/debug install_lib LIBFILE_PATH=$(LIB_LOCAL_PATH) CC=$(CC) CXX=$(CXX) AR=$(AR)|| exit 1; \
	done

clean_AllPro:
	@for p in $(SUBPROJECTS); do \
	    echo "==== [clean_AllPro] $$p ===="; \
	    $(MAKE) -C $(ROOT_DIR)/$$p/debug clean LIBFILE_PATH=$(LIB_LOCAL_PATH) CC=$(CC) CXX=$(CXX) AR=$(AR)|| exit 1; \
	done
