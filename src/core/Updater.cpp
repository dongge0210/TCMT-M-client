// Updater — self-update via GitHub Releases with Ed25519 signature checks.
// macOS-only for now (TCMT_USE_TLS); compiles to a no-op elsewhere.
#include "Updater.h"
#include "I18n.h"
#include "Utils/Logger.h"
#include "nlohmann/json.hpp"

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef TCMT_USE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifndef _WIN32
#include <mach-o/dyld.h>
#endif

using json = nlohmann::json;

// Public key paired with the release-signing private key (kept out of the
// repo at ~/.tcmt/updater/update_key.pem). Rotate both together.
static const char kUpdatePublicKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAb2CraL8elTRcTOiM0xExx02ZNGkQ18c8jqRl5vPQnI0=\n"
    "-----END PUBLIC KEY-----\n";

static const char* kRepoOwner = "dongge0210";
static const char* kRepoName = "TCMT-M-client";

namespace {

int StateInt(Updater::State s) { return static_cast<int>(s); }
Updater::State FromInt(int i) { return static_cast<Updater::State>(i); }

void SetStatus(Updater::State s, std::string text, std::atomic<int>* state,
               std::mutex* m, std::string* status) {
    state->store(StateInt(s));
    std::lock_guard<std::mutex> lk(*m);
    *status = std::move(text);
}

// "0.2.1" style numeric-dotted comparison; missing parts count as 0.
int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> v;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, '.')) {
            int n = 0;
            for (char c : part) {
                if (c < '0' || c > '9') break;
                n = n * 10 + (c - '0');
            }
            v.push_back(n);
        }
        return v;
    };
    auto va = split(a), vb = split(b);
    size_t n = va.size() > vb.size() ? va.size() : vb.size();
    for (size_t i = 0; i < n; ++i) {
        int x = i < va.size() ? va[i] : 0;
        int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

} // namespace

void Updater::CheckForUpdate() {
#ifdef TCMT_USE_TLS
    if (running_.exchange(true)) return;
    thread_ = std::thread(&Updater::CheckThread, this);
#else
    (void)0;
#endif
}

void Updater::StartDownload() {
#ifdef TCMT_USE_TLS
    if (state_.load() != StateInt(State::Available)) return;
    state_.store(StateInt(State::Downloading));
    SetStatus(State::Downloading, tcmt::Tr("update.downloading"), &state_, &mutex_, &status_);
    thread_ = std::thread(&Updater::DownloadThread, this);
#endif
}

void Updater::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

Updater::State Updater::GetState() const { return FromInt(state_.load()); }

std::string Updater::StatusText() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

std::string Updater::LatestVersion() const { return latestVersion_; }

#ifdef TCMT_USE_TLS
// Minimal HTTPS GET (no chunked encoding; reads until close).
bool Updater::HttpsGet(const std::string& url, std::string& out) {
    std::string host, path;
    {
        std::string s = url;
        if (s.rfind("https://", 0) != 0) return false;
        s = s.substr(8);
        size_t slash = s.find('/');
        if (slash == std::string::npos) { host = s; path = "/"; }
        else { host = s.substr(0, slash); path = s.substr(slash); }
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || !res) return false;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); return false; }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return false;
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
        "\r\nUser-Agent: TCMT-M-updater\r\nAccept: application/octet-stream, application/json\r\nConnection: close\r\n\r\n";
    if (SSL_write(ssl, req.data(), (int)req.size()) <= 0) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return false;
    }
    std::string raw;
    char buf[8192];
    for (;;) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, n);
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    const char* bodyStart = strstr(raw.c_str(), "\r\n\r\n");
    if (!bodyStart) return false;
    // Only accept 2xx responses (404 = no releases yet → caller treats as idle).
    if (strncmp(raw.c_str(), "HTTP/1.1 2", 11) != 0 &&
        strncmp(raw.c_str(), "HTTP/1.0 2", 11) != 0) {
        return false;
    }
    out = std::string(bodyStart + 4);
    return !out.empty();
}

bool Updater::VerifyManifest(const std::string& manifestJson, const std::string& sigB64) {
    // base64 decode the signature
    std::string sig;
    {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int bits = 0, acc = 0;
        for (char c : sigB64) {
            if (c == '=' || c == '\n' || c == '\r') continue;
            const char* p = strchr(tbl, c);
            if (!p) return false;
            acc = (acc << 6) | (int)(p - tbl);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                sig.push_back((char)((acc >> bits) & 0xff));
            }
        }
    }
    if (sig.empty()) return false;

    BIO* bio = BIO_new_mem_buf(kUpdatePublicKeyPem, -1);
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) return false;

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    bool ok = md &&
        EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(md,
            reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
            reinterpret_cast<const unsigned char*>(manifestJson.data()), manifestJson.size()) == 1;
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    return ok;
}

