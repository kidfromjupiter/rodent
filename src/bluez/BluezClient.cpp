#include "BluezClient.h"

#include <utility>

namespace rodent::bluez {

void SilentPropertiesProxy::onPropertiesChanged(
    const sdbus::InterfaceName&,
    const std::map<sdbus::PropertyName, sdbus::Variant>&,
    const std::vector<sdbus::PropertyName>&)
{
}

RootProxy::RootProxy(sdbus::IConnection& connection)
    : ProxyInterfaces(connection, sdbus::ServiceName{kServiceName}, sdbus::ObjectPath{kRootObjectPath})
{
    registerProxy();
}

RootProxy::~RootProxy()
{
    unregisterProxy();
}

ManagerProxy::ManagerProxy(sdbus::IConnection& connection)
    : ProxyInterfaces(connection, sdbus::ServiceName{kServiceName}, sdbus::ObjectPath{kManagerObjectPath})
{
    registerProxy();
}

ManagerProxy::~ManagerProxy()
{
    unregisterProxy();
}

void ManagerProxy::onInterfacesAdded(const sdbus::ObjectPath&, const InterfaceMap&)
{
}

void ManagerProxy::onInterfacesRemoved(const sdbus::ObjectPath&, const std::vector<sdbus::InterfaceName>&)
{
}

AdapterProxy::AdapterProxy(sdbus::IConnection& connection, sdbus::ObjectPath objectPath)
    : ProxyInterfaces(connection, sdbus::ServiceName{kServiceName}, objectPath)
    , objectPath_(std::move(objectPath))
{
    registerProxy();
}

AdapterProxy::~AdapterProxy()
{
    unregisterProxy();
}

const sdbus::ObjectPath& AdapterProxy::objectPath() const noexcept
{
    return objectPath_;
}

void AdapterProxy::onRegisterAdvertisementReply(std::optional<sdbus::Error>)
{
}

DeviceProxy::DeviceProxy(sdbus::IConnection& connection, sdbus::ObjectPath objectPath)
    : ProxyInterfaces(connection, sdbus::ServiceName{kServiceName}, objectPath)
    , objectPath_(std::move(objectPath))
{
    registerProxy();
}

DeviceProxy::~DeviceProxy()
{
    unregisterProxy();
}

const sdbus::ObjectPath& DeviceProxy::objectPath() const noexcept
{
    return objectPath_;
}

void DeviceProxy::onDisconnected(const std::string&, const std::string&)
{
}

BluezClient::BluezClient()
    : connection_(sdbus::createSystemBusConnection())
    , root_(std::make_unique<RootProxy>(*connection_))
    , manager_(std::make_unique<ManagerProxy>(*connection_))
{
    connection_->enterEventLoopAsync();
}

BluezClient::~BluezClient()
{
    connection_->leaveEventLoop();
}

sdbus::IConnection& BluezClient::connection() const noexcept
{
    return *connection_;
}

RootProxy& BluezClient::root() const noexcept
{
    return *root_;
}

ManagerProxy& BluezClient::manager() const noexcept
{
    return *manager_;
}

ManagedObjects BluezClient::getManagedObjects() const
{
    return manager_->GetManagedObjects();
}

std::vector<sdbus::ObjectPath> BluezClient::listObjectsWithInterface(std::string_view interfaceName) const
{
    std::vector<sdbus::ObjectPath> paths;
    for (const auto& [objectPath, interfaces] : getManagedObjects()) {
        if (hasInterface(interfaces, interfaceName)) {
            paths.push_back(objectPath);
        }
    }
    return paths;
}

std::vector<sdbus::ObjectPath> BluezClient::listAdapters() const
{
    return listObjectsWithInterface(org::bluez::Adapter1_proxy::INTERFACE_NAME);
}

std::vector<sdbus::ObjectPath> BluezClient::listDevices() const
{
    return listObjectsWithInterface(org::bluez::Device1_proxy::INTERFACE_NAME);
}

std::unique_ptr<AdapterProxy> BluezClient::createAdapter(const sdbus::ObjectPath& objectPath) const
{
    return std::make_unique<AdapterProxy>(*connection_, objectPath);
}

std::unique_ptr<DeviceProxy> BluezClient::createDevice(const sdbus::ObjectPath& objectPath) const
{
    return std::make_unique<DeviceProxy>(*connection_, objectPath);
}

bool hasInterface(const InterfaceMap& interfaces, std::string_view interfaceName)
{
    return interfaces.contains(sdbus::InterfaceName{std::string{interfaceName}});
}

}  // namespace rodent::bluez
