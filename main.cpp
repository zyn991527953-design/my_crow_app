#include <crow.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <iostream>
#include <ldap.h>
#include <pqxx/pqxx>
#include <hiredis/hiredis.h>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <regex>
#include "vector_rag.hpp"
#include <sqlite3.h>   // 别忘了添加此头文件



static std::unique_ptr<SimpleRAGEngine> g_rag_engine;
static std::mutex g_rag_mutex;

void init_rag_engine() {
    try {
        std::cout << "[INFO] 初始化 RAG 引擎..." << std::endl;
        std::string llm_url = "http://192.168.138.140:3000/api/chat/completions";
        g_rag_engine = std::make_unique<SimpleRAGEngine>(llm_url);
        g_rag_engine->init_k8s_knowledge();
        std::cout << "[INFO] RAG 初始化完成，知识库大小: "
                  << g_rag_engine->knowledge_size() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] RAG 初始化失败: " << e.what() << std::endl;
    }
}








// ===== WriteCallback =====
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
        json req;
        req["jsonrpc"] = "2.0";
        req["method"] = method;
        req["params"] = params;
        req["id"] = 1;
        
        std::string request_str = req.dump();
        std::cout << "[DEBUG] Sending: " << request_str << std::endl;
        
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[ERROR] curl_easy_init failed" << std::endl;
            return json::object();
        }

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, rpc_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            std::cerr << "[ERROR] CURL error: " << curl_easy_strerror(res) << std::endl;
            return json::object();
        }

        if (http_code != 200) {
            std::cerr << "[ERROR] HTTP error: " << http_code << ", response: " << response << std::endl;
            return json::object();
        }

        std::cout << "[DEBUG] Response: " << response << std::endl;

        try {
            return json::parse(response);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] JSON parse error: " << e.what() << std::endl;
            return json::object();
        }
    }

    uint64_t blockNumber() {
        auto resp = rpcCall("eth_blockNumber", json::array());
        if (resp.contains("result")) {
            std::string hex = resp["result"].get<std::string>();
            return std::stoull(hex, nullptr, 16);
        }
        return 0;
    }

    json getBalance(const std::string& address) {
        auto resp = rpcCall("eth_getBalance", json::array({address, "latest"}));
        if (resp.contains("result")) {
            std::string hex = resp["result"].get<std::string>();
            double eth = std::stoull(hex, nullptr, 16) / 1e18;
            return {{"balance", eth}};
        }
        return json::object();
    }

    // 获取账户列表
    json getAccounts() {
        return rpcCall("eth_accounts", json::array());
    }

    // 解锁账户
    json unlockAccount(const std::string& address, const std::string& password, int duration = 300) {
        return rpcCall("personal_unlockAccount", json::array({address, password, duration}));
    }

    // 发送交易
    json sendTransaction(const json& tx) {
        return rpcCall("eth_sendTransaction", json::array({tx}));
    }

    // 获取交易信息
    json getTransactionByHash(const std::string& txHash) {
        return rpcCall("eth_getTransactionByHash", json::array({txHash}));
    }

    // 启动挖矿
    json startMining(int threads = 1) {
        return rpcCall("miner_start", json::array({threads}));
    }

    // 停止挖矿
    json stopMining() {
        return rpcCall("miner_stop", json::array());
    }

    // 获取挖矿状态
    json isMining() {
        return rpcCall("eth_mining", json::array());
    }

    // 获取节点信息
    json getPeers() {
        return rpcCall("admin_peers", json::array());
    }

    // 获取 Gas 价格
    json getGasPrice() {
        return rpcCall("eth_gasPrice", json::array());
    }

    // 获取交易收据
    json getTransactionReceipt(const std::string& txHash) {
        return rpcCall("eth_getTransactionReceipt", json::array({txHash}));
    }

    // 获取链 ID
    json getChainId() {
        return rpcCall("eth_chainId", json::array());
    }

    // 获取协议版本
    json getProtocolVersion() {
        return rpcCall("eth_protocolVersion", json::array());
    }

    // 获取同步状态
    json getSyncing() {
        return rpcCall("eth_syncing", json::array());
    }

    // 获取 Coinbase（矿工地址）
    json getCoinbase() {
        return rpcCall("eth_coinbase", json::array());
    }

    // 获取 Hashrate
    json getHashrate() {
        return rpcCall("eth_hashrate", json::array());
    }

    // 获取工作量证明
    json getWork() {
        return rpcCall("eth_getWork", json::array());
    }

    // 提交工作量证明
    json submitWork(const std::string& nonce, const std::string& header, const std::string& mix) {
        return rpcCall("eth_submitWork", json::array({nonce, header, mix}));
    }

    // 提交 Hashrate
    json submitHashrate(const std::string& hashrate, const std::string& id) {
        return rpcCall("eth_submitHashrate", json::array({hashrate, id}));
    }

    // 获取存储
    json getStorageAt(const std::string& address, const std::string& position, const std::string& block = "latest") {
        return rpcCall("eth_getStorageAt", json::array({address, position, block}));
    }

    // 获取代码
    json getCode(const std::string& address, const std::string& block = "latest") {
        return rpcCall("eth_getCode", json::array({address, block}));
    }

    // 调用合约（只读）
    json call(const json& tx, const std::string& block = "latest") {
        return rpcCall("eth_call", json::array({tx, block}));
    }

    // 预估 Gas
    json estimateGas(const json& tx) {
        return rpcCall("eth_estimateGas", json::array({tx}));
    }

    // 获取过滤器日志
    json getLogs(const json& filter) {
        return rpcCall("eth_getLogs", json::array({filter}));
    }

    // 获取过滤器变更
    json getFilterChanges(const std::string& filterId) {
        return rpcCall("eth_getFilterChanges", json::array({filterId}));
    }

    // 获取过滤器日志（按 ID）
    json getFilterLogs(const std::string& filterId) {
        return rpcCall("eth_getFilterLogs", json::array({filterId}));
    }

    // 创建过滤器（按区块）
    json newBlockFilter() {
        return rpcCall("eth_newBlockFilter", json::array());
    }

    // 创建过滤器（按待处理交易）
    json newPendingTransactionFilter() {
        return rpcCall("eth_newPendingTransactionFilter", json::array());
    }

    // 创建过滤器（按条件）
    json newFilter(const json& filter) {
        return rpcCall("eth_newFilter", json::array({filter}));
    }

    // 卸载过滤器
    json uninstallFilter(const std::string& filterId) {
        return rpcCall("eth_uninstallFilter", json::array({filterId}));
    }

    // 获取日志（通过过滤器）
    json getLogsByFilter(const json& filter) {
        return rpcCall("eth_getLogs", json::array({filter}));
    }

    // 获取区块信息（按号码）
    json getBlockByNumber(const std::string& block, bool full = false) {
        return rpcCall("eth_getBlockByNumber", json::array({block, full}));
    }

    // 获取区块信息（按哈希）
    json getBlockByHash(const std::string& blockHash, bool full = false) {
        return rpcCall("eth_getBlockByHash", json::array({blockHash, full}));
    }

    // 获取交易计数
    json getTransactionCount(const std::string& address, const std::string& block = "latest") {
        return rpcCall("eth_getTransactionCount", json::array({address, block}));
    }

    // 发送原始交易
    json sendRawTransaction(const std::string& rawTx) {
        return rpcCall("eth_sendRawTransaction", json::array({rawTx}));
    }

    // 签名
    json sign(const std::string& address, const std::string& data) {
        return rpcCall("eth_sign", json::array({address, data}));
    }

    // 签名交易
    json signTransaction(const json& tx) {
        return rpcCall("eth_signTransaction", json::array({tx}));
    }

    // 恢复签名
    json recover(const std::string& message, const std::string& signature) {
        return rpcCall("personal_ecRecover", json::array({message, signature}));
    }

    // 导入原始私钥
    json importRawKey(const std::string& privateKey, const std::string& password) {
        return rpcCall("personal_importRawKey", json::array({privateKey, password}));
    }

    // 列出账户（包括私钥状态）
    json listAccounts() {
        return rpcCall("personal_listAccounts", json::array());
    }

    // 锁定账户
    json lockAccount(const std::string& address) {
        return rpcCall("personal_lockAccount", json::array({address}));
    }

    // 发送交易（简化版）
    json sendSimpleTransaction(const std::string& from, const std::string& to, const std::string& value, const std::string& data = "0x") {
        json tx;
        tx["from"] = from;
        tx["to"] = to;
        tx["value"] = value;
        tx["data"] = data;
        return sendTransaction(tx);
    }

    // 发送交易（带 Gas 设置）
    json sendTransactionWithGas(const std::string& from, const std::string& to, 
                                const std::string& value, const std::string& gasPrice, 
                                const std::string& gasLimit, const std::string& data = "0x") {
        json tx;
        tx["from"] = from;
        tx["to"] = to;
        tx["value"] = value;
        tx["gasPrice"] = gasPrice;
        tx["gas"] = gasLimit;
        tx["data"] = data;
        return sendTransaction(tx);
    }
};



using json = nlohmann::json;

static std::string g_k8s_token;
static std::string g_k8s_api_server = "https://192.168.138.150:7443";

// ===== LDAP 认证函数 =====
bool ldap_auth(const std::string& username, const std::string& password) {
    std::string ldap_uri = "ldap://openldap:389";
    std::string user_dn = "uid=" + username + ",dc=example,dc=com";
    LDAP* ld = nullptr;
    int rc = ldap_initialize(&ld, ldap_uri.c_str());
    if (rc != LDAP_SUCCESS) { std::cerr << "LDAP init failed: " << ldap_err2string(rc) << std::endl; return false; }
    int version = LDAP_VERSION3;
    ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
    struct berval cred;
    cred.bv_val = (char*)password.c_str();
    cred.bv_len = password.length();
    rc = ldap_sasl_bind_s(ld, user_dn.c_str(), LDAP_SASL_SIMPLE, &cred, NULL, NULL, NULL);
    ldap_unbind_ext_s(ld, nullptr, nullptr);
    return (rc == LDAP_SUCCESS);
}

// ===== 从数据库获取用户信息 =====
struct UserInfo {
    int id = 0;
    std::string username;
    std::string fullname;
    std::string email;
    std::string role;
    bool found = false;
};

UserInfo getUserFromDB(const std::string& username) {
    UserInfo info;
    try {
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        pqxx::work txn(conn);
        auto r = txn.exec_params(
            "SELECT id, username, fullname, email, role FROM users WHERE username=$1",
            username
        );
        if (!r.empty()) {
            info.id       = r[0]["id"].as<int>();
            info.username = r[0]["username"].as<std::string>();
            info.fullname = r[0]["fullname"].is_null() ? username : r[0]["fullname"].as<std::string>();
            info.email    = r[0]["email"].is_null() ? "" : r[0]["email"].as<std::string>();
            info.role     = r[0]["role"].is_null() ? "user" : r[0]["role"].as<std::string>();
            info.found    = true;
        }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "getUserFromDB error: " << e.what() << std::endl;
    }
    return info;
}

void upsertUserToDB(const std::string& username) {
    try {
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO users (username, fullname, email, role, ldap_dn, last_login) "
            "VALUES ($1, $1, '', 'user', $2, NOW()) "
            "ON CONFLICT (username) DO UPDATE SET last_login=NOW()",
            username,
            "uid=" + username + ",dc=example,dc=com"
        );
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "upsertUserToDB error: " << e.what() << std::endl;
    }
}












// ===== 文件读取 =====
std::string readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return "<h1>Error: index.html not found</h1>";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ========== Ceph 文件系统 API ==========

// Ceph 存储根目录（在容器内）
#define CEPH_STORAGE_ROOT "/ceph-storage"

// 初始化 Ceph 存储目录
void initCephStorage() {
    mkdir(CEPH_STORAGE_ROOT, 0777);
    mkdir((std::string(CEPH_STORAGE_ROOT) + "/data").c_str(), 0777);
}



// ===== Redis 函数 =====
redisContext* redis_conn = nullptr;

void initRedis() {
    redis_conn = redisConnect("redis", 6379);
    if (redis_conn == nullptr || redis_conn->err) {
        std::cerr << "Redis 连接失败" << std::endl;
    } else {
        std::cout << "✅ Redis 连接成功" << std::endl;
    }
}

void refreshImageCacheToDB() {
    try {
        // 创建数据库连接
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        
        // 先确保表存在（包含所有需要的字段）
        {
            pqxx::work txn(conn);
            txn.exec("CREATE TABLE IF NOT EXISTS harbor_image_cache ("
                     "id SERIAL PRIMARY KEY,"
                     "image_name VARCHAR(255) NOT NULL,"
                     "tag VARCHAR(255) NOT NULL,"
                     "full_path VARCHAR(512) NOT NULL,"
                     "project VARCHAR(255),"
                     "pull_count INTEGER DEFAULT 0,"
                     "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                     "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                     "UNIQUE(image_name, tag))");
            txn.commit();
        }

        std::string url = "http://192.168.138.139:30002/api/v2.0/projects/library/repositories?page_size=100";
        std::string response;

        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "Failed to initialize CURL" << std::endl;
            return;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            return;
        }

        auto repos = json::parse(response);
        
        // 开始数据库事务
        pqxx::work txn(conn);
        
        if (repos.is_array()) {
            for (const auto& repo : repos) {
                std::string repoName = repo["name"];
                std::string imageName = repoName.find('/') != std::string::npos ? 
                                        repoName.substr(repoName.find('/') + 1) : repoName;
                std::string project = "library";
                int pullCount = repo.contains("pull_count") ? repo["pull_count"].get<int>() : 0;

                // 获取 tags
                std::string artUrl = "http://192.168.138.139:30002/api/v2.0/projects/library/repositories/" + 
                                     imageName + "/artifacts";
                std::string artResp;
                CURL* curl2 = curl_easy_init();
                if (curl2) {
                    curl_easy_setopt(curl2, CURLOPT_URL, artUrl.c_str());
                    curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, WriteCallback);
                    curl_easy_setopt(curl2, CURLOPT_WRITEDATA, &artResp);
                    curl_easy_setopt(curl2, CURLOPT_HTTPHEADER, headers);
                    curl_easy_setopt(curl2, CURLOPT_TIMEOUT, 30L);
                    curl_easy_perform(curl2);
                    curl_easy_cleanup(curl2);
                }

                auto artifacts = json::parse(artResp);
                if (artifacts.is_array()) {
                    for (const auto& art : artifacts) {
                        if (art.contains("tags") && art["tags"].is_array()) {
                            for (const auto& tag : art["tags"]) {
                                std::string tagName = tag["name"];
                                std::string fullPath = "192.168.138.139:30002/" + repoName + ":" + tagName;

                                // UPSERT
                                std::string sql = "INSERT INTO harbor_image_cache "
                                                 "(image_name, tag, full_path, project, pull_count, updated_at) "
                                                 "VALUES ('" + txn.esc(imageName) + "', '" + txn.esc(tagName) + "', '" 
                                                 + txn.esc(fullPath) + "', '" + txn.esc(project) + "', " 
                                                 + std::to_string(pullCount) + ", NOW()) "
                                                 "ON CONFLICT (image_name, tag) DO UPDATE SET "
                                                 "full_path = EXCLUDED.full_path, "
                                                 "pull_count = EXCLUDED.pull_count, "
                                                 "updated_at = NOW()";
                                txn.exec(sql);
                            }
                        }
                    }
                }
            }
        }
        
        txn.commit();
        std::cout << "✅ Harbor 镜像已缓存到数据库" << std::endl;
        
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        
    } catch (const std::exception& e) {
        std::cerr << "DB cache error: " << e.what() << std::endl;
    }
}

void cacheRefreshThread() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::minutes(30));
        refreshImageCacheToDB();
    }
}

// ===== 数据库函数 =====
std::string getDbConfig() {
    try {
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        pqxx::work txn(conn);
        pqxx::result res = txn.exec("SELECT name, host, port, path FROM component_config ORDER BY id");
        json config = json::array();
        for (const auto& row : res) {
            json item;
            item["name"] = row["name"].c_str();
            item["host"] = row["host"].c_str();
            item["port"] = row["port"].c_str();
            item["path"] = row["path"].c_str();
            config.push_back(item);
        }
        return config.dump();
    } catch (const std::exception& e) {
        std::cerr << "DB error: " << e.what() << std::endl;
        return "[]";
    }
}

void saveDbConfig(const std::string& jsonStr) {
    try {
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        pqxx::work txn(conn);
        auto config = json::parse(jsonStr);
        txn.exec("DELETE FROM component_config");
        for (const auto& item : config) {
            std::string sql = "INSERT INTO component_config (name, host, port, path) VALUES ('" +
                std::string(item["name"]) + "', '" +
                std::string(item["host"]) + "', '" +
                std::string(item["port"]) + "', '" +
                std::string(item.value("path", "/")) + "')";
            txn.exec(sql);
        }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "DB save error: " << e.what() << std::endl;
    }
}

// ===== K8s API =====
std::string k8sRequestRaw(const std::string& method, const std::string& path, const std::string& body = "") {
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
    if (method == "GET") curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    else if (method == "DELETE") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    else if (method == "POST") { curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length()); }
    else if (method == "PATCH") {
    	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    }
    curl_easy_perform(curl);
    std::cout << "[DEBUG] k8sRequestRaw url: " << url << std::endl;
    std::cout << "[DEBUG] k8sRequestRaw response length: " << response.length() << std::endl;
    if (response.length() > 0) {
        std::cout << "[DEBUG] k8sRequestRaw response preview: " << response.substr(0, 200) << std::endl;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return response;
}


// 辅助函数：转义 JSON 字符串中的特殊字符
std::string escapeJsonString(const std::string& input) {
    std::string output;
    for (char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += c; break;
        }
    }
    return output;
}


std::string base64_encode(const std::string& input) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    int in_len = input.size();
    const unsigned char* bytes_to_encode = reinterpret_cast<const unsigned char*>(input.c_str());
    
    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; i < 4; i++) {
                result += base64_chars[char_array_4[i]];
            }
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) {
            char_array_3[j] = '\0';
        }
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (int j = 0; j < i + 1; j++) {
            result += base64_chars[char_array_4[j]];
        }
        while (i++ < 3) {
            result += '=';
        }
    }
    return result;
}



// 解析 multipart/form-data 的辅助函数
std::string extract_boundary(const std::string& content_type) {
    size_t pos = content_type.find("boundary=");
    if (pos == std::string::npos) return "";
    return "--" + content_type.substr(pos + 9);
}

// 从 multipart 中提取文件内容
bool parse_multipart(const std::string& body, const std::string& boundary, 
                     std::string& filename, std::string& file_content,
                     std::map<std::string, std::string>& form_fields) {
    size_t pos = 0;
    size_t boundary_len = boundary.length();
    
    while (true) {
        size_t start = body.find(boundary, pos);
        if (start == std::string::npos) break;
        start += boundary_len;
        
        // 查找头部结束位置
        size_t header_end = body.find("\r\n\r\n", start);
        if (header_end == std::string::npos) break;
        
        std::string headers = body.substr(start, header_end - start);
        
        // 解析 Content-Disposition
        size_t name_pos = headers.find("name=\"");
        std::string field_name;
        if (name_pos != std::string::npos) {
            name_pos += 6;
            size_t name_end = headers.find("\"", name_pos);
            if (name_end != std::string::npos) {
                field_name = headers.substr(name_pos, name_end - name_pos);
            }
        }
        
        // 检查是否是文件
        size_t filename_pos = headers.find("filename=\"");
        bool is_file = (filename_pos != std::string::npos);
        
        size_t data_start = header_end + 4;
        size_t data_end = body.find(boundary, data_start);
        if (data_end == std::string::npos) break;
        
        std::string data = body.substr(data_start, data_end - data_start - 2); // 去掉末尾的\r\n
        
        if (is_file) {
            filename_pos += 10;
            size_t filename_end = headers.find("\"", filename_pos);
            if (filename_end != std::string::npos) {
                filename = headers.substr(filename_pos, filename_end - filename_pos);
                file_content = data;
            }
        } else {
            form_fields[field_name] = data;
        }
        
        pos = data_end;
    }
    
    return !file_content.empty();
}



// GitLab 配置
std::string gitlab_url = "http://192.168.138.148:80";
std::string gitlab_token = "glpat-3fK8O6NRgfD-ySW6_pionG86MQp1OjQH.01.0w09svvjp";  // 从配置文件或环境变量读取

// 通用代理函数
std::string gitlab_proxy(const std::string& method, const std::string& path, const std::string& body = "") {
    CURL* curl = curl_easy_init();
    std::string response;
    
    std::string url = gitlab_url + "/api/v4" + path;
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, ("PRIVATE-TOKEN: " + gitlab_token).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // 5秒超时
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);  // 3秒连接超时




    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return response;
}





std::string getCurrentUser(const crow::request& req) {
    // 尝试从 Authorization header 获取
    std::string auth = req.get_header_value("Authorization");
    if (!auth.empty() && auth.find("Bearer ") == 0) {
        std::string token = auth.substr(7);
        if (token.find("token-") == 0) {
            return token.substr(6);
        }
    }
    // 默认返回 admin（临时方案）
    return "admin";
}

// 获取客户端 IP
std::string getClientIP(const crow::request& req) {
    std::string ip = req.get_header_value("X-Forwarded-For");
    if (ip.empty()) {
        ip = req.get_header_value("X-Real-IP");
    }
    if (ip.empty()) {
        ip = req.remote_ip_address;
    }
    return ip;
}

// 记录操作日志
void logOperation(const crow::request& req, 
                  const std::string& operation_type,
                  const std::string& operation_module,
                  const std::string& api_path,
                  int response_status,
                  int duration_ms,
                  const std::string& request_params = "") {
    try {
        pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
        pqxx::work txn(conn);
        
        std::string user_name = getCurrentUser(req);
        std::string client_ip = getClientIP(req);
        std::string method = crow::method_name(req.method);  // 修复这一行
        
        // 截取过长的参数
        std::string params = request_params;
        if (params.empty() && !req.body.empty()) {
            params = req.body;
        }
        if (params.length() > 1000) {
            params = params.substr(0, 1000) + "...";
        }
        
        std::string sql = "INSERT INTO operation_logs "
                         "(user_name, operation_type, operation_module, api_path, method, "
                         "request_params, response_status, client_ip, duration_ms) "
                         "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)";
        
        txn.exec_params(sql, 
                       user_name,
                       operation_type,
                       operation_module,
                       api_path,
                       method,
                       params,
                       response_status,
                       client_ip,
                       duration_ms);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 记录操作日志失败: " << e.what() << std::endl;
    }
}




// 日志包装器
template<typename F>
auto withLogging(F&& func, const std::string& module, const std::string& operation_type) {
    return [=](const crow::request& req) mutable {
        auto start = std::chrono::steady_clock::now();
        std::string request_params = req.body;
        if (request_params.length() > 1000) {
            request_params = request_params.substr(0, 1000) + "...";
        }

        auto response = func(req);

        auto end = std::chrono::steady_clock::now();
        int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        logOperation(req, operation_type, module, req.url, response.code, duration_ms, request_params);

        return response;
    };
}




// 在文件开头添加 getNodeIP 函数
std::string getNodeIP(const std::string& nodeName) {
    if (nodeName.empty()) return "";
    std::string cmd = "kubectl get node " + nodeName + " -o jsonpath='{.status.addresses[?(@.type==\"InternalIP\")].address}' 2>/dev/null";
    std::string result = "";
    char buffer[256];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        pclose(pipe);
    }
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    return result;
}










// 读取 SQLite 索引（复用 chain_index_v4.db）
std::string getRecordsFromDB() {
    sqlite3* db;
    std::string result = "[";
    if (sqlite3_open("/root/chain_index_v4.db", &db) == SQLITE_OK) {
        const char* sql = "SELECT block_number, type, detail, tx_hash FROM records ORDER BY block_number ASC";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            bool first = true;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) result += ",";
                first = false;
                int block = sqlite3_column_int(stmt, 0);
                const char* type = (const char*)sqlite3_column_text(stmt, 1);
                const char* detail = (const char*)sqlite3_column_text(stmt, 2);
                const char* hash = (const char*)sqlite3_column_text(stmt, 3);
                char buf[1024];
                snprintf(buf, sizeof(buf), R"({"block":%d,"type":"%s","detail":"%s","txHash":"%s"})",
                         block, type ? type : "", detail ? detail : "", hash ? hash : "");
                result += buf;
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    result += "]";
    return result;
}














std::string safeGetString(const json& obj, const std::string& key, const std::string& defaultValue = "") {
    try { if (obj.contains(key) && !obj[key].is_null()) return obj[key].get<std::string>(); } catch (...) {}
    return defaultValue;
}

int safeGetInt(const json& obj, const std::string& key, int defaultValue = 0) {
    try { if (obj.contains(key) && !obj[key].is_null()) return obj[key].get<int>(); } catch (...) {}
    return defaultValue;
}

