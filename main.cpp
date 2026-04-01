#include "src/bluez/BluezClient.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <csignal>
#include <chrono>
#include <future>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "generated/bluez/adaptors/org_freedesktop_DBus_ObjectManager_adaptor.h"
#include "generated/bluez/bluez_all_adaptors.h"
#include "generated/bluez/proxies/org_freedesktop_DBus_Introspectable_proxy.h"

namespace
{
    volatile std::sig_atomic_t g_shouldStop = 0;

    void handleSignal(int)
    {
        g_shouldStop = 1;
    }
}

class GattChar final: public sdbus::AdaptorInterfaces<org::bluez::GattCharacteristic1_adaptor,
    sdbus::ManagedObject_adaptor, sdbus::Properties_adaptor>
{
    /// base class for gatt characteristics
    std::string char_uuid_;
    sdbus::ObjectPath service_;
    std::vector<uint8_t> value_;
    bool write_acquired_;
    bool notify_acquired_;
    bool notifying_;
    std::vector<std::string> flags_;
    uint16_t handle_;
    int notify_stream_fd_;
public:
    GattChar(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string char_uuid,
        sdbus::ObjectPath service,
        std::vector<uint8_t> value = {},
        std::vector<std::string> flags = {})
        : AdaptorInterfaces(connection, std::move(object_path))
        , char_uuid_(std::move(char_uuid))
        , service_(std::move(service))
        , value_(std::move(value))
        , flags_(std::move(flags))
    {
       registerAdaptor();
    }
    ~GattChar()
    {
        unregisterAdaptor();
    }

   void SetValueAndNotify(std::vector<uint8_t> value)
    {
        value_ = std::move(value);

        if (notifying_) {
            emitPropertiesChangedSignal(
                sdbus::InterfaceName{org::bluez::GattCharacteristic1_adaptor::INTERFACE_NAME},
                {sdbus::PropertyName{"Value"}}
            );
        }
    }
    [[nodiscard]] bool NotificationEnabled() const
    {
        return notifying_;
    }

private:
    void ReadValue(
        sdbus::Result<std::vector<uint8_t>>&& result,
        std::map<std::string, sdbus::Variant> /*options*/) override
    {
        result.returnResults(value_);
    }

    void WriteValue(
        sdbus::Result<>&& result,
        std::vector<uint8_t> value,
        std::map<std::string, sdbus::Variant> /*options*/) override
    {
        value_ = std::move(value);
        result.returnResults();
    }

    void StartNotify() override
    {
        notifying_ = true;
    }

    void StopNotify() override
    {
        notifying_ = false;
        notify_acquired_ = false;
    }

    void Confirm() override
    {
    }

    std::string UUID() override
    {
        return char_uuid_;
    }

    sdbus::ObjectPath Service() override
    {
        return service_;
    }

    std::vector<uint8_t> Value() override
    {
        return value_;
    }

    bool Notifying() override
    {
        return notifying_;
    }

    std::vector<std::string> Flags() override
    {
        return flags_;
    }
};

class GattDesc final: public sdbus::AdaptorInterfaces<org::bluez::GattDescriptor1_adaptor,
    sdbus::ManagedObject_adaptor, sdbus::Properties_adaptor>
{
    std::string uuid_;
    sdbus::ObjectPath characteristic_;
    std::vector<uint8_t> value_;
    std::vector<std::string> flags_;
    uint16_t handle_;
public:
    GattDesc(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string uuid,
        sdbus::ObjectPath characteristic,
        std::vector<uint8_t> value = {},
        std::vector<std::string> flags = {},
        uint16_t handle = 0)
        : AdaptorInterfaces(connection, std::move(object_path))
        , uuid_(std::move(uuid))
        , characteristic_(std::move(characteristic))
        , value_(std::move(value))
        , flags_(std::move(flags))
        , handle_(handle)
    {
        registerAdaptor();
    }

    ~GattDesc()
    {
        unregisterAdaptor();
    }

private:
    std::vector<uint8_t> ReadValue(const std::map<std::string, sdbus::Variant>&) override
    {
        return value_;
    }

    void WriteValue(const std::vector<uint8_t>& value, const std::map<std::string, sdbus::Variant>&) override
    {
        value_ = value;
    }

    std::string UUID() override
    {
        return uuid_;
    }

    sdbus::ObjectPath Characteristic() override
    {
        return characteristic_;
    }

    std::vector<uint8_t> Value() override
    {
        return value_;
    }

    std::vector<std::string> Flags() override
    {
        return flags_;
    }

    uint16_t Handle() override
    {
        return handle_;
    }

    void Handle(const uint16_t& value) override
    {
        handle_ = value;
    }
};

