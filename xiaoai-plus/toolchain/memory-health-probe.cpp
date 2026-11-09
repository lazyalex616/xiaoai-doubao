// Diagnostic-only replay of P5c3's HTTPS health request; no Ark/upload/voice.
#include "config/config.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <chrono>
#include <iostream>
#include <csignal>
#include <netdb.h>
#include <ixwebsocket/IXUrlParser.h>
int main(int argc,char** argv){
 if(argc!=2)return 2;signal(SIGPIPE,SIG_IGN);
 auto cfg=xiaoai_plus::config::load(argv[1],true);
 if(!cfg.memory.enabled)return 2;
 ix::HttpClient http;ix::SocketTLSOptions tls;tls.caFile=cfg.realtime.preset.ca_file;http.setTLSOptions(tls);
 auto args=http.createRequest();args->connectTimeout=1;args->transferTimeout=1;
 args->followRedirects=false;args->compress=false;args->verbose=false;
 args->extraHeaders={{"Authorization","Bearer "+cfg.memory.device_secret},{"Content-Type","application/json"}};
 auto start=std::chrono::steady_clock::now();auto r=http.get(cfg.memory.server_url+"/health",args);
 std::cout<<"LEGACY_HEALTH ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()
 <<" error_code="<<(r?int(r->errorCode):-1)<<" http_status="<<(r?r->statusCode:0);
 const auto message=r?r->errorMsg:std::string();
 const char* category=message.find("Cancellation requested")!=std::string::npos?"connect_timeout":
  message.find("verify")!=std::string::npos || message.find("certificate")!=std::string::npos?"certificate":
  message.find("resolve")!=std::string::npos?"dns":r && r->errorCode==ix::HttpErrorCode::Ok?"ok":"other";
 std::cout<<" category="<<category<<'\n';
 // Compare both resolver families without logging addresses, hostnames or data.
 std::string protocol,host,path,query;int port=0;bool default_port=false;
 if(!ix::UrlParser::parse(cfg.memory.server_url,protocol,host,path,query,port,default_port))return 2;
 for(int family:{AF_INET,AF_UNSPEC}) {
   addrinfo hints{};hints.ai_family=family;hints.ai_socktype=SOCK_STREAM;addrinfo* result=nullptr;
   auto t=std::chrono::steady_clock::now();int code=getaddrinfo(host.c_str(),"443",&hints,&result);
   std::cout<<"LEGACY_DNS family="<<(family==AF_INET?"ipv4":"dual")<<" ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t).count()<<" code="<<code<<'\n';
   if(result)freeaddrinfo(result);
 }
 return 0;
}
