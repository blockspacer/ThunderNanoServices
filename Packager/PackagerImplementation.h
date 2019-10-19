#pragma once

#include "Module.h"
#include <interfaces/IPackager.h>

#include <list>
#include <string>

// Forward declarations so we do not need to include the OPKG headers here.
struct opkg_conf;
struct _opkg_progress_data_t;

namespace WPEFramework {
namespace Plugin {

    class PackagerImplementation : public Exchange::IPackager {
    public:
        PackagerImplementation(const PackagerImplementation&) = delete;
        PackagerImplementation& operator=(const PackagerImplementation&) = delete;

        class EXTERNAL Config : public Core::JSON::Container {
        public:
            Config()
                : Core::JSON::Container()
                , TempDir()                     // Specify tmp-dir.
                , CacheDir()                    // Specify cache directory
                , MakeCacheVolatile(false)      // Use volatile cache. Volatile cache will be cleared on exit
                , Verbosity()
                , NoDeps()
                , NoSignatureCheck()
                , AlwaysUpdateFirst()
            {
                Add(_T("config"), &ConfigFile);
                Add(_T("temppath"), &TempDir);
                Add(_T("cachepath"), &CacheDir);
                Add(_T("volatilecache"), &MakeCacheVolatile);
                Add(_T("verbosity"), &Verbosity);
                Add(_T("nodeps"), &NoDeps);
                Add(_T("nosignaturecheck"), &NoSignatureCheck);
                Add(_T("alwaysupdatefirst"), &AlwaysUpdateFirst);
            }

            ~Config() override
            {
            }

            Config(const Config&) = delete;
            Config& operator=(const Config&) = delete;

            Core::JSON::String  ConfigFile;
            Core::JSON::String  TempDir;
            Core::JSON::String  CacheDir;
            Core::JSON::Boolean MakeCacheVolatile;
            Core::JSON::DecUInt8 Verbosity;
            Core::JSON::Boolean NoDeps;
            Core::JSON::Boolean NoSignatureCheck;
            Core::JSON::Boolean AlwaysUpdateFirst;
        };

        PackagerImplementation()
            : _adminLock()
            , _configFile()
            , _tempPath()
            , _cachePath()
            , _verbosity(0)
            , _noDeps(false)
            , _skipSignatureChecking(false)
            , _alwaysUpdateFirst(false)
            , _volatileCache(false)
            , _opkgInitialized(false)
            , _worker(this)
            , _isUpgrade(false)
            , _actitity(ActivityType::NONE)
        {
        }

        ~PackagerImplementation() override;

        BEGIN_INTERFACE_MAP(PackagerImplementation)
            INTERFACE_ENTRY(Exchange::IPackager)
        END_INTERFACE_MAP

        //   IPackager methods
        void Register(Exchange::IPackager::INotification* observer) override;
        void Unregister(const Exchange::IPackager::INotification* observer) override;
        uint32_t Configure(PluginHost::IShell* service) override;
        uint32_t Install(const string& name, const string& version, const string& arch, bool downloadOnly) override;
        uint32_t SynchronizeRepository() override;

    private:
        class PackageInfo : public Exchange::IPackager::IPackageInfo {
        public:
            PackageInfo(const PackageInfo&) = delete;
            PackageInfo& operator=(const PackageInfo&) = delete;

            ~PackageInfo() override
            {
            }

            PackageInfo(const std::string& name,
                        const std::string& version,
                        const std::string& arch)
                : _name(name)
                , _version(version)
                , _arch(arch)
            {
            }

            BEGIN_INTERFACE_MAP(PackageInfo)
                INTERFACE_ENTRY(Exchange::IPackager::IPackageInfo)
            END_INTERFACE_MAP

            // IPackageInfo methods
            string Name() const override
            {
                return _name;
            }

            string Version() const override
            {
                return _version;
            }

            string Architecture() const override
            {
                return _arch;
            }

        private:
            std::string _name;
            std::string _version;
            std::string _arch;
        };

        class InstallInfo : public Exchange::IPackager::IInstallationInfo {
        public:
            InstallInfo(const PackageInfo&) = delete;
            InstallInfo& operator=(const PackageInfo&) = delete;

            ~InstallInfo() override
            {
            }

            InstallInfo() = default;

