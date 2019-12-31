#include "../DeviceProperties.h"

#include <bcm_host.h>

namespace WPEFramework {
namespace Device {
namespace Implementation {

class RPIPlatform : public Plugin::IDeviceProperties, public Plugin::IGraphicsProperties, Plugin::IConnectionProperties, public Core::IReferenceCounted {
public:
    RPIPlatform()
        : _refCount(0)
        , _adminLock() {
        bcm_host_init();
        UpdateTotalGpuRam(_totalGpuRam);
    }

    RPIPlatform(const RPIPlatform&) = delete;
    RPIPlatform& operator= (const RPIPlatform&) = delete;
    virtual ~RPIPlatform()
    {
        bcm_host_deinit();
    }

public:
    void AddRef() const override
    {
        Core::InterlockedIncrement(_refCount);
    }
    uint32_t Release() const override
    {
        Core::InterlockedDecrement(_refcount);
        return (Core::ERROR_NONE);
    }

    // Device Propertirs interface
    const std::string Chipset() const override
    {
        return string();
    }
    const std::string FirmwareVersion() const override
    {
        return string();
    }
    Core::ProxyType<IGraphicsProperties>  GraphicsInstance() override
    {
        return static_cast<Core::ProxyType<Plugin::IGraphicsProperties>>(*this);
    }
    Core::ProxyType<IConnectionProperties>  ConnectionInstance() override
    {
        return static_cast<Core::ProxyType<Plugin::IConnectionProperties>>(*this);
    }

    // Graphics Properties interface
    uint64_t TotalGpuRam() const override
    {
        return _totalGpuRam;
    }
    uint64_t FreeGpuRam() const override
    {
        uint64_t result;
        Command("get_mem reloc ", result);
        return (result);
    }

    // Connection Properties interface
    uint32_t Register(INotification* notification) override
    {
        _adminLock.Lock();

        // Make sure a sink is not registered multiple times.
        ASSERT(std::find(_observers.begin(), _observers.end(), notification) == _observers.end());

        _observers.push_back(notification);
        notification->AddRef();

        _adminLock.Unlock();

        return (Core::ERROR_NONE);
    }
    uint32_t Unregister(INotification* notification) override
    {
        _adminLock.Lock();

        std::list<IConnectionProperties::INotification*>::iterator index(std::find(_observers.begin(), _observers.end(), notification));

        // Make sure you do not unregister something you did not register !!!
        ASSERT(index != _observers.end());

        if (index != _observers.end()) {
            (*index)->Release();
            _observers.erase(index);
        }

        _adminLock.Unlock();

        return (Core::ERROR_NONE);
    }

    bool IsAudioPassThrough () const override
    {
        return false;
    }
    bool Connected() const override
    {
        return false;
    }
    uint32_t Width() const override
    {
        uint32_t width = 0;
        return width;
    }
    uint32_t Height() const override
    {
        uint32_t height = 0;
        return height;
    }
    uint8_t HDCPMajor() const override
    {
        uint8_t major = 0;

        return major;
    }
    uint8_t HDCPMinor() const override
    {
        uint8_t minor = 0;
        return minor;
    }
    HDRType Type() const override
    {
        HDRType type = HDR_OFF;
        return type;
    }

private:
    inline void UpdateTotalGpuRam(uint64_t& totalRam)
    {
        Command("get_mem reloc_total ", totalRam);
    }
    void Command(const char request[], std::string& value) const
    {
        char buffer[512];

        // Reset the string
        buffer[0] = '\0';

        // Most VC API calls are guarded but we want to be sure anyway
        _adminLock.Lock();

        int VARIABLE_IS_NOT_USED status = vc_gencmd(buffer, sizeof(buffer), &request[0]);
        assert((status == 0) && "Error: vc_gencmd failed.\n");

        _adminLock.Unlock();

        // Make sure it is null-terminated
        buffer[sizeof(buffer) - 1] = '\0';

        // We do not need the stuff that is before the '=', we know what we requested :-)
        char* equal = strchr(buffer, '=');
        if (equal != nullptr) {
            equal++;
        }
        else {
            equal = buffer;
        }

        // Create string from buffer.
        value = std::string (equal);
    }
    template<typename VALUE>
    void Command(const char request[], VALUE& result) const
    {
        std::string response;

        Command(request, response);

        const char* start = response.c_str();
        const char* index = start;

        // move the index to the unit inidicatuion type
        while (::isdigit(*index) || (*index == ' ') || (*index == '.') || (*index == ',')) {
            index++;
        }

        result = WPEFramework::Core::NumberType<VALUE>(WPEFramework::Core::TextFragment(start, (index - start))).Value();

        // Convert into bytes, if necessary.
        if ( (*index == 'M') && (index[1] == '\0') ) {
            // Multiply with MB
            result *= (1024 * 1024);
        }
        else if ( (*index == 'K') && (index[1] == '\0') ) {
            // Multiply with KB
            result *= 1024;
        }
    }

private:
    mutable uint32_t _refCount;
    uint64_t _totalGpuRam;
    std::list<IConnectionProperties::INotification*> _observers;

    mutable WPEFramework::Core::CriticalSection _adminLock;
};
}
}

/* static */ Core::ProxyType<Plugin::IDeviceProperties> Plugin::IDeviceProperties::Instance()
{
    static Device::Implementation::RPIPlatform rpiPlatform;
    return static_cast<Core::ProxyType<Plugin::IDeviceProperties>>(rpiPlatform);
}
}