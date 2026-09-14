#pragma once
#include<string.h>
#include<cstdint>
//TCP协议端口号
#define DEF_TCP_PORT	(56789)
//昵称 手机 密码最大长度
#define DEF_MAX_LEN		(15)
//声明结果宏
//注册结果
#define def_register_success	(0)
#define def_register_telexists	(1)
#define def_register_nameexists	(2)
//登录结果
#define def_login_success			(0)
#define def_login_telnotexists		(1)
#define def_login_password_error	(2)
//添加好友结果
#define def_add_friend_success		(0)
#define def_add_friend_offline		(1)
#define def_add_friend_refuse		(2)
#define def_add_friend_notexists	(3)
// 好友申请已经持久化，等待对方上线处理。
#define def_add_friend_pending		(4)
//聊天内容最大长度
#define DEF_CONTENT_LEN		(8*1024)
// 单个 TCP 数据包允许的最大长度。
// 防止客户端伪造超大包长，导致程序申请过多内存。
#define DEF_MAX_PACKET_SIZE (1024 * 1024)
//发送结果
#define def_send_success		(0)
#define def_send_fail			(1)
//声明结构体类型宏
#define DEF_PROTOCOL_BASE		(1000)
//结构体宏的个数
#define DEF_PROTOCOL_COUNT		(18)
//注册请求
#define DEF_REGISTER_RQ			(DEF_PROTOCOL_BASE+1)
//注册回复
#define DEF_REGISTER_RS			(DEF_PROTOCOL_BASE+2)
//登录请求
#define DEF_LOGIN_RQ			(DEF_PROTOCOL_BASE+3)
//登录回复
#define DEF_LOGIN_RS			(DEF_PROTOCOL_BASE+4)
//添加好友请求
#define DEF_ADD_FRIEND_RQ		(DEF_PROTOCOL_BASE+5)
//添加好友回复
#define DEF_ADD_FRIEND_RS		(DEF_PROTOCOL_BASE+6)
//聊天请求
#define DEF_CHAT_RQ				(DEF_PROTOCOL_BASE+7)
//聊天回复
#define DEF_CHAT_RS				(DEF_PROTOCOL_BASE+8)
//下线请求
#define DEF_OFFLINE_RQ			(DEF_PROTOCOL_BASE+9)
//用户信息
#define DEF_FRIEND_INFO			(DEF_PROTOCOL_BASE+10)
//心跳请求
#define DEF_HEARTBEAT_RQ        (DEF_PROTOCOL_BASE+11)
//心跳回复
#define DEF_HEARTBEAT_RS        (DEF_PROTOCOL_BASE+12)
//服务端投递的聊天消息
#define DEF_CHAT_MESSAGE        (DEF_PROTOCOL_BASE+13)
//消息送达确认
#define DEF_MESSAGE_ACK         (DEF_PROTOCOL_BASE+14)
//聊天历史请求
#define DEF_HISTORY_RQ          (DEF_PROTOCOL_BASE+15)
//聊天历史一页发送完毕
#define DEF_HISTORY_END         (DEF_PROTOCOL_BASE+16)
//好友申请处理结果通知
#define DEF_FRIEND_RESULT_NOTIFY (DEF_PROTOCOL_BASE+17)
//好友申请处理结果送达确认
#define DEF_FRIEND_RESULT_ACK    (DEF_PROTOCOL_BASE+18)
//用户线状态
#define def_status_online       (0)
#define def_status_offline       (1)


//声明结构体类型变量
typedef int packtype;




//请求结构体
//注册请求 电话 密码 昵称
typedef struct STRU_REGISTER_RQ {
    STRU_REGISTER_RQ() : type(DEF_REGISTER_RQ)
    {
        memset(tel, 0, DEF_MAX_LEN);
        memset(password, 0, DEF_MAX_LEN);
        memset(name, 0, DEF_MAX_LEN);
    }
    packtype type;
    char tel[DEF_MAX_LEN];
    char password[DEF_MAX_LEN];
    char name[DEF_MAX_LEN];
} STRU_REGISTER_RQ;
//注册回复 结果（成功 失败（电话号码已被注册 昵称已被注册 ））
typedef struct STRU_REGISTER_RS
{
    STRU_REGISTER_RS() : type(DEF_REGISTER_RS), result(def_register_telexists) {}
    packtype type;
    int result;
}STRU_REGISTER_RS;

