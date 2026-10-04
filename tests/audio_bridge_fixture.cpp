#include "audio_service.h"
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
int main() {
    AudioService service(0); std::string epoch, token;
    if (!service.Start("127.0.0.1") || !service.BeginSession(epoch, token)) return 1;
    // Random credentials exist only for this ephemeral loopback test process.
    std::cout << service.Port() << ' ' << epoch << ' ' << token << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command != "start") break;
        service.BeginStream(); std::vector<uint8_t> pcm(3840);
        for (size_t i = 0; i < pcm.size(); i += 4) { pcm[i] = 0xe8; pcm[i + 1] = 0x03; pcm[i + 2] = 0x18; pcm[i + 3] = 0xfc; }
        for (unsigned i = 0; i < 12; ++i) { service.PublishPCM(pcm.data(), pcm.size(), uint64_t(i) * 20000); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        service.EndStream();
    }
    service.Stop();
}
