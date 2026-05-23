#include "home_page.h"
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>
#include "../network_manager.h"
#include "chat/chat_panel.h"
#include "friend/add_friend_panel.h"
#include "friend/handle_friend_panel.h"
#include "ui_common.h"

using namespace ftxui;

HomePage BuildHomePage(const std::function<void()>& on_logout, HomePageState* state) {
    enum class RightPanel { NONE = 0, ADD_FRIEND = 1, HANDLE_FRIEND = 2, CHAT = 3 };

    auto refresh_friends = [state] {
        std::vector<im::User> users;
        std::string err;
        if (NetworkManager::GetInstance().GetFriendList(users, err)) {
            state->friend_list = users;
            state->friend_names.clear();
            for (const auto& u : users) {
                state->friend_names.push_back(u.username());
            }
            if (state->selected_friend_index >= (int)state->friend_names.size()) {
                state->selected_friend_index = 0;
            }
        }
    };
    refresh_friends();

    auto btn_add_friend = Button(
        "Add Friend",
        [state] {
            state->add_friend_hint.clear();
            state->current_panel = static_cast<int>(RightPanel::ADD_FRIEND);
        },
        MakeButtonStyle());
    auto btn_handle_friend = Button(
        "Friend Requests",
        [state] {
            state->handle_hint.clear();
            state->current_panel = static_cast<int>(RightPanel::HANDLE_FRIEND);
        },
        MakeButtonStyle());

    auto switch_to_chat = [state] {
        if (state->selected_friend_index >= 0 && state->selected_friend_index < (int)state->friend_list.size()) {
            const auto& user = state->friend_list[state->selected_friend_index];
            state->current_chat_friend_id = user.user_id();
            state->current_chat_friend_name = user.username();
            state->current_panel = static_cast<int>(RightPanel::CHAT);
        }
    };

    // Friend Menu
    MenuOption menu_opt;
    menu_opt.on_change = switch_to_chat;
    menu_opt.on_enter = switch_to_chat;

    auto friend_menu = Menu(&state->friend_names, &state->selected_friend_index, menu_opt);

    auto left_panel = Container::Vertical({
        btn_add_friend,
        btn_handle_friend,
        friend_menu,
    });

    auto left_view = Renderer(left_panel, [=] {
        return vbox({
                   text("Contacts") | bold | center,
                   separator(),
                   friend_menu->Render() | flex | yframe,
                   separator(),
                   vbox({
                       btn_add_friend->Render(),
                       btn_handle_friend->Render(),
                   }),
               }) |
               border | size(WIDTH, EQUAL, 26) | size(HEIGHT, GREATER_THAN, 10);
    });

    auto add_friend_panel = BuildAddFriendPanel(
        state->add_friend_user_id, state->add_friend_verify_msg, state->add_friend_hint,
        [state] {
            std::string error_msg;
            try {
                if (!NetworkManager::GetInstance().AddFriend(std::stoull(state->add_friend_user_id),
                                                             state->add_friend_verify_msg, error_msg)) {
                    state->add_friend_hint = error_msg;
                } else {
                    state->current_panel = static_cast<int>(RightPanel::NONE);
                    state->add_friend_hint = "Request sent.";
                }
            } catch (...) {
                state->add_friend_hint = "Invalid User ID format.";
            }
        },
        [state] { state->current_panel = static_cast<int>(RightPanel::NONE); });

    auto handle_friend_panel = BuildHandleFriendPanel(
        state->handle_hint,
        [state, refresh_friends](uint64_t req_id, uint64_t sender_id) {
            std::string error_msg;
            if (!NetworkManager::GetInstance().HandleFriendRequest(req_id, sender_id, im::FriendAction::ACTION_ACCEPT,
                                                                   error_msg)) {
                state->handle_hint = error_msg;
            } else {
                state->handle_hint = "Request accepted successfully.";
                NetworkManager::GetInstance().RemovePendingRequest(req_id);
                refresh_friends();
            }
        },
        [state](uint64_t req_id, uint64_t sender_id) {
            std::string error_msg;
            if (!NetworkManager::GetInstance().HandleFriendRequest(req_id, sender_id, im::FriendAction::ACTION_REJECT,
                                                                   error_msg)) {
                state->handle_hint = error_msg;
            } else {
                state->handle_hint = "Request rejected successfully.";
                NetworkManager::GetInstance().RemovePendingRequest(req_id);
            }
        });

    // Chat Panel
    auto chat_panel = BuildChatPanel(
        [state] { return state->current_chat_friend_id; }, [state] { return state->current_chat_friend_name; },
        [state](const std::string& msg, std::string& err) {
            if (state->current_chat_friend_id == 0) {
                err = "Select a chat first.";
                return false;
            }
            return NetworkManager::GetInstance().SendP2PMessage(state->current_chat_friend_id, msg, err);
        });

    auto empty_layout = Container::Vertical({});
    auto empty_renderer = Renderer(empty_layout, [] {
        return vbox({
                   text("Select an action on the left.") | dim | center,
               }) |
               border | flex;
    });

    auto right_panel = Container::Tab(
        {
            empty_renderer,
            add_friend_panel.renderer,
            handle_friend_panel.renderer,
            chat_panel.renderer,
        },
        &state->current_panel);

    auto right_panel_renderer = Renderer(right_panel, [=] { return right_panel->Render() | flex; });

    auto body_layout = Container::Horizontal({
        left_view,
        right_panel,
    });

    auto btn_logout = Button("Logout", on_logout, MakeButtonStyle());

    auto main_layout = Container::Vertical({
        btn_logout,
        body_layout,
    });

    auto main_renderer = Renderer(main_layout, [=] {
        return vbox({
            hbox({
                text("User ID: " + std::to_string(NetworkManager::GetInstance().GetUserId())) | bold,
                filler(),
                btn_logout->Render(),
            }) | border,
            hbox({
                left_view->Render(),
                right_panel_renderer->Render() | flex,
            }) | flex,
        });
    });

    return {main_layout, main_renderer};
}
