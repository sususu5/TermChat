#include "http_handler.h"
#include <spdlog/spdlog.h>

HttpHandler::~HttpHandler() { response_.UnmapFile(); }

bool HttpHandler::Process(Buffer& read_buff, Buffer& write_buff) {
    request_.Init();

    if (read_buff.readable_bytes() <= 0) {
        return false;
    }

    if (request_.parse(read_buff)) {
        spdlog::debug("HTTP request path: {}", request_.path());
        response_.Init(TcpConnection::src_dir, request_.path(), request_.IsKeepAlive(), 200);
    } else {
        response_.Init(TcpConnection::src_dir, request_.path(), false, 400);
    }

    response_.MakeResponse(write_buff);

    spdlog::debug("HTTP response: filesize={}, to_write={}", response_.FileLen(), write_buff.readable_bytes());
    return true;
}
