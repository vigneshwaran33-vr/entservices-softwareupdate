/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2024 RDK Management
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

#include "FirmwareUpdateImplementation.h"

std::atomic<bool> isFlashingInProgress(false);
std::mutex flashMutex;
std::mutex logMutex;
void startProgressTimer() ;
namespace WPEFramework {
    namespace Plugin {
        SERVICE_REGISTRATION(FirmwareUpdateImplementation, 1, 0);

        FirmwareUpdateImplementation::FirmwareUpdateImplementation()
            : _adminLock(),
            mShell(nullptr)
        {
            LOGINFO("Create FirmwareUpdateImplementation Instance");

            FirmwareUpdateImplementation::instance(this);

            InitializeIARM();
        }

        FirmwareUpdateImplementation* FirmwareUpdateImplementation::instance(FirmwareUpdateImplementation *FirmwareUpdateImpl)
        {
            static FirmwareUpdateImplementation *FirmwareUpdateImpl_instance = nullptr;

            ASSERT ((nullptr == FirmwareUpdateImpl_instance) || (nullptr == FirmwareUpdateImpl));

            if (FirmwareUpdateImpl != nullptr)
            {
                FirmwareUpdateImpl_instance = FirmwareUpdateImpl;
            }

            return(FirmwareUpdateImpl_instance);
        }

        FirmwareUpdateImplementation::~FirmwareUpdateImplementation()
        {

            LOGINFO("~FirmwareUpdateImplementation \n");
            // Check if flashThread is still running or joinable and join it if it is
            if (flashThread.joinable()) {
                LOGINFO("flashThread is still running or joinable. Joining now...");
                flashThread.join();  // Ensure the thread has completed before main exits
            }
            mShell = nullptr;
            DeinitializeIARM();
        }

        void FirmwareUpdateImplementation::InitializeIARM()
        {
            Utils::IARM::init();        	    
        }

        void FirmwareUpdateImplementation::DeinitializeIARM()
        {
        }

        /**
         * Register a notification callback
         */
        Core::hresult FirmwareUpdateImplementation::Register(Exchange::IFirmwareUpdate::INotification *notification)
        {
            ASSERT (nullptr != notification);

            _adminLock.Lock();

            // Make sure we can't register the same notification callback multiple times
            if (std::find(_FirmwareUpdateNotification.begin(), _FirmwareUpdateNotification.end(), notification) == _FirmwareUpdateNotification.end())
            {
                LOGINFO("Register notification");
                _FirmwareUpdateNotification.push_back(notification);
                notification->AddRef();
            }

            _adminLock.Unlock();

            return Core::ERROR_NONE;
        }

        /**
         * Unregister a notification callback
         */
        Core::hresult FirmwareUpdateImplementation::Unregister(Exchange::IFirmwareUpdate::INotification *notification )
        {
            uint32_t status = Core::ERROR_GENERAL;

            ASSERT (nullptr != notification);

            _adminLock.Lock();

            // Make sure we can't unregister the same notification callback multiple times
            auto itr = std::find(_FirmwareUpdateNotification.begin(), _FirmwareUpdateNotification.end(), notification);
            if (itr != _FirmwareUpdateNotification.end())
            {
                (*itr)->Release();
                LOGINFO("Unregister notification");
                _FirmwareUpdateNotification.erase(itr);
                status = Core::ERROR_NONE;
            }
            else
            {
                LOGERR("notification not found");
            }

            _adminLock.Unlock();

            return status;
        }

        void FirmwareUpdateImplementation::dispatchEvent(Event event, const JsonObject &params)
        {
            Core::IWorkerPool::Instance().Submit(Job::Create(this, event, params));
        }

        void FirmwareUpdateImplementation::Dispatch(Event event, const JsonObject params)
        {
            _adminLock.Lock();

            std::list<Exchange::IFirmwareUpdate::INotification*>::const_iterator index(_FirmwareUpdateNotification.begin());

            switch(event) {
                case ON_UPDATE_STATE_CHANGE:{
                                                if (index != _FirmwareUpdateNotification.end())
                                                {
                                                    string strState = params["state"].String();
                                                    string strSubstate = params["substate"].String();
                                                    SWUPDATEINFO("onUpdateStateChange event triggred with state:%s substate:%s \n",strState.c_str(),strSubstate.c_str());
                                                    WPEFramework::Exchange::IFirmwareUpdate::State state ;
                                                    WPEFramework::Exchange::IFirmwareUpdate::SubState   substate = WPEFramework::Exchange::IFirmwareUpdate::SubState::NOT_APPLICABLE;
                                                    auto it = firmwareState.find(strState);
                                                    if (it != firmwareState.end()) {
                                                        state  = it->second;
                                                    }

                                                    auto it1 = firmwareSubState.find(strSubstate);
                                                    if (it1 != firmwareSubState.end()) {
                                                        substate  = it1->second;
                                                    }
                                                    if (it != firmwareState.end() ) {

                                                        while (index != _FirmwareUpdateNotification.end()){
                                                            (*index)->OnUpdateStateChange(state, substate);
                                                            ++index;
                                                        }
                                                    } else {
                                                        SWUPDATEERR("Invalid state %s and subState %s ",strState.c_str() ,strSubstate.c_str());
                                                    }
                                                }
                                            }
                                            break;
                case ON_FLASHING_STATE_CHANGE:
                    while (index != _FirmwareUpdateNotification.end())
                    {
                        string percentageComplete_str =  params["percentageComplete"].String() ;
                        const uint32_t percentageComplete =  static_cast<uint32_t>(std::stoi(percentageComplete_str));;
                        (*index)->OnFlashingStateChange(percentageComplete);
                        ++index;
                    }
                    break;

                default:
                    break;
            }

            _adminLock.Unlock();
        }


