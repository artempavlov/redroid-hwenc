/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * HIDL port of redroid-hwenc's VA-API Codec2 service, for Android 11/12/13
 * (none of which have the AIDL Codec2 HAL -- confirmed by inspecting AOSP
 * source directly at android-11.0.0_r48, android-12.0.0_r34 and
 * android-13.0.0_r83: media/codec2/hal/aidl only appears starting at
 * android-14.0.0_r75; before that there is only media/codec2/hidl/{1.0,1.1}).
 * The real work (VaapiEncComponent/VaapiDecComponent, talking to the
 * tier5-vaapi-daemon over the same host-side protocol) is untouched --
 * identical to the AIDL version's service.cpp. Only the registration layer
 * changes: HIDL's registerAsService() (hwservicemanager) instead of AIDL's
 * AServiceManager_addService() (servicemanager). Adapted from AOSP's own
 * empty-service template at frameworks/av/media/codec2/hidl/services/vendor.cpp.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "android.hardware.media.c2@1.1-vaapi-service"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.1/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

#include <util/C2InterfaceHelper.h>
#include <C2Component.h>
#include <C2Config.h>

#include "VaapiEncComponent.h"
#include "VaapiDecComponent.h"

static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-vaapi-seccomp_policy";

static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-vaapi-extended-seccomp_policy";

class StoreImpl : public C2ComponentStore {
public:
    StoreImpl()
        : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
          mInterface(mReflectorHelper) {
    }

    virtual ~StoreImpl() override = default;

    virtual C2String getName() const override {
        return "default";
    }

    virtual c2_status_t createComponent(
            C2String name,
            std::shared_ptr<C2Component>* const component) override {
        if (name == "c2.hardware.encoder.h264") {
            auto intf = std::make_shared<android::VaapiEncInterface>(mReflectorHelper);
            *component = std::make_shared<android::VaapiEncComponent>(name.c_str(), mNextId++, intf);
            return C2_OK;
        }
        if (const android::VaapiDecCodec *codec = android::findVaapiDecCodec(name)) {
            auto intf = std::make_shared<android::VaapiDecInterface>(mReflectorHelper, codec);
            *component = std::make_shared<android::VaapiDecComponent>(name.c_str(), mNextId++, intf, codec);
            return C2_OK;
        }
        return C2_NOT_FOUND;
    }

    virtual c2_status_t createInterface(
            C2String name,
            std::shared_ptr<C2ComponentInterface>* const interface) override {
        if (name == "c2.hardware.encoder.h264") {
            auto intf = std::make_shared<android::VaapiEncInterface>(mReflectorHelper);
            *interface = std::make_shared<
                    android::SimpleInterface<android::VaapiEncInterface>>(
                    name.c_str(), mNextId++, intf);
            return C2_OK;
        }
        if (const android::VaapiDecCodec *codec = android::findVaapiDecCodec(name)) {
            auto intf = std::make_shared<android::VaapiDecInterface>(mReflectorHelper, codec);
            *interface = std::make_shared<
                    android::SimpleInterface<android::VaapiDecInterface>>(
                    name.c_str(), mNextId++, intf);
            return C2_OK;
        }
        return C2_NOT_FOUND;
    }

    virtual std::vector<std::shared_ptr<const C2Component::Traits>>
            listComponents() override {
        auto encTraits = std::make_shared<C2Component::Traits>();
        encTraits->name = "c2.hardware.encoder.h264";
        encTraits->domain = C2Component::DOMAIN_VIDEO;
        encTraits->kind = C2Component::KIND_ENCODER;
        encTraits->rank = 1;
        encTraits->mediaType = "video/avc";
        encTraits->owner = "vaapi";

        LOG(ERROR) << "listComponents: encTraits name=" << encTraits->name
                   << " domain=" << (int)encTraits->domain << " kind=" << (int)encTraits->kind;
        std::vector<std::shared_ptr<const C2Component::Traits>> traits = {encTraits};
        for (const android::VaapiDecCodec &codec : android::vaapiDecCodecs()) {
            auto decTraits = std::make_shared<C2Component::Traits>();
            decTraits->name = codec.name;
            decTraits->domain = C2Component::DOMAIN_VIDEO;
            decTraits->kind = C2Component::KIND_DECODER;
            decTraits->rank = 1;
            decTraits->mediaType = codec.mediaType;
            decTraits->owner = "vaapi";
            traits.push_back(decTraits);
        }
        LOG(ERROR) << "listComponents: returning " << traits.size() << " traits";
        return traits;
    }

    virtual c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> /* src */,
            std::shared_ptr<C2GraphicBuffer> /* dst */) override {
        return C2_OMITTED;
    }

    virtual c2_status_t query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const override {
        return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
    }

    virtual c2_status_t config_sm(
            const std::vector<C2Param*>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override {
        return mInterface.config(params, C2_MAY_BLOCK, failures);
    }

    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override {
        return mReflectorHelper;
    }

    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override {
        return mInterface.querySupportedParams(params);
    }

    virtual c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override {
        return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
    }

private:
    class Interface : public C2InterfaceHelper {
    public:
        Interface(const std::shared_ptr<C2ReflectorHelper> &helper)
            : C2InterfaceHelper(helper) {
            setDerivedInstance(this);

            addParameter(
                DefineParam(mIonUsageInfo, "ion-usage")
                .withDefault(new C2StoreIonUsageInfo())
                .withFields({
                    C2F(mIonUsageInfo, usage).flags(
                            {C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mIonUsageInfo, capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mIonUsageInfo, heapMask).any(),
                    C2F(mIonUsageInfo, allocFlags).flags({}),
                    C2F(mIonUsageInfo, minAlignment).equalTo(0)
                })
                .withSetter(SetIonUsage)
                .build());
        }

        virtual ~Interface() = default;

    private:
        static C2R SetIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
            me.set().heapMask = ~0;
            me.set().allocFlags = 0;
            me.set().minAlignment = 0;
            return C2R::Ok();
        }

        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
    };
    std::shared_ptr<C2ReflectorHelper> mReflectorHelper;
    Interface mInterface;
    c2_node_id_t mNextId = 0;
};

int main(int /* argc */, char** /* argv */) {
    using namespace ::android;
    LOG(DEBUG) << "android.hardware.media.c2@1.1-vaapi-service starting...";

    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Enable vndbinder to allow vendor-to-vendor binder calls.
    ProcessState::initWithDriver("/dev/vndbinder");
    ProcessState::self()->startThreadPool();
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    {
        using namespace ::android::hardware::media::c2::V1_1;
        sp<IComponentStore> store;

        LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
        store = new utils::ComponentStore(std::make_shared<StoreImpl>());

        if (store == nullptr) {
            LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
        } else {
            constexpr char const* serviceName = "default";
            if (store->registerAsService(serviceName) != OK) {
                LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                              " with instance name \"" << serviceName << "\".";
            } else {
                LOG(DEBUG) << "Codec2's IComponentStore service registered. "
                              "Instance name: \"" << serviceName << "\".";
            }
        }
    }

    hardware::joinRpcThreadpool();
    return 0;
}