class GattService final: public sdbus::AdaptorInterfaces<org::bluez::GattService1_adaptor,
    sdbus::ManagedObject_adaptor, sdbus::Properties_adaptor>
{
    /// Base class for gatt services
    std::string uuid_;
    bool primary_;
    sdbus::ObjectPath device_;
    std::vector<sdbus::ObjectPath> includes_;
    uint16_t handle_;
public:
    GattService(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        std::string uuid,
        bool primary = true,
        sdbus::ObjectPath device = sdbus::ObjectPath("/"),
        std::vector<sdbus::ObjectPath> includes = {},
        uint16_t handle = 0)
        : AdaptorInterfaces(connection, std::move(object_path))
        , uuid_(std::move(uuid))
        , primary_(primary)
        , device_(std::move(device))
        , includes_(std::move(includes))
        , handle_(handle)
    {
       registerAdaptor();
    }
    ~GattService()
    {
        unregisterAdaptor();
    }

private:
    std::string UUID() override
    {
        return uuid_;
    }

    bool Primary() override
    {
        return primary_;
    }

    sdbus::ObjectPath Device() override
    {
        return device_;
    }

    std::vector<sdbus::ObjectPath> Includes() override
    {
        return includes_;
    }

    uint16_t Handle() override
    {
        return handle_;
    }

    void Handle(const uint16_t& value) override
    {
        handle_ = value;
    }
};

class Application final: public sdbus::AdaptorInterfaces<sdbus::ObjectManager_adaptor>
{
public:
    Application(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
        : AdaptorInterfaces(connection, std::move(object_path))
    {
       registerAdaptor();
    }
    ~Application()
    {
        unregisterAdaptor();
    }

};
class GattManager final: public sdbus::ProxyInterfaces<org::bluez::GattManager1_proxy>
{
public:
    GattManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
        : ProxyInterfaces(connection, sdbus::ServiceName("org.bluez"), std::move(object_path))
    {
       registerProxy();
    }
    ~GattManager()
    {
        unregisterProxy();
    }

};

class AdvertisingManager final: public sdbus::ProxyInterfaces<org::bluez::LEAdvertisingManager1_proxy>
{
    std::promise<std::optional<sdbus::Error>> register_adv_promise_;
    std::future<std::optional<sdbus::Error>> register_adv_future_;

public:
    AdvertisingManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
        : ProxyInterfaces(connection, sdbus::ServiceName("org.bluez"), std::move(object_path))
    {
        registerProxy();
    }
    ~AdvertisingManager()
    {
        unregisterProxy();
    }

    sdbus::PendingAsyncCall RegisterAdvertisementAsync(
        const sdbus::ObjectPath& advertisement,
        const std::map<std::string, sdbus::Variant>& options)
    {
        register_adv_promise_ = std::promise<std::optional<sdbus::Error>>{};
        register_adv_future_ = register_adv_promise_.get_future();
        return RegisterAdvertisement(advertisement, options);
    }

    std::optional<sdbus::Error> WaitForRegisterAdvertisement()
    {
        return register_adv_future_.get();
    }

private:
    void onRegisterAdvertisementReply(std::optional<sdbus::Error> error) override
    {
        register_adv_promise_.set_value(std::move(error));
    }
};

class LEAdvertisement final: public sdbus::AdaptorInterfaces<org::bluez::LEAdvertisement1_adaptor>
{
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

private:
    Config config_;

public:
    LEAdvertisement(
        sdbus::IConnection& connection,
        sdbus::ObjectPath object_path,
        Config config)
        : AdaptorInterfaces(connection, std::move(object_path))
        , config_(std::move(config))
    {
        registerAdaptor();
    }

