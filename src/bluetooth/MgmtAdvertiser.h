#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <bluetooth/bluetooth.h>

namespace rodent::bluetooth {

struct DirectedTargetConfig {
    bdaddr_t addr {};
    uint8_t addr_type = 0x01;  // LE Public
    uint8_t action = 0x01;     // Allow incoming connection
};

class MgmtAdvertiser {
public:
    explicit MgmtAdvertiser(uint16_t controller_index, uint8_t instance = 1);
    ~MgmtAdvertiser();

    void Start(
        const std::vector<std::string>& service_uuids,
        const std::string& local_name,
        uint16_t appearance,
        const std::optional<DirectedTargetConfig>& directed_target);

    void Stop();

private:
    void EnsureSocketOpen();
    bool RemoveAdvertisement();

    int mgmt_fd_ = -1;
    uint16_t controller_index_;
    uint8_t instance_;
    bool active_ = false;
};

}  // namespace rodent::bluetooth
