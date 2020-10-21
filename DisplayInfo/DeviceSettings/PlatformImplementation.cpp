/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "../Module.h"
#include "../DisplayInfoTracing.h"

#include <interfaces/IDisplayInfo.h>

#include "host.hpp"
#include "exception.hpp"
#include "videoOutputPort.hpp"
#include "videoOutputPortType.hpp"
#include "videoOutputPortConfig.hpp"
#include "videoResolution.hpp"
#include "audioOutputPort.hpp"
#include "audioOutputPortType.hpp"
#include "audioOutputPortConfig.hpp"
#include "manager.hpp"
#include "utils.h"

#include "libIBus.h"
#include "libIBusDaemon.h"
#include "dsMgr.h"

#define EDID_MAX_HORIZONTAL_SIZE 21
#define EDID_MAX_VERTICAL_SIZE   22

#ifdef USE_DISPLAYINFO_REALTEK
#include "Realtek/kms.h"
#define TOTAL_MEM_PARAM_STR  "MemTotal:"
#define FREE_MEM_PARAM_STR  "MemFree:"
#define DEFAULT_DEVICE "/dev/dri/card0"
#endif

namespace WPEFramework {
namespace Plugin {

class DisplayInfoImplementation :
    public Exchange::IGraphicsProperties,
    public Exchange::IConnectionProperties,
    public Exchange::IHDRProperties  {
private:
    using HdrteratorImplementation = RPC::IteratorType<Exchange::IHDRProperties::IHDRIterator>;
public:
    DisplayInfoImplementation()
        : _totalGpuRam(0)
        , _frameRate(0)
    {
        LOGINFO();
        DisplayInfoImplementation::_instance = this;
        try
        {
            Utils::IARM::init();
            IARM_Result_t res;
            IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_RES_PRECHANGE,ResolutionChange) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_RES_POSTCHANGE, ResolutionChange) );

            //TODO: this is probably per process so we either need to be running in our own process or be carefull no other plugin is calling it
            device::Manager::Initialize();
            TRACE(Trace::Information, (_T("device::Manager::Initialize success")));
            UpdateFrameRate(_frameRate);
#ifdef USE_DISPLAYINFO_REALTEK
            UpdateTotalMem(_totalGpuRam);
#endif
        }
        catch(...)
        {
           TRACE(Trace::Error, (_T("device::Manager::Initialize failed")));
        }
    }

    DisplayInfoImplementation(const DisplayInfoImplementation&) = delete;
    DisplayInfoImplementation& operator= (const DisplayInfoImplementation&) = delete;

    virtual ~DisplayInfoImplementation()
    {
        LOGINFO();
        IARM_Result_t res;
        IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_RES_PRECHANGE) );
        IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_RES_POSTCHANGE) );
        DisplayInfoImplementation::_instance = nullptr;
    }