    ~LEAdvertisement()
    {
        unregisterAdaptor();
    }

private:
    void Release() override
    {
    }

    std::string Type() override
    {
        return config_.type;
    }

    std::vector<std::string> ServiceUUIDs() override
    {
        return config_.service_uuids;
    }

    std::map<uint16_t, sdbus::Variant> ManufacturerData() override
    {
        return config_.manufacturer_data;
    }

    std::vector<std::string> SolicitUUIDs() override
    {
        return config_.solicit_uuids;
    }

    std::map<std::string, sdbus::Variant> ServiceData() override
    {
        return config_.service_data;
    }

    std::map<uint8_t, sdbus::Variant> Data() override
    {
        return config_.data;
    }

    std::vector<std::string> ScanResponseServiceUUIDs() override
    {
        return config_.scan_response_service_uuids;
    }

    std::map<uint16_t, sdbus::Variant> ScanResponseManufacturerData() override
    {
        return config_.scan_response_manufacturer_data;
    }

    std::vector<std::string> ScanResponseSolicitUUIDs() override
    {
        return config_.scan_response_solicit_uuids;
    }

    std::map<std::string, sdbus::Variant> ScanResponseServiceData() override
    {
        return config_.scan_response_service_data;
    }

    std::map<uint8_t, sdbus::Variant> ScanResponseData() override
    {
        return config_.scan_response_data;
    }

    bool Discoverable() override
    {
        return config_.discoverable;
    }

    uint16_t DiscoverableTimeout() override
    {
        return config_.discoverable_timeout;
    }

    std::vector<std::string> Includes() override
    {
        return config_.includes;
    }

    std::string LocalName() override
    {
        return config_.local_name;
    }

    uint16_t Appearance() override
    {
        return config_.appearance;
    }

    uint16_t Duration() override
    {
        return config_.duration;
    }

    uint16_t Timeout() override
    {
        return config_.timeout;
    }

    std::string SecondaryChannel() override
    {
        return config_.secondary_channel;
    }

    uint32_t MinInterval() override
    {
        return config_.min_interval_ms;
    }

    uint32_t MaxInterval() override
    {
        return config_.max_interval_ms;
    }

    int16_t TxPower() override
    {
        return config_.tx_power;
    }
};
#include <stdint.h>

static const std::vector<uint8_t> mouse_hid_report_map {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        // Report ID(1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)

    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)

    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const,Var,Abs) - padding

    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)

    0xC0,              //   End Collection
    0xC0               // End Collection
};