        /* Description:Use for do some action after flash complete successful
         * @param: upgrade_file : Image file to be flash
         * @param: reboot_flag : reboot action after flash
         * @param: upgrade_type : pci/pdri
         * @param: maint : maintenance manager enable/disable
         * @return int: 0
         * */
        int FirmwareUpdateImplementation::postFlash(const char *maint, const char *upgrade_file, int upgrade_type, const char *reboot_flag ,const char *initiated_type)
        {
            //DownloadData DwnLoc;
            int ret = -1;
            char device_type[32];
            char device_name[32];
            FILE *fp = NULL;
            bool st_notify_flag = false;
            /*
            char stage2lock[32] = {0};
            const char *stage2file = NULL;
            char *tmp_stage2lock = NULL;
            */

            if (maint == NULL || reboot_flag == NULL || upgrade_file == NULL) {
                SWUPDATEERR("%s: Parameter is NULL\n", __FUNCTION__);
                return ret;
            }
            /*Below logic is use for to call updateSecurityStage function inside script*/
            fp = fopen("/tmp/rdkvfw_sec_stage", "w");
            if (fp != NULL) {
                fclose(fp);
            }
            ret = getDevicePropertyData("DEVICE_TYPE", device_type, sizeof(device_type));
            if (ret == UTILS_SUCCESS) {
                SWUPDATEINFO("%s: device_type = %s\n", __FUNCTION__, device_type);
            } else {
                SWUPDATEERR("%s: getDevicePropertyData() for device_type fail\n", __FUNCTION__);
                return ret;
            }
            ret = getDevicePropertyData("DEVICE_NAME", device_name, sizeof(device_name));
            if (ret == UTILS_SUCCESS) {
                SWUPDATEINFO("%s: device_name = %s\n", __FUNCTION__, device_name);
            } else {
                SWUPDATEERR("%s: getDevicePropertyData() for device_name fail\n", __FUNCTION__);
                return ret;
            }
#if 0
            if (0 == (strncmp(device_name, "PLATCO", 6))) {


                ret = getDevicePropertyData("STAGE2LOCKFILE", stage2lock, sizeof(stage2lock));
                if (ret == UTILS_SUCCESS) {
                    SWUPDATEINFO("%s: security stage2file name = %s\n", __FUNCTION__, stage2lock);
                    /* Below block is use for remove special charecter("") from the string */
                    if (stage2lock[0] == '"') {
                        tmp_stage2lock = strchr(stage2lock+1, '"');
                        if (tmp_stage2lock != NULL) {
                            *tmp_stage2lock = '\0';
                        }
                        SWUPDATEINFO("Security stage file name=%s\n", stage2lock+1);
                        stage2file = stage2lock+1;
                        if (stage2file != NULL) {
                            SWUPDATEINFO("Security stage file name after remove special character=%s\n", stage2file);
                        }
                    }else {
                        stage2file = stage2lock;
                    }
                    if (! (Utils::fileExists(stage2file)) ) {
                        updateSecurityStage();
                        fp = fopen(stage2file, "w");
                        if (fp != NULL) {
                            SWUPDATEINFO("Security stage file created\n");
                            fclose(fp);
                        }else {
                            SWUPDATEERR("Unable to create Security stage file\n");
                        }
                    }
                } else {
                    SWUPDATEERR("%s: getDevicePropertyData() for device_name fail\n", __FUNCTION__);
                }
            }
#endif
            st_notify_flag = isMmgbleNotifyEnabled();
            eventManager(FW_STATE_EVENT, FW_STATE_VALIDATION_COMPLETE);
            eventManager(IMG_DWL_EVENT, IMAGE_FWDNLD_FLASH_COMPLETE);
            if ((strncmp(device_type, "broadband", 9)) && (0 == (strncmp(maint, "true", 4)))) {
                eventManager("MaintenanceMGR", MAINT_FWDOWNLOAD_COMPLETE_);
            }
            sleep(5);
            sync();
            if (!(Utils::fileExists("/tmp/fw_preparing_to_reboot"))) {
                fp = fopen("/tmp/fw_preparing_to_reboot", "w");
                if (fp == NULL) {
                    SWUPDATEERR("Error creating file /tmp/fw_preparing_to_reboot\n");
                }else {
                    SWUPDATEINFO("Creating flag for preparing to reboot event sent to AS/EPG\n");
                    fclose(fp);
                }
                eventManager(FW_STATE_EVENT,FW_STATE_PREPARING_TO_REBOOT);
            }
            if (upgrade_type == PDRI_UPGRADE) {
                SWUPDATEINFO("Reboot Not Needed after PDRI Upgrade..!\n");
            } else {
                SWUPDATEINFO("%s : Upgraded file = %s\n", __FUNCTION__, upgrade_file);
                fp = fopen("/opt/cdl_flashed_file_name", "w");
                if (fp != NULL) {
                    fprintf(fp, "%s\n", upgrade_file);
                    fclose(fp);
                }
                if (0 == (strncmp(maint, "true", 4))) {
                    if (Utils::fileExists("/rebootNow.sh")) {
                        eventManager("MaintenanceMGR", MAINT_REBOOT_REQUIRED_);
                        if (0 == (strncmp(device_name, "PLATCO", 6)) && (0 == (strncmp(reboot_flag, "true", 4)))) {
                            SWUPDATEINFO("Send notification to reboot in 10mins due to critical upgrade\n");
                            eventManager(FW_STATE_EVENT, FW_STATE_CRITICAL_REBOOT);
                            SWUPDATEINFO("Sleeping for 600 sec before rebooting the STB\n");
                            sleep(600);
                            SWUPDATEINFO("Application Reboot Timer of 600 expired, Rebooting from RDK\n");
                            //sh /rebootNow.sh -s UpgradeReboot_"`basename $0`" -o "Rebooting the box from RDK for Pending Critical Firmware Upgrade..."
                            v_secure_system("sh /rebootNow.sh -s '%s' -o '%s'","UpgradeReboot_rdkvfwupgrader >> /opt/logs/swupdate.log", "Rebooting the box from RDK for Pending Critical Firmware Upgrade...");
                        }
                        updateOPTOUTFile(MAINTENANCE_MGR_RECORD_FILE);
                    } else {
                        SWUPDATEERR("rebootNow.sh is not avaibale\n");
                    }

                }else {
                    if (0 == (strncmp(reboot_flag, "true", 4))) {
                        if (Utils::fileExists("/rebootNow.sh")) {
                            SWUPDATEINFO("Download is complete. Rebooting the box now...\n");
                            SWUPDATEINFO("Trigger RebootPendingNotification in background\n");
                            if (true == st_notify_flag) {
                                SWUPDATEINFO("RDKV_REBOOT : Setting RebootPendingNotification before reboot\n");
                                notifyDwnlStatus(RFC_FW_REBOOT_NOTIFY, REBOOT_PENDING_DELAY, RFC_UINT);
                                SWUPDATEINFO("RDKV_REBOOT  : RebootPendingNotification SET succeeded\n");
                            }
                            unsetStateRed();
                            SWUPDATEINFO("sleep for 2 sec to send reboot pending notification\n");
                            sleep(2);
                            //sh /rebootNow.sh -s UpgradeReboot_"`basename $0`" -o "Rebooting the box after Firmware Image Upgrade..."
                            v_secure_system("sh /rebootNow.sh -s '%s' -o '%s' >> /opt/logs/swupdate.log", "UpgradeReboot_rdkvfwupgrader", "Rebooting the box after Firmware Image Upgrade...");
                        } else {
                            SWUPDATEERR("rebootNow.sh is not avaibale\n");
                        }
                    }
                }
            }
            return 0;
        }

