#include <futures/EventExecutor.h>
#include <futures/Signal.h>
#include <futures/Timeout.h>
#include <futures/io/AsyncSocket.h>
#include <futures/io/AsyncSSLSocket.h>
#include <futures/io/PipelinedRpcFuture.h>
#include <futures/http/HttpCodec.h>
#include <futures/core/Compression.h>
#include <futures/http/http_parser.h>

using namespace futures;

template <typename ReadStream, typename WriteSink, typename Dispatch>
RpcFuture<ReadStream, WriteSink>
makeRpcClientFuture(io::Channel::Ptr transport,
        std::shared_ptr<Dispatch> dispatch) {
    using Req = typename ReadStream::Item;
    using Resp = typename WriteSink::Out;
    return RpcFuture<ReadStream, WriteSink>(
            transport,
            dispatch);
}

static std::string getField(const char *buf,
        const http_parser_url& url, int field) {
    if (url.field_set & (1 << field)) {
        return std::string(buf + url.field_data[field].off,
                url.field_data[field].len);
    } else {
        return "";
    }
}

static BoxedFuture<io::SocketChannel::Ptr> connect(
        EventExecutor *ev, io::SSLContext *ctx, const folly::SocketAddress &addr, bool ssl)
{
    if (ssl) {
        return io::SSLSocketChannel::connect(ev, ctx, addr)
            .cast<io::SocketChannel::Ptr>();
#if 0
            | [] (io::SSLSocketChannel::Ptr sock) {
                sock->printPeerCert();
                io::SocketChannel::Ptr ptr = sock;
                return ptr;
            };
#endif
    } else {
        return io::SocketChannel::connect(ev, addr);
    }
}

static BoxedFuture<folly::Unit> fetch(EventExecutor *ev, io::SSLContext *ctx, const char *s) {
    http_parser_url url;
    http_parser_url_init(&url);
    int ret = http_parser_parse_url(s, strlen(s), false, &url);
    FUTURES_CHECK(ret == 0) << " invalid url: " << s << " " << ret;

    std::string host = getField(s, url, UF_HOST);
    std::string path = getField(s, url, UF_PATH);
    bool is_https = getField(s, url, UF_SCHEMA) == "https";
    int port = url.port ? url.port : (is_https ? 443 : 80);
    // int port = std::stoi(getField(argv[1], url, UF_PORT));

    folly::SocketAddress addr(host.c_str(), port, true);

    return connect(ev, ctx, addr, is_https)
        >> [host] (io::SocketChannel::Ptr sock) {
            FUTURES_LOG(INFO) << "connected";
            auto client = std::make_shared<PipelineClientDispatcher<http::Request,
                http::Response>>();
            EventExecutor::current()->spawn(
                    makeRpcClientFuture<io::FramedStream<http::HttpV1ResponseDecoder>,
                    io::FramedSink<http::HttpV1RequestEncoder>>(sock, client));
            http::Request req;
            req.path = "/";
            req.method = HTTP_GET;
            req.headers["Host"] = host;
            req.headers["Accept"] = "*/*";
            return (*client)(std::move(req))
                << [client] (Try<http::Response> req) {
                    if (req.hasException()) {
                        std::cerr << "CALL: " << req.exception().what() << std::endl;
                    } else {
                        std::cerr << *req << std::endl;
                        std::string body;
                        auto it = req->headers.find("Content-Encoding");
                        if (it == req->headers.end()) {
                            req->body.appendToString(body);
                        } else if (it->second == "gzip") {
                            auto codec = folly::io::getCodec(folly::io::CodecType::GZIP);
                            FUTURES_LOG(INFO) << "size: " << req->body.front()->length();
                            auto out = codec->uncompress(req->body.front());
                            FUTURES_LOG(INFO) << "uncompressed size: " << out->computeChainDataLength();
                            auto r = out->coalesce();
                            body = r.toString();
                        } else {
                            body = "<UNSUPPORTED Content-Encoding>";
                        }
                        std::cerr << "========" << std::endl;
                        std::cout << body << std::endl;
                        std::cerr << "========" << std::endl;
                    }
                    return makeOk();
                }
                << [client] (Try<Unit> err) {
                    if (err.hasException())
                        FUTURES_LOG(ERROR) << err.exception().what();
                    return client->close();
                };
        };
}

int main(int argc, char *argv[])
{

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " url" << std::endl;
        return 1;
    }

    EventExecutor loop(true);
    io::SSLContext ctx;

    auto f = timeout(&loop, fetch(&loop, &ctx, argv[1]), 5.0)
        .error([] (folly::exception_wrapper err) {
            FUTURES_LOG(ERROR) << err.what();
        });
#if 0
    auto sig = signal(&loop, SIGINT)
        .andThen([&] (int signum) {
        EventExecutor::current()->stop();
        return makeOk();
    });
    loop.spawn(std::move(sig));
#endif

    loop.spawn(std::move(f));
    loop.run();
    return 0;
}
