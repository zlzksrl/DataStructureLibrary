/** 
 * @file            ProjectCommon.h 
 * @brief           项目公共头文件
 * @details         1、只能定义基本公共结构体
 *                  2、数据传输相关的不能定义在本文件
 * @author          shutan 
 * @version         v1.0.1 
 * @date            2025-01-02 
 * @copyright       Copyright (C) 2025
 */

/** 
 * @date             2025-01-02 
 * @version          v1.0.1 
 * @brief            创建文件
 * @author           shutan
 */


#ifndef __ProjectCommon_H__
#define __ProjectCommon_H__

#ifdef __cplusplus
extern "C"{
#endif

/***************模块程序接口相关**********************************************************************************************/
   

//开关状态
typedef enum 
{
    eStatus_Fail    = -1,   //失败
    eStatus_OFF     = 0,    //关闭
    eStatus_ON      = 1,    //打开状态
    ewitchStatus_Max,
}E_SwitchStatus;


//一般用于回调函数，或者功能状态
typedef enum 
{ 
    eTaskSignal_Failure            ,   // 任务其他原因失败 
    eTaskSignal_Timeout            ,   // 超时
    eTaskSignal_Interruption       ,   // 任务执被中端，新的任务来临 
    eTaskSignal_Finish             ,   // 任务执行完毕 
    eTaskSignal_Max                ,
}E_TaskSignal;

//旋转方向
typedef enum 
{
    eRotaionDirection_Cw   = 0,      //顺时针，AC
    eRotaionDirection_Ccw,          //逆时针，BD
    eRotaionDirection_Max,
}E_RotaionDirection;

//一般用于返回值
typedef enum 
{ 
    eRetvalSignal_Failure      = -1  ,   // 失败
    eRetvalSignal_Finish       = 0   ,   // 成功 
    eRetvalSignal_Max                ,
}E_RetvalSignal;


//调整值
typedef enum 
{
    eValue_Increase,    //值增加，
    eValue_Decrease,    //值减小
    eValueIncDec_Max,
}E_ValueIncDec;

//调整值
typedef enum 
{
    eMoveDir_Backward   = -1,   //负方向-向上,收回方向,反向行走方向----减小
    eMoveDir_Stop       = 0,    //停止
    eMoveDir_Forward    = 1,    //正方向-向下,伸出方向,焊接行走方向----增加
    eMoveDir_Max,
}E_MoveDirection;


//模块的在线状态
typedef enum  
{
    eDeviceStatus_Offline,    //设备掉线
    eDeviceStatus_ErrOnline,  //设备错误在线
    eDeviceStatus_Online,     //设备在线
    DeviceStatus_Max,
}E_DeviceOnlineStatus;//在线状态


typedef enum 
{
    eLanguage_Chinese    = 0x00,
    eLanguage_English    = 0x01,
    eLanguage_Russian    = 0x02,
    eLanguage_Max,
}E_Language;//语言类型


typedef enum 
{  
    eKeyStatus_Up      = 0,        //弹起
    eKeyStatus_Down    = 1,        //按下
    eKeyStatus_Hold    = 2,        //长按
    eKeyStatus_Max,
}E_KeyStatus;//按键按下状态


typedef struct
{
    int iKeyId;
    E_KeyStatus eKeyStatus;
}T_KeyValue;//键值数据

typedef struct
{
    float fVoltage;
    float fCurrent;
}T_CurVolData;

typedef struct
{
    float fAxis_Rho;     //ρ 杆伸方向-高度
    float fAxis_Theta;   //θ 行走方向-角度-距离
    float fAxis_Zeta;    //ζ 左右摆动方向-宽度
}T_Coordinates;


typedef struct
{
    int iAngle_x;
    int iAngle_y;
    int iAngle_z;
}T_AngleData;


typedef struct
{
    float fMinValue;
    float fMaxValue;
    float fSetValue; 
}T_fRangeParam;

typedef struct
{
    int iMinValue;
    int iMaxValue;
    int iSetValue; 
}T_iRangeParam;




typedef struct
{
    E_SwitchStatus bLogSwitch;       //日志记录开关
    int  iFlushTime;                 //日志I/O时间  ,单位:秒
    int  iLogSet;                    //根据程序不同增加的自定义设置-暂未使用
    char sLogFilePath[64];           //日志的保存路径
    char sLogFileName[64];           //日志的名称
}T_LogConfigParam;


typedef struct
{
    char sClientName[32];       //MQTT 连接名字
    char sBrokerIP[32];         //MQTT 代理端IP
    unsigned int iBrokerPort;   //MQTT 端口号
    char sUserName[32];          //用户名
    char sUserPasswd[32];        //密码
    int iKeepLive;               //心跳
    int iQos;                    //连接等级
}T_MqttClientLinkConfigParam;





#ifdef __cplusplus
}
#endif


#endif 

