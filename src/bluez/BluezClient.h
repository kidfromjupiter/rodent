#pragma once

#include "generated/bluez/bluez_all_proxies.h"

#include <sdbus-c++/ProxyInterfaces.h>
#include <sdbus-c++/StandardInterfaces.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace rodent::bluez {

inline constexpr auto kServiceName = "org.bluez";
inline constexpr auto kRootObjectPath = "/org/bluez";
inline constexpr auto kManagerObjectPath = "/";

using PropertyMap = std::map<sdbus::PropertyName, sdbus::Variant>;
using InterfaceMap = std::map<sdbus::InterfaceName, PropertyMap>;
using ManagedObjects = std::map<sdbus::ObjectPath, InterfaceMap>;

class SilentPropertiesProxy : public sdbus::Properties_proxy {
public:
    using sdbus::Properties_proxy::Properties_proxy;

private:
    void onPropertiesChanged(
        const sdbus::InterfaceName& interfaceName,
        const std::map<sdbus::PropertyName, sdbus::Variant>& changedProperties,
        const std::vector<sdbus::PropertyName>& invalidatedProperties) override;
};

class RootProxy final : public sdbus::ProxyInterfaces<
                            sdbus::Introspectable_proxy,
                            org::bluez::AgentManager1_proxy,
                            org::bluez::ProfileManager1_proxy> {
public:
    explicit RootProxy(sdbus::IConnection& connection);
    ~RootProxy();
};

class ManagerProxy final : public sdbus::ProxyInterfaces<
                               sdbus::Introspectable_proxy,
                               sdbus::ObjectManager_proxy> {
public:
    explicit ManagerProxy(sdbus::IConnection& connection);
    ~ManagerProxy();

private:
    void onInterfacesAdded(const sdbus::ObjectPath& objectPath, const InterfaceMap& interfacesAndProperties) override;
    void onInterfacesRemoved(const sdbus::ObjectPath& objectPath, const std::vector<sdbus::InterfaceName>& interfaces) override;
};

class AdapterProxy final : public sdbus::ProxyInterfaces<
                               SilentPropertiesProxy,
                               org::bluez::Adapter1_proxy,
                               org::bluez::BatteryProviderManager1_proxy,
                               org::bluez::GattManager1_proxy,
                               org::bluez::AdvertisementMonitorManager1_proxy,
                               org::bluez::Media1_proxy,
                               org::bluez::NetworkServer1_proxy,
                               org::bluez::LEAdvertisingManager1_proxy> {
public:
    AdapterProxy(sdbus::IConnection& connection, sdbus::ObjectPath objectPath);
    ~AdapterProxy();

    [[nodiscard]] const sdbus::ObjectPath& objectPath() const noexcept;

private:
    void onRegisterAdvertisementReply(std::optional<sdbus::Error> error) override;

    sdbus::ObjectPath objectPath_;
};

class DeviceProxy final : public sdbus::ProxyInterfaces<
                              SilentPropertiesProxy,
                              org::bluez::Device1_proxy,
                              org::bluez::Network1_proxy,
                              org::bluez::MediaControl1_proxy,
                              org::bluez::Bearer::BREDR1_proxy,
                              org::bluez::Bearer::LE1_proxy> {
public:
    DeviceProxy(sdbus::IConnection& connection, sdbus::ObjectPath objectPath);
    ~DeviceProxy();

    [[nodiscard]] const sdbus::ObjectPath& objectPath() const noexcept;

private:
    void onDisconnected(const std::string& name, const std::string& message) override;

    sdbus::ObjectPath objectPath_;
};

class BluezClient final {
public:
    BluezClient();
    ~BluezClient();

    BluezClient(const BluezClient&) = delete;
    BluezClient& operator=(const BluezClient&) = delete;
    BluezClient(BluezClient&&) = delete;
    BluezClient& operator=(BluezClient&&) = delete;

    [[nodiscard]] sdbus::IConnection& connection() const noexcept;
    [[nodiscard]] RootProxy& root() const noexcept;
    [[nodiscard]] ManagerProxy& manager() const noexcept;
    [[nodiscard]] ManagedObjects getManagedObjects() const;

    [[nodiscard]] std::vector<sdbus::ObjectPath> listObjectsWithInterface(std::string_view interfaceName) const;
    [[nodiscard]] std::vector<sdbus::ObjectPath> listAdapters() const;
    [[nodiscard]] std::vector<sdbus::ObjectPath> listDevices() const;

    [[nodiscard]] std::unique_ptr<AdapterProxy> createAdapter(const sdbus::ObjectPath& objectPath) const;
    [[nodiscard]] std::unique_ptr<DeviceProxy> createDevice(const sdbus::ObjectPath& objectPath) const;

private:
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<RootProxy> root_;
    std::unique_ptr<ManagerProxy> manager_;
};

[[nodiscard]] bool hasInterface(const InterfaceMap& interfaces, std::string_view interfaceName);

}  // namespace rodent::bluez
