#include"TcpServerMediator.h"
#include"../net/EpollTcpServer.h"
#include"../CKernel.h"
TcpServerMediator::TcpServerMediator(){
	// Linux 服务端统一使用 epoll Reactor 实现，不再保留旧的一连接一线程模型。
	m_pNet = new EpollTcpServer(this);
}
TcpServerMediator::~TcpServerMediator(){
	if (m_pNet) {
		m_pNet->unInitNet();
		delete m_pNet;
		m_pNet = nullptr;
	}
}
//打开网络
bool TcpServerMediator::openNet() {
	return m_pNet->initNet();
}
//关闭网络
void TcpServerMediator::closeNet() {
	m_pNet->unInitNet();
}
//发送数据
bool TcpServerMediator::sendData(char* data, int len, uintptr_t to) {
	return m_pNet->sendData(data, len, to);
}
// 将网络层收到的完整协议包转交核心业务层。
void TcpServerMediator::transmitData(char* data, int len, uintptr_t from) {
	CKernel::pKernel->dealData(data, len, from);
}
// 网络层使用唯一连接编号通知核心层清理在线状态。
void TcpServerMediator::onDisconnected(uintptr_t connectionId)
{
	if (CKernel::pKernel != nullptr) {
		CKernel::pKernel->dealDisconnect(connectionId);
	}
}