// ===== main =====
int main() {

// 初始化数据库
sqlite3* db;
if (sqlite3_open("/root/chain_index_v4.db", &db) == SQLITE_OK) {
    const char* sql = "CREATE TABLE IF NOT EXISTS records (id INTEGER PRIMARY KEY, block_number INTEGER, type TEXT, detail TEXT, tx_hash TEXT);";
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
}    

	const char* tokenEnv = std::getenv("K8S_TOKEN");
    if (tokenEnv != nullptr && strlen(tokenEnv) > 0) g_k8s_token = tokenEnv;
    else { std::cout << "❌ K8S_TOKEN not found!" << std::endl; return 1; }
    const char* apiServer = std::getenv("K8S_API_SERVER");
    if (apiServer != nullptr && strlen(apiServer) > 0) g_k8s_api_server = apiServer;
    std::cout << "K8S_API_SERVER: " << g_k8s_api_server << std::endl;









    crow::App<> app;








// 区块链状态
CROW_ROUTE(app, "/api/blockchain/status").methods("GET"_method)([](const crow::request& req) {
    auto start = std::chrono::steady_clock::now();
    GethClient client("http://10.244.145.16:8545");
    uint64_t height = client.blockNumber();
    // 检查挖矿状态（eth_mining）
    auto miningResp = client.rpcCall("eth_mining", json::array());
    bool mining = miningResp.contains("result") && miningResp["result"].get<bool>();
    // 节点数可以通过 admin_peers 获取，或固定为3
    json resp = {{"height", height}, {"mining", mining}, {"peers", 1}};
    auto end = std::chrono::steady_clock::now();
    int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    logOperation(req, "QUERY", "BLOCKCHAIN", req.url, 200, duration_ms);
    return crow::response(resp.dump());
});






// 获取账户列表
CROW_ROUTE(app, "/api/blockchain/accounts").methods("GET"_method)([](const crow::request& req) {
    GethClient client("http://10.244.145.16:8545");
    auto resp = client.getAccounts();
    if (resp.contains("result")) {
        json data = {{"accounts", resp["result"]}};
        return crow::response(data.dump());
    }
    return crow::response(500, R"({"error":"Failed to fetch accounts"})");
});





CROW_ROUTE(app, "/api/blockchain/unlock").methods("POST"_method)([](const crow::request& req) {
    auto body = json::parse(req.body);
    std::string address = body.value("address", "");
    std::string password = body.value("password", "");
    if (address.empty() || password.empty())
        return crow::response(400, R"({"error":"address and password required"})");
    GethClient client("http://10.244.145.16:8545");
    auto resp = client.rpcCall("personal_unlockAccount", {address, password, 300});
    std::cout << "[DEBUG] unlock response: " << resp.dump() << std::endl;  // 加这行
    if (resp.contains("result") && resp["result"].get<bool>()) {
        return crow::response(R"({"success":true})");
    }
    return crow::response(500, R"({"error":"Unlock failed"})");
});


CROW_ROUTE(app, "/api/blockchain/send").methods("POST"_method)([](const crow::request& req) {
    auto body = json::parse(req.body);
    std::string from = body.value("from", "");
    std::string to = body.value("to", from);
    std::string data = body.value("data", "");
    if (from.empty() || data.empty())
        return crow::response(400, R"({"error":"from and data required"})");

    // 转十六进制
    std::string hex;
    for (unsigned char c : data) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        hex += buf;
    }
    std::string hexData = "0x" + hex;

    // 构造交易
    json tx = {
        {"from", from},
        {"to", to},
        {"data", hexData},
        {"value", "0x0"}
    };

    GethClient client("http://10.244.145.16:8545");
    
    // 修复：显式构造 JSON 数组
    json params = json::array();
    params.push_back(tx);
    
    auto resp = client.rpcCall("eth_sendTransaction", params);
    if (resp.contains("result")) {
        std::string txHash = resp["result"].get<std::string>();
        return crow::response(R"({"txHash":")" + txHash + R"("})");
    }
    
    // 打印错误信息便于调试
    std::string errorMsg = "Send failed";
    if (resp.contains("error")) {
        errorMsg = resp["error"].dump();
    }
    return crow::response(500, R"({"error":")" + errorMsg + R"("})");
});


// 查询交易
CROW_ROUTE(app, "/api/blockchain/tx/<string>").methods("GET"_method)([](const std::string& hash) {
    GethClient client("http://10.244.145.16:8545");
    auto resp = client.rpcCall("eth_getTransactionByHash", {hash});
    if (resp.contains("result") && !resp["result"].is_null()) {
        json tx = resp["result"];
        json result = {
            {"blockNumber", tx.value("blockNumber", "0x0")},
            {"from", tx.value("from", "")},
            {"to", tx.value("to", "")},
            {"input", tx.value("input", "")}
        };
        return crow::response(result.dump());
    }
    return crow::response(404, R"({"error":"Transaction not found"})");
});

// 查询余额
CROW_ROUTE(app, "/api/blockchain/balance/<string>").methods("GET"_method)([](const std::string& address) {
    GethClient client("http://10.244.145.16:8545");
    auto balance = client.getBalance(address);
    if (!balance.empty()) {
        return crow::response(balance.dump());
    }
    return crow::response(500, R"({"error":"Balance query failed"})");
});

// 启动挖矿
CROW_ROUTE(app, "/api/blockchain/mine/start").methods("POST"_method)([](const crow::request& req) {
    auto body = json::parse(req.body);
    int threads = body.value("threads", 1);
    GethClient client("http://10.244.145.16:8545");
    auto resp = client.rpcCall("miner_start", {threads});
    if (resp.contains("result")) {
        return crow::response(R"({"success":true})");
    }
    return crow::response(500, R"({"error":"Start mining failed"})");
});

// 停止挖矿
CROW_ROUTE(app, "/api/blockchain/mine/stop").methods("POST"_method)([]() {
    GethClient client("http://10.244.145.16:8545");
    auto resp = client.rpcCall("miner_stop", json::array());
    if (resp.contains("result")) {
        return crow::response(R"({"success":true})");
    }
    return crow::response(500, R"({"error":"Stop mining failed"})");
});

// 获取本地索引记录（list）
CROW_ROUTE(app, "/api/blockchain/records").methods("GET"_method)([]() {
    std::string records = getRecordsFromDB();
    return crow::response(records);
});






CROW_ROUTE(app, "/api/blockchain/write").methods("POST"_method)([](const crow::request& req) {
    auto body = json::parse(req.body);
    std::string text = body.value("text", "");
    std::string from = body.value("from", "0xa861a779a3b81fbda87f6d859e03e4620a0df161");
    if (text.empty())
        return crow::response(400, R"({"error":"text required"})");

    std::string hex;
    for (unsigned char c : text) {
        char buf[3];
        sprintf(buf, "%02x", c);
        hex += buf;
    }
    std::string hexData = "0x" + hex;
    json tx = {{"from", from}, {"to", from}, {"data", hexData}, {"value", "0x0"}};

    GethClient client("http://10.244.145.16:8545");
    json params = json::array();
    params.push_back(tx);
    auto resp = client.rpcCall("eth_sendTransaction", params);
    
    if (resp.contains("result")) {
        std::string txHash = resp["result"].get<std::string>();
        
        // 获取当前区块号
        auto blockResp = client.rpcCall("eth_blockNumber", json::array());
        uint64_t blockNumber = 0;
        if (blockResp.contains("result")) {
            std::string hexBlock = blockResp["result"].get<std::string>();
            blockNumber = std::stoull(hexBlock, nullptr, 16);
        }
        
        // 写入 SQLite 数据库
        sqlite3* db;
        if (sqlite3_open("/root/chain_index_v4.db", &db) == SQLITE_OK) {
            std::string sql = "INSERT INTO records (block_number, type, detail, tx_hash) VALUES (" +
                              std::to_string(blockNumber) + ", '存证', '" + text + "', '" + txHash + "')";
            sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
            sqlite3_close(db);
        }
        
        return crow::response(R"({"txHash":")" + txHash + R"("})");
    }
    return crow::response(500, R"({"error":"Write failed"})");
});






    // 初始化 RAG
    init_rag_engine();

    // ========== RAG API 路由 ==========

    // 1. 获取 RAG 状态
    CROW_ROUTE(app, "/api/rag/status").methods("GET"_method)([]() {
        json resp;
        if (g_rag_engine) {
            resp["status"] = "ready";
            resp["knowledge_count"] = (int)g_rag_engine->knowledge_size();
        } else {
            resp["status"] = "not_initialized";
            resp["knowledge_count"] = 0;
        }
        return crow::response(resp.dump());
    });

    // 2. 初始化知识库
    CROW_ROUTE(app, "/api/rag/init").methods("POST"_method)([]() {
        if (!g_rag_engine) {
            init_rag_engine();
        }
        if (g_rag_engine) {
            g_rag_engine->init_k8s_knowledge();
            json resp;
            resp["success"] = true;
            resp["message"] = "知识库初始化完成";
            resp["count"] = (int)g_rag_engine->knowledge_size();
            return crow::response(resp.dump());
        }
        json resp;
        resp["success"] = false;
        resp["message"] = "RAG 引擎初始化失败";
        return crow::response(500, resp.dump());
    });

    // 3. 添加知识
    CROW_ROUTE(app, "/api/rag/add").methods("POST"_method)([](const crow::request& req) {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化";
            return crow::response(500, resp.dump());
        }
        try {
            auto body = json::parse(req.body);
            std::string text = body.value("text", "");
            if (text.empty()) {
                json resp;
                resp["success"] = false;
                resp["message"] = "text 不能为空";
                return crow::response(400, resp.dump());
            }
            std::unordered_map<std::string, std::string> meta;
            if (body.contains("metadata") && body["metadata"].is_object()) {
                // 修正：使用 nlohmann/json 的正确遍历方式
                for (auto it = body["metadata"].begin(); it != body["metadata"].end(); ++it) {
                    meta[it.key()] = it.value().get<std::string>();
                }
            }
            g_rag_engine->add_knowledge(text, meta);
            json resp;
            resp["success"] = true;
            resp["message"] = "知识添加成功";
            resp["total"] = (int)g_rag_engine->knowledge_size();
            return crow::response(resp.dump());
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    // 4. 搜索
    CROW_ROUTE(app, "/api/rag/search").methods("POST"_method)([](const crow::request& req) {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化";
            return crow::response(500, resp.dump());
        }
        try {
            auto body = json::parse(req.body);
            std::string query = body.value("query", "");
            int top_k = body.value("top_k", 5);
            if (query.empty()) {
                json resp;
                resp["success"] = false;
                resp["message"] = "query 不能为空";
                return crow::response(400, resp.dump());
            }
            auto results = g_rag_engine->search(query, top_k);
            json resp;
            resp["success"] = true;
            resp["results"] = json::array();
            for (const auto& r : results) {
                json item;
                item["id"] = r.id;
                item["text"] = r.text;
                item["score"] = r.score;
                json meta;
                for (const auto& kv : r.metadata) {
                    meta[kv.first] = kv.second;
                }
                item["metadata"] = meta;
                resp["results"].push_back(item);
            }
            return crow::response(resp.dump());
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    // 5. RAG 问答（核心接口）
    CROW_ROUTE(app, "/api/rag/ask").methods("POST"_method)([](const crow::request& req) {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化，请先调用 /api/rag/init";
            return crow::response(500, resp.dump());
        }
        try {
            auto body = json::parse(req.body);
            std::string query = body.value("query", "");
            int top_k = body.value("top_k", 3);
            if (query.empty()) {
                json resp;
                resp["success"] = false;
                resp["message"] = "query 不能为空";
                return crow::response(400, resp.dump());
            }
            
            auto start = std::chrono::steady_clock::now();
            std::string answer = g_rag_engine->ask(query, top_k);
            auto end = std::chrono::steady_clock::now();
            int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            
            // 同时获取检索到的来源
            auto results = g_rag_engine->search(query, top_k);
            
            json resp;
            resp["success"] = true;
            resp["answer"] = answer;
            resp["duration_ms"] = duration_ms;
            resp["sources"] = json::array();
            for (const auto& r : results) {
                json item;
                item["id"] = r.id;
                item["text"] = r.text;
                item["score"] = r.score;
                json meta;
                for (const auto& kv : r.metadata) {
                    meta[kv.first] = kv.second;
                }
                item["metadata"] = meta;
                resp["sources"].push_back(item);
            }
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });






    // 6. 获取知识列表（增强版）
    CROW_ROUTE(app, "/api/rag/list").methods("GET"_method)([]() {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化";
            return crow::response(500, resp.dump());
        }
        try {
            auto docs = g_rag_engine->get_all_knowledge();
            json resp;
            resp["success"] = true;
            resp["total"] = (int)docs.size();
            resp["data"] = json::array();
            for (const auto& doc : docs) {
                json item;
                item["id"] = doc.id;
                item["text"] = doc.text;
                json meta;
                for (const auto& kv : doc.metadata) {
                    meta[kv.first] = kv.second;
                }
                item["metadata"] = meta;
                resp["data"].push_back(item);
            }
            return crow::response(resp.dump());
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    // 8. 删除知识（按 ID）- 增强版
    CROW_ROUTE(app, "/api/rag/delete/<string>").methods("DELETE"_method)([](const std::string& id) {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化";
            return crow::response(500, resp.dump());
        }
        try {
            bool deleted = g_rag_engine->delete_knowledge(id);
            json resp;
            resp["success"] = deleted;
            resp["message"] = deleted ? "删除成功" : "未找到该知识条目";
            return crow::response(resp.dump());
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    // 9. 清空全部知识 - 增强版
    CROW_ROUTE(app, "/api/rag/clear").methods("POST"_method)([]() {
        if (!g_rag_engine) {
            json resp;
            resp["success"] = false;
            resp["message"] = "RAG 引擎未初始化";
            return crow::response(500, resp.dump());
        }
        try {
            g_rag_engine->clear_all_knowledge();
            json resp;
            resp["success"] = true;
            resp["message"] = "知识库已清空";
            resp["total"] = 0;
            return crow::response(resp.dump());
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["message"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });








    
    // ========== 开发工作空间 API ==========


    
    // 修改 list 接口
    CROW_ROUTE(app, "/api/dev-workspace/list").methods("GET"_method)([]() {
        try {
            std::string cmd = "kubectl get pods -n default -l app=dev-workspace -o json 2>/dev/null";
            std::string result = "";
            char buffer[4096];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    result += buffer;
                }
                pclose(pipe);
            }
    
            if (result.empty()) {
                json resp;
                resp["code"] = 0;
                resp["data"] = json::array();
                return crow::response(resp.dump());
            }
    
            auto pods = json::parse(result);
            json data = json::array();
    
            if (pods.contains("items")) {
                for (const auto& item : pods["items"]) {
                    json pod;
                    std::string podName = safeGetString(item["metadata"], "name");
                    std::string nodeName = safeGetString(item["spec"], "nodeName");
                    
                    pod["name"] = podName;
                    pod["status"] = safeGetString(item["status"], "phase");
                    pod["labels"] = item["metadata"].value("labels", json::object());
                    pod["ip"] = safeGetString(item["status"], "podIP");
                    pod["node"] = nodeName;
                    pod["created"] = safeGetString(item["metadata"], "creationTimestamp");
    
                    // 获取对应的 Service NodePort
                    std::string svc_cmd = "kubectl get svc " + podName + " -n default -o jsonpath='{.spec.ports[0].nodePort}' 2>/dev/null";
                    std::string svc_port = "";
                    FILE* svc_pipe = popen(svc_cmd.c_str(), "r");
                    if (svc_pipe) {
                        while (fgets(buffer, sizeof(buffer), svc_pipe) != NULL) {
                            svc_port += buffer;
                        }
                        pclose(svc_pipe);
                    }
                    svc_port.erase(std::remove(svc_port.begin(), svc_port.end(), '\n'), svc_port.end());
    
                    if (!svc_port.empty()) {
                        pod["nodePort"] = svc_port;
                        // 获取节点 IP
                        std::string nodeIP = getNodeIP(nodeName);
                        pod["accessUrl"] = "http://" + nodeIP + ":" + svc_port;
                    } else {
                        pod["nodePort"] = "";
                        pod["accessUrl"] = "";
                    }
    
                    data.push_back(pod);
                }
            }
    
            json resp;
            resp["code"] = 0;
            resp["data"] = data;
            return crow::response(resp.dump());
    
        } catch (const std::exception& e) {
            json resp;
            resp["code"] = -1;
            resp["message"] = e.what();
            return crow::response(500, resp.dump());
        }
    });
    
    




// ========== 开发工作空间 - 创建 ==========
CROW_ROUTE(app, "/api/dev-workspace/create").methods("POST"_method)([](const crow::request& req) {
    try {
        auto body = json::parse(req.body);

        std::string name = body.value("name", "");
        std::string type = body.value("type", "code-server");
        std::string user = body.value("user", "unknown");
        std::string cpu = body.value("cpu", "500m");
        std::string memory = body.value("memory", "1Gi");
        std::string storage = body.value("storage", "5Gi");
        std::string password = body.value("password", "123456");

        if (name.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "名称不能为空";
            return crow::response(400, resp.dump());
        }

        // 确定镜像和端口
        std::string image, container_name, mount_path;
        int port;
        if (type == "jupyter") {
            image = "192.168.138.139:30002/library/jupyterlab:latest";
            container_name = "jupyter";
            port = 8888;
            mount_path = "/home/jovyan/work";
        } else {
            image = "192.168.138.139:30002/library/code-server:latest";
            container_name = "code-server";
            port = 8080;
            mount_path = "/home/coder/project";
        }

        // 1. 创建 PVC
        std::string pvc_yaml = R"(
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: )" + name + R"(-pvc
  namespace: default
spec:
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: )" + storage + R"(
)";
        std::string create_pvc_cmd = "cat <<EOF | kubectl apply -f -\n" + pvc_yaml + "\nEOF";
        system(create_pvc_cmd.c_str());

        // 等待 PVC 绑定
        bool pvc_bound = false;
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::string check_pvc_cmd = "kubectl get pvc " + name + "-pvc -n default -o jsonpath='{.status.phase}' 2>/dev/null";
            std::string phase = "";
            FILE* pipe = popen(check_pvc_cmd.c_str(), "r");
            if (pipe) {
                char buffer[64];
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    phase += buffer;
                }
                pclose(pipe);
            }
            phase.erase(std::remove(phase.begin(), phase.end(), '\n'), phase.end());
            if (phase == "Bound") {
                pvc_bound = true;
                break;
            }
        }

        // 2. 创建 Pod
        std::string startup_script;
        if (type == "jupyter") {
            startup_script =
                "jupyter lab --ip=0.0.0.0 --port=" + std::to_string(port) +
                " --allow-root --no-browser --NotebookApp.token='" + password +
                "' --NotebookApp.password='' --notebook-dir=" + mount_path;
        } else {
            startup_script =
                "mkdir -p /home/coder/.config/code-server && "
                "echo 'bind-addr: 0.0.0.0:" + std::to_string(port) + "' > /home/coder/.config/code-server/config.yaml && "
                "echo 'auth: password' >> /home/coder/.config/code-server/config.yaml && "
                "echo 'password: " + password + "' >> /home/coder/.config/code-server/config.yaml && "
                "code-server /home/coder/project";
        }

        json pod = {
            {"apiVersion", "v1"},
            {"kind", "Pod"},
            {"metadata", {
                {"name", name},
                {"namespace", "default"},
                {"labels", {
                    {"app", "dev-workspace"},
                    {"dev-type", type},
                    {"dev-user", user}
                }}
            }},
            {"spec", {
                {"containers", json::array({
                    {
                        {"name", container_name},
                        {"image", image},
                        {"imagePullPolicy", "IfNotPresent"},
                        {"ports", json::array({{{"containerPort", port}}})},
                        {"command", json::array({"sh", "-c", startup_script})},
                        {"resources", {
                            {"requests", {{"cpu", cpu}, {"memory", memory}}},
                            {"limits", {{"cpu", cpu}, {"memory", memory}}}
                        }},
                        {"volumeMounts", json::array({
                            {{"name", "workspace-data"}, {"mountPath", mount_path}}
                        })}
                    }
                })},
                {"volumes", json::array({
                    {
                        {"name", "workspace-data"},
                        {"persistentVolumeClaim", {{"claimName", name + "-pvc"}}}
                    }
                })},
                {"restartPolicy", "Always"}
            }}
        };

        std::string pod_yaml = pod.dump();
        std::string create_pod_cmd = "cat <<EOF | kubectl apply -f -\n" + pod_yaml + "\nEOF";
        system(create_pod_cmd.c_str());

        // 3. 创建 Service
        json svc = {
            {"apiVersion", "v1"},
            {"kind", "Service"},
            {"metadata", {
                {"name", name},
                {"namespace", "default"},
                {"labels", {
                    {"app", "dev-workspace"},
                    {"dev-type", type},
                    {"dev-user", user}
                }}
            }},
            {"spec", {
                {"selector", {
                    {"app", "dev-workspace"},
                    {"dev-type", type}
                }},
                {"type", "NodePort"},
                {"ports", json::array({
                    {
                        {"name", "http"},
                        {"port", port},
                        {"targetPort", port}
                    }
                })}
            }}
        };

        std::string svc_yaml = svc.dump();
        std::string create_svc_cmd = "cat <<EOF | kubectl apply -f -\n" + svc_yaml + "\nEOF";
        system(create_svc_cmd.c_str());

        // 4. 获取 NodePort
        std::string node_port = "";
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::string get_port_cmd = "kubectl get svc " + name + " -n default -o jsonpath='{.spec.ports[0].nodePort}' 2>/dev/null";
            FILE* pipe = popen(get_port_cmd.c_str(), "r");
            if (pipe) {
                char buffer[128];
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    node_port += buffer;
                }
                pclose(pipe);
            }
            node_port.erase(std::remove(node_port.begin(), node_port.end(), '\n'), node_port.end());
            if (!node_port.empty()) break;
        }

        json resp;
        resp["code"] = 0;
        resp["message"] = "工作空间创建成功";
        resp["serviceName"] = name;
        if (!node_port.empty()) {
            resp["nodePort"] = node_port;
        }
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 创建开发工作空间异常: " << e.what() << std::endl;
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});


    // ========== 开发工作空间 - 创建 ==========
CROW_ROUTE(app, "/api/dev-workspace/create11").methods("POST"_method)([](const crow::request& req) {
    try {
        auto body = json::parse(req.body);

        std::string name = body.value("name", "");
        std::string type = body.value("type", "code-server");
        std::string user = body.value("user", "unknown");
        std::string cpu = body.value("cpu", "500m");
        std::string memory = body.value("memory", "1Gi");
        std::string storage = body.value("storage", "5Gi");
        std::string password = body.value("password", "123456");

        if (name.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "名称不能为空";
            return crow::response(400, resp.dump());
        }

        // 确定镜像和端口
        std::string image, container_name, mount_path;
        int port;
        if (type == "jupyter") {
            image = "192.168.138.139:30002/library/jupyterlab:latest";
            container_name = "jupyter";
            port = 8888;
            mount_path = "/home/jovyan/work";
        } else {
            image = "192.168.138.139:30002/library/code-server:latest";
            container_name = "code-server";
            port = 8080;
            mount_path = "/home/coder/project";
        }

        // 1. 创建 PVC
        std::string pvc_yaml = R"(
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: )" + name + R"(-pvc
  namespace: default
spec:
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: )" + storage + R"(
)";
        std::string create_pvc_cmd = "cat <<EOF | kubectl apply -f -\n" + pvc_yaml + "\nEOF";
        system(create_pvc_cmd.c_str());

// 2. 创建 Pod（使用配置文件方式设置密码）
std::string startup_script;
if (type == "jupyter") {
    startup_script =
        "jupyter lab --ip=0.0.0.0 --port=" + std::to_string(port) +
        " --allow-root --no-browser --NotebookApp.token='" + password +
        "' --NotebookApp.password='' --notebook-dir=" + mount_path;
} else {
    startup_script =
        "mkdir -p /home/coder/.config/code-server && "
        "echo 'bind-addr: 0.0.0.0:" + std::to_string(port) + "' > /home/coder/.config/code-server/config.yaml && "
        "echo 'auth: password' >> /home/coder/.config/code-server/config.yaml && "
        "echo 'password: " + password + "' >> /home/coder/.config/code-server/config.yaml && "
        "code-server /home/coder/project";
}

json pod = {
    {"apiVersion", "v1"},
    {"kind", "Pod"},
    {"metadata", {
        {"name", name},
        {"namespace", "default"},
        {"labels", {
            {"app", "dev-workspace"},
            {"dev-type", type},
            {"dev-user", user}
        }}
    }},
    {"spec", {
        {"containers", json::array({
            {
                {"name", container_name},
                {"image", image},
                {"imagePullPolicy", "IfNotPresent"},  // ✅ 添加这一行
                {"ports", json::array({{{"containerPort", port}}})},
                {"command", json::array({"sh", "-c", startup_script})},
                {"resources", {
                    {"requests", {{"cpu", cpu}, {"memory", memory}}},
                    {"limits", {{"cpu", cpu}, {"memory", memory}}}
                }},
                {"volumeMounts", json::array({
                    {{"name", "workspace-data"}, {"mountPath", mount_path}}
                })}
            }
        })},
        {"volumes", json::array({
            {
                {"name", "workspace-data"},
                {"persistentVolumeClaim", {{"claimName", name + "-pvc"}}}
            }
        })},
        {"restartPolicy", "Always"}
    }}
};


        // 3. 创建 Service（NodePort 类型）
        json svc = {
            {"apiVersion", "v1"},
            {"kind", "Service"},
            {"metadata", {
                {"name", name},
                {"namespace", "default"},
                {"labels", {
                    {"app", "dev-workspace"},
                    {"dev-type", type},
                    {"dev-user", user}
                }}
            }},
            {"spec", {
                {"selector", {
                    {"app", "dev-workspace"},
                    {"dev-type", type}
                }},
                {"type", "NodePort"},
                {"ports", json::array({
                    {
                        {"name", "http"},
                        {"port", port},
                        {"targetPort", port}
                    }
                })}
            }}
        };

        std::string svc_yaml = svc.dump();
        std::string create_svc_cmd = "cat <<EOF | kubectl apply -f -\n" + svc_yaml + "\nEOF";
        system(create_svc_cmd.c_str());

        // 4. 获取 NodePort
        std::string node_port = "";
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::string get_port_cmd = "kubectl get svc " + name + " -n default -o jsonpath='{.spec.ports[0].nodePort}' 2>/dev/null";
            FILE* pipe = popen(get_port_cmd.c_str(), "r");
            if (pipe) {
                char buffer[128];
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    node_port += buffer;
                }
                pclose(pipe);
            }
            node_port.erase(std::remove(node_port.begin(), node_port.end(), '\n'), node_port.end());
            if (!node_port.empty()) break;
        }

        json resp;
        resp["code"] = 0;
        resp["message"] = "工作空间创建成功";
        resp["serviceName"] = name;
        if (!node_port.empty()) {
            resp["nodePort"] = node_port;
        }
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 创建开发工作空间异常: " << e.what() << std::endl;
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});



    
        



// ========== 开发工作空间 - 创建 ==========
    
   // ========== 开发工作空间 - 删除（需要密码验证）==========
CROW_ROUTE(app, "/api/dev-workspace/deletei1/<string>").methods("DELETE"_method)([](const crow::request& req, const std::string& name) {
    try {
        // 检查请求体是否为空
        if (req.body.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请求体为空，请提供密码";
            return crow::response(400, resp.dump());
        }

        // 解析 JSON
        auto body = json::parse(req.body);
        std::string password = body.value("password", "");

        if (password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请输入删除密码";
            return crow::response(400, resp.dump());
        }

        // 获取 Pod 中配置的密码
        std::string get_password_cmd = "kubectl exec " + name + " -n default -- cat /home/coder/.config/code-server/config.yaml 2>/dev/null | grep password | awk '{print $2}'";
        std::string correct_password = "";
        char buffer[256];
        FILE* pipe = popen(get_password_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                correct_password += buffer;
            }
            pclose(pipe);
        }
        correct_password.erase(std::remove(correct_password.begin(), correct_password.end(), '\n'), correct_password.end());

        if (correct_password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "无法获取工作空间密码";
            return crow::response(500, resp.dump());
        }

        if (password != correct_password) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "密码错误，删除失败";
            return crow::response(403, resp.dump());
        }

        // 删除资源
        system(("kubectl delete pod " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete service " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete pvc " + name + "-pvc -n default --ignore-not-found=true 2>&1").c_str());

        json resp;
        resp["code"] = 0;
        resp["message"] = "删除成功";
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});




CROW_ROUTE(app, "/api/dev-workspace/delete/<string>").methods("DELETE"_method)([](const crow::request& req, const std::string& name) {
    try {
        std::string password;

        // 1. 从 body 获取密码
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                password = body.value("password", "");
            } catch (const std::exception& e) {
                std::cerr << "[WARN] 解析 body 失败: " << e.what() << std::endl;
            }
        }

        if (password.empty()) {
            password = req.get_header_value("X-Password");
        }

        password.erase(0, password.find_first_not_of(" \t\n\r\f\v"));
        password.erase(password.find_last_not_of(" \t\n\r\f\v") + 1);

        if (password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请输入删除密码";
            return crow::response(400, resp.dump());
        }

        // ✅ 获取 Pod 类型
        std::string get_type_cmd = "kubectl get pod " + name + " -n default -o jsonpath='{.metadata.labels.dev-type}' 2>/dev/null";
        std::string pod_type = "";
        FILE* type_pipe = popen(get_type_cmd.c_str(), "r");
        if (type_pipe) {
            char buffer[64];
            while (fgets(buffer, sizeof(buffer), type_pipe) != NULL) {
                pod_type += buffer;
            }
            pclose(type_pipe);
        }
        pod_type.erase(std::remove(pod_type.begin(), pod_type.end(), '\n'), pod_type.end());

        std::string correct_password = "";

        if (pod_type == "jupyter") {
            // JupyterLab：从环境变量或启动命令中获取 token
            std::string get_token_cmd = "kubectl get pod " + name + " -n default -o jsonpath='{.spec.containers[0].command}' 2>/dev/null | grep -o 'token=[^ ]*' | cut -d= -f2";
            FILE* token_pipe = popen(get_token_cmd.c_str(), "r");
            if (token_pipe) {
                char buffer[256];
                while (fgets(buffer, sizeof(buffer), token_pipe) != NULL) {
                    correct_password += buffer;
                }
                pclose(token_pipe);
            }
            correct_password.erase(std::remove(correct_password.begin(), correct_password.end(), '\n'), correct_password.end());
            correct_password.erase(std::remove(correct_password.begin(), correct_password.end(), '\''), correct_password.end());
        } else {
            // Code-Server：从 config.yaml 获取密码
            std::string get_password_cmd = "kubectl exec " + name + " -n default -- cat /home/coder/.config/code-server/config.yaml 2>/dev/null | grep '^password:' | awk -F': ' '{print $2}' | tr -d '\\r\\n'";
            FILE* pipe = popen(get_password_cmd.c_str(), "r");
            if (pipe) {
                char buffer[256];
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    correct_password += buffer;
                }
                pclose(pipe);
            }
            correct_password.erase(0, correct_password.find_first_not_of(" \t\n\r\f\v"));
            correct_password.erase(correct_password.find_last_not_of(" \t\n\r\f\v") + 1);
        }

        if (correct_password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "无法获取工作空间密码，请确认 Pod 状态";
            return crow::response(500, resp.dump());
        }

        if (password != correct_password) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "密码错误，删除失败";
            return crow::response(403, resp.dump());
        }

        // 删除资源
        system(("kubectl delete pod " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete service " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete pvc " + name + "-pvc -n default --ignore-not-found=true 2>&1").c_str());

        json resp;
        resp["code"] = 0;
        resp["message"] = "删除成功";
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 删除工作空间异常: " << e.what() << std::endl;
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});



