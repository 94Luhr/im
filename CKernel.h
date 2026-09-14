#pragma once
#include"mediator/iNetMediator.h"
#include"net/def.h"
#include"MySQL/CMySql.h"
#include<iostream>
#include <chrono>
#include<map>
#include <mutex>
#include <string>
#include <unordered_map>
#include "net/socket_compat.h"
#include <cstdint>
using namespace std;
//声明指向处理函数的指针
class CKernel;
typedef void (CKernel::*PFUN)(char* data, int len, uintptr_t from);
using ConnectionId = uintptr_t;
class CKernel
{
public:
	CKernel();
	~CKernel();
	//函数指针数组 初始化 存数据
	void setProtocol();
	//打开服务器（打开网络 连接数据库）
	bool startServer();
	//关闭服务器（回收资源 关闭网络 断开数据库）
	void endSrever();
	//处理所有接收到的数据
	void dealData(char* data, int len, uintptr_t from);
	//处理注册请求
	void dealRegisterRq(char* data, int len, uintptr_t from);
	//处理登录请求
	void dealLoginRq(char* data, int len, uintptr_t from);
	//处理聊天请求
	void dealChatRq(char* data, int len, uintptr_t from);
	//处理下线请求
	void dealOfflineRq(char* data, int len, uintptr_t from);
	//处理添加好友请求
	void dealAddFriendRq(char* data, int len, uintptr_t from);
	//处理添加好友回复
	void dealAddFriendRs(char* data, int len, uintptr_t from);
	// 处理心跳请求并向当前连接回复。
	void dealHeartbeatRq(char* data, int len, uintptr_t from);
	// 处理消息送达确认。
	void dealMessageAck(char* data, int len, uintptr_t from);
	// 处理聊天历史分页请求。
	void dealHistoryRq(char* data, int len, uintptr_t from);
	// 处理好友申请结果送达确认。
	void dealFriendResultAck(char* data, int len, uintptr_t from);
	static CKernel* pKernel;
	//获取当前登录用户的信息 以及好友的信息
	void getUserInfoAndFriendInfo(int userId);
	//根据id查询用户信息(查到的用户信息作为输出参数返回STRU_FRIEND_INFO* info)
	void getInfoById(int id, STRU_FRIEND_INFO* info);
	// 登录成功后补发尚未确认送达的消息。
	void deliverPendingMessages(int userId, ConnectionId connectionId);
	// 用户登录后重新投递尚未处理的好友申请。
	void deliverPendingFriendRequests(int userId, ConnectionId connectionId);
	// 用户登录后补发尚未确认送达的好友申请处理结果。
	void deliverPendingFriendResults(int userId, ConnectionId connectionId);
	// 根据用户 ID 获取在线用户对应的 Socket。
	bool getUserSocket(int userId, ConnectionId& connectionId);
	// 根据连接反查已经通过登录认证的用户。
	bool getSocketUserId(ConnectionId connectionId, int& userId);
	// 处理客户端连接断开事件。
	void dealDisconnect(ConnectionId connectionId);
private:
	struct LoginAttempt
	{
		int failures = 0;
		std::chrono::steady_clock::time_point blockedUntil{};
		std::chrono::steady_clock::time_point lastSeen{};
	};

	// 登录限流使用手机号摘要作为键，内存中也不保存原始手机号。
	bool isLoginAllowed(const std::string& accountKey, int& retryAfterSeconds);
	bool recordLoginFailure(const std::string& accountKey);
	void clearLoginFailures(const std::string& accountKey);

	INetMediator* m_pMediator;
	CMySql m_sql;
	//声明函数指针数组
	PFUN m_protocol[DEF_PROTOCOL_COUNT];
	//保存登陆成功用户的socket（用户下线删除对应的socket）
	map<int, ConnectionId> m_mapIdSocket;
	map<ConnectionId, int> m_mapSocketId;
	// 同时维护用户到连接、连接到用户的双向映射。
	// 登录、聊天、下线和断线处理可能同时访问该 Map。
	std::mutex m_userSocketMutex;
	std::unordered_map<std::string, LoginAttempt> m_loginAttempts;
	std::mutex m_loginAttemptsMutex;
};

