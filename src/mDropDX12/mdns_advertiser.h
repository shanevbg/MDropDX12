#pragma once
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#include <string>
#include <atomic>
#include <thread>

#pragma comment(lib, "dnsapi.lib")

// UDP beacon port for LAN discovery (separate from TCP data port)
#define MILKWAVE_BEACON_PORT 9271

class MdnsAdvertiser {
public:
    bool Register(const std::string& serviceName, int port, int pid);
    void Unregister();
    ~MdnsAdvertiser() { Unregister(); }
private:
    // mDNS (Windows DNS-SD, may not work on all setups)
    DNS_SERVICE_INSTANCE* m_instance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST m_request{};
    bool m_registered = false;

    // UDP broadcast beacon (reliable fallback)
    std::atomic<bool> m_beaconRunning{false};
    std::thread m_beaconThread;
    void BeaconLoop(std::string serviceName, int tcpPort, int pid);
};
