// test_blockchain.cpp
#include <crow.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

class GethClient {
    std::string rpc_url;
public:
    GethClient(const std::string& url) : rpc_url(url) {}
    
    json rpcCall(const std::string& method, const json& params) {
        json req = {{"jsonrpc","2.0"}, {"method",method}, {"params",params}, {"id",1}};
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) return json::object();
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, rpc_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.dump().c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        if (res != CURLE_OK) return json::object();
        return json::parse(response);
    }
    
    uint64_t blockNumber() {
        auto resp = rpcCall("eth_blockNumber", json::array());
        if (resp.contains("result")) {
            std::string hex = resp["result"].get<std::string>();
            return std::stoull(hex, nullptr, 16);
        }
        return 0;
    }
};

int main() {
    crow::App<> app;
    
    CROW_ROUTE(app, "/api/blockchain/status").methods("GET"_method)([]() {
        GethClient client("http://192.168.138.146:30001");
        uint64_t height = client.blockNumber();
        json resp = {{"height", height}, {"mining", false}, {"peers", 3}};
        return crow::response(resp.dump());
    });
    
    CROW_ROUTE(app, "/api/health")([]() {
        return crow::response("{\"status\":\"ok\"}");
    });
    
    std::cout << "🚀 Server running at http://localhost:8580" << std::endl;
    app.port(8580).multithreaded().run();
    return 0;
}
