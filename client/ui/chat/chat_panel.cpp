#include "chat_panel.h"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <vector>
#include "../../network_manager.h"
#include "../ui_common.h"

using namespace ftxui;

struct ChatState {
    int selected = 0;
    std::vector<std::string> dummy_entries;
    std::string content;
    std::string hint;
};

ChatPanel BuildChatPanel(const std::function<uint64_t()>& get_friend_id,
                         const std::function<std::string()>& get_friend_name,
                         const std::function<bool(const std::string&, std::string&)>& on_send) {
    auto state = std::make_shared<ChatState>();
    auto input_content = Input(&state->content, "Type a message...");

    auto btn_send = Button(
        "Send",
        [state, on_send] {
            if (state->content.empty()) {
                return;
            }
            std::string error_msg;
            if (on_send(state->content, error_msg)) {
                state->content.clear();
                state->hint.clear();
            } else {
                state->hint = error_msg.empty() ? "Message send failed." : error_msg;
            }
        },
        MakeButtonStyle());

    MenuOption option;
    option.entries_option.transform = [get_friend_id](const EntryState& entry_state) {
        const auto friend_id = get_friend_id();
        const auto& history = NetworkManager::GetInstance().GetP2PHistory(friend_id);
        if (entry_state.index >= (int)history.size()) {
            return text("");
        }
        const auto& msg = history[entry_state.index];
        bool is_self = msg.sender_id() == NetworkManager::GetInstance().GetUserId();
        auto msg_box = text(msg.content()) | border;

        Element e;
        if (is_self) {
            e = hbox({filler(), msg_box}) | color(Color::Green);
        } else {
            e = hbox(msg_box, filler());
        }

        if (entry_state.focused) {
            e = e | inverted;
        }
        return e;
    };

    auto msg_menu = Menu(&state->dummy_entries, &state->selected, option);

    auto btn_layout = Container::Horizontal({
        input_content,
        btn_send,
    });

    auto layout = Container::Vertical({
        msg_menu | flex,
        btn_layout,
    });

    auto renderer = Renderer(layout, [state, get_friend_id, get_friend_name, input_content, btn_send, msg_menu] {
        const auto friend_id = get_friend_id();
        if (friend_id != 0) {
            const auto& history = NetworkManager::GetInstance().GetP2PHistory(friend_id);
            if (state->dummy_entries.size() != history.size()) {
                auto old_size = state->dummy_entries.size();
                state->dummy_entries.resize(history.size());
                // Auto-scroll to bottom if we were at the bottom or it's a new load
                if (state->selected == (int)old_size - 1 || old_size == 0) {
                    state->selected = (int)history.size() - 1;
                }
            }
        } else {
            state->dummy_entries.clear();
            state->selected = 0;
        }

        return vbox({
                   text("Chat with " + get_friend_name()) | bold | center,
                   separator(),
                   msg_menu->Render() | vscroll_indicator | yframe | flex,
                   separator(),
                   state->hint.empty() ? text("") : text(state->hint) | color(Color::Red),
                   hbox({
                       input_content->Render() | flex,
                       btn_send->Render(),
                   }),
               }) |
               border | flex;
    });

    return {layout, renderer};
}
