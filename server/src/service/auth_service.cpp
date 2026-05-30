#include "auth_service.h"
#include <spdlog/spdlog.h>
#include "../utils/id_generator.h"
#include "../utils/token_util.h"

void AuthService::user_register(const im::RegisterReq& req, im::RegisterResp* resp) {
    const auto& username = req.username();
    const auto& password = req.password();

    if (username.empty() || password.empty()) {
        resp->set_success(false);
        resp->set_error_msg("Username or password cannot be empty");
        return;
    }

    if (user_dao_.QueryExist(username)) {
        resp->set_success(false);
        resp->set_error_msg("Username already exists");
        return;
    }

    const auto& user_id = IdGenerator::GenerateRandId();
    if (user_dao_.Insert(user_id, username, password)) {
        resp->set_success(true);
        resp->set_user_id(user_id);
        spdlog::info("Register success: {} (ID: {})", username, user_id);
    } else {
        resp->set_success(false);
        resp->set_error_msg("Database internal error");
        spdlog::error("Register failed for user: {}", username);
    }
}

void AuthService::user_login(TcpConnection* conn, const im::LoginReq& req, im::LoginResp* resp) {
    const auto& username = req.username();
    const auto& password = req.password();

    if (username.empty() || password.empty()) {
        resp->set_success(false);
        resp->set_error_msg("Username or password cannot be empty");
        return;
    }

    if (!user_dao_.QueryExist(username)) {
        resp->set_success(false);
        resp->set_error_msg("Username not found");
        return;
    }

    if (user_dao_.VerifyUser(username, password)) {
        resp->set_success(true);
        auto user = user_dao_.FindByUsername(username);
        *resp->mutable_user_info() = user;
        std::string token = TokenUtil::create_token(user.user_id(), user.username());
        resp->set_token(token);

        if (conn) conn->set_user_id(user.user_id());
        spdlog::info("Login success: {}", username);
    } else {
        resp->set_success(false);
        resp->set_error_msg("Database internal error");
        spdlog::error("Login failed for user: {}", username);
    }
}