//登录请求
typedef struct STRU_LOGIN_RQ {
    STRU_LOGIN_RQ() :type(DEF_LOGIN_RQ)
    {
        memset(tel, 0, DEF_MAX_LEN);
        memset(password, 0, DEF_MAX_LEN);
    }
    packtype type;
    char tel[DEF_MAX_LEN];
    char password[DEF_MAX_LEN];

} STRU_LOGIN_RQ;

//登录回复(成功 失败（电话号码未注册 密码错误 ） )
typedef struct STRU_LOGIN_RS
{
    STRU_LOGIN_RS() : type(DEF_LOGIN_RS), userId(0), result(def_login_password_error) {}
    packtype type;
    int userId;
    int result;
}STRU_LOGIN_RS;
//添加好友请求 好友昵称 自己的id 自己的昵称
typedef struct STRU_ADD_FRIEND_RQ {
    STRU_ADD_FRIEND_RQ() : type(DEF_ADD_FRIEND_RQ), userId(0)
    {
        memset(friendName, 0, DEF_MAX_LEN);
        memset(userName, 0, DEF_MAX_LEN);
    }
    packtype type;
    int userId;
    char userName[DEF_MAX_LEN];
    char friendName[DEF_MAX_LEN];
} STRU_ADD_FRIEND_RQ;

//添加好友回复(成功 失败（用户不存在 拒绝 不在线 ）)
typedef struct STRU_ADD_FRIEND_RS {
    STRU_ADD_FRIEND_RS() : type(DEF_ADD_FRIEND_RS), result(def_add_friend_refuse), userId(0), friendId(0)
    {
        memset(friendName, 0, DEF_MAX_LEN);
        memset(userName, 0, DEF_MAX_LEN);
    }
    packtype type;
    int result;
    int userId;
    int friendId;
    char userName[DEF_MAX_LEN];
    char friendName[DEF_MAX_LEN];
} STRU_ADD_FRIEND_RS;

//聊天请求(聊天内容 自己id 好友id)
typedef struct STRU_CHAT_RQ {
    STRU_CHAT_RQ() : type(DEF_CHAT_RQ), userId(0), friendId(0)
    {
        memset(content, 0, DEF_CONTENT_LEN);
    }
    packtype type;
    int userId;
    int friendId;
    char content[DEF_CONTENT_LEN];
} STRU_CHAT_RQ;

//聊天回复(成功 失败)
typedef struct STRU_CHAT_RS {
    STRU_CHAT_RS() : type(DEF_CHAT_RS), friendId(0), result(def_send_fail)
    {
    }
    packtype type;
    int friendId;
    int result;
} STRU_CHAT_RS;
//下线请求 （自己id ）
typedef struct STRU_OFFLINE_RQ {
    STRU_OFFLINE_RQ() : type(DEF_OFFLINE_RQ), userId(0)
    {
    }
    packtype type;
    int userId;
} STRU_OFFLINE_RQ;
//用户信息 type id 昵称 签名 头像id 状态
typedef struct STRU_FRIEND_INFO {
    STRU_FRIEND_INFO() :type(DEF_FRIEND_INFO), id(0), iconId(0), status(def_status_offline)
    {
        memset(name, 0, DEF_MAX_LEN);
        memset(feeling, 0, DEF_MAX_LEN);
    }
    packtype type;
    int id;
    int iconId;
    int status;
    char name[DEF_MAX_LEN];
    char feeling[DEF_MAX_LEN];
} STRU_FRIEND_INFO;

// 心跳包不携带业务数据，只用于确认连接仍然可用。
typedef struct STRU_HEARTBEAT_RQ {
    STRU_HEARTBEAT_RQ() : type(DEF_HEARTBEAT_RQ) {}
    packtype type;
} STRU_HEARTBEAT_RQ;

