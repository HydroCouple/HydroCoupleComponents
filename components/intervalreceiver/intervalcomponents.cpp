// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
// Reference components for the OpenSWMM CF2 interval-transfer contract.
#include <hydrocouplecomponentabi.h>
#include <hydrocouplesdk/component/abstractmodelcomponent.h>
#include <hydrocouplesdk/component/abstractmodelcomponentinfo.h>
#include <hydrocouplesdk/core/dimension.h>
#include <hydrocouplesdk/core/unit.h>
#include <hydrocouplesdk/core/valuedefinition.h>
#include <hydrocouplesdk/data/argument1d.h>
#include <hydrocouplesdk/data/exchangeitems1d.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr auto profile = "hydrocouple.interval-transfer/1";
using Status = IModelComponent::ComponentStatus;
using Axis = IUnitDimensions::FundamentalUnitDimension;

class Configuration final : public Argument1DString {
public:
    using Argument1DString::Argument1DString;
    bool initialize(const std::string& text, ArgumentInputType type, std::string& message) override {
        try {
            if (modelComponent()->status()!=Status::Created)
                throw std::invalid_argument("Configure before initialization; create another instance to change the port catalogue");
            if (type==ArgumentInputType::JSON) {
                const auto value=Json::parse(text);
                if (value.is_object() && value.contains("schema")) {
                    (*this)[0]=value.dump(); message.clear(); return true;
                }
            }
            return Argument1DString::initialize(text,type,message);
        } catch (const std::exception& error) {
            message=error.what();
            dynamic_cast<AbstractModelComponent*>(modelComponent())->queueError(ErrorEntry::Severity::Error,1,message);
            return false;
        }
    }
    bool setValuesFrom(const BufferDescriptor& buffer,std::span<const int64_t> start,
        std::span<const int64_t> count,std::string* message=nullptr) override {
        if(modelComponent()->status()!=Status::Created) {
            const std::string error="Configure before initialization";
            if(message)*message=error;
            dynamic_cast<AbstractModelComponent*>(modelComponent())->queueError(ErrorEntry::Severity::Error,1,error);
            return false;
        }
        return Argument1DString::setValuesFrom(buffer,start,count,message);
    }
    bool serialize(ArgumentInputType type,std::string& text,std::string& message) const override {
        if (type==ArgumentInputType::JSON && !(*this)[0].empty()) {
            try { text=Json::parse((*this)[0]).dump(); message.clear(); return true; }
            catch (const std::exception& error) { message=error.what(); return false; }
        }
        return Argument1DString::serialize(type,text,message);
    }
};

// Exact scalar shape and SI/basis checks avoid truncating a provider array or
// connecting a concentration snapshot to an interval amount.
template<class T> class IntervalInput final : public Input1D<T> {
public:
    using Input1D<T>::Input1D;
    bool canConsume(IOutput* provider,std::string& message) const override {
        const auto* expected=dynamic_cast<IQuantity*>(this->valueDefinition());
        const auto* actual=provider?dynamic_cast<IQuantity*>(provider->valueDefinition()):nullptr;
        bool ok=provider && provider->dataKind()==this->dataKind() &&
            provider->shape()==std::vector<int64_t>{1} && provider->id()==this->id() &&
            provider->description()==this->description() && !dynamic_cast<IAdaptedOutput*>(provider);
        if (expected) {
            ok=ok && actual && actual->valueKind()==expected->valueKind() && actual->unit() &&
                actual->unit()->caption()==expected->unit()->caption() &&
                actual->unit()->conversionFactorToSI()==expected->unit()->conversionFactorToSI() &&
                actual->unit()->offsetToSI()==expected->unit()->offsetToSI() && actual->unit()->dimensions();
            for (size_t axis=0;ok && axis<=static_cast<size_t>(Axis::PlaneAngle);++axis)
                ok=actual->unit()->dimensions()->power(static_cast<Axis>(axis))==
                    expected->unit()->dimensions()->power(static_cast<Axis>(axis));
        }
        message=ok?"":"Use a direct scalar interval-transfer port with matching field, species basis and units";
        return ok;
    }
};

