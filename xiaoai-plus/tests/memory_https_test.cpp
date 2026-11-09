#ifdef NDEBUG
#error assertions required
#endif
#include "memory/service.hpp"
#include <cassert>
#include <chrono>
#include <csignal>
#include <iostream>
#include <netdb.h>
#include <thread>
#include <cstdlib>
using namespace xiaoai_plus;
#ifdef __linux__
extern "C" int __real_getaddrinfo(const char*,const char*,const addrinfo*,addrinfo**);
extern "C" int __wrap_getaddrinfo(const char* host,const char* service,const addrinfo* hints,addrinfo** result){
 assert(hints && hints->ai_family==AF_INET); // device has no routed IPv6
 if(const auto delay=std::getenv("P5C4_TEST_DNS_DELAY_MS"))std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(delay)));
 return __real_getaddrinfo(host,service,hints,result);
}
#endif
int main(int argc,char** argv){
 assert(argc==4);signal(SIGPIPE,SIG_IGN);
 config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.device_secret=std::string(64,'a');cfg.server_url=argv[1];
 auto start=std::chrono::steady_clock::now();
 auto r=memory::HttpsTransport(cfg,argv[2])("/health",memory::Json());
 std::string mode=argv[3];
 if(mode=="slow" || mode=="slow_dns" || mode=="chunked"){assert(r.status==200);assert(r.body["status"]=="ok");}
 else assert(r.status==0);
 if(mode=="slow_dns")assert(std::chrono::steady_clock::now()-start>std::chrono::seconds(5));
 if(mode=="slow")assert(std::chrono::steady_clock::now()-start>std::chrono::seconds(1));
 std::cout<<"HTTPS_CASE_PASS "<<mode<<'\n';
}