public:
    // Graphics Properties interface
    uint32_t TotalGpuRam(uint64_t& total) const override
    {
        LOGINFO();
        
        // TODO: Implement using DeviceSettings
		total = _totalGpuRam;
        return (Core::ERROR_NONE);
    }
    uint32_t FreeGpuRam(uint64_t& free ) const override
    {
        LOGINFO();

        // TODO: Implement using DeviceSettings
#ifdef USE_DISPLAYINFO_REALTEK
        free = GetMemInfo(FREE_MEM_PARAM_STR);
#else
        free = 0;
#endif
        return (Core::ERROR_NONE);
    }

    // Connection Properties interface
    uint32_t Register(INotification* notification) override
    {
        LOGINFO();
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
        LOGINFO();
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

    static void ResolutionChange(const char *owner, IARM_EventId_t eventId, void *data, size_t len)
    {
        LOGINFO();
        IConnectionProperties::INotification::Source eventtype;
        if (strcmp(owner, IARM_BUS_DSMGR_NAME) == 0)
        {
            switch (eventId) {
                case IARM_BUS_DSMGR_EVENT_RES_POSTCHANGE:
                    eventtype = IConnectionProperties::INotification::Source::POST_RESOLUTION_CHANGE;
                    break;
                case IARM_BUS_DSMGR_EVENT_RES_PRECHANGE:
                    eventtype = IConnectionProperties::INotification::Source::PRE_RESOLUTION_CHANGE;
            }
        }

        if(DisplayInfoImplementation::_instance)
        {
           DisplayInfoImplementation::_instance->ResolutionChangeImpl(eventtype);
        }
    }

    void ResolutionChangeImpl(IConnectionProperties::INotification::Source eventtype)
    {
        LOGINFO();
        _adminLock.Lock();

        std::list<IConnectionProperties::INotification*>::const_iterator index = _observers.begin();
        if(eventtype == IConnectionProperties::INotification::Source::POST_RESOLUTION_CHANGE) {
            UpdateFrameRate(_frameRate);
        }
        
        while(index != _observers.end()) {
            (*index)->Updated(IConnectionProperties::INotification::Source::POST_RESOLUTION_CHANGE);
            index++;
        }

        _adminLock.Unlock();
    }

    uint32_t IsAudioPassthrough (bool& value) const override
    {
        uint32_t ret =  (Core::ERROR_NONE);
        value = false;
        try
        {
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            device::AudioStereoMode mode = vPort.getAudioOutputPort().getStereoMode(true);
            if (mode == device::AudioStereoMode::kPassThru)
                value = true;
        }
        catch (const device::Exception& err)
        {
           TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
           ret = Core::ERROR_GENERAL;
        }
		return ret;
    }
    uint32_t Connected(bool& connected) const override
    {
        try
        {
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            connected = vPort.isDisplayConnected();
        }
        catch (const device::Exception& err)
        {
           TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
           return Core::ERROR_GENERAL;
        }
        return (Core::ERROR_NONE);
    }
    uint32_t Width(uint32_t& value) const override
    {
        // TODO: Implement using DeviceSettings
#ifdef USE_DISPLAYINFO_REALTEK
        uint32_t temp = 0;
        UpdateGraphicSize(value, temp);
#else
        LOGINFO("Stubbed function. TODO: Implement using DeviceSettings");
        value = 0;
#endif
        return (Core::ERROR_NONE);
    }
    uint32_t Height(uint32_t& value) const override
    {
        // TODO: Implement using DeviceSettings
#ifdef USE_DISPLAYINFO_REALTEK
        uint32_t temp = 0;
        UpdateGraphicSize(temp, value);
#else
        LOGINFO("Stubbed function. TODO: Implement using DeviceSettings");
        value = 0;
#endif
        return (Core::ERROR_NONE);
    }
    uint32_t VerticalFreq(uint32_t& value) const override
    {
        value = _frameRate;
        return (Core::ERROR_NONE);
    }

    uint32_t HDCPProtection(HDCPProtectionType& value) const override //get
    {
        LOGINFO();
        int hdcpversion = 1;
        string portname;
        PortName(portname);
        if(!portname.empty())
        {
            try
            {
                device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort(portname);
                hdcpversion = vPort.GetHdmiPreference();
                switch(static_cast<dsHdcpProtocolVersion_t>(hdcpversion))
                {
                    case dsHDCP_VERSION_1X: value = IConnectionProperties::HDCPProtectionType::HDCP_1X; break;
                    case dsHDCP_VERSION_2X: value = IConnectionProperties::HDCPProtectionType::HDCP_2X; break;
                    case dsHDCP_VERSION_MAX: value = IConnectionProperties::HDCPProtectionType::HDCP_AUTO; break;
                }
            }
            catch(const device::Exception& err)
            {
                TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
            }
        }
        else
        {
            TRACE(Trace::Information, (_T("No STB video ouptut ports connected to TV, returning HDCP as unencrypted %d"), hdcpversion));
        }
        return (Core::ERROR_NONE);
    }

    uint32_t HDCPProtection(const HDCPProtectionType value) override //set
    {
        LOGINFO();
        dsHdcpProtocolVersion_t hdcpversion = dsHDCP_VERSION_MAX;
        string portname;
        PortName(portname);
        if(!portname.empty())
        {
            switch(value)
            {
                case IConnectionProperties::HDCPProtectionType::HDCP_1X : hdcpversion = dsHDCP_VERSION_1X; break;
                case IConnectionProperties::HDCPProtectionType::HDCP_2X: hdcpversion = dsHDCP_VERSION_2X; break;
                case IConnectionProperties::HDCPProtectionType::HDCP_AUTO: hdcpversion = dsHDCP_VERSION_MAX; break;
            }
            try
            {
                device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort(portname);
                if(!vPort.SetHdmiPreference(hdcpversion))
                {
                    TRACE(Trace::Information, (_T("HDCPProtection: SetHdmiPreference failed")));
                    LOGERR("SetHdmiPreference failed");
                }
            }
            catch(const device::Exception& err)
            {
                TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
            }
        }
        else
        {
            TRACE(Trace::Information, (_T("No STB video ouptut ports connected to TV, returning HDCP as unencrypted %d"), hdcpversion));
        }
        return (Core::ERROR_NONE);
    }

    uint32_t WidthInCentimeters(uint8_t& width /* @out */) const override
    {
        LOGINFO();
        try
        {
            ::device::VideoOutputPort vPort = ::device::Host::getInstance().getVideoOutputPort("HDMI0");
            if (vPort.isDisplayConnected())
            {
                std::vector<uint8_t> edidVec;

                vPort.getDisplay().getEDIDBytes(edidVec);

                if(edidVec.size() > EDID_MAX_VERTICAL_SIZE)
                {
                    width = edidVec[EDID_MAX_HORIZONTAL_SIZE];
                    TRACE(Trace::Information, (_T("Width in cm = %d"), width));
                }
                else
                {
                    LOGWARN("Failed to get Display Size!\n");
                }
            }
        }
        catch (const device::Exception& err)
        {
           TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        return (Core::ERROR_NONE);
    }

    uint32_t HeightInCentimeters(uint8_t& height /* @out */) const override
    {
        LOGINFO();
        try
        {
            ::device::VideoOutputPort vPort = ::device::Host::getInstance().getVideoOutputPort("HDMI0");
            if (vPort.isDisplayConnected())
            {
                std::vector<uint8_t> edidVec;

                vPort.getDisplay().getEDIDBytes(edidVec);

                if(edidVec.size() > EDID_MAX_VERTICAL_SIZE)
                {
                    height = edidVec[EDID_MAX_VERTICAL_SIZE];
                    TRACE(Trace::Information, (_T("Height in cm = %d"), height));
                }
                else
                {
                    LOGWARN("Failed to get Display Size!\n");
                }
            }
        }
        catch (const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        return (Core::ERROR_NONE);
    }

    uint32_t EDID (uint16_t& length /* @inout */, uint8_t data[] /* @out @length:length */) const override
    {
        LOGINFO();
        vector<uint8_t> edidVec({'u','n','k','n','o','w','n' });
        try
        {
            vector<uint8_t> edidVec2;
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            if (vPort.isDisplayConnected())
            {
                vPort.getDisplay().getEDIDBytes(edidVec2);
                edidVec = edidVec2;//edidVec must be "unknown" unless we successfully get to this line
            }
            else
            {
                LOGWARN("failure: HDMI0 not connected!");
            }
        }
        catch (const device::Exception& err)
        {
            LOG_DEVICE_EXCEPTION0();
        }
        //convert to base64
        uint16_t size = min(edidVec.size(), (size_t)numeric_limits<uint16_t>::max());
        if(edidVec.size() > (size_t)numeric_limits<uint16_t>::max())
            LOGERR("Size too large to use ToString base64 wpe api");
        string edidbase64;
        // Align input string size to multiple of 3
        int paddingSize = 0;
        for (; paddingSize < (3-size%3);paddingSize++)
        {
            edidVec.push_back(0x00);
        }
        size += paddingSize;
        int i = 0;

        for (i; i < length && i < size; i++)
        {
            data[i] = edidVec[i];
        }
        length = i;
        return (Core::ERROR_NONE);

    }

    uint32_t PortName (string& name /* @out */) const
    {
        LOGINFO();
        try
        {
            device::List<device::VideoOutputPort> vPorts = device::Host::getInstance().getVideoOutputPorts();
            for (size_t i = 0; i < vPorts.size(); i++)
            {
                device::VideoOutputPort &vPort = vPorts.at(i);
                if (vPort.isDisplayConnected())
                {
                    name = vPort.getName();
                    TRACE(Trace::Information, (_T("Connected video output port = %s"), name));
                    break;
                }
            }
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        return (Core::ERROR_NONE);
    }

    // @property
    // @brief HDR formats supported by TV
    // @return HDRType: array of HDR formats
    uint32_t TVCapabilities(IHDRIterator*& type /* out */) const override
    {
        LOGINFO();
        std::list<Exchange::IHDRProperties::HDRType> hdrCapabilities;

        int capabilities = static_cast<int>(dsHDRSTANDARD_NONE);
        try
        {
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            if (vPort.isDisplayConnected()) {
                vPort.getTVHDRCapabilities(&capabilities);
            }
            else {
                TRACE(Trace::Error, (_T("getTVHDRCapabilities failure: HDMI0 not connected!")));
            }
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        if(!capabilities) hdrCapabilities.push_back(HDR_OFF);
        if(capabilities & dsHDRSTANDARD_HDR10) hdrCapabilities.push_back(HDR_10);
        if(capabilities & dsHDRSTANDARD_HLG) hdrCapabilities.push_back(HDR_HLG);
        if(capabilities & dsHDRSTANDARD_DolbyVision) hdrCapabilities.push_back(HDR_DOLBYVISION);
        if(capabilities & dsHDRSTANDARD_TechnicolorPrime) hdrCapabilities.push_back(HDR_TECHNICOLOR);
        if(capabilities & dsHDRSTANDARD_Invalid)hdrCapabilities.push_back(HDR_OFF);


        type = Core::Service<HdrteratorImplementation>::Create<Exchange::IHDRProperties::IHDRIterator>(hdrCapabilities);
        return (type != nullptr ? Core::ERROR_NONE : Core::ERROR_GENERAL);
    }
    // @property
    // @brief HDR formats supported by STB
    // @return HDRType: array of HDR formats
    uint32_t STBCapabilities(IHDRIterator*& type /* out */) const override
    {
        LOGINFO();
        std::list<Exchange::IHDRProperties::HDRType> hdrCapabilities;

        int capabilities = static_cast<int>(dsHDRSTANDARD_NONE);
        try
        {
            device::VideoDevice &device = device::Host::getInstance().getVideoDevices().at(0);
            device.getHDRCapabilities(&capabilities);
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        if(!capabilities) hdrCapabilities.push_back(HDR_OFF);
        if(capabilities & dsHDRSTANDARD_HDR10) hdrCapabilities.push_back(HDR_10);
        if(capabilities & dsHDRSTANDARD_HLG) hdrCapabilities.push_back(HDR_HLG);
        if(capabilities & dsHDRSTANDARD_DolbyVision) hdrCapabilities.push_back(HDR_DOLBYVISION);
        if(capabilities & dsHDRSTANDARD_TechnicolorPrime) hdrCapabilities.push_back(HDR_TECHNICOLOR);
        if(capabilities & dsHDRSTANDARD_Invalid)hdrCapabilities.push_back(HDR_OFF);


        type = Core::Service<HdrteratorImplementation>::Create<Exchange::IHDRProperties::IHDRIterator>(hdrCapabilities);
        return (type != nullptr ? Core::ERROR_NONE : Core::ERROR_GENERAL);
    }
    // @property
    // @brief HDR format in use
    // @param type: HDR format
    uint32_t HDRSetting(HDRType& type /* @out */) const override
    {
        LOGINFO();
        LOGINFO();
        type = IHDRProperties::HDRType::HDR_OFF;
        bool isHdr = false;
        try
        {
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            if (vPort.isDisplayConnected()) {
                isHdr = vPort.IsOutputHDR();
            }
            else
            {
                TRACE(Trace::Information, (_T("IsOutputHDR failure: HDMI0 not connected!")));
            }
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }
        TRACE(Trace::Information, (_T("Output HDR = %s"), isHdr ? "Yes" : "No"));

        type = isHdr? HDR_10 : HDR_OFF;
        return (Core::ERROR_NONE);
    }


    BEGIN_INTERFACE_MAP(DisplayInfoImplementation)
        INTERFACE_ENTRY(Exchange::IGraphicsProperties)
        INTERFACE_ENTRY(Exchange::IConnectionProperties)
        INTERFACE_ENTRY(Exchange::IHDRProperties)
    END_INTERFACE_MAP

private:
    std::list<IConnectionProperties::INotification*> _observers;
    mutable Core::CriticalSection _adminLock;
    uint64_t _totalGpuRam;
    uint32_t _frameRate;

    static uint64_t parseLine(const char * line)
    {
        string str(line);
        uint64_t val = 0;
        size_t begin = str.find_first_of("0123456789");
        size_t end = std::string::npos;

        if (std::string::npos != begin)
            end = str.find_first_not_of("0123456789", begin);

        if (std::string::npos != begin && std::string::npos != end)
        {

            str = str.substr(begin, end);
            val = strtoul(str.c_str(), NULL, 10);

        }
        else
        {
            printf("%s:%d Failed to parse value from %s", __FUNCTION__, __LINE__,line);

        }

        return val;
    }
    
    static uint64_t GetMemInfo(const char * param)
    {
        uint64_t memVal = 0;
        FILE *meminfoFile = fopen("/proc/meminfo", "r");
        if (NULL == meminfoFile)
        {
            printf("%s:%d : Failed to open /proc/meminfo:%s", __FUNCTION__, __LINE__, strerror(errno));
        }
        else
        {
            std::vector <char> buf;
            buf.resize(1024);

            while (fgets(buf.data(), buf.size(), meminfoFile))
            {
                 if ( strstr(buf.data(), param ) == buf.data())
                 {
                     memVal = parseLine(buf.data()) * 1000;
                     break;
                 }
            }

            fclose(meminfoFile);
        }
        return memVal;
    }

#ifdef USE_DISPLAYINFO_REALTEK
    void UpdateTotalMem(uint64_t& totalRam)
    {
        totalRam = GetMemInfo(TOTAL_MEM_PARAM_STR);
    }
   
    static void get_primary_plane(int drm_fd, kms_ctx *kms, drmModePlane **plane) 
    {
        kms_get_plane(drm_fd, kms);
        printf("[INFO] Primary Plane ID :  %d\n", kms->primary_plane_id);
        *plane = drmModeGetPlane(drm_fd, kms->primary_plane_id );
        if(*plane)
            printf("fb id : %d\n", (*plane)->fb_id); 
    }
    
    static uint32_t UpdateGraphicSize(uint32_t &w, uint32_t &h)
    {
        uint32_t ret =  (Core::ERROR_NONE);
        int drm_fd;
        kms_ctx *kms = NULL;
        drmModePlane *plane = NULL;
        int trytimes = 0;
        
        do {
            /* Setup buffer information */
            drm_fd = open( DEFAULT_DEVICE, O_RDWR);

            /* Setup KMS */
            kms = kms_setup(drm_fd);
            if( !kms->crtc ) {
                ret = Core::ERROR_GENERAL;
                printf("[UpdateGraphicSize] kms_setup fail\n");
                break;
            }
            
            /* Get primary buffer */
            get_primary_plane(drm_fd, kms, &plane);
            if( !plane) {
                ret = Core::ERROR_GENERAL;
                printf("[UpdateGraphicSize] fail to get_primary_plane\n");
                break;
            }
        
            /* get fb */
            drmModeFB *fb = drmModeGetFB(drm_fd, plane->fb_id);
            while(!fb) {
                get_primary_plane(drm_fd, kms, &plane);
                fb = drmModeGetFB(drm_fd, plane->fb_id);
                if (trytimes++ > 100) {
                    ret = Core::ERROR_GENERAL;
                    printf("[UpdateGraphicSize] fail to get_primary_plane\n");
                    break;
                }
            }
            
            /* Get the width and height */
            if(fb) {
                w = fb->width;
                h = fb->height;
                drmModeFreeFB(fb);
            }
        } while(0);

        /* release */
        /* Cleanup buffer info */
        if(kms) {
            kms_cleanup_context(kms);
            free(kms);
        }
        
        printf("[UpdateGraphicSize] width : %d\n", w);
        printf("[UpdateGraphicSize] height : %d\n", h);
        return ret;
    }
#endif
   
   uint32_t UpdateFrameRate(uint32_t &rate)
   {
        uint32_t ret =  (Core::ERROR_NONE);
        try
        {
            device::VideoOutputPort vPort = device::Host::getInstance().getVideoOutputPort("HDMI0");
            device::VideoResolution resolution = vPort.getResolution();
            device::PixelResolution pr = resolution.getPixelResolution();
            device::FrameRate fr = resolution.getFrameRate();
            if (fr == device::FrameRate::k24 ) {
                rate = 24;
            } else if(fr == device::FrameRate::k25) {
                rate = 25;
            } else if(fr == device::FrameRate::k30) {
                rate = 30;
            } else if(fr == device::FrameRate::k60) {
                rate = 60;
            } else if(fr == device::FrameRate::k23dot98) {
                rate = 23;
            } else if(fr == device::FrameRate::k29dot97) {
                rate = 29;
            } else if(fr == device::FrameRate::k50) {
                rate = 50;
            } else if(fr == device::FrameRate::k59dot94) {
                rate = 59;
            } else {
                ret = Core::ERROR_GENERAL;
            }
           
        }
        catch (const device::Exception& err)
        {
           TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
           ret = Core::ERROR_GENERAL;
        }
        
        return ret;
    }
   


public:
    static DisplayInfoImplementation* _instance;
};
    DisplayInfoImplementation* DisplayInfoImplementation::_instance = nullptr;
    SERVICE_REGISTRATION(DisplayInfoImplementation, 1, 0);
}
}
