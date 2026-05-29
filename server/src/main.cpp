#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#include "core/webserver.h"

static Webserver* g_server = nullptr;

void signal_handler(int sig) {
    const char msg[] = "\n[Signal] Received shutdown signal, stopping server...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    if (g_server) {
        g_server->Stop();
    }
}

int EnvInt(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return default_value;
    }

    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

int main(int argc, char* argv[]) {
    bool open_log = true;
    int opt;
    const char* opt_string = "l:";

    while ((opt = getopt(argc, argv, opt_string)) != -1) {
        switch (opt) {
            case 'l':
                open_log = false;
                break;
            default:
                printf("Usage: %s [-l 0[1]\n", argv[0]);
                return 1;
        }
    }

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);   // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr);  // kill command

    {
        const int mysql_pool_num = EnvInt("TERMCHAT_MYSQL_POOL_SIZE", 50);
        const int thread_num = EnvInt("TERMCHAT_THREAD_NUM", 40);
        const int epoll_event_num = EnvInt("TERMCHAT_EPOLL_EVENTS", 4096);
        Webserver server(1316, 3, 60000, 3306, "root", "123456", "testdb", mysql_pool_num, thread_num, epoll_event_num,
                         open_log, 1, 1024);
        g_server = &server;
        server.Start();
        g_server = nullptr;
    }

    return 0;
}
