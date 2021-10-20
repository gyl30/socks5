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

    void do_handshake(boost::asio::yield_context yield)
    {
        // connect remote
        boost::system::error_code ec;
        boost::asio::ip::tcp::resolver resolver(local_socket_.get_executor());
        std::string addr = "127.0.0.1";
        uint16_t port = 5555;
        std::string host = "127.0.0.1:5555";
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
            LOG_DEBUG << src_address << dot << dst_address;
            boost::asio::async_write(dst, boost::asio::buffer(data, n), yield[ec]);
            if (ec)
            {
                LOG_ERROR << "remote write failed " << ec.message();
                break;
            }
        }
        LOG_DEBUG << src_address << dot << dst_address << " finish";
        src.close(ec);
        dst.close(ec);
    }

   private:
    boost::asio::ip::tcp::socket local_socket_;
    boost::asio::ip::tcp::socket remote_socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
};

int main(int argc, char* argv[])
{
    uint16_t port = 6666;

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
