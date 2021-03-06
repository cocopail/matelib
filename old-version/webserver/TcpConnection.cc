// Author Fangping Liu
// 2019-09-04

#include "base/Logging.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "TcpServer.h"
#include <functional>
#include <netinet/tcp.h> //TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>

using namespace lfp;

TcpConnection::TcpConnection(TcpServer* server,
							  EventLoop* loop,
							  int sockfd,
							  const std::string& name,
							  const InetAddress& localAddr,
							  const InetAddress& peerAddr)
  : connected_(false),
  	needDisconn_(false),
  	server_(server),
	loop_(loop),
	mutex_(),
	sockfd_(sockfd),
	channel_(loop, sockfd_),
	name_(name),
	localAddr_(localAddr),
	peerAddr_(peerAddr)
{
	//设置channel各事件回调函数
	channel_.setReadCallback(
			std::bind(&TcpConnection::handleRead, this));
	channel_.setWriteCallback(
			std::bind(&TcpConnection::handleWrite, this));
	channel_.setCloseCallback(
			std::bind(&TcpConnection::handleClose, this));
	channel_.setErrorCallback(
			std::bind(&TcpConnection::handleError, this));

	int on = 1;
	//TCP keepalive是指定期探测连接是否存在，如果应用层有心跳的话，这个选项不是必需要设置的
	::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
	//禁用Nagle算法，可以避免连续发包出现网络延迟（Nagle算法可以一定程度上避免网络拥塞）
	::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

TcpConnection::~TcpConnection()
{
	::close(sockfd_);
}

void TcpConnection::send(const void* data, size_t len)
{
	if (connected_)
	{
		if (loop_->isInLoopThread()) {
			sendInThread(data, len);
		}
		else {
			//如果是跨线程调用，这里会有内存拷贝对效率有损失
			std::string message(static_cast<const char*>(data), len);
			loop_->runInLoop(std::bind(&TcpConnection::sendInLoop,
										this,
										message));
		}
	}
}

void TcpConnection::send(const StringPiece& message)
{
	if (connected_)
	{
		if (loop_->isInLoopThread()) {
			sendInThread(message.data(), message.size());
		}
		else {
			//如果是跨线程调用，这里会有内存拷贝对效率有损失
			loop_->runInLoop(std::bind(&TcpConnection::sendInLoop,
										this,
										message.asString()));
		}
	}
}

void TcpConnection::send(Buffer* buffer)
{
	if (connected_)
	{
		if (loop_->isInLoopThread()) {
			sendInThread(buffer->peek(), buffer->readableBytes());
		}
		else {
			//如果是跨线程调用，这里会有内存拷贝对效率有损失
			loop_->runInLoop(std::bind(&TcpConnection::sendInLoop,
										this,
										buffer->retrieveAllAsString()));
		}
	}
}


void TcpConnection::sendInLoop(const StringPiece& message)
{
	sendInThread(message.data(), message.size());
}

void TcpConnection::sendInThread(const void* data, size_t len)
{
	loop_->assertInLoopThread();
	if (!connected_) {
		ASYNC_LOG << "disconnected, give up writing";
		return;
	}

	bool error = false;
	int nwrote = 0;
	size_t remaining = len;
	if (!channel_.isWriting() && outputBuffer_.readableBytes() == 0)
	{
		nwrote = ::write(sockfd_, data, len);
		if (nwrote >= 0) {
			remaining = len - nwrote;
		}
		else {
			error = true;
			ASYNC_LOG << "TcpConnection::sendInLoop() ::write error";
		}
	}

	if (!error && remaining > 0) {
		ASYNC_LOG << "Kernel send buffer is full";

		outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
		if (!channel_.isWriting()) {
			channel_.enableWriting(); //关注通道可写事件
		}
	}
}


//只需调用一次
void TcpConnection::shutdown()
{
	if (!needDisconn_) {
		needDisconn_ = true;
		loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
	}
}

void TcpConnection::shutdownInLoop()
{
	loop_->isInLoopThread();

	if (!channel_.isWriting()) { //如果发送缓冲区还有数据则不能立即关闭
		connected_ = false;
		::shutdown(sockfd_, SHUT_WR);
	}
}

//在连接建立完成时被调用
void TcpConnection::connectionEstablished()
{
	loop_->assertInLoopThread();

	connected_ = true;
	channel_.enableReading();
	connectionCallback_(shared_from_this());	//连接建立完成，调用用户连接回调函数
}

//在连接断开后被调用
void TcpConnection::connectionDestroyed()
{
	loop_->assertInLoopThread();
	channel_.disableReading();
	connectionCallback_(shared_from_this());	//连接断开，调用用户连接回调函数
	channel_.remove();
}

//消息到来时被channel调用
void TcpConnection::handleRead()
{
	loop_->assertInLoopThread();
	int savedErrno = 0;
	ssize_t n = inputBuffer_.readFd(channel_.fd(), &savedErrno);
	if (n > 0) {
		Timestamp now(Timestamp::now());
		messageCallback_(shared_from_this(), &inputBuffer_, now);
	}
	else if(n == 0) {
		handleClose();
	}
	else {
		ASYNC_LOG << "TcpConnection::handleRead() ::readv error";
		handleError();
	}
}

void TcpConnection::handleWrite()
{
	if (channel_.isWriting())
	{
		ssize_t n = ::write(channel_.fd(),
							outputBuffer_.peek(),
							outputBuffer_.readableBytes());
		if (n > 0)
		{
			outputBuffer_.retrieve(n);
			if (outputBuffer_.readableBytes() == 0) { //发送缓冲区清空
				channel_.disableWriting();	//缓冲区清空后要取消关注当前通道的可写事件
				if (needDisconn_) {			//如果用户想关闭，则在数据发送完成后关闭
					shutdownInLoop();
				}
			}
			else {
				ASYNC_LOG << "application layer send buffer is empty";
			}
		}
		else {
			ASYNC_LOG << "TcpConnection::handleWrite() ::write error";
		}
	}
}

void TcpConnection::handleClose()
{
	connected_ = false;
	channel_.disableAll();

	//向TcpServer注册一个回调函数，删除当前TcpConnection对象（也可以通过两次回调实现）
	server_->getMainLoop()->runInLoop(std::bind(&TcpServer::removeConnection,
												 server_,
												 shared_from_this()));
}

void TcpConnection::handleError()
{
	int err; socklen_t len = sizeof err;
	::getsockopt(channel_.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
	ASYNC_LOG << "TcpConnection::handleError() SO_ERROR=" << err << ": " << strerror_tl(err);
}