#pragma once

#include <cstdint>
#include <ftxui/component/component.hpp>
#include <functional>
#include <string>

struct ChatPanel {
    ftxui::Component layout;
    ftxui::Component renderer;
};

ChatPanel BuildChatPanel(const std::function<uint64_t()>& get_friend_id,
                         const std::function<std::string()>& get_friend_name,
                         const std::function<bool(const std::string&, std::string&)>& on_send);