        /* Description:Use for Flashing the image
         * @param: server_url : server url
         * @param: upgrade_file : Image file to be flash
         * @param: reboot_flag : reboot action after flash
         * @param: proto : protocol used
         * @param: upgrade_type : pci/pdri
         * @param: maint : maintenance manager enable/disable
         * @return int: 0 : Success other than 0 false
         * */
//Note : flashImage() and postFlash() is combination of both rdkfwupdater/src/flash.c(Flashing part of deviceInitiatedFWDnld.sh) and Flashing part of userInitiatedFWDnld.sh . For now except upgrade_file ,upgrade_type all other param are passed with default value .other param useful when for future implementations
        int FirmwareUpdateImplementation::flashImage(const char *server_url, const char *upgrade_file, const char *reboot_flag, const char *proto, int upgrade_type, const char *maint ,const char *initiated_type ,const char * codebig) 
        {
            int ret =  -1;
            bool mediaclient = false;
            const char *failureReason = NULL;
            char cpu_arch[8] = {0};
            char headerinfofile[128] = {0};
            char difw_path[32] = {0};
            const char *rflag = "0";
            const char *uptype = "pci";
            const char *file = NULL;
            int flash_status = -1;
            struct FWDownloadStatus fwdls;
            memset(&fwdls, '\0', sizeof(fwdls));
            if (initiated_type == nullptr || *initiated_type == '\0') {
                initiated_type = "device";
            }
            if (codebig == nullptr || *codebig == '\0') {
                codebig = "false";
            }
            if (server_url == nullptr || *server_url == '\0') {
                server_url = "empty";
            }

            if ( ((strcmp(proto, "usb") == 0) && server_url == NULL) || upgrade_file == NULL || reboot_flag == NULL || proto == NULL || maint == NULL) {
                SWUPDATEERR("%s : Parametr is NULL\n", __FUNCTION__);
                return ret;
            }
            if (0 == ((strncmp(reboot_flag, "true", 4)))) {
                rflag = "1";
                SWUPDATEINFO("reboot flag = %s\n", rflag);
            }
            file = strrchr(upgrade_file, '/');
            if (file != NULL) {
                SWUPDATEINFO("upgrade file = %s\n", file);
                SWUPDATEINFO("upgrade file = %s\n", file+1);
            }
            snprintf(headerinfofile, sizeof(headerinfofile), "%s.header", upgrade_file);
            SWUPDATEINFO("Starting Image Flashing ...\n");
            SWUPDATEINFO("Upgrade Server = %s\nUpgrade File = %s\nReboot Flag = %s\nUpgrade protocol = %s\nPDRI Upgrade = %d\nImage name = %s\nheaderfile=%s\n", server_url, upgrade_file, reboot_flag, proto, upgrade_type, file, headerinfofile);
            if (upgrade_type == PDRI_UPGRADE) {
                SWUPDATEINFO("Updating PDRI image with  %s\n", upgrade_file);
            }

            mediaclient = isMediaClientDevice();
            if (std::string(initiated_type) == "device")
            { 
                if (true == mediaclient) {
                    eventManager(IMG_DWL_EVENT, IMAGE_FWDNLD_FLASH_INPROGRESS);
                }

                ret = getDevicePropertyData("CPU_ARCH", cpu_arch, sizeof(cpu_arch));
                if (ret == UTILS_SUCCESS) {
                    SWUPDATEINFO("cpu_arch = %s\n", cpu_arch);
                } else {
                    SWUPDATEERR("%s: getDevicePropertyData() for cpu arch fail\n", __FUNCTION__);
                }
            }


            if (std::string(proto) == "usb")
            {
                string upgrade_file_str = std::string(upgrade_file);
                string path = upgrade_file_str.substr(0, upgrade_file_str.find_last_of("/\\") + 1);
                std::strcpy(difw_path, path.c_str());
                SWUPDATEINFO("difw path = %s\n", difw_path);
            }
            else
            {
                ret = getDevicePropertyData("DIFW_PATH", difw_path, sizeof(difw_path));
                if (ret == UTILS_SUCCESS) {
                    SWUPDATEINFO("difw path = %s\n", difw_path);
                } else {
                    SWUPDATEERR("%s: getDevicePropertyData() for DIFW_PATH fail\n", __FUNCTION__);
                }
            }

            if (Utils::fileExists("/lib/rdk/imageFlasher.sh")) {
                //lib/rdk/imageFlasher.sh $UPGRADE_PROTO $UPGRADE_SERVER $DIFW_PATH $UPGRADE_FILE $REBOOT_IMMEDIATE_FLAG $PDRI_UPGRADE
                //proto=2 for http
                //server=server url upgrade file = download file reboot = 1 for true and 0 for false
                if (upgrade_type == PDRI_UPGRADE) {
                    uptype = "pdri";
                    SWUPDATEINFO("upgrade type = %s\n", uptype);
                }

                // Start a thread for progress updates
                std::thread timerThread(&WPEFramework::Plugin::FirmwareUpdateImplementation::startProgressTimer, this);
	            dispatchAndUpdateEvent(_FLASHING_STARTED,"");

                ret = v_secure_system("/lib/rdk/imageFlasher.sh '%s' '%s' '%s' '%s' '%s' '%s' >> /opt/logs/swupdate.log", proto, server_url, difw_path, file+1, rflag, uptype);

                // Reset flashing status
                isFlashingInProgress = false;

                // Wait for the timer thread to complete
                if (timerThread.joinable()) timerThread.join();


                flash_status = ret;
                SWUPDATEINFO("flash_status = %d and ret = %d\n",flash_status, ret);
            } else {
                SWUPDATEERR("imageFlasher.sh required for flash image. This is device specific implementation\n");
            }
            if (flash_status == 0 && (upgrade_type != PDRI_UPGRADE)) {
                SWUPDATEINFO("doCDL success.\n");
            }
            if (flash_status != 0) {

                SWUPDATEINFO("Image Flashing failed\n");
                dispatchAndUpdateEvent(_FLASHING_FAILED,"");

                if (false == mediaclient && (std::string(initiated_type) == "device")) {
                    failureReason = "RCDL Upgrade Failed";
                    if ((strncmp(cpu_arch, "x86", 3)) == 0) {
                        failureReason = "ECM trigger failed";
                    }
                } else {
                    failureReason = "Failed in flash write";
                    eventManager(IMG_DWL_EVENT, IMAGE_FWDNLD_FLASH_FAILED);
                    if ((strncmp(maint, "true", 4)) == 0) {
                        eventManager("MaintenanceMGR", MAINT_FWDOWNLOAD_ERROR_);
                        SWUPDATEINFO("Image Flash Failed and send status to MaintenanceMGR\n");
                    }
                }
                //updateFWDownloadStatus "$cloudProto" "Failure" "$cloudImmediateRebootFlag" "$failureReason" "$dnldVersion" "$cloudFWFile" "$runtime" "Failed" "$DelayDownloadXconf"
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|%s\n", failureReason);
                eventManager(FW_STATE_EVENT, FW_STATE_FAILED);
                if(std::string(initiated_type) == "device")
                {                    
                    updateUpgradeFlag(0,2);
                }

            } else if (true == mediaclient) {                
                SWUPDATEINFO("Image Flashing is success\n");
                JsonObject params1;
                params1["percentageComplete"]  = "100";
                dispatchEvent(ON_FLASHING_STATE_CHANGE, params1);

                SWUPDATEINFO("imageFlasher.sh completed successfully . onFlashingStateChange event triggred for 100%% \n");

                //updateFWDownloadStatus "$cloudProto" "Success" "$cloudImmediateRebootFlag" "" "$dnldVersion" "$cloudFWFile" "$runtime" "Validation complete" "$DelayDownloadXconf"
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Success\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Validation complete\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|");
                if (((strncmp(maint, "true", 4)) == 0) && (0 == (strncmp(reboot_flag, "true", 4)))) {
                    eventManager("MaintenanceMGR", MAINT_CRITICAL_UPDATE_);
                    SWUPDATEINFO("Posting Critical update");
                }
                if (Utils::fileExists(upgrade_file)) {
                    SWUPDATEINFO("flashImage: Flashing completed. Deleting File:%s\n", upgrade_file);
                    unlink(upgrade_file);
                }


                if (std::string(proto) == "usb") {
                    eventManager(FW_STATE_EVENT, FW_STATE_VALIDATION_COMPLETE);
                    FILE *fp = fopen("/opt/cdl_flashed_file_name", "w");
                    if (fp != NULL) {
                        fprintf(fp, "%s\n", file+1);
                        fclose(fp);
                    }

                    dispatchAndUpdateEvent(_FLASHING_SUCCEEDED,"");
                    dispatchAndUpdateEvent(_WAITING_FOR_REBOOT,"");
                }	
                else
                {
                    postFlash(maint, file+1, upgrade_type, reboot_flag ,initiated_type);
                    dispatchAndUpdateEvent(_FLASHING_SUCCEEDED,"");
                }

                if(std::string(initiated_type) == "device")
                {
                    updateUpgradeFlag(0,2);// Remove file TODO: Logic need to change
                }

            }else {
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Download complete\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Download complete\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|");
            }

            if (Utils::fileExists(headerinfofile)) {
                SWUPDATEINFO("flashImage: Flashing completed. Deleting headerfile File:%s\n", headerinfofile);
                unlink(headerinfofile);
            }
            if (0 == ((strncmp(initiated_type, "user", 4)))) {
                if (Utils::fileExists(RCDL_FLAG)) {
                    unlink(RCDL_FLAG);
                }
            }
            snprintf(fwdls.method, sizeof(fwdls.method), "Method|xconf\n");
            snprintf(fwdls.proto, sizeof(fwdls.proto), "Proto|http\n");
            snprintf(fwdls.reboot, sizeof(fwdls.reboot), "Reboot|%s\n", reboot_flag);
            snprintf(fwdls.dnldVersn, sizeof(fwdls.dnldVersn), "DnldVersn|\n");
            snprintf(fwdls.dnldfile, sizeof(fwdls.dnldfile), "DnldFile|%s\n", upgrade_file);
            snprintf(fwdls.dnldurl, sizeof(fwdls.dnldurl), "DnldURL|%s\n", server_url);
            snprintf(fwdls.lastrun, sizeof(fwdls.lastrun), "LastRun|\n");
            snprintf(fwdls.DelayDownload, sizeof(fwdls.DelayDownload), "DelayDownload|\n");
            snprintf(fwdls.codeBig, sizeof(fwdls.codeBig), "Codebig|%s\n", codebig);
            if (upgrade_type == PDRI_UPGRADE) {
                updateFWDownloadStatus(&fwdls, "yes",initiated_type);
            } else {
                updateFWDownloadStatus(&fwdls, "no",initiated_type);
            }

            return flash_status;
        }

