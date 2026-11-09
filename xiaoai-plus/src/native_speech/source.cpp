#include "native_speech/source.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace xiaoai_plus::native_speech {
namespace {

constexpr size_t kMaxDatagram = 256 * 1024;
constexpr size_t kMaxQueuedEvents = 512;
constexpr size_t kMaxQueuedPcmBytes = 1024 * 1024;
constexpr uint64_t kUpward = 0;
constexpr uint64_t kDownward = 1;
constexpr uint64_t kRegister = 0;
constexpr uint64_t kPrepare = 1;
constexpr uint64_t kCancel = 2;
constexpr uint64_t kTransmitting = 3;
constexpr uint64_t kEnd = 4;
constexpr uint64_t kAsrStream = 2;
constexpr uint64_t kRegisterResponse = 0;
constexpr uint64_t kPrepareResponse = 1;
constexpr uint64_t kStopCapture = 2;
constexpr uint64_t kExpectSpeech = 3;
constexpr uint64_t kDialogFinish = 5;
constexpr uint64_t kEnableVoiceWakeup = 9;

struct Field {
  uint64_t number{0};
  uint64_t wire{0};
  uint64_t value{0};
  std::vector<uint8_t> bytes;
};

bool ReadVarint(const uint8_t*& cursor, const uint8_t* end, uint64_t* value) {
  *value = 0;
  for (int shift = 0; shift <= 63; shift += 7) {
    if (cursor == end) return false;
    const uint8_t byte = *cursor++;
    if (shift == 63 && (byte & 0x7e) != 0) return false;
    *value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

bool Parse(const uint8_t* data, size_t size, std::vector<Field>* fields) {
  fields->clear();
  if (size > kMaxDatagram) return false;
  const auto* cursor = data;
  const auto* end = data + size;
  while (cursor < end) {
    uint64_t key = 0;
    if (!ReadVarint(cursor, end, &key) || (key >> 3) == 0) return false;
    Field field;
    field.number = key >> 3;
    field.wire = key & 7;
    if (field.wire == 0) {
      if (!ReadVarint(cursor, end, &field.value)) return false;
    } else if (field.wire == 2) {
      uint64_t length = 0;
      if (!ReadVarint(cursor, end, &length) ||
          length > static_cast<uint64_t>(end - cursor)) return false;
      field.bytes.assign(cursor, cursor + length);
      cursor += length;
    } else if (field.wire == 1 || field.wire == 5) {
      const size_t length = field.wire == 1 ? 8 : 4;
      if (static_cast<size_t>(end - cursor) < length) return false;
      cursor += length;
    } else {
      return false;
    }
    fields->push_back(std::move(field));
    if (fields->size() > 64) return false;
  }
  return true;
}

std::optional<uint64_t> Number(const std::vector<Field>& fields, uint64_t number) {
  for (const auto& field : fields) {
    if (field.number == number && field.wire == 0) return field.value;
  }
  return std::nullopt;
}

const std::vector<uint8_t>* Bytes(const std::vector<Field>& fields, uint64_t number) {
  for (const auto& field : fields) {
    if (field.number == number && field.wire == 2) return &field.bytes;
  }
  return nullptr;
}

void AppendVarint(uint64_t value, std::vector<uint8_t>* out) {
  while (value >= 128) {
    out->push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<uint8_t>(value));
}

void AppendNumber(uint64_t field, uint64_t value, std::vector<uint8_t>* out) {
  AppendVarint(field << 3, out);
  AppendVarint(value, out);
}

void AppendBytes(uint64_t field, const uint8_t* data, size_t size,
                 std::vector<uint8_t>* out) {
  AppendVarint((field << 3) | 2, out);
  AppendVarint(size, out);
  out->insert(out->end(), data, data + size);
}

void AppendBytes(uint64_t field, const std::vector<uint8_t>& bytes,
                 std::vector<uint8_t>* out) {
  AppendBytes(field, bytes.data(), bytes.size(), out);
}

std::string DialogId() {
  std::random_device random;
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 4; ++i) out << std::setw(8) << random();
  return out.str();
}

std::string ErrnoText(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}

bool SameAddress(const sockaddr_un& a, socklen_t a_len,
                 const sockaddr_un& b, socklen_t b_len) {
  return a_len <= sizeof(sockaddr_un) && a_len == b_len &&
         std::memcmp(&a, &b, a_len) == 0;
}

std::string PeerPath(const sockaddr_un& peer, socklen_t length) {
  if (length <= offsetof(sockaddr_un, sun_path) ||
      length > sizeof(sockaddr_un) || peer.sun_path[0] == '\0') return {};
  const size_t capacity = length - offsetof(sockaddr_un, sun_path);
  const size_t n = strnlen(peer.sun_path, capacity);
  if (n == capacity) return {};
  return std::string(peer.sun_path, n);
}

}  // namespace

Source::Source() : Source(Options{}) {}

Source::Source(Options options) : options_(std::move(options)) {}

Source::~Source() { Stop(); }

bool Source::Start(Callbacks callbacks) {
  std::lock_guard<std::mutex> lock(state_mu_);
  if (running_ || fd_ >= 0) {
    last_error_ = "native speech source already started";
    return false;
  }
  diagnostics_ = {};
  fatal_class_ = FatalClass::kNone;
  fatal_count_ = 0;
  if (options_.socket_path != kSystemSocketPath && !options_.allow_test_path) {
    last_error_ = "nonstandard speech socket requires explicit test mode";
    return false;
  }
  if (options_.socket_path.empty() ||
      options_.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    last_error_ = "invalid speech socket path";
    return false;
  }
  const auto slash = options_.socket_path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    last_error_ = "speech socket needs an absolute private parent directory";
    return false;
  }
  const std::string parent = options_.socket_path.substr(0, slash);
  struct stat parent_stat{};
  if (lstat(parent.c_str(), &parent_stat) != 0 || !S_ISDIR(parent_stat.st_mode) ||
      parent_stat.st_uid != geteuid() || (parent_stat.st_mode & 0022) != 0) {
    last_error_ = "speech socket parent must exist, be owned by this user, and not group/world writable";
    return false;
  }
  struct stat existing{};
  if (lstat(options_.socket_path.c_str(), &existing) == 0 || errno != ENOENT) {
    last_error_ = "speech socket path already exists or cannot be checked";
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, options_.socket_path.c_str(), options_.socket_path.size() + 1);
  const auto address_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                                   options_.socket_path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    last_error_ = ErrnoText("socket");
    return false;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), address_len) != 0) {
    last_error_ = ErrnoText("bind");
    close(fd);
    return false;
  }
  if (chmod(options_.socket_path.c_str(), 0600) != 0) {
    last_error_ = ErrnoText("chmod socket");
    close(fd);
    if (options_.allow_test_path) unlink(options_.socket_path.c_str());
    return false;
  }
  struct stat bound{};
  if (lstat(options_.socket_path.c_str(), &bound) != 0 || !S_ISSOCK(bound.st_mode)) {
    last_error_ = "bound speech socket disappeared";
    close(fd);
    return false;
  }
  timeval send_timeout{};
  send_timeout.tv_sec = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                 sizeof(send_timeout)) != 0) {
    last_error_ = ErrnoText("set native socket send timeout");
    close(fd);
    if (options_.allow_test_path) unlink(options_.socket_path.c_str());
    return false;
  }
  fd_ = fd;
  socket_dev_ = static_cast<uint64_t>(bound.st_dev);
  socket_ino_ = static_cast<uint64_t>(bound.st_ino);
  callbacks_ = std::move(callbacks);
  registered_ = false;
  peer_len_ = 0;
  next_turn_id_ = 0;
  current_turn_id_ = 0;
  stream_active_ = false;
  stop_capture_sent_ = false;
  turn_continued_ = false;
  turn_finished_ = false;
  chat_count_ = 0;
  dialog_count_ = 0;
  dialog_id_.clear();
  last_error_.clear();
  diagnostics_.stage = RegistrationStage::kSocketBound;
  {
    std::lock_guard<std::mutex> queue_lock(queue_mu_);
    queue_.clear();
    queued_pcm_bytes_ = 0;
    dispatch_stop_ = false;
  }
  running_ = true;
  dispatch_thread_ = std::thread([this] { DispatchLoop(); });
  receive_thread_ = std::thread([this] { ReceiveLoop(); });
  return true;
}

