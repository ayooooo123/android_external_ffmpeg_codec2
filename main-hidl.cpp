/*
 * Copyright (C) 2022 Michael Goffioul <michael.goffioul@gmail.com>
 * Copyright (C) 2025 KonstaKANG
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

#define LOG_TAG "android.hardware.media.c2@1.2-service-ffmpeg"

#include "C2FFMPEGComponentStore.h"
#include "C2FFMPEGZeroCopyRegistry.h"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>
#include <set>

using namespace ::android;
using namespace ::android::hardware::media::c2::V1_2;
using ::android::hardware::media::c2::V1_0::IComponentInterface;
using ::android::hardware::media::c2::V1_0::IComponentListener;
using ::android::hardware::media::c2::V1_0::IInputSink;
using ::android::hardware::media::c2::V1_0::IInputSurface;
using ::android::hardware::media::c2::V1_0::IInputSurfaceConnection;
using ::android::hardware::media::c2::V1_0::IConfigurable;
using ::android::hardware::media::c2::V1_0::Status;
using ::android::hardware::media::c2::V1_0::WorkBundle;
using ::android::hardware::media::c2::V1_2::SurfaceSyncObj;
using ::android::hardware::media::bufferpool::V2_0::IClientManager;
using HGraphicBufferProducer1 = ::android::hardware::graphics::bufferqueue::V1_0::IGraphicBufferProducer;
using HGraphicBufferProducer2 = ::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer;
using ::android::hardware::media::omx::V1_0::IGraphicBufferSource;
using ::android::hardware::Return;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Void;

class ComponentWrapper : public IComponent {
public:
    ComponentWrapper(const sp<IComponent>& base) : mBase(base) {}
    virtual ~ComponentWrapper() override {
        unregisterMyPools();
    }

    virtual Return<Status> queue(const WorkBundle& workBundle) override {
        return mBase->queue(workBundle);
    }

    virtual Return<void> flush(flush_cb _hidl_cb) override {
        return mBase->flush(_hidl_cb);
    }

    virtual Return<Status> drain(bool withEos) override {
        return mBase->drain(withEos);
    }

    virtual Return<Status> setOutputSurface(
            uint64_t blockPoolId,
            const sp<HGraphicBufferProducer2>& surface) override {
        if (surface != nullptr) {
            uint64_t bqId = 0;
            Return<uint64_t> ret = surface->getUniqueId();
            if (ret.isOk()) {
                bqId = ret;
            }
            {
                std::lock_guard<std::mutex> lock(mPoolLock);
                mRegisteredPoolIds.insert(blockPoolId);
            }
            SurfaceRegistry::get().registerSurface(
                    blockPoolId, surface, nullptr, bqId, 0, 0);
        } else {
            {
                std::lock_guard<std::mutex> lock(mPoolLock);
                mRegisteredPoolIds.erase(blockPoolId);
            }
            SurfaceRegistry::get().unregisterSurface(blockPoolId);
        }
        return mBase->setOutputSurface(blockPoolId, surface);
    }

    virtual Return<void> connectToInputSurface(
            const sp<IInputSurface>& inputSurface,
            connectToInputSurface_cb _hidl_cb) override {
        return mBase->connectToInputSurface(inputSurface, _hidl_cb);
    }

    virtual Return<void> connectToOmxInputSurface(
            const sp<HGraphicBufferProducer1>& producer,
            const sp<IGraphicBufferSource>& source,
            connectToOmxInputSurface_cb _hidl_cb) override {
        return mBase->connectToOmxInputSurface(producer, source, _hidl_cb);
    }

    virtual Return<Status> disconnectFromInputSurface() override {
        return mBase->disconnectFromInputSurface();
    }

    virtual Return<void> createBlockPool(
            uint32_t allocatorId,
            createBlockPool_cb _hidl_cb) override {
        return mBase->createBlockPool(allocatorId, _hidl_cb);
    }

    virtual Return<Status> destroyBlockPool(uint64_t blockPoolId) override {
        {
            std::lock_guard<std::mutex> lock(mPoolLock);
            mRegisteredPoolIds.erase(blockPoolId);
        }
        SurfaceRegistry::get().unregisterSurface(blockPoolId);
        return mBase->destroyBlockPool(blockPoolId);
    }

    virtual Return<Status> start() override {
        return mBase->start();
    }

    virtual Return<Status> stop() override {
        return mBase->stop();
    }

    virtual Return<Status> reset() override {
        unregisterMyPools();
        return mBase->reset();
    }

    virtual Return<Status> release() override {
        unregisterMyPools();
        return mBase->release();
    }

    virtual Return<sp<IComponentInterface>> getInterface() override {
        return mBase->getInterface();
    }

    virtual Return<sp<IInputSink>> asInputSink() override {
        return mBase->asInputSink();
    }

    virtual Return<void> configureVideoTunnel(
            uint32_t avSyncHwId, configureVideoTunnel_cb _hidl_cb) override {
        return mBase->configureVideoTunnel(avSyncHwId, _hidl_cb);
    }

    virtual Return<Status> setOutputSurfaceWithSyncObj(
            uint64_t blockPoolId,
            const sp<HGraphicBufferProducer2>& surface,
            const SurfaceSyncObj& syncObject) override {
        if (surface != nullptr) {
            const native_handle_t *h = syncObject.syncMemory;
            native_handle_t *syncMemory = h ? native_handle_clone(h) : nullptr;
            std::shared_ptr<C2SurfaceSyncMemory> syncMem =
                    syncMemory ? C2SurfaceSyncMemory::Import(syncMemory) : nullptr;
            {
                std::lock_guard<std::mutex> lock(mPoolLock);
                mRegisteredPoolIds.insert(blockPoolId);
            }
            SurfaceRegistry::get().registerSurface(
                    blockPoolId, surface, syncMem,
                    syncObject.bqId, syncObject.generationId, syncObject.consumerUsage);
        } else {
            {
                std::lock_guard<std::mutex> lock(mPoolLock);
                mRegisteredPoolIds.erase(blockPoolId);
            }
            SurfaceRegistry::get().unregisterSurface(blockPoolId);
        }
        return mBase->setOutputSurfaceWithSyncObj(blockPoolId, surface, syncObject);
    }

private:
    void unregisterMyPools() {
        std::lock_guard<std::mutex> lock(mPoolLock);
        for (uint64_t poolId : mRegisteredPoolIds) {
            SurfaceRegistry::get().unregisterSurface(poolId);
        }
        mRegisteredPoolIds.clear();
    }

    sp<IComponent> mBase;
    std::mutex mPoolLock;
    std::set<uint64_t> mRegisteredPoolIds;
};

class CustomComponentStore : public utils::ComponentStore {
public:
    CustomComponentStore(const std::shared_ptr<C2ComponentStore>& store)
        : utils::ComponentStore(store) {}
    virtual ~CustomComponentStore() override = default;

    virtual Return<void> createComponent_1_2(
            const hidl_string& name,
            const sp<IComponentListener>& listener,
            const sp<IClientManager>& pool,
            createComponent_1_2_cb _hidl_cb) override {
        return utils::ComponentStore::createComponent_1_2(
                name, listener, pool,
                [_hidl_cb](Status status, const sp<IComponent>& component) {
                    if (status == Status::OK && component != nullptr) {
                        _hidl_cb(status, new ComponentWrapper(component));
                    } else {
                        _hidl_cb(status, component);
                    }
                });
    }

    virtual Return<void> createComponent_1_1(
            const hidl_string& name,
            const sp<IComponentListener>& listener,
            const sp<IClientManager>& pool,
            createComponent_1_1_cb _hidl_cb) override {
        return utils::ComponentStore::createComponent_1_1(
                name, listener, pool,
                [_hidl_cb](Status status, const sp<::android::hardware::media::c2::V1_1::IComponent>& component) {
                    if (status == Status::OK && component != nullptr) {
                        sp<IComponent> comp12 = IComponent::castFrom(component);
                        if (comp12 != nullptr) {
                            _hidl_cb(status, new ComponentWrapper(comp12));
                            return;
                        }
                    }
                    _hidl_cb(status, component);
                });
    }

    virtual Return<void> createComponent(
            const hidl_string& name,
            const sp<IComponentListener>& listener,
            const sp<IClientManager>& pool,
            createComponent_cb _hidl_cb) override {
        return utils::ComponentStore::createComponent(
                name, listener, pool,
                [_hidl_cb](Status status, const sp<::android::hardware::media::c2::V1_0::IComponent>& component) {
                    if (status == Status::OK && component != nullptr) {
                        sp<IComponent> comp12 = IComponent::castFrom(component);
                        if (comp12 != nullptr) {
                            _hidl_cb(status, new ComponentWrapper(comp12));
                            return;
                        }
                    }
                    _hidl_cb(status, component);
                });
    }
};

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-ffmpeg.policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg.policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg-extended.policy";

int main() {
    LOG(DEBUG) << "android.hardware.media.c2@1.2-service-ffmpeg starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    ProcessState::self()->startThreadPool();
    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    sp<IComponentStore> store;

    LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
    store = new CustomComponentStore(
            std::make_shared<C2FFMPEGComponentStore>());

    if (store == nullptr) {
        LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
    } else {
        constexpr char const* serviceName = "ffmpeg";
        if (store->registerAsService(serviceName) != OK) {
            LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                          " with instance name << \""
                       << serviceName << "\".";
        } else {
            LOG(DEBUG) << "Codec2's IComponentStore service registered. "
                          "Instance name: \"" << serviceName << "\".";
        }
    }

    hardware::joinRpcThreadpool();
    return 0;
}
