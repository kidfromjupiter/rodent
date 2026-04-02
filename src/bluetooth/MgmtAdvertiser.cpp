#include "MgmtAdvertiser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <optional>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <vector>

#include <bluetooth/hci.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace rodent::bluetooth {
namespace {

constexpr std::size_t kLegacyAdvDataMaxLen = 31;

constexpr uint16_t kMgmtOpAddDevice = 0x0033;
constexpr uint16_t kMgmtOpAddAdvertising = 0x003E;
constexpr uint16_t kMgmtOpRemoveAdvertising = 0x003F;
constexpr uint16_t kMgmtEventCommandComplete = 0x0001;
constexpr uint16_t kMgmtEventCommandStatus = 0x0002;
constexpr uint16_t kMgmtIndexNone = 0xFFFF;

constexpr uint8_t kAdTypeComplete16BitServiceUuids = 0x03;
constexpr uint8_t kAdTypeShortenedLocalName = 0x08;
constexpr uint8_t kAdTypeCompleteLocalName = 0x09;
constexpr uint8_t kAdTypeAppearance = 0x19;

constexpr uint32_t kMgmtAdvFlagConnectable = 1u << 0;
constexpr uint32_t kMgmtAdvFlagDiscoverable = 1u << 1;

struct mgmt_hdr_local {
    uint16_t opcode;
    uint16_t index;
    uint16_t len;
} __attribute__((packed));

struct mgmt_ev_cmd_complete_local {
    uint16_t opcode;
    uint8_t status;
} __attribute__((packed));

struct mgmt_ev_cmd_status_local {
    uint16_t opcode;
    uint8_t status;
} __attribute__((packed));

struct mgmt_cp_add_advertising_local {
    uint8_t instance;
    uint32_t flags;
    uint16_t duration;
    uint16_t timeout;
    uint8_t adv_data_len;
    uint8_t scan_rsp_len;
} __attribute__((packed));

struct mgmt_cp_add_device_local {
    bdaddr_t addr;
    uint8_t addr_type;
    uint8_t action;
} __attribute__((packed));

struct mgmt_cp_remove_advertising_local {
    uint8_t instance;
} __attribute__((packed));

bool appendAdField(std::vector<uint8_t>& buffer, uint8_t type, const uint8_t* payload, std::size_t payload_len)
{
    if (payload_len > 0xFF - 1) {
        return false;
    }

    const std::size_t required = 2 + payload_len;
    if (buffer.size() + required > kLegacyAdvDataMaxLen) {
        return false;
    }

    buffer.push_back(static_cast<uint8_t>(1 + payload_len));
    buffer.push_back(type);
    buffer.insert(buffer.end(), payload, payload + payload_len);
    return true;
}

std::optional<uint16_t> parseUuid16(std::string uuid)
{
    std::transform(
        uuid.begin(),
        uuid.end(),
        uuid.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto parse_hex16 = [](const std::string& hex) -> std::optional<uint16_t> {
        if (hex.size() != 4) {
            return std::nullopt;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(hex.c_str(), &end, 16);
        if (errno != 0 || end == hex.c_str() || (end != nullptr && *end != '\0') || value > 0xFFFF) {
            return std::nullopt;
        }
        return static_cast<uint16_t>(value);
    };

    if (const auto direct = parse_hex16(uuid); direct.has_value()) {
        return direct;
    }

    constexpr const char* kBaseSuffix = "-0000-1000-8000-00805f9b34fb";
    if (uuid.size() == 36 &&
        uuid.compare(8, std::strlen(kBaseSuffix), kBaseSuffix) == 0 &&
        uuid.rfind("0000", 0) == 0) {
        return parse_hex16(uuid.substr(4, 4));
    }

    return std::nullopt;
}

std::vector<uint8_t> buildLegacyAdvDataFromServices(const std::vector<std::string>& service_uuids)
{
    std::vector<uint8_t> adv_data;
    std::vector<uint8_t> uuid_payload;
    uuid_payload.reserve(service_uuids.size() * 2);

    for (const auto& uuid : service_uuids) {
        const auto parsed = parseUuid16(uuid);
        if (!parsed.has_value()) {
            continue;
        }
        uuid_payload.push_back(static_cast<uint8_t>(parsed.value() & 0xFF));
        uuid_payload.push_back(static_cast<uint8_t>((parsed.value() >> 8) & 0xFF));
    }

    if (!uuid_payload.empty()) {
        (void)appendAdField(
            adv_data,
            kAdTypeComplete16BitServiceUuids,
            uuid_payload.data(),
            uuid_payload.size());
    }
    return adv_data;
}

std::vector<uint8_t> buildLegacyScanResponse(const std::string& local_name, uint16_t appearance)
{
    std::vector<uint8_t> scan_rsp;

    const uint8_t appearance_payload[2] = {
        static_cast<uint8_t>(appearance & 0xFF),
        static_cast<uint8_t>((appearance >> 8) & 0xFF)};
    (void)appendAdField(scan_rsp, kAdTypeAppearance, appearance_payload, sizeof(appearance_payload));

    if (!local_name.empty() && scan_rsp.size() < kLegacyAdvDataMaxLen) {
        const std::size_t room_for_name = kLegacyAdvDataMaxLen - scan_rsp.size();
        if (room_for_name > 2) {
            const std::size_t max_name_len = room_for_name - 2;
            const bool truncated = local_name.size() > max_name_len;
            const std::size_t used_len = std::min(local_name.size(), max_name_len);
            const uint8_t name_type = truncated ? kAdTypeShortenedLocalName : kAdTypeCompleteLocalName;
            (void)appendAdField(
                scan_rsp,
                name_type,
                reinterpret_cast<const uint8_t*>(local_name.data()),
                used_len);
        }
    }

    return scan_rsp;
}

int openMgmtControlSocket()
{
    const int fd = ::socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) {
        throw std::runtime_error("Failed to create Bluetooth mgmt socket");
    }

    sockaddr_hci addr {};
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = htobs(HCI_DEV_NONE);
    addr.hci_channel = HCI_CHANNEL_CONTROL;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to bind Bluetooth mgmt socket");
    }

    const timeval timeout {3, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool sendMgmtCommandWaitStatus(
    int mgmt_fd,
    uint16_t controller_index,
    uint16_t opcode,
    const uint8_t* payload,
    std::size_t payload_len,
    uint8_t& out_status)
{
    mgmt_hdr_local header {
        htobs(opcode),
        htobs(controller_index),
        htobs(static_cast<uint16_t>(payload_len))};

    std::vector<uint8_t> request(sizeof(header) + payload_len);
    std::memcpy(request.data(), &header, sizeof(header));
    if (payload_len > 0) {
        std::memcpy(request.data() + sizeof(header), payload, payload_len);
    }

    if (::send(mgmt_fd, request.data(), request.size(), 0) < 0) {
        return false;
    }

    std::array<uint8_t, 1024> response {};
    while (true) {
        const ssize_t received = ::recv(mgmt_fd, response.data(), response.size(), 0);
        if (received < 0) {
            return false;
        }
        if (static_cast<std::size_t>(received) < sizeof(mgmt_hdr_local)) {
            continue;
        }

        const auto* response_header = reinterpret_cast<const mgmt_hdr_local*>(response.data());
        const uint16_t response_opcode = btohs(response_header->opcode);
        const uint16_t response_index = btohs(response_header->index);
        const uint16_t response_len = btohs(response_header->len);

        if (sizeof(mgmt_hdr_local) + response_len > static_cast<std::size_t>(received)) {
            continue;
        }
        if (response_index != controller_index && response_index != kMgmtIndexNone) {
            continue;
        }

        const uint8_t* response_payload = response.data() + sizeof(mgmt_hdr_local);
        if (response_opcode == kMgmtEventCommandComplete) {
            if (response_len < sizeof(mgmt_ev_cmd_complete_local)) {
                continue;
            }
            const auto* complete = reinterpret_cast<const mgmt_ev_cmd_complete_local*>(response_payload);
            if (btohs(complete->opcode) != opcode) {
                continue;
            }
            out_status = complete->status;
            return true;
        }
        if (response_opcode == kMgmtEventCommandStatus) {
            if (response_len < sizeof(mgmt_ev_cmd_status_local)) {
                continue;
            }
            const auto* status = reinterpret_cast<const mgmt_ev_cmd_status_local*>(response_payload);
            if (btohs(status->opcode) != opcode) {
                continue;
            }
            out_status = status->status;
            return true;
        }
    }
}

}  // namespace

MgmtAdvertiser::MgmtAdvertiser(uint16_t controller_index, uint8_t instance)
    : controller_index_(controller_index)
    , instance_(instance)
{
}

MgmtAdvertiser::~MgmtAdvertiser()
{
    Stop();
    if (mgmt_fd_ >= 0) {
        ::close(mgmt_fd_);
        mgmt_fd_ = -1;
    }
}

void MgmtAdvertiser::Start(
    const std::vector<std::string>& service_uuids,
    const std::string& local_name,
    uint16_t appearance,
    const std::optional<DirectedTargetConfig>& directed_target)
{
    EnsureSocketOpen();

    (void)RemoveAdvertisement();

    if (directed_target.has_value()) {
        mgmt_cp_add_device_local add_device_cmd {};
        add_device_cmd.addr = directed_target->addr;
        add_device_cmd.addr_type = directed_target->addr_type;
        add_device_cmd.action = directed_target->action;

        uint8_t add_device_status = 0xFF;
        if (!sendMgmtCommandWaitStatus(
                mgmt_fd_,
                controller_index_,
                kMgmtOpAddDevice,
                reinterpret_cast<const uint8_t*>(&add_device_cmd),
                sizeof(add_device_cmd),
                add_device_status)) {
            throw std::runtime_error("MGMT_OP_ADD_DEVICE failed: no reply from controller");
        }
        if (add_device_status != 0x00) {
            throw std::runtime_error("MGMT_OP_ADD_DEVICE failed with status " + std::to_string(add_device_status));
        }
    }

    const auto adv_data = buildLegacyAdvDataFromServices(service_uuids);
    const auto scan_rsp = buildLegacyScanResponse(local_name, appearance);

    std::vector<uint8_t> payload(sizeof(mgmt_cp_add_advertising_local) + adv_data.size() + scan_rsp.size());
    auto* cmd = reinterpret_cast<mgmt_cp_add_advertising_local*>(payload.data());
    cmd->instance = instance_;
    cmd->flags = htobl(kMgmtAdvFlagConnectable | kMgmtAdvFlagDiscoverable);
    cmd->duration = htobs(0);
    cmd->timeout = htobs(0);
    cmd->adv_data_len = static_cast<uint8_t>(adv_data.size());
    cmd->scan_rsp_len = static_cast<uint8_t>(scan_rsp.size());
    std::memcpy(payload.data() + sizeof(mgmt_cp_add_advertising_local), adv_data.data(), adv_data.size());
    std::memcpy(
        payload.data() + sizeof(mgmt_cp_add_advertising_local) + adv_data.size(),
        scan_rsp.data(),
        scan_rsp.size());

    uint8_t status = 0xFF;
    if (!sendMgmtCommandWaitStatus(
            mgmt_fd_,
            controller_index_,
            kMgmtOpAddAdvertising,
            payload.data(),
            payload.size(),
            status)) {
        throw std::runtime_error("MGMT_OP_ADD_ADVERTISING failed: no reply from controller");
    }
    if (status != 0x00) {
        throw std::runtime_error("MGMT_OP_ADD_ADVERTISING failed with status " + std::to_string(status));
    }
    active_ = true;
}

void MgmtAdvertiser::Stop()
{
    if (!active_ || mgmt_fd_ < 0) {
        return;
    }
    (void)RemoveAdvertisement();
    active_ = false;
}

void MgmtAdvertiser::EnsureSocketOpen()
{
    if (mgmt_fd_ < 0) {
        mgmt_fd_ = openMgmtControlSocket();
    }
}

bool MgmtAdvertiser::RemoveAdvertisement()
{
    mgmt_cp_remove_advertising_local remove_cmd {instance_};
    uint8_t status = 0xFF;
    if (!sendMgmtCommandWaitStatus(
            mgmt_fd_,
            controller_index_,
            kMgmtOpRemoveAdvertising,
            reinterpret_cast<const uint8_t*>(&remove_cmd),
            sizeof(remove_cmd),
            status)) {
        return false;
    }
    return status == 0x00;
}

}  // namespace rodent::bluetooth