void Source::Stop() {
  running_ = false;
  registration_cv_.notify_all();
  // A callback may request shutdown. It cannot join its own dispatch thread;
  // the owning control thread must call Stop() again before destruction.
  if (std::this_thread::get_id() == dispatch_thread_.get_id()) {
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      queue_.clear();
      queued_pcm_bytes_ = 0;
      dispatch_stop_ = true;
    }
    queue_cv_.notify_all();
    return;
  }
  if (receive_thread_.joinable()) receive_thread_.join();
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    dispatch_stop_ = true;
  }
  queue_cv_.notify_all();
  if (dispatch_thread_.joinable()) dispatch_thread_.join();
  std::lock_guard<std::mutex> lock(state_mu_);
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  socket_dev_ = 0;
  socket_ino_ = 0;
  registered_ = false;
  peer_len_ = 0;
  // The production launcher owns the standard socket's mount and removal.
  // Tests own their private paths and can clean them up here.
  if (options_.allow_test_path && options_.socket_path != kSystemSocketPath)
    unlink(options_.socket_path.c_str());
}

bool Source::socket_ready() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return running_ && fd_ >= 0;
}

bool Source::registered() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return running_ && registered_;
}

bool Source::WaitForRegistered(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(state_mu_);
  registration_cv_.wait_for(lock, timeout, [this] { return registered_ || !running_; });
  return running_ && registered_;
}

