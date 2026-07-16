#pragma once

#include "job_runner.hpp"

#include <filesystem>

namespace fsd {

class HttpServer {
public:
    HttpServer(int port, JobRunner& runner, std::filesystem::path repoRoot);
    void serveForever();

private:
    int port_;
    JobRunner& runner_;
    std::filesystem::path repoRoot_;

    static std::string makeHttpResponse(int status, const std::string& body, const std::string& contentType = "application/json", const std::string& extraHeaders = "");
    std::string handleRequest(const std::string& request) const;
    bool streamArtifactResponse(int clientFd, const std::string& request) const;
};

} // namespace fsd