int main()
{
    try {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        rodent::bluez::BluezClient client;
        const auto managedObjects = client.getManagedObjects();

        auto application = Application(client.connection(), sdbus::ObjectPath("/rodent"));
        auto manager = GattManager(client.connection(), sdbus::ObjectPath("/org/bluez/hci0"));
        auto adv_manager = AdvertisingManager(client.connection(), sdbus::ObjectPath("/org/bluez/hci0"));

        LEAdvertisement::Config advertisement_config;
        advertisement_config.type = "peripheral";
        advertisement_config.service_uuids = {"00001812-0000-1000-8000-00805f9b34fb"};
        // advertisement_config.service_data = {{"00001812-0000-1000-8000-00805f9b34fb",
        //     sdbus::Variant(std::vector<uint8_t>{0x05})}};
        advertisement_config.local_name = "FabricMouse";
        advertisement_config.appearance = static_cast<uint16_t>(0x03C2);
        advertisement_config.min_interval_ms = 30;
        advertisement_config.max_interval_ms = 50;
        auto advertisement = LEAdvertisement(client.connection(), sdbus::ObjectPath("/rodent/advert0"), std::move(advertisement_config));
        // add battery service
        auto batt_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0" ),
            "0000180f-0000-1000-8000-00805f9b34fb"
        );
        batt_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        auto batt_level_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0/char0"),
            "00002a19-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service0"),
            {100},
            {"read", "notify"}

        );
        batt_level_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        // add HID Service
        auto hid_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1"),
            "00001812-0000-1000-8000-00805f9b34fb"
            );
        hid_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        // TODO: add HID chars
        // protocolmode, HIDInfoChars, ControlPointChars, ReportMapChars, ReportMapj

        auto protocol_mode = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char0"),
            "00002a4e-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01},
            {"read", "write-without-response"}

        );
        protocol_mode.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto hid_info = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char1"),
            "00002a4a-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01, 0x11, 0x00, 0x02},
            {"read"}

        );
        hid_info.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto control_point_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char2"),
            "00002a4c-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00},
            {"write-without-response"}

        );
        control_point_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto report_map_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char3"),
            "00002a4b-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            mouse_hid_report_map,
            {"read"}

        );

        report_map_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto report_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4"),
            "00002a4d-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00},
            {"notify", "read"}

        );
        report_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto report_desc = GattDesc(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4/desc0"),
            "00002908-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1/char4"),
            std::vector<uint8_t>{0x01,0x01},
            {"read"}
        );
        report_desc.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattDescriptor1")});

        // add DeviceInfoService
        auto device_info_service =GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2"),
            "0000180a-0000-1000-8000-00805f9b34fb"
        );
        device_info_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});
        // TODO: add DeviceInfo chars.
        std::string vendor_name = "kidfromjupiter";
        std::string product_name = "FabricMouse";
        auto vendor_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char0"),
            "00002a29-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t> (vendor_name.begin(), vendor_name.end()),
            {"read"}
        );
        vendor_char.emitInterfacesAddedSignal();


        auto product_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char1"),
            "00002a24-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t> (product_name.begin(), product_name.end()),
            {"read"}
        );
        product_char.emitInterfacesAddedSignal();

        // PnP ID (2A50): [Vendor ID Source, Vendor ID LSB, Vendor ID MSB, Product ID LSB,
        // Product ID MSB, Product Version LSB, Product Version MSB]
        auto pnp_id_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char2"),
            "00002a50-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t>{0x01, 0xE0, 0x00, 0x01, 0x00, 0x00, 0x01},
            {"read"}
        );
        pnp_id_char.emitInterfacesAddedSignal();


        try
        {
            auto register_future = manager.RegisterApplication(sdbus::ObjectPath("/rodent"), {});
            register_future.get();
            std::cout << "Register successful\n";
        } catch (const sdbus::Error& e)
        {
            std::cout << "D-Bus e: " << e.getName() << ": " << e.getMessage() << '\n';
            return 1;
        } catch (const std::exception& e)
        {
            std::cout << "RegisterApplication failed: " << e.what() << '\n';
            return 1;
        }

        try
        {
            auto register_adv_pending = adv_manager.RegisterAdvertisementAsync(sdbus::ObjectPath("/rodent/advert0"), {});
            (void)register_adv_pending;
            auto register_adv_error = adv_manager.WaitForRegisterAdvertisement();
            if (register_adv_error.has_value()) {
                std::cout << "RegisterAdvertisement D-Bus e: "
                          << register_adv_error->getName() << ": "
                          << register_adv_error->getMessage() << '\n';
                return 1;
            }

            std::cout << "Advertisement register successful\n";
        } catch (const std::exception& e)
        {
            std::cout << "RegisterAdvertisement failed: " << e.what() << '\n';
            return 1;
        }
        std::cout << "Exported GATT application at /rodent. Waiting for Ctrl+C...\n";

        uint8_t buttons = 0x00;
        int8_t dx = 5;

        auto next_mouse_update = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        while (!g_shouldStop) {
            const auto now = std::chrono::steady_clock::now();

            if (now >= next_mouse_update) {
                if (report_char.NotificationEnabled()) {
                    // Move right
                    report_char.SetValueAndNotify({
                        buttons,                         // buttons
                        static_cast<uint8_t>(dx),       // x
                        static_cast<uint8_t>(0),        // y
                        static_cast<uint8_t>(0)         // wheel
                    });

                    // Optional: send neutral report after movement
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    report_char.SetValueAndNotify({
                        0x00, 0x00, 0x00, 0x00
                    });

                    dx = -dx; // alternate left/right
                }

                next_mouse_update = now + std::chrono::milliseconds(800);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }


    } catch (const sdbus::Error& error) {
        std::cerr << "D-Bus error: " << error.getName() << ": " << error.getMessage() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