std::string Source::last_error() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return last_error_;
}

Source::Diagnostics Source::diagnostics() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  Diagnostics snapshot = diagnostics_;
  snapshot.fatal_class = fatal_class_.load();
  snapshot.fatal_count = fatal_count_.load();
  return snapshot;
}

const char* Source::RegistrationStageName(RegistrationStage stage) {
  switch (stage) {
    case RegistrationStage::kNotStarted: return "NOT_STARTED";
    case RegistrationStage::kSocketBound: return "SOCKET_BOUND";
    case RegistrationStage::kPeerQualified: return "PEER_QUALIFIED";
    case RegistrationStage::kRegisterParsed: return "REGISTER_PARSED";
    case RegistrationStage::kCodecAccepted: return "CODEC_ACCEPTED";
    case RegistrationStage::kRegistered: return "REGISTERED";
  }
  return "UNKNOWN";
}

const char* Source::FatalClassName(FatalClass fatal_class) {
  switch (fatal_class) {
    case FatalClass::kNone: return "NONE";
    case FatalClass::kSocket: return "SOCKET";
    case FatalClass::kProtocol: return "PROTOCOL";
    case FatalClass::kCodec: return "CODEC";
    case FatalClass::kSend: return "SEND";
    case FatalClass::kQueue: return "QUEUE";
    case FatalClass::kCallback: return "CALLBACK";
  }
  return "UNKNOWN";
}

bool Source::CheckTurnLocked(uint64_t turn_id) {
  if (!running_ || !registered_ || fd_ < 0 || current_turn_id_ == 0 ||
      current_turn_id_ != turn_id || turn_finished_) {
    last_error_ = "stale or finished native speech turn";
    return false;
  }
  return true;
}

bool Source::StopCapture(uint64_t turn_id) {
  std::lock_guard<std::mutex> lock(state_mu_);
  if (!CheckTurnLocked(turn_id)) return false;
  if (turn_continued_) return false;
  if (stop_capture_sent_) return true;
  if (!SendDownLocked(kStopCapture, dialog_id_)) return false;
  stop_capture_sent_ = true;
  return true;
}

bool Source::ContinueDialog(uint64_t turn_id) {
  std::lock_guard<std::mutex> lock(state_mu_);
  if (!CheckTurnLocked(turn_id)) return false;
  if (turn_continued_) return true;
  std::vector<uint8_t> expect;
  AppendNumber(2, 0, &expect);
  if (!SendDownLocked(kExpectSpeech, dialog_id_, 4, expect)) return false;
  std::vector<uint8_t> finish;
  AppendNumber(1, 0, &finish);
  AppendNumber(2, 1, &finish);
  AppendNumber(3, 1, &finish);
  AppendNumber(4, 0, &finish);
  AppendNumber(5, chat_count_, &finish);
  AppendNumber(6, dialog_count_, &finish);
  if (!SendDownLocked(kDialogFinish, dialog_id_, 6, finish)) return false;
  turn_continued_ = true;
  return true;
}

