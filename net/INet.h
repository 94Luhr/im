#pragma once
#include "socket_compat.h"
#include<iostream>
#include <atomic>
#include <cstdint>
#include"def.h"

using namespace std;
class INetMediator;
class INet
{
public:

	INet(): m_sock(INVALID_SOCKET),m_isRunning(true), m_pMediator(nullptr){

	}
	virtual ~INet() {

	}
	//初始化网络
	virtual bool initNet()=0;


	//发送数据(udp ip ulong类型决定发给谁 tcp socket uint 决定发给谁)
	//udp sendto(socket,buf,len,flag,to,tolen);
	//tcp send(socket,buf,len,flag);
	virtual bool sendData(char* data, int len, uintptr_t to) = 0;

	//接收数据(放在线程里)
	virtual void recvData() = 0;

	//关闭网络(关闭套接字，卸载库，回收线程资源)
	virtual void unInitNet() = 0;

protected:
	SOCKET m_sock;
	// 监听线程、接收线程和主线程都会访问该变量。
	// 使用原子变量避免多线程读写产生数据竞争。
	std::atomic_bool m_isRunning;
	INetMediator* m_pMediator;
};

