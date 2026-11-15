#include "memory/service.hpp"
#include "app/conversation_policy.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

namespace xiaoai_plus::memory {
namespace {
int64_t Now() {return std::time(nullptr);}
void SyncDirectory(const std::string& path) {
  const int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(fd<0)throw std::runtime_error("memory_sync_failed");
  const int result=fsync(fd);close(fd);
  if(result!=0)throw std::runtime_error("memory_sync_failed");
}
void Write(const std::string& path,const Json& data) {
  const auto text=data.dump();
  const auto temp=path+".tmp";
  const int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
  if (fd<0) throw std::runtime_error("memory_write_failed");
  size_t n=0;
  while(n<text.size()) {const auto count=write(fd,text.data()+n,text.size()-n);if(count<=0){close(fd);throw std::runtime_error("memory_write_failed");}n+=count;}
  const bool ok=fsync(fd)==0;close(fd);
  if(!ok || rename(temp.c_str(),path.c_str())!=0) throw std::runtime_error("memory_write_failed");
  SyncDirectory(std::filesystem::path(path).parent_path().string());
}
Json Read(const std::string& path,size_t limit,Json fallback) {
  const int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
  if(fd<0) return fallback;
  struct stat st{};
  if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)!=0 || st.st_size<0 || size_t(st.st_size)>limit){close(fd);throw std::runtime_error("memory_file_invalid");}
  std::string s;char buf[4096];ssize_t n;
  while((n=read(fd,buf,sizeof(buf)))>0) s.append(buf,n);
  close(fd);if(n<0)throw std::runtime_error("memory_read_failed");return Json::parse(s);
}
std::string Id() {
  std::random_device random;std::string out;
  constexpr char hex[]="0123456789abcdef";
  for(int i=0;i<32;++i)out+=hex[random()&15];return out;
}
bool ValidSnapshot(const Json& data) {
  try {
    if(data.dump().size()>kCacheLimit || !data.at("generation").is_number_integer() || data.at("generation").get<int64_t>()<0 ||
       !data.at("expires_at").is_number_integer() || data.at("expires_at").get<int64_t>()>Now()+86460 ||
       !data.at("children").is_array() || data["children"].size()>16)return false;
    for(const auto& c:data["children"]){
      const auto id=c.at("id").get<std::string>();
      if(id.empty() || id.size()>80 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)return false;
      if(!c.at("profile").at("name").is_string() || !c.at("profile").at("aliases").is_array() ||
         !c.at("summaries").is_array() || c["summaries"].size()>3 || !c.at("version").is_number_integer())return false;
    }
    return true;
  }catch(...){return false;}
}
}  // namespace
Service::Service(config::Memory cfg,Transport transport,std::function<void(Action)> action)
 :cfg_(std::move(cfg)),transport_(std::move(transport)),action_(std::move(action)),identity_(cfg_.identity_question.empty()?IdentityQuestion(cfg_.child_names):cfg_.identity_question){}