typedef struct STRU_HEARTBEAT_RS {
    STRU_HEARTBEAT_RS() : type(DEF_HEARTBEAT_RS) {}
    packtype type;
} STRU_HEARTBEAT_RS;

// 新增协议结构使用 1 字节对齐，避免 Linux 与 Windows 编译器布局不同。
#pragma pack(push, 1)
typedef struct STRU_CHAT_MESSAGE {
    STRU_CHAT_MESSAGE()
        : type(DEF_CHAT_MESSAGE), senderId(0), receiverId(0), source(0),
          messageId(0), createdAt(0)
    {
        memset(content, 0, DEF_CONTENT_LEN);
    }
    packtype type;
    int senderId;
    int receiverId;
    int source;             // 0：实时消息，1：登录补发，2：历史记录。
    uint64_t messageId;
    int64_t createdAt;      // Unix 秒级时间戳。
    char content[DEF_CONTENT_LEN];
} STRU_CHAT_MESSAGE;

typedef struct STRU_MESSAGE_ACK {
    STRU_MESSAGE_ACK() : type(DEF_MESSAGE_ACK), messageId(0) {}
    packtype type;
    uint64_t messageId;
} STRU_MESSAGE_ACK;

typedef struct STRU_HISTORY_RQ {
    STRU_HISTORY_RQ() : type(DEF_HISTORY_RQ), friendId(0), limit(30), beforeMessageId(0) {}
    packtype type;
    int friendId;
    int limit;
    uint64_t beforeMessageId; // 0 表示从最新消息开始查询。
} STRU_HISTORY_RQ;

typedef struct STRU_HISTORY_END {
    STRU_HISTORY_END()
        : type(DEF_HISTORY_END), friendId(0), count(0), hasMore(0), oldestMessageId(0) {}
    packtype type;
    int friendId;
    int count;
    int hasMore;
    uint64_t oldestMessageId; // 下一页查询使用的游标。
} STRU_HISTORY_END;

typedef struct STRU_FRIEND_RESULT_NOTIFY {
    STRU_FRIEND_RESULT_NOTIFY()
        : type(DEF_FRIEND_RESULT_NOTIFY), requestId(0), result(def_add_friend_refuse),
          requesterId(0), targetId(0), handledAt(0)
    {
        memset(requesterName, 0, DEF_MAX_LEN);
        memset(targetName, 0, DEF_MAX_LEN);
    }
    packtype type;
    uint64_t requestId;
    int result;
    int requesterId;
    int targetId;
    int64_t handledAt;
    char requesterName[DEF_MAX_LEN];
    char targetName[DEF_MAX_LEN];
} STRU_FRIEND_RESULT_NOTIFY;

typedef struct STRU_FRIEND_RESULT_ACK {
    STRU_FRIEND_RESULT_ACK() : type(DEF_FRIEND_RESULT_ACK), requestId(0) {}
    packtype type;
    uint64_t requestId;
} STRU_FRIEND_RESULT_ACK;
#pragma pack(pop)

// 编译期检查跨平台协议布局，避免编译器对齐差异导致收发错位。
static_assert(sizeof(STRU_CHAT_MESSAGE) == 8224, "STRU_CHAT_MESSAGE 协议布局错误");
static_assert(sizeof(STRU_MESSAGE_ACK) == 12, "STRU_MESSAGE_ACK 协议布局错误");
static_assert(sizeof(STRU_HISTORY_RQ) == 20, "STRU_HISTORY_RQ 协议布局错误");
static_assert(sizeof(STRU_HISTORY_END) == 24, "STRU_HISTORY_END 协议布局错误");
static_assert(sizeof(STRU_FRIEND_RESULT_NOTIFY) == 62, "STRU_FRIEND_RESULT_NOTIFY 协议布局错误");
static_assert(sizeof(STRU_FRIEND_RESULT_ACK) == 12, "STRU_FRIEND_RESULT_ACK 协议布局错误");