CROW_ROUTE(app, "/api/dev-workspace/delete11/<string>").methods("DELETE"_method)([](const crow::request& req, const std::string& name) {
    try {
        std::string password;

        // 1. 先从 body 获取密码
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                password = body.value("password", "");
            } catch (const std::exception& e) {
                std::cerr << "[WARN] 解析 body 失败: " << e.what() << std::endl;
            }
        }

        // 2. 如果 body 没有，从 header 获取
        if (password.empty()) {
            password = req.get_header_value("X-Password");
        }

        // 3. trim 密码
        password.erase(0, password.find_first_not_of(" \t\n\r\f\v"));
        password.erase(password.find_last_not_of(" \t\n\r\f\v") + 1);

        if (password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请输入删除密码";
            return crow::response(400, resp.dump());
        }

        // ✅ 修复：正确提取密码（只匹配以 password: 开头的行）
        std::string get_password_cmd = "kubectl exec " + name + " -n default -- cat /home/coder/.config/code-server/config.yaml 2>/dev/null | grep '^password:' | awk -F': ' '{print $2}' | tr -d '\\r\\n'";
        std::string correct_password = "";
        char buffer[256];
        FILE* pipe = popen(get_password_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                correct_password += buffer;
            }
            pclose(pipe);
        }

        // trim 正确密码
        correct_password.erase(0, correct_password.find_first_not_of(" \t\n\r\f\v"));
        correct_password.erase(correct_password.find_last_not_of(" \t\n\r\f\v") + 1);

        if (correct_password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "无法获取工作空间密码，请确认 Pod 状态";
            return crow::response(500, resp.dump());
        }

        std::cout << "[INFO] 删除验证 - 输入的密码: '" << password << "', Pod 内密码: '" << correct_password << "'" << std::endl;

        if (password != correct_password) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "密码错误，删除失败";
            return crow::response(403, resp.dump());
        }

        system(("kubectl delete pod " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete service " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete pvc " + name + "-pvc -n default --ignore-not-found=true 2>&1").c_str());

        json resp;
        resp["code"] = 0;
        resp["message"] = "删除成功";
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 删除工作空间异常: " << e.what() << std::endl;
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});

// ========== 开发工作空间 - 删除 ==========
CROW_ROUTE(app, "/api/dev-workspace/delete33/<string>").methods("DELETE"_method)([](const crow::request& req, const std::string& name) {
    try {
        std::string password;

        // 1. 先从 body 获取密码
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                password = body.value("password", "");
            } catch (const std::exception& e) {
                std::cerr << "[WARN] 解析 body 失败: " << e.what() << std::endl;
            }
        }

        // 2. 如果 body 没有，从 header 获取
        if (password.empty()) {
            password = req.get_header_value("X-Password");
        }

        // 3. trim 密码（去除前后空格和换行符）
        password.erase(0, password.find_first_not_of(" \t\n\r\f\v"));
        password.erase(password.find_last_not_of(" \t\n\r\f\v") + 1);

        if (password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请输入删除密码";
            return crow::response(400, resp.dump());
        }

        // 获取 Pod 中配置的密码（用 tr 去除换行符）
        std::string get_password_cmd = "kubectl exec " + name + " -n default -- cat /home/coder/.config/code-server/config.yaml 2>/dev/null | grep password | awk '{print $2}' | tr -d '\\r\\n'";
        std::string correct_password = "";
        char buffer[256];
        FILE* pipe = popen(get_password_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                correct_password += buffer;
            }
            pclose(pipe);
        }

        // trim 正确密码
        correct_password.erase(0, correct_password.find_first_not_of(" \t\n\r\f\v"));
        correct_password.erase(correct_password.find_last_not_of(" \t\n\r\f\v") + 1);

        if (correct_password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "无法获取工作空间密码，请确认 Pod 状态";
            return crow::response(500, resp.dump());
        }

        std::cout << "[INFO] 删除验证 - 输入的密码: '" << password << "', Pod 内密码: '" << correct_password << "'" << std::endl;

        // 验证密码
        if (password != correct_password) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "密码错误，删除失败";
            return crow::response(403, resp.dump());
        }

        // 密码正确，执行删除
        std::cout << "[INFO] 密码验证成功，删除工作空间: " << name << std::endl;

        system(("kubectl delete pod " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete service " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete pvc " + name + "-pvc -n default --ignore-not-found=true 2>&1").c_str());

        json resp;
        resp["code"] = 0;
        resp["message"] = "删除成功";
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 删除工作空间异常: " << e.what() << std::endl;
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});


