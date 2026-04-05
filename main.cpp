#include "location_shared.h"
#include "server_thread.h"
#include "gui_thread.h"

#include <thread>

int main() {
    static LocationShared locationInfo;

    std::thread gui_thread(run_gui, &locationInfo);
    std::thread server_thread(run_server, &locationInfo);

    gui_thread.join();
    server_thread.join();
    return 0;
}