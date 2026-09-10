#include <crow.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <iostream>

using json = nlohmann::json;

static std::string g_k8s_token;
static std::string g_k8s_api_server = "https://192.168.138.150:7443";

std::string readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return "<h1>Error: index.html not found</h1>";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

std::string k8sRequestRaw(const std::string& method, const std::string& path) {
    CURL* curl = curl_easy_init();
    std::string response;
    std::string url = g_k8s_api_server + path;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    
    struct curl_slist* headers = NULL;
    std::string authHeader = "Authorization: Bearer " + g_k8s_token;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return response;
}

// 安全获取 JSON 字段值
std::string safeGetString(const json& obj, const std::string& key, const std::string& defaultValue = "") {
    try {
        if (obj.contains(key) && !obj[key].is_null()) {
            return obj[key].get<std::string>();
        }
    } catch (...) {}
    return defaultValue;
}

int safeGetInt(const json& obj, const std::string& key, int defaultValue = 0) {
    try {
        if (obj.contains(key) && !obj[key].is_null()) {
            return obj[key].get<int>();
        }
    } catch (...) {}
    return defaultValue;
}

int main() {
    const char* tokenEnv = std::getenv("K8S_TOKEN");
    if (tokenEnv != nullptr && strlen(tokenEnv) > 0) {
        g_k8s_token = tokenEnv;
        std::cout << "✅ K8S_TOKEN loaded (length: " << g_k8s_token.length() << ")" << std::endl;
    } else {
        std::cout << "❌ K8S_TOKEN not found!" << std::endl;
        return 1;
    }
    
    std::cout << "K8S_API_SERVER: " << g_k8s_api_server << std::endl;
    
    crow::App<> app;
    
    // 提供静态 HTML 文件
    CROW_ROUTE(app, "/")
    ([](){
        return crow::response(readFile("index.html"));
    });
    
    // 健康检查
    CROW_ROUTE(app, "/api/health")
    ([](){
        json resp = {{"status", "ok"}, {"token_loaded", !g_k8s_token.empty()}};
        return crow::response(resp.dump());
    });
    
    // 获取所有 Pods - 安全版本
    CROW_ROUTE(app, "/api/pods")
    ([](){
        if (g_k8s_token.empty()) {
            return crow::response(500, R"({"error":"no token"})");
        }
        try {
            std::string response = k8sRequestRaw("GET", "/api/v1/pods");
            if (response.empty()) {
                return crow::response("[]");
            }
            auto pods = json::parse(response);
            json result;
            if (pods.contains("items")) {
                for (const auto& item : pods["items"]) {
                    json pod;
                    // 使用安全获取方法
                    pod["name"] = safeGetString(item["metadata"], "name");
                    pod["namespace"] = safeGetString(item["metadata"], "namespace");
                    pod["status"] = safeGetString(item["status"], "phase");
                    pod["node"] = safeGetString(item["spec"], "nodeName");
                    pod["ip"] = safeGetString(item["status"], "podIP");
                    result.push_back(pod);
                }
            }
            return crow::response(result.dump());
        } catch (const std::exception& e) {
            return crow::response("[]");
        }
    });
    
    // 获取所有 Nodes - 安全版本
    CROW_ROUTE(app, "/api/nodes")
    ([](){
        if (g_k8s_token.empty()) {
            return crow::response(500, R"({"error":"no token"})");
        }
        try {
            std::string response = k8sRequestRaw("GET", "/api/v1/nodes");
            if (response.empty()) {
                return crow::response("[]");
            }
            auto nodes = json::parse(response);
            json result;
            if (nodes.contains("items")) {
                for (const auto& item : nodes["items"]) {
                    json node;
                    node["name"] = safeGetString(item["metadata"], "name");
                    node["status"] = "Unknown";
                    if (item.contains("status") && item["status"].contains("conditions")) {
                        for (const auto& cond : item["status"]["conditions"]) {
                            if (safeGetString(cond, "type") == "Ready") {
                                node["status"] = safeGetString(cond, "status") == "True" ? "Ready" : "NotReady";
                                break;
                            }
                        }
                    }
                    node["kubelet"] = safeGetString(item["status"]["nodeInfo"], "kubeletVersion");
                    node["os"] = safeGetString(item["status"]["nodeInfo"], "operatingSystem");
                    result.push_back(node);
                }
            }
            return crow::response(result.dump());
        } catch (const std::exception& e) {
            return crow::response("[]");
        }
    });
    
    // 获取 Deployments - 直接返回空数组
    CROW_ROUTE(app, "/api/deployments")
    ([](){
        return crow::response("[]");
    });
    
    // 集群概览 - 安全版本
    CROW_ROUTE(app, "/api/summary")
    ([](){
        if (g_k8s_token.empty()) {
            json summary = {{"nodeCount", 0}, {"podCount", 0}, {"runningPods", 0}, {"pendingPods", 0}, {"failedPods", 0}, {"deployCount", 0}};
            return crow::response(summary.dump());
        }
        try {
            std::string podsResp = k8sRequestRaw("GET", "/api/v1/pods");
            std::string nodesResp = k8sRequestRaw("GET", "/api/v1/nodes");
            
            int nodeCount = 0, podCount = 0, running = 0, pending = 0, failed = 0;
            
            if (!nodesResp.empty()) {
                try {
                    auto nodes = json::parse(nodesResp);
                    nodeCount = nodes.contains("items") ? (int)nodes["items"].size() : 0;
                } catch (...) {}
            }
            
            if (!podsResp.empty()) {
                try {
                    auto pods = json::parse(podsResp);
                    if (pods.contains("items")) {
                        podCount = (int)pods["items"].size();
                        for (const auto& item : pods["items"]) {
                            std::string s = safeGetString(item["status"], "phase");
                            if (s == "Running") running++;
                            else if (s == "Pending") pending++;
                            else if (s == "Failed") failed++;
                        }
                    }
                } catch (...) {}
            }
            
            json summary = {
                {"nodeCount", nodeCount},
                {"podCount", podCount},
                {"runningPods", running},
                {"pendingPods", pending},
                {"failedPods", failed},
                {"deployCount", 0}
            };
            return crow::response(summary.dump());
        } catch (const std::exception& e) {
            json summary = {{"nodeCount", 0}, {"podCount", 0}, {"runningPods", 0}, {"pendingPods", 0}, {"failedPods", 0}, {"deployCount", 0}};
            return crow::response(summary.dump());
        }
    });
    
    // 删除 Pod
    CROW_ROUTE(app, "/api/namespaces/<string>/pods/<string>")
    .methods("DELETE"_method)([](const std::string& ns, const std::string& podName){
        if (g_k8s_token.empty()) {
            return crow::response(500, R"({"error":"no token"})");
        }
        try {
            k8sRequestRaw("DELETE", "/api/v1/namespaces/" + ns + "/pods/" + podName);
            return crow::response(R"({"status":"deleted"})");
        } catch (const std::exception& e) {
            return crow::response(500, R"({"error":")" + std::string(e.what()) + "\"}");
        }
    });
    
    std::cout << "🚀 Server running at http://localhost:8580" << std::endl;
    std::cout << "📊 Open browser: http://192.168.138.139:8580" << std::endl;
    app.port(8580).multithreaded().run();
    return 0;
}
