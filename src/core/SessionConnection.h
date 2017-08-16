#ifndef SESSION_CONNECTION_H__
#define SESSION_CONNECTION_H__

#include "common/Defs.h"

#include <mutex>
#include <deque>
#include <boost/asio.hpp>

namespace Interceptor {
  class SessionConnection : public
    std::enable_shared_from_this<SessionConnection> {
  public:
    SessionConnection(ParamsPtr params, boost::asio::io_service& ioService);
    ~SessionConnection();

    boost::asio::ip::tcp::socket& socket() const;
    const std::string ip() const;
    ParamsPtr params() const;
    void closeConnection();
    void asyncReadSome(char* buffer, size_t size,
                       std::function<void(const boost::system::error_code&, size_t bytesTransferred)>
                       callback);
    void postReply(BufferPtr buffer);


  private:
    enum State : uint8_t  {
      CanSend = 0x01,
      Reading = 0x02,
      Sending = 0x04,
      Closing = 0x08
    };
    enum TimerType : uint8_t {
      ReadTimer = 0x01,
      WriteTimer = 0x02
    };

  private:
    void startReadTimer();
    void startWriteTimer();
    void stopReadTimer();
    void stopWriteTimer();

    void handleTimeout(TimerType timerType, const boost::system::error_code& error);
    void handleTransmissionCompleted(BufferPtr httpBuffer,
                                     const boost::system::error_code& error, size_t bytesTransferred);

    void sendReply(BufferPtr buffer);
    void sendNext(BufferPtr buffer);
    void doCloseConnection();


  private:
    boost::asio::io_service& m_ioService;
    InboundConnectionPtr m_connection;
    ParamsPtr m_params;

    int m_state;

    std::deque<BufferPtr> m_buffers;
    std::mutex m_buffersMutex;

    boost::asio::strand m_ostrand;
	boost::asio::strand m_istrand;
    boost::asio::strand m_fsstrand;

    // Timers
    boost::asio::deadline_timer m_readTimer;
    boost::asio::deadline_timer m_writeTimer;
  };

}

#endif
