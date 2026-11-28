#include "discord_presence.h"

#include "discord_activity_json.h"

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace DiscordPresence {
namespace {

constexpr uint32_t kHandshakeOpcode = 0;
constexpr uint32_t kFrameOpcode = 1;
constexpr size_t kMaxClientIdLength = 32;
// Discord not running: look for it again this often (10 pipe or socket names, cheap).
constexpr auto kConnectionRetryCooldown = std::chrono::seconds(10);
// Discord accepts at most 5 SET_ACTIVITY calls per 20 s; a newer activity waits for the next slot.
constexpr auto kMinSendInterval = std::chrono::seconds(5);
// While connected, read Discord's answers and notice it quitting this often.
constexpr auto kIdleWake = std::chrono::seconds(5);

bool IsClientId(std::string_view value) {
    return !value.empty() && value.size() <= kMaxClientIdLength &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string BuildActivityPayload(const Activity& activity) {
#if defined(_WIN32)
    return ActivityJson::Build(activity, static_cast<int64_t>(::GetCurrentProcessId()));
#else
    return ActivityJson::Build(activity, static_cast<int64_t>(::getpid()));
#endif
}

// The game thread and the guest only hand the wanted activity to a worker thread; the worker does all the pipe I/O
// (connect, the READY wait of up to a second, writes), so a slow or missing Discord never stalls a frame. Connection
// state (connected_, handle_/fd_, lastFailedConnectionAttempt_) belongs to the worker alone.
class Client {
public:
    ~Client() { Shutdown(); }

    void Initialize(const std::string& clientId, const std::string& title) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            basicClientId_ = IsClientId(clientId) ? clientId : std::string{};
            basicActivity_ = {};
            basicActivity_.largeImageText = title;
            basicActivity_.startTimestamp = UnixSeconds();
            customClient_ = false;
            activity_ = basicActivity_;
            reconnect_ = true;
            notified_ = true;
            stop_ = false;
            if (!worker_.joinable()) {
                worker_ = std::thread([this] { Run(); });
            }
        }
        wake_.notify_all();
    }

    // The host's own activity (what the game is doing). Keeps the start time set by Initialize.
    void SetBasicActivity(Activity activity) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activity.startTimestamp = basicActivity_.startTimestamp;
            basicActivity_ = std::move(activity);
            if (customClient_) {
                return;
            }
            activity_ = basicActivity_;
            dirty_ = true;
            notified_ = true;
        }
        wake_.notify_all();
    }

    void SetClient(const std::string& clientId) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!IsClientId(clientId)) {
                RT_LOG(RT_TAG_RUNTIME) << "Ignoring invalid Discord client ID from /dev/dolphin" << std::endl;
                return;
            }
            if (customClient_ && clientId_ == clientId) {
                return;
            }
            customClient_ = true;
            clientId_ = clientId;
            reconnect_ = true;
            notified_ = true;
        }
        wake_.notify_all();
    }

    void SetActivity(Activity activity) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!customClient_) {
                return;
            }
            activity_ = std::move(activity);
            dirty_ = true;
            notified_ = true;
        }
        wake_.notify_all();
    }

    void Reset() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            customClient_ = false;
            clientId_.clear();
            activity_ = basicActivity_;
            reconnect_ = true;
            notified_ = true;
        }
        wake_.notify_all();
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

