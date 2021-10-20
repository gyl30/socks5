#include <thread>
#include <memory>
#include <functional>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include "log.h"
static std::string socket_address(const boost::asio::ip::tcp::socket& socket)
{
    boost::system::error_code ec;
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
    session(boost::asio::ip::tcp::socket socket)
        : local_socket_(std::move(socket)), remote_socket_(socket.get_executor()), strand_(socket.get_executor())
    {
    }
    ~session() {}

    void go() { handshake(); }

   private:
    void handshake()
    {
        boost::asio::spawn(strand_, [this, self = shared_from_this()](auto yield) { do_handshake(yield); });
    }

    void forward()
    {
        // clang-format off
        auto local_to_remote = [this, self = shared_from_this()](auto yield) { do_forward(local_socket_, " --> ", remote_socket_, yield); };
        auto remote_to_local = [this, self = shared_from_this()](auto yield) { do_forward(remote_socket_, " <-- ", local_socket_, yield); };
        boost::asio::spawn(strand_, local_to_remote);
        boost::asio::spawn(strand_, remote_to_local);
        // clang-format on
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
    void do_handshake(boost::asio::yield_context yield)
    {
        int methods_size = 0;
        int version = 0;
        {
            std::vector<uint8_t> buffer(2, '\0');
            auto handler = completion_handler(2);
            boost::system::error_code ec;
            boost::asio::async_read(local_socket_, boost::asio::buffer(buffer.data(), buffer.size()), handler,
                                    yield[ec]);
            if (ec)
            {
                LOG_ERROR << "handshake read version failed " << ec.message();
                return;
            }
            version = buffer[0];
            methods_size = buffer[1];
            LOG_DEBUG << " version " << version;
            LOG_DEBUG << " name methods " << methods_size;
        }

        {
            auto handler = completion_handler(methods_size);
            boost::system::error_code ec;
            std::vector<uint8_t> buffer(methods_size, '\0');
            boost::asio::async_read(local_socket_, boost::asio::buffer(buffer.data(), buffer.size()), handler,
                                    yield[ec]);
            if (ec)
            {
                LOG_ERROR << "handshake read methods failed " << ec.message();
                return;
            }
            for (int i = 0; i < methods_size; i++)
            {
                LOG_DEBUG << "methods --> " << std::to_string(buffer[i]);
            }
        }
        {
            boost::system::error_code ec;
            char v[] = {0x05, 0x00};
            boost::asio::async_write(local_socket_, boost::asio::buffer(v), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "handshake write version failed " << ec.message();
                return;
            }
            LOG_DEBUG << "write version finished";
        }
        int cmd = 0;
        int atyp = 0;
        {
            auto handler = completion_handler(4);
            boost::system::error_code ec;
            std::vector<uint8_t> buffer(4, '\0');
            boost::asio::async_read(local_socket_, boost::asio::buffer(buffer.data(), buffer.size()), handler,
                                    yield[ec]);
            if (ec)
            {
                LOG_ERROR << "handshake read connect failed " << ec.message();
                return;
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
                return;
            }
        }
        std::string addr;
        {
            if (atyp == 1)
            {
                char address[32] = {0};
                auto handler = completion_handler(4);
                boost::system::error_code ec;
                std::vector<uint8_t> buffer(4, '\0');
                boost::asio::async_read(local_socket_, boost::asio::buffer(buffer.data(), buffer.size()), handler,
                                        yield[ec]);
                if (ec)
                {
                    LOG_ERROR << "handshake read connect failed " << ec.message();
                    return;
                }
                sprintf(address, "%d.%d.%d.%d", buffer[0], buffer[1], buffer[2], buffer[3]);
                addr = address;
            }
            else if (atyp == 3)
            {
                auto handler = completion_handler(1);
                boost::system::error_code ec;
                std::vector<uint8_t> buffer(1, '\0');
                boost::asio::async_read(local_socket_, boost::asio::buffer(buffer.data(), buffer.size()), handler,
                                        yield[ec]);
                if (ec)
                {
                    LOG_ERROR << "handshake read addr len failed " << ec.message();
                    return;
                }
                int address_len = buffer[0];
                LOG_DEBUG << "address_len " << address_len;
                auto address_handler = completion_handler(address_len);
                std::vector<uint8_t> address_buffer(address_len, '\0');
                boost::asio::async_read(local_socket_,
                                        boost::asio::buffer(address_buffer.data(), address_buffer.size()),
                                        address_handler, yield[ec]);
                if (ec)
                {
                    LOG_ERROR << "handshake read addr failed " << ec.message();
                    return;
                }
                addr = std::string(address_buffer.begin(), address_buffer.end());
            }
            else
            {
                LOG_ERROR << "not support address type " << atyp;
                return;
            }
        }
        LOG_DEBUG << "connect addr    --> " << addr;
        uint16_t port = 0;
        {
            auto handler = completion_handler(sizeof(port));
            boost::system::error_code ec;
            boost::asio::async_read(local_socket_, boost::asio::buffer(&port, sizeof(port)), handler, yield[ec]);
            if (ec)
            {
                LOG_ERROR << "handshake read connect failed " << ec.message();
                return;
            }
            char addr[32] = {0};
            port = ntohs(port);
            LOG_DEBUG << "connect port    --> " << port;
        }
        // connect remote
        {
            boost::system::error_code ec;
            boost::asio::ip::tcp::resolver resolver(local_socket_.get_executor());
            std::string host = addr;
            host += ":" + std::to_string(port);
            auto const it = resolver.async_resolve(addr, std::to_string(port), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "resulve remote " << host << " failed --> " << ec.message();
                return;
            }
            boost::asio::async_connect(remote_socket_, it.begin(), it.end(), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "connect remote " << host << " failed --> " << ec.category().name() << ":" << ec.message();
                return;
            }

            LOG_DEBUG << "local " << host << " connect remote " << socket_address(remote_socket_) << " successful";
        }
        {
            char buff[] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
            boost::system::error_code ec;
            boost::asio::async_write(local_socket_, boost::asio::buffer(buff), yield[ec]);
            if (ec)
            {
                LOG_ERROR << " handshake write connect failed " << ec.message();
                return;
            }
            LOG_DEBUG << "write connect finished";
        }
        forward();
    }
    void do_forward(boost::asio::ip::tcp::socket& src, const std::string& dot, boost::asio::ip::tcp::socket& dst,
                    boost::asio::yield_context yield)
    {
        std::string src_address = socket_address(src);
        std::string dst_address = socket_address(dst);
        boost::system::error_code ec;
        for (;;)
        {
            char data[32 * 1024] = {0};
            std::size_t n = src.async_read_some(boost::asio::buffer(data), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "local read failed " << ec.message();
                break;
            }
            LOG_DEBUG << src_address << dot << dst_address << " read len " << n;
            boost::asio::async_write(dst, boost::asio::buffer(data, n), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "remote write failed " << ec.message();
                break;
            }
        }
        LOG_DEBUG << src_address << dot << dst_address << " finish";
    }

   private:
    boost::asio::ip::tcp::socket local_socket_;
    boost::asio::ip::tcp::socket remote_socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
};

int main(int argc, char* argv[])
{
    uint16_t port = 5555;

    boost::asio::io_context io(1);
    boost::asio::io_context so(1);

    boost::asio::ip::tcp::endpoint ed(boost::asio::ip::tcp::v4(), port);

    auto fn = [&](boost::asio::yield_context yield)
    {
        boost::asio::ip::tcp::acceptor acceptor(io, ed);

        LOG_DEBUG << "run acceptor " << ed.address().to_string();

        for (;;)
        {
            boost::system::error_code ec;
            boost::asio::ip::tcp::socket socket(so);
            acceptor.async_accept(socket, yield[ec]);
            if (ec)
            {
                LOG_ERROR << "acceptor error --> " << ec.category().name() << ":" << ec.message();
            }
            else
            {
                LOG_DEBUG << "accept socket --> " << socket_address(socket);
                socket.set_option(boost::asio::ip::tcp::no_delay(true));
                std::make_shared<session>(std::move(socket))->go();
            }
        }
    };

    boost::asio::spawn(io, fn);

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait(
        [&](auto, auto)
        {
            so.stop();
            io.stop();
        });

    std::thread(
        [&so]
        {
            auto w = boost::asio::make_work_guard(so);
            so.run();
        })
        .detach();

    io.run();

    return 0;
}