            BEGIN_INTERFACE_MAP(InstallInfo)
                INTERFACE_ENTRY(Exchange::IPackager::IInstallationInfo)
            END_INTERFACE_MAP

            // IInstallationInfo methods
            Exchange::IPackager::state State() const override
            {
                return _state;
            }

            uint8_t Progress() const override
            {
                return _progress;
            }

            uint32_t ErrorCode() const override
            {
                return _error;
            }

            uint32_t Abort() override
            {
                return _error != 0 ? Core::ERROR_NONE : Core::ERROR_UNAVAILABLE;
            }

            void SetState(Exchange::IPackager::state state)
            {
                TRACE_L1("Setting state to %d", state);
                _state = state;
            }

            void SetProgress(uint8_t progress)
            {
                TRACE_L1("Setting progress to %d", progress);
                _progress = progress;
            }

            void SetError(uint32_t err)
            {
                TRACE_L1("Setting error to %d", err);
                _error = err;
            }

        private:
            Exchange::IPackager::state _state = Exchange::IPackager::IDLE;
            uint32_t _error = 0u;
            uint8_t _progress = 0u;
        };

        struct InstallationData {
            InstallationData(const InstallationData& other) = delete;
            InstallationData& operator=(const InstallationData& other) = delete;
            InstallationData() = default;

            ~InstallationData()
            {
                if (Package)
                    Package->Release();
                if (Install)
                    Install->Release();
            }
            PackageInfo* Package = nullptr;
            InstallInfo* Install = nullptr;
        };

        class InstallThread : public Core::Thread {
        public:
            InstallThread(PackagerImplementation* parent)
                : _parent(parent)
            {}

            InstallThread& operator=(const InstallThread&) = delete;
            InstallThread(const InstallThread&) = delete;

            uint32_t Worker() override {
                while(IsRunning() == true) {
                    _parent->_adminLock.Lock(); // The parent may have lock when this starts so wait for it to release.
                    bool isInstall = _parent->_actitity == ActivityType::INSTALL || _parent->_actitity == ActivityType::DOWNLOAD;
                    ASSERT(isInstall != true || _parent->_inProgress.Package != nullptr);
                    _parent->_adminLock.Unlock();

                    // After this point locking is not needed because API running on other threads only read if in
                    // progress is filled in.
                    _parent->BlockingSetupLocalRepoNoLock(isInstall == true ? RepoSyncMode::SETUP : RepoSyncMode::FORCED);
                    if (isInstall)
                        _parent->BlockingInstallUntilCompletionNoLock();

                    _parent->_adminLock.Lock();
                    if (isInstall == true) {
                        _parent->_inProgress.Install->Release();
                        _parent->_inProgress.Package->Release();
                        _parent->_inProgress.Install = nullptr;
                        _parent->_inProgress.Package = nullptr;
                    }
                    _parent->_actitity = ActivityType::NONE;
                    _parent->_adminLock.Unlock();

                    Block();
                }

                return Core::infinite;
            }

        private:
            PackagerImplementation* _parent;
        };

        enum class RepoSyncMode {
            FORCED,
            SETUP
        };

        enum class ActivityType {
            NONE,
            INSTALL,
            REPO_SYNC,
            DOWNLOAD
        };

        uint32_t DoWork(const string& name, const string& version, const string& arch, ActivityType type);
        void UpdateConfig() const;
#if !defined (DO_NOT_USE_DEPRECATED_API)
        static void InstallationProgessNoLock(const _opkg_progress_data_t* progress, void* data);
#endif
        void NotifyStateChange();
        void NotifyRepoSynced(uint32_t status);
        void BlockingInstallUntilCompletionNoLock();
        void BlockingSetupLocalRepoNoLock(RepoSyncMode mode);
        bool InitOPKG();
        void FreeOPKG();

        Core::CriticalSection _adminLock;
        string _configFile;
        string _tempPath;
        string _cachePath;
        int _verbosity;
        bool _noDeps;
        bool _skipSignatureChecking;
        bool _alwaysUpdateFirst;
        bool _volatileCache;
        bool _opkgInitialized;
        std::vector<Exchange::IPackager::INotification*> _notifications;
        InstallationData _inProgress;
        InstallThread _worker;
        bool _isUpgrade;
        ActivityType _actitity;
    };

}  // namespace Plugin
}  // namespace WPEFramework