private:
    static int64_t UnixSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            notified_ = false;
            if (reconnect_) {
                reconnect_ = false;
                Close();
                dirty_ = true;
            }
            const std::string clientId = customClient_ ? clientId_ : basicClientId_;
            const auto now = std::chrono::steady_clock::now();
            auto wakeAt = now + kIdleWake;
            if (!clientId.empty()) {
                if (connected_ || ConnectionRetryAllowed()) {
                    // A closed connection always leaves dirty_ set: a new one sends the activity again.
                    const bool wasConnected = connected_;
                    const bool sendDue = dirty_;
                    const bool sendAllowed = !lastSend_ || now - *lastSend_ >= kMinSendInterval;
                    const std::string payload = sendDue && sendAllowed ? BuildActivityPayload(activity_) : std::string{};
                    if (sendDue && sendAllowed) {
                        dirty_ = false;
                        lastSend_ = now;
                    } else if (sendDue) {
                        wakeAt = *lastSend_ + kMinSendInterval;
                    }
                    lock.unlock();
                    bool ok = wasConnected ? DrainIncoming() : Handshake(clientId);
                    if (ok && !payload.empty()) {
                        ok = WriteFrame(kFrameOpcode, payload);
                    }
                    if (!ok) {
                        if (wasConnected) {
                            RT_LOG(RT_TAG_RUNTIME) << "Discord Rich Presence: lost the Discord app" << std::endl;
                        }
                        RecordFailedConnection();
                        Close();
                    } else if (!wasConnected) {
                        RT_LOG(RT_TAG_RUNTIME) << "Discord Rich Presence: connected" << std::endl;
                    }
                    lock.lock();
                    if (!ok) {
                        dirty_ = true;
                        wakeAt = std::chrono::steady_clock::now() + kConnectionRetryCooldown;
                    }
                } else {
                    wakeAt = *lastFailedConnectionAttempt_ + kConnectionRetryCooldown;
                }
            }
            wake_.wait_until(lock, wakeAt, [this] { return stop_ || notified_; });
        }
        lock.unlock();
        Close();
    }

    bool Handshake(const std::string& clientId) {
        if (!Connect()) {
            return false;
        }
        const std::string handshake = "{\"v\":1,\"client_id\":\"" + clientId + "\"}";
        return WriteFrame(kHandshakeOpcode, handshake) && ReadReady();
    }

    bool WriteFrame(uint32_t opcode, std::string_view payload) {
        std::array<uint8_t, 8> header{};
        const uint32_t size = static_cast<uint32_t>(payload.size());
        for (size_t i = 0; i < 4; ++i) {
            header[i] = static_cast<uint8_t>(opcode >> (i * 8));
            header[4 + i] = static_cast<uint8_t>(size >> (i * 8));
        }
        return WriteAll(header.data(), header.size()) &&
               WriteAll(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    }

    bool ConnectionRetryAllowed() const {
        return !lastFailedConnectionAttempt_ ||
               std::chrono::steady_clock::now() - *lastFailedConnectionAttempt_ >= kConnectionRetryCooldown;
    }

    void RecordFailedConnection() {
        lastFailedConnectionAttempt_ = std::chrono::steady_clock::now();
    }

#if defined(_WIN32)
    bool Connect() {
        if (connected_) {
            return true;
        }
        for (unsigned int index = 0; index < 10; ++index) {
            const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(index);
            handle_ = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_BYTE;
                ::SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr);
                connected_ = true;
                lastFailedConnectionAttempt_.reset();
                return true;
            }
        }
        return false;
    }

    bool WriteAll(const uint8_t* data, size_t size) {
        while (size != 0) {
            DWORD written = 0;
            if (!::WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) {
                return false;
            }
            data += written;
            size -= written;
        }
        return true;
    }

    bool ReadAll(uint8_t* data, size_t size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (size != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            DWORD available = 0;
            if (!::PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                ::Sleep(10);
                continue;
            }
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(size, available));
            if (!::ReadFile(handle_, data, requested, &read, nullptr) || read == 0) {
                return false;
            }
            data += read;
            size -= read;
        }
        return true;
    }

    // Discord answers every command; read the answers so the pipe never fills. False when Discord has gone.
    bool DrainIncoming() {
        std::array<uint8_t, 4096> buffer{};
        for (;;) {
            DWORD available = 0;
            if (!::PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                return true;
            }
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(buffer.size(), available));
            if (!::ReadFile(handle_, buffer.data(), requested, &read, nullptr) || read == 0) {
                return false;
            }
            ReportRefusals(buffer.data(), read);
        }
    }

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        connected_ = false;
        answers_.clear();
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    bool Connect() {
        if (connected_) {
            return true;
        }
        // Discord's socket sits in the runtime or temp folder, or under the runtime folder in a sandbox's own
        // subfolder for Flatpak and Snap builds of Discord (and Vesktop's Flatpak).
        std::vector<std::string> roots;
        if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir) {
            const std::string runtime = runtimeDir;
            roots.push_back(runtime);
            roots.push_back(runtime + "/app/com.discordapp.Discord");
            roots.push_back(runtime + "/snap.discord");
            roots.push_back(runtime + "/.flatpak/dev.vencord.Vesktop/xdg-run");
        }
        for (const char* variable : {"TMPDIR", "TMP", "TEMP"}) {
            if (const char* tempDir = std::getenv(variable); tempDir && *tempDir) {
                roots.push_back(tempDir);
            }
        }
        roots.push_back("/tmp");
        const size_t rootCount = roots.size();
        for (size_t rootIndex = 0; rootIndex < rootCount; ++rootIndex) {
            for (unsigned int index = 0; index < 10; ++index) {
                const std::string path = roots[rootIndex] + "/discord-ipc-" + std::to_string(index);
                if (path.size() >= sizeof(sockaddr_un::sun_path)) {
                    continue;
                }
                const int socketFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (socketFd < 0) {
                    continue;
                }
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
                if (::connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                    timeval timeout{};
                    timeout.tv_sec = 1;
                    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    fd_ = socketFd;
                    connected_ = true;
                    lastFailedConnectionAttempt_.reset();
                    return true;
                }
                ::close(socketFd);
            }
        }
        return false;
    }

    bool WriteAll(const uint8_t* data, size_t size) {
        while (size != 0) {
            const ssize_t written = ::send(fd_, data, size, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            data += written;
            size -= static_cast<size_t>(written);
        }
        return true;
    }

    bool ReadAll(uint8_t* data, size_t size) {
        while (size != 0) {
            const ssize_t read = ::recv(fd_, data, size, 0);
            if (read <= 0) {
                return false;
            }
            data += read;
            size -= static_cast<size_t>(read);
        }
        return true;
    }

    // Discord answers every command; read the answers so the socket never fills. False when Discord has gone.
    bool DrainIncoming() {
        std::array<uint8_t, 4096> buffer{};
        for (;;) {
            const ssize_t read = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (read > 0) {
                ReportRefusals(buffer.data(), static_cast<size_t>(read));
                continue;
            }
            return read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
        answers_.clear();
    }

    int fd_ = -1;
#endif

    // Discord answers a refused SET_ACTIVITY with an ERROR event and the reason ("state" length must be at least 2
    // characters long): log the first few, which would otherwise leave the status silently unchanged.
    void ReportRefusals(const uint8_t* data, size_t size) {
        answers_.append(reinterpret_cast<const char*>(data), size);
        while (answers_.size() >= 8) {
            uint32_t length = 0;
            for (size_t i = 0; i < 4; ++i) {
                length |= static_cast<uint32_t>(static_cast<uint8_t>(answers_[4 + i])) << (i * 8);
            }
            if (length > 1024 * 1024) {
                answers_.clear();  // not a frame boundary: drop it and keep draining
                return;
            }
            if (answers_.size() < 8 + size_t(length)) {
                return;
            }
            const std::string_view json(answers_.data() + 8, length);
            if (json.find("\"evt\":\"ERROR\"") != std::string_view::npos && refusalsLogged_ < 5) {
                ++refusalsLogged_;
                RT_LOG(RT_TAG_RUNTIME) << "Discord Rich Presence: Discord refused the status: "
                                       << json.substr(0, 300) << std::endl;
            }
            answers_.erase(0, 8 + size_t(length));
        }
    }

    bool ReadReady() {
        std::array<uint8_t, 8> header{};
        if (!ReadAll(header.data(), header.size())) {
            return false;
        }
        uint32_t opcode = 0;
        uint32_t size = 0;
        for (size_t i = 0; i < 4; ++i) {
            opcode |= static_cast<uint32_t>(header[i]) << (i * 8);
            size |= static_cast<uint32_t>(header[4 + i]) << (i * 8);
        }
        if (opcode != kFrameOpcode || size > 1024 * 1024) {
            return false;
        }
        std::vector<uint8_t> payload(size);
        if (!ReadAll(payload.data(), payload.size())) {
            return false;
        }
        const std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());
        return json.find("\"evt\":\"READY\"") != std::string_view::npos;
    }

    // Shared with the callers, under mutex_.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false;
    bool notified_ = false;
    bool reconnect_ = false;
    bool dirty_ = false;
    bool customClient_ = false;
    std::string basicClientId_;
    std::string clientId_;
    Activity basicActivity_;
    Activity activity_;
    std::optional<std::chrono::steady_clock::time_point> lastSend_;

    // The worker's alone.
    bool connected_ = false;
    std::optional<std::chrono::steady_clock::time_point> lastFailedConnectionAttempt_;
    std::string answers_;      // Discord's answers not yet read to a frame's end (ReportRefusals)
    int refusalsLogged_ = 0;
};

Client g_client;

} // namespace

void Initialize(const std::string& basicClientId, const std::string& basicTitle) {
    g_client.Initialize(basicClientId, basicTitle);
}

void SetBasicActivity(Activity activity) {
    g_client.SetBasicActivity(std::move(activity));
}

void SetClient(const std::string& clientId) {
    g_client.SetClient(clientId);
}

void SetActivity(Activity activity) {
    g_client.SetActivity(std::move(activity));
}

void Reset() {
    g_client.Reset();
}

void Shutdown() {
    g_client.Shutdown();
}

} // namespace DiscordPresence