        // Timer function to track progress
        void FirmwareUpdateImplementation::startProgressTimer() {
            int progressIntervals[] = {5, 10, 15, 20, 25};  // Seconds
            int percentages[] = {20, 40, 60, 80, 99};       // Corresponding progress percentages
            int numIntervals = sizeof(progressIntervals) / sizeof(progressIntervals[0]);

            for (int i = 0; i < numIntervals; ++i) {
                // Sleep for small intervals and check if flashing is complete
                int interval = progressIntervals[i] - (i > 0 ? progressIntervals[i - 1] : 0);
                for (int j = 0; j < interval; ++j) {
                    if (!isFlashingInProgress.load()) return; // Exit if flashing is done
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }

                // Check if the script has already completed
                if (!isFlashingInProgress.load()) break;

                JsonObject params;
                params["percentageComplete"]  = std::to_string(percentages[i]);
                dispatchEvent(ON_FLASHING_STATE_CHANGE, params);
                SWUPDATEINFO("onFlashingStateChange event triggred with percentage %d  \n",percentages[i]);

            }
        }
        uint32_t FirmwareUpdateImplementation::Configure(PluginHost::IShell* shell)
        {
            LOGINFO("Configuring FirmwareUpdateImplementation");
            uint32_t result = Core::ERROR_NONE;
            ASSERT(shell != nullptr);
            mShell = shell;
            return result;
        }

        void FirmwareUpdateImplementation::dispatchAndUpdateEvent (string state ,string substate)
        {
            JsonObject params;
            params["state"]  = state;
            params["substate"]  = (substate == "") ? "NOT_APPLICABLE" : substate;
            dispatchEvent(ON_UPDATE_STATE_CHANGE, params);
            FirmwareStatus( state,substate , "write");
        }
#if 0
        void FirmwareUpdateImplementation::updateSecurityStage ()
        {
            if(mShell){
                PluginHost::IShell::state state;

                if ((Utils::getServiceState(mShell, FACTORYPROTECT_CALLSIGN_VER, state) == Core::ERROR_NONE) && (state != PluginHost::IShell::state::ACTIVATED))
                {
                    Utils::activatePlugin(mShell, FACTORYPROTECT_CALLSIGN_VER);
                }

                if ((Utils::getServiceState(mShell, FACTORYPROTECT_CALLSIGN_VER, state) == Core::ERROR_NONE) && (state == PluginHost::IShell::state::ACTIVATED))
                {
                    std::shared_ptr<WPEFramework::JSONRPC::LinkType<WPEFramework::Core::JSON::IElement>> factoryProtectConnection = Utils::getThunderControllerClient(FACTORYPROTECT_CALLSIGN_VER);

                    if (!factoryProtectConnection)
                    {
                        SWUPDATEERR("%s plugin initialisation failed", FACTORYPROTECT_CALLSIGN_VER);
                    }
                    else
                    {
                        JsonObject params;
                        params["key"] = "deviceStage";
                        params["value"] = "stage2";
                        JsonObject res;
                        factoryProtectConnection->Invoke<JsonObject, JsonObject>(2000, "setManufacturerData", params, res);
                        if (res["success"].Boolean())
                        {
                            SWUPDATEINFO("setManufacturerData success for setting key :deviceStage, value :stage2");
                        }
                        else
                        {
                            SWUPDATEERR("Failed to set setManufacturerData key :deviceStage, value :stage2");
                        }
                    }
                }
                else
                {
                    SWUPDATEERR("%s plugin is not activated", FACTORYPROTECT_CALLSIGN_VER);
                }
            }
            else
            {
                SWUPDATEERR("Failed to get mShell controller");
            }

        }
#endif        

        // Thread function to initiate flashImage
        void FirmwareUpdateImplementation::flashImageThread(std::string firmwareFilepath,std::string firmwareType) {
            // Lock mutex to ensure thread-safe execution of flashImage
            std::lock_guard<std::mutex> lock(flashMutex);

            std::string upgrade_file = firmwareFilepath;
            int upgrade_type = PCI_UPGRADE;
            if (firmwareType == "DRI")
            {
                upgrade_type = PDRI_UPGRADE;
            }


            const char *server_url ="";
            const char *reboot_flag = "false";
            const char *proto = "usb";
            const char *maint = "false";
            const char *initiated_type = "user";
            const char *codebig = "";            
            struct FWDownloadStatus fwdls;
            memset(&fwdls, '\0', sizeof(fwdls));
            string dri = (firmwareType == "DRI") ? "yes" : "no";
            string name = firmwareFilepath.substr(firmwareFilepath.find_last_of("/\\") + 1);
            string path = firmwareFilepath.substr(0, firmwareFilepath.find_last_of("/\\") + 1);
            
            string currentFlashedImage = readProperty("/version.txt","imagename", ":") ;
            SWUPDATEINFO("currentFlashedImage : %s",currentFlashedImage.c_str());
            std::string fileWithoutExtension ="";
            // Find the position of the last '.'
            size_t dotPos = name.find_last_of('.');
            if (dotPos != std::string::npos) {
                // Extract substring before the '.'
                fileWithoutExtension = name.substr(0, dotPos);
            } else {
                // If no '.' is found, use the original string
                fileWithoutExtension = name;
            }

            if (fileWithoutExtension == currentFlashedImage)
            {

                SWUPDATEERR("FW version of the active image and the image to be upgraded are the same. No upgrade required.");                
                isFlashingInProgress = false; // Reset the flag if exiting early
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|No upgrade needed\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|No upgrade needed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|No upgrade needed\n");
                updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type);
		dispatchAndUpdateEvent(_VALIDATION_FAILED,_FIRMWARE_UPTODATE);
                return ;
            }