bool Source::FinishDialog(uint64_t turn_id) {
  std::lock_guard<std::mutex> lock(state_mu_);
  if (!CheckTurnLocked(turn_id)) return false;
  std::vector<uint8_t> finish;
  AppendNumber(1, 0, &finish);
  AppendNumber(2, 0, &finish);
  AppendNumber(3, 0, &finish);
  AppendNumber(4, 1, &finish);
  AppendNumber(5, chat_count_, &finish);
  AppendNumber(6, dialog_count_, &finish);
  if (!SendDownLocked(kDialogFinish, dialog_id_, 6, finish)) return false;
  turn_finished_ = true;
  return true;
}

bool Source::SendDownLocked(uint64_t kind, const std::string& dialog_id,
                            uint64_t body_field, const std::vector<uint8_t>& body) {
  if (fd_ < 0 || peer_len_ == 0) {
    last_error_ = "native peer unavailable";
    return false;
  }
  std::vector<uint8_t> inner;
  AppendNumber(1, kind, &inner);
  if (!dialog_id.empty())
    AppendBytes(2, reinterpret_cast<const uint8_t*>(dialog_id.data()), dialog_id.size(), &inner);
  if (body_field != 0) AppendBytes(body_field, body, &inner);
  std::vector<uint8_t> outer;
  AppendNumber(1, kDownward, &outer);
  AppendBytes(3, inner, &outer);
  const ssize_t sent = sendto(fd_, outer.data(), outer.size(), 0,
                              reinterpret_cast<const sockaddr*>(&peer_), peer_len_);
  if (sent < 0 || static_cast<size_t>(sent) != outer.size()) {
    last_error_ = ErrnoText("sendto native peer");
    return false;
  }
  return true;
}

void Source::Queue(Event event) {
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    if (dispatch_stop_) return;
    if (queue_.size() >= kMaxQueuedEvents ||
        event.pcm.size() > kMaxQueuedPcmBytes - queued_pcm_bytes_) {
      queue_.clear();
      queued_pcm_bytes_ = 0;
      queue_.push_back(Event{Event::Kind::kFatal, 0, Activation::kUnknown,
                             "native speech callback queue overflow", {}, {}});
      overflow = true;
    } else {
      queued_pcm_bytes_ += event.pcm.size();
      queue_.push_back(std::move(event));
    }
  }
  if (overflow) {
    fatal_class_ = FatalClass::kQueue;
    ++fatal_count_;
    running_ = false;
    registration_cv_.notify_all();
  }
  queue_cv_.notify_one();
}

void Source::Fail(const std::string& reason, FatalClass fatal_class) {
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    last_error_ = reason;
  }
  fatal_class_ = fatal_class;
  ++fatal_count_;
  running_ = false;
  registration_cv_.notify_all();
  Queue(Event{Event::Kind::kFatal, 0, Activation::kUnknown, reason, {}, {}});
}

void Source::ReceiveLoop() {
  std::vector<uint8_t> buffer(kMaxDatagram + 1);
  while (running_) {
    struct stat live{};
    const bool path_ok = lstat(options_.socket_path.c_str(), &live) == 0 &&
        S_ISSOCK(live.st_mode) &&
        static_cast<uint64_t>(live.st_dev) == socket_dev_ &&
        static_cast<uint64_t>(live.st_ino) == socket_ino_;
    if (!running_) break;
    if (!path_ok) {
      Fail("native speech socket path disappeared or was replaced", FatalClass::kSocket);
      break;
    }
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int ready = poll(&descriptor, 1, 100);
    if (!running_) break;
    if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
    if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      Fail(ErrnoText("poll native speech socket"), FatalClass::kSocket);
      break;
    }
    if (!(descriptor.revents & POLLIN)) continue;
    sockaddr_un sender{};
    socklen_t sender_len = sizeof(sender);
    const ssize_t size = recvfrom(fd_, buffer.data(), buffer.size(), MSG_TRUNC,
                                  reinterpret_cast<sockaddr*>(&sender), &sender_len);
    if (size < 0) {
      if (errno == EINTR) continue;
      Fail(ErrnoText("recvfrom native speech socket"), FatalClass::kSocket);
      break;
    }
    if (static_cast<size_t>(size) > kMaxDatagram) {
      Fail("native speech datagram exceeds 256 KiB", FatalClass::kProtocol);
      break;
    }
    FatalClass failure_class = FatalClass::kProtocol;
    if (!HandlePacket(buffer.data(), static_cast<size_t>(size), sender,
                      sender_len, &failure_class)) {
      if (running_) Fail(last_error(), failure_class);
      break;
    }
  }
}