class IntervalComponent : public AbstractModelComponent {
public:
    IntervalComponent(std::string_view id,AbstractModelComponentInfo* info)
        : AbstractModelComponent(id,info) {}
    void finish() override {
        if (status()==Status::Finished) return;
        setStatus(Status::Finishing); csv_.close(); setStatus(Status::Finished);
    }
protected:
    Configuration* config_=nullptr;
    Argument1DDouble* scaleArgument_=nullptr;
    Dimension scalar_{"scalar","One interval field"};
    ValueDefinition string_{typeid(std::string)};
    std::vector<std::unique_ptr<Unit>> units_;
    std::vector<std::unique_ptr<Quantity>> quantities_;
    // SDK registries observe these objects; the component owns their lifetime.
    std::vector<std::unique_ptr<IComponentDataItem>> ownedArguments_,ownedInputs_,ownedOutputs_;
    Json configValue_;
    std::vector<std::string> fields_,basis_;
    std::map<std::string,ValueDefinition*> definitions_;
    std::ofstream csv_;
    double scale_=1;

    ValueDefinition* quantity(const std::string& id,const std::string& unit,
                              double l=0,double m=0,double t=0,
                              ValueKind kind=ValueKind::Extensive,double factor=1) {
        auto u=std::make_unique<Unit>(unit,factor,0);
        u->dimensionsInternal()->setPower(Axis::Length,l);
        u->dimensionsInternal()->setPower(Axis::Mass,m);
        u->dimensionsInternal()->setPower(Axis::Time,t);
        auto q=std::make_unique<Quantity>(id,typeid(double),u.get()); q->setValueKind(kind);
        auto* result=q.get(); units_.push_back(std::move(u)); quantities_.push_back(std::move(q)); return result;
    }
    void createArguments() override {
        config_=new Configuration("config",&scalar_,1,&string_,this);
        config_->setIsOptional(false); config_->setDescription("Interval-transfer/1 configuration; species and outputs are discovered after initialize, without running or writing files");
        ownedArguments_.emplace_back(config_); addArgument(config_);
        scaleArgument_=new Argument1DDouble("scale",&scalar_,1,quantity("scale","1",0,0,0,ValueKind::Intensive),this);
        (*scaleArgument_)[0]=1; scaleArgument_->setIsOptional(true); scaleArgument_->setRole(IArgument::ArgumentRole::Parameter);
        ownedArguments_.emplace_back(scaleArgument_); addArgument(scaleArgument_);
    }
    bool initializeArguments(std::string& message) override {
        try {
            if(config_->shape()!=std::vector<int64_t>{1} || scaleArgument_->shape()!=std::vector<int64_t>{1})
                throw std::invalid_argument("config and scale must each contain exactly one value");
            configValue_=Json::parse((*config_)[0]);
            if (configValue_.at("schema")!=profile) throw std::invalid_argument("Use schema hydrocouple.interval-transfer/1");
            scale_=(*scaleArgument_)[0];
            if (scaleArgument_->shape()!=std::vector<int64_t>{1} || !std::isfinite(scale_) || scale_<0)
                throw std::invalid_argument("scale must be one finite nonnegative number; conservation qualification requires 1");
            const double epoch=configValue_.at("epoch_julian_day");
            if (!std::isfinite(epoch)) throw std::invalid_argument("epoch_julian_day must be finite");
            fields_={"epoch_julian_day","interval_start_s","interval_end_s","water_m3","enthalpy_j"};
            basis_={"day","s","s","m3","J"};
            if (!configValue_.at("species").is_array()) throw std::invalid_argument("species must be an array");
            std::set<std::string> ids;
            for (const auto& species:configValue_.at("species")) {
                const std::string id=species.at("id"),basis=species.at("basis");
                if (id.empty() || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
                      return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.';
                    }) || !ids.insert(id).second || (basis!="kg" && basis!="kg-N"))
                    throw std::invalid_argument("Use unique species IDs and a declared kg or kg-N basis in this reference profile");
                fields_.push_back("species."+id); basis_.push_back(basis);
            }
            definitions_.clear();
            for (size_t i=0;i<fields_.size();++i) {
                definitions_[fields_[i]]=i<3?quantity(fields_[i],basis_[i],0,0,1,ValueKind::Intensive,i==0?86400:1):
                    i==3?quantity(fields_[i],"m3",3):i==4?quantity(fields_[i],"J",2,1,-2):quantity(fields_[i],basis_[i],0,1);
            }
            configure(); message.clear(); return true;
        } catch (const std::exception& error) { message=error.what(); return false; }
    }
    virtual void configure()=0;
    void initializeFailureCleanUp() override {
        csv_.close(); clearInputs(); clearOutputs(); ownedInputs_.clear(); ownedOutputs_.clear();
    }
    void describe(IComponentDataItem* item,const std::string& field) {
        item->setDescription(std::string(profile)+";field="+field);
    }
    template<class T> Output1D<T>* output(const std::string& id,ValueDefinition* value,const std::string& field={}) {
        auto* item=new Output1D<T>(id,&scalar_,1,value,this); describe(item,field.empty()?id:field); ownedOutputs_.emplace_back(item); addOutput(item); return item;
    }
    static std::string quote(const std::string& text) {
        std::string result="\"";
        for(char c:text) { if(c=='\"')result+='\"'; result+=c; }
        return result+'\"';
    }
};