            if(std::string(proto) == "usb")
            {
                if (!copyFileToDirectory(upgrade_file.c_str(), USB_TMP_COPY)) {
                    SWUPDATEERR("File copy operation failed.\n");
                    dispatchAndUpdateEvent(_VALIDATION_FAILED,"");
                    isFlashingInProgress = false; // Reset the flag if exiting early
                    snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                    snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                    snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|File copy operation failed.\n");
                    updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type);

                    return ;
                }
                else
                {
                    std::string full_path = std::string(USB_TMP_COPY) + "/" + name;
                    upgrade_file = full_path;
                    SWUPDATEINFO("Upgrade file path after copy %s \n" ,upgrade_file.c_str());
                }
            }

            //Note : flashImage() is combination of both rdkfwupdater/src/flash.c(Flashing part of deviceInitiatedFWDnld.sh) and Flashing part of userInitiatedFWDnld.sh . For now except upgrade_file ,upgrade_type all other param are passed with default value .other param useful when for future implementations
            // Call the actual flashing function
            flashImage(server_url, upgrade_file.c_str(), reboot_flag, proto, upgrade_type, maint ,initiated_type , codebig);

        }

        Core::hresult FirmwareUpdateImplementation::UpdateFirmware(const string& firmwareFilepath , const string& firmwareType , Result &result ) 
        {
            Core::hresult status = Core::ERROR_GENERAL;
            std::string state =  "";
            std::string substate =  "";
            struct FWDownloadStatus fwdls;
            memset(&fwdls, '\0', sizeof(fwdls));
            string dri = (firmwareType == "DRI") ? "yes" : "no";
            string initiated_type = "user"; //default is user now .Based on Future implementation it will change.

            if(firmwareFilepath == "")
            {
                SWUPDATEERR("firmwareFilepath is empty");
                dispatchAndUpdateEvent(_VALIDATION_FAILED,_FIRMWARE_NOT_FOUND);
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|firmwareFilepath is empty\n");
                updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type.c_str());
                status = Core::ERROR_INVALID_PARAMETER;
                return status;
            }
            else if (!(Utils::fileExists(firmwareFilepath.c_str()))) {
                SWUPDATEERR("firmwareFile is not present %s",firmwareFilepath.c_str());
                SWUPDATEERR("Local image Download Failed"); //Existing marker
                dispatchAndUpdateEvent(_VALIDATION_FAILED,_FIRMWARE_NOT_FOUND);
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|firmwareFile is not present\n");
                updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type.c_str());
                status = Core::ERROR_INVALID_PARAMETER;
                return status;
            }

            if(firmwareType !=""){
                if (firmwareType != "PCI" && firmwareType != "DRI") {
                    SWUPDATEERR("firmwareType must be either 'PCI' or 'DRI'.");
                    dispatchAndUpdateEvent(_VALIDATION_FAILED,"");
                    snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                    snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                    snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|firmwareType must be either 'PCI' or 'DRI'.\n");
                    updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type.c_str());
                    status = Core::ERROR_INVALID_PARAMETER;
                    return status;
                }
            }
            else
            {
                SWUPDATEERR("firmwareType is empty");
                dispatchAndUpdateEvent(_VALIDATION_FAILED,"");
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|firmwareType is empty\n");
                updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type.c_str());
                status = Core::ERROR_INVALID_PARAMETER;
                return status;
            }

            // Ensure only one flashing operation happens at a time
            bool expected = false;
            if (!isFlashingInProgress.compare_exchange_strong(expected, true)) {
                SWUPDATEERR("Error: Flashing is already in progress. Cannot start a new operation.");
                snprintf(fwdls.status, sizeof(fwdls.status), "Status|Failure\n");
                snprintf(fwdls.FwUpdateState, sizeof(fwdls.FwUpdateState), "FwUpdateState|Failed\n");
                snprintf(fwdls.failureReason, sizeof(fwdls.failureReason), "FailureReason|Flashing is already in progress\n");
                updateFWDownloadStatus(&fwdls, dri.c_str(),initiated_type.c_str());
                status = Core::ERROR_FIRMWAREUPDATE_INPROGRESS;
                return status;
            }

            if (flashThread.joinable()) {
                SWUPDATEINFO("flashThread is still running or joinable. Joining now...");
                flashThread.join();  // Ensure the thread has completed before main exits
            }
            // Start a new flashing thread
            flashThread = std::thread(&WPEFramework::Plugin::FirmwareUpdateImplementation::flashImageThread, this, firmwareFilepath, firmwareType);
            result.success = true;
            status =Core::ERROR_NONE;

            return status;
        }

        Core::hresult FirmwareUpdateImplementation::GetUpdateState(GetUpdateStateResult& getUpdateStateResult ) 
        {
            Core::hresult status = Core::ERROR_GENERAL;    
            std::string strState = "";
            std::string strSubState = "";
            if(FirmwareStatus(strState, strSubState, "read"))
            {

                auto it = firmwareState.find(strState);
                if (it != firmwareState.end()) {
                    getUpdateStateResult.state  = it->second;
                }

                auto it1 = firmwareSubState.find(strSubState);
                if (it1 != firmwareSubState.end()) {
                    getUpdateStateResult.substate  = it1->second;
                }

                SWUPDATEINFO("FirmwareStatus state :%s substate :%s",strState.c_str(),strSubState.c_str());
                status = Core::ERROR_NONE;
            }
            else
            {
                LOGERR("FirmwareStatus is failed to get state & substate");
                SWUPDATEERR("FirmwareStatus is failed to get state & substate");
                status = Core::ERROR_NONE;
                return status;
            }
            return status;
        }


    } // namespace Plugin
} // namespace WPEFramework


//Helper start
void eventManager(const char *cur_event_name, const char *event_status) {
    struct eventList {
        const char* event_name;
        unsigned char sys_state_event;
    } event_list[] = { 
        { "ImageDwldEvent", IARM_BUS_SYSMGR_SYSSTATE_FIRMWARE_DWNLD },
        { "FirmwareStateEvent", IARM_BUS_SYSMGR_SYSSTATE_FIRMWARE_UPDATE_STATE }
    };
    IARM_Bus_SYSMgr_EventData_t event_data;
    int i;
    int len = sizeof(event_list) / sizeof(struct eventList);
    IARM_Result_t ret_code = IARM_RESULT_SUCCESS; 
    bool event_match = false;


    if(cur_event_name == NULL || event_status == NULL) {
        SWUPDATEERR("eventManager() failed due to NULL parameter\n");
        return;
    }
    SWUPDATEINFO("%s: Generate IARM_BUS_NAME current event=%s\n", __FUNCTION__, cur_event_name);
    if ( !(strncmp(cur_event_name,"MaintenanceMGR", 14)) ) {
        IARM_Bus_MaintMGR_EventData_t infoStatus;
        unsigned int main_mgr_event;

        memset( &infoStatus, 0, sizeof(IARM_Bus_MaintMGR_EventData_t) );
        main_mgr_event = atoi(event_status);
        SWUPDATEINFO(">>>>> Identified MaintenanceMGR with event value=%u", main_mgr_event);
        infoStatus.data.maintenance_module_status.status = (IARM_Maint_module_status_t)main_mgr_event;
        ret_code=IARM_Bus_BroadcastEvent(IARM_BUS_MAINTENANCE_MGR_NAME,(IARM_EventId_t)IARM_BUS_MAINTENANCEMGR_EVENT_UPDATE, (void *)&infoStatus, sizeof(infoStatus));
        SWUPDATEINFO(">>>>> IARM %s  Event  = %d",(ret_code == IARM_RESULT_SUCCESS) ? "SUCCESS" : "FAILURE",\
                infoStatus.data.maintenance_module_status.status);
    }
    else
    {
        SWUPDATEINFO( "%s: event_status = %u\n", __FUNCTION__, atoi(event_status) );
        for( i = 0; i < len; i++ ) {
            if(!(strcmp(cur_event_name, event_list[i].event_name))) {
                event_data.data.systemStates.stateId = static_cast<IARM_Bus_SYSMgr_SystemState_t>(event_list[i].sys_state_event);
                event_data.data.systemStates.state = atoi(event_status);
                event_match = true;
                break;
            }
        }
        if(event_match == true) {
            event_data.data.systemStates.error = 0;
            ret_code = IARM_Bus_BroadcastEvent(IARM_BUS_SYSMGR_NAME, (IARM_EventId_t) IARM_BUS_SYSMGR_EVENT_SYSTEMSTATE, (void *) &event_data,
                    sizeof(event_data));
            if(ret_code == IARM_RESULT_SUCCESS) {
                SWUPDATEINFO("%s : >> IARM SUCCESS  Event=%s,sysStateEvent=%d\n", __FUNCTION__, event_list[i].event_name, event_list[i].sys_state_event);
            }else {
                SWUPDATEERR("%s : >> IARM FAILURE  Event=%s,sysStateEvent=%d\n", __FUNCTION__, event_list[i].event_name, event_list[i].sys_state_event);
            }
        }else {
            SWUPDATEERR("%s: There are no matching IARM sys events for %s\n", __FUNCTION__, cur_event_name);
        }
    }
    SWUPDATEINFO("%s : IARM_event_sender closing\n", __FUNCTION__);
}

