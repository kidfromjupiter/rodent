#pragma once

#include "generated/bluez/proxies/org_bluez_Adapter1_proxy.h"
#include "generated/bluez/proxies/org_bluez_Device1_proxy.h"

#include <sdbus-c++/ProxyInterfaces.h>
#include <sdbus-c++/StandardInterfaces.h>

#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace rodent::bluez {

inline constexpr auto kServiceName = "org.bluez";
inline constexpr auto kManagerObjectPath = "/";

using PropertyMap = std::map<sdbus::PropertyName, sdbus::Variant>;
using InterfaceMap = std::map<sdbus::InterfaceName, PropertyMap>;
using ManagedObjects = std::map<sdbus::ObjectPath, InterfaceMap>;

class ManagerProxy final : public sdbus::ProxyInterfaces<sdbus::ObjectManager_proxy> {
public:
    explicit ManagerProxy(sdbus::IConnection& connection);
    ~ManagerProxy();

private:
    void onInterfacesAdded(const sdbus::ObjectPath& objectPath, const InterfaceMap& interfacesAndProperties) override;
    void onInterfacesRemoved(const sdbus::ObjectPath& objectPath, const std::vector<sdbus::InterfaceName>& interfaces) override;
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
    [[nodiscard]] ManagerProxy& manager() const noexcept;
    [[nodiscard]] ManagedObjects getManagedObjects() const;

    [[nodiscard]] std::vector<sdbus::ObjectPath> listObjectsWithInterface(std::string_view interfaceName) const;
    [[nodiscard]] std::vector<sdbus::ObjectPath> listAdapters() const;
    [[nodiscard]] std::vector<sdbus::ObjectPath> listDevices() const;

    void setAdapterAlias(std::string_view alias);
    void clearAdapterAlias();

private:
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<ManagerProxy> manager_;
};

[[nodiscard]] bool hasInterface(const InterfaceMap& interfaces, std::string_view interfaceName);

}  // namespace rodent::bluez