CROW_ROUTE(app, "/api/dev-workspace/delete2/<string>").methods("DELETE"_method)([](const crow::request& req, const std::string& name) {
    try {
        std::string password;

        // 1. 先从 body 获取
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                password = body.value("password", "");
            } catch (...) {}
        }

        // 2. 如果 body 没有，从 header 获取
        if (password.empty()) {
            password = req.get_header_value("X-Password");
        }

        if (password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "请输入删除密码";
            return crow::response(400, resp.dump());
        }

        // 获取 Pod 中配置的密码
        std::string get_password_cmd = "kubectl exec " + name + " -n default -- cat /home/coder/.config/code-server/config.yaml 2>/dev/null | grep password | awk '{print $2}'";
        std::string correct_password = "";
        char buffer[256];
        FILE* pipe = popen(get_password_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                correct_password += buffer;
            }
            pclose(pipe);
        }
        correct_password.erase(std::remove(correct_password.begin(), correct_password.end(), '\n'), correct_password.end());

        if (correct_password.empty()) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "无法获取工作空间密码";
            return crow::response(500, resp.dump());
        }

        if (password != correct_password) {
            json resp;
            resp["code"] = -1;
            resp["message"] = "密码错误，删除失败";
            return crow::response(403, resp.dump());
        }

        // 删除资源
        system(("kubectl delete pod " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete service " + name + " -n default --ignore-not-found=true 2>&1").c_str());
        system(("kubectl delete pvc " + name + "-pvc -n default --ignore-not-found=true 2>&1").c_str());

        json resp;
        resp["code"] = 0;
        resp["message"] = "删除成功";
        return crow::response(resp.dump());

    } catch (const std::exception& e) {
        json resp;
        resp["code"] = -1;
        resp["message"] = std::string(e.what());
        return crow::response(500, resp.dump());
    }
});




    // ========== GitLab 配置 ==========
    std::string gitlab_url = "http://192.168.138.148:80";
    std::string gitlab_token = "glpat-3fK8O6NRgfD-ySW6_pionG86MQp1OjQH.01.0w09svvjp";
    
    // GitLab 代理函数
    auto gitlab_proxy = [&](const std::string& method, const std::string& path, const std::string& body = "") -> std::string {
        CURL* curl = curl_easy_init();
        std::string response;
        
        std::string url = gitlab_url + "/api/v4" + path;
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, ("PRIVATE-TOKEN: " + gitlab_token).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        if (method == "GET") {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        
        return response;
    };
    
    // GitLab 路由 - 使用 [&] 捕获所有变量
    CROW_ROUTE(app, "/api/gitlab/projects").methods("GET"_method)([&]() {
        std::string response = gitlab_proxy("GET", "/projects?per_page=100");
        return crow::response(response);
    });
    
    CROW_ROUTE(app, "/api/gitlab/projects").methods("POST"_method)([&](const crow::request& req) {
        std::string response = gitlab_proxy("POST", "/projects", req.body);
        return crow::response(response);
    });
    
    CROW_ROUTE(app, "/api/gitlab/projects/<int>").methods("DELETE"_method)([&](int project_id) {
        std::string response = gitlab_proxy("DELETE", "/projects/" + std::to_string(project_id));
        return crow::response(response);
    });
    
    CROW_ROUTE(app, "/api/gitlab/user").methods("GET"_method)([&]() {
        std::string response = gitlab_proxy("GET", "/user");
        return crow::response(response);
    });
    
    CROW_ROUTE(app, "/api/gitlab/users").methods("GET"_method)([&]() {
        std::string response = gitlab_proxy("GET", "/users?per_page=100");
        return crow::response(response);
    });


        
    // 通用 GitLab 代理（支持任何路径）
    CROW_ROUTE(app, "/api/gitlab/proxy/<path>").methods("GET"_method)([&](const std::string& path) {
        std::string full_path = "/" + path;
        std::string response = gitlab_proxy("GET", full_path);
        return crow::response(response);
    });
    
    
    




    // ========== 首页 ==========
    CROW_ROUTE(app, "/1")([](){ return crow::response(readFile("index.html")); });

    // ========== 健康检查 ==========
    CROW_ROUTE(app, "/api/health1")([](){ json resp = {{"status","ok"},{"token_loaded",!g_k8s_token.empty()}}; return crow::response(resp.dump()); });

    CROW_ROUTE(app, "/api/harbor/cached-images1")
    ([](){
        try {
            pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
            pqxx::work txn(conn);
            pqxx::result res = txn.exec("SELECT image_name, tag, full_path, project, pull_count FROM harbor_image_cache ORDER BY image_name, tag");
            json result = json::array();
            for (const auto& row : res) {
                json item;
                item["name"] = row["image_name"].c_str();
                item["tag"] = row["tag"].c_str();
                item["full_path"] = row["full_path"].c_str();
                item["project"] = row["project"].c_str();
                item["pull_count"] = atoi(row["pull_count"].c_str());
                result.push_back(item);
            }
            return crow::response(result.dump());
        } catch (...) {
            return crow::response("[]");
        }
    });

    
    // ========== 首页 ==========
    CROW_ROUTE(app, "/")([](const crow::request& req) {
        auto start = std::chrono::steady_clock::now();
        auto response = crow::response(readFile("index.html"));
        auto end = std::chrono::steady_clock::now();
        int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        logOperation(req, "ACCESS", "PAGE", req.url, 200, duration_ms);
        return response;
    });
    
    // ========== 健康检查 ==========
    CROW_ROUTE(app, "/api/health")([](const crow::request& req) {
        auto start = std::chrono::steady_clock::now();
        json resp = {{"status","ok"},{"token_loaded",!g_k8s_token.empty()}};
        auto end = std::chrono::steady_clock::now();
        int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        logOperation(req, "CHECK", "HEALTH", req.url, 200, duration_ms);
        return crow::response(resp.dump());
    });
    
    // ========== Harbor 缓存镜像接口 ==========
    CROW_ROUTE(app, "/api/harbor/cached-images")([](const crow::request& req) {
        auto start = std::chrono::steady_clock::now();
        int status_code = 200;
        std::string result = "[]";
        try {
            pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
            pqxx::work txn(conn);
            pqxx::result res = txn.exec("SELECT image_name, tag, full_path, project, pull_count FROM harbor_image_cache ORDER BY image_name, tag");
            json result_json = json::array();
            for (const auto& row : res) {
                json item;
                item["name"] = row["image_name"].c_str();
                item["tag"] = row["tag"].c_str();
                item["full_path"] = row["full_path"].c_str();
                item["project"] = row["project"].c_str();
                item["pull_count"] = atoi(row["pull_count"].c_str());
                result_json.push_back(item);
            }
            result = result_json.dump();
        } catch (...) {
            status_code = 500;
            result = "[]";
        }
        auto end = std::chrono::steady_clock::now();
        int duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        logOperation(req, "QUERY", "HARBOR", req.url, status_code, duration_ms);
        return crow::response(result);
    });
    
    





    








    
    // 上传镜像到 Harbor
    CROW_ROUTE(app, "/api/harbor/upload-image").methods("POST"_method)([](const crow::request& req) {
        std::cout << "[INFO] 开始处理上传请求" << std::endl;
        
        // 获取 Content-Type
        std::string content_type = req.get_header_value("Content-Type");
        std::string boundary = extract_boundary(content_type);
        
        if (boundary.empty()) {
            json resp;
            resp["success"] = false;
            resp["error"] = "无效的 Content-Type";
            return crow::response(400, resp.dump());
        }
        
        // 解析 multipart 数据
        std::string filename;
        std::string file_content;
        std::map<std::string, std::string> form_fields;
        
        if (!parse_multipart(req.body, boundary, filename, file_content, form_fields)) {
            json resp;
            resp["success"] = false;
            resp["error"] = "解析上传文件失败";
            return crow::response(400, resp.dump());
        }
        
        std::string project = form_fields["project"];
        std::string image_name = form_fields["image_name"];
        std::string image_tag = form_fields["image_tag"];
        
        std::cout << "[INFO] 项目: " << project << std::endl;
        std::cout << "[INFO] 镜像名: " << image_name << std::endl;
        std::cout << "[INFO] 标签: " << image_tag << std::endl;
        std::cout << "[INFO] 文件名: " << filename << std::endl;
        std::cout << "[INFO] 文件大小: " << file_content.size() << " bytes" << std::endl;
        
        if (project.empty() || image_name.empty() || image_tag.empty() || file_content.empty()) {
            json resp;
            resp["success"] = false;
            resp["error"] = "缺少必要参数";
            return crow::response(400, resp.dump());
        }
        
        // 保存临时文件
        std::string temp_tar = "/tmp/" + image_name + "_" + std::to_string(time(nullptr)) + ".tar";
        std::ofstream ofs(temp_tar, std::ios::binary);
        if (!ofs.is_open()) {
            json resp;
            resp["success"] = false;
            resp["error"] = "无法创建临时文件";
            return crow::response(500, resp.dump());
        }
        ofs.write(file_content.c_str(), file_content.size());
        ofs.close();
        
        std::cout << "[INFO] 临时文件保存: " << temp_tar << std::endl;
        
        // 构建完整镜像名称
        std::string harbor_addr = "192.168.138.139:30002";
        std::string full_image = harbor_addr + "/" + project + "/" + image_name + ":" + image_tag;
        
        // 执行 docker 命令序列
        std::string output;
        char buffer[4096];
        
        // 1. 登录 Harbor
        std::string login_cmd = "docker login " + harbor_addr + " -u admin -p Harbor12345 2>&1";
        std::cout << "[INFO] 执行登录: " << login_cmd << std::endl;
        FILE* pipe = popen(login_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                output += buffer;
            }
            pclose(pipe);
        }
        std::cout << "[INFO] 登录输出: " << output << std::endl;
        
        // 2. 加载镜像
        output.clear();
        std::string load_cmd = "docker load -i " + temp_tar + " 2>&1";
        std::cout << "[INFO] 加载镜像: " << load_cmd << std::endl;
        pipe = popen(load_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                output += buffer;
            }
            pclose(pipe);
        }
        std::cout << "[INFO] 加载输出: " << output << std::endl;
        
        // 从输出中提取镜像 ID 或名称
        std::string loaded_image;
        size_t pos = output.find("Loaded image: ");
        if (pos != std::string::npos) {
            loaded_image = output.substr(pos + 14);
            loaded_image.erase(loaded_image.find_last_not_of(" \n\r\t") + 1);
        } else {
            pos = output.find("Loaded image ID: ");
            if (pos != std::string::npos) {
                loaded_image = output.substr(pos + 17);
                loaded_image.erase(loaded_image.find_last_not_of(" \n\r\t") + 1);
            }
        }
        
        if (loaded_image.empty()) {
            // 尝试用 docker images 获取最新镜像
            std::string images_cmd = "docker images --format '{{.Repository}}:{{.Tag}}' | head -1 2>&1";
            pipe = popen(images_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    loaded_image += buffer;
                }
                pclose(pipe);
            }
            loaded_image.erase(loaded_image.find_last_not_of(" \n\r\t") + 1);
        }
        
        std::cout << "[INFO] 加载的镜像: " << loaded_image << std::endl;
        
        if (loaded_image.empty()) {
            unlink(temp_tar.c_str());
            json resp;
            resp["success"] = false;
            resp["error"] = "加载镜像失败，无法识别镜像";
            return crow::response(500, resp.dump());
        }
        
        // 3. 打标签
        output.clear();
        std::string tag_cmd = "docker tag " + loaded_image + " " + full_image + " 2>&1";
        std::cout << "[INFO] 打标签: " << tag_cmd << std::endl;
        pipe = popen(tag_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                output += buffer;
            }
            pclose(pipe);
        }
        
        if (output.find("Error") != std::string::npos) {
            unlink(temp_tar.c_str());
            json resp;
            resp["success"] = false;
            resp["error"] = "打标签失败: " + output;
            return crow::response(500, resp.dump());
        }
        
        // 4. 推送镜像
        output.clear();
        std::string push_cmd = "docker push " + full_image + " 2>&1";
        std::cout << "[INFO] 推送镜像: " << push_cmd << std::endl;
        pipe = popen(push_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                output += buffer;
            }
            pclose(pipe);
        }
        
        // 删除临时文件
        unlink(temp_tar.c_str());
        
        if (output.find("error") != std::string::npos || output.find("Error") != std::string::npos) {
            json resp;
            resp["success"] = false;
            resp["error"] = "推送失败: " + output;
            return crow::response(500, resp.dump());
        }
        
        // 5. 刷新镜像缓存
        refreshImageCacheToDB();
        
        json resp;
        resp["success"] = true;
        resp["message"] = "镜像上传成功: " + full_image;
        resp["image"] = full_image;
        return crow::response(resp.dump());
    });
    
    
    
    
    


    /////////////////////
   // 获取 ReplicaSet 列表（按 Deployment 过滤）
        
        
    CROW_ROUTE(app, "/api/harbor/download-image").methods("GET"_method)([](const crow::request& req) {
        std::string image = req.url_params.get("image");
        if (image.empty()) {
            json resp;
            resp["error"] = "镜像名称不能为空";
            return crow::response(400, resp.dump());
        }
        
        std::string harbor_addr = "192.168.138.139:30002";
        std::string full_image = harbor_addr + "/" + image;
        
        std::string temp_file = "/tmp/" + std::to_string(time(nullptr)) + ".tar";
        
        std::string login_cmd = "docker login " + harbor_addr + " -u admin -p Harbor12345 2>&1";
        system(login_cmd.c_str());
        
        std::string cmd = "docker pull " + full_image + " 2>&1 && docker save " + full_image + " -o " + temp_file + " 2>&1";
        
        std::cout << "[INFO] 执行命令: " << cmd << std::endl;
        
        int ret = system(cmd.c_str());
        
        if (ret != 0) {
            json resp;
            resp["error"] = "镜像拉取或导出失败";
            return crow::response(500, resp.dump());
        }
        
        struct stat st;
        if (stat(temp_file.c_str(), &st) != 0 || st.st_size == 0) {
            json resp;
            resp["error"] = "镜像文件为空";
            return crow::response(500, resp.dump());
        }
        
        std::ifstream file(temp_file, std::ios::binary);
        if (!file.is_open()) {
            json resp;
            resp["error"] = "无法读取镜像文件";
            return crow::response(500, resp.dump());
        }
        
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        unlink(temp_file.c_str());
        
        std::string filename = image.replace(image.find(':'), 1, "_") + ".tar";
        
        crow::response res;
        res.set_header("Content-Type", "application/octet-stream");
        res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        res.write(content);
        return res;
    });
    
    
    CROW_ROUTE(app, "/api/harbor/repositories/<string>/<string>/tags").methods("GET"_method)([](const std::string& project, const std::string& repo) {
        std::string url = "http://192.168.138.139:30002/api/v2.0/projects/" + project +
                          "/repositories/" + repo + "/artifacts";
    
        CURL* curl = curl_easy_init();
        std::string response;
    
        if (curl) {
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Authorization: Basic YWRtaW46SGFyYm9yMTIzNDU=");
    
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
            curl_easy_perform(curl);
    
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        }
    
        json tags = json::array();
        try {
            auto artifacts = json::parse(response);
            if (artifacts.is_array()) {
                for (const auto& artifact : artifacts) {
                    if (artifact.contains("tags") && artifact["tags"].is_array()) {
                        for (const auto& tag : artifact["tags"]) {
                            tags.push_back(tag["name"]);
                        }
                    }
                }
            }
        } catch(...) {}
    
        return crow::response(tags.dump());
    });
    
                
    

    

    
    
    

    // 删除 Harbor 项目
    CROW_ROUTE(app, "/api/harbor/projects/<string>").methods("DELETE"_method)([](const std::string& project_name) {
        try {
            // 先检查项目是否有镜像
            std::string check_url = "http://192.168.138.139:30002/api/v2.0/projects/" + project_name + "/repositories";
            
            CURL* curl = curl_easy_init();
            std::string response;
            long repo_count = 0;
            
            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                
                curl_easy_setopt(curl, CURLOPT_URL, check_url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_USERPWD, "admin:Harbor12345");
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                if (res == CURLE_OK && http_code == 200) {
                    try {
                        auto repos = json::parse(response);
                        repo_count = repos.is_array() ? repos.size() : 0;
                    } catch(...) {}
                }
                
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
            }
            
            // 如果有镜像，不允许删除
            if (repo_count > 0) {
                json resp;
                resp["success"] = false;
                resp["error"] = "项目包含 " + std::to_string(repo_count) + " 个镜像，无法删除";
                resp["has_images"] = true;
                return crow::response(400, resp.dump());
            }
            
            // 删除项目
            std::string delete_url = "http://192.168.138.139:30002/api/v2.0/projects/" + project_name;
            
            curl = curl_easy_init();
            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                
                curl_easy_setopt(curl, CURLOPT_URL, delete_url.c_str());
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_USERPWD, "admin:Harbor12345");
                
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                
                if (res == CURLE_OK && (http_code == 200 || http_code == 204)) {
                    json resp;
                    resp["success"] = true;
                    resp["message"] = "项目删除成功: " + project_name;
                    return crow::response(resp.dump());
                } else {
                    json resp;
                    resp["success"] = false;
                    resp["error"] = "删除失败，HTTP状态码: " + std::to_string(http_code);
                    return crow::response(500, resp.dump());
                }
            }
            
            json resp;
            resp["success"] = false;
            resp["error"] = "CURL 初始化失败";
            return crow::response(500, resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });
    
    



    // 创建 Harbor 项目
    CROW_ROUTE(app, "/api/harbor/projects").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            std::string project_name = body.value("project_name", "");
            bool is_public = body.value("public", false);
            int storage_limit = body.value("storage_limit", -1);
            
            if (project_name.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "项目名称不能为空";
                return crow::response(400, resp.dump());
            }
            
            // 构建 Harbor API 请求
            std::string url = "http://192.168.138.139:30002/api/v2.0/projects";
            
            json project_data;
            project_data["project_name"] = project_name;
            project_data["public"] = is_public;
            if (storage_limit > 0) {
                project_data["storage_limit"] = storage_limit * 1024 * 1024 * 1024LL;
            }
            
            std::string post_data = project_data.dump();
            
            CURL* curl = curl_easy_init();
            std::string response;
            
            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_USERPWD, "admin:Harbor12345");  // 添加认证
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                
                if (res == CURLE_OK && http_code == 201) {
                    json resp;
                    resp["success"] = true;
                    resp["message"] = "项目创建成功: " + project_name;
                    return crow::response(resp.dump());
                } else {
                    std::string error_msg = "创建失败，HTTP状态码: " + std::to_string(http_code);
                    
                    // 解析错误信息
                    try {
                        auto err_json = json::parse(response);
                        if (err_json.contains("errors") && err_json["errors"].is_array()) {
                            error_msg += " - " + err_json["errors"][0].value("message", "");
                        }
                    } catch(...) {}
                    
                    json resp;
                    resp["success"] = false;
                    resp["error"] = error_msg;
                    return crow::response(500, resp.dump());
                }
            }
            
            json resp;
            resp["success"] = false;
            resp["error"] = "CURL 初始化失败";
            return crow::response(500, resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });







    
    // 创建 Harbor 项目
    CROW_ROUTE(app, "/api/harbor/projects2").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            std::string project_name = body.value("project_name", "");
            bool is_public = body.value("public", false);
            int storage_limit = body.value("storage_limit", -1);
            
            if (project_name.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "项目名称不能为空";
                return crow::response(400, resp.dump());
            }
            
            // Harbor 认证信息
            std::string harbor_user = "admin";
            std::string harbor_password = "Harbor12345";
            
            // 构建 Harbor API 请求
            std::string url = "http://192.168.138.139:30002/api/v2.0/projects";
            
            json project_data;
            project_data["project_name"] = project_name;
            project_data["public"] = is_public;
            if (storage_limit > 0) {
                project_data["storage_limit"] = storage_limit * 1024 * 1024 * 1024LL;
            }
            
            std::string post_data = project_data.dump();
            
            CURL* curl = curl_easy_init();
            std::string response;
            
            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                
                // 添加 Basic 认证
                std::string auth = harbor_user + ":" + harbor_password;
                std::string auth_base64 = "";
                
                // Base64 编码
                std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                size_t i = 0;
                unsigned char char_array_3[3];
                unsigned char char_array_4[4];
                int in_len = auth.size();
                
                while (in_len--) {
                    char_array_3[i++] = *(auth.c_str());
                    if (i == 3) {
                        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                        char_array_4[3] = char_array_3[2] & 0x3f;
                        for (i = 0; i < 4; i++) {
                            auth_base64 += base64_chars[char_array_4[i]];
                        }
                        i = 0;
                    }
                }
                if (i) {
                    for (int j = i; j < 3; j++) {
                        char_array_3[j] = '\0';
                    }
                    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                    char_array_4[3] = char_array_3[2] & 0x3f;
                    for (int j = 0; j < i + 1; j++) {
                        auth_base64 += base64_chars[char_array_4[j]];
                    }
                    while (i++ < 3) {
                        auth_base64 += '=';
                    }
                }
                
                std::string auth_header = "Authorization: Basic " + auth_base64;
                headers = curl_slist_append(headers, auth_header.c_str());
                
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                
                if (res == CURLE_OK && (http_code == 201 || http_code == 200)) {
                    json resp;
                    resp["success"] = true;
                    resp["message"] = "项目创建成功: " + project_name;
                    return crow::response(resp.dump());
                } else {
                    json resp;
                    resp["success"] = false;
                    resp["error"] = "创建失败，HTTP状态码: " + std::to_string(http_code);
                    resp["detail"] = response;
                    return crow::response(500, resp.dump());
                }
            }
            
            json resp;
            resp["success"] = false;
            resp["error"] = "CURL 初始化失败";
            return crow::response(500, resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });
    





    
    // 创建 Harbor 项目
    CROW_ROUTE(app, "/api/harbor/projects1").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            std::string project_name = body.value("project_name", "");
            bool is_public = body.value("public", false);
            int storage_limit = body.value("storage_limit", -1);
            
            if (project_name.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "项目名称不能为空";
                return crow::response(400, resp.dump());
            }
            
            // 构建 Harbor API 请求
            std::string url = "http://192.168.138.139:30002/api/v2.0/projects";
            
            json project_data;
            project_data["project_name"] = project_name;
            project_data["public"] = is_public;
            if (storage_limit > 0) {
                project_data["storage_limit"] = storage_limit * 1024 * 1024 * 1024LL; // 转换为字节
            }
            
            std::string post_data = project_data.dump();
            
            CURL* curl = curl_easy_init();
            std::string response;
            
            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                
                if (res == CURLE_OK && (http_code == 201 || http_code == 200)) {
                    // 刷新项目列表缓存
                    refreshImageCacheToDB();  // 可选，刷新缓存
                    
                    json resp;
                    resp["success"] = true;
                    resp["message"] = "项目创建成功: " + project_name;
                    return crow::response(resp.dump());
                } else {
                    json resp;
                    resp["success"] = false;
                    resp["error"] = "创建失败，HTTP状态码: " + std::to_string(http_code);
                    resp["detail"] = response;
                    return crow::response(500, resp.dump());
                }
            }
            
            json resp;
            resp["success"] = false;
            resp["error"] = "CURL 初始化失败";
            return crow::response(500, resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });



    // ========== 集群健康检查 API ==========
        
    CROW_ROUTE(app, "/api/health/check").methods("GET"_method)([]() {
        json resp;
        resp["success"] = true;
        resp["timestamp"] = std::time(nullptr);
        
        char buffer[4096];
        FILE* pipe = nullptr;
        
        // 1. etcd 健康检查（快速检查 Pod 数量）
        std::string etcd_cmd = "kubectl get pods -n kube-system -l component=etcd --field-selector=status.phase=Running -o name 2>/dev/null | wc -l";
        std::string etcd_result = "";
        pipe = popen(etcd_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                etcd_result += buffer;
            }
            pclose(pipe);
        }
        int etcd_count = atoi(etcd_result.c_str());
        resp["etcd"]["healthy"] = (etcd_count >= 2);
        resp["etcd"]["running_count"] = etcd_count;
        
        // 2. OOM 检查（只检查最近24小时，且只获取数量）
        std::string oom_cmd = "kubectl get events --all-namespaces --field-selector reason=OOMKilling 2>/dev/null | wc -l";
        std::string oom_result = "";
        pipe = popen(oom_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                oom_result += buffer;
            }
            pclose(pipe);
        }
        int oom_count = atoi(oom_result.c_str());
        resp["oom"]["has_oom"] = (oom_count > 0);
        resp["oom"]["count"] = oom_count;
        
        // 3. 慢节点检查（只检查 NotReady 节点）
        std::string nodes_cmd = "kubectl get nodes --field-selector=status.condition.ready!=True -o name 2>/dev/null | wc -l";
        std::string nodes_result = "";
        pipe = popen(nodes_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                nodes_result += buffer;
            }
            pclose(pipe);
        }
        int not_ready_count = atoi(nodes_result.c_str());
        
        json slow_nodes = json::array();
        if (not_ready_count > 0) {
            // 获取具体节点名称
            std::string name_cmd = "kubectl get nodes --field-selector=status.condition.ready!=True -o jsonpath='{.items[*].metadata.name}' 2>/dev/null";
            std::string name_result = "";
            pipe = popen(name_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    name_result += buffer;
                }
                pclose(pipe);
            }
            
            json slow_node;
            slow_node["name"] = name_result;
            slow_node["issue"] = "节点状态异常";
            slow_nodes.push_back(slow_node);
        }
        
        resp["slow_nodes"]["count"] = not_ready_count;
        resp["slow_nodes"]["details"] = slow_nodes;
        
        // 4. 网络异常检查（快速检查 NetworkUnavailable 事件数量）
        std::string network_cmd = "kubectl get events --all-namespaces --field-selector reason=NetworkUnavailable 2>/dev/null | wc -l";
        std::string network_result = "";
        pipe = popen(network_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                network_result += buffer;
            }
            pclose(pipe);
        }
        int network_count = atoi(network_result.c_str());
        resp["network"]["has_issue"] = (network_count > 0);
        resp["network"]["count"] = network_count;
        
        // 5. API Server 健康检查
        std::string api_cmd = "kubectl get --raw /healthz 2>/dev/null";
        std::string api_result = "";
        pipe = popen(api_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                api_result += buffer;
            }
            pclose(pipe);
        }
        resp["api_server"]["healthy"] = (api_result.find("ok") != std::string::npos);
        
        return crow::response(resp.dump());
    });


    

    CROW_ROUTE(app, "/api/auth/logout").methods("POST"_method)([]() {
        json resp;
        resp["code"] = 0;
        resp["message"] = "登出成功";
        return crow::response(resp.dump());
    });

    CROW_ROUTE(app, "/logout").methods("GET"_method)([]() {
        // 清除 session 或 cookie
        crow::response res;
        res.set_header("Location", "/");
        res.code = 302;
        return res;
    });






    // 手动刷新镜像缓存
    CROW_ROUTE(app, "/api/harbor/refresh-cache").methods("POST"_method)([]() {
        refreshImageCacheToDB();
        json resp;
        resp["success"] = true;
        resp["message"] = "镜像缓存已刷新";
        return crow::response(resp.dump());
    });




    
    // 获取用户信息
    CROW_ROUTE(app, "/api/auth/user-info1").methods("GET"_method)([](const crow::request& req) {
        // 从 session 或 token 获取当前用户
        std::string username = "admin";
        std::string auth = req.get_header_value("Authorization");
        if (!auth.empty() && auth.find("Bearer ") == 0) {
            std::string token = auth.substr(7);
            if (token.find("token-") == 0) {
                username = token.substr(6);
            }
        }
        
        json resp;
        resp["success"] = true;
        resp["username"] = username;
        resp["fullname"] = "System Administrator";
        resp["group"] = "administrators";
        resp["email"] = "admin@example.com";
        resp["role"] = "admin";
        return crow::response(resp.dump());
    });
   
    
    CROW_ROUTE(app, "/api/auth/user-info2").methods("GET"_method)([](const crow::request& req) {
        std::string username = "admin";
        std::string auth = req.get_header_value("Authorization");
        if (!auth.empty() && auth.find("Bearer ") == 0) {
            std::string token = auth.substr(7);
            if (token.find("token-") == 0) username = token.substr(6);
        }
        UserInfo info = getUserFromDB(username);
        json resp;
        resp["success"]  = true;
        resp["username"] = username;
        resp["fullname"] = info.found ? info.fullname : username;
        resp["email"]    = info.found ? info.email : "";
        resp["role"]     = info.found ? info.role : "admin";
        return crow::response(resp.dump());
    });
    

    
    CROW_ROUTE(app, "/api/auth/user-info").methods("PUT"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);
        std::string username = body.value("username", "");
        std::string fullname = body.value("fullname", "");
        std::string email    = body.value("email", "");
    
        json resp;
        if (username.empty()) {
            resp["success"] = false;
            resp["message"] = "用户名不能为空";
            return crow::response(resp.dump());
        }
    
        try {
            pqxx::connection conn("postgresql://testuser:changeme@postgres/testplatform");
            pqxx::work txn(conn);
            txn.exec_params(
                "UPDATE users SET fullname=$1, email=$2 WHERE username=$3",
                fullname, email, username
            );
            txn.commit();
            resp["success"] = true;
            resp["message"] = "用户信息已更新";
        } catch (const std::exception& e) {
            std::cerr << "update user-info error: " << e.what() << std::endl;
            resp["success"] = false;
            resp["message"] = "数据库更新失败";
        }
        return crow::response(resp.dump());
    });
    
    



    // 更新用户信息
    CROW_ROUTE(app, "/api/auth/user-info33").methods("PUT"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);
        std::string username = body.value("username", "");
        std::string fullname = body.value("fullname", "");
        std::string email = body.value("email", "");
        
        // 这里可以保存到数据库
        // 目前只做响应
        json resp;
        resp["success"] = true;
        resp["message"] = "用户信息已更新";
        return crow::response(resp.dump());
    });
    






    // 验证删除密码
    CROW_ROUTE(app, "/api/verify-delete-password").methods("POST"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);
        std::string password = body.value("password", "");
        
        // 从环境变量读取密码，或使用固定密码
        const char* admin_password = std::getenv("DELETE_SECURITY_CODE");
        std::string valid_password = admin_password ? admin_password : "admin142857$";
        
        json resp;
        resp["valid"] = (password == valid_password);
        return crow::response(resp.dump());
    });
    
    
    




    // ========== 通过 Job 在目标节点打包镜像 ==========
    
    // 创建打包 Job
    CROW_ROUTE(app, "/api/image-pack/create-job").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
	    //std::string namespace_ = "default";  // 强制使用 default
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            std::string harbor_registry = body.value("harborRegistry", "");
            std::string harbor_username = body.value("harborUsername", "");
            std::string harbor_password = body.value("harborPassword", "");
           

	    std::string job_namespace = "default";

            if (pod_name.empty() || target_image.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数";
                return crow::response(400, resp.dump());
            }
            
            std::cout << "[INFO] 创建打包 Job - Pod: " << pod_name << ", 目标镜像: " << target_image << std::endl;
            
            // 1. 获取目标 Pod 所在的节点
            std::string get_node_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                       " -o jsonpath='{.spec.nodeName}' 2>/dev/null";
            
            std::string node_name = "";
            char buffer[4096];
            FILE* node_pipe = popen(get_node_cmd.c_str(), "r");
            if (node_pipe) {
                while (fgets(buffer, sizeof(buffer), node_pipe) != NULL) {
                    node_name += buffer;
                }
                pclose(node_pipe);
            }
            node_name.erase(std::remove(node_name.begin(), node_name.end(), '\n'), node_name.end());
            
            if (node_name.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "未找到 Pod 所在节点";
                return crow::response(404, resp.dump());
            }
            
            std::cout << "[INFO] 目标节点: " << node_name << std::endl;
            
            // 2. 获取容器 ID
            std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                            " -o jsonpath='{.status.containerStatuses[?(@.name==\"" + container_name + "\")].containerID}' 2>/dev/null";
            
            std::string container_id = "";
            FILE* container_pipe = popen(get_container_cmd.c_str(), "r");
            if (container_pipe) {
                while (fgets(buffer, sizeof(buffer), container_pipe) != NULL) {
                    container_id += buffer;
                }
                pclose(container_pipe);
            }
            
            // 移除 containerd:// 前缀
            size_t pos = container_id.find("://");
            if (pos != std::string::npos) {
                container_id = container_id.substr(pos + 3);
            }
            container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
            container_id.erase(std::remove(container_id.begin(), container_id.end(), '\r'), container_id.end());
            
            if (container_id.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "未找到容器 ID";
                return crow::response(404, resp.dump());
            }
            
            std::cout << "[INFO] 容器 ID: " << container_id << std::endl;
            
            // 3. 创建 Job YAML
            std::string job_name = "image-packer-" + std::to_string(time(nullptr));
            std::string image_tag = target_image;
            
            // 构建 Job YAML
            json job = {
                {"apiVersion", "batch/v1"},
                {"kind", "Job"},
                {"metadata", {
                    {"name", job_name},
                    {"namespace", job_namespace}
                }},
                {"spec", {
                    {"ttlSecondsAfterFinished", 60},
                    {"template", {
                        {"metadata", {
                            {"name", job_name}
                        }},
                        {"spec", {
                            {"nodeName", node_name},
                            {"restartPolicy", "Never"},
			    {"terminationGracePeriodSeconds", 30},
                            {"containers", json::array({
                                {
                                    {"name", "image-packer"},
                                    {"image", "192.168.138.139:30002/library/nerdctl:1.7.6"},
                                    {"command", json::array({
                                        "sh", "-c", 
                                        "echo '开始打包镜像...' && " +
                                        std::string("nerdctl --namespace k8s.io commit ") + container_id + " " + target_image + " && " +
                                        "echo '镜像创建成功，开始推送...' && " +
                                        "nerdctl --namespace k8s.io login " + harbor_registry + " -u " + harbor_username + " -p " + harbor_password + " --insecure-registry && " +
                                        "nerdctl --namespace k8s.io tag " + target_image + " " + harbor_registry + target_image + " && " +
                                        "nerdctl --namespace k8s.io push --insecure-registry " + harbor_registry + target_image + " && " +
                                        "echo '✅ 镜像推送成功: " + harbor_registry + target_image + "'"
                                    })},
                                    {"volumeMounts", json::array({
                                        {{"name", "containerd-sock"}, {"mountPath", "/run/containerd/containerd.sock"}}
                                    })}
                                }
                            })},
                            {"volumes", json::array({
                                {
                                    {"name", "containerd-sock"},
                                    {"hostPath", {
                                        {"path", "/run/containerd/containerd.sock"},
                                        {"type", "Socket"}
                                    }}
                                }
                            })}
                        }}
                    }}
                }}
            };
            
            // 4. 创建 Job
            std::string create_job_cmd = "cat <<EOF | kubectl apply -f -\n" + job.dump() + "\nEOF";
            std::cout << "[INFO] 创建 Job: " << job_name << std::endl;
            
            FILE* create_pipe = popen(create_job_cmd.c_str(), "r");
            std::string create_output = "";
            if (create_pipe) {
                while (fgets(buffer, sizeof(buffer), create_pipe) != NULL) {
                    create_output += buffer;
                }
                pclose(create_pipe);
            }
            
            json resp;
            resp["success"] = true;
            resp["jobName"] = job_name;
            resp["message"] = "打包 Job 已创建: " + job_name;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 创建 Job 异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });
   
    
    CROW_ROUTE(app, "/api/image-pack/job-status1/<string>").methods("GET"_method)([](const std::string& job_name) {
        try {
            // 在所有命名空间中查找 Job
            std::string cmd = "kubectl get job " + job_name + " --all-namespaces -o json 2>/dev/null";
            std::string output = "";
            char buffer[4096];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    output += buffer;
                }
                pclose(pipe);
            }
            
            if (output.empty()) {
                json resp;
                resp["success"] = true;
                resp["status"] = "not_found";
                return crow::response(resp.dump());
            }
            
            auto job_info = json::parse(output);
            json resp;
            resp["success"] = true;
            
            // 从 items 中获取第一个（如果有多个同名 Job，取第一个）
            if (job_info.contains("items") && job_info["items"].is_array() && !job_info["items"].empty()) {
                auto item = job_info["items"][0];
                if (item.contains("status")) {
                    auto status = item["status"];
                    if (status.contains("succeeded") && status["succeeded"].get<int>() > 0) {
                        resp["status"] = "succeeded";
                    } else if (status.contains("failed") && status["failed"].get<int>() > 0) {
                        resp["status"] = "failed";
                    } else if (status.contains("active") && status["active"].get<int>() > 0) {
                        resp["status"] = "running";
                    } else {
                        resp["status"] = "pending";
                    }
                } else {
                    resp["status"] = "pending";
                }
            } else {
                resp["status"] = "not_found";
            }
            
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["status"] = "error";
            resp["error"] = e.what();
            return crow::response(resp.dump());
        }
    });

    // 查询 Job 状态
    CROW_ROUTE(app, "/api/image-pack/job-status/<string>").methods("GET"_method)([](const std::string& job_name) {
        try {
            std::string cmd = "kubectl get job " + job_name + " -o json 2>/dev/null";
            std::string output = "";
            char buffer[4096];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    output += buffer;
                }
                pclose(pipe);
            }
            
            if (output.empty()) {
                json resp;
                resp["success"] = false;
                resp["status"] = "not_found";
                return crow::response(resp.dump());
            }
            
            auto job_info = json::parse(output);
            json resp;
            resp["success"] = true;
            
            if (job_info.contains("status")) {
                auto status = job_info["status"];
                if (status.contains("succeeded")) {
                    resp["status"] = "succeeded";
                } else if (status.contains("failed")) {
                    resp["status"] = "failed";
                } else if (status.contains("active")) {
                    resp["status"] = "running";
                } else {
                    resp["status"] = "pending";
                }
            } else {
                resp["status"] = "pending";
            }
            
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });
    
    
    CROW_ROUTE(app, "/api/image-pack/job-logs11/<string>").methods("GET"_method)([](const std::string& job_name) {
        try {
            // 先找到 Job 所在的命名空间
            std::string ns_cmd = "kubectl get job " + job_name + " --all-namespaces -o jsonpath='{.items[0].metadata.namespace}' 2>/dev/null";
            std::string namespace_ = "";
            char buffer[4096];
            FILE* ns_pipe = popen(ns_cmd.c_str(), "r");
            if (ns_pipe) {
                while (fgets(buffer, sizeof(buffer), ns_pipe) != NULL) {
                    namespace_ += buffer;
                }
                pclose(ns_pipe);
            }
            
            if (namespace_.empty()) {
                json resp;
                resp["success"] = true;
                resp["logs"] = "";
                return crow::response(resp.dump());
            }
            
            // 获取日志并清理控制台代码
            std::string cmd = "kubectl logs job/" + job_name + " -n " + namespace_ + " 2>/dev/null | sed 's/\\x1b\\[[0-9;]*m//g'";
            std::string output = "";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    output += buffer;
                }
                pclose(pipe);
            }
            
            json resp;
            resp["success"] = true;
            resp["logs"] = output;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });



    // 获取 Job 日志
    CROW_ROUTE(app, "/api/image-pack/job-logs/<string>").methods("GET"_method)([](const std::string& job_name) {
        try {
            std::string cmd = "kubectl logs job/" + job_name + " 2>/dev/null";
            std::string output = "";
            char buffer[4096];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    output += buffer;
                }
                pclose(pipe);
            }
            
            json resp;
            resp["success"] = true;
            resp["logs"] = output;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });
    
    // 删除 Job
    CROW_ROUTE(app, "/api/image-pack/delete-job/<string>").methods("DELETE"_method)([](const std::string& job_name) {
        try {
            std::string cmd = "kubectl delete job " + job_name + " --ignore-not-found=true 2>&1";
            system(cmd.c_str());
            
            json resp;
            resp["success"] = true;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });










    /////////////////////////




    // 调试接口：获取 Pod 容器列表（备用）
    CROW_ROUTE(app, "/api/debug/pod-containers").methods("GET"_method)([](const crow::request& req) {
        const char* ns_param = req.url_params.get("namespace");
        const char* pod_param = req.url_params.get("pod");
        
        std::string namespace_ = ns_param ? ns_param : "default";
        std::string pod_name = pod_param ? pod_param : "";
        
        if (pod_name.empty()) {
            return crow::response(400, R"({"error":"pod name required"})");
        }
        
        std::string cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                          " -o jsonpath='{.spec.containers[*].name}' 2>/dev/null";
        
        std::string result = "";
        char buffer[256];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
        }
        
        json containers = json::array();
        std::istringstream iss(result);
        std::string container_name;
        while (iss >> container_name) {
            if (!container_name.empty()) {
                containers.push_back(container_name);
            }
        }
        
        json resp;
        resp["containers"] = containers;
        return crow::response(resp.dump());
    });
    




    // 获取 Pod 中的容器列表
    CROW_ROUTE(app, "/api/pods/<string>/<string>/containers").methods("GET"_method)([](const std::string& ns, const std::string& pod_name) {
        std::cout << "[DEBUG] 获取容器列表: namespace=" << ns << ", pod=" << pod_name << std::endl;
        
        // 使用 kubectl 获取所有容器名称
        std::string cmd = "kubectl get pod " + pod_name + " -n " + ns + 
                          " -o jsonpath='{.spec.containers[*].name}' 2>/dev/null";
        
        std::string result = "";
        char buffer[256];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
        }
        
        std::cout << "[DEBUG] kubectl 输出: '" << result << "'" << std::endl;
        
        // 解析容器名（空格分隔）
        json containers = json::array();
        std::istringstream iss(result);
        std::string container_name;
        while (iss >> container_name) {
            if (!container_name.empty()) {
                containers.push_back(container_name);
                std::cout << "[DEBUG] 找到容器: " << container_name << std::endl;
            }
        }
        
        // 返回 JSON
        json response;
        response["containers"] = containers;
        std::cout << "[DEBUG] 返回: " << response.dump() << std::endl;
        return crow::response(response.dump());
    });




    // ========== 镜像打包 API ==========
    
    // 获取 Pod 中的容器列表
    CROW_ROUTE(app, "/api/pods/<string>/<string>/containers7").methods("GET"_method)([](const std::string& ns, const std::string& pod_name) {
        try {
            std::string cmd = "kubectl get pod " + pod_name + " -n " + ns + 
                              " -o jsonpath='{range .spec.containers[*]}{.name}{\\\"\\n\\\"}{end}' 2>/dev/null";
            
            std::string result = "";
            char buffer[256];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    result += buffer;
                }
                pclose(pipe);
            }
            
            json containers = json::array();
            std::istringstream iss(result);
            std::string container_name;
            while (std::getline(iss, container_name)) {
                if (!container_name.empty()) {
                    containers.push_back(container_name);
                }
            }
            
            return crow::response(R"({"containers":)" + containers.dump() + "}");
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"error":")" + std::string(e.what()) + R"("})");
        }
    });
    
    // 从 Pod 创建镜像
    CROW_ROUTE(app, "/api/image-pack/create").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数";
                return crow::response(400, resp.dump());
            }
            
            // 如果没有指定容器名，自动获取第一个容器
            if (container_name.empty()) {
                std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                                " -o jsonpath='{.spec.containers[0].name}' 2>/dev/null";
                char buffer[256];
                FILE* pipe = popen(get_container_cmd.c_str(), "r");
                if (pipe) {
                    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                        container_name += buffer;
                    }
                    pclose(pipe);
                }
                // 移除换行符
                container_name.erase(std::remove(container_name.begin(), container_name.end(), '\n'), container_name.end());
            }
            
            // 获取容器 ID
            std::string get_id_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                     " -o jsonpath='{.status.containerStatuses[?(@.name==\"" + container_name + "\")].containerID}' 2>/dev/null";
            
            std::string container_id = "";
            char buffer[256];
            FILE* pipe = popen(get_id_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    container_id += buffer;
                }
                pclose(pipe);
            }
            
            // 移除运行时前缀
            if (!container_id.empty()) {
                size_t pos = container_id.find("://");
                if (pos != std::string::npos) {
                    container_id = container_id.substr(pos + 3);
                }
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\''), container_id.end());
            }
            
            if (container_id.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "未找到容器: " + pod_name + "/" + container_name;
                return crow::response(404, resp.dump());
            }
            
            // 使用 crictl commit 创建镜像
            std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
            std::string commit_output = "";
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            json resp;
            resp["success"] = true;
            resp["imageId"] = target_image;
            resp["message"] = "镜像创建成功";
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });


    // 推送镜像到 Harbor（修复版）
    CROW_ROUTE(app, "/api/image-pack/push").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string source_image = body.value("sourceImage", "");
            std::string target_registry = body.value("targetRegistry", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string final_image_name = body.value("finalImageName", source_image);
            
            if (source_image.empty() || target_registry.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数";
                return crow::response(400, resp.dump());
            }
            
            // 确保仓库地址格式正确
            std::string registry = target_registry;
            if (registry.back() != '/') {
                registry += '/';
            }
            
            std::string full_target = registry + final_image_name;
            
            std::cout << "[INFO] 推送镜像: " << source_image << " -> " << full_target << std::endl;
            
            char buffer[4096];
            std::string output;
            
            // 提取 registry 主机地址
            std::string registry_host = registry;
            size_t scheme_pos = registry_host.find("://");
            if (scheme_pos != std::string::npos) {
                registry_host = registry_host.substr(scheme_pos + 3);
            }
            size_t slash_pos = registry_host.find('/');
            if (slash_pos != std::string::npos) {
                registry_host = registry_host.substr(0, slash_pos);
            }
            
            // 1. 登录 Harbor
            if (!username.empty() && !password.empty()) {
                std::string login_cmd = "docker login " + registry_host + " -u " + username + " -p " + password + " 2>&1";
                std::cout << "[INFO] 执行登录: " << login_cmd << std::endl;
                
                FILE* login_pipe = popen(login_cmd.c_str(), "r");
                if (login_pipe) {
                    while (fgets(buffer, sizeof(buffer), login_pipe) != NULL) {
                        output += buffer;
                    }
                    pclose(login_pipe);
                }
                std::cout << "[INFO] 登录输出: " << output << std::endl;
                output.clear();
            }
            
            // 2. 检查本地镜像是否存在
            std::string inspect_cmd = "docker images --format '{{.Repository}}:{{.Tag}}' | grep '^" + source_image + "$' 2>/dev/null";
            FILE* inspect_pipe = popen(inspect_cmd.c_str(), "r");
            bool image_exists = false;
            if (inspect_pipe) {
                std::string line;
                while (fgets(buffer, sizeof(buffer), inspect_pipe) != NULL) {
                    line = buffer;
                    line.erase(line.find_last_not_of(" \n\r\t") + 1);
                    if (line == source_image) {
                        image_exists = true;
                    }
                }
                pclose(inspect_pipe);
            }
            
            if (!image_exists) {
                std::cout << "[WARN] 本地镜像不存在: " << source_image << std::endl;
                // 尝试用 crictl 查找
                std::string crictl_cmd = "crictl images --quiet | xargs -I {} crictl inspecti {} | grep -A2 '" + source_image + "' 2>/dev/null";
                FILE* crictl_pipe = popen(crictl_cmd.c_str(), "r");
                bool found = false;
                if (crictl_pipe) {
                    while (fgets(buffer, sizeof(buffer), crictl_pipe) != NULL) {
                        if (std::string(buffer).find(source_image) != std::string::npos) {
                            found = true;
                            break;
                        }
                    }
                    pclose(crictl_pipe);
                }
                
                if (!found) {
                    json resp;
                    resp["success"] = false;
                    resp["error"] = "本地镜像不存在: " + source_image;
                    return crow::response(404, resp.dump());
                }
            }
            
            // 3. 打标签
            std::string tag_cmd = "docker tag " + source_image + " " + full_target + " 2>&1";
            std::cout << "[INFO] 打标签: " << tag_cmd << std::endl;
            FILE* tag_pipe = popen(tag_cmd.c_str(), "r");
            if (tag_pipe) {
                while (fgets(buffer, sizeof(buffer), tag_pipe) != NULL) {
                    output += buffer;
                }
                pclose(tag_pipe);
            }
            
            if (output.find("Error") != std::string::npos) {
                json resp;
                resp["success"] = false;
                resp["error"] = "打标签失败: " + output;
                return crow::response(500, resp.dump());
            }
            output.clear();
            
            // 4. 推送镜像
            std::string push_cmd = "docker push " + full_target + " 2>&1";
            std::cout << "[INFO] 推送: " << push_cmd << std::endl;
            FILE* push_pipe = popen(push_cmd.c_str(), "r");
            if (push_pipe) {
                while (fgets(buffer, sizeof(buffer), push_pipe) != NULL) {
                    output += buffer;
                    std::cout << "[INFO] 推送输出: " << buffer;
                }
                pclose(push_pipe);
            }
            
            // 5. 检查推送结果
            if (output.find("error") != std::string::npos || 
                output.find("Error") != std::string::npos ||
                output.find("failed") != std::string::npos) {
                json resp;
                resp["success"] = false;
                resp["error"] = "推送失败: " + output;
                return crow::response(500, resp.dump());
            }
            
            json resp;
            resp["success"] = true;
            resp["fullImageName"] = full_target;
            resp["message"] = "镜像推送成功: " + full_target;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 推送镜像异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });



    // 推送镜像到 Harbor
    CROW_ROUTE(app, "/api/image-pack/push11").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string source_image = body.value("sourceImage", "");
            std::string target_registry = body.value("targetRegistry", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string final_image_name = body.value("finalImageName", source_image);
            
            if (source_image.empty() || target_registry.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数";
                return crow::response(400, resp.dump());
            }
            
            // 确保仓库地址格式正确
            std::string registry = target_registry;
            if (registry.back() != '/') {
                registry += '/';
            }
            
            std::string full_target = registry + final_image_name;
            
            char buffer[256];
            
            // 使用 docker 命令
            // 打标签
            std::string tag_cmd = "docker tag " + source_image + " " + full_target + " 2>&1";
            popen(tag_cmd.c_str(), "r");
            
            // 登录
            if (!username.empty() && !password.empty()) {
                std::string registry_host = registry;
                size_t slash_pos = registry_host.find('/');
                if (slash_pos != std::string::npos) {
                    registry_host = registry_host.substr(0, slash_pos);
                }
                std::string login_cmd = "docker login " + registry_host + " -u " + username + " -p " + password + " 2>&1";
                popen(login_cmd.c_str(), "r");
            }
            
            // 推送
            std::string push_cmd = "docker push " + full_target + " 2>&1";
            FILE* push_pipe = popen(push_cmd.c_str(), "r");
            std::string push_output;
            if (push_pipe) {
                while (fgets(buffer, sizeof(buffer), push_pipe) != NULL) {
                    push_output += buffer;
                }
                pclose(push_pipe);
            }
            
            json resp;
            resp["success"] = true;
            resp["fullImageName"] = full_target;
            return crow::response(resp.dump());
            
        } catch (const std::exception& e) {
            json resp;
            resp["success"] = false;
            resp["error"] = e.what();
            return crow::response(500, resp.dump());
        }
    });



    // 从 Pod 中的容器创建镜像 - 完全自动检测容器
    CROW_ROUTE(app, "/api/image-pack/create6").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数: podName 和 targetImage 不能为空";
                return crow::response(400, resp.dump());
            }
            
            std::cout << "[INFO] ========== 开始镜像打包 ==========" << std::endl;
            std::cout << "[INFO] Namespace: " << namespace_ << std::endl;
            std::cout << "[INFO] Pod: " << pod_name << std::endl;
            std::cout << "[INFO] 指定的容器名: " << (container_name.empty() ? "(自动检测)" : container_name) << std::endl;
            std::cout << "[INFO] 目标镜像: " << target_image << std::endl;
            
            // 步骤1: 获取 Pod 的完整信息
            std::string get_pod_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + " -o json 2>/dev/null";
            std::string pod_json = "";
            char buffer[8192];
            FILE* pipe = popen(get_pod_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    pod_json += buffer;
                }
                pclose(pipe);
            }
            
            if (pod_json.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "未找到 Pod: " + pod_name;
                return crow::response(404, resp.dump());
            }
            
            // 步骤2: 解析 Pod 信息，获取容器列表和运行状态
            std::vector<std::string> container_names;
            std::string found_container_id = "";
            std::string found_container_name = "";
            
            try {
                auto pod_info = json::parse(pod_json);
                
                // 获取所有容器名称
                if (pod_info.contains("spec") && pod_info["spec"].contains("containers")) {
                    for (const auto& c : pod_info["spec"]["containers"]) {
                        std::string cname = safeGetString(c, "name");
                        if (!cname.empty()) {
                            container_names.push_back(cname);
                            std::cout << "[INFO] 发现容器: " << cname << std::endl;
                        }
                    }
                }
                
                // 查找运行中的容器并获取 Container ID
                if (pod_info.contains("status") && pod_info["status"].contains("containerStatuses")) {
                    for (const auto& cs : pod_info["status"]["containerStatuses"]) {
                        std::string cs_name = safeGetString(cs, "name");
                        std::string cs_ready = safeGetString(cs, "ready");
                        std::string cs_state = safeGetString(cs, "state", "running");
                        
                        std::cout << "[INFO] 容器状态 - Name: " << cs_name 
                                  << ", Ready: " << cs_ready 
                                  << ", State: " << cs_state << std::endl;
                        
                        // 如果指定了容器名，匹配指定的
                        if (!container_name.empty() && cs_name == container_name) {
                            found_container_id = safeGetString(cs, "containerID");
                            found_container_name = cs_name;
                            std::cout << "[INFO] 找到指定的容器: " << cs_name << std::endl;
                            break;
                        }
                        // 如果没有指定容器名，取第一个 Ready 且 Running 的容器
                        else if (container_name.empty() && cs_ready == "true") {
                            found_container_id = safeGetString(cs, "containerID");
                            found_container_name = cs_name;
                            std::cout << "[INFO] 自动选择运行的容器: " << cs_name << std::endl;
                            break;
                        }
                    }
                }
                
                // 如果还是没找到，取第一个容器
                if (found_container_id.empty() && !container_names.empty()) {
                    std::cout << "[INFO] 使用第一个容器: " << container_names[0] << std::endl;
                    // 重新获取容器 ID
                    for (const auto& cs : pod_info["status"]["containerStatuses"]) {
                        std::string cs_name = safeGetString(cs, "name");
                        if (cs_name == container_names[0]) {
                            found_container_id = safeGetString(cs, "containerID");
                            found_container_name = cs_name;
                            break;
                        }
                    }
                }
                
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] 解析 Pod JSON 失败: " << e.what() << std::endl;
                json resp;
                resp["success"] = false;
                resp["error"] = std::string("解析 Pod 信息失败: ") + e.what();
                return crow::response(500, resp.dump());
            }
            
            if (found_container_id.empty()) {
                std::string error_msg = "未找到运行中的容器。Pod: " + pod_name;
                if (!container_names.empty()) {
                    error_msg += ", 可用容器: ";
                    for (const auto& cn : container_names) {
                        error_msg += cn + " ";
                    }
                } else {
                    error_msg += ", 请确保 Pod 处于 Running 状态";
                }
                std::cerr << "[ERROR] " << error_msg << std::endl;
                json resp;
                resp["success"] = false;
                resp["error"] = error_msg;
                return crow::response(404, resp.dump());
            }
            
            // 移除容器运行时前缀 (containerd://, docker:// 等)
            size_t pos = found_container_id.find("://");
            if (pos != std::string::npos) {
                found_container_id = found_container_id.substr(pos + 3);
            }
            // 清理空白字符
            found_container_id.erase(std::remove(found_container_id.begin(), found_container_id.end(), '\n'), found_container_id.end());
            found_container_id.erase(std::remove(found_container_id.begin(), found_container_id.end(), '\r'), found_container_id.end());
            found_container_id.erase(std::remove(found_container_id.begin(), found_container_id.end(), ' '), found_container_id.end());
            
            std::cout << "[INFO] 容器 ID: " << found_container_id << std::endl;
            std::cout << "[INFO] 容器名称: " << found_container_name << std::endl;
            
            // 步骤3: 使用 crictl 或 docker 创建镜像
            std::string commit_output = "";
            bool commit_success = false;
            
            // 尝试方法1: crictl
            std::string commit_cmd = "crictl commit " + found_container_id + " " + target_image + " 2>&1";
            std::cout << "[INFO] 执行命令: " << commit_cmd << std::endl;
            
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            // 检查是否成功
            commit_success = (commit_output.find("error") == std::string::npos && 
                             commit_output.find("Error") == std::string::npos &&
                             commit_output.find("failed") == std::string::npos);
            
            // 如果 crictl 失败，尝试 docker
            if (!commit_success) {
                std::cout << "[INFO] crictl 失败，尝试使用 docker..." << std::endl;
                std::string docker_cmd = "docker commit " + found_container_id + " " + target_image + " 2>&1";
                commit_output = "";
                FILE* docker_pipe = popen(docker_cmd.c_str(), "r");
                if (docker_pipe) {
                    while (fgets(buffer, sizeof(buffer), docker_pipe) != NULL) {
                        commit_output += buffer;
                    }
                    pclose(docker_pipe);
                }
                commit_success = (commit_output.find("error") == std::string::npos && 
                                 commit_output.find("Error") == std::string::npos);
            }
            
            json resp;
            if (commit_success) {
                resp["success"] = true;
                resp["imageId"] = target_image;
                resp["containerName"] = found_container_name;
                resp["message"] = "镜像创建成功: " + target_image + " (来自容器: " + found_container_name + ")";
                std::cout << "[INFO] 镜像创建成功: " << target_image << std::endl;
                return crow::response(resp.dump());
            } else {
                resp["success"] = false;
                resp["error"] = "创建镜像失败: " + commit_output;
                std::cerr << "[ERROR] 创建镜像失败: " << commit_output << std::endl;
                return crow::response(500, resp.dump());
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 镜像打包异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });
    
    // 测试接口：列出指定 namespace 下所有 Running 的 Pod
    CROW_ROUTE(app, "/api/debug/running-pods").methods("GET"_method)([](const crow::request& req) {
        const char* ns_param = req.url_params.get("namespace");
        std::string namespace_ = ns_param ? std::string(ns_param) : "default";
    
        std::string cmd = "kubectl get pods -n " + namespace_ + " --field-selector=status.phase=Running -o json 2>/dev/null";
        std::string result = "";
        char buffer[4096];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
        }
    
        json pods_info = json::array();
        if (!result.empty()) {
            try {
                auto pods = json::parse(result);
                if (pods.contains("items")) {
                    for (const auto& pod : pods["items"]) {
                        json info;
                        info["name"] = safeGetString(pod["metadata"], "name");
                        info["namespace"] = safeGetString(pod["metadata"], "namespace");
    
                        json containers = json::array();
                        if (pod.contains("spec") && pod["spec"].contains("containers")) {
                            for (const auto& c : pod["spec"]["containers"]) {
                                containers.push_back(safeGetString(c, "name"));
                            }
                        }
                        info["containers"] = containers;
                        pods_info.push_back(info);
                    }
                }
            } catch (...) {}
        }
    
        return crow::response(pods_info.dump());
    });


   // ========== 镜像打包 API（修复版 - 自动获取容器）==========
   
   // 从 Pod 中的容器创建镜像 - 自动检测容器名称
   CROW_ROUTE(app, "/api/image-pack/create4").methods("POST"_method)([](const crow::request& req) {
       try {
           auto body = json::parse(req.body);
           
           std::string namespace_ = body.value("namespace", "default");
           std::string pod_name = body.value("podName", "");
           std::string container_name = body.value("containerName", "");
           std::string target_image = body.value("targetImage", "");
           
           if (pod_name.empty() || target_image.empty()) {
               json resp;
               resp["success"] = false;
               resp["error"] = "缺少必要参数: podName 和 targetImage 不能为空";
               return crow::response(400, resp.dump());
           }
           
           std::cout << "[INFO] 开始打包镜像 - Namespace: " << namespace_ 
                     << ", Pod: " << pod_name 
                     << ", Container: " << container_name 
                     << ", Target: " << target_image << std::endl;
           
           // 如果没有指定容器名，自动获取 Pod 中的第一个容器
           if (container_name.empty()) {
               std::string get_first_container = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                                 " -o jsonpath='{.spec.containers[0].name}' 2>/dev/null";
               char buffer[256];
               FILE* pipe = popen(get_first_container.c_str(), "r");
               if (pipe) {
                   while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                       container_name += buffer;
                   }
                   pclose(pipe);
               }
               // 移除换行符
               container_name.erase(std::remove(container_name.begin(), container_name.end(), '\n'), container_name.end());
               std::cout << "[INFO] 自动检测到容器名: " << container_name << std::endl;
           }
           
           if (container_name.empty()) {
               container_name = "app"; // 默认值
           }
           
           // 方法1: 使用 kubectl 获取容器 ID
           std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                           " -o json 2>/dev/null";
           
           std::string pod_json = "";
           char buffer[4096];
           FILE* pipe = popen(get_container_cmd.c_str(), "r");
           if (pipe) {
               while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                   pod_json += buffer;
               }
               pclose(pipe);
           }
           
           std::string container_id = "";
           
           if (!pod_json.empty()) {
               try {
                   auto pod_info = json::parse(pod_json);
                   
                   // 查找容器状态
                   if (pod_info.contains("status") && pod_info["status"].contains("containerStatuses")) {
                       for (const auto& cs : pod_info["status"]["containerStatuses"]) {
                           std::string cs_name = safeGetString(cs, "name");
                           if (cs_name == container_name) {
                               container_id = safeGetString(cs, "containerID");
                               std::cout << "[INFO] 找到容器 " << cs_name << " 的 ID: " << container_id << std::endl;
                               break;
                           }
                       }
                   }
               } catch (const std::exception& e) {
                   std::cerr << "[ERROR] 解析 Pod JSON 失败: " << e.what() << std::endl;
               }
           }
           
           // 如果还没找到，使用 crictl 直接查找
           if (container_id.empty()) {
               std::string crictl_cmd = "crictl ps --name " + pod_name + " -o json 2>/dev/null";
               std::string crictl_output = "";
               FILE* crictl_pipe = popen(crictl_cmd.c_str(), "r");
               if (crictl_pipe) {
                   while (fgets(buffer, sizeof(buffer), crictl_pipe) != NULL) {
                       crictl_output += buffer;
                   }
                   pclose(crictl_pipe);
               }
               
               if (!crictl_output.empty()) {
                   try {
                       auto crictl_data = json::parse(crictl_output);
                       if (crictl_data.contains("items") && crictl_data["items"].is_array()) {
                           for (const auto& item : crictl_data["items"]) {
                               if (item.contains("metadata") && item["metadata"].contains("name")) {
                                   std::string name = item["metadata"]["name"].get<std::string>();
                                   if (name.find(pod_name) != std::string::npos) {
                                       container_id = safeGetString(item, "id");
                                       std::cout << "[INFO] 通过 crictl 找到容器 ID: " << container_id << std::endl;
                                       break;
                                   }
                               }
                           }
                       }
                   } catch (const std::exception& e) {
                       std::cerr << "[ERROR] 解析 crictl 输出失败: " << e.what() << std::endl;
                   }
               }
           }
           
           // 最后尝试使用 docker
           if (container_id.empty()) {
               std::string docker_cmd = "docker ps --format '{{.ID}}' --filter name=" + pod_name + " 2>/dev/null | head -1";
               std::string docker_result = "";
               FILE* docker_pipe = popen(docker_cmd.c_str(), "r");
               if (docker_pipe) {
                   while (fgets(buffer, sizeof(buffer), docker_pipe) != NULL) {
                       docker_result += buffer;
                   }
                   pclose(docker_pipe);
               }
               
               if (!docker_result.empty()) {
                   container_id = docker_result;
                   container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                   std::cout << "[INFO] 通过 docker 找到容器 ID: " << container_id << std::endl;
               }
           }
           
           if (container_id.empty()) {
               json resp;
               resp["success"] = false;
               resp["error"] = "未找到容器: Pod " + pod_name + " 中的容器 " + container_name + 
                               "，请确保 Pod 处于 Running 状态";
               return crow::response(404, resp.dump());
           }
           
           // 移除容器运行时前缀
           size_t pos = container_id.find("://");
           if (pos != std::string::npos) {
               container_id = container_id.substr(pos + 3);
           }
           container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
           container_id.erase(std::remove(container_id.begin(), container_id.end(), '\r'), container_id.end());
           
           std::cout << "[INFO] 最终使用的容器 ID: " << container_id << std::endl;
           
           // 使用 crictl 创建镜像
           std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
           std::string commit_output = "";
           FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
           if (commit_pipe) {
               while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                   commit_output += buffer;
               }
               pclose(commit_pipe);
           }
           
           std::cout << "[INFO] commit 输出: " << commit_output << std::endl;
           
           // 检查是否成功
           bool success = (commit_output.find("error") == std::string::npos && 
                          commit_output.find("Error") == std::string::npos &&
                          commit_output.find("failed") == std::string::npos);
           
           json resp;
           if (success) {
               resp["success"] = true;
               resp["imageId"] = target_image;
               resp["message"] = "镜像创建成功: " + target_image;
               return crow::response(resp.dump());
           } else {
               resp["success"] = false;
               resp["error"] = "创建镜像失败: " + commit_output;
               return crow::response(500, resp.dump());
           }
           
       } catch (const std::exception& e) {
           std::cerr << "[ERROR] 镜像打包异常: " << e.what() << std::endl;
           json resp;
           resp["success"] = false;
           resp["error"] = std::string(e.what());
           return crow::response(500, resp.dump());
       }
   });
   
   // 调试接口：查看 Pod 详情
   CROW_ROUTE(app, "/api/debug/pod/<string>/<string>").methods("GET"_method)([](const std::string& ns, const std::string& pod_name) {
       std::string cmd = "kubectl get pod " + pod_name + " -n " + ns + " -o json 2>/dev/null";
       std::string result = "";
       char buffer[4096];
       FILE* pipe = popen(cmd.c_str(), "r");
       if (pipe) {
           while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
               result += buffer;
           }
           pclose(pipe);
       }
   
       // 提取关键信息
       json info;
       info["pod_name"] = pod_name;
       info["namespace"] = ns;
   
       if (!result.empty()) {
           try {
               auto pod = json::parse(result);
               json containers = json::array();
               if (pod.contains("spec") && pod["spec"].contains("containers")) {
                   for (const auto& c : pod["spec"]["containers"]) {
                       containers.push_back(safeGetString(c, "name"));
                   }
               }
               info["containers"] = containers;
   
               json container_statuses = json::array();
               if (pod.contains("status") && pod["status"].contains("containerStatuses")) {
                   for (const auto& cs : pod["status"]["containerStatuses"]) {
                       json status;
                       status["name"] = safeGetString(cs, "name");
                       status["ready"] = safeGetString(cs, "ready");
                       status["restartCount"] = safeGetInt(cs, "restartCount");
                       status["containerID"] = safeGetString(cs, "containerID");
                       container_statuses.push_back(status);
                   }
               }
               info["container_statuses"] = container_statuses;
               info["phase"] = safeGetString(pod["status"], "phase");
           } catch (...) {
               info["error"] = "解析失败";
           }
       } else {
           info["error"] = "Pod 不存在";
       }
   
       return crow::response(info.dump());
   });
   


    
    // 从 Pod 中的容器创建镜像
    CROW_ROUTE(app, "/api/image-pack/create3").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数: podName 和 targetImage 不能为空";
                return crow::response(400, resp.dump());
            }
            
            // 默认容器名
            if (container_name.empty()) {
                container_name = "app";
            }
            
            std::cout << "[INFO] 开始打包镜像 - Namespace: " << namespace_ 
                      << ", Pod: " << pod_name 
                      << ", Container: " << container_name 
                      << ", Target: " << target_image << std::endl;
            
            // 使用 kubectl 获取容器 ID
            std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                            " -o jsonpath='{.status.containerStatuses[?(@.name==\"" + container_name + "\")].containerID}' 2>/dev/null";
            
            std::string container_id = "";
            char buffer[4096];
            FILE* pipe = popen(get_container_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    container_id += buffer;
                }
                pclose(pipe);
            }
            
            // 移除容器运行时前缀
            if (!container_id.empty()) {
                size_t pos = container_id.find("://");
                if (pos != std::string::npos) {
                    container_id = container_id.substr(pos + 3);
                }
                // 移除可能的引号和换行符
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\''), container_id.end());
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\r'), container_id.end());
            }
            
            if (container_id.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "未找到容器: " + pod_name + "/" + container_name;
                return crow::response(404, resp.dump());
            }
            
            std::cout << "[INFO] 找到容器 ID: " << container_id << std::endl;
            
            // 使用 crictl 创建镜像
            std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
            std::string commit_output = "";
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            // 检查是否成功
            bool success = (commit_output.find("error") == std::string::npos && 
                           commit_output.find("Error") == std::string::npos &&
                           commit_output.find("failed") == std::string::npos);
            
            json resp;
            if (success || commit_output.empty()) {
                resp["success"] = true;
                resp["imageId"] = target_image;
                resp["message"] = "镜像创建成功: " + target_image;
                return crow::response(resp.dump());
            } else {
                resp["success"] = false;
                resp["error"] = "创建镜像失败: " + commit_output;
                return crow::response(500, resp.dump());
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 镜像打包异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    
    // 推送镜像到 Harbor（修复版）
    CROW_ROUTE(app, "/api/image-pack/push6").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string source_image = body.value("sourceImage", "");
            std::string target_registry = body.value("targetRegistry", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string final_image_name = body.value("finalImageName", source_image);
            
            if (source_image.empty() || target_registry.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数: sourceImage 和 targetRegistry 不能为空";
                return crow::response(400, resp.dump());
            }
            
            // 确保仓库地址以 / 结尾
            std::string registry = target_registry;
            if (registry.back() != '/') {
                registry += '/';
            }
            
            // 解析源镜像名称，提取镜像名和标签
            std::string image_name = source_image;
            std::string image_tag = "latest";
            size_t colon_pos = source_image.find_last_of(':');
            if (colon_pos != std::string::npos) {
                image_name = source_image.substr(0, colon_pos);
                image_tag = source_image.substr(colon_pos + 1);
            }
            
            // 清理镜像名：移除开头的非法字符
            while (!image_name.empty() && (image_name[0] == '-' || image_name[0] == '_' || image_name[0] == '.')) {
                image_name = image_name.substr(1);
            }
            
            // 构建完整的目标镜像名称
            std::string full_target = registry + image_name + ":" + image_tag;
            
            std::cout << "[INFO] 源镜像: " << source_image << std::endl;
            std::cout << "[INFO] 镜像名: " << image_name << std::endl;
            std::cout << "[INFO] 标签: " << image_tag << std::endl;
            std::cout << "[INFO] 目标镜像: " << full_target << std::endl;
            
            char buffer[256];
            
            // 使用 docker 或 nerdctl
            std::string runtime = "docker";
            std::string check_cmd = "which docker 2>/dev/null";
            FILE* check_pipe = popen(check_cmd.c_str(), "r");
            if (!check_pipe) {
                runtime = "nerdctl";
            } else {
                std::string check_result;
                while (fgets(buffer, sizeof(buffer), check_pipe) != NULL) {
                    check_result += buffer;
                }
                pclose(check_pipe);
                if (check_result.empty()) {
                    runtime = "nerdctl";
                }
            }
            
            std::cout << "[INFO] 使用运行时: " << runtime << std::endl;
            
            // 打标签
            std::string tag_cmd = runtime + " tag " + source_image + " " + full_target + " 2>&1";
            std::cout << "[INFO] 执行打标签: " << tag_cmd << std::endl;
            FILE* tag_pipe = popen(tag_cmd.c_str(), "r");
            if (tag_pipe) {
                while (fgets(buffer, sizeof(buffer), tag_pipe) != NULL) {
                    // 忽略输出
                }
                pclose(tag_pipe);
            }
            
            // 如果需要登录
            if (!username.empty() && !password.empty()) {
                std::string registry_host = registry;
                // 提取主机名和端口
                size_t scheme_pos = registry_host.find("://");
                if (scheme_pos != std::string::npos) {
                    registry_host = registry_host.substr(scheme_pos + 3);
                }
                size_t slash_pos = registry_host.find('/');
                if (slash_pos != std::string::npos) {
                    registry_host = registry_host.substr(0, slash_pos);
                }
                
                std::cout << "[INFO] 登录 Harbor: " << registry_host << std::endl;
                std::string login_cmd = runtime + " login " + registry_host + " -u " + username + " -p " + password + " 2>&1";
                std::string login_output;
                FILE* login_pipe = popen(login_cmd.c_str(), "r");
                if (login_pipe) {
                    while (fgets(buffer, sizeof(buffer), login_pipe) != NULL) {
                        login_output += buffer;
                    }
                    pclose(login_pipe);
                }
                std::cout << "[INFO] 登录输出: " << login_output << std::endl;
            }
            
            // 推送镜像
            std::string push_cmd = runtime + " push " + full_target + " 2>&1";
            std::cout << "[INFO] 执行推送: " << push_cmd << std::endl;
            std::string push_output;
            FILE* push_pipe = popen(push_cmd.c_str(), "r");
            if (push_pipe) {
                while (fgets(buffer, sizeof(buffer), push_pipe) != NULL) {
                    push_output += buffer;
                    std::cout << "[INFO] 推送输出: " << buffer;
                }
                pclose(push_pipe);
            }
            
            json resp;
            // 检查推送是否成功
            if (push_output.find("error") == std::string::npos && 
                push_output.find("Error") == std::string::npos &&
                push_output.find("unauthorized") == std::string::npos) {
                resp["success"] = true;
                resp["fullImageName"] = full_target;
                resp["message"] = "镜像推送成功: " + full_target;
                std::cout << "[INFO] 推送成功: " << full_target << std::endl;
                return crow::response(resp.dump());
            } else {
                resp["success"] = false;
                resp["error"] = "推送镜像失败: " + push_output;
                std::cerr << "[ERROR] 推送失败: " << push_output << std::endl;
                return crow::response(500, resp.dump());
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 推送镜像异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });

    // 推送镜像到 Harbor
    CROW_ROUTE(app, "/api/image-pack/push5").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
    
            std::string source_image = body.value("sourceImage", "");
            std::string target_registry = body.value("targetRegistry", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string final_image_name = body.value("finalImageName", source_image);
    
            if (source_image.empty() || target_registry.empty()) {
                json resp;
                resp["success"] = false;
                resp["error"] = "缺少必要参数: sourceImage 和 targetRegistry 不能为空";
                return crow::response(400, resp.dump());
            }
    
            // 构建完整的目标镜像名称
            std::string full_target = target_registry;
            if (full_target.back() != '/') full_target += '/';
            full_target += final_image_name;
    
            char buffer[256];
    
            // 使用 docker 或 nerdctl
            std::string runtime = "docker";
            std::string check_cmd = "which docker 2>/dev/null";
            FILE* check_pipe = popen(check_cmd.c_str(), "r");
            if (!check_pipe) {
                runtime = "nerdctl";
            } else {
                std::string check_result;
                while (fgets(buffer, sizeof(buffer), check_pipe) != NULL) {
                    check_result += buffer;
                }
                pclose(check_pipe);
                if (check_result.empty()) {
                    runtime = "nerdctl";
                }
            }
    
            // 打标签
            std::string tag_cmd = runtime + " tag " + source_image + " " + full_target + " 2>&1";
            FILE* tag_pipe = popen(tag_cmd.c_str(), "r");
            if (tag_pipe) {
                while (fgets(buffer, sizeof(buffer), tag_pipe) != NULL) {
                    // 忽略输出
                }
                pclose(tag_pipe);
            }
    
            // 如果需要登录
            if (!username.empty() && !password.empty()) {
                std::string registry_host = target_registry;
                size_t slash_pos = registry_host.find('/');
                if (slash_pos != std::string::npos) {
                    registry_host = registry_host.substr(0, slash_pos);
                }
    
                std::string login_cmd = runtime + " login " + registry_host + " -u " + username + " -p " + password + " 2>&1";
                std::string login_output;
                FILE* login_pipe = popen(login_cmd.c_str(), "r");
                if (login_pipe) {
                    while (fgets(buffer, sizeof(buffer), login_pipe) != NULL) {
                        login_output += buffer;
                    }
                    pclose(login_pipe);
                }
            }
    
            // 推送镜像
            std::string push_cmd = runtime + " push " + full_target + " 2>&1";
            std::string push_output;
            FILE* push_pipe = popen(push_cmd.c_str(), "r");
            if (push_pipe) {
                while (fgets(buffer, sizeof(buffer), push_pipe) != NULL) {
                    push_output += buffer;
                }
                pclose(push_pipe);
            }
    
            json resp;
            // 检查推送是否成功
            if (push_output.find("error") == std::string::npos &&
                push_output.find("Error") == std::string::npos) {
                resp["success"] = true;
                resp["fullImageName"] = full_target;
                resp["message"] = "镜像推送成功: " + full_target;
                return crow::response(resp.dump());
            } else {
                resp["success"] = false;
                resp["error"] = "推送镜像失败: " + push_output;
                return crow::response(500, resp.dump());
            }
    
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 推送镜像异常: " << e.what() << std::endl;
            json resp;
            resp["success"] = false;
            resp["error"] = std::string(e.what());
            return crow::response(500, resp.dump());
        }
    });


    // ========== 镜像打包 API（改进版）==========
    
    // 从 Pod 中的容器创建镜像 - 支持 StatefulSet 和其他类型 Pod
    CROW_ROUTE(app, "/api/image-pack/create2").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                return crow::response(400, R"({"success":false,"error":"缺少必要参数: podName 和 targetImage 不能为空"})");
            }
            
            // 默认容器名
            if (container_name.empty()) {
                container_name = "app";
            }
            
            std::cout << "[INFO] 开始打包镜像 - Namespace: " << namespace_ 
                      << ", Pod: " << pod_name 
                      << ", Container: " << container_name 
                      << ", Target: " << target_image << std::endl;
            
            // 方法1: 使用 kubectl 获取容器 ID
            std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                            " -o json 2>/dev/null";
            
            std::string pod_json = "";
            char buffer[4096];
            FILE* pipe = popen(get_container_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    pod_json += buffer;
                }
                pclose(pipe);
            }
            
            if (pod_json.empty()) {
                return crow::response(404, R"({"success":false,"error":"未找到 Pod: )" + pod_name + R"("})");
            }
            
            // 解析 JSON 查找容器 ID
            std::string container_id = "";
            try {
                auto pod_info = json::parse(pod_json);
                
                // 查找容器状态
                if (pod_info.contains("status") && pod_info["status"].contains("containerStatuses")) {
                    for (const auto& cs : pod_info["status"]["containerStatuses"]) {
                        std::string cs_name = safeGetString(cs, "name");
                        if (cs_name == container_name) {
                            container_id = safeGetString(cs, "containerID");
                            break;
                        }
                    }
                }
                
                // 如果没找到，尝试使用 containerd 的命名格式
                if (container_id.empty() && pod_info.contains("status") && pod_info["status"].contains("containerStatuses")) {
                    // 取第一个运行中的容器
                    for (const auto& cs : pod_info["status"]["containerStatuses"]) {
                        if (safeGetString(cs, "state", "running") == "running") {
                            container_id = safeGetString(cs, "containerID");
                            std::cout << "[INFO] 使用第一个运行中的容器: " << safeGetString(cs, "name") << std::endl;
                            break;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] 解析 Pod JSON 失败: " << e.what() << std::endl;
            }
            
            // 移除容器运行时前缀
            if (!container_id.empty()) {
                size_t pos = container_id.find("://");
                if (pos != std::string::npos) {
                    container_id = container_id.substr(pos + 3);
                }
                // 移除可能的引号和换行符
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\''), container_id.end());
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                container_id.erase(std::remove(container_id.begin(), container_id.end(), '\r'), container_id.end());
            }
            
            if (container_id.empty()) {
                // 尝试使用 crictl 直接查找
                std::string crictl_cmd = "crictl ps --name " + pod_name + " --namespace " + namespace_ + 
                                         " --quiet 2>/dev/null";
                std::string crictl_result = "";
                FILE* crictl_pipe = popen(crictl_cmd.c_str(), "r");
                if (crictl_pipe) {
                    while (fgets(buffer, sizeof(buffer), crictl_pipe) != NULL) {
                        crictl_result += buffer;
                    }
                    pclose(crictl_pipe);
                }
                
                if (!crictl_result.empty()) {
                    container_id = crictl_result;
                    container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                    std::cout << "[INFO] 通过 crictl 找到容器: " << container_id << std::endl;
                } else {
                    // 最后尝试使用 docker
                    std::string docker_cmd = "docker ps --format '{{.ID}}' --filter name=" + pod_name + " 2>/dev/null | head -1";
                    std::string docker_result = "";
                    FILE* docker_pipe = popen(docker_cmd.c_str(), "r");
                    if (docker_pipe) {
                        while (fgets(buffer, sizeof(buffer), docker_pipe) != NULL) {
                            docker_result += buffer;
                        }
                        pclose(docker_pipe);
                    }
                    
                    if (!docker_result.empty()) {
                        container_id = docker_result;
                        container_id.erase(std::remove(container_id.begin(), container_id.end(), '\n'), container_id.end());
                        std::cout << "[INFO] 通过 docker 找到容器: " << container_id << std::endl;
                    }
                }
            }
            
            if (container_id.empty()) {
                return crow::response(404, R"({"success":false,"error":"未找到容器: )" + pod_name + "/" + container_name + 
                                      R"(, 请确保 Pod 处于 Running 状态"})");
            }
            
            std::cout << "[INFO] 找到容器 ID: " << container_id << std::endl;
            
            // 使用 crictl 创建镜像
            std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
            std::string commit_output = "";
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            // 检查是否成功
            bool success = (commit_output.find("error") == std::string::npos && 
                           commit_output.find("Error") == std::string::npos &&
                           commit_output.find("failed") == std::string::npos);
            
            if (success || commit_output.empty()) {
                // 验证镜像是否创建成功
                std::string inspect_cmd = "crictl images | grep " + target_image + " 2>/dev/null";
                std::string inspect_result = "";
                FILE* inspect_pipe = popen(inspect_cmd.c_str(), "r");
                if (inspect_pipe) {
                    while (fgets(buffer, sizeof(buffer), inspect_pipe) != NULL) {
                        inspect_result += buffer;
                    }
                    pclose(inspect_pipe);
                }
                
                if (!inspect_result.empty()) {
                    return crow::response(R"({"success":true,"imageId":")" + target_image + 
                                         R"(","message":"镜像创建成功: )" + target_image + R"("})");
                } else if (commit_output.empty()) {
                    return crow::response(R"({"success":true,"imageId":")" + target_image + 
                                         R"(","message":"镜像创建成功: )" + target_image + R"("})");
                } else {
                    return crow::response(500, R"({"success":false,"error":")" + commit_output + R"("})");
                }
            } else {
                return crow::response(500, R"({"success":false,"error":"创建镜像失败: )" + commit_output + R"("})");
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] 镜像打包异常: " << e.what() << std::endl;
            return crow::response(500, R"({"success":false,"error":")" + std::string(e.what()) + R"("})");
        }
    });
    
    // 获取 Pod 中的容器列表（改进版）
    CROW_ROUTE(app, "/api/pods/<string>/<string>/containers6").methods("GET"_method)([](const std::string& ns, const std::string& pod_name) {
        try {
            std::string cmd = "kubectl get pod " + pod_name + " -n " + ns + 
                              " -o jsonpath='{range .spec.containers[*]}{.name}{\"\\n\"}{end}' 2>/dev/null";
            
            std::string result = "";
            char buffer[256];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    result += buffer;
                }
                pclose(pipe);
            }
            
            json containers = json::array();
            std::istringstream iss(result);
            std::string container_name;
            while (std::getline(iss, container_name)) {
                if (!container_name.empty()) {
                    containers.push_back(container_name);
                }
            }
            
            // 如果没有找到容器，返回默认的 app
            if (containers.empty()) {
                containers.push_back("app");
            }
            
            return crow::response(R"({"containers":)" + containers.dump() + "}");
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"error":")" + std::string(e.what()) + R"("})");
        }
    });

    // 调试接口：列出所有运行中的容器
    CROW_ROUTE(app, "/api/debug/containers").methods("GET"_method)([](const crow::request& req) {
        const char* ns_param = req.url_params.get("namespace");
        std::string namespace_ = ns_param ? std::string(ns_param) : "default";
        
        std::string cmd = "kubectl get pods -n " + namespace_ + " -o wide 2>/dev/null";
        std::string result = "";
        char buffer[4096];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
        }
        
        return crow::response(result);
    });
    
    // 调试接口：检查 crictl 是否可用
    CROW_ROUTE(app, "/api/debug/crictl-check").methods("GET"_method)([]() {
        std::string result = "";
        char buffer[256];
        
        // 检查 crictl
        FILE* pipe = popen("which crictl 2>/dev/null", "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
        }
        
        json resp;
        resp["crictl_path"] = result.empty() ? "not found" : result;
        
        // 检查 crictl 是否能连接
        std::string ps_result = "";
        pipe = popen("crictl ps 2>&1 | head -5", "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                ps_result += buffer;
            }
            pclose(pipe);
        }
        resp["crictl_test"] = ps_result;
        
        return crow::response(resp.dump());
    });





    // ========== 镜像打包 API ==========
    
    // 从 Pod 中的容器创建镜像
    CROW_ROUTE(app, "/api/image-pack/create1").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                return crow::response(400, R"({"success":false,"error":"缺少必要参数: podName 和 targetImage 不能为空"})");
            }
            
            // 获取容器 ID
            std::string get_container_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                                            " -o jsonpath='{.status.containerStatuses[?(@.name==\"" + container_name + "\")].containerID}' 2>/dev/null";
            
            std::string container_id = "";
            char buffer[256];
            FILE* pipe = popen(get_container_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    container_id += buffer;
                }
                pclose(pipe);
            }
            
            // 移除容器运行时前缀
            if (!container_id.empty()) {
                size_t pos = container_id.find("://");
                if (pos != std::string::npos) {
                    container_id = container_id.substr(pos + 3);
                }
                // 移除可能的引号
                if (!container_id.empty() && container_id.front() == '\'') {
                    container_id = container_id.substr(1, container_id.length() - 2);
                }
            }
            
            if (container_id.empty()) {
                return crow::response(404, R"({"success":false,"error":"未找到容器: )" + pod_name + "/" + container_name + R"("})");
            }
            
            // 使用 crictl 或 docker 创建镜像
            // 尝试使用 crictl
            std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
            std::string commit_output = "";
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            // 检查是否成功
            bool success = (commit_output.find("error") == std::string::npos && 
                           commit_output.find("Error") == std::string::npos);
            
            if (success) {
                return crow::response(R"({"success":true,"imageId":")" + target_image + R"(","message":"镜像创建成功: )" + target_image + R"("})");
            } else {
                return crow::response(500, R"({"success":false,"error":")" + commit_output + R"("})");
            }
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"success":false,"error":")" + std::string(e.what()) + R"("})");
        }
    });
    
    // 推送镜像到 Harbor
    CROW_ROUTE(app, "/api/image-pack/push1").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string source_image = body.value("sourceImage", "");
            std::string target_registry = body.value("targetRegistry", "");
            std::string username = body.value("username", "");
            std::string password = body.value("password", "");
            std::string final_image_name = body.value("finalImageName", source_image);
            
            if (source_image.empty() || target_registry.empty()) {
                return crow::response(400, R"({"success":false,"error":"缺少必要参数: sourceImage 和 targetRegistry 不能为空"})");
            }
            
            // 构建完整的目标镜像名称
            std::string full_target = target_registry;
            if (full_target.back() != '/') full_target += '/';
            full_target += final_image_name;
            
            char buffer[256];
            std::string output;
            
            // 尝试使用 nerdctl，如果没有则使用 docker
            std::string runtime = "nerdctl";
            std::string check_cmd = "which nerdctl 2>/dev/null";
            FILE* check_pipe = popen(check_cmd.c_str(), "r");
            if (check_pipe) {
                std::string result;
                while (fgets(buffer, sizeof(buffer), check_pipe) != NULL) {
                    result += buffer;
                }
                pclose(check_pipe);
                if (result.empty()) {
                    runtime = "docker";
                }
            } else {
                runtime = "docker";
            }
            
            // 打标签
            std::string tag_cmd = runtime + " tag " + source_image + " " + full_target + " 2>&1";
            FILE* tag_pipe = popen(tag_cmd.c_str(), "r");
            if (tag_pipe) {
                while (fgets(buffer, sizeof(buffer), tag_pipe) != NULL) {
                    output += buffer;
                }
                pclose(tag_pipe);
            }
            
            // 如果需要登录
            if (!username.empty() && !password.empty()) {
                std::string registry_host = target_registry;
                size_t slash_pos = registry_host.find('/');
                if (slash_pos != std::string::npos) {
                    registry_host = registry_host.substr(0, slash_pos);
                }
                
                std::string login_cmd = runtime + " login " + registry_host + " -u " + username + " -p " + password + " 2>&1";
                std::string login_output;
                FILE* login_pipe = popen(login_cmd.c_str(), "r");
                if (login_pipe) {
                    while (fgets(buffer, sizeof(buffer), login_pipe) != NULL) {
                        login_output += buffer;
                    }
                    pclose(login_pipe);
                }
                
                if (login_output.find("Login Succeeded") == std::string::npos &&
                    login_output.find("Login succeeded") == std::string::npos) {
                    return crow::response(401, R"({"success":false,"error":"登录 Harbor 失败: )" + login_output + R"("})");
                }
            }
            
            // 推送镜像
            std::string push_cmd = runtime + " push " + full_target + " 2>&1";
            std::string push_output;
            FILE* push_pipe = popen(push_cmd.c_str(), "r");
            if (push_pipe) {
                while (fgets(buffer, sizeof(buffer), push_pipe) != NULL) {
                    push_output += buffer;
                }
                pclose(push_pipe);
            }
            
            // 检查推送是否成功
            if (push_output.find("error") == std::string::npos && 
                push_output.find("Error") == std::string::npos) {
                return crow::response(R"({"success":true,"fullImageName":")" + full_target + R"(","message":"镜像推送成功: )" + full_target + R"("})");
            } else {
                return crow::response(500, R"({"success":false,"error":"推送镜像失败: )" + push_output + R"("})");
            }
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"success":false,"error":")" + std::string(e.what()) + R"("})");
        }
    });
    
    // 获取 Pod 中的容器列表
    CROW_ROUTE(app, "/api/pods/<string>/<string>/containers1").methods("GET"_method)([](const std::string& ns, const std::string& pod_name) {
        try {
            std::string cmd = "kubectl get pod " + pod_name + " -n " + ns + 
                              " -o jsonpath='{.spec.containers[*].name}' 2>/dev/null";
            
            std::string result = "";
            char buffer[256];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    result += buffer;
                }
                pclose(pipe);
            }
            
            // 解析容器名称（空格分隔）
            json containers = json::array();
            std::istringstream iss(result);
            std::string container_name;
            while (iss >> container_name) {
                containers.push_back(container_name);
            }
            
            return crow::response(R"({"containers":)" + containers.dump() + "}");
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"error":")" + std::string(e.what()) + R"("})");
        }
    });
    
    // 流式打包镜像（实时返回日志）- 用于前端实时显示进度
    CROW_ROUTE(app, "/api/image-pack/stream").methods("POST"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);
        
        std::string namespace_ = body.value("namespace", "default");
        std::string pod_name = body.value("podName", "");
        std::string container_name = body.value("containerName", "");
        std::string target_image = body.value("targetImage", "");
        
        crow::response res;
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        
        std::stringstream output;
        
        if (pod_name.empty() || target_image.empty()) {
            output << "data: " << json({{"type", "error"}, {"message", "缺少必要参数"}}).dump() << "\n\n";
            res.write(output.str());
            return res;
        }
        
        output << "data: " << json({{"type", "info"}, {"message", "开始打包镜像..."}}).dump() << "\n\n";
        output << "data: " << json({{"type", "info"}, {"message", "Pod: " + pod_name}}).dump() << "\n\n";
        output << "data: " << json({{"type", "info"}, {"message", "容器: " + container_name}}).dump() << "\n\n";
        
        // 获取容器 ID
        output << "data: " << json({{"type", "info"}, {"message", "正在获取容器 ID..."}}).dump() << "\n\n";
        
        std::string get_cmd = "kubectl get pod " + pod_name + " -n " + namespace_ + 
                              " -o jsonpath='{.status.containerStatuses[?(@.name==\"" + container_name + "\")].containerID}' 2>/dev/null";
        
        std::string container_id = "";
        char buffer[256];
        FILE* pipe = popen(get_cmd.c_str(), "r");
        if (pipe) {
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                container_id += buffer;
            }
            pclose(pipe);
        }
        
        if (container_id.empty()) {
            output << "data: " << json({{"type", "error"}, {"message", "未找到容器"}}).dump() << "\n\n";
            res.write(output.str());
            return res;
        }
        
        // 移除前缀
        size_t pos = container_id.find("://");
        if (pos != std::string::npos) {
            container_id = container_id.substr(pos + 3);
        }
        if (!container_id.empty() && container_id.front() == '\'') {
            container_id = container_id.substr(1, container_id.length() - 2);
        }
        
        output << "data: " << json({{"type", "info"}, {"message", "容器 ID: " + container_id.substr(0, 12) + "..."}}).dump() << "\n\n";
        
        // 创建镜像
        output << "data: " << json({{"type", "info"}, {"message", "正在创建镜像: " + target_image}}).dump() << "\n\n";
        
        std::string commit_cmd = "crictl commit " + container_id + " " + target_image + " 2>&1";
        FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
        if (commit_pipe) {
            while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                std::string line(buffer);
                output << "data: " << json({{"type", "output"}, {"message", line}}).dump() << "\n\n";
            }
            pclose(commit_pipe);
        }
        
        output << "data: " << json({{"type", "success"}, {"message", "镜像创建成功: " + target_image}}).dump() << "\n\n";
        
        res.write(output.str());
        return res;
    });
    
    // 直接使用 docker commit 从运行中的容器创建镜像（备用方案）
    CROW_ROUTE(app, "/api/image-pack/create-docker").methods("POST"_method)([](const crow::request& req) {
        try {
            auto body = json::parse(req.body);
            
            std::string namespace_ = body.value("namespace", "default");
            std::string pod_name = body.value("podName", "");
            std::string container_name = body.value("containerName", "");
            std::string target_image = body.value("targetImage", "");
            
            if (pod_name.empty() || target_image.empty()) {
                return crow::response(400, R"({"success":false,"error":"缺少必要参数"})");
            }
            
            // 使用 docker ps 查找容器
            std::string find_cmd = "docker ps --filter label=io.kubernetes.pod.namespace=" + namespace_ + 
                                   " --filter label=io.kubernetes.pod.name=" + pod_name + 
                                   " --format '{{.ID}}' 2>/dev/null | head -1";
            
            std::string container_id = "";
            char buffer[256];
            FILE* pipe = popen(find_cmd.c_str(), "r");
            if (pipe) {
                while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                    container_id += buffer;
                }
                pclose(pipe);
            }
            
            // 移除换行符
            if (!container_id.empty()) {
                container_id.erase(container_id.find_last_not_of(" \n\r\t") + 1);
            }
            
            if (container_id.empty()) {
                return crow::response(404, R"({"success":false,"error":"未找到容器"})");
            }
            
            // 提交镜像
            std::string commit_cmd = "docker commit " + container_id + " " + target_image + " 2>&1";
            std::string commit_output = "";
            FILE* commit_pipe = popen(commit_cmd.c_str(), "r");
            if (commit_pipe) {
                while (fgets(buffer, sizeof(buffer), commit_pipe) != NULL) {
                    commit_output += buffer;
                }
                pclose(commit_pipe);
            }
            
            return crow::response(R"({"success":true,"imageId":")" + target_image + R"(","message":"镜像创建成功: )" + target_image + R"("})");
            
        } catch (const std::exception& e) {
            return crow::response(500, R"({"success":false,"error":")" + std::string(e.what()) + R"("})");
        }
    });







    // ========== 文件浏览器 API ==========
    
    // 列出目录内容
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/list").methods("GET"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        std::string targetPath = req.url_params.get("path") ? std::string(req.url_params.get("path")) : "/";
        
        // 使用 kubectl exec 执行 ls -la
        std::string cmd = "kubectl exec " + name + " -n " + ns + " -- ls -la " + targetPath + " 2>/dev/null | tail -n +2";
        std::string result = "";
        char buffer[128];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return crow::response(500, "{\"error\":\"exec failed\"}");
        }
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        pclose(pipe);
        
        json files = json::array();
        std::istringstream iss(result);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            
            // 解析 ls -la 输出
            // 格式: drwxr-xr-x 2 root root 4096 May 27 10:00 dirname
            bool isDir = (line[0] == 'd');
            
            // 提取文件名（最后一个字段）
            size_t lastSpace = line.find_last_of(' ');
            if (lastSpace != std::string::npos) {
                std::string fileName = line.substr(lastSpace + 1);
                if (fileName != "." && fileName != "..") {
                    files.push_back({
                        {"name", fileName},
                        {"isDir", isDir},
                        {"path", targetPath == "/" ? "/" + fileName : targetPath + "/" + fileName}
                    });
                }
            }
        }
        
        return crow::response(files.dump());
    });
    
    // 读取文件内容
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/view").methods("GET"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        std::string filePath = req.url_params.get("path") ? std::string(req.url_params.get("path")) : "";
        if (filePath.empty()) {
            return crow::response(400, "{\"error\":\"path required\"}");
        }
        
        std::string cmd = "kubectl exec " + name + " -n " + ns + " -- cat " + filePath + " 2>/dev/null";
        std::string result = "";
        char buffer[4096];
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return crow::response(500, "{\"error\":\"exec failed\"}");
        }
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        pclose(pipe);
        
        return crow::response(result);
    });
    
    // ========== 下载文件 ==========
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/download").methods("GET"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        std::string filePath = req.url_params.get("path") ? std::string(req.url_params.get("path")) : "";
        if (filePath.empty()) {
            return crow::response(400, "{\"error\":\"path required\"}");
        }
        
        // 使用 kubectl cp 读取文件内容
        std::string tempFile = "/tmp/k8s_download_" + std::to_string(time(nullptr));
        std::string cmd = "kubectl cp " + ns + "/" + name + ":" + filePath + " " + tempFile + " 2>&1";
        system(cmd.c_str());
        
        // 读取临时文件
        std::ifstream ifs(tempFile, std::ios::binary);
        if (!ifs.is_open()) {
            return crow::response(404, "{\"error\":\"file not found\"}");
        }
        
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();
        unlink(tempFile.c_str());
        
        // 提取文件名
        std::string filename = filePath.substr(filePath.find_last_of('/') + 1);
        
        crow::response res;
        res.set_header("Content-Type", "application/octet-stream");
        res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        res.write(content);
        return res;
    });
   

    
    // ========== 上传文件到 Pod ==========
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/upload").methods("POST"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        // 从 URL 参数获取目标路径
        std::string targetPath = req.url_params.get("targetPath") ? std::string(req.url_params.get("targetPath")) : "/";
        
        // 解析 multipart/form-data
        std::string boundary = req.get_header_value("Content-Type");
        size_t pos = boundary.find("boundary=");
        if (pos == std::string::npos) {
            return crow::response(400, "{\"error\":\"Invalid content type\"}");
        }
        boundary = "--" + boundary.substr(pos + 9);
        
        std::string body = req.body;
        std::string filename;
        std::vector<char> fileData;
        
        // 查找文件名
        pos = body.find("filename=\"");
        if (pos != std::string::npos) {
            pos += 10;
            size_t end = body.find("\"", pos);
            filename = body.substr(pos, end - pos);
            
            // 查找文件内容
            pos = body.find("\r\n\r\n", end);
            if (pos != std::string::npos) {
                pos += 4;
                end = body.find(boundary, pos);
                std::string content = body.substr(pos, end - pos - 2);
                fileData.assign(content.begin(), content.end());
            }
        }
        
        if (filename.empty() || fileData.empty()) {
            return crow::response(400, "{\"error\":\"No file data\"}");
        }
        
        // 写入临时文件
        std::string tempFile = "/tmp/k8s_upload_" + std::to_string(time(nullptr));
        std::ofstream ofs(tempFile, std::ios::binary);
        ofs.write(fileData.data(), fileData.size());
        ofs.close();
        
        // 构建目标路径
        std::string targetFile = targetPath;
        if (targetFile.back() != '/') targetFile += '/';
        targetFile += filename;
        
        // 使用 kubectl cp 复制到 Pod
        std::string cmd = "kubectl cp " + tempFile + " " + ns + "/" + name + ":" + targetFile + " 2>&1";
        int ret = system(cmd.c_str());
        
        // 删除临时文件
        unlink(tempFile.c_str());
        
        if (ret == 0) {
            return crow::response("{\"success\":true,\"filename\":\"" + filename + "\",\"path\":\"" + targetFile + "\"}");
        } else {
            return crow::response(500, "{\"error\":\"upload failed\"}");
        }
    });
    


    // ========== 上传文件到 Pod ==========
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/upload1").methods("POST"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        // 获取目标路径
        std::string targetPath = req.get_header_value("X-Target-Path");
        if (targetPath.empty()) {
            targetPath = "/";
        }
        
        // 解析 multipart/form-data
        std::string boundary = req.get_header_value("Content-Type");
        size_t pos = boundary.find("boundary=");
        if (pos == std::string::npos) {
            return crow::response(400, "{\"error\":\"Invalid content type\"}");
        }
        boundary = "--" + boundary.substr(pos + 9);
        
        std::string body = req.body;
        std::string filename;
        std::vector<char> fileData;
        
        // 查找文件名
        pos = body.find("filename=\"");
        if (pos != std::string::npos) {
            pos += 10;
            size_t end = body.find("\"", pos);
            filename = body.substr(pos, end - pos);
            
            // 查找文件内容
            pos = body.find("\r\n\r\n", end);
            if (pos != std::string::npos) {
                pos += 4;
                end = body.find(boundary, pos);
                std::string content = body.substr(pos, end - pos - 2);
                fileData.assign(content.begin(), content.end());
            }
        }
        
        if (filename.empty() || fileData.empty()) {
            return crow::response(400, "{\"error\":\"No file data\"}");
        }
        
        // 写入临时文件
        std::string tempFile = "/tmp/k8s_upload_" + std::to_string(time(nullptr));
        std::ofstream ofs(tempFile, std::ios::binary);
        ofs.write(fileData.data(), fileData.size());
        ofs.close();
        
        // 使用 kubectl cp 复制到 Pod
        std::string targetFile = targetPath;
        if (targetFile.back() != '/') targetFile += '/';
        targetFile += filename;
        
        std::string cmd = "kubectl cp " + tempFile + " " + ns + "/" + name + ":" + targetFile + " 2>&1";
        int ret = system(cmd.c_str());
        
        // 删除临时文件
        unlink(tempFile.c_str());
        
        if (ret == 0) {
            return crow::response("{\"success\":true,\"filename\":\"" + filename + "\",\"path\":\"" + targetFile + "\"}");
        } else {
            return crow::response(500, "{\"error\":\"upload failed\"}");
        }
    });
    
    // ========== 删除文件或目录 ==========
    CROW_ROUTE(app, "/api/pods/<string>/<string>/files/delete").methods("POST"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        auto body = json::parse(req.body);
        std::string filePath = body["path"];
        
        if (filePath.empty()) {
            return crow::response(400, "{\"error\":\"path required\"}");
        }
        
        std::string cmd = "kubectl exec " + name + " -n " + ns + " -- rm -rf " + filePath + " 2>&1";
        int ret = system(cmd.c_str());
        
        if (ret == 0) {
            return crow::response("{\"success\":true}");
        } else {
            return crow::response(500, "{\"error\":\"delete failed\"}");
        }
    });









    // ========== Deployment 操作 API ==========
    
    // 更新 Deployment (PUT 请求需要读取 body)
    CROW_ROUTE(app, "/api/deployments/<string>/<string>")
        .methods("PUT"_method)
        ([&](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        std::string path = "/apis/apps/v1/namespaces/" + ns + "/deployments/" + name;
        std::string response = k8sRequestRaw("PUT", path, req.body);
        return crow::response(response);
    });
    
    // 删除 Deployment
    CROW_ROUTE(app, "/api/deployments/<string>/<string>")
        .methods("DELETE"_method)
        ([](const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        std::string path = "/apis/apps/v1/namespaces/" + ns + "/deployments/" + name;
        std::string response = k8sRequestRaw("DELETE", path);
        return crow::response(response);
    });

    
    // ========== 命令行伸缩 ==========
    CROW_ROUTE(app, "/api/deployments/<string>/<string>/scale").methods("PUT"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        try {
            auto body = json::parse(req.body);
            int replicas = body["spec"]["replicas"];
            
            // 直接执行 kubectl 命令
            std::string cmd = "kubectl scale deployment/" + name + " -n " + ns + " --replicas=" + std::to_string(replicas) + " 2>&1";
            std::string result = "";
            char buffer[128];
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return crow::response(500, "{\"error\":\"popen failed\"}");
            }
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result += buffer;
            }
            pclose(pipe);
            
            // 重新加载详情
            std::string getPath = "/apis/apps/v1/namespaces/" + ns + "/deployments/" + name;
            std::string getResp = k8sRequestRaw("GET", getPath);
            
            return crow::response(getResp);
        } catch (const std::exception& e) {
            return crow::response(500, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });

    


    // ========== 重启 Deployment ==========
    CROW_ROUTE(app, "/api/deployments/<string>/<string>/restart").methods("POST"_method)([](const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) {
            return crow::response(500, "{\"error\":\"no token\"}");
        }
        
        // 1. 先获取 Deployment，读取它的标签选择器
        std::string deployPath = "/apis/apps/v1/namespaces/" + ns + "/deployments/" + name;
        std::string deployResp = k8sRequestRaw("GET", deployPath);
        
        try {
            auto deploy = json::parse(deployResp);
            std::string labelSelector = "";
            
            // 从 Deployment 中获取标签选择器
            if (deploy.contains("spec") && deploy["spec"].contains("selector") && deploy["spec"]["selector"].contains("matchLabels")) {
                auto matchLabels = deploy["spec"]["selector"]["matchLabels"];
                for (auto it = matchLabels.begin(); it != matchLabels.end(); ++it) {
                    if (!labelSelector.empty()) labelSelector += ",";
                    labelSelector += it.key() + "=" + it.value().get<std::string>();
                }
            }
            
            if (labelSelector.empty()) {
                return crow::response(500, "{\"error\":\"无法获取标签选择器\"}");
            }
            
            // 2. 根据标签选择器查找 Pods
            std::string podsPath = "/api/v1/namespaces/" + ns + "/pods?labelSelector=" + labelSelector;
            std::string podsResp = k8sRequestRaw("GET", podsPath);
            
            auto pods = json::parse(podsResp);
            int deletedCount = 0;
            
            if (pods.contains("items")) {
                for (const auto& pod : pods["items"]) {
                    std::string podName = safeGetString(pod["metadata"], "name");
                    if (!podName.empty()) {
                        std::string deletePath = "/api/v1/namespaces/" + ns + "/pods/" + podName;
                        k8sRequestRaw("DELETE", deletePath);
                        deletedCount++;
                        std::cout << "删除 Pod: " << podName << std::endl;
                    }
                }
            }
            
            if (deletedCount > 0) {
                return crow::response("{\"success\":true,\"message\":\"已删除 " + std::to_string(deletedCount) + " 个 Pod，正在重建\"}");
            } else {
                return crow::response("{\"success\":false,\"message\":\"未找到可删除的 Pod\"}");
            }
        } catch (const std::exception& e) {
            return crow::response(500, "{\"error\":\"" + std::string(e.what()) + "\"}");
        }
    });






    // 服务列表
    CROW_ROUTE(app, "/api/services")
        .methods("GET"_method)
        ([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        const char* nsParam = req.url_params.get("namespace");
        std::string ns = nsParam ? std::string(nsParam) : "";
        std::string path = ns.empty() ? "/api/v1/services" : "/api/v1/namespaces/" + ns + "/services";
        return crow::response(k8sRequestRaw("GET", path));
    });
    
    // Ingress 列表
    CROW_ROUTE(app, "/api/ingresses")
        .methods("GET"_method)
        ([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        const char* nsParam = req.url_params.get("namespace");
        std::string ns = nsParam ? std::string(nsParam) : "";
        std::string path = ns.empty() ? "/apis/networking.k8s.io/v1/ingresses" : "/apis/networking.k8s.io/v1/namespaces/" + ns + "/ingresses";
        return crow::response(k8sRequestRaw("GET", path));
    });
    
    // 事件列表
    CROW_ROUTE(app, "/api/events")
        .methods("GET"_method)
        ([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        const char* nsParam = req.url_params.get("namespace");
        std::string ns = nsParam ? std::string(nsParam) : "";
        std::string path = ns.empty() ? "/api/v1/events" : "/api/v1/namespaces/" + ns + "/events";
        return crow::response(k8sRequestRaw("GET", path));
    });







    // ========== Deployment 详情 API ==========
    // 获取单个 Deployment 详情
    CROW_ROUTE(app, "/api/deployments/<string>/<string>").methods("GET"_method)([](const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        
        std::string path = "/apis/apps/v1/namespaces/" + ns + "/deployments/" + name;
        std::string response = k8sRequestRaw("GET", path);
        
        return crow::response(response);
    });
    
    // 获取 ReplicaSet 列表（按 Deployment 过滤）
    CROW_ROUTE(app, "/api/replicasets").methods("GET"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        
        const char* ns_param = req.url_params.get("namespace");
        const char* deploy_param = req.url_params.get("deployment");
        
        std::string namespace_filter;
        std::string deployment_filter;
        
        if (ns_param != nullptr) namespace_filter = std::string(ns_param);
        if (deploy_param != nullptr) deployment_filter = std::string(deploy_param);
        
        std::string path;
        if (!namespace_filter.empty()) {
            path = "/apis/apps/v1/namespaces/" + namespace_filter + "/replicasets";
        } else {
            path = "/apis/apps/v1/replicasets";
        }
        
        std::string response = k8sRequestRaw("GET", path);
        
        // 如果指定了 deployment，过滤相关的 ReplicaSet
        if (!deployment_filter.empty()) {
            try {
                auto rs = json::parse(response);
                json result = json::array();
                
                if (rs.contains("items")) {
                    for (const auto& item : rs["items"]) {
                        if (item.contains("metadata") && item["metadata"].contains("ownerReferences")) {
                            for (const auto& owner : item["metadata"]["ownerReferences"]) {
                                std::string kind = safeGetString(owner, "kind");
                                std::string name = safeGetString(owner, "name");
                                if (kind == "Deployment" && name == deployment_filter) {
                                    result.push_back(item);
                                    break;
                                }
                            }
                        }
                    }
                }
                return crow::response(result.dump());
            } catch (...) {
                return crow::response(response);
            }
        }
        
        return crow::response(response);
    });



        // ========== 获取 ReplicaSet 列表（按 Deployment 过滤）==========
    // 获取 ReplicaSet 列表 - 修复空指针问题
    CROW_ROUTE(app, "/api/replicasets1").methods("GET"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        
        const char* ns_param = req.url_params.get("namespace");
        const char* deploy_param = req.url_params.get("deployment");
        
        std::string namespace_filter;
        std::string deployment_filter;
        
        if (ns_param != nullptr) namespace_filter = std::string(ns_param);
        if (deploy_param != nullptr) deployment_filter = std::string(deploy_param);
        
        std::string path;
        if (!namespace_filter.empty()) {
            path = "/apis/apps/v1/namespaces/" + namespace_filter + "/replicasets";
        } else {
            path = "/apis/apps/v1/replicasets";
        }
        
        std::string response = k8sRequestRaw("GET", path);
        
        if (!deployment_filter.empty()) {
            try {
                auto rs = json::parse(response);
                json result = json::array();
                if (rs.contains("items")) {
                    for (const auto& item : rs["items"]) {
                        if (item.contains("metadata") && item["metadata"].contains("ownerReferences")) {
                            for (const auto& owner : item["metadata"]["ownerReferences"]) {
                                if (safeGetString(owner, "kind") == "Deployment" &&
                                    safeGetString(owner, "name") == deployment_filter) {
                                    result.push_back(item);
                                    break;
                                }
                            }
                        }
                    }
                }
                return crow::response(result.dump());
            } catch (...) {
                return crow::response(response);
            }
        }
        
        return crow::response(response);
    });
    
    // 获取 Ingress 列表 - 修复空指针问题
    CROW_ROUTE(app, "/api/ingresses1").methods("GET"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
    
        const char* ns_param = req.url_params.get("namespace");
        std::string namespace_filter;
        if (ns_param != nullptr) {
            namespace_filter = std::string(ns_param);
        }
    
        std::string path;
        if (!namespace_filter.empty()) {
            path = "/apis/networking.k8s.io/v1/namespaces/" + namespace_filter + "/ingresses";
        } else {
            path = "/apis/networking.k8s.io/v1/ingresses";
        }
    
        std::string response = k8sRequestRaw("GET", path);
        return crow::response(response);
    });
    
    // 获取事件列表 - 修复空指针问题
    CROW_ROUTE(app, "/api/events2").methods("GET"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
    
        const char* ns_param = req.url_params.get("namespace");
        std::string namespace_filter;
        if (ns_param != nullptr) {
            namespace_filter = std::string(ns_param);
        }
    
        std::string path;
        if (!namespace_filter.empty()) {
            path = "/api/v1/namespaces/" + namespace_filter + "/events";
        } else {
            path = "/api/v1/events";
        }
    
        std::string response = k8sRequestRaw("GET", path);
    
        try {
            auto events = json::parse(response);
            json result = json::array();
            if (events.contains("items")) {
                for (const auto& item : events["items"]) {
                    json e;
                    e["type"] = safeGetString(item, "type");
                    e["reason"] = safeGetString(item, "reason");
                    e["message"] = safeGetString(item, "message");
                    e["count"] = safeGetInt(item, "count", 1);
                    e["lastTimestamp"] = safeGetString(item, "lastTimestamp");
                    e["firstTimestamp"] = safeGetString(item, "firstTimestamp");
                    if (item.contains("involvedObject")) {
                        json involved;
                        involved["kind"] = safeGetString(item["involvedObject"], "kind");
                        involved["name"] = safeGetString(item["involvedObject"], "name");
                        involved["namespace"] = safeGetString(item["involvedObject"], "namespace");
                        e["involvedObject"] = involved;
                    }
                    result.push_back(e);
                }
            }
            return crow::response(result.dump());
        } catch (...) {
            return crow::response(response);
        }
    });
    
    // 获取服务列表（支持命名空间过滤）- 修复空指针问题
    CROW_ROUTE(app, "/api/services2").methods("GET"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
    
        // 修复：安全检查，防止空指针
        const char* ns_param = req.url_params.get("namespace");
        std::string namespace_filter;
        if (ns_param != nullptr) {
            namespace_filter = std::string(ns_param);
        }
    
        std::string path;
        if (!namespace_filter.empty()) {
            path = "/api/v1/namespaces/" + namespace_filter + "/services";
        } else {
            path = "/api/v1/services";
        }
    
        std::string response = k8sRequestRaw("GET", path);
        return crow::response(response);
    });


   //////////////






    // ========== Pod 详细信息和日志 API ==========
    
    // 获取单个 Pod 详情
    CROW_ROUTE(app, "/api/pods/<string>/<string>").methods("GET"_method)([](const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        std::string response = k8sRequestRaw("GET", "/api/v1/namespaces/" + ns + "/pods/" + name);
        return crow::response(response);
    });