/** Description: Getting device property by reading device.property file
 * @param: dev_prop_name : Device property name to get from file
 * @param: out_data : pointer to hold the device property get from file
 * @param: buff_size : Buffer size of the out_data.
 * @return int: Success: UTILS_SUCCESS  and Fail : UTILS_FAIL
 * */
int getDevicePropertyData(const char *dev_prop_name, char *out_data, unsigned int buff_size)
{
    int ret = UTILS_FAIL;
    FILE *fp;
    char tbuff[MAX_DEVICE_PROP_BUFF_SIZE];
    char *tmp;
    int index;

    if (out_data == NULL || dev_prop_name == NULL) {
        SWUPDATEERR("%s : parameter is NULL\n", __FUNCTION__);
        return ret;
    }
    if (buff_size == 0 || buff_size > MAX_DEVICE_PROP_BUFF_SIZE) {
        SWUPDATEERR("%s : buff size not in the range. size should be < %d\n", __FUNCTION__, MAX_DEVICE_PROP_BUFF_SIZE);
        return ret;
    }
    SWUPDATEINFO("%s : Trying device property data for %s and buf size=%u\n", __FUNCTION__, dev_prop_name, buff_size);
    fp = fopen(DEVICE_PROPERTIES_FILE, "r");
    if(fp == NULL) {
        SWUPDATEERR("%s : device.property File not found\n", __FUNCTION__);
        return ret;
    }
    while((fgets(tbuff, sizeof(tbuff), fp) != NULL)) {
        if(strstr(tbuff, dev_prop_name)) {
            index = strcspn(tbuff, "\n");
            if (index > 0) {
                tbuff[index] = '\0';
            }
            tmp = strchr(tbuff, '=');
            if(tmp != NULL) {
                snprintf(out_data, buff_size, "%s", tmp+1);
                SWUPDATEINFO("%s : %s=%s\n", __FUNCTION__, dev_prop_name, out_data);
                ret = UTILS_SUCCESS;
                break;
            } else {
                SWUPDATEERR("%s : strchr failed. '=' not found. str=%s\n", __FUNCTION__, tbuff);
            }
        }
    }
    fclose(fp);
    return ret;
}

/* Description: Checking device type
 * @param : void
 * @return : mediaclient true and not mediaclient false
 * */
bool isMediaClientDevice(void){
    bool isMediaClientDevice = false ;
    int ret = UTILS_FAIL;
    const char *dev_prop_name = "DEVICE_TYPE";
    char dev_type[16];

    // The device type field from device.properties and determine platform type
    ret = getDevicePropertyData(dev_prop_name, dev_type, sizeof(dev_type));
    if (ret == UTILS_SUCCESS) {
        SWUPDATEINFO("%s: device name from device.property file=%s\n", __FUNCTION__, dev_type);
    } else {
        SWUPDATEINFO("%s: device name not present device.property file\n", __FUNCTION__);
        return isMediaClientDevice;
    }
    if ((strncmp(dev_type, "mediaclient", 11)) == 0) {
        SWUPDATEINFO("Device is a Mediaclient\n");
        isMediaClientDevice = true ;
    } else {
        SWUPDATEINFO("Device is not a Mediaclient\n");
    }
    return isMediaClientDevice ;

}

void updateUpgradeFlag(int proto ,int action)
{
    const char *flag_file = NULL;
    if (isMediaClientDevice()) {
        flag_file = "/tmp/.imageDnldInProgress";
    } else if (proto == 1) {
        flag_file = HTTP_CDL_FLAG;
    } else {
        flag_file = SNMP_CDL_FLAG;
    }
    if (action == 1) {
        std::ifstream infile(flag_file);
        if (!infile.good()) {
            // File doesn't exist, so create it
            std::ofstream outfile(flag_file);
            if (outfile) {
                SWUPDATEINFO("File created successfully: %s\n", flag_file);
            } else {
                SWUPDATEERR("Error creating file: %s\n", flag_file);
            }
        }
    } else if (action == 2 && ( Utils::fileExists(flag_file))) {
        unlink(flag_file);
    }
}

std::string extractField(const std::string& line, char delimiter, int fieldIndex) {
    std::stringstream ss(line);
    std::string field;
    for (int i = 0; i <= fieldIndex; ++i) {
        if (!std::getline(ss, field, delimiter)) {
            return ""; // Return an empty string if the field index is out of range
        }
    }
    return field;
}

/* Description: Updating Firmware Download status inside status File.
 * @param fwdls: This is structure use to Receive all data required to write inside status file.
 * */