Service::~Service(){Stop();}
void Service::Start(){if(!cfg_.enabled || running_.exchange(true))return;network_thread_=std::thread([this]{NetworkLoop();});thread_=std::thread([this]{Loop();});}
void Service::Stop(){running_.store(false);cv_.notify_all();network_cv_.notify_all();if(thread_.joinable())thread_.join();if(network_thread_.joinable())network_thread_.join();}
bool Service::Push(Command c) {
  if(!cfg_.enabled || !running_.load())return false;
  std::unique_lock<std::mutex> lock(mu_);
  if(commands_.size() >= ((c.kind==2 || c.kind==9) ? kQueueLimit-4 : kQueueLimit)){overflow_.store(true);return false;}
  commands_.push_back(std::move(c));lock.unlock();cv_.notify_one();return true;
}
bool Service::Prefetch(){return Push({5,0,{}});}
bool Service::Begin(uint64_t e){return Push({0,e,{}});}
bool Service::Resume(uint64_t e){return Push({15,e,{}});}
bool Service::Activate(uint64_t e){return Push({1,e,{}});}
void Service::EmitAction(Action action) {
  if(action.identity_pending && !action.greeting.empty()) {
    if(identity_asked_)action.greeting.clear();
    else identity_asked_=true;
  }
  action.identity_revision=identity_revision_;
  if(action.restart)identity_session_expected_revision_=identity_revision_;
  action_(std::move(action));
}
bool Service::ActivateIdentitySession(const realtime::IdentitySessionOrigin& origin,std::function<void(ResumeResult)> reply) {
  Command c{16,origin.epoch(),{}};c.identity_origin=origin;c.reply=std::move(reply);return c.reply && Push(std::move(c));
}
bool Service::IdentitySessionEvent(const realtime::IdentitySessionOrigin& origin,const realtime::duplex::Event& event) {
  if(event.text.size()>8192)return false;
  Command c{17,origin.epoch(),{}};c.identity_origin=origin;
  c.event.type=event.type;c.event.text=event.text;c.event.event_id=event.event_id;c.event.item_id=event.item_id;
  c.event.question_id=event.question_id;c.event.response_id=event.response_id;c.event.content_index=event.content_index;
  c.event.correlation_proven=false;return Push(std::move(c));
}
bool Service::Event(uint64_t e,const realtime::duplex::Event& event){
  if(event.text.size()>8192){overflow_.store(true);return false;}
  // Do not copy audio into the memory queue.
  realtime::duplex::Event text;
  text.type=event.type;text.text=event.text;text.event_id=event.event_id;text.item_id=event.item_id;
  text.question_id=event.question_id;text.response_id=event.response_id;text.content_index=event.content_index;
  text.correlation_proven=event.correlation_proven;
  return Push({2,e,std::move(text)});
}
bool Service::End(uint64_t e,bool complete){return Push({3,e,{},complete});}
bool Service::Interrupt(uint64_t e){return Push({4,e,{}});}
bool Service::InspectTransportResume(uint64_t e,std::function<void(ResumeResult)> reply) {
  Command c{6,e,{}};c.reply=std::move(reply);return c.reply && Push(std::move(c));
}
bool Service::PrepareTransportResume(const ResumeIdentity& expected,uint64_t transport,uint64_t capture,uint64_t input,
                                    std::function<void(ResumeResult)> reply) {
  Command c{7,expected.epoch,{}};c.expected=expected;c.transport=transport;c.capture=capture;c.input=input;c.reply=std::move(reply);
  return c.reply && Push(std::move(c));
}
bool Service::ActivateTransportResume(const realtime::TransportEnvelope& envelope,std::function<void(ResumeResult)> reply) {
  Command c{8,envelope.epoch(),{}};c.envelope=envelope;c.reply=std::move(reply);return c.reply && Push(std::move(c));
}
bool Service::Event(const realtime::TransportEnvelope& envelope,const realtime::duplex::Event& event) {
  if(event.text.size()>8192)return false;
  Command c{9,envelope.epoch(),{}};c.envelope=envelope;
  c.event.type=event.type;c.event.text=event.text;c.event.event_id=event.event_id;c.event.item_id=event.item_id;
  c.event.question_id=event.question_id;c.event.response_id=event.response_id;c.event.content_index=event.content_index;
  c.event.event_field=event.event_field;c.event.item_field=event.item_field;c.event.question_field=event.question_field;c.event.response_field=event.response_field;
  c.event.correlation_proven=false;return Push(std::move(c));
}
bool Service::RetireTransportResume(const realtime::TransportEnvelope& envelope) {
  Command c{10,envelope.epoch(),{}};c.envelope=envelope;return Push(std::move(c));
}
bool Service::PlaybackDrained(const realtime::TransportEnvelope& envelope) {
  Command c{13,envelope.epoch(),{}};c.envelope=envelope;return Push(std::move(c));
}
bool Service::End(const realtime::TransportEnvelope& envelope,bool complete) {
  Command c{11,envelope.epoch(),{},complete};c.envelope=envelope;return Push(std::move(c));
}
bool Service::Interrupt(const realtime::TransportEnvelope& envelope) {
  Command c{12,envelope.epoch(),{}};c.envelope=envelope;return Push(std::move(c));
}
void Service::RevokeResume(){
  if(resume_live_)resume_live_->store(false);
  resume_live_.reset();resume_envelope_={};resume_granted_=false;
  resume_snapshot_=Json::object();resume_instructions_.clear();
}
bool Service::ActiveResumeValid() const {
  return resume_granted_ && resume_live_ && resume_live_->load() &&
    resume_envelope_.valid() && conversational_ && !transitioning_ &&
    resume_identity_.epoch==epoch_ && resume_identity_.revision==identity_revision_ &&
    resume_identity_.kind==identity_.kind() && resume_identity_.child_id==identity_.child_id() &&
    resume_identity_.expires_at>Now() &&
    resume_snapshot_.value("generation",int64_t{-1})==resume_identity_.generation &&
    resume_snapshot_.value("expires_at",int64_t{0})==resume_identity_.expires_at &&
    snapshot_.value("generation",int64_t{-1})==resume_identity_.generation &&
    snapshot_.value("expires_at",int64_t{0})>Now();
}
void Service::AlignActiveRecorder() {
  // Only between leases: keep a streamed turn on its authorized profile version.
  if(resume_granted_ || transitioning_ || !conversational_ || identity_.kind()!="child")return;
  const auto child=Child(identity_.child_id());if(child.empty())return;
  if(!active_.empty() && (active_.at("child_id")!=identity_.child_id() ||
      active_.at("generation")!=snapshot_.at("generation") ||
      active_.at("profile_version")!=child.at("version")))Seal(false);
  if(active_.empty() && PendingBytes()+kSessionLimit+2048<kSpoolLimit) {
    active_=Json{{"session_id",Id()},{"child_id",identity_.child_id()},{"generation",snapshot_["generation"]},
      {"profile_version",child["version"]},{"started_at",Now()},{"ended_at",Now()},{"complete",false},{"turns",Json::array()}};
    transcript_=Transcript{};recording_=true;PersistActive();
  }
}
ResumeResult Service::ResumeState(uint64_t epoch) const {
  ResumeResult r;
  if(epoch!=epoch_){r.reason="epoch_mismatch";return r;}
  if(!conversational_ || transitioning_ || !classification_.empty()){r.reason="identity_not_active";return r;}
  if(identity_.kind()!="child" && identity_.kind()!="adult" && identity_.kind()!="guest" && identity_.kind()!="unknown"){r.reason="identity_unconfirmed";return r;}
  if(snapshot_.empty() || snapshot_.value("expires_at",int64_t{0})<=Now()){r.reason="snapshot_expired";return r;}
  r.identity={epoch_,identity_revision_,cache_revision_.load(),snapshot_.at("generation").get<int64_t>(),-1,
              snapshot_.at("expires_at").get<int64_t>(),identity_.kind(),identity_.child_id()};
  Json child=Json::object();
  if(identity_.kind()=="child") {
    if(!recording_ || active_.empty()){r.reason="recorder_unavailable";return r;}
    child=Child(identity_.child_id());
    if(child.empty() || active_.at("child_id")!=identity_.child_id() || active_.at("generation")!=snapshot_.at("generation") ||
       active_.at("profile_version")!=child.at("version")){r.reason="profile_mismatch";return r;}
    r.identity.profile_version=child.at("version").get<int64_t>();
    r.instructions=ConversationInstructions();
  }else r.instructions=ConversationInstructions();
  r.approved=true;return r;
}
std::string Service::status()const{std::lock_guard<std::mutex> lock(mu_);return status_;}
void Service::SetStatus(std::string s){std::lock_guard<std::mutex> lock(mu_);status_=std::move(s);}
Json Service::Child(const std::string& id)const{
  if(snapshot_.value("expires_at",int64_t{0})<=Now())return Json::object();
  for(auto child:snapshot_.value("children",Json::array()))if(child["id"]==id) {
    // Immediate offline re-wake: only this child's latest unacknowledged,
    // explicitly linked QA can supplement older summaries. Never pair by order.
    for(auto it=pending_.rbegin();it!=pending_.rend();++it) {
      if(it->at("child_id")!=id || it->at("generation")!=snapshot_["generation"])continue;
      auto saved=Read(cfg_.cache_dir+"/spool/"+it->at("session_id").get<std::string>()+".json",kSessionLimit,Json::object());
      std::deque<std::string> pairs;
      for(const auto& a:saved.value("turns",Json::array())) {
        if(a.at("role")!="assistant" || a.value("interrupted",false) || a.value("incomplete",false) || a.value("truncated",false) || a.value("question_id","").empty())continue;
        for(const auto& u:saved["turns"])if(u.at("role")=="user" && !u.value("incomplete",false) && !u.value("truncated",false) && u.value("item_id","")==a["question_id"]) {
          pairs.push_back(u.at("text").get<std::string>()+" / "+a.at("text").get<std::string>());
          if(pairs.size()>2)pairs.pop_front();
        }
      }
      if(!pairs.empty()) {
        std::string recap="Unsent temporary continuation: ";
        for(const auto& pair:pairs)recap+=pair+" | ";
        // The instructions builder applies the actual UTF-8/byte budget.
        child["temporary"]=std::move(recap);
      }
      break;
    }
    return child;
  }
  return Json::object();
}
void Service::Schedule(int kind, Json body) {
  std::lock_guard<std::mutex> lock(network_mu_);
  network_jobs_.push_back({kind,std::move(body),epoch_,identity_revision_});network_cv_.notify_one();
}
void Service::NetworkLoop() {
#ifdef __linux__
  (void)setpriority(PRIO_PROCESS,static_cast<id_t>(syscall(SYS_gettid)),10);
#endif
  while(running_.load()) {
    NetworkJob job;
    {std::unique_lock<std::mutex> lock(network_mu_);
      network_cv_.wait(lock,[this]{return !running_.load() || !network_jobs_.empty();});
      if(!running_.load())break;
      job=std::move(network_jobs_.front());network_jobs_.pop_front();}
    Reply reply;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool stale=job.kind==3 && ms>=job.body.value("deadline_ms",int64_t{0});
    try {if(!stale)reply=transport_(job.kind==0?"/device/snapshot":job.kind==1?"/device/sessions":job.kind==3?"/device/identity":"/device/ack",job.kind==3?Json{{"text",job.body.at("text")}}:job.body);}catch(...){}
    {std::lock_guard<std::mutex> lock(network_mu_);network_results_.push_back({job.kind,std::move(job.body),std::move(reply),job.epoch,job.identity_revision});}
    cv_.notify_one();
  }
}
bool Service::Sync(){
  if(sync_inflight_)return false;
  sync_inflight_=true;next_sync_=std::chrono::steady_clock::now()+std::chrono::minutes(5);
  Schedule(0);return true;
}
bool Service::ApplySnapshot(Reply reply){
  if(reply.status!=200 || !ValidSnapshot(reply.body))return false;
  if(snapshot_==reply.body)return true; // A genuinely identical fetch is a no-op.
  Json before=snapshot_,after=reply.body;before.erase("expires_at");after.erase("expires_at");
  const bool semantic_change=before!=after;
  const int64_t old=snapshot_.value("generation",int64_t{-1});
  const bool generation_changed=old>=0 && old!=reply.body["generation"].get<int64_t>();
  bool actor_deleted=false;
  if(identity_.kind()=="child") {
    actor_deleted=true;
    for(const auto& child:reply.body["children"])if(child.at("id")==identity_.child_id())actor_deleted=false;
  }
  const bool invalid=generation_changed || actor_deleted || reply.body.at("expires_at").get<int64_t>()<=Now();
  // Prepared facts may change before grant. An Active same-actor lease keeps
  // its immutable facts and ORIGINAL expiry, never the refreshed cache expiry.
  if(invalid || (semantic_change && !resume_granted_))RevokeResume();
  if(old!=reply.body["generation"].get<int64_t>()) {
    Json keep=Json::array();
    for(const auto& session:pending_)if(session.at("generation")==reply.body["generation"])keep.push_back(session);
    pending_=std::move(keep);active_=Json::object();transcript_=Transcript{};recording_=false;
    PersistPending();Write(cfg_.cache_dir+"/active.json",active_);
  }else if(actor_deleted)Seal(false);
  snapshot_=std::move(reply.body);Write(cfg_.cache_dir+"/snapshot.json",snapshot_);
  if(semantic_change)++cache_revision_;
  identity_.UpdateChildren(snapshot_["children"]);
  if(generation_changed) {
    continuation_.clear();ended_at_={};identity_asked_=false;
    if(!conversational_)identity_.Reset(snapshot_["children"]);
  }
  if(!ack_inflight_){ack_inflight_=true;Schedule(2,Json{{"generation",snapshot_["generation"]},{"status",status()}});}
  if((generation_changed || actor_deleted) && conversational_) {
    ++identity_revision_;classification_=Json::object();transitioning_=true;identity_.Reset(snapshot_["children"]);
    EmitAction({epoch_,true,IdentityInstructions(cfg_.child_names),identity_.question(),true});
  }
  return true;
}
void Service::CompleteNetwork(bool storage_ok){
  std::deque<NetworkResult> results;
  {std::lock_guard<std::mutex> lock(network_mu_);results.swap(network_results_);}
  for(auto& result:results) {
    if(result.kind==3) {
      if(!classification_.empty() && result.request.value("seq",uint64_t{0})==classification_["seq"] &&
         std::chrono::steady_clock::now()<classification_deadline_)
        FinishClassification(result.reply.status==200?result.reply.body.value("result",""):"");
      continue;
    }
    if(result.kind==0){sync_inflight_=false;
      if(result.epoch!=epoch_ || result.identity_revision!=identity_revision_) {
        // A fetch begun for the old actor cannot overwrite current facts.
        next_sync_=std::chrono::steady_clock::now();++snapshot_receipts_;continue;
      }
      if(storage_ok)ApplySnapshot(std::move(result.reply));
      else if(result.reply.status==200 && ValidSnapshot(result.reply.body) && !ack_inflight_) {
        ack_inflight_=true;Schedule(2,Json{{"generation",result.reply.body["generation"]},{"status",status()}});
      }++snapshot_receipts_;continue;}
    if(result.kind==2){ack_inflight_=false;
      if(storage_ok && result.request.value("generation",int64_t{-1})!=snapshot_.value("generation",int64_t{-1})) {
        ack_inflight_=true;Schedule(2,Json{{"generation",snapshot_["generation"]},{"status",status()}});
      }continue;}
    upload_inflight_=false;
    const auto& reply=result.reply;
    // A deletion may already have removed this in-flight session locally.
    auto it=std::find_if(pending_.begin(),pending_.end(),[&](const Json& p){return p["session_id"]==result.request["session_id"];});
    if(it==pending_.end())continue;
    const auto now=std::chrono::steady_clock::now();
    if((reply.status==200 && reply.body.value("ack","")==it->value("session_id","")) || reply.status==410) {
      pending_.erase(it);PersistPending();backoff_=5;next_retry_=now;
      // Summary refresh is background work, never performed by Begin.
      next_sync_=now;
    }else if(reply.status==409 && reply.body.value("error","")=="generation_changed") {
      next_sync_=now;Sync();next_retry_=now+std::chrono::seconds(5);
    }else{SetStatus("unsaved");next_retry_=now+std::chrono::seconds(backoff_+int(std::random_device{}()%3));backoff_=std::min(300,backoff_*2);}
  }
}
size_t Service::PendingBytes()const {
  size_t total=0;for(const auto& p:pending_)total+=p.at("bytes").get<size_t>();return total;
}
void Service::AddPending(const Json& session) {
  const auto id=session.at("session_id").get<std::string>();
  const auto bytes=session.dump().size();
  if(id.size()!=32 || id.find_first_not_of("0123456789abcdef")!=std::string::npos || bytes>kSessionLimit || PendingBytes()+bytes>kSpoolLimit)throw std::runtime_error("memory_spool_full");
  Write(cfg_.cache_dir+"/spool/"+id+".json",session);
  // Keep metadata only in RAM; read one bounded session when uploading.
  pending_.push_back(Json{{"session_id",id},{"generation",session.at("generation")},
    {"child_id",session.at("child_id")},{"started_at",session.at("started_at")},{"bytes",bytes}});
}
void Service::PersistPending(){
  std::unordered_set<std::string> names;
  for(const auto& session:pending_) {
    const auto id=session.at("session_id").get<std::string>();
    if(id.size()!=32 || id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("memory_spool_invalid");
    const auto name=id+".json";names.insert(name);
    const auto path=cfg_.cache_dir+"/spool/"+name;
    if(!std::filesystem::exists(path))throw std::runtime_error("memory_spool_missing");
  }
  for(const auto& entry:std::filesystem::directory_iterator(cfg_.cache_dir+"/spool")) {
    const auto name=entry.path().filename().string();
    // Only exact files created by this component may be removed.
    if(name.size()==37 && name.substr(32)==".json" && name.substr(0,32).find_first_not_of("0123456789abcdef")==std::string::npos && !names.count(name))
      if(unlink(entry.path().c_str())!=0)throw std::runtime_error("memory_spool_remove_failed");
  }
  SyncDirectory(cfg_.cache_dir+"/spool");
}
void Service::PersistActive(){
  if(active_.empty())return;
  active_["turns"]=transcript_.turns();active_["complete"]=false;active_["ended_at"]=Now();
  Write(cfg_.cache_dir+"/active.json",active_);
}
void Service::Seal(bool complete){
  if(active_.empty()){recording_=false;return;}
  active_["turns"]=transcript_.turns();active_["complete"]=complete && transcript_.complete();active_["ended_at"]=Now();
  bool has_child_text=false;
  for(const auto& turn:active_["turns"]) if(turn["role"]=="user")has_child_text=true;
  if(has_child_text) {
    // Capacity was reserved before opening the recorder; keep all unacknowledged sessions.
    if(PendingBytes()+active_.dump().size()<=kSpoolLimit) {
      AddPending(active_);
    }else{SetStatus("unsaved");PersistActive();recording_=false;return;}
  }
  active_=Json::object();Write(cfg_.cache_dir+"/active.json",active_);recording_=false;
}
void Service::Flush(){
  if(sync_inflight_ || conversational_ || upload_inflight_ || std::chrono::steady_clock::now()<next_retry_ || pending_.empty())return;
  auto data=Read(cfg_.cache_dir+"/spool/"+pending_[0].at("session_id").get<std::string>()+".json",kSessionLimit,Json::object());
  upload_inflight_=true;Schedule(1,std::move(data));
}
void Service::ResolveIdentity(const std::string& text,Candidate candidate) {
  const bool leaving_child=identity_.kind()=="child";
  const auto d=identity_.Hear(text,candidate);
  if(d.changed || !d.greeting.empty()) {
    // Only Hear's one explicit retry reopens the existing question guard.
    if(!d.changed && !d.greeting.empty())identity_asked_=false;
    ++identity_revision_;
    // Revoke old child output and context before any network/session wait.
    RevokeResume();
    if(!active_.empty())transcript_.Interrupted();
    Seal(false);continuation_.clear();transcript_=Transcript{};transitioning_=true;
    EmitAction({epoch_,true,ConversationInstructions(),d.greeting,
                d.kind=="unknown",false,0,!leaving_child});
  }
}
std::string Service::ConversationInstructions(bool short_resume) const {
  if(identity_.kind()=="unknown")return IdentityInstructions(cfg_.child_names);
  const auto child=identity_.kind()=="child" ? Child(identity_.child_id()) : Json::object();
  Json safe=Json::object();
  if(!child.empty()) {
    safe["profile"]=Json::object();
    for(const char* f:{"name","grade","level","interests","goals"})
      if(child.at("profile").contains(f))safe["profile"][f]=child.at("profile")[f];
    if(child.contains("summaries"))safe["summaries"]=child["summaries"];
    if(child.contains("temporary"))safe["temporary"]=child["temporary"];
  }
  std::string identity="Speaker already identified";
  if(!child.empty())identity+=" as "+child.at("profile").at("name").get<std::string>();
  identity+="; never re-ask unless corrected. Continue without greeting/restart.\n";
  std::string recent=continuation_;
  // Preserve ordered messages as untrusted context, without inventing QA links.
  const auto& turns=transcript_.turns();
  for(size_t i=turns.size()>8?turns.size()-8:0;i<turns.size();++i) {
    const auto& t=turns[i];
    auto text=Redact(t.value("text",""));size_t cap=std::min<size_t>(480,text.size());
    while(cap<text.size() && cap && (static_cast<unsigned char>(text[cap])&0xc0)==0x80)--cap;
    recent+="Untrusted recent "+t.value("role","")+(t.value("interrupted",false)?" (possibly interrupted)":"")+": "+text.substr(0,cap)+"\n";
  }
  return BuildInstructions(safe,identity,recent,short_resume,cfg_.child_names);
}
void Service::FinishClassification(const std::string& result) {
  auto request=std::move(classification_);classification_=Json::object();
  if(request.empty() || !conversational_ || transitioning_ || request["epoch"]!=epoch_)return;
  if(result=="exit") {
    RevokeResume();Seal(true);conversational_=false;EmitAction({epoch_,false,"","",false,true});return;
  }
  ResolveIdentity(request.at("text"),Candidate{result,1.0});
}
void Service::Process(const Command& c){
  if(c.kind==15) {
    if(c.epoch!=epoch_ || !conversational_ || transitioning_)return;
    RevokeResume();classification_=Json::object();legacy_identity_origin_={};transport_mode_=false;transitioning_=true;
    EmitAction({epoch_,true,ConversationInstructions(),"",identity_.kind()=="unknown"});return;
  }
  if(c.kind==16) {
    ResumeResult r;
    if(!c.identity_origin.valid() || c.epoch!=epoch_ || c.identity_origin.revision()!=identity_revision_ ||
       c.identity_origin.session()<=last_legacy_session_ || resume_envelope_.valid() || !conversational_ || identity_session_expected_revision_!=c.identity_origin.revision())
      r.reason="identity_origin_stale_or_not_retired";
    else {
      Command activation{1,c.epoch,{}};Process(activation);
      legacy_identity_origin_=c.identity_origin;last_legacy_session_=c.identity_origin.session();
      r.approved=true;r.identity.epoch=epoch_;r.identity.revision=identity_revision_;
      r.identity.kind=identity_.kind();r.identity.child_id=identity_.child_id();
    }
    if(c.reply)c.reply(std::move(r));return;
  }
  if(c.kind==17) {
    if(!c.identity_origin.valid() || !(c.identity_origin==legacy_identity_origin_) ||
       c.identity_origin.revision()!=identity_revision_ || c.epoch!=epoch_ || resume_envelope_.valid())return;
    Command forward=c;forward.kind=2;Process(forward);return;
  }
  if(c.kind==11 || c.kind==12) {
    if(!c.envelope.valid() || c.envelope!=resume_envelope_ || c.epoch!=epoch_)return;
    Command forward=c;forward.kind=c.kind==11?3:4;Process(forward);return;
  }
  if(c.kind==13) {
    if(c.envelope==resume_envelope_ && ActiveResumeValid()) {
      PersistActive();RevokeResume();AlignActiveRecorder();
    }return;
  }
  if(c.kind>=6 && c.kind<=8) {
    if(c.kind==6 || c.kind==7)AlignActiveRecorder();
    auto r=ResumeState(c.epoch);
    if(c.kind==7 && r.approved) {
      if(!(r.identity==c.expected)){r.approved=false;r.reason="identity_snapshot_mismatch";}
      else if(resume_envelope_.valid() || !c.transport || !c.capture || !c.input || c.transport<=last_transport_ || c.input<=last_input_) {
        r.approved=false;r.reason="lease_or_generation_invalid";
      }else {
        transcript_.Interrupted();PersistActive();classification_=Json::object();
        legacy_identity_origin_={};transport_mode_=true;resume_granted_=false;
        if(r.identity.kind=="unknown") {++identity_revision_;r.identity.revision=identity_revision_;}
        resume_identity_=r.identity;resume_snapshot_=snapshot_;resume_instructions_=r.instructions;
        if(r.identity.kind=="unknown")resume_snapshot_["children"]=Json::array();
        resume_live_=std::make_shared<std::atomic<bool>>(true);
        resume_envelope_=realtime::TransportEnvelope(epoch_,identity_revision_,r.identity.generation,r.identity.profile_version,
                                                   ++resume_sequence_,c.transport,c.capture,c.input);
        last_transport_=c.transport;last_input_=c.input;r.envelope=resume_envelope_;
      }
    }
    if(c.kind==8 && r.approved) {
      auto current=r.identity;current.expires_at=resume_identity_.expires_at;
      if(!c.envelope.valid() || c.envelope!=resume_envelope_ || resume_granted_ ||
          !resume_live_ || !resume_live_->load() || !(current==resume_identity_) || resume_identity_.expires_at<=Now()) {
        r.approved=false;r.reason="stale_or_duplicate_activation";
      }else {
        resume_granted_=true;r.identity=resume_identity_;r.instructions=resume_instructions_;
        r.envelope=c.envelope;r.permit=realtime::ResumePermit(c.envelope,resume_live_,resume_identity_.expires_at);
      }
    }
    if(c.reply)c.reply(std::move(r));return;
  }
  if(c.kind==10){if(c.envelope==resume_envelope_){transcript_.Interrupted();PersistActive();RevokeResume();}return;}
  if(c.kind==9) {
    if(!c.envelope.valid() || c.envelope!=resume_envelope_ || !ActiveResumeValid())return;
    const bool asr=c.event.type=="conversation.item.input_audio_transcription.completed";
    const bool asr_failed=c.event.type=="conversation.item.input_audio_transcription.failed";
    const bool text=c.event.type=="response.output_text.done";
    if(((asr || asr_failed) && (!c.event.item_field.valid() || !c.event.event_field.valid())) ||
       (text && (!c.event.response_field.valid() || !c.event.question_field.valid() || !c.event.event_field.valid())))return;
    if(!asr && !text && !asr_failed)return;
    if(asr_failed && resume_identity_.kind=="unknown") {ResolveIdentity("");return;}
    Command forward=c;forward.kind=2;Process(forward);return;
  }
  if(c.kind==5){next_sync_=std::chrono::steady_clock::now();Sync();return;}
  if(c.kind==0) {
    if(transport_mode_ && c.epoch==epoch_)return; // Late same-epoch Begin must not reset identity.
    ++identity_revision_;RevokeResume();transport_mode_=false;
    const bool rewake=!conversational_;
    const bool retain=identity_.kind()=="child" && (conversational_ ||
      (ended_at_.time_since_epoch().count() && std::chrono::steady_clock::now()-ended_at_<std::chrono::minutes(10))) &&
      snapshot_.value("expires_at",int64_t{0})>Now() && !Child(identity_.child_id()).empty();
    if(retain) {
      auto text=ConversationInstructions();const auto pos=text.find("Untrusted recent ");
      continuation_=pos==std::string::npos?"":text.substr(pos);
      // Keep the most recent context bounded across repeated short re-wakes.
      while(continuation_.size()>4096)continuation_.erase(0,continuation_.find('\n')+1);
    }else continuation_.clear();
    if(!retain && !conversational_)identity_asked_=false;
    classification_=Json::object();Seal(false);transcript_=Transcript{};epoch_=c.epoch;identity_events_.clear();recording_=false;transitioning_=false;conversational_=true;
    if(!retain)identity_.Reset(snapshot_.value("expires_at",int64_t{0})>Now()?snapshot_.value("children",SeedNames(cfg_.child_names)):SeedNames(cfg_.child_names));
    std::string greeting=retain ? "" : identity_.question();
    if(retain && rewake && identity_.kind()=="child")
      greeting="Hi "+Child(identity_.child_id()).at("profile").at("name").get<std::string>()+", want to continue?";
    auto instructions=ConversationInstructions(retain);
    EmitAction({epoch_,true,instructions,greeting,!retain});return;
  }
  if(c.epoch!=epoch_)return;
  // Fresh owners use the envelope overloads. Old completion/interrupt tasks
  // cannot seize the same epoch after a transport replacement.
  if(transport_mode_ && !c.envelope.valid() &&
     (c.kind==3 || c.kind==4 || (c.kind==1 && resume_envelope_.valid())))return;
  if(c.kind==3){RevokeResume();classification_=Json::object();Seal(c.complete);conversational_=false;ended_at_=std::chrono::steady_clock::now();return;}
  if(c.kind==4){transcript_.Interrupted();PersistActive();return;}
  if(c.kind==1) {
    RevokeResume();
    identity_session_expected_revision_=0;
    transitioning_=false;
    if(identity_.kind()=="child") {
      const auto child=Child(identity_.child_id());
      if(!child.empty() && PendingBytes()+kSessionLimit+2048<kSpoolLimit && active_.empty()) {
        active_=Json{{"session_id",Id()},{"child_id",identity_.child_id()},{"generation",snapshot_["generation"]},
          {"profile_version",child["version"]},{"started_at",Now()},{"ended_at",Now()},{"complete",false},{"turns",Json::array()}};
        transcript_=Transcript{};recording_=true;PersistActive();
      }else SetStatus(child.empty()?"cache_expired":"unsaved");
    }
    return;
  }
  if(c.kind!=2 || transitioning_ || (transport_mode_ && !c.envelope.valid() && !(c.identity_origin.valid() && c.identity_origin==legacy_identity_origin_)) || !conversational_)return;
  if(c.event.type=="conversation.item.input_audio_transcription.completed") {
    if(identity_.kind()=="unknown" && identity_.IsQuestionTail(c.event.text))return;
    if(!c.event.event_id.empty()) {
      if(identity_events_.count(c.event.event_id)) return;
      if(identity_events_.size()<512) identity_events_.insert(c.event.event_id);
    }
    if(app::IsExitCommand(c.event.text)) {
      RevokeResume();classification_=Json::object();Seal(true);conversational_=false;ended_at_=std::chrono::steady_clock::now();
      EmitAction({epoch_,false,"","",false,true});return;
    }
    if(identity_.kind()=="unknown" && identity_.Rule(c.event.text).empty() &&
       c.event.text.size()<=480 && !c.event.text.empty()) {
      if(!classification_.empty())return;
      classification_=Json{{"text",c.event.text},{"epoch",epoch_},{"seq",++classification_seq_}};
      classification_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      classification_["deadline_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(classification_deadline_.time_since_epoch()).count();
      Schedule(3,classification_);return;
    }
    const auto previous=identity_.kind();
    // This synchronous identity decision supersedes an older async attempt.
    // Keep classification_seq_ monotonic so its late result cannot consume a
    // subsequent pending classification after identity is reset.
    classification_=Json::object();
    ResolveIdentity(c.event.text);
    if(transitioning_ || previous=="unknown")return;
  }
  if(recording_) {
    if(transcript_.Add(c.event))PersistActive();
    else if(!transcript_.complete())SetStatus("unsaved");
  }
}
void Service::Loop(){
#ifdef __linux__
  // Linux nice is per-thread: lower only this worker, never capture/KWS threads.
  (void)setpriority(PRIO_PROCESS,static_cast<id_t>(syscall(SYS_gettid)),10);
#endif
  bool storage_ok=true;
  try {
    std::filesystem::create_directories(cfg_.cache_dir);
    struct stat st{};
    if(lstat(cfg_.cache_dir.c_str(),&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0022)!=0)throw std::runtime_error("memory_directory_invalid");
    chmod(cfg_.cache_dir.c_str(),0700);
    snapshot_=Read(cfg_.cache_dir+"/snapshot.json",kCacheLimit,Json::object());
    if(!snapshot_.empty() && (!ValidSnapshot(snapshot_) || snapshot_.value("expires_at",int64_t{0})<=Now())){snapshot_=Json::object();Write(cfg_.cache_dir+"/snapshot.json",snapshot_);}
    std::filesystem::create_directory(cfg_.cache_dir+"/spool");
    if(lstat((cfg_.cache_dir+"/spool").c_str(),&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0022)!=0)throw std::runtime_error("memory_directory_invalid");
    chmod((cfg_.cache_dir+"/spool").c_str(),0700);
    for(const auto& entry:std::filesystem::directory_iterator(cfg_.cache_dir+"/spool")) {
      auto path=entry.path();
      const auto name=path.filename().string();
      if(path.extension()==".tmp") {
        if(name.size()!=41 || name.substr(32)!=".json.tmp" || name.substr(0,32).find_first_not_of("0123456789abcdef")!=std::string::npos)
          throw std::runtime_error("memory_spool_invalid");
        const auto final=path.parent_path()/(name.substr(0,37));
        bool valid=false;
        try {auto tmp=Read(path.string(),kSessionLimit,Json::object());valid=tmp.value("session_id","")==name.substr(0,32);}catch(...){}
        if(valid && !std::filesystem::exists(final)) {
          if(rename(path.c_str(),final.c_str())!=0)throw std::runtime_error("memory_spool_recovery_failed");
          SyncDirectory(cfg_.cache_dir+"/spool");
        }else {
          // A partial temp is discardable only with a proven durable counterpart.
          auto backup=Read(std::filesystem::exists(final)?final.string():cfg_.cache_dir+"/active.json",kSessionLimit,Json::object());
          if(backup.value("session_id","")!=name.substr(0,32))throw std::runtime_error("memory_spool_recovery_failed");
          if(unlink(path.c_str())!=0)throw std::runtime_error("memory_spool_recovery_failed");
          SyncDirectory(cfg_.cache_dir+"/spool");
          if(!std::filesystem::exists(final))continue; // active recovery below seals it.
        }
        path=final;
      }
      auto session=Read(path.string(),kSessionLimit,Json::object());
      if(session.empty() || session.value("session_id","")+".json"!=path.filename())throw std::runtime_error("memory_spool_invalid");
      bool duplicate=false;for(const auto& prior:pending_)if(prior["session_id"]==session["session_id"])duplicate=true;
      if(duplicate)continue;
      pending_.push_back(Json{{"session_id",session.at("session_id")},{"generation",session.at("generation")},
        {"child_id",session.at("child_id")},{"started_at",session.at("started_at")},{"bytes",std::filesystem::file_size(path)}});
      if(PendingBytes()>kSpoolLimit)throw std::runtime_error("memory_spool_full");
    }
    std::sort(pending_.begin(),pending_.end(),[](const Json& a,const Json& b){return a.at("started_at")<b.at("started_at");});
    active_=Read(cfg_.cache_dir+"/active.json",kSessionLimit,Json::object());
    if(!active_.empty()) {
      // Crash recovery keeps final text with partial completeness, and de-duplicates
      // the case where spool fsync succeeded immediately before clearing active.
      bool exists=false;for(const auto& p:pending_)if(p["session_id"]==active_["session_id"])exists=true;
      if(!exists && PendingBytes()+active_.dump().size()<=kSpoolLimit){AddPending(active_);}
      else if(!exists)throw std::runtime_error("memory_spool_full");
      active_=Json::object();Write(cfg_.cache_dir+"/active.json",active_);
    }
  }catch(...){SetStatus("storage_error");storage_ok=false;}
  Sync(); // immediate startup prefetch on the independent I/O worker
  while(true) {
    Command command;bool have=false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock,classification_.empty()?std::chrono::milliseconds(1000):std::chrono::milliseconds(20),[this]{return !running_.load() || !commands_.empty();});
      if(!running_.load() && commands_.empty()) break;
      if(!commands_.empty()){command=std::move(commands_.front());commands_.pop_front();have=true;}
    }
    try {
      if(overflow_.exchange(false)){RevokeResume();transcript_.Incomplete();recording_=false;SetStatus("queue_full");}
      if(have) {
        if(storage_ok)Process(command);
        else if(command.reply){ResumeResult r;r.reason="storage_unavailable";command.reply(std::move(r));}
        else if(command.kind==0){epoch_=command.epoch;identity_.Reset(SeedNames(cfg_.child_names));EmitAction({epoch_,true,IdentityInstructions(cfg_.child_names),identity_.question(),true});}
        else if(command.kind==2 && command.epoch==epoch_ && command.event.type=="conversation.item.input_audio_transcription.completed") {
          const auto d=identity_.Hear(command.event.text);if(!d.changed && !d.greeting.empty())identity_asked_=false;if(d.changed || !d.greeting.empty())EmitAction({epoch_,d.changed,BuildInstructions(Json::object(),"","",false,cfg_.child_names),d.greeting,d.kind=="unknown"});
        }
      }
      CompleteNetwork(storage_ok);
      if(!classification_.empty() && std::chrono::steady_clock::now()>=classification_deadline_)FinishClassification("");
      if(!storage_ok && std::chrono::steady_clock::now()>=next_sync_)Sync();
      if(storage_ok) {
        if(resume_envelope_.valid() && resume_identity_.expires_at<=Now()) {
          RevokeResume();transcript_.Interrupted();PersistActive();
          if(conversational_) {
            transitioning_=true;
            const auto child=identity_.kind()=="child" ? Child(identity_.child_id()) : Json::object();
            EmitAction({epoch_,true,identity_.kind()=="unknown"?IdentityInstructions(cfg_.child_names):BuildInstructions(child,"","",false,cfg_.child_names),
                     "Please say that again.",identity_.kind()=="unknown"});
          }
        }
        if(snapshot_.value("expires_at",int64_t{0})<=Now() && !snapshot_.empty()) {
          ++identity_revision_;RevokeResume();Seal(false);snapshot_=Json::object();Write(cfg_.cache_dir+"/snapshot.json",snapshot_);SetStatus("cache_expired");
          // Abandon the old cloud context too; expired/deleted memory must not remain
          // in a long-running child's session.
          if(conversational_) {transitioning_=true;identity_.Reset(SeedNames(cfg_.child_names));EmitAction({epoch_,true,IdentityInstructions(cfg_.child_names),identity_.question(),true});}
        }
        if(std::chrono::steady_clock::now()>=next_sync_)Sync();
        Flush();
      }
    }catch(...){
      ++identity_revision_;RevokeResume();
      SetStatus("storage_error");storage_ok=false;recording_=false;
      if(conversational_) {
        identity_.Reset(SeedNames(cfg_.child_names));transitioning_=true;
        EmitAction({epoch_,true,IdentityInstructions(cfg_.child_names),identity_.question(),true});
      }
    }
  }
  RevokeResume();
  if(storage_ok)try{Seal(false);}catch(...){SetStatus("storage_error");}
}
}  // namespace xiaoai_plus::memory