// 获取 Pod 日志
CROW_ROUTE(app, "/api/pods/<string>/<string>/logs").methods("GET"_method)([](const crow::request& req, const std::string& ns, const std::string& name) {
    if (g_k8s_token.empty()) {
        return crow::response(500, "{\"error\":\"no token\"}");
    }

    // 构建正确的路径
    std::string path = "/api/v1/namespaces/" + ns + "/pods/" + name + "/log";

    // 获取参数
    const char* previous_param = req.url_params.get("previous");
    if (previous_param != nullptr && std::string(previous_param) == "true") {
        path += "?previous=true";
    }

    try {
        std::string response = k8sRequestRaw("GET", path);
        if (response.empty()) {
            return crow::response("(无日志内容)");
        }
        // 返回日志内容
        crow::response res;
        res.set_header("Content-Type", "text/plain");
        res.write(response);
        return res;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] 获取日志失败: " << e.what() << std::endl;
        return crow::response(500, "{\"error\":\"Failed to get logs: " + std::string(e.what()) + "\"}");
    }
});




    
    // 删除 Pod
    CROW_ROUTE(app, "/api/pods/<string>/<string>").methods("DELETE"_method)([](const std::string& ns, const std::string& name) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        std::string response = k8sRequestRaw("DELETE", "/api/v1/namespaces/" + ns + "/pods/" + name);
        return crow::response(response);
    });
    
    // 创建 Pod
    CROW_ROUTE(app, "/api/pods").methods("POST"_method)([](const crow::request& req) {
        if (g_k8s_token.empty()) return crow::response(500, "{\"error\":\"no token\"}");
        std::string response = k8sRequestRaw("POST", "/api/v1/namespaces/default/pods", req.body);
        return crow::response(response);
    });
    




    // ========== 高级部署 API ==========
    CROW_ROUTE(app, "/api/advanced-deploy").methods("POST"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);

        std::string namespace_ = body.value("namespace", "default");
        std::string deploymentName = body.value("name", "deploy-" + std::to_string(time(nullptr)));

        // 安全检查：确保 container 字段存在
        std::string image = "";
        if (body.contains("container") && body["container"].contains("image")) {
            image = body["container"]["image"].get<std::string>();
        } else if (body.contains("image")) {
            image = body["image"].get<std::string>();
        } else {
            return crow::response(400, "{\"error\":\"missing image field\"}");
        }

        // 基础部署配置
        json deployment = {
            {"apiVersion", "apps/v1"},
            {"kind", "Deployment"},
            {"metadata", {
                {"name", deploymentName},
                {"namespace", namespace_}
            }},
            {"spec", {
                {"replicas", body.value("replicas", 1)},
                {"selector", {
                    {"matchLabels", {{"app", deploymentName}}}
                }},
                {"template", {
                    {"metadata", {
                        {"labels", {{"app", deploymentName}}}
                    }},
                    {"spec", {
                        {"containers", json::array()}
                    }}
                }}
            }}
        };

        // 容器配置
        json container = {
            {"name", deploymentName},
            {"image", image},
            {"imagePullPolicy", "IfNotPresent"}
        };

        // 资源配置
        if (body.contains("resources")) {
            container["resources"] = body["resources"];
        }

        // 启动命令
        if (body.contains("command") && body["command"].is_array() && !body["command"].empty()) {
            container["command"] = body["command"];
        }

        // 环境变量
        if (body.contains("env") && body["env"].is_array()) {
            container["env"] = body["env"];
        }

        // 端口映射
        if (body.contains("ports") && body["ports"].is_array()) {
            json ports = json::array();
            for (const auto& p : body["ports"]) {
                json port;
                port["containerPort"] = p["containerPort"];
                if (p.contains("protocol")) port["protocol"] = p["protocol"];
                ports.push_back(port);
            }
            container["ports"] = ports;
        }

        // 存储卷挂载（支持 hostPath 和 PVC）
        if (body.contains("volumes") && body["volumes"].is_array()) {
            json volumes = json::array();
            json volumeMounts = json::array();
            for (const auto& vol : body["volumes"]) {
                json volume;
                volume["name"] = vol["name"];
                if (vol.contains("hostPath")) {
                    volume["hostPath"] = {{"path", vol["hostPath"]["path"]}, {"type", "DirectoryOrCreate"}};
                } else if (vol.contains("persistentVolumeClaim")) {
                    volume["persistentVolumeClaim"] = {{"claimName", vol["persistentVolumeClaim"]["claimName"]}};
                }
                volumes.push_back(volume);
                if (vol.contains("mountPath")) {
                    volumeMounts.push_back({{"name", vol["name"]}, {"mountPath", vol["mountPath"]}});
                }
            }
            if (!volumes.empty()) {
                deployment["spec"]["template"]["spec"]["volumes"] = volumes;
            }
            if (!volumeMounts.empty()) {
                container["volumeMounts"] = volumeMounts;
            }
        }

        // 处理 volumeMounts（单独传递的情况）
        if (body.contains("volumeMounts") && body["volumeMounts"].is_array()) {
            container["volumeMounts"] = body["volumeMounts"];
        }

        // GPU 支持
        if (body.contains("gpu")) {
            if (!container.contains("resources")) {
                container["resources"] = json::object();
            }
            if (!container["resources"].contains("limits")) {
                container["resources"]["limits"] = json::object();
            }
            container["resources"]["limits"]["nvidia.com/gpu"] = body["gpu"]["count"];
            if (body["gpu"].contains("memory")) {
                container["resources"]["limits"]["nvidia.com/gpumem"] = body["gpu"]["memory"];
            }
            deployment["metadata"]["annotations"] = {
                {"hami.io/gpu-scheduler", "true"}
            };
        }

        deployment["spec"]["template"]["spec"]["containers"].push_back(container);

        // 节点选择
        if (body.contains("nodeName") && !body["nodeName"].empty()) {
            deployment["spec"]["template"]["spec"]["nodeName"] = body["nodeName"];
        }
        if (body.contains("nodeSelector") && body["nodeSelector"].is_object()) {
            deployment["spec"]["template"]["spec"]["nodeSelector"] = body["nodeSelector"];
        }

        // 多节点部署
        if (body.contains("multiNode") && body["multiNode"]["enabled"]) {
            json results = json::array();
            int replicasPerNode = body["multiNode"].value("replicasPerNode", 1);
            for (const auto& node : body["multiNode"]["nodes"]) {
                json nodeDeployment = deployment;
                std::string nodeDeployName = deploymentName + "-" + node.get<std::string>();
                nodeDeployment["metadata"]["name"] = nodeDeployName;
                nodeDeployment["spec"]["replicas"] = replicasPerNode;
                nodeDeployment["spec"]["template"]["spec"]["nodeName"] = node;

                std::string response = k8sRequestRaw("POST",
                    "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
                    nodeDeployment.dump());
                results.push_back(json::parse(response));
            }
            return crow::response(results.dump());
        }

        // YAML 直接部署
        if (body.contains("yaml") && !body["yaml"].empty()) {
            std::string response = k8sRequestRaw("POST",
                "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
                body["yaml"]);
            return crow::response(response);
        }

        std::string response = k8sRequestRaw("POST",
            "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
            deployment.dump());

        return crow::response(response);
    });




    // ========== 高级部署 API ==========
    CROW_ROUTE(app, "/api/advanced-deploy1").methods("POST"_method)([](const crow::request& req) {
        auto body = json::parse(req.body);
        
        std::string namespace_ = body.value("namespace", "default");
        std::string deploymentName = body.value("name", "deploy-" + std::to_string(time(nullptr)));
        
        // 基础部署配置
        json deployment = {
            {"apiVersion", "apps/v1"},
            {"kind", "Deployment"},
            {"metadata", {
                {"name", deploymentName},
                {"namespace", namespace_}
            }},
            {"spec", {
                {"replicas", body.value("replicas", 1)},
                {"selector", {
                    {"matchLabels", {{"app", deploymentName}}}
                }},
                {"template", {
                    {"metadata", {
                        {"labels", {{"app", deploymentName}}}
                    }},
                    {"spec", {
                        {"containers", json::array()}
                    }}
                }}
            }}
        };
        
        // 添加标签
        if (body.contains("labels")) {
            deployment["metadata"]["labels"] = body["labels"];
            deployment["spec"]["template"]["metadata"]["labels"].merge_patch(body["labels"]);
        }
        
        // 容器配置
        json container = {
            {"name", deploymentName},
            {"image", body["container"]["image"]},
            {"imagePullPolicy", "IfNotPresent"}
        };
        
        // 资源配置
        if (body.contains("resources")) {
            container["resources"] = body["resources"];
        }
        
        // 启动命令
        if (body.contains("command") && body["command"].is_array() && !body["command"].empty()) {
            container["command"] = body["command"];
        }
        
        // 环境变量
        if (body.contains("env") && body["env"].is_array()) {
            container["env"] = body["env"];
        }
        
        // 端口映射
        if (body.contains("ports") && body["ports"].is_array()) {
            json ports = json::array();
            for (const auto& p : body["ports"]) {
                json port;
                port["containerPort"] = p["containerPort"];
                if (p.contains("protocol")) port["protocol"] = p["protocol"];
                ports.push_back(port);
            }
            container["ports"] = ports;
        }
        
        // 存储卷挂载（支持 hostPath 和 PVC）
        if (body.contains("volumes") && body["volumes"].is_array()) {
            json volumes = json::array();
            json volumeMounts = json::array();
            for (const auto& vol : body["volumes"]) {
                json volume;
                volume["name"] = vol["name"];
                if (vol.contains("hostPath")) {
                    volume["hostPath"] = {{"path", vol["hostPath"]["path"]}, {"type", "DirectoryOrCreate"}};
                } else if (vol.contains("persistentVolumeClaim")) {
                    volume["persistentVolumeClaim"] = {{"claimName", vol["persistentVolumeClaim"]["claimName"]}};
                }
                volumes.push_back(volume);
                volumeMounts.push_back({{"name", vol["name"]}, {"mountPath", vol["mountPath"]}});
            }
            deployment["spec"]["template"]["spec"]["volumes"] = volumes;
            container["volumeMounts"] = volumeMounts;
        }

        
        // GPU 支持
        if (body.contains("gpu")) {
            if (!container.contains("resources")) {
                container["resources"] = json::object();
            }
            if (!container["resources"].contains("limits")) {
                container["resources"]["limits"] = json::object();
            }
            container["resources"]["limits"]["nvidia.com/gpu"] = body["gpu"]["count"];
            if (body["gpu"].contains("memory")) {
                container["resources"]["limits"]["nvidia.com/gpumem"] = body["gpu"]["memory"];
            }
            // HAMI 调度器注解
            deployment["metadata"]["annotations"] = {
                {"hami.io/gpu-scheduler", "true"}
            };
        }
        
        deployment["spec"]["template"]["spec"]["containers"].push_back(container);
        
        // 节点选择
        if (body.contains("nodeName") && !body["nodeName"].empty()) {
            deployment["spec"]["template"]["spec"]["nodeName"] = body["nodeName"];
        }
        if (body.contains("nodeSelector") && body["nodeSelector"].is_object()) {
            deployment["spec"]["template"]["spec"]["nodeSelector"] = body["nodeSelector"];
        }
        
        // 多节点部署
        if (body.contains("multiNode") && body["multiNode"]["enabled"]) {
            json results = json::array();
            int replicasPerNode = body["multiNode"].value("replicasPerNode", 1);
            for (const auto& node : body["multiNode"]["nodes"]) {
                json nodeDeployment = deployment;
                std::string nodeDeployName = deploymentName + "-" + node.get<std::string>();
                nodeDeployment["metadata"]["name"] = nodeDeployName;
                nodeDeployment["spec"]["replicas"] = replicasPerNode;
                nodeDeployment["spec"]["template"]["spec"]["nodeName"] = node;
                
                std::string response = k8sRequestRaw("POST", 
                    "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
                    nodeDeployment.dump());
                results.push_back(json::parse(response));
            }
            return crow::response(results.dump());
        }
        
        // YAML 直接部署
        if (body.contains("yaml") && !body["yaml"].empty()) {
            std::string response = k8sRequestRaw("POST", 
                "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
                body["yaml"]);
            return crow::response(response);
        }
        
        std::string response = k8sRequestRaw("POST", 
            "/apis/apps/v1/namespaces/" + namespace_ + "/deployments",
            deployment.dump());
        
        return crow::response(response);
    });
    


    




    // ========== Pods API ==========
    CROW_ROUTE(app, "/api/pods")([](){
        if (g_k8s_token.empty()) return crow::response(500, R"({"error":"no token"})");
        try {
            std::string response = k8sRequestRaw("GET", "/api/v1/pods");
            auto pods = json::parse(response);
            json result = json::array();
            if (pods.contains("items")) for (const auto& item : pods["items"]) {
                json pod;
                pod["name"] = safeGetString(item["metadata"], "name");
                pod["namespace"] = safeGetString(item["metadata"], "namespace");
                pod["status"] = safeGetString(item["status"], "phase");
                pod["node"] = safeGetString(item["spec"], "nodeName");
                pod["ip"] = safeGetString(item["status"], "podIP");
                int restarts = 0;
                if (item.contains("status") && item["status"].contains("containerStatuses"))
                    for (const auto& cs : item["status"]["containerStatuses"]) restarts += safeGetInt(cs, "restartCount", 0);
                pod["restarts"] = restarts;
                result.push_back(pod);
            }
            return crow::response(result.dump());
        } catch (...) { return crow::response("[]"); }
    });

    // ========== Harbor 镜像标签 ==========
    CROW_ROUTE(app, "/api/harbor/images-with-tags")([](){
        CURL* curl = curl_easy_init();
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.138.139:30002/api/v2.0/projects/library/repositories?page_size=100");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_perform(curl);
        auto repos = json::parse(response);
        json result = json::array();
        if (repos.is_array()) for (const auto& repo : repos) {
            std::string repoName = repo["name"];
            std::string artifactsUrl = "http://192.168.138.139:30002/api/v2.0/projects/library/repositories/" + repoName.substr(repoName.find('/') + 1) + "/artifacts";
            std::string artResp;
            CURL* curl2 = curl_easy_init();
            curl_easy_setopt(curl2, CURLOPT_URL, artifactsUrl.c_str());
            curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl2, CURLOPT_WRITEDATA, &artResp);
            curl_easy_setopt(curl2, CURLOPT_HTTPHEADER, headers);
            curl_easy_perform(curl2);
            curl_easy_cleanup(curl2);
            json item; item["name"] = repoName; item["tags"] = json::array();
            auto artifacts = json::parse(artResp);
            if (artifacts.is_array()) for (const auto& art : artifacts)
                if (art.contains("tags") && art["tags"].is_array()) for (const auto& tag : art["tags"]) item["tags"].push_back(tag["name"]);
            result.push_back(item);
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return crow::response(result.dump());
    });

    // ========== 组件配置 API ==========
    CROW_ROUTE(app, "/api/component-config")([](){ return crow::response(getDbConfig()); });
    CROW_ROUTE(app, "/api/component-config").methods("PUT"_method)([](const crow::request& req){ saveDbConfig(req.body); return crow::response("{\"success\":true}"); });

    // ========== Harbor API ==========
    CROW_ROUTE(app, "/api/harbor/projects/<string>/repositories/<string>/artifacts")([](const std::string& project, const std::string& repo){
        CURL* curl = curl_easy_init(); std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, ("http://192.168.138.139:30002/api/v2.0/projects/" + project + "/repositories/" + repo + "/artifacts").c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        struct curl_slist* headers = NULL; headers = curl_slist_append(headers, "Accept: application/json"); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_perform(curl); curl_easy_cleanup(curl); curl_slist_free_all(headers);
        return crow::response(response);
    });

    CROW_ROUTE(app, "/api/harbor/projects")([](){
        CURL* curl = curl_easy_init(); std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.138.139:30002/api/v2.0/projects");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        struct curl_slist* headers = NULL; headers = curl_slist_append(headers, "Accept: application/json"); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_perform(curl); curl_easy_cleanup(curl); curl_slist_free_all(headers);
        return crow::response(response);
    });

    CROW_ROUTE(app, "/api/harbor/projects/<string>/repositories")([](const std::string& project){
        CURL* curl = curl_easy_init(); std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, ("http://192.168.138.139:30002/api/v2.0/projects/" + project + "/repositories?page_size=100").c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        struct curl_slist* headers = NULL; headers = curl_slist_append(headers, "Accept: application/json"); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_perform(curl); curl_easy_cleanup(curl); curl_slist_free_all(headers);
        return crow::response(response);
    });

    // ========== 创建 Deployment ==========
    CROW_ROUTE(app, "/api/namespaces/<string>/deployments").methods("POST"_method)([](const crow::request& req, const std::string& ns){
        auto body = json::parse(req.body);
        json deployment = {{"apiVersion","apps/v1"},{"kind","Deployment"},{"metadata",{{"name",body["metadata"]["name"]},{"namespace",ns}}},{"spec",body["spec"]}};
        std::string response = k8sRequestRaw("POST", "/apis/apps/v1/namespaces/" + ns + "/deployments", deployment.dump());
        return crow::response(response);
    });

    // ========== 认证 API ==========
    CROW_ROUTE(app, "/api/auth/login1").methods("POST"_method)([](const crow::request& req){
        auto body = json::parse(req.body);
        if (ldap_auth(body["username"], body["password"])) {
            json resp = {{"code",0},{"data",{{"token","token-"+std::string(body["username"])},{"user",{{"id",1},{"username",body["username"]},{"name",body["username"]},{"role","admin"}}}}}};
            return crow::response(resp.dump());
        }
        return crow::response(401, "{\"code\":401,\"message\":\"用户名或密码错误\"}");
    });
    
    CROW_ROUTE(app, "/api/auth/login").methods("POST"_method)([](const crow::request& req){
        auto body = json::parse(req.body);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        if (ldap_auth(username, password)) {
            upsertUserToDB(username);
            UserInfo info = getUserFromDB(username);
            std::string token = "token-" + username;
            json resp = {{"code",0},{"data",{
                {"token", token},
                {"user",{
                    {"id",       info.id},
                    {"username", info.username},
                    {"name",     info.fullname},
                    {"email",    info.email},
                    {"role",     info.role}
                }}
            }}};
            return crow::response(resp.dump());
        }
        return crow::response(401, "{\"code\":401,\"message\":\"用户名或密码错误\"}");
    });
    
    CROW_ROUTE(app, "/api/auth/me")([](const crow::request& req){
        std::string username = "admin";
        std::string auth = req.get_header_value("Authorization");
        if (!auth.empty() && auth.find("Bearer ") == 0) {
            std::string token = auth.substr(7);
            if (token.find("token-") == 0) username = token.substr(6);
        }
        UserInfo info = getUserFromDB(username);
        json resp = {{"code",0},{"data",{
            {"id",       info.found ? info.id : 1},
            {"username", username},
            {"name",     info.found ? info.fullname : username},
            {"email",    info.found ? info.email : ""},
            {"role",     info.found ? info.role : "admin"}
        }}};
        return crow::response(resp.dump());
    });
    

    CROW_ROUTE(app, "/api/auth/me1")([](const crow::request& req){
        std::string username = "admin";
        std::string auth = req.get_header_value("Authorization");
        if (!auth.empty() && auth.find("Bearer ") == 0) { std::string token = auth.substr(7); if (token.find("token-") == 0) username = token.substr(6); }
        json resp = {{"code",0},{"data",{{"id",1},{"username",username},{"name",username},{"role","admin"}}}};
        return crow::response(resp.dump());
    });

    // ========== K8s API ==========
    CROW_ROUTE(app, "/api/nodes1")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try {
            std::string response = k8sRequestRaw("GET", "/api/v1/nodes");
            auto nodes = json::parse(response);
            json result = json::array();
            if (nodes.contains("items")) for (const auto& item : nodes["items"]) {
                json node;
                node["name"] = safeGetString(item["metadata"], "name");
                node["status"] = "Unknown";
                if (item.contains("status") && item["status"].contains("conditions"))
                    for (const auto& cond : item["status"]["conditions"])
                        if (safeGetString(cond, "type") == "Ready") { node["status"] = safeGetString(cond, "status") == "True" ? "Ready" : "NotReady"; break; }
                node["kubelet"] = safeGetString(item["status"]["nodeInfo"], "kubeletVersion");
                node["os"] = safeGetString(item["status"]["nodeInfo"], "operatingSystem");
                node["cpu"] = safeGetString(item["status"]["capacity"], "cpu");
                result.push_back(node);
            }
            return crow::response(result.dump());
        } catch (...) { return crow::response("[]"); }
    });