int updateFWDownloadStatus(struct FWDownloadStatus *fwdls, const char *disableStatsUpdate , const char *initiated_type) {
    FILE *fp;
    std::string lastSuccessfulRun = "";
    std::string currentVersion = "";
    std::string currentFile = "";
    std::string lastSuccessfulUpgradeFile = "";

    //Delete STATUS_FILE if already exist and create new one
    if ( Utils::fileExists(STATUS_FILE)) {
        unlink(STATUS_FILE);
    }
    else {
        std::ifstream infile(STATUS_FILE);
        if (!infile.good()) {
            // File doesn't exist, so create it
            std::ofstream outfile(STATUS_FILE);
            if (outfile) {
                SWUPDATEINFO("File created successfully: %s\n", STATUS_FILE);
            } else {
                SWUPDATEERR("Error creating file: %s\n", STATUS_FILE);
            }
        }
    }

    //strncpy(disableStatsUpdate, "no", 2); // For testing. Need to remove final change
    //Flag to disable STATUS_FILE updates in case of PDRI upgrade
    if (fwdls == NULL || disableStatsUpdate == NULL) {
        SWUPDATEERR("updateFWDownloadStatus(): Parameter is NULL\n");
        return FAILURE;
    }
    if((strcmp(disableStatsUpdate, "yes")) == 0) {
        SWUPDATEINFO("updateFWDownloadStatus(): Status Update Disable:%s\n", disableStatsUpdate);
        return SUCCESS;
    }

    if (0 == ((strncmp(initiated_type, "user", 4)))) {

        // Reading LastSuccessfulRun
        std::ifstream fwFile(STATUS_FILE);
        if (fwFile) {
            std::string line;
            while (std::getline(fwFile, line)) {
                if (line.find("LastSuccessfulRun") != std::string::npos) {
                    lastSuccessfulRun = extractField(line, '|', 1);
                    break;
                }
            }
            fwFile.close();
        }

        // Reading CurrentVersion
        std::ifstream versionFile("/version.txt");
        if (versionFile) {
            std::string line;
            while (std::getline(versionFile, line)) {
                if (line.find("imagename") != std::string::npos) {
                    currentVersion = extractField(line, ':', 1);
                    break;
                }
            }
            versionFile.close();
        }

        // Reading CurrentFile
        std::ifstream currentFileStream("/tmp/currently_running_image_name");
        if (currentFileStream) {
            std::getline(currentFileStream, currentFile);
            currentFileStream.close();
        }

        // Reading LastSuccessfulUpgradeFile
        std::ifstream upgradeFileStream("/opt/cdl_flashed_file_name");
        if (upgradeFileStream) {
            std::getline(upgradeFileStream, lastSuccessfulUpgradeFile);
            upgradeFileStream.close();
        }
    }
    fp = fopen(STATUS_FILE, "w");
    if(fp == NULL) {
        SWUPDATEERR("updateFWDownloadStatus(): fopen failed:%s\n", STATUS_FILE);
        return FAILURE;
    }

    SWUPDATEINFO("updateFWDownloadStatus(): Going to write:%s\n", STATUS_FILE);
    //TODO: Need to implement if FwUpdateState not present read from STATUS_FILE file
    fprintf( fp, "%s", fwdls->method );
    fprintf( fp, "%s", fwdls->proto );
    fprintf( fp, "%s", fwdls->status );
    fprintf( fp, "%s", fwdls->reboot );
    fprintf( fp, "%s", fwdls->failureReason );
    fprintf( fp, "%s", fwdls->dnldVersn );
    fprintf( fp, "%s", fwdls->dnldfile );
    fprintf( fp, "%s", fwdls->dnldurl );
    fprintf( fp, "%s", fwdls->lastrun );
    fprintf( fp, "%s", fwdls->FwUpdateState );
    fprintf( fp, "%s", fwdls->DelayDownload );
    if (0 == ((strncmp(initiated_type, "user", 4)))) {
        fprintf(fp, "LastSuccessfulRun|%s\n", lastSuccessfulRun.c_str());
        fprintf(fp, "CurrentVersion|%s\n", currentVersion.c_str());
        fprintf(fp, "CurrentFile|%s\n", currentFile.c_str());
        fprintf(fp, "LastSuccessfulUpgradeFile|%s\n", lastSuccessfulUpgradeFile.c_str());
    }
    fclose(fp);
    return SUCCESS;
}


/* Description: Reading rfc data
 * @param type : rfc type
 * @param key: rfc key
 * @param data : Store rfc value
 * @return int 1 READ_RFC_SUCCESS on success and READ_RFC_FAILURE -1 on failure
 * */
int read_RFCProperty(char* type, const char* key, char *out_value, size_t datasize) {
    RFC_ParamData_t param;
    int data_len;
    int ret = READ_RFC_FAILURE;
    char intermediateBuffer[1000];

    if(key == NULL || out_value == NULL || datasize == 0 || type == NULL) {
        SWUPDATEERR("read_RFCProperty() one or more input values are invalid\n");
        return ret;
    }
    WDMP_STATUS status = getRFCParameter(type, key, &param);
    if(status == WDMP_SUCCESS || status == WDMP_ERR_DEFAULT_VALUE) {
        data_len = strlen(param.value);
        if(data_len >= 2 && (param.value[0] == '"') && (param.value[data_len - 1] == '"')) {
            // remove quotes arround data
            snprintf( out_value, datasize, "%s", &param.value[1] );
            *(out_value + data_len - 2) = 0;
        }else {
            snprintf( out_value, datasize, "%s", param.value );
        }
        snprintf(intermediateBuffer, sizeof(intermediateBuffer),"read_RFCProperty() name=%.*s,type=%d,value=%.*s,status=%d\n",
                256, param.name ? param.name : "(null)",
                param.type,
                256, param.value ? param.value : "(null)",
                status);
        SWUPDATEINFO("%s", intermediateBuffer);
        ret = READ_RFC_SUCCESS;
    }else {
        SWUPDATEERR("error:read_RFCProperty(): status= %d\n", status);
        *out_value = 0;
    }
    return ret;
}


/* Description: Writing rfc data
 * @param type : rfc type
 * @param key: rfc key
 * @param data : new rfc value
 * @param datatype: data type of value parameter
 * @return int 1 WRITE_RFC_SUCCESS on success and WRITE_RFC_FAILURE -1 on failure
 * */
int write_RFCProperty(char* type, const char* key, const char *value, RFCVALDATATYPE datatype) {
    WDMP_STATUS status = WDMP_FAILURE;
    int ret = WRITE_RFC_FAILURE;
    if (type == NULL || key == NULL || value == NULL) {
        SWUPDATEERR("%s: Parameter is NULL\n", __FUNCTION__);
        return ret;
    }
    if (datatype == RFC_STRING) {
        status = setRFCParameter(type, key, value, WDMP_STRING);
    } else if(datatype == RFC_UINT) {
        status = setRFCParameter(type, key, value, WDMP_UINT);
    } else {
        status = setRFCParameter(type, key, value, WDMP_BOOLEAN);
    }
    if (status != WDMP_SUCCESS) {
        SWUPDATEERR("%s: setRFCParameter failed. key=%s and status=%s\n", __FUNCTION__, key, getRFCErrorString(status));
    } else {
        SWUPDATEINFO("%s: setRFCParameter Success\n", __FUNCTION__);
        ret = WRITE_RFC_SUCCESS;
    }
    return ret;
}

/* Description: Cheacking notify rfc status
 * @param type : void
 * @return bool true : enable and false: disable
 * */
bool isMmgbleNotifyEnabled(void)
{
    bool status = false;
    int ret = -1;
    char rfc_data[RFC_VALUE_BUF_SIZE];

    *rfc_data = 0;
    char type[20] ={0};
    strcpy(type, "ManageNotify");
    ret = read_RFCProperty(type, RFC_MNG_NOTIFY, rfc_data, sizeof(rfc_data));
    if (ret == -1) {
        SWUPDATEERR("%s: ManageNotify rfc=%s failed Status %d\n", __FUNCTION__, RFC_MNG_NOTIFY, ret);
        return status;
    } else {
        SWUPDATEINFO("%s: rfc ManageNotify= %s\n", __FUNCTION__, rfc_data);
        if ((strncmp(rfc_data, "true", 4)) == 0) {
            status = true;
        }
    }
    return status;
}

/* Description: Update optout option to ENFORCE_OPTOUT
 * @param: optout_file_name : pointer to optout config file name
 * return :true = success
 * return :false = failure */