bool Source::HandlePacket(const uint8_t* data, size_t size,
                          const sockaddr_un& sender, socklen_t sender_len,
                          FatalClass* failure_class) {
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++diagnostics_.datagrams_seen;
  }
  const std::string path = PeerPath(sender, sender_len);
  if (path.empty()) {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++diagnostics_.peer_path_rejected;
    return true;
  }
  if (!options_.expected_peer_prefix.empty() &&
      path.compare(0, options_.expected_peer_prefix.size(),
                   options_.expected_peer_prefix) != 0) {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++diagnostics_.peer_path_rejected;
    return true;
  }
  struct stat peer_stat{};
  if (lstat(path.c_str(), &peer_stat) != 0 || !S_ISSOCK(peer_stat.st_mode) ||
      peer_stat.st_uid != geteuid()) {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++diagnostics_.peer_owner_rejected;
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    ++diagnostics_.peer_qualified;
    diagnostics_.stage = std::max(diagnostics_.stage, RegistrationStage::kPeerQualified);
  }

  std::vector<Field> outer;
  std::vector<Field> upward;
  if (!Parse(data, size, &outer) || Number(outer, 1) != kUpward ||
      !Bytes(outer, 2) ||
      !Parse(Bytes(outer, 2)->data(), Bytes(outer, 2)->size(), &upward)) {
    Fail("malformed native speech upward packet", FatalClass::kProtocol);
    return false;
  }
  const auto type = Number(upward, 1);
  if (!type) {
    Fail("native speech packet missing type", FatalClass::kProtocol);
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mu_);
  if (!running_) return false;
  if (*type != kRegister && (!registered_ || !SameAddress(sender, sender_len, peer_, peer_len_)))
    return true;
  if (*type == kRegister) {
    if (registered_ && !SameAddress(sender, sender_len, peer_, peer_len_)) return true;
    ++diagnostics_.register_packets_seen;
    std::vector<Field> body;
    const auto* bytes = Bytes(upward, 2);
    if (bytes && !Parse(bytes->data(), bytes->size(), &body)) {
      last_error_ = "malformed native registration";
      return false;
    }
    diagnostics_.stage = std::max(diagnostics_.stage, RegistrationStage::kRegisterParsed);
    const auto* vendor_bytes = Bytes(body, 1);
    const auto* codec_bytes = Bytes(body, 2);
    std::string vendor = vendor_bytes ? std::string(vendor_bytes->begin(), vendor_bytes->end()) : "unknown";
    if (vendor.empty() || vendor.size() > 64 ||
        !std::all_of(vendor.begin(), vendor.end(), [](unsigned char c) {
          return c >= 32 && c <= 126;
        })) vendor = "unknown";
    const bool codec_absent = codec_bytes == nullptr;
    const bool codec_empty = codec_bytes &&
        std::all_of(codec_bytes->begin(), codec_bytes->end(), [](unsigned char c) {
          return std::isspace(c) != 0;
        });
    std::string codec = codec_absent || codec_empty ? "pcm16" :
        std::string(codec_bytes->begin(), codec_bytes->end());
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (codec != "pcm" && codec != "pcm16" && codec != "s16le") {
      ++diagnostics_.codec_explicit_rejected;
      *failure_class = FatalClass::kCodec;
      last_error_ = "native speech registration declared unsupported codec";
      return false;
    }
    if (codec_absent) ++diagnostics_.codec_absent;
    else if (codec_empty) ++diagnostics_.codec_empty;
    else ++diagnostics_.codec_explicit_pcm;
    diagnostics_.stage = std::max(diagnostics_.stage, RegistrationStage::kCodecAccepted);
    peer_ = sender;
    peer_len_ = sender_len;
    registered_ = true;
    current_turn_id_ = 0;
    stream_active_ = false;
    turn_finished_ = false;
    turn_continued_ = false;
    chat_count_ = 0;
    if (!SendDownLocked(kRegisterResponse, {}) ||
        !SendDownLocked(kEnableVoiceWakeup, {})) {
      *failure_class = FatalClass::kSend;
      return false;
    }
    ++diagnostics_.register_ready;
    diagnostics_.stage = RegistrationStage::kRegistered;
    registration_cv_.notify_all();
    Queue(Event{Event::Kind::kReady, 0, Activation::kUnknown, vendor, codec, {}});
    return true;
  }
  if (*type == kPrepare) {
    std::vector<Field> body;
    const auto* bytes = Bytes(upward, 3);
    if (!bytes || !Parse(bytes->data(), bytes->size(), &body)) {
      last_error_ = "malformed native stream prepare";
      return false;
    }
    const uint64_t mode = Number(body, 1).value_or(UINT64_MAX);
    const Activation activation = mode == 0 ? Activation::kWake :
                                  mode == 1 ? Activation::kFollowUp : Activation::kUnknown;
    if (activation == Activation::kWake) {
      ++dialog_count_;
      chat_count_ = 1;
    } else {
      ++chat_count_;
    }
    ++next_turn_id_;
    current_turn_id_ = next_turn_id_;
    stream_active_ = true;
    stop_capture_sent_ = false;
    turn_continued_ = false;
    turn_finished_ = false;
    dialog_id_ = DialogId();
    std::vector<uint8_t> response;
    AppendNumber(1, 1, &response);
    if (!SendDownLocked(kPrepareResponse, dialog_id_, 3, response)) {
      *failure_class = FatalClass::kSend;
      return false;
    }
    Queue(Event{Event::Kind::kTurn, current_turn_id_, activation, {}, {}, {}});
    return true;
  }
  if (*type == kTransmitting) {
    std::vector<Field> body;
    const auto* bytes = Bytes(upward, 4);
    if (!bytes || !Parse(bytes->data(), bytes->size(), &body)) {
      last_error_ = "malformed native stream packet";
      return false;
    }
    if (Number(body, 1) != kAsrStream || !stream_active_ || turn_finished_) return true;
    const auto* pcm = Bytes(body, 2);
    if (!pcm || pcm->empty() || pcm->size() % 2 != 0) {
      last_error_ = "invalid native PCM16 payload";
      return false;
    }
    Event event{Event::Kind::kPcm, current_turn_id_, Activation::kUnknown, {}, {}, {}};
    event.pcm = *pcm;
    Queue(std::move(event));
    return true;
  }
  if (*type == kCancel || *type == kEnd) {
    if (stream_active_) {
      stream_active_ = false;
      Queue(Event{*type == kCancel ? Event::Kind::kCancel : Event::Kind::kEnd,
                  current_turn_id_, Activation::kUnknown, {}, {}, {}});
    }
    return true;
  }
  return true;
}