// ========== K8s API ==========
// ========== K8s API ==========
    CROW_ROUTE(app, "/api/nodes")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try {
            // 获取节点信息
            std::string nodesResp = k8sRequestRaw("GET", "/api/v1/nodes");
            auto nodes = json::parse(nodesResp);
            
            // 获取 Pod 列表
            std::string podsResp = k8sRequestRaw("GET", "/api/v1/pods");
            auto pods = json::parse(podsResp);
            
            // 获取节点实时指标
            std::string metricsResp = k8sRequestRaw("GET", "/apis/metrics.k8s.io/v1beta1/nodes");
            auto metrics = json::parse(metricsResp);
            
            // 构建节点指标映射
            std::map<std::string, json> metricsMap;
            if (metrics.contains("items")) {
                for (const auto& item : metrics["items"]) {
                    std::string name = safeGetString(item["metadata"], "name");
                    metricsMap[name] = item["usage"];
                }
            }
            
            // 统计 Pod 数量
            std::map<std::string, int> podCountMap;
            if (pods.contains("items")) {
                for (const auto& pod : pods["items"]) {
                    std::string nodeName = safeGetString(pod["spec"], "nodeName");
                    if (!nodeName.empty()) {
                        podCountMap[nodeName]++;
                    }
                }
            }
            
            json result = json::array();
            if (nodes.contains("items")) {
                for (const auto& item : nodes["items"]) {
                    json node;
                    std::string nodeName = safeGetString(item["metadata"], "name");
                    node["name"] = nodeName;
                    
                    // 节点状态
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
                    
                    // 总量
                    json capacity = item["status"]["capacity"];
                    double cpuTotal = 0, memTotalMB = 0;
                    
                    if (capacity.contains("cpu")) {
                        cpuTotal = std::stod(capacity["cpu"].get<std::string>());
                    }
                    if (capacity.contains("memory")) {
                        std::string memStr = capacity["memory"].get<std::string>();
                        std::string numStr;
                        for (char c : memStr) {
                            if (isdigit(c) || c == '.') numStr += c;
                            else break;
                        }
                        double memVal = std::stod(numStr);
                        if (memStr.find("Ki") != std::string::npos) memTotalMB = memVal / 1024;
                        else if (memStr.find("Mi") != std::string::npos) memTotalMB = memVal;
                        else if (memStr.find("Gi") != std::string::npos) memTotalMB = memVal * 1024;
                        else memTotalMB = memVal / (1024 * 1024);
                    }
                    
                    // 从 metrics-server 获取实际使用量
                    double cpuUsed = 0, memUsedMB = 0;
                    if (metricsMap.count(nodeName)) {
                        json usage = metricsMap[nodeName];
                        if (usage.contains("cpu")) {
                            std::string cpuStr = usage["cpu"].get<std::string>();
                            // 解析 "167404399n" (纳核)
                            std::string numStr;
                            for (char c : cpuStr) {
                                if (isdigit(c)) numStr += c;
                                else break;
                            }
                            cpuUsed = std::stod(numStr) / 1000000000;  // 纳核转核
                        }
                        if (usage.contains("memory")) {
                            std::string memStr = usage["memory"].get<std::string>();
                            std::string numStr;
                            for (char c : memStr) {
                                if (isdigit(c)) numStr += c;
                                else break;
                            }
                            double memVal = std::stod(numStr);
                            if (memStr.find("Ki") != std::string::npos) memUsedMB = memVal / 1024;
                            else if (memStr.find("Mi") != std::string::npos) memUsedMB = memVal;
                            else if (memStr.find("Gi") != std::string::npos) memUsedMB = memVal * 1024;
                            else memUsedMB = memVal / (1024 * 1024);
                        }
                    }
                    
                    node["cpu"] = cpuTotal;
                    node["cpuUsed"] = cpuUsed;
                    node["cpuPercent"] = cpuTotal > 0 ? (cpuUsed / cpuTotal * 100) : 0;
                    node["memoryTotal"] = memTotalMB;
                    node["memoryUsed"] = memUsedMB;
                    node["memoryPercent"] = memTotalMB > 0 ? (memUsedMB / memTotalMB * 100) : 0;
                    node["podCount"] = podCountMap[nodeName];
                    
                    result.push_back(node);
                }
            }
            return crow::response(result.dump());
        } catch (const std::exception& e) {
            std::cerr << "Nodes API error: " << e.what() << std::endl;
            return crow::response("[]");
        }
    });
