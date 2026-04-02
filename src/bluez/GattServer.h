#pragma once

#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/AdaptorInterfaces.h>
#include <sdbus-c++/ProxyInterfaces.h>
#include <sdbus-c++/StandardInterfaces.h>
#include <sdbus-c++/Types.h>

#include "generated/bluez/bluez_all_adaptors.h"
#include "generated/bluez/bluez_all_proxies.h"

namespace rodent::bluez {

class GattChar final: public sdbus::AdaptorInterfaces<
                          org::bluez::GattCharacteristic1_adaptor,
                          sdbus::ManagedObject_adaptor,
                          sdbus::Properties_adaptor> {
public:
    GattChar(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string char_uuid,
        sdbus::ObjectPath service,
        std::vector<uint8_t> value = {},
        std::vector<std::string> flags = {});
    ~GattChar();

    void SetValueAndNotify(std::vector<uint8_t> value);
    [[nodiscard]] bool NotificationEnabled() const;

private:
    void ReadValue(
        sdbus::Result<std::vector<uint8_t>>&& result,
        std::map<std::string, sdbus::Variant> options) override;
    void WriteValue(
        sdbus::Result<>&& result,
        std::vector<uint8_t> value,
        std::map<std::string, sdbus::Variant> options) override;
    void StartNotify() override;
    void StopNotify() override;
    void Confirm() override;
    std::string UUID() override;
    sdbus::ObjectPath Service() override;
    std::vector<uint8_t> Value() override;
    bool Notifying() override;
    std::vector<std::string> Flags() override;

    std::string char_uuid_;
    sdbus::ObjectPath service_;
    std::vector<uint8_t> value_;
    bool write_acquired_ = false;
    bool notify_acquired_ = false;
    bool notifying_ = false;
    std::vector<std::string> flags_;
    uint16_t handle_ = 0;
    int notify_stream_fd_ = -1;
};

class GattDesc final: public sdbus::AdaptorInterfaces<
                          org::bluez::GattDescriptor1_adaptor,
                          sdbus::ManagedObject_adaptor,
                          sdbus::Properties_adaptor> {
public:
    GattDesc(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string uuid,
        sdbus::ObjectPath characteristic,
        std::vector<uint8_t> value = {},
        std::vector<std::string> flags = {},
        uint16_t handle = 0);
    ~GattDesc();

private:
    std::vector<uint8_t> ReadValue(const std::map<std::string, sdbus::Variant>& options) override;
    void WriteValue(const std::vector<uint8_t>& value, const std::map<std::string, sdbus::Variant>& options) override;
    std::string UUID() override;
    sdbus::ObjectPath Characteristic() override;
    std::vector<uint8_t> Value() override;
    std::vector<std::string> Flags() override;
    uint16_t Handle() override;
    void Handle(const uint16_t& value) override;

    std::string uuid_;
    sdbus::ObjectPath characteristic_;
    std::vector<uint8_t> value_;
    std::vector<std::string> flags_;
    uint16_t handle_ = 0;
};

class GattService final: public sdbus::AdaptorInterfaces<
                             org::bluez::GattService1_adaptor,
                             sdbus::ManagedObject_adaptor,
                             sdbus::Properties_adaptor> {
public:
    GattService(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string uuid,
        bool primary = true,
        sdbus::ObjectPath device = sdbus::ObjectPath("/"),
        std::vector<sdbus::ObjectPath> includes = {},
        uint16_t handle = 0);
    ~GattService();

private:
    std::string UUID() override;
    bool Primary() override;
    sdbus::ObjectPath Device() override;
    std::vector<sdbus::ObjectPath> Includes() override;
    uint16_t Handle() override;
    void Handle(const uint16_t& value) override;

    std::string uuid_;
    bool primary_ = true;
    sdbus::ObjectPath device_;
    std::vector<sdbus::ObjectPath> includes_;
    uint16_t handle_ = 0;
};

class Application final: public sdbus::AdaptorInterfaces<sdbus::ObjectManager_adaptor> {
public:
    Application(sdbus::IConnection& connection, sdbus::ObjectPath object_path);
    ~Application();
};

class GattManager final: public sdbus::ProxyInterfaces<org::bluez::GattManager1_proxy> {
public:
    GattManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path);
    ~GattManager();
};

class AdvertisingManager final: public sdbus::ProxyInterfaces<org::bluez::LEAdvertisingManager1_proxy> {
public:
    AdvertisingManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path);
    ~AdvertisingManager();

    sdbus::PendingAsyncCall RegisterAdvertisementAsync(
        const sdbus::ObjectPath& advertisement,
        const std::map<std::string, sdbus::Variant>& options);
    std::optional<sdbus::Error> WaitForRegisterAdvertisement();

private:
    void onRegisterAdvertisementReply(std::optional<sdbus::Error> error) override;

    std::promise<std::optional<sdbus::Error>> register_adv_promise_;
    std::future<std::optional<sdbus::Error>> register_adv_future_;
};

class LEAdvertisement final: public sdbus::AdaptorInterfaces<org::bluez::LEAdvertisement1_adaptor> {
public:
    struct Config {
        std::string type = "peripheral";
        std::vector<std::string> service_uuids{};
        std::map<uint16_t, sdbus::Variant> manufacturer_data{};
        std::vector<std::string> solicit_uuids{};
        std::map<std::string, sdbus::Variant> service_data{};
        std::map<uint8_t, sdbus::Variant> data{};
        std::vector<std::string> scan_response_service_uuids{};
        std::map<uint16_t, sdbus::Variant> scan_response_manufacturer_data{};
        std::vector<std::string> scan_response_solicit_uuids{};
        std::map<std::string, sdbus::Variant> scan_response_service_data{};
        std::map<uint8_t, sdbus::Variant> scan_response_data{};
        bool discoverable = true;
        uint16_t discoverable_timeout = 0;
        std::vector<std::string> includes{};
        std::string local_name{};
        uint16_t appearance = 0;
        uint16_t duration = 0;
        uint16_t timeout = 0;
        std::string secondary_channel = "1M";
        uint32_t min_interval_ms = 30;
        uint32_t max_interval_ms = 50;
        int16_t tx_power = 0;
    };

    LEAdvertisement(sdbus::IConnection& connection, sdbus::ObjectPath object_path, Config config);
    ~LEAdvertisement();

private:
    void Release() override;
    std::string Type() override;
    std::vector<std::string> ServiceUUIDs() override;
    std::map<uint16_t, sdbus::Variant> ManufacturerData() override;
    std::vector<std::string> SolicitUUIDs() override;
    std::map<std::string, sdbus::Variant> ServiceData() override;
    std::map<uint8_t, sdbus::Variant> Data() override;
    std::vector<std::string> ScanResponseServiceUUIDs() override;
    std::map<uint16_t, sdbus::Variant> ScanResponseManufacturerData() override;
    std::vector<std::string> ScanResponseSolicitUUIDs() override;
    std::map<std::string, sdbus::Variant> ScanResponseServiceData() override;
    std::map<uint8_t, sdbus::Variant> ScanResponseData() override;
    bool Discoverable() override;
    uint16_t DiscoverableTimeout() override;
    std::vector<std::string> Includes() override;
    std::string LocalName() override;
    uint16_t Appearance() override;
    uint16_t Duration() override;
    uint16_t Timeout() override;
    std::string SecondaryChannel() override;
    uint32_t MinInterval() override;
    uint32_t MaxInterval() override;
    int16_t TxPower() override;

    Config config_;
};

}  // namespace rodent::bluez