class IntervalSource final : public IntervalComponent {
public:
    explicit IntervalSource(AbstractModelComponentInfo* info):IntervalComponent("interval-source",info) {}
    std::vector<std::string> validate() override { setStatus(Status::Validating); setStatus(Status::Valid); return {}; }
    void prepare() override { setStatus(Status::Preparing); index_=0; setStatus(Status::Updated); }
    void update(const std::vector<IOutput*>& ={}) override {
        if (status()!=Status::Updated) { queueError(ErrorEntry::Severity::Error,1,"Source is not ready to update"); return; }
        try {
            setStatus(Status::Updating);
            const auto& values=intervals_.at(index_);
            (*token_)[0]=id()+":"+std::to_string(index_+1);
            for(size_t i=0;i<values.size();++i) (*ports_[i])[0]=values[i];
            ++index_; setStatus(index_==intervals_.size()?Status::Done:Status::Updated);
        } catch(const std::exception& error) { setStatus(Status::Failed,error.what()); }
    }
protected:
    void configure() override {
        if(scale_!=1)throw std::invalid_argument("The controllable source requires scale 1");
        intervals_.clear(); double last=0;
        for(const auto& row:configValue_.at("intervals")) {
            std::vector<double> values{configValue_.at("epoch_julian_day"),row.at("start_s"),row.at("end_s"),row.at("water_m3"),row.at("enthalpy_j")};
            if(!row.at("species").is_object() || row.at("species").size()!=fields_.size()-5)
                throw std::invalid_argument("Supply each configured species exactly once per interval");
            for(size_t i=5;i<fields_.size();++i)values.push_back(row.at("species").at(fields_[i].substr(8)));
            if(values[1]!=last || values[2]<=values[1])throw std::invalid_argument("Source intervals must be contiguous and increasing from zero");
            for(size_t i=0;i<values.size();++i)if(!std::isfinite(values[i]) || (i>=3 && i!=4 && values[i]<0))
                throw std::invalid_argument("Supply finite values and nonnegative water/species amounts");
            if(values[3]==0 && (values[4]!=0 || std::any_of(values.begin()+5,values.end(),[](double v){return v!=0;})))
                throw std::invalid_argument("A dry interval cannot transfer waterborne species or heat");
            last=values[2]; intervals_.push_back(std::move(values));
        }
        if(intervals_.empty())throw std::invalid_argument("Supply at least one source interval");
    }
    void createInputs() override {}
    void createOutputs() override {
        ownedOutputs_.clear(); ports_.clear(); token_=output<std::string>("transfer_id",&string_);
        for(const auto& field:fields_)ports_.push_back(output<double>(field,definitions_.at(field)));
    }
private:
    size_t index_=0;
    Output1DString* token_=nullptr;
    std::vector<Output1DDouble*> ports_;
    std::vector<std::vector<double>> intervals_;
};