// ========== K8s API ==========

    CROW_ROUTE(app, "/api/namespaces")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try { auto ns = json::parse(k8sRequestRaw("GET", "/api/v1/namespaces")); json result = json::array();
            if (ns.contains("items")) for (const auto& item : ns["items"]) { json n; n["name"] = safeGetString(item["metadata"], "name"); n["status"] = safeGetString(item["status"], "phase"); result.push_back(n); }
            return crow::response(result.dump()); } catch (...) { return crow::response("[]"); }
    });

    CROW_ROUTE(app, "/api/deployments")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try { auto deps = json::parse(k8sRequestRaw("GET", "/apis/apps/v1/deployments")); json result = json::array();
            if (deps.contains("items")) for (const auto& item : deps["items"]) { json d; d["name"] = safeGetString(item["metadata"], "name"); d["namespace"] = safeGetString(item["metadata"], "namespace"); d["replicas"] = safeGetInt(item["spec"], "replicas"); result.push_back(d); }
            return crow::response(result.dump()); } catch (...) { return crow::response("[]"); }
    });

    CROW_ROUTE(app, "/api/services1")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try { auto svcs = json::parse(k8sRequestRaw("GET", "/api/v1/services")); json result = json::array();
            if (svcs.contains("items")) for (const auto& item : svcs["items"]) { json s; s["name"] = safeGetString(item["metadata"], "name"); s["namespace"] = safeGetString(item["metadata"], "namespace"); s["type"] = safeGetString(item["spec"], "type"); result.push_back(s); }
            return crow::response(result.dump()); } catch (...) { return crow::response("[]"); }
    });

    CROW_ROUTE(app, "/api/configmaps")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try { auto cms = json::parse(k8sRequestRaw("GET", "/api/v1/configmaps")); json result = json::array();
            if (cms.contains("items")) for (const auto& item : cms["items"]) { json c; c["name"] = safeGetString(item["metadata"], "name"); c["namespace"] = safeGetString(item["metadata"], "namespace"); result.push_back(c); }
            return crow::response(result.dump()); } catch (...) { return crow::response("[]"); }
    });

    CROW_ROUTE(app, "/api/events11")([](){
        if (g_k8s_token.empty()) return crow::response("[]");
        try { auto events = json::parse(k8sRequestRaw("GET", "/api/v1/events")); json result = json::array();
            if (events.contains("items")) { int count = 0; for (const auto& item : events["items"]) { if (count++ >= 50) break;
                json e; e["type"] = safeGetString(item, "type"); e["reason"] = safeGetString(item, "reason"); e["message"] = safeGetString(item, "message"); result.push_back(e); } }
            return crow::response(result.dump()); } catch (...) { return crow::response("[]"); }
    });




