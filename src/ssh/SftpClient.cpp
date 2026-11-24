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
    const int code = sftp_ ? sftp_get_error(sftp_) : 0;
    switch (code) {
    case SSH_FX_NO_SUCH_FILE: return what + ": no such file";
    case SSH_FX_PERMISSION_DENIED: return what + ": permission denied";
    case SSH_FX_FILE_ALREADY_EXISTS: return what + ": it already exists";
    }
    return what + ": " + (sftp_ ? "SFTP error " + std::to_string(code) + ", " : "") + ssh_->lastError();
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
                entries.push_back({a->name, a->size, int64_t(a->mtime), a->type == SSH_FILEXFER_TYPE_DIRECTORY, a->permissions & 07777});
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
        if (failure.empty() && std::rename(partial.c_str(), local.c_str()) != 0) failure = "Cannot rename to " + local;
        if (!failure.empty()) std::remove(partial.c_str());
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

void SftpClient::call(std::function<int()> op, std::string failure, Done done)
{
    post([this, op = std::move(op), failure = std::move(failure), done = std::move(done)] { done(op() == 0 ? "" : error(failure)); });
}

void SftpClient::rename(std::string from, std::string to, Done done)
{
    post([this, from = std::move(from), to = std::move(to), done = std::move(done)] {
        if (sftp_attributes a = sftp_lstat(sftp_, to.c_str())) { // libssh's posix-rename would replace it
            sftp_attributes_free(a);
            return done(to.substr(to.find_last_of('/') + 1) + " already exists");
        }
        done(sftp_rename(sftp_, from.c_str(), to.c_str()) == 0 ? "" : error("Cannot rename " + from));
    });
}

int SftpClient::removeTree(const std::string& path)
{
    sftp_attributes a = sftp_lstat(sftp_, path.c_str()); // a link is removed, not followed
    if (!a) return -1;
    const bool directory = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
    sftp_attributes_free(a);
    if (!directory) return sftp_unlink(sftp_, path.c_str());
    if (sftp_dir dir = sftp_opendir(sftp_, path.c_str())) {
        int rc = 0;
        while (sftp_attributes e = sftp_readdir(sftp_, dir)) {
            if (rc == 0 && strcmp(e->name, ".") != 0 && strcmp(e->name, "..") != 0) rc = removeTree(path + "/" + e->name);
            sftp_attributes_free(e);
        }
        sftp_closedir(dir);
        if (rc != 0) return rc;
    }
    return sftp_rmdir(sftp_, path.c_str());
}

void SftpClient::remove(std::string path, Done done)
{
    call([this, path] { return removeTree(path); }, "Cannot delete " + path, std::move(done));
}

void SftpClient::makeFolder(std::string path, Done done)
{
    call([this, path] { return sftp_mkdir(sftp_, path.c_str(), 0755); }, "Cannot create " + path, std::move(done));
}

void SftpClient::makeFile(std::string path, Done done)
{
    call([this, path] {
        sftp_file f = sftp_open(sftp_, path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        return f ? sftp_close(f) : -1;
    }, "Cannot create " + path, std::move(done));
}

void SftpClient::setPermissions(std::string path, unsigned mode, Done done)
{
    call([this, path, mode] { return sftp_chmod(sftp_, path.c_str(), mode); }, "Cannot change " + path, std::move(done));
}

} // namespace rdc