class IntervalReceiver final : public IntervalComponent {
public:
    explicit IntervalReceiver(AbstractModelComponentInfo* info):IntervalComponent("interval-receiver",info) {}
    std::vector<std::string> validate() override {
        setStatus(Status::Validating);
        try { validateConnections(); setStatus(Status::Valid); return {}; }
        catch(const std::exception& error) {
            queueError(ErrorEntry::Severity::Error,1,error.what()); setStatus(Status::Invalid,error.what()); return {error.what()};
        }
    }
    void prepare() override {
        try {
            setStatus(Status::Preparing); validateConnections();
            auto path=fs::path(configValue_.at("receipt_file").get<std::string>());
            if(path.is_relative())path=fs::path(referenceDirectory())/path;
            if(path.has_parent_path())fs::create_directories(path.parent_path());
            csv_.open(path,std::ios::trunc); if(!csv_)throw std::runtime_error("Cannot open receiver receipt file: "+path.string());
            csv_<<"instance_id,transfer_id,epoch_julian_day,start_s,end_s,offered_water_m3,accepted_water_m3,rejected_water_m3,scale,accepted_enthalpy_j";
            for(size_t i=5;i<fields_.size();++i)csv_<<','<<fields_[i]<<'_'<<basis_[i];
            csv_<<'\n'<<std::setprecision(17); csv_.flush();
            if(!csv_)throw std::runtime_error("Cannot write receiver receipt header");
            processed_=0; lastEnd_=0; receipts_.clear();
            setStatus(Status::Updated);
        } catch(const std::exception& error) { csv_.close(); setStatus(Status::Failed,error.what()); }
    }
    void update(const std::vector<IOutput*>& ={}) override {
        if(status()!=Status::Updated) { queueError(ErrorEntry::Severity::Error,1,"Receiver is not ready to update"); return; }
        try {
            setStatus(Status::Updating); validateConnections();
            const std::string token=read<std::string>(tokenInput_);
            std::vector<double> values;
            for(auto* input:ports_)values.push_back(read<double>(input));
            if(token!=read<std::string>(tokenInput_))throw std::runtime_error("Transfer changed during retrieval");
            if(token.empty() || token.find_first_of("\r\n")!=std::string::npos)throw std::invalid_argument("Supply a nonempty transfer ID without line breaks");
            if(auto prior=receipts_.find(token);prior!=receipts_.end()) {
                if(prior->second!=values || token!=lastToken_)throw std::invalid_argument("Changed or stale transfer ID");
                setStatus(Status::Updated); return; // identical replay: retain acknowledgment, no second credit
            }
            for(size_t i=0;i<values.size();++i)if(!std::isfinite(values[i]) || (i>=3 && i!=4 && values[i]<0))
                throw std::invalid_argument("Invalid water/species/heat amount");
            if(values[0]!=configValue_.at("epoch_julian_day").get<double>() || values[1]!=lastEnd_ || values[2]<=values[1])
                throw std::invalid_argument("Epoch mismatch or noncontiguous/stale transfer interval");
            if(values[3]==0 && (values[4]!=0 || std::any_of(values.begin()+5,values.end(),[](double v){return v!=0;})))
                throw std::invalid_argument("A dry interval cannot carry waterborne species or heat");
            const bool donorDone=tokenInput_->provider()->modelComponent()->status()==Status::Done;
            if(donorDone!=(processed_+1==expected_))
                throw std::invalid_argument("Donor completion does not match expected_transfers");
            const double fraction=values[3]>0?std::min(1.0,capacity_/values[3]):1.0;
            std::vector<double> accepted(values.size()-3),totals(values.size()-3);
            for(size_t i=3;i<values.size();++i) {
                accepted[i-3]=values[i]*fraction;
                totals[i-3]=(*cumulative_[i-3])[0]+accepted[i-3]*scale_;
                if(!std::isfinite(totals[i-3]))throw std::overflow_error("Receiver cumulative amount overflow");
            }
            csv_<<quote(id())<<','<<quote(token)<<','<<values[0]<<','<<values[1]<<','<<values[2]<<','<<values[3]<<','<<accepted[0]<<','<<values[3]-accepted[0]<<','<<scale_;
            for(size_t i=1;i<accepted.size();++i)csv_<<','<<accepted[i];
            csv_<<'\n'; csv_.flush();
            if(!csv_)throw std::runtime_error("Receipt write failed; stop this run without retry");
            // Publish one complete accepted tuple only after every field and the log passed.
            for(size_t i=0;i<accepted.size();++i) { (*accepted_[i])[0]=accepted[i]; (*cumulative_[i])[0]=totals[i]; }
            (*rejected_)[0]=values[3]-accepted[0]; (*ack_)[0]=token;
            receipts_.emplace(token,values); lastToken_=token; lastEnd_=values[2]; ++processed_;
            setStatus(processed_==expected_?Status::Done:Status::Updated);
        } catch(const std::exception& error) { setStatus(Status::Failed,error.what()); }
    }
protected:
    void configure() override {
        if(!configValue_.at("expected_transfers").is_number_integer() || configValue_.at("expected_transfers").get<int64_t>()<=0)
            throw std::invalid_argument("expected_transfers must be a positive integer");
        expected_=configValue_.at("expected_transfers").get<int64_t>();
        capacity_=configValue_.value("max_water_m3",std::numeric_limits<double>::max());
        if(!std::isfinite(capacity_) || capacity_<0 || configValue_.at("receipt_file").get<std::string>().empty())
            throw std::invalid_argument("Supply a receipt_file and finite nonnegative max_water_m3");
    }
    void createInputs() override {
        ownedInputs_.clear(); ports_.clear();
        tokenInput_=new IntervalInput<std::string>("transfer_id",&scalar_,1,&string_,this); describe(tokenInput_,"transfer_id"); ownedInputs_.emplace_back(tokenInput_); addInput(tokenInput_);
        for(const auto& field:fields_) {
            auto* input=new IntervalInput<double>(field,&scalar_,1,definitions_.at(field),this); describe(input,field); ownedInputs_.emplace_back(input); addInput(input); ports_.push_back(input);
        }
    }
    void createOutputs() override {
        ownedOutputs_.clear(); accepted_.clear(); cumulative_.clear();
        ack_=output<std::string>("accepted_transfer_id",&string_);
        rejected_=output<double>("rejected_water_m3",definitions_.at("water_m3"));
        for(size_t i=3;i<fields_.size();++i) {
            accepted_.push_back(output<double>("accepted."+fields_[i],definitions_.at(fields_[i])));
            cumulative_.push_back(output<double>("cumulative."+fields_[i],definitions_.at(fields_[i])));
        }
    }
private:
    void validateConnections() const {
        IModelComponent* donor=nullptr;
        for(auto* input:inputs()) {
            std::string message; auto* provider=input->provider();
            if(!input->canConsume(provider,message))throw std::invalid_argument(input->id()+": "+message);
            if(!provider->modelComponent())throw std::invalid_argument("Interval provider must have an owning component");
            if(donor && donor!=provider->modelComponent())throw std::invalid_argument("An interval tuple must come from one donor component");
            donor=provider->modelComponent();
            if(donor->status()==Status::Failed || donor->status()==Status::Finished)
                throw std::invalid_argument("The interval donor is unavailable");
        }
        if (!donor || donor==this) throw std::invalid_argument("Connect a separate interval donor");
        for (auto* item:donor->outputs()) {
            if (!item || !item->id().starts_with("species.")) continue;
            const auto own=inputs();
            if (std::none_of(own.begin(),own.end(),[item](IInput* input){return input->id()==item->id();}))
                throw std::invalid_argument("Receiver is missing donor species: "+item->id());
        }
        // This profile is one donor / one physical receiver. Diagnostic reads
        // are separate from consumer connections and never debit the donor.
        for(auto* input:inputs()) {
            auto* provider=input->provider();
            if(provider->consumers().size()!=1 || provider->consumers().front()!=input || !provider->adaptedOutputs().empty())
                throw std::invalid_argument("Register exactly one direct receiver for every interval field");
        }
    }
    template<class T> T read(IInput* input) const {
        T value{}; const int64_t one=1,zero=0; std::string message;
        BufferDescriptor buffer{&value,std::is_same_v<T,double>?DataKind::Float64:DataKind::String,1,&one,nullptr,sizeof(T)};
        if(!input->provider()->getValuesInto(buffer,{&zero,1},{&one,1},&message))
            throw std::runtime_error(input->id()+": "+message);
        return value;
    }
    int64_t expected_=0,processed_=0;
    double capacity_=0,lastEnd_=0;
    std::string lastToken_;
    IntervalInput<std::string>* tokenInput_=nullptr;
    Output1DString* ack_=nullptr;
    Output1DDouble* rejected_=nullptr;
    std::vector<IntervalInput<double>*> ports_;
    std::vector<Output1DDouble*> accepted_,cumulative_;
    std::map<std::string,std::vector<double>> receipts_;
};

class Info final : public AbstractModelComponentInfo {
public:
#ifdef HCC_INTERVAL_SOURCE
    Info():AbstractModelComponentInfo("org.hydrocouple.openswmm.test.interval-source") { setCaption("Controllable interval source"); }
    std::unique_ptr<IModelComponent> createComponentInstance() override { return std::make_unique<IntervalSource>(this); }
#else
    Info():AbstractModelComponentInfo("org.hydrocouple.openswmm.test.sink") { setCaption("Conservative interval receiver"); }
    std::unique_ptr<IModelComponent> createComponentInstance() override { return std::make_unique<IntervalReceiver>(this); }
#endif
};
}
HYDROCOUPLE_DECLARE_COMPONENT(Info)
