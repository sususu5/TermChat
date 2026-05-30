#include "httprequest.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include "../dao/user_dao.h"
#include "../utils/id_generator.h"

// Webpage path
const std::unordered_set<std::string> HttpRequest::DEFAULT_HTML{"/index",   "/register", "/login",
                                                                "/welcome", "/video",    "/picture"};

// Login page and register page
const std::unordered_map<std::string, int> HttpRequest::DEFAULT_HTML_TAG{{"/login.html", 1}, {"/register.html", 0}};

void HttpRequest::Init() {
    state_ = REQUEST_LINE;
    method_ = path_ = version_ = body_ = "";
    header_.clear();
    post_.clear();
}

// Use state machine to parse the request
bool HttpRequest::parse(Buffer& buff) {
    const char END[] = "\r\n";
    // If the buffer is empty, return false
    if (buff.readable_bytes() == 0) {
        return false;
    };
    while (buff.readable_bytes() && state_ != FINISH) {
        // Reads a string ends with '\r\n' for each loop
        const char* lineEnd = std::search(buff.peek(), buff.begin_write_const(), END, END + 2);
        std::string line(buff.peek(), lineEnd);
        switch (state_) {
            case REQUEST_LINE:
                // If the request line is not parsed successfully, return false
                if (!ParseRequestLine_(line)) {
                    return false;
                }
                ParsePath_();
                break;
            case HEADERS:
                ParseHeader_(line);
                if (buff.readable_bytes() <= 2) {
                    state_ = FINISH;
                }
                break;
            case BODY:
                ParseBody_(line);
                break;
            default:
                break;
        }
        if (lineEnd == buff.begin_write()) {
            buff.retrieve_all();
            break;
        }
        // Skip the '\r\n' characters
        buff.retrieve_until(lineEnd + 2);
    }
    spdlog::debug("[{}], [{}], [{}]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

bool HttpRequest::ParseRequestLine_(const std::string& line) {
    std::regex pattern("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    std::smatch Match;
    if (regex_match(line, Match, pattern)) {
        method_ = Match[1];
        path_ = Match[2];
        version_ = Match[3];
        state_ = HEADERS;
        return true;
    }
    spdlog::error("RequestLine Error");
    return false;
}

void HttpRequest::ParsePath_() {
    if (path_ == "/") {
        path_ = "/index.html";
    } else {
        if (DEFAULT_HTML.find(path_) != DEFAULT_HTML.end()) {
            path_ += ".html";
        }
    }
}

void HttpRequest::ParseHeader_(const std::string& line) {
    std::regex pattern("^([^:]*): ?(.*)$");
    std::smatch Match;
    if (regex_match(line, Match, pattern)) {
        header_[Match[1]] = Match[2];
    } else {
        //  The header is empty, which means the body is coming
        state_ = BODY;
    }
}

void HttpRequest::ParseBody_(const std::string& line) {
    body_ = line;
    ParsePost_();
    // The state is set to FINISH, which means the parsing is complete
    state_ = FINISH;
    spdlog::debug("Body: {}, len: {}", line, line.size());
}

int HttpRequest::ConvertHex(char ch) {
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch;
}

// The POST request contains data of username and password
void HttpRequest::ParsePost_() {
    if (method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFromUrlencoded_();
        if (DEFAULT_HTML_TAG.count(path_)) {
            int tag = DEFAULT_HTML_TAG.find(path_)->second;
            spdlog::debug("Tag: {}", tag);
            if (tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if (UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } else {
                    path_ = "/error.html";
                }
            }
        }
    }
}

void HttpRequest::ParseFromUrlencoded_() {
    if (body_.size() == 0) return;
    std::string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;
    for (; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
            case '=':
                key = body_.substr(j, i - j);
                j = i + 1;
                break;
            case '+':
                body_[i] = ' ';
                break;
            case '%':
                num = ConvertHex(body_[i + 1]) * 16 + ConvertHex(body_[i + 2]);
                body_[i + 2] = num % 10 + '0';
                body_[i + 1] = num / 10 + '0';
                i += 2;
                break;
            case '&':
                value = body_.substr(j, i - j);
                j = i + 1;
                post_[key] = value;
                spdlog::debug("{} = {}", key, value);
                break;
            default:
                break;
        }
    }
    assert(j <= i);
    if (post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

bool HttpRequest::UserVerify(const std::string& name, const std::string& pwd, bool isLogin) {
    if (name == "" || pwd == "") {
        spdlog::error("Username or password is empty!");
        return false;
    }
    spdlog::info("Verify name: {} pwd: {}", name, pwd);

    UserDao dao;
    if (isLogin) {
        if (dao.VerifyUser(name, pwd)) {
            spdlog::debug("UserVerify success!");
            return true;
        } else {
            spdlog::info("pwd error or user not found!");
            return false;
        }
    } else {
        // Register
        if (dao.QueryExist(name)) {
            spdlog::info("user used!");
            return false;
        }
        if (dao.Insert(IdGenerator::GenerateRandId(), name, pwd)) {
            spdlog::debug("register!");
            return true;
        } else {
            spdlog::debug("Insert error!");
            return false;
        }
    }
}

std::string HttpRequest::path() const { return path_; }

std::string& HttpRequest::path() { return path_; }

std::string HttpRequest::method() const { return method_; }

std::string HttpRequest::version() const { return version_; }

std::string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if (post_.count(key) == 1) return post_.find(key)->second;
    return "";
}

std::string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if (post_.count(key) == 1) return post_.find(key)->second;
    return "";
}

bool HttpRequest::IsKeepAlive() const {
    return header_.count("Connection") == 1 && header_.find("Connection")->second == "keep-alive" && version_ == "1.1";
}