bool updateOPTOUTFile(const char *optout_file_name)
{
    bool opt_status = false;
    bool enforce_optout_set = false;
    FILE *fp = NULL;
    FILE *fp_write = NULL;
    char tbuff[80] = {0};
    const char *update_data = "softwareoptout=ENFORCE_OPTOUT\n";
    int ret = -1;
    if (optout_file_name == NULL) {
        SWUPDATEERR("%s: parameter is NULL\n", __FUNCTION__);
        return opt_status;
    }
    fp_write = fopen(MAINTENANCE_MGR_RECORD_UPDATE_FILE, "w");
    if (fp_write == NULL) {
        SWUPDATEERR("%s: Unable to create file:%s\n", __FUNCTION__, MAINTENANCE_MGR_RECORD_UPDATE_FILE);
        return opt_status;
    }
    fp = fopen(optout_file_name , "r");
    if (fp != NULL) {
        while (NULL != (fgets(tbuff, sizeof(tbuff), fp))) {
            if ((NULL != (strstr(tbuff, "softwareoptout"))) && (NULL != (strstr(tbuff, "BYPASS_OPTOUT")))) {
                SWUPDATEINFO("optout set to:%s\n", update_data);
                fputs(update_data, fp_write);
                enforce_optout_set = true;
            }else {
                fputs(tbuff, fp_write);
            }
        }
        fclose(fp);
        fclose(fp_write);
        if (enforce_optout_set == true) {
            /*rename updated file to orginal optout config file*/
            ret = rename(MAINTENANCE_MGR_RECORD_UPDATE_FILE, optout_file_name);
            if (ret == 0) {
                SWUPDATEINFO("rename optout file to %s\n", optout_file_name);
                opt_status = true;
            }else {
                SWUPDATEERR("fail to rename optout file to %s: error=%d\n", optout_file_name, ret);
            }
        }
    }else {
        SWUPDATEERR("optout file:%s not present\n", optout_file_name);
        fclose(fp_write);
    }
    unlink(MAINTENANCE_MGR_RECORD_UPDATE_FILE);
    return opt_status;
}

/* Description: Updating download status to the rfc.
 * @param key: Pointer to the rfc name
 * @param value: Rfc value going to set
 * */
int notifyDwnlStatus(const char *key, const char *value, RFCVALDATATYPE datatype) {
    int ret = WRITE_RFC_FAILURE;
    if (key == NULL || value == NULL) {
        SWUPDATEERR("%s: Parameter is NULL\n", __FUNCTION__);
    } else {
        char type[20] ={0};
        strcpy(type, "NotifyDwnlSt");
        ret = write_RFCProperty(type, key, value, datatype);
    }
    return ret;
}

void unsetStateRed(void)
{
    if (Utils::fileExists(STATEREDFLAG) ) {
        SWUPDATEINFO("RED:unsetStateRed: Exiting State Red\n");
        unlink(STATEREDFLAG);
    } else {
        SWUPDATEINFO("RED:unsetStateRed: Not in State Red\n");
    }
}

string deviceSpecificRegexBin() 
{
    static string result;
    static std::once_flag flag;
    char model_num[32] = {0};
    std::call_once(flag, [&]() {
            getDevicePropertyData("MODEL_NUM", model_num, sizeof(model_num));
            result = ( std::string(model_num) + REGEX_BIN);

            SWUPDATEINFO("bin file regex for device '%s' is '%s'", model_num, result.c_str());
            });
    return result;
}
string deviceSpecificRegexPath(){
    static string result;
    static std::once_flag flag;
    std::call_once(flag, [&](){
            result = REGEX_PATH;

            SWUPDATEINFO("regex for device is '%s'", result.c_str());

            });
    return result;
}

bool createDirectory(const std::string &path) {
    struct stat st = {0};
    // Check if the directory exists
    if (stat(path.c_str(), &st) == -1) {
        // Create the directory
        if (mkdir(path.c_str(), 0755) != 0) {
            SWUPDATEERR("Error creating directory: %s\n", strerror(errno));
            return false;
        }
    }
    return true;
}

bool copyFileToDirectory(const char *source_file, const char *destination_dir) {
    if (!source_file || !destination_dir) {
        SWUPDATEERR("Invalid input: source or destination is null.\n");
        return false;
    }

    // Ensure the destination directory exists
    if (!createDirectory(destination_dir)) {
        SWUPDATEERR("Failed to create or access directory: %s\n", destination_dir);
        return false;
    }

    // Extract file name from the source file path
    const char *file_name = strrchr(source_file, '/');
    if (!file_name) {
        SWUPDATEERR("Invalid source file path: %s\n", source_file);
        return false;
    }
    file_name++; // Skip the '/' character

    // Construct the destination file path
    std::string dest_file_path = std::string(destination_dir) + "/" + file_name;

    // Check if the file already exists at the destination
    if (access(dest_file_path.c_str(), F_OK) == 0) {
        SWUPDATEINFO("File already exists at destination. Removing old file...\n");
        if (unlink(dest_file_path.c_str()) != 0) {
            SWUPDATEERR("Error removing old file: %s\n", strerror(errno));
            return false;
        }
    }

    // Open the source file
    std::ifstream src(source_file, std::ios::binary);
    if (!src) {
        SWUPDATEERR("Error: Could not open source file %s\n", source_file);
        return false;
    }

    // Open the destination file
    std::ofstream dest(dest_file_path, std::ios::binary);
    if (!dest) {
        SWUPDATEERR("Error: Could not open destination file %s\n", dest_file_path.c_str());
        return false;
    }

    // Copy the file content
    dest << src.rdbuf();

    // Check if the copy was successful
    if (!src || !dest) {
        SWUPDATEERR("Error: File copy failed.\n");
        return false;
    }

    SWUPDATEINFO("File copied successfully to %s\n", dest_file_path.c_str());
    return true;
}
bool FirmwareStatus(std::string& state, std::string& substate, const std::string& mode) {
    auto writeFile = [](const std::string& state, const std::string& substate) -> bool {
        std::ofstream file(FIRMWARE_UPDATE_STATE);
        if (!file.is_open()) {
            SWUPDATEERR("Error opening the file for writing." );
            return false;
        }

        file << "state:" << state << std::endl;
        file << "substate:" << substate << std::endl;

        file.close();
        return true;
    };

    auto readFile = [](std::string& state, std::string& substate) -> bool {
        std::ifstream file(FIRMWARE_UPDATE_STATE);
        if (!file.is_open()) {
            SWUPDATEERR("Error: File not found.");
            return false;
        }

        std::string line;
        bool stateFound = false, substateFound = false;

        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string key, value;
            std::getline(ss, key, ':');
            std::getline(ss, value);

            if (key == "state") {
                state = value;
                stateFound = true;
            }
            if (key == "substate") {
                substate = value;
                substateFound = true;
            }

            if (stateFound && substateFound) {
                break;
            }
        }

        file.close();
        return stateFound && substateFound;
    };

    // Handle the mode
    if (mode == "write") {
        return writeFile(state, substate);
    }
    else if (mode == "read") {
        return readFile(state, substate);
    }
    else {
        SWUPDATEERR("Error: Invalid mode provided. Use 'read' or 'write'.");
        return false;
    }
}
std::string GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << millis.count()
        << "Z";
    return oss.str();
}

std::string readProperty( std::string filename,std::string property, std::string delimiter) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        SWUPDATEERR("Error: Unable to open file: %s",filename.c_str());
        return "";
    }

    std::string line;
    while (std::getline(file, line)) {
        // If delimiter is empty, check if the line starts with the property
        if (delimiter.empty()) {
            if (line.find(property) == 0) {
                return line.substr(property.size());
            }
        } else {
            // Check if the line starts with property + delimiter
            std::string searchKey = property + delimiter;
            if (line.find(searchKey) == 0) {
                return line.substr(searchKey.size());
            }
        }
    }

    SWUPDATEERR("Error: Property not found:: %s",property.c_str());
    return "";
}


//Helper end