CROW_ROUTE(app, "/api/events1")([](){
    if (g_k8s_token.empty()) return crow::response("[]");
    try {
        // 获取所有事件，不限命名空间，按时间排序
        std::string response = k8sRequestRaw("GET", "/api/v1/events");
        auto events = json::parse(response);
        json result = json::array();
        
        if (events.contains("items")) {
            // 按 lastTimestamp 排序（最新的在前）
            std::sort(events["items"].begin(), events["items"].end(),
                [](const json& a, const json& b) {
                    return safeGetString(a, "lastTimestamp") > safeGetString(b, "lastTimestamp");
                });
            
            int count = 0;
            for (const auto& item : events["items"]) {
                if (count++ >= 50) break;
                
                json e;
                e["type"] = safeGetString(item, "type", "Normal");
                e["reason"] = safeGetString(item, "reason", "");
                e["message"] = safeGetString(item, "message", "");
                e["count"] = safeGetInt(item, "count", 1);
                e["lastTimestamp"] = safeGetString(item, "lastTimestamp", "");
                e["firstTimestamp"] = safeGetString(item, "firstTimestamp", "");
                
                // 获取命名空间
                std::string namespace_ = "";
                if (item.contains("involvedObject")) {
                    namespace_ = safeGetString(item["involvedObject"], "namespace", "");
                }
                e["namespace"] = namespace_;
                
                // 获取事件来源组件
                if (item.contains("source")) {
                    e["source"] = safeGetString(item["source"], "component", "");
                } else {
                    e["source"] = "";
                }
                
                // 获取对象名称
                if (item.contains("involvedObject")) {
                    e["objectName"] = safeGetString(item["involvedObject"], "name", "");
                } else {
                    e["objectName"] = "";
                }
                
                result.push_back(e);
            }
        }
        return crow::response(result.dump());
    } catch (const std::exception& e) {
        std::cerr << "Events API error: " << e.what() << std::endl;
        return crow::response("[]");
    }
});



CROW_ROUTE(app, "/api/events13")([](){
    if (g_k8s_token.empty()) return crow::response("[]");
    try {
        auto events = json::parse(k8sRequestRaw("GET", "/api/v1/events"));
        json result = json::array();
        if (events.contains("items")) {
            int count = 0;
            for (const auto& item : events["items"]) {
                if (count++ >= 50) break;
                
                json e;
                e["type"] = safeGetString(item, "type");
                e["reason"] = safeGetString(item, "reason");
                e["message"] = safeGetString(item, "message");
                e["count"] = safeGetInt(item, "count", 1);
                e["lastTimestamp"] = safeGetString(item, "lastTimestamp");
                
                // 获取命名空间
                std::string namespace_ = "";
                if (item.contains("involvedObject")) {
                    namespace_ = safeGetString(item["involvedObject"], "namespace");
                }
                e["namespace"] = namespace_;
                
                // 获取事件来源
                if (item.contains("source")) {
                    e["source"] = safeGetString(item["source"], "component");
                } else {
                    e["source"] = "";
                }
                
                result.push_back(e);
            }
        }
        return crow::response(result.dump());
    } catch (...) {
        return crow::response("[]");
    }
});

CROW_ROUTE(app, "/api/events12")([](){
    if (g_k8s_token.empty()) return crow::response("[]");
    try {
        auto events = json::parse(k8sRequestRaw("GET", "/api/v1/events"));
        json result = json::array();
        if (events.contains("items")) {
            int count = 0;
            for (const auto& item : events["items"]) {
                if (count++ >= 50) break;
                
                json e;
                e["type"] = safeGetString(item, "type");
                e["reason"] = safeGetString(item, "reason");
                e["message"] = safeGetString(item, "message");
                e["count"] = safeGetInt(item, "count", 1);
                e["lastTimestamp"] = safeGetString(item, "lastTimestamp");
                e["firstTimestamp"] = safeGetString(item, "firstTimestamp");
                
                // 获取命名空间
                std::string namespace_ = "";
                if (item.contains("involvedObject")) {
                    namespace_ = safeGetString(item["involvedObject"], "namespace");
                }
                e["namespace"] = namespace_;
                
                // 获取事件来源
                if (item.contains("source")) {
                    e["source"] = safeGetString(item["source"], "component");
                } else {
                    e["source"] = "";
                }
                
                result.push_back(e);
            }
        }
        return crow::response(result.dump());
    } catch (...) {
        return crow::response("[]");
    }
});



    CROW_ROUTE(app, "/api/summary")
    ([](){
        if (g_k8s_token.empty()) {
            return crow::response("{}");
        }
        try {
            std::string podsResp = k8sRequestRaw("GET", "/api/v1/pods");
            std::string nodesResp = k8sRequestRaw("GET", "/api/v1/nodes");
            std::string depsResp = k8sRequestRaw("GET", "/apis/apps/v1/deployments");
            std::string svcsResp = k8sRequestRaw("GET", "/api/v1/services");
            std::string nsResp = k8sRequestRaw("GET", "/api/v1/namespaces");

            int nodeCount = 0, podCount = 0, running = 0, pending = 0, failed = 0;
            int deployCount = 0, serviceCount = 0, namespaceCount = 0;

            if (!nodesResp.empty()) {
                auto nodes = json::parse(nodesResp);
                nodeCount = nodes.contains("items") ? (int)nodes["items"].size() : 0;
            }
            if (!podsResp.empty()) {
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
            }
            if (!depsResp.empty()) {
                auto deps = json::parse(depsResp);
                deployCount = deps.contains("items") ? (int)deps["items"].size() : 0;
            }
            if (!svcsResp.empty()) {
                auto svcs = json::parse(svcsResp);
                serviceCount = svcs.contains("items") ? (int)svcs["items"].size() : 0;
            }
            if (!nsResp.empty()) {
                auto ns = json::parse(nsResp);
                namespaceCount = ns.contains("items") ? (int)ns["items"].size() : 0;
            }

            json summary = {
                {"nodeCount", nodeCount}, {"podCount", podCount}, {"runningPods", running},
                {"pendingPods", pending}, {"failedPods", failed},
                {"deployCount", deployCount}, {"serviceCount", serviceCount}, {"namespaceCount", namespaceCount}
            };
            return crow::response(summary.dump());
        } catch (...) {
            return crow::response("{}");
        }
    });

    // ===== 初始化 =====
    initRedis();
    refreshImageCacheToDB();
    std::thread(cacheRefreshThread).detach();




    std::cout << "🚀 Server running at http://localhost:8580" << std::endl;
    app.port(8580).multithreaded().run();
    return 0;
}
