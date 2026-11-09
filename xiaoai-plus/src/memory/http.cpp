#include "memory/service.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <mutex>
#include <cerrno>
#include <algorithm>

namespace xiaoai_plus::memory {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex diagnostic_mu;
// Only fixed labels and integers are emitted; never OpenSSL error strings,
// URLs, headers, response bodies, credentials, or profiles.
void Diagnostic(const char* phase,Clock::time_point start,const char* category,int code=0) {
  std::lock_guard<std::mutex> lock(diagnostic_mu);
  std::cerr<<"MEMORY_HTTPS phase="<<phase<<" ms="
    <<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()
    <<" category="<<category<<" code="<<code<<"\n";
}
struct Connection {
  int fd{-1}; SSL_CTX* ctx{nullptr}; SSL* ssl{nullptr};
  ~Connection(){if(ssl)SSL_free(ssl);if(ctx)SSL_CTX_free(ctx);if(fd>=0)close(fd);}
};
bool Poll(int fd,short events,Clock::time_point deadline) {
  for(;;){int ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    if(ms<=0)return false;pollfd p{fd,events,0};int n=poll(&p,1,ms);
    if(n<0 && errno==EINTR)continue;return n>0 && (p.revents&(events|POLLHUP|POLLERR));}
}
bool Retry(Connection& c,int result,Clock::time_point deadline) {
  int e=SSL_get_error(c.ssl,result);
  return (e==SSL_ERROR_WANT_READ || e==SSL_ERROR_WANT_WRITE) &&
    Poll(c.fd,e==SSL_ERROR_WANT_READ?POLLIN:POLLOUT,deadline);
}
bool Decode(const std::string& wire,int& status,std::string& body) {
  const auto end=wire.find("\r\n\r\n");if(end==std::string::npos || end>16384)return false;
  std::istringstream headers(wire.substr(0,end));std::string version;
  headers>>version>>status;if(version!="HTTP/1.1" && version!="HTTP/1.0")return false;
  if(status<100 || status>599)return false;
  std::string line;bool chunked=false;long long length=-1;
  std::getline(headers,line);
  while(std::getline(headers,line)) {
    for(char& ch:line)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if(line.rfind("transfer-encoding:",0)==0){if(line.find("chunked")==std::string::npos)return false;chunked=true;}
    if(line.rfind("content-length:",0)==0){try{length=std::stoll(line.substr(15));}catch(...){return false;}}
    if(line.rfind("content-encoding:",0)==0 && line.find("identity")==std::string::npos)return false;
  }
  auto payload=wire.substr(end+4);
  if(chunked) {
    size_t pos=0;
    for(;;){auto nl=payload.find("\r\n",pos);if(nl==std::string::npos)return false;
      size_t size=0;try{size=std::stoull(payload.substr(pos,nl-pos),nullptr,16);}catch(...){return false;}
      pos=nl+2;if(size==0)return body.size()<=kCacheLimit;
      if(size>kCacheLimit-body.size() || size>payload.size()-pos || payload.size()-pos-size<2)return false;
      body.append(payload,pos,size);pos+=size;if(payload.substr(pos,2)!="\r\n")return false;pos+=2;}
  }
  if(length>=0 && static_cast<size_t>(length)!=payload.size())return false;
  body=std::move(payload);return body.size()<=kCacheLimit;
}
}
Transport HttpsTransport(const config::Memory& cfg,const std::string& ca_file) {
  return [cfg,ca_file](const std::string& path,const Json& body)->Reply {
    auto started=Clock::now();auto phase=started;
    if(cfg.server_url.rfind("https://",0)!=0 || cfg.device_secret.size()!=64 || ca_file.empty() || ca_file=="NONE" ||
       (path!="/health" && path!="/device/snapshot" && path!="/device/sessions" && path!="/device/ack" && path!="/device/identity")) {
      Diagnostic("config",phase,"invalid");return {};}
    // Origin only; no redirects, userinfo, fragments or header injection.
    std::string authority=cfg.server_url.substr(8),host=authority,port="443";
    if(authority.find_first_of("/@?#\r\n")!=std::string::npos){Diagnostic("config",phase,"invalid_origin");return {};}
    auto colon=authority.find(':');if(colon!=std::string::npos){host=authority.substr(0,colon);port=authority.substr(colon+1);}
    if(host.empty() || port.empty() || port.find_first_not_of("0123456789")!=std::string::npos){Diagnostic("config",phase,"invalid_origin");return {};}
    Connection c;ERR_clear_error();c.ctx=SSL_CTX_new(TLS_client_method());
    if(!c.ctx || SSL_CTX_load_verify_locations(c.ctx,ca_file.c_str(),nullptr)!=1){Diagnostic("certificate",phase,"ca_load",ERR_GET_REASON(ERR_get_error()));return {};}
    SSL_CTX_set_verify(c.ctx,SSL_VERIFY_PEER,nullptr);
    Diagnostic("certificate",phase,"ca_loaded");
    // DNS uses the linked libc resolver. Its timing is reported separately;
    // blocking getaddrinfo cannot stall the voice/command worker.
    phase=Clock::now();addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_INET; // OH2P has IPv4 uplink only; avoid paired A/AAAA resolver delay.
    addrinfo* addresses=nullptr;int dns=getaddrinfo(host.c_str(),port.c_str(),&hints,&addresses);
    Diagnostic("dns",phase,dns?"resolver_error":"ok",dns);
    if(dns)return {};
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> resolved(addresses,freeaddrinfo);
    const auto deadline=started+std::chrono::seconds(path=="/device/identity"?2:15);
    // DNS has a separate measured phase. Give TCP/TLS their own 5s budget,
    // capped by the remaining overall deadline rather than spending it on DNS.
    const auto connect_deadline=std::min(deadline,Clock::now()+std::chrono::seconds(5));
    phase=Clock::now();int tcp_error=0;
    for(auto a=addresses;a;a=a->ai_next){
      c.fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(c.fd<0){tcp_error=errno;continue;}
      if(fcntl(c.fd,F_SETFL,O_NONBLOCK)<0){tcp_error=errno;close(c.fd);c.fd=-1;continue;}
      int result=connect(c.fd,a->ai_addr,a->ai_addrlen);
      if(result==0)break;
      if(errno==EINPROGRESS && Poll(c.fd,POLLOUT,connect_deadline)) {
        socklen_t len=sizeof(tcp_error);if(getsockopt(c.fd,SOL_SOCKET,SO_ERROR,&tcp_error,&len)==0 && tcp_error==0)break;
      }else tcp_error=Clock::now()>=connect_deadline?ETIMEDOUT:errno;
      close(c.fd);c.fd=-1;if(Clock::now()>=connect_deadline)break;
    }
    Diagnostic("tcp",phase,c.fd>=0?"ok":tcp_error==ETIMEDOUT?"timeout":"connect_error",tcp_error);
    if(c.fd<0)return {};
    phase=Clock::now();c.ssl=SSL_new(c.ctx);
    if(!c.ssl || SSL_set_fd(c.ssl,c.fd)!=1 || SSL_set_tlsext_host_name(c.ssl,host.c_str())!=1 || SSL_set1_host(c.ssl,host.c_str())!=1) {
      Diagnostic("tls",phase,"setup_error",ERR_GET_REASON(ERR_get_error()));return {};}
    int result;
    while((result=SSL_connect(c.ssl))!=1)if(!Retry(c,result,connect_deadline)) {
      auto verify=SSL_get_verify_result(c.ssl);
      Diagnostic(verify==X509_V_OK?"tls":"certificate",phase,Clock::now()>=connect_deadline?"timeout":verify==X509_V_OK?"handshake_error":"verify_error",verify==X509_V_OK?SSL_get_error(c.ssl,result):verify);return {};}
    Diagnostic("tls",phase,"ok");Diagnostic("certificate",phase,"verified",SSL_get_verify_result(c.ssl));
    auto data=body.is_null()?std::string():body.dump();
    std::string request=(body.is_null()?"GET ":"POST ")+path+" HTTP/1.1\r\nHost: "+authority+
      "\r\nAuthorization: Bearer "+cfg.device_secret+"\r\nContent-Type: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if(!body.is_null())request+="Content-Length: "+std::to_string(data.size())+"\r\n";
    request+="\r\n"+data;phase=Clock::now();size_t sent=0;
    while(sent<request.size()) {
      int n=SSL_write(c.ssl,request.data()+sent,request.size()-sent);
      if(n>0){sent+=n;continue;}
      if(!Retry(c,n,deadline)){Diagnostic("send",phase,Clock::now()>=deadline?"timeout":"io_error",SSL_get_error(c.ssl,n));return {};}
    }
    Diagnostic("send",phase,"ok");phase=Clock::now();std::string wire;char buffer[4096];
    for(;;){int n=SSL_read(c.ssl,buffer,sizeof(buffer));
      if(n>0){wire.append(buffer,n);if(wire.size()>kCacheLimit+32768){Diagnostic("receive",phase,"size_limit");return {};}continue;}
      int error=SSL_get_error(c.ssl,n);
      // HTTP Connection: close also permits peers closing TCP without close_notify.
      if(error==SSL_ERROR_ZERO_RETURN || (error==SSL_ERROR_SYSCALL && n==0) ||
         (error==SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_error())==SSL_R_UNEXPECTED_EOF_WHILE_READING))break;
      if(!Retry(c,n,deadline)){Diagnostic("receive",phase,Clock::now()>=deadline?"timeout":"io_error",error);return {};}
    }
    Diagnostic("receive",phase,"ok");phase=Clock::now();int status=0;std::string payload;
    if(!Decode(wire,status,payload)){Diagnostic("http",phase,"framing_error");return {};}
    Diagnostic("http",phase,"status",status);phase=Clock::now();
    try{auto parsed=Json::parse(payload);Diagnostic("json",phase,"ok");Diagnostic("total",started,"ok");return {status,std::move(parsed)};}
    catch(...){Diagnostic("json",phase,"parse_error");return {};}
  };
}
} // namespace xiaoai_plus::memory
