#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <stdint.h>

#include "log.h"

static std::string socket_address(const asio::ip::tcp::socket& socket)
{
    std::error_code ec;
    auto ed = socket.remote_endpoint(ec);
    if (ec)
    {
        LOG_ERROR << "socket remote endpoint failed " << ec.message();
        return "";
    }
    std::string address = ed.address().to_string(ec);
    if (ec)
    {
        LOG_ERROR << "socket remote address to string failed " << ec.message();
        return "";
    }
    uint16_t port = ed.port();

    return address + ":" + std::to_string(port);
}
class session : public std::enable_shared_from_this<session>
{
   public:
    session(asio::ip::tcp::socket socket) : socket_(std::move(socket)), remote_socket_(socket_.get_executor()) {}
    ~session() { LOG_DEBUG << "session finish"; }

    void go()
    {
        auto r = [self = shared_from_this()] { return self->do_handshake(); };
        asio::co_spawn(socket_.get_executor(), r, asio::detached);
    }
    asio::awaitable<void> do_handshake()
    {
        std::error_code ec;
        auto err = asio::redirect_error(asio::use_awaitable, ec);
        asio::ip::tcp::resolver resolver(socket_.get_executor());
        std::string addr = "127.0.0.1";
        uint16_t port = 1080;
        std::string host = "127.0.0.1:1080";
        auto const it = co_await resolver.async_resolve(addr, std::to_string(port), err);
        if (ec)
        {
            LOG_ERROR << "resulve remote " << host << " failed --> " << ec.message();
            co_return;
        }
        co_await asio::async_connect(remote_socket_, it.begin(), it.end(), err);
        if (ec)
        {
            LOG_ERROR << "connect remote " << host << " failed --> " << ec.category().name() << ":" << ec.message();
            co_return;
        }
        LOG_DEBUG << "connect remote " << host << " successful";
        forward();
    }
    void forward()
    {
        auto l = [this, self = shared_from_this()] { return do_forward(socket_, remote_socket_); };
        auto r = [this, self = shared_from_this()] { return do_forward(remote_socket_, socket_); };
        asio::co_spawn(socket_.get_executor(), l, asio::detached);
        asio::co_spawn(socket_.get_executor(), r, asio::detached);
    }

    asio::awaitable<void> do_forward(asio::ip::tcp::socket& src, asio::ip::tcp::socket& dst)
    {
        std::string src_address = socket_address(src);
        std::string dst_address = socket_address(dst);
        std::error_code ec;
        auto err = asio::redirect_error(asio::use_awaitable, ec);

        for (;;)
        {
            char data[32 * 1024] = {0};
            std::size_t n = co_await src.async_read_some(asio::buffer(data), err);
            if (ec)
            {
                LOG_ERROR << "read from " << src_address << " failed " << ec.message();
                break;
            }
            co_await asio::async_write(dst, asio::buffer(data, n), err);
            if (ec)
            {
                LOG_ERROR << "write to " << dst_address << " failed " << ec.message();
                break;
            }
        }
        LOG_DEBUG << src_address << " to " << dst_address << " finish";
        close();
    }

    void close()
    {
        std::error_code ignore;
        socket_.close(ignore);
        remote_socket_.close(ignore);
    }

   private:
    asio::ip::tcp::socket socket_;
    asio::ip::tcp::socket remote_socket_;
};
asio::awaitable<void> listener(asio::ip::tcp::acceptor acceptor)
{
    std::error_code ec;
    auto err = asio::redirect_error(asio::use_awaitable, ec);
    for (;;)
    {
        auto socket = co_await acceptor.async_accept(err);
        if (ec)
        {
            LOG_ERROR << "accept failed " << ec.message();
        }
        else
        {
            std::make_shared<session>(std::move(socket))->go();
        }
    }
}

int main(int argc, char* argv[])
{
    asio::io_context io_context(1);

    unsigned short port = 5555;
    co_spawn(io_context, listener(asio::ip::tcp::acceptor(io_context, {asio::ip::tcp::v4(), port})), asio::detached);
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { io_context.stop(); });

    io_context.run();

    return 0;
}
