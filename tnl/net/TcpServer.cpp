#include <assert.h>
#include <functional>

#include "tnl/base/Logger.h"
#include "tnl/net/TcpServer.h"

using namespace tnl;
using namespace tnl::net;

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                const std::string& name, bool reusePort) :
    mLoop(loop),
    mIpPort(listenAddr.toIpPort()),
    mName(name),
    mStarted(-1),
    mAcceptor(new Acceptor(loop, listenAddr, reusePort)),
    mThreadPools(new EventLoopThreadPool(loop, name)),
    mNextConnId(1)
{
    mAcceptor->setNewConnectionCallback(
            std::bind(&TcpServer::newConnection,
                        this,
                        std::placeholders::_1,
                        std::placeholders::_2));
}

TcpServer::~TcpServer()
{
    assert(mLoop->isInLoopThread());

    for (auto& item : mConnections)
    {
        TcpConnectionPtr conn(item.second);
        item.second.reset();
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn));
    }
}

void TcpServer::setThreadNum(int numThreads)
{
    assert(numThreads >= 0);
    mThreadPools->setThreadNum(numThreads);
}

void TcpServer::start()
{
    if (++mStarted == 0)
    {        
        mThreadPools->start();
        assert(!mAcceptor->listenning());        
        mLoop->runInLoop(std::bind(&Acceptor::listen, mAcceptor.get()));
    }
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
    assert(mLoop->isInLoopThread());

    EventLoop* ioLoop = mThreadPools->getNextLoop();
    char buf[64];
    snprintf(buf, sizeof buf, "-%s#%d", mIpPort.c_str(), mNextConnId);
    ++mNextConnId;
    std::string connName = mName + buf;

    LOG_TRACE("TcpServer::newConnection [%s] - new connection [%s] from %s",
                mName.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());
    
    InetAddress localAddr(sockets::getLocalAddr(sockfd));
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr));

    mConnections[connName] = conn;
    
    if (mConnectionCallback)
        conn->setConnectionCallback(mConnectionCallback);
    if (mMessageCallback)
        conn->setMessageCallback(mMessageCallback);
    
    if (mWriteCompleteCallback)
        conn->setWriteCompleteCallback(mWriteCompleteCallback);
    
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection,
                            this, std::placeholders::_1));
    
    // 连接完成，开启读
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    mLoop->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    assert(mLoop->isInLoopThread());

    LOG_TRACE("TcpServer::removeConnectionInLoop [%s] - connection [%s]",
                mName.c_str(), conn->name().c_str());

    size_t n = mConnections.erase(conn->name());
    assert(n == 1);

    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
}