void Updater::CheckThread() {
    SetStatus(State::Checking, tcmt::Tr("update.checking"), &state_, &mutex_, &status_);

    std::string releaseJson;
    std::string url = std::string("https://api.github.com/repos/") +
        kRepoOwner + "/" + kRepoName + "/releases/latest";
    if (!HttpsGet(url, releaseJson)) {
        // No releases yet (404) or network trouble — stay silent until a
        // release actually exists; don't nag with a red banner.
        SetStatus(State::Idle, "", &state_, &mutex_, &status_);
        running_.store(false);
        return;
    }

    try {
        json rel = json::parse(releaseJson);
        std::string tag = rel.value("tag_name", "");
        if (!tag.empty() && tag[0] == 'v') tag = tag.substr(1);
        latestVersion_ = tag;

        // Find manifest + signature assets.
        std::string manifestUrl, sigUrl;
        for (const auto& a : rel.value("assets", json::array())) {
            std::string name = a.value("name", "");
            if (name == "manifest.json") manifestUrl = a.value("browser_download_url", "");
            else if (name == "manifest.json.sig") sigUrl = a.value("browser_download_url", "");
        }
        if (manifestUrl.empty() || sigUrl.empty()) {
            SetStatus(State::Failed, tcmt::Tr("update.fail.manifest"), &state_, &mutex_, &status_);
            running_.store(false);
            return;
        }

        std::string manifest, sigB64;
        if (!HttpsGet(manifestUrl, manifest) || !HttpsGet(sigUrl, sigB64) ||
            !VerifyManifest(manifest, sigB64)) {
            SetStatus(State::Failed, tcmt::Tr("update.fail.sig"), &state_, &mutex_, &status_);
            running_.store(false);
            return;
        }

        json m = json::parse(manifest);
        std::string version = m.value("version", "");
        if (CompareVersions(version, TCMT_VERSION_STR) <= 0) {
            SetStatus(State::Idle, "", &state_, &mutex_, &status_);
            running_.store(false);
            return;
        }
        // Resolve the binary asset by name (URLs are only known post-upload).
        const std::string assetName = m.value("asset", "");
        for (const auto& a : rel.value("assets", json::array())) {
            if (a.value("name", "") == assetName) {
                assetUrl_ = a.value("browser_download_url", "");
            }
        }
        if (assetUrl_.empty()) {
            SetStatus(State::Failed, tcmt::Tr("update.fail.nobin"), &state_, &mutex_, &status_);
            running_.store(false);
            return;
        }
        manifestJson_ = manifest;
        latestVersion_ = version;
        char verBuf[256];
        snprintf(verBuf, sizeof(verBuf), tcmt::Tr("update.newver"),
                 version.c_str(), TCMT_VERSION_STR);
        SetStatus(State::Available, verBuf, &state_, &mutex_, &status_);
    } catch (...) {
        SetStatus(State::Failed, tcmt::Tr("update.fail.parse"), &state_, &mutex_, &status_);
    }
    running_.store(false);
}

void Updater::DownloadThread() {
    if (assetUrl_.empty() || exePath_.empty()) {
        SetStatus(State::Failed, tcmt::Tr("update.fail.url"), &state_, &mutex_, &status_);
        return;
    }
    std::string data;
    if (!HttpsGet(assetUrl_, data)) {
        SetStatus(State::Failed, tcmt::Tr("update.fail.dl"), &state_, &mutex_, &status_);
        return;
    }

    // sha256 check against the signed manifest.
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int mdLen = 0;
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(mdctx, data.data(), data.size());
        EVP_DigestFinal_ex(mdctx, md, &mdLen);
        EVP_MD_CTX_free(mdctx);
        char hex[EVP_MAX_MD_SIZE * 2 + 1] = {0};
        for (unsigned int i = 0; i < mdLen; ++i)
            snprintf(hex + i * 2, 3, "%02x", md[i]);
        std::string expect;
        try {
            expect = json::parse(manifestJson_).value("sha256", "");
        } catch (...) {}
        if (expect.empty() || expect != std::string(hex)) {
            SetStatus(State::Failed, tcmt::Tr("update.fail.hash"), &state_, &mutex_, &status_);
            return;
        }
    }

    // Swap the running binary: write next to it, then rename over it.
    // (On Unix a running executable can be replaced in place; the new
    // binary takes effect on next launch.)
    std::string tmp = exePath_ + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SetStatus(State::Failed, tcmt::Tr("update.fail.tmp"), &state_, &mutex_, &status_);
            return;
        }
        f.write(data.data(), (std::streamsize)data.size());
        f.close();
        chmod(tmp.c_str(), 0755);
    }
    if (rename(tmp.c_str(), exePath_.c_str()) != 0) {
        unlink(tmp.c_str());
        SetStatus(State::Failed, tcmt::Tr("update.fail.bin"), &state_, &mutex_, &status_);
        return;
    }
    SetStatus(State::Ready, tcmt::Tr("update.ready"), &state_, &mutex_, &status_);
}
#endif // TCMT_USE_TLS
