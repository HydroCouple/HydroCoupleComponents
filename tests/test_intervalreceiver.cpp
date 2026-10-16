// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <hydrocouplecomponentabi.h>
#include <hydrocouplesdk/component/abstractmodelcomponent.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
using namespace HydroCouple;
using Json=nlohmann::json;
using Status=IModelComponent::ComponentStatus;
namespace fs=std::filesystem;
namespace {
class Library {
public:
    explicit Library(const char* path) {
#ifdef _WIN32
        handle_=LoadLibraryA(path);
#else
        handle_=dlopen(path,RTLD_NOW|RTLD_LOCAL);
#endif
    }
    ~Library() {
        if(!handle_)return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }
    void* symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_,name));
#else
        return dlsym(handle_,name);
#endif
    }
    std::unique_ptr<IModelComponent> create() const {
        auto abi=reinterpret_cast<HydroCoupleComponentAbiFn>(symbol(HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
        if(!abi || std::string(abi())!=HYDROCOUPLE_COMPONENT_ABI_STAMP)return {};
        auto factory=reinterpret_cast<HydroCoupleComponentInfoFn>(symbol(HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        auto* info=factory?dynamic_cast<IModelComponentInfo*>(factory()):nullptr;
        return info?info->createComponentInstance():nullptr;
    }
private:
#ifdef _WIN32
    HMODULE handle_=nullptr;
#else
    void* handle_=nullptr;
#endif
};
template<class T> T get(IComponentDataItem* item) {
    T value{}; const int64_t n=1,z=0; std::string error;
    BufferDescriptor buffer{&value,std::is_same_v<T,double>?DataKind::Float64:DataKind::String,1,&n,nullptr,sizeof(T)};
    EXPECT_TRUE(item->getValuesInto(buffer,{&z,1},{&n,1},&error))<<error;
    return value;
}
template<class T> void put(IComponentDataItem* item,T value) {
    const int64_t n=1,z=0; std::string error;
    BufferDescriptor buffer{&value,std::is_same_v<T,double>?DataKind::Float64:DataKind::String,1,&n,nullptr,sizeof(T)};
    ASSERT_TRUE(item->setValuesFrom(buffer,{&z,1},{&n,1},&error))<<error;
}
IOutput* port(IModelComponent& component,const std::string& id) {
    for(auto* p:component.outputs())if(p->id()==id)return p;
    ADD_FAILURE()<<"Missing output: "<<id; return nullptr;
}
Json common() {
    return {{"schema","hydrocouple.interval-transfer/1"},{"epoch_julian_day",2440587.5},
        {"species",Json::array({{{"id","NH4_N"},{"basis","kg-N"}},{{"id","NO3_N"},{"basis","kg-N"}}})}};
}
Json sourceConfig() {
    auto j=common(); j["intervals"]=Json::array({
        {{"start_s",0},{"end_s",60},{"water_m3",2},{"enthalpy_j",100},{"species",{{"NH4_N",.2},{"NO3_N",.1}}}},
        {{"start_s",60},{"end_s",90},{"water_m3",3},{"enthalpy_j",150},{"species",{{"NH4_N",.3},{"NO3_N",.15}}}}});
    return j;
}
class IntervalReceiverTest:public ::testing::Test {
protected:
    Library sourceLibrary{HCC_SOURCE_LIBRARY},receiverLibrary{HCC_RECEIVER_LIBRARY};
    std::unique_ptr<IModelComponent> source,receiver;
    fs::path directory;
    Json config;
    void SetUp() override {
        source=sourceLibrary.create(); receiver=receiverLibrary.create();
        ASSERT_TRUE(source); ASSERT_TRUE(receiver);
        directory=fs::path(HCC_ARTIFACTS_DIR)/"intervalreceiver"/::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::create_directories(directory);
        config=common(); config["expected_transfers"]=2; config["receipt_file"]="receipts.csv";
        receiver->setReferenceDirectory(directory.string());
        auto* sdk=dynamic_cast<SDK::AbstractModelComponent*>(source.get());
        ASSERT_NE(sdk,nullptr); std::string message;
        ASSERT_TRUE(sdk->setInstanceId("controlled-donor",message))<<message;
    }
    void initialize(IModelComponent& component,const Json& j,double scale=1) {
        std::string message;
        for(auto* argument:component.arguments()) {
            if(argument->id()=="config")ASSERT_TRUE(argument->initialize(j.dump(),IArgument::ArgumentInputType::JSON,message))<<message;
            if(argument->id()=="scale")ASSERT_TRUE(argument->initialize(Json{{"values",{scale}}}.dump(),IArgument::ArgumentInputType::JSON,message))<<message;
        }
        component.initialize(); ASSERT_EQ(component.status(),Status::Initialized);
    }
    void wire() {
        for(auto* input:receiver->inputs()) {
            auto* output=port(*source,input->id()); ASSERT_NE(output,nullptr);
            std::string message; ASSERT_TRUE(input->canConsume(output,message))<<input->id()<<": "<<message;
            output->addConsumer(input); ASSERT_EQ(input->provider(),output);
        }
    }
    void ready(double scale=1) {
        initialize(*source,sourceConfig()); initialize(*receiver,config,scale); wire();
        ASSERT_TRUE(source->validate().empty()); ASSERT_TRUE(receiver->validate().empty());
        source->prepare(); receiver->prepare(); ASSERT_EQ(receiver->status(),Status::Updated);
    }
    size_t lines() const {
        std::ifstream input(directory/"receipts.csv"); std::string row; size_t count=0;
        while(std::getline(input,row))++count;
        return count;
    }
    double total(const std::string& field) {return get<double>(port(*receiver,"cumulative."+field));}
};
}
TEST_F(IntervalReceiverTest, ConfiguredMetadataIsDiscoverableWithoutWritingOutputs) {
    fs::remove(directory/"receipts.csv");
    EXPECT_TRUE(receiver->inputs().empty()); initialize(*receiver,config);
    EXPECT_FALSE(fs::exists(directory/"receipts.csv"));
    ASSERT_EQ(receiver->inputs().size(),8u);
    auto* species=port(*receiver,"cumulative.species.NH4_N"); ASSERT_NE(species,nullptr);
    auto* quantity=dynamic_cast<IQuantity*>(species->valueDefinition()); ASSERT_NE(quantity,nullptr);
    EXPECT_EQ(quantity->unit()->caption(),"kg-N"); EXPECT_EQ(quantity->valueKind(),ValueKind::Extensive);
    EXPECT_DOUBLE_EQ(quantity->unit()->dimensions()->power(IUnitDimensions::FundamentalUnitDimension::Mass),1);
    EXPECT_FALSE(receiver->capabilities().contains(Capability::Checkpointing));
    std::string saved,message;
    for(auto* argument:receiver->arguments())if(argument->id()=="config") {
        ASSERT_TRUE(argument->serialize(IArgument::ArgumentInputType::JSON,saved,message)); EXPECT_EQ(Json::parse(saved),config);
    }
}
TEST_F(IntervalReceiverTest, UnequalIntervalsCloseWaterSpeciesAndHeatAndLeaveReceipts) {
    // Species order differs; identity-based connections preserve the chemistry.
    std::reverse(config["species"].begin(),config["species"].end()); ready();
    source->update(); receiver->update(); source->update(); receiver->update();
    ASSERT_EQ(receiver->status(),Status::Done);
    EXPECT_DOUBLE_EQ(total("water_m3"),5); EXPECT_DOUBLE_EQ(total("enthalpy_j"),250);
    EXPECT_DOUBLE_EQ(total("species.NH4_N"),.5); EXPECT_DOUBLE_EQ(total("species.NO3_N"),.25);
    EXPECT_EQ(get<std::string>(port(*receiver,"accepted_transfer_id")),"controlled-donor:2");
    EXPECT_EQ(lines(),3u); receiver->finish(); EXPECT_EQ(receiver->status(),Status::Finished);
}
TEST_F(IntervalReceiverTest, IdenticalReplayDoesNotCreditOrWriteTwice) {
    ready(); source->update(); receiver->update(); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Updated); EXPECT_DOUBLE_EQ(total("water_m3"),2); EXPECT_EQ(lines(),2u);
    source->update(); receiver->update(); EXPECT_EQ(receiver->status(),Status::Done); EXPECT_DOUBLE_EQ(total("water_m3"),5);
}
TEST_F(IntervalReceiverTest, ChangedReplayFailsWithoutPartialCredit) {
    ready(); source->update(); receiver->update();
    put<double>(port(*source,"water_m3"),3); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),2); EXPECT_EQ(lines(),2u);
    EXPECT_FALSE(receiver->errors().empty());
}
TEST_F(IntervalReceiverTest, PartialAcceptanceScalesTheWholeTupleAndRetainsRejection) {
    config["max_water_m3"]=1; ready(); source->update(); receiver->update();
    EXPECT_DOUBLE_EQ(total("water_m3"),1); EXPECT_DOUBLE_EQ(total("species.NH4_N"),.1);
    EXPECT_DOUBLE_EQ(total("enthalpy_j"),50); EXPECT_DOUBLE_EQ(get<double>(port(*receiver,"rejected_water_m3")),1);
    EXPECT_DOUBLE_EQ(get<double>(port(*receiver,"accepted.water_m3"))+get<double>(port(*receiver,"rejected_water_m3")),2);
}
TEST_F(IntervalReceiverTest, ZeroCapacityRefusesWithoutCreditingInventory) {
    config["max_water_m3"]=0; ready(); source->update(); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Updated); EXPECT_DOUBLE_EQ(total("water_m3"),0);
    EXPECT_DOUBLE_EQ(total("species.NH4_N"),0); EXPECT_DOUBLE_EQ(total("enthalpy_j"),0);
    EXPECT_DOUBLE_EQ(get<double>(port(*receiver,"rejected_water_m3")),2);
}
TEST_F(IntervalReceiverTest, NonConservativeScaleIsExplicitAndOnlyForArgumentTesting) {
    ready(2); source->update(); receiver->update();
    EXPECT_DOUBLE_EQ(total("water_m3"),4); EXPECT_DOUBLE_EQ(get<double>(port(*receiver,"accepted.water_m3")),2);
}
TEST_F(IntervalReceiverTest, InvalidFieldFailsAtomically) {
    ready(); source->update(); put<double>(port(*source,"species.NO3_N"),std::numeric_limits<double>::quiet_NaN()); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),0); EXPECT_EQ(lines(),1u);
}
TEST_F(IntervalReceiverTest, ReverseWaterIsRejectedByThisOneWayReferenceProfile) {
    ready(); source->update(); put<double>(port(*source,"water_m3"),-1); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),0);
}
TEST_F(IntervalReceiverTest, WrongEpochOrStaleIntervalFailsBeforeCredit) {
    ready(); source->update(); put<double>(port(*source,"epoch_julian_day"),2440588.5); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),0);
}
TEST_F(IntervalReceiverTest, SpeciesBasisMismatchRefusesTheConnection) {
    initialize(*source,sourceConfig()); config["species"][0]["basis"]="kg"; initialize(*receiver,config);
    for(auto* input:receiver->inputs())if(input->id()=="species.NH4_N") {
        std::string message; EXPECT_FALSE(input->canConsume(port(*source,input->id()),message)); EXPECT_FALSE(message.empty());
    }
}
TEST_F(IntervalReceiverTest, MissingDonorSpeciesIsNotSilentlyDiscarded) {
    initialize(*source,sourceConfig()); config["species"].erase(1); initialize(*receiver,config); wire();
    EXPECT_FALSE(receiver->validate().empty()); EXPECT_EQ(receiver->status(),Status::Invalid);
}
TEST_F(IntervalReceiverTest, MissingArgumentFailsBeforePortsOrFilesExist) {
    fs::remove(directory/"receipts.csv"); receiver->initialize();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_TRUE(receiver->inputs().empty());
    EXPECT_FALSE(fs::exists(directory/"receipts.csv")); EXPECT_FALSE(receiver->errors().empty());
}
TEST_F(IntervalReceiverTest, BufferShapeAndTypeAreValidated) {
    initialize(*source,sourceConfig()); auto* item=port(*source,"water_m3");
    double value=0; const int64_t two=2,zero=0; std::string message;
    BufferDescriptor buffer{&value,DataKind::Float64,1,&two,nullptr,sizeof(double)};
    EXPECT_FALSE(item->getValuesInto(buffer,{&zero,1},{&two,1},&message));
    const int64_t one=1; buffer.shape=&one; buffer.kind=DataKind::Int64;
    EXPECT_FALSE(item->getValuesInto(buffer,{&zero,1},{&one,1},&message));
}
TEST_F(IntervalReceiverTest, EmptyScaleArrayFailsWithoutReadingMissingStorage) {
    std::string message;
    for(auto* argument:receiver->arguments()) {
        if(argument->id()=="config")ASSERT_TRUE(argument->initialize(config.dump(),IArgument::ArgumentInputType::JSON,message));
        if(argument->id()=="scale")ASSERT_TRUE(argument->initialize("[]",IArgument::ArgumentInputType::JSON,message));
    }
    receiver->initialize(); EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_TRUE(receiver->inputs().empty());
}
TEST_F(IntervalReceiverTest, ConfigurationCannotChangeAfterCatalogueInitialization) {
    initialize(*receiver,config);
    for(auto* argument:receiver->arguments())if(argument->id()=="config") {
        std::string message;
        EXPECT_FALSE(argument->initialize(config.dump(),IArgument::ArgumentInputType::JSON,message));
        const int64_t one=1,zero=0; std::string value=config.dump();
        BufferDescriptor buffer{&value,DataKind::String,1,&one,nullptr,sizeof(std::string)};
        EXPECT_FALSE(argument->setValuesFrom(buffer,{&zero,1},{&one,1},&message));
    }
    EXPECT_EQ(receiver->inputs().size(),8u);
}
TEST_F(IntervalReceiverTest, StaleNewIdCannotReplayAnOldInterval) {
    ready(); source->update(); receiver->update();
    put<std::string>(port(*source,"transfer_id"),"another-id"); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),2); EXPECT_EQ(lines(),2u);
}
TEST_F(IntervalReceiverTest, DryTransferHasZeroInventories) {
    auto j=sourceConfig(); j["intervals"][0]["water_m3"]=0; j["intervals"][0]["enthalpy_j"]=0;
    j["intervals"][0]["species"]={{"NH4_N",0},{"NO3_N",0}};
    initialize(*source,j); initialize(*receiver,config); wire();
    ASSERT_TRUE(source->validate().empty()); ASSERT_TRUE(receiver->validate().empty()); source->prepare(); receiver->prepare();
    source->update(); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Updated); EXPECT_DOUBLE_EQ(total("water_m3"),0); EXPECT_DOUBLE_EQ(total("species.NH4_N"),0);
}

TEST_F(IntervalReceiverTest, TransferCountMismatchStopsBeforePrematureCompletion) {
    config["expected_transfers"]=1; ready(); source->update(); receiver->update();
    EXPECT_EQ(receiver->status(),Status::Failed); EXPECT_DOUBLE_EQ(total("water_m3"),0); EXPECT_EQ(lines(),1u);
}
