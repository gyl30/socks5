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
    session(asio::ip::tcp::socket socket)
        : local_socket_(std::move(socket)), remote_socket_(local_socket_.get_executor())
    {
    }
    ~session() {}

    void go() { handshake(); }

   private:
    void handshake()
    {
        auto fn = [this, self = shared_from_this()] { return do_handshake(); };
        asio::co_spawn(local_socket_.get_executor(), fn, asio::detached);
    }

    auto completion_handler(uint32_t minimum_read)
    {
        return [minimum_read](auto& ec, auto bytes_transferred) -> std::size_t
        {
            if (ec || bytes_transferred >= minimum_read)
            {
                return 0;
            }
            else
            {
                return minimum_read - bytes_transferred;
            }
        };
    }
    asio::awaitable<void> do_handshake()
    {
        std::error_code ec;
        auto err = asio::redirect_error(asio::use_awaitable, ec);

        int methods_size = 0;
        int version = 0;
        {
            std::vector<uint8_t> buffer(2, '\0');
            auto handler = completion_handler(2);
            co_await asio::async_read(local_socket_, asio::buffer(buffer.data(), buffer.size()), handler, err);
            if (ec)
            {
                LOG_ERROR << "handshake read version failed " << ec.message();
                co_return;
            }
            version = buffer[0];
            methods_size = buffer[1];
            LOG_DEBUG << "version " << version;
            LOG_DEBUG << "name methods " << methods_size;
        }

        {
            auto handler = completion_handler(methods_size);
            std::vector<uint8_t> buffer(methods_size, '\0');
            co_await asio::async_read(local_socket_, asio::buffer(buffer.data(), buffer.size()), handler, err);
            if (ec)
            {
                LOG_ERROR << "handshake read methods failed " << ec.message();
                co_return;
            }
            for (int i = 0; i < methods_size; i++)
            {
                LOG_DEBUG << "methods --> " << std::to_string(buffer[i]);
            }
        }
        {
            char v[] = {0x05, 0x00};
            co_await asio::async_write(local_socket_, asio::buffer(v), err);
            if (ec)
            {
                LOG_ERROR << "handshake write version failed " << ec.message();
                co_return;
            }
            LOG_DEBUG << "write version finished";
        }
        int cmd = 0;
        int atyp = 0;
        {
            auto handler = completion_handler(4);
            std::vector<uint8_t> buffer(4, '\0');
            co_await asio::async_read(local_socket_, asio::buffer(buffer.data(), buffer.size()), handler, err);
            if (ec)
            {
                LOG_ERROR << "handshake read connect failed " << ec.message();
                co_return;
            }
            int version = buffer[0];
            cmd = buffer[1];
            int rsv = buffer[2];
            atyp = buffer[3];
            LOG_DEBUG << "connect version --> " << version;
            LOG_DEBUG << "connect cmd     --> " << cmd;
            LOG_DEBUG << "connect rsv     --> " << rsv;
            LOG_DEBUG << "connect atyp    --> " << atyp;
            if (cmd != 1)
            {
                LOG_ERROR << "only support connect on ipv4";
                co_return;
            }
        }
        std::string addr;
        {
            if (atyp == 1)
            {
                char address[32] = {0};
                auto handler = completion_handler(4);
                std::vector<uint8_t> buffer(4, '\0');
                co_await asio::async_read(local_socket_, asio::buffer(buffer.data(), buffer.size()), handler, err);
                if (ec)
                {
                    LOG_ERROR << "handshake read connect failed " << ec.message();
                    co_return;
                }
                sprintf(address, "%d.%d.%d.%d", buffer[0], buffer[1], buffer[2], buffer[3]);
                addr = address;
            }
            else if (atyp == 3)
            {
                auto handler = completion_handler(1);
                std::vector<uint8_t> buffer(1, '\0');
                co_await asio::async_read(local_socket_, asio::buffer(buffer.data(), buffer.size()), handler, err);
                if (ec)
                {
                    LOG_ERROR << "handshake read addr len failed " << ec.message();
                    co_return;
                }
                int address_len = buffer[0];
                LOG_DEBUG << "address_len " << address_len;
                auto address_handler = completion_handler(address_len);
                std::vector<uint8_t> address_buffer(address_len, '\0');
                co_await asio::async_read(local_socket_, asio::buffer(address_buffer.data(), address_buffer.size()),
                                          address_handler, err);
                if (ec)
                {
                    LOG_ERROR << "handshake read addr failed " << ec.message();
                    co_return;
                }
                addr = std::string(address_buffer.begin(), address_buffer.end());
            }
            else
            {
                LOG_ERROR << "not support address type " << atyp;
                co_return;
            }
        }
        LOG_DEBUG << "connect addr    --> " << addr;
        uint16_t port = 0;
        {
            auto handler = completion_handler(sizeof(port));
            co_await asio::async_read(local_socket_, asio::buffer(&port, sizeof(port)), handler, err);
            if (ec)
            {
                LOG_ERROR << "handshake read connect failed " << ec.message();
                co_return;
            }
            char addr[32] = {0};
            port = ntohs(port);
            LOG_DEBUG << "connect port    --> " << port;
        }
        // connect remote
        {
            asio::ip::tcp::resolver resolver(local_socket_.get_executor());
            std::string host = addr;
            host += ":" + std::to_string(port);
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

            LOG_DEBUG << "local " << host << " connect remote " << socket_address(remote_socket_) << " successful";
        }
        {
            char buff[] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
            co_await asio::async_write(local_socket_, asio::buffer(buff), err);
            if (ec)
            {
                LOG_ERROR << " handshake write connect failed " << ec.message();
                co_return;
            }
            LOG_DEBUG << "write connect finished";
        }
        forward();
    }
    void forward()
    {
        auto l = [this, self = shared_from_this()] { return do_forward(local_socket_, remote_socket_); };
        auto r = [this, self = shared_from_this()] { return do_forward(remote_socket_, local_socket_); };
        asio::co_spawn(local_socket_.get_executor(), r, asio::detached);
        asio::co_spawn(local_socket_.get_executor(), l, asio::detached);
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
        local_socket_.close(ignore);
        remote_socket_.close(ignore);
    }

   private:
    asio::ip::tcp::socket local_socket_;
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
    uint16_t port = 5555;

    asio::io_context io(1);
    LOG_DEBUG << "run at " << port;

    asio::co_spawn(io, listener(asio::ip::tcp::acceptor(io, {asio::ip::tcp::v4(), port})), asio::detached);

    asio::signal_set signals(io, SIGINT, SIGTERM);

    signals.async_wait([&](auto, auto) { io.stop(); });

    io.run();

    return 0;
}
