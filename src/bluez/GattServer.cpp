#include "GattServer.h"

#include <algorithm>
#include <utility>

namespace rodent::bluez {

GattChar::GattChar(
    sdbus::IConnection& connection,
    sdbus::ObjectPath object_path,
    std::string char_uuid,
    sdbus::ObjectPath service,
    std::vector<uint8_t> value,
    std::vector<std::string> flags)
    : AdaptorInterfaces(connection, std::move(object_path))
    , char_uuid_(std::move(char_uuid))
    , service_(std::move(service))
    , value_(std::move(value))
    , flags_(std::move(flags))
{
    registerAdaptor();
}

GattChar::~GattChar()
{
    unregisterAdaptor();
}

void GattChar::SetValueAndNotify(std::vector<uint8_t> value)
{
    value_ = std::move(value);

    if (notifying_) {
        emitPropertiesChangedSignal(
            sdbus::InterfaceName{org::bluez::GattCharacteristic1_adaptor::INTERFACE_NAME},
            {sdbus::PropertyName{"Value"}});
    }
}

bool GattChar::NotificationEnabled() const
{
    return notifying_;
}

size_t GattChar::MaxValueLen() const
{
    const size_t capped = std::min<size_t>(mtu_, 515U);
    return capped >= 3 ? (capped - 3) : 0U;
}

void GattChar::ReadValue(
    sdbus::Result<std::vector<uint8_t>>&& result,
    std::map<std::string, sdbus::Variant> options)
{
    const auto mtu_it = options.find("mtu");
    if (mtu_it != options.end()) {
        try {
            mtu_ = mtu_it->second.get<uint16_t>();
        } catch (...) {
        }
    }
    result.returnResults(value_);
}

void GattChar::WriteValue(
    sdbus::Result<>&& result,
    std::vector<uint8_t> value,
    std::map<std::string, sdbus::Variant> options)
{
    const auto mtu_it = options.find("mtu");
    if (mtu_it != options.end()) {
        try {
            mtu_ = mtu_it->second.get<uint16_t>();
        } catch (...) {
        }
    }
    value_ = std::move(value);
    result.returnResults();
}

void GattChar::StartNotify()
{
    notifying_ = true;
}

void GattChar::StopNotify()
{
    notifying_ = false;
    notify_acquired_ = false;
}

void GattChar::Confirm()
{
}

std::string GattChar::UUID()
{
    return char_uuid_;
}

sdbus::ObjectPath GattChar::Service()
{
    return service_;
}

std::vector<uint8_t> GattChar::Value()
{
    return value_;
}

bool GattChar::Notifying()
{
    return notifying_;
}

std::vector<std::string> GattChar::Flags()
{
    return flags_;
}

GattDesc::GattDesc(
    sdbus::IConnection& connection,
    sdbus::ObjectPath object_path,
    std::string uuid,
    sdbus::ObjectPath characteristic,
    std::vector<uint8_t> value,
    std::vector<std::string> flags,
    uint16_t handle)
    : AdaptorInterfaces(connection, std::move(object_path))
    , uuid_(std::move(uuid))
    , characteristic_(std::move(characteristic))
    , value_(std::move(value))
    , flags_(std::move(flags))
    , handle_(handle)
{
    registerAdaptor();
}

GattDesc::~GattDesc()
{
    unregisterAdaptor();
}

std::vector<uint8_t> GattDesc::ReadValue(const std::map<std::string, sdbus::Variant>&)
{
    return value_;
}

void GattDesc::WriteValue(const std::vector<uint8_t>& value, const std::map<std::string, sdbus::Variant>&)
{
    value_ = value;
}

std::string GattDesc::UUID()
{
    return uuid_;
}

sdbus::ObjectPath GattDesc::Characteristic()
{
    return characteristic_;
}

std::vector<uint8_t> GattDesc::Value()
{
    return value_;
}

std::vector<std::string> GattDesc::Flags()
{
    return flags_;
}

uint16_t GattDesc::Handle()
{
    return handle_;
}

void GattDesc::Handle(const uint16_t& value)
{
    handle_ = value;
}

GattService::GattService(
    sdbus::IConnection& connection,
    sdbus::ObjectPath object_path,
    std::string uuid,
    bool primary,
    sdbus::ObjectPath device,
    std::vector<sdbus::ObjectPath> includes,
    uint16_t handle)
    : AdaptorInterfaces(connection, std::move(object_path))
    , uuid_(std::move(uuid))
    , primary_(primary)
    , device_(std::move(device))
    , includes_(std::move(includes))
    , handle_(handle)
{
    registerAdaptor();
}

GattService::~GattService()
{
    unregisterAdaptor();
}

std::string GattService::UUID()
{
    return uuid_;
}

bool GattService::Primary()
{
    return primary_;
}

sdbus::ObjectPath GattService::Device()
{
    return device_;
}

std::vector<sdbus::ObjectPath> GattService::Includes()
{
    return includes_;
}

uint16_t GattService::Handle()
{
    return handle_;
}

void GattService::Handle(const uint16_t& value)
{
    handle_ = value;
}

Application::Application(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
    : AdaptorInterfaces(connection, std::move(object_path))
{
    registerAdaptor();
}

Application::~Application()
{
    unregisterAdaptor();
}

GattManager::GattManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
    : ProxyInterfaces(connection, sdbus::ServiceName("org.bluez"), std::move(object_path))
{
    registerProxy();
}

GattManager::~GattManager()
{
    unregisterProxy();
}

AdvertisingManager::AdvertisingManager(sdbus::IConnection& connection, sdbus::ObjectPath object_path)
    : ProxyInterfaces(connection, sdbus::ServiceName("org.bluez"), std::move(object_path))
{
    registerProxy();
}

AdvertisingManager::~AdvertisingManager()
{
    unregisterProxy();
}

sdbus::PendingAsyncCall AdvertisingManager::RegisterAdvertisementAsync(
    const sdbus::ObjectPath& advertisement,
    const std::map<std::string, sdbus::Variant>& options)
{
    register_adv_promise_ = std::promise<std::optional<sdbus::Error>>{};
    register_adv_future_ = register_adv_promise_.get_future();
    return RegisterAdvertisement(advertisement, options);
}

std::optional<sdbus::Error> AdvertisingManager::WaitForRegisterAdvertisement()
{
    return register_adv_future_.get();
}

void AdvertisingManager::onRegisterAdvertisementReply(std::optional<sdbus::Error> error)
{
    register_adv_promise_.set_value(std::move(error));
}

LEAdvertisement::LEAdvertisement(sdbus::IConnection& connection, sdbus::ObjectPath object_path, Config config)
    : AdaptorInterfaces(connection, std::move(object_path))
    , config_(std::move(config))
{
    registerAdaptor();
}

LEAdvertisement::~LEAdvertisement()
{
    unregisterAdaptor();
}

void LEAdvertisement::Release()
{
}

std::string LEAdvertisement::Type()
{
    return config_.type;
}

std::vector<std::string> LEAdvertisement::ServiceUUIDs()
{
    return config_.service_uuids;
}

std::map<uint16_t, sdbus::Variant> LEAdvertisement::ManufacturerData()
{
    return config_.manufacturer_data;
}

std::vector<std::string> LEAdvertisement::SolicitUUIDs()
{
    return config_.solicit_uuids;
}

std::map<std::string, sdbus::Variant> LEAdvertisement::ServiceData()
{
    return config_.service_data;
}

std::map<uint8_t, sdbus::Variant> LEAdvertisement::Data()
{
    return config_.data;
}

std::vector<std::string> LEAdvertisement::ScanResponseServiceUUIDs()
{
    return config_.scan_response_service_uuids;
}

std::map<uint16_t, sdbus::Variant> LEAdvertisement::ScanResponseManufacturerData()
{
    return config_.scan_response_manufacturer_data;
}

std::vector<std::string> LEAdvertisement::ScanResponseSolicitUUIDs()
{
    return config_.scan_response_solicit_uuids;
}

std::map<std::string, sdbus::Variant> LEAdvertisement::ScanResponseServiceData()
{
    return config_.scan_response_service_data;
}

std::map<uint8_t, sdbus::Variant> LEAdvertisement::ScanResponseData()
{
    return config_.scan_response_data;
}

bool LEAdvertisement::Discoverable()
{
    return config_.discoverable;
}

uint16_t LEAdvertisement::DiscoverableTimeout()
{
    return config_.discoverable_timeout;
}

std::vector<std::string> LEAdvertisement::Includes()
{
    return config_.includes;
}

std::string LEAdvertisement::LocalName()
{
    return config_.local_name;
}

uint16_t LEAdvertisement::Appearance()
{
    return config_.appearance;
}

uint16_t LEAdvertisement::Duration()
{
    return config_.duration;
}

uint16_t LEAdvertisement::Timeout()
{
    return config_.timeout;
}

std::string LEAdvertisement::SecondaryChannel()
{
    return config_.secondary_channel;
}

uint32_t LEAdvertisement::MinInterval()
{
    return config_.min_interval_ms;
}

uint32_t LEAdvertisement::MaxInterval()
{
    return config_.max_interval_ms;
}

int16_t LEAdvertisement::TxPower()
{
    return config_.tx_power;
}

}  // namespace rodent::bluez