void Source::DispatchLoop() {
  while (true) {
    Event event;
    {
      std::unique_lock<std::mutex> lock(queue_mu_);
      queue_cv_.wait(lock, [this] { return !queue_.empty() || dispatch_stop_; });
      if (dispatch_stop_ && queue_.empty()) break;
      event = std::move(queue_.front());
      queue_.pop_front();
      queued_pcm_bytes_ -= event.pcm.size();
    }
    try {
      switch (event.kind) {
        case Event::Kind::kReady:
          if (callbacks_.on_ready) callbacks_.on_ready(event.text, event.codec);
          break;
        case Event::Kind::kTurn:
          if (callbacks_.on_turn) callbacks_.on_turn(event.turn_id, event.activation);
          break;
        case Event::Kind::kPcm:
          if (callbacks_.on_pcm) callbacks_.on_pcm(event.turn_id, event.pcm);
          break;
        case Event::Kind::kEnd:
          if (callbacks_.on_end) callbacks_.on_end(event.turn_id);
          break;
        case Event::Kind::kCancel:
          if (callbacks_.on_cancel) callbacks_.on_cancel(event.turn_id);
          break;
        case Event::Kind::kFatal:
          if (callbacks_.on_fatal) callbacks_.on_fatal(event.text);
          break;
      }
    } catch (const std::exception&) {
      // A callback failure must not terminate the protocol thread.
      if (event.kind != Event::Kind::kFatal)
        Fail("native speech callback exception", FatalClass::kCallback);
    } catch (...) {
      if (event.kind != Event::Kind::kFatal)
        Fail("native speech callback threw unknown exception", FatalClass::kCallback);
    }
  }
}

}  // namespace xiaoai_plus::native_speech
