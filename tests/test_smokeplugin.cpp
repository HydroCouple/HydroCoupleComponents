// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The scaffold's gates: the smoke plugin loads through the stamped ABI
// convention exactly as a host would load it — dlopen, stamp check before
// any C++ symbol, info entry point, a cross-image dynamic_cast to
// IModelComponentInfo (the interface-RTTI visibility guarantee at work),
// and a full run of the created instance.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"

#include <dlfcn.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path smokeLibraryPath()
{
    const std::filesystem::path dir{HCC_COMPONENTS_DIR};
#if defined(__APPLE__)
    return dir / "libhcc_smoke.so";
#else
    return dir / "libhcc_smoke.so";
#endif
}

class SmokePluginTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::path path = smokeLibraryPath();
        if (!std::filesystem::exists(path))
        {
            // MODULE libraries default to .so on Linux and .so on macOS
            // only when told to; accept the platform default too.
            path.replace_extension(".dylib");
        }
        ASSERT_TRUE(std::filesystem::exists(path))
            << "smoke plugin not built at " << path;
        m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(m_handle, nullptr) << dlerror();
    }

    void TearDown() override
    {
        // Instances are destroyed before the library that allocated them
        // (the ownership rule in componentabi.h); each test keeps them
        // scoped, so closing last is safe.
        if (m_handle)
            dlclose(m_handle);
    }

    void *m_handle = nullptr;
};

} // namespace

TEST_F(SmokePluginTest, TheStampMatchesBeforeAnythingElseIsCalled)
{
    auto abi = reinterpret_cast<HydroCoupleComponentAbiFn>(
        dlsym(m_handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
    ASSERT_NE(abi, nullptr) << dlerror();
    EXPECT_STREQ(abi(), HYDROCOUPLE_COMPONENT_ABI_STAMP);
}

TEST_F(SmokePluginTest, TheInfoCrossesTheImageBoundaryAsAModelComponentInfo)
{
    auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
        dlsym(m_handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
    ASSERT_NE(info, nullptr) << dlerror();

    HydroCouple::IComponentInfo *raw = info();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->id(), "hydrocouple.components.smoke");

    // The cast every host performs, across the dlopen boundary — this is
    // the interface-RTTI visibility guarantee under test.
    auto *modelInfo = dynamic_cast<HydroCouple::IModelComponentInfo *>(raw);
    ASSERT_NE(modelInfo, nullptr)
        << "cross-image dynamic_cast failed: interface RTTI is not shared";
}

TEST_F(SmokePluginTest, ACreatedInstanceRunsToFinishedAndProduces)
{
    auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
        dlsym(m_handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
    ASSERT_NE(info, nullptr) << dlerror();
    auto *modelInfo =
        dynamic_cast<HydroCouple::IModelComponentInfo *>(info());
    ASSERT_NE(modelInfo, nullptr);

    using Status = HydroCouple::IModelComponent::ComponentStatus;
    {
        std::unique_ptr<HydroCouple::IModelComponent> component =
            modelInfo->createComponentInstance();
        ASSERT_NE(component, nullptr);

        component->initialize();
        ASSERT_EQ(component->status(), Status::Initialized);
        ASSERT_EQ(component->outputs().size(), 1u);
        EXPECT_EQ(component->outputs()[0]->id(), "produced");

        EXPECT_TRUE(component->validate().empty());
        component->prepare();
        while (component->status() != Status::Done &&
               component->status() != Status::Failed)
            component->update();
        component->finish();
        ASSERT_EQ(component->status(), Status::Finished);

        double value = -1.0;
        const int64_t shape[1] = {1};
        const HydroCouple::BufferDescriptor destination{
            &value, HydroCouple::DataKind::Float64, 1, shape, nullptr,
            HydroCouple::MemorySpace::Host, 0};
        const int64_t zero = 0;
        const int64_t one  = 1;
        std::string message;
        ASSERT_TRUE(component->outputs()[0]->getValuesInto(
            destination, std::span<const int64_t>(&zero, 1),
            std::span<const int64_t>(&one, 1), &message))
            << message;
        EXPECT_DOUBLE_EQ(value, 42.0);
    } // instance destroyed before dlclose
}
