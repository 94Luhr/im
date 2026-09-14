#pragma once
#include <cstdint>
//先声明有INet这个类 可以直接使用
class INet;
class INetMediator {
public:
	INetMediator():m_pNet(nullptr){}
	virtual ~INetMediator() = default;


	//打开网络
	virtual bool openNet() = 0;
	//关闭网络
	virtual void closeNet() = 0;
	//发送数据
	virtual bool sendData(char* data, int len, uintptr_t to) = 0;
	// 将完整协议包转发给核心业务层，from 是唯一连接编号。
	virtual void transmitData(char* data, int len, uintptr_t from) = 0;
	// 网络连接断开时，由网络层通知中介层。
	// connectionId 是网络层生成且不会复用的连接编号。
	virtual void onDisconnected(uintptr_t connectionId) = 0;
protected:
	INet* m_pNet;

};
	
