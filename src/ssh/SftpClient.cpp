#include "ssh/SftpClient.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rdc {

SftpClient::SftpClient(SessionListener& listener, SshConnection::Params params)
    : listener_(listener), params_(std::move(params))
{
}

void SftpClient::start(Done opened)
{
    if (worker_.joinable()) return;
    ssh_ = std::make_unique<SshConnection>(listener_, params_);
    worker_ = std::jthread([this, opened = std::move(opened)](std::stop_token st) { run(st, opened); });
}

void SftpClient::stop()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    ssh_->abort();
    wake_.notify_all();
    worker_.join();
}

void SftpClient::post(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_all();
}

std::string SftpClient::error(const std::string& what) const
{
    return what + ": " + (sftp_ ? "SFTP error " + std::to_string(sftp_get_error(sftp_)) + ", " : "") + ssh_->lastError();
}

void SftpClient::run(std::stop_token stop, Done opened)
{
    if (!ssh_->open([&] { return stop.stop_requested(); })) {
        opened(stop.stop_requested() || ssh_->error().empty() ? "cancelled" : ssh_->error());
        return;
    }
    sftp_ = sftp_new(ssh_->session());
    if (!sftp_ || sftp_init(sftp_) != SSH_OK) {
        opened(error("Could not start SFTP"));
        if (sftp_) sftp_free(sftp_);
        sftp_ = nullptr;
        return;
    }
    if (char* path = sftp_canonicalize_path(sftp_, ".")) {
        home_ = path;
        ssh_string_free_char(path);
    }
    opened("");

    std::stop_callback wakeOnStop(stop, [this] { wake_.notify_all(); });
    while (!stop.stop_requested()) {
        std::vector<std::function<void()>> tasks;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return !tasks_.empty() || stop.stop_requested(); });
            tasks.swap(tasks_);
        }
        for (auto& t : tasks) t();
    }
    sftp_free(sftp_);
    sftp_ = nullptr;
}

void SftpClient::list(std::string path, std::function<void(std::vector<Entry>, std::string)> done)
{
    post([this, path = std::move(path), done = std::move(done)] {
        sftp_dir dir = sftp_opendir(sftp_, path.c_str());
        if (!dir) return done({}, error("Cannot open " + path));
        std::vector<Entry> entries;
        while (sftp_attributes a = sftp_readdir(sftp_, dir)) {
            if (strcmp(a->name, ".") != 0 && strcmp(a->name, "..") != 0)
                entries.push_back({a->name, a->size, int64_t(a->mtime), a->type == SSH_FILEXFER_TYPE_DIRECTORY});
            sftp_attributes_free(a);
        }
        sftp_closedir(dir);
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.directory != b.directory ? a.directory : a.name < b.name;
        });
        done(std::move(entries), "");
    });
}

void SftpClient::download(std::string remote, std::string local, Done done)
{
    post([this, remote = std::move(remote), local = std::move(local), done = std::move(done)] {
        sftp_file in = sftp_open(sftp_, remote.c_str(), O_RDONLY, 0);
        if (!in) return done(error("Cannot open " + remote));
        // temp name + rename: a failed transfer never replaces an existing file
        const std::string partial = local + ".part";
        FILE* out = fopen(partial.c_str(), "wb");
        std::string failure = out ? "" : "Cannot write " + partial;
        char buffer[65536];
        while (failure.empty()) {
            const ssize_t n = sftp_read(in, buffer, sizeof buffer);
            if (n == 0) break;
            if (n < 0) failure = error("Read failed on " + remote);
            else if (fwrite(buffer, 1, size_t(n), out) != size_t(n)) failure = "Write failed on " + partial;
        }
        sftp_close(in);
        if (out && fclose(out) != 0 && failure.empty()) failure = "Write failed on " + partial;
        if (failure.empty() && rename(partial.c_str(), local.c_str()) != 0) failure = "Cannot rename to " + local;
        if (!failure.empty()) remove(partial.c_str());
        done(failure);
    });
}

void SftpClient::upload(std::string local, std::string remote, Done done)
{
    post([this, local = std::move(local), remote = std::move(remote), done = std::move(done)] {
        FILE* in = fopen(local.c_str(), "rb");
        if (!in) return done("Cannot read " + local);
        sftp_file out = sftp_open(sftp_, remote.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        std::string failure = out ? "" : error("Cannot create " + remote);
        char buffer[65536];
        while (failure.empty()) {
            const size_t n = fread(buffer, 1, sizeof buffer, in);
            if (n == 0) {
                if (ferror(in)) failure = "Read failed on " + local;
                break;
            }
            if (sftp_write(out, buffer, n) != ssize_t(n)) failure = error("Write failed on " + remote);
        }
        fclose(in);
        if (out) sftp_close(out);
        done(failure);
    });
}

} // namespace rdc
