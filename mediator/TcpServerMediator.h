#pragma once

#include "iNetMediator.h"

class TcpServerMediator : public INetMediator {
public:
	TcpServerMediator();
	~TcpServerMediator() override;

	bool openNet() override;
	void closeNet() override;
	bool sendData(char* data, int len, uintptr_t to) override;
	// 将完整协议包交给核心业务层。
	void transmitData(char* data, int len, uintptr_t from) override;
	// 将唯一连接编号对应的断线事件交给核心业务层。
	void onDisconnected(uintptr_t connectionId) override;
};
