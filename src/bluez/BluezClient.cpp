#include "BluezClient.h"

namespace rodent::bluez {

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

BluezClient::BluezClient()
    : connection_(sdbus::createSystemBusConnection())
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

bool hasInterface(const InterfaceMap& interfaces, std::string_view interfaceName)
{
    return interfaces.contains(sdbus::InterfaceName{std::string{interfaceName}});
}

}  // namespace rodent::bluez